/* GStreamer
 * Copyright (C) <2015> Wim Taymans <wim.taymans@gmail.com>
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Library General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Library General Public License for more details.
 *
 * You should have received a copy of the GNU Library General Public
 * License along with this library; if not, write to the
 * Free Software Foundation, Inc., 51 Franklin St, Fifth Floor,
 * Boston, MA 02110-1301, USA.
 */

/**
 * SECTION:element-pipewiresrc
 *
 * <refsect2>
 * <title>Example launch line</title>
 * |[
 * gst-launch -v pipewiresrc ! videoconvert ! ximagesink
 * ]| Shows pipewire output in an X window.
 * </refsect2>
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif
#include "gstpipewiresrc.h"
#include "gstpipewireformat.h"

#include <string.h>
#include <stdlib.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <unistd.h>

#include <gst/net/gstnetclientclock.h>
#include <gst/allocators/gstfdmemory.h>
#include <gst/allocators/gstdmabuf.h>
#include <gst/video/video.h>

#include "gstpipewireclock.h"

static GQuark process_mem_data_quark;

GST_DEBUG_CATEGORY_STATIC (pipewire_src_debug);
#define GST_CAT_DEFAULT pipewire_src_debug

#define DEFAULT_ALWAYS_COPY     false

enum
{
  PROP_0,
  PROP_PATH,
  PROP_CLIENT_NAME,
  PROP_STREAM_PROPERTIES,
  PROP_ALWAYS_COPY,
  PROP_FD,
};


static GstStaticPadTemplate gst_pipewire_src_template =
GST_STATIC_PAD_TEMPLATE ("src",
    GST_PAD_SRC,
    GST_PAD_ALWAYS,
    GST_STATIC_CAPS_ANY
    );

#define gst_pipewire_src_parent_class parent_class
G_DEFINE_TYPE (GstPipeWireSrc, gst_pipewire_src, GST_TYPE_PUSH_SRC);

static GstStateChangeReturn
gst_pipewire_src_change_state (GstElement * element, GstStateChange transition);

static gboolean gst_pipewire_src_negotiate (GstBaseSrc * basesrc);

static GstFlowReturn gst_pipewire_src_create (GstPushSrc * psrc,
    GstBuffer ** buffer);
static gboolean gst_pipewire_src_unlock (GstBaseSrc * basesrc);
static gboolean gst_pipewire_src_unlock_stop (GstBaseSrc * basesrc);
static gboolean gst_pipewire_src_start (GstBaseSrc * basesrc);
static gboolean gst_pipewire_src_stop (GstBaseSrc * basesrc);
static gboolean gst_pipewire_src_event (GstBaseSrc * src, GstEvent * event);
static gboolean gst_pipewire_src_query (GstBaseSrc * src, GstQuery * query);

static void
gst_pipewire_src_set_property (GObject * object, guint prop_id,
    const GValue * value, GParamSpec * pspec)
{
  GstPipeWireSrc *pwsrc = GST_PIPEWIRE_SRC (object);

  switch (prop_id) {
    case PROP_PATH:
      g_free (pwsrc->path);
      pwsrc->path = g_value_dup_string (value);
      break;

    case PROP_CLIENT_NAME:
      g_free (pwsrc->client_name);
      pwsrc->client_name = g_value_dup_string (value);
      break;

    case PROP_STREAM_PROPERTIES:
      if (pwsrc->properties)
        gst_structure_free (pwsrc->properties);
      pwsrc->properties =
          gst_structure_copy (gst_value_get_structure (value));
      break;

    case PROP_ALWAYS_COPY:
      pwsrc->always_copy = g_value_get_boolean (value);
      break;

    case PROP_FD:
      pwsrc->fd = g_value_get_int (value);
      break;

    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
  }
}

static void
gst_pipewire_src_get_property (GObject * object, guint prop_id,
    GValue * value, GParamSpec * pspec)
{
  GstPipeWireSrc *pwsrc = GST_PIPEWIRE_SRC (object);

  switch (prop_id) {
    case PROP_PATH:
      g_value_set_string (value, pwsrc->path);
      break;

    case PROP_CLIENT_NAME:
      g_value_set_string (value, pwsrc->client_name);
      break;

    case PROP_STREAM_PROPERTIES:
      gst_value_set_structure (value, pwsrc->properties);
      break;

    case PROP_ALWAYS_COPY:
      g_value_set_boolean (value, pwsrc->always_copy);
      break;

    case PROP_FD:
      g_value_set_int (value, pwsrc->fd);
      break;

    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
  }
}

static GstClock *
gst_pipewire_src_provide_clock (GstElement * elem)
{
  GstPipeWireSrc *pwsrc = GST_PIPEWIRE_SRC (elem);
  GstClock *clock;

  GST_OBJECT_LOCK (pwsrc);
  if (!GST_OBJECT_FLAG_IS_SET (pwsrc, GST_ELEMENT_FLAG_PROVIDE_CLOCK))
    goto clock_disabled;

  if (pwsrc->clock && pwsrc->is_live)
    clock = GST_CLOCK_CAST (gst_object_ref (pwsrc->clock));
  else
    clock = NULL;
  GST_OBJECT_UNLOCK (pwsrc);

  return clock;

  /* ERRORS */
clock_disabled:
  {
    GST_DEBUG_OBJECT (pwsrc, "clock provide disabled");
    GST_OBJECT_UNLOCK (pwsrc);
    return NULL;
  }
}

static void
clear_queue (GstPipeWireSrc *pwsrc)
{
  g_queue_foreach (&pwsrc->queue, (GFunc) gst_mini_object_unref, NULL);
  g_queue_clear (&pwsrc->queue);
}

static void
gst_pipewire_src_finalize (GObject * object)
{
  GstPipeWireSrc *pwsrc = GST_PIPEWIRE_SRC (object);

  clear_queue (pwsrc);

  pw_core_destroy (pwsrc->core);
  pwsrc->core = NULL;
  pwsrc->type = NULL;
  pw_thread_loop_destroy (pwsrc->main_loop);
  pwsrc->main_loop = NULL;
  pw_loop_destroy (pwsrc->loop);
  pwsrc->loop = NULL;

  if (pwsrc->properties)
    gst_structure_free (pwsrc->properties);
  if (pwsrc->clock)
    gst_object_unref (pwsrc->clock);
  g_free (pwsrc->path);
  g_free (pwsrc->client_name);
  g_object_unref(pwsrc->pool);

  G_OBJECT_CLASS (parent_class)->finalize (object);
}

static void
gst_pipewire_src_class_init (GstPipeWireSrcClass * klass)
{
  GObjectClass *gobject_class;
  GstElementClass *gstelement_class;
  GstBaseSrcClass *gstbasesrc_class;
  GstPushSrcClass *gstpushsrc_class;

  gobject_class = (GObjectClass *) klass;
  gstelement_class = (GstElementClass *) klass;
  gstbasesrc_class = (GstBaseSrcClass *) klass;
  gstpushsrc_class = (GstPushSrcClass *) klass;

  gobject_class->finalize = gst_pipewire_src_finalize;
  gobject_class->set_property = gst_pipewire_src_set_property;
  gobject_class->get_property = gst_pipewire_src_get_property;

  g_object_class_install_property (gobject_class,
                                   PROP_PATH,
                                   g_param_spec_string ("path",
                                                        "Path",
                                                        "The source path to connect to (NULL = default)",
                                                        NULL,
                                                        G_PARAM_READWRITE |
                                                        G_PARAM_STATIC_STRINGS));

  g_object_class_install_property (gobject_class,
                                   PROP_CLIENT_NAME,
                                   g_param_spec_string ("client-name",
                                                        "Client Name",
                                                        "The client name to use (NULL = default)",
                                                        NULL,
                                                        G_PARAM_READWRITE |
                                                        G_PARAM_STATIC_STRINGS));

  g_object_class_install_property (gobject_class,
                                   PROP_STREAM_PROPERTIES,
                                   g_param_spec_boxed ("stream-properties",
                                                       "stream properties",
                                                       "list of PipeWire stream properties",
                                                       GST_TYPE_STRUCTURE,
                                                       G_PARAM_READWRITE |
                                                       G_PARAM_STATIC_STRINGS));

  g_object_class_install_property (gobject_class,
                                   PROP_ALWAYS_COPY,
                                   g_param_spec_boolean ("always-copy",
                                                         "Always copy",
                                                         "Always copy the buffer and data",
                                                         DEFAULT_ALWAYS_COPY,
                                                         G_PARAM_READWRITE |
                                                         G_PARAM_STATIC_STRINGS));

   g_object_class_install_property (gobject_class,
                                    PROP_FD,
                                    g_param_spec_int ("fd",
                                                      "Fd",
                                                      "The fd to connect with",
                                                      -1, G_MAXINT, -1,
                                                      G_PARAM_READWRITE |
                                                      G_PARAM_STATIC_STRINGS));

  gstelement_class->provide_clock = gst_pipewire_src_provide_clock;
  gstelement_class->change_state = gst_pipewire_src_change_state;

  gst_element_class_set_static_metadata (gstelement_class,
      "PipeWire source", "Source/Video",
      "Uses PipeWire to create video", "Wim Taymans <wim.taymans@gmail.com>");

  gst_element_class_add_pad_template (gstelement_class,
      gst_static_pad_template_get (&gst_pipewire_src_template));

  gstbasesrc_class->negotiate = gst_pipewire_src_negotiate;
  gstbasesrc_class->unlock = gst_pipewire_src_unlock;
  gstbasesrc_class->unlock_stop = gst_pipewire_src_unlock_stop;
  gstbasesrc_class->start = gst_pipewire_src_start;
  gstbasesrc_class->stop = gst_pipewire_src_stop;
  gstbasesrc_class->event = gst_pipewire_src_event;
  gstbasesrc_class->query = gst_pipewire_src_query;
  gstpushsrc_class->create = gst_pipewire_src_create;

  GST_DEBUG_CATEGORY_INIT (pipewire_src_debug, "pipewiresrc", 0,
      "PipeWire Source");

  process_mem_data_quark = g_quark_from_static_string ("GstPipeWireSrcProcessMemQuark");
}

static void
gst_pipewire_src_init (GstPipeWireSrc * src)
{
  /* we operate in time */
  gst_base_src_set_format (GST_BASE_SRC (src), GST_FORMAT_TIME);

  GST_OBJECT_FLAG_SET (src, GST_ELEMENT_FLAG_PROVIDE_CLOCK);

  src->always_copy = DEFAULT_ALWAYS_COPY;
  src->fd = -1;

  g_queue_init (&src->queue);

  src->client_name = pw_get_client_name ();

  src->pool =  gst_pipewire_pool_new ();
  src->loop = pw_loop_new (NULL);
  src->main_loop = pw_thread_loop_new (src->loop, "pipewire-main-loop");
  src->core = pw_core_new (src->loop, NULL);
  src->type = pw_core_get_type (src->core);
  src->pool->t = src->type;
  GST_DEBUG ("loop %p, mainloop %p", src->loop, src->main_loop);

}

static gboolean
buffer_recycle (GstMiniObject *obj)
{
  GstPipeWireSrc *src;
  GstPipeWirePoolData *data;

  gst_mini_object_ref (obj);
  data = gst_pipewire_pool_get_data (GST_BUFFER_CAST(obj));

  GST_BUFFER_FLAGS (obj) = data->flags;
  src = data->owner;

  GST_LOG_OBJECT (obj, "recycle buffer");
  pw_thread_loop_lock (src->main_loop);
  pw_stream_queue_buffer (src->stream, data->b);
  pw_thread_loop_unlock (src->main_loop);

  return FALSE;
}

static void
on_add_buffer (void *_data, struct pw_buffer *b)
{
  GstPipeWireSrc *pwsrc = _data;
  GstPipeWirePoolData *data;

  GST_LOG_OBJECT (pwsrc, "add buffer");
  gst_pipewire_pool_wrap_buffer (pwsrc->pool, b);
  data = b->user_data;
  data->owner = pwsrc;
  GST_MINI_OBJECT_CAST (data->buf)->dispose = buffer_recycle;
}

static void
on_remove_buffer (void *_data, struct pw_buffer *b)
{
  GstPipeWireSrc *pwsrc = _data;
  GstPipeWirePoolData *data = b->user_data;
  GstBuffer *buf = data->buf;
  GList *walk;

  GST_LOG_OBJECT (pwsrc, "remove buffer %p", buf);

  GST_MINI_OBJECT_CAST (buf)->dispose = NULL;

  walk = pwsrc->queue.head;
  while (walk) {
    GList *next = walk->next;

    if (walk->data == buf) {
      gst_buffer_unref (buf);
      g_queue_delete_link (&pwsrc->queue, walk);
    }
    walk = next;
  }
  gst_buffer_unref (buf);
}

static void
on_process (void *_data)
{
  GstPipeWireSrc *pwsrc = _data;
  struct pw_buffer *b;
  GstBuffer *buf;
  GstPipeWirePoolData *data;
  struct spa_meta_header *h;
  guint i;

  b = pw_stream_dequeue_buffer (pwsrc->stream);
  if (b == NULL)
	  return;

  data = b->user_data;
  buf = data->buf;

  GST_LOG_OBJECT (pwsrc, "got new buffer %p", buf);

  GST_BUFFER_PTS (buf) = GST_CLOCK_TIME_NONE;
  GST_BUFFER_DTS (buf) = GST_CLOCK_TIME_NONE;

  h = data->header;
  if (h) {
    GST_INFO ("pts %" G_GUINT64_FORMAT ", dts_offset %"G_GUINT64_FORMAT, h->pts, h->dts_offset);

    if (GST_CLOCK_TIME_IS_VALID (h->pts)) {
      GST_BUFFER_PTS (buf) = h->pts;
      if (GST_BUFFER_PTS (buf) + h->dts_offset > 0)
        GST_BUFFER_DTS (buf) = GST_BUFFER_PTS (buf) + h->dts_offset;
    }
    GST_BUFFER_OFFSET (buf) = h->seq;
  }
  for (i = 0; i < b->buffer->n_datas; i++) {
    struct spa_data *d = &b->buffer->datas[i];
    GstMemory *mem = gst_buffer_peek_memory (buf, i);
    mem->offset = SPA_MIN(d->chunk->offset, d->maxsize);
    mem->size = SPA_MIN(d->chunk->size, d->maxsize - mem->offset);
    mem->offset += data->offset;
  }


  gst_buffer_ref (buf);
  g_queue_push_tail (&pwsrc->queue, buf);

  pw_thread_loop_signal (pwsrc->main_loop, FALSE);
  return;
}

static void
on_state_changed (void *data,
		  enum pw_stream_state old,
		  enum pw_stream_state state, const char *error)
{
  GstPipeWireSrc *pwsrc = data;

  GST_DEBUG ("got stream state %s", pw_stream_state_as_string (state));

  switch (state) {
    case PW_STREAM_STATE_UNCONNECTED:
    case PW_STREAM_STATE_CONNECTING:
    case PW_STREAM_STATE_CONFIGURE:
    case PW_STREAM_STATE_READY:
    case PW_STREAM_STATE_PAUSED:
    case PW_STREAM_STATE_STREAMING:
      break;
    case PW_STREAM_STATE_ERROR:
      GST_ELEMENT_ERROR (pwsrc, RESOURCE, FAILED,
          ("stream error: %s", error), (NULL));
      break;
  }
  pw_thread_loop_signal (pwsrc->main_loop, FALSE);
}

static void
parse_stream_properties (GstPipeWireSrc *pwsrc, const struct pw_properties *props)
{
  const gchar *var;
  gboolean is_live;

  GST_OBJECT_LOCK (pwsrc);
  var = pw_properties_get (props, PW_STREAM_PROP_IS_LIVE);
  is_live = pwsrc->is_live = var ? pw_properties_parse_bool(var) : FALSE;

  var = pw_properties_get (props, PW_STREAM_PROP_LATENCY_MIN);
  pwsrc->min_latency = var ? (GstClockTime) atoi (var) : 0;

  var = pw_properties_get (props, PW_STREAM_PROP_LATENCY_MAX);
  pwsrc->max_latency = var ? (GstClockTime) atoi (var) : GST_CLOCK_TIME_NONE;
  GST_OBJECT_UNLOCK (pwsrc);

  GST_DEBUG_OBJECT (pwsrc, "live %d", is_live);

  gst_base_src_set_live (GST_BASE_SRC (pwsrc), is_live);
}

static gboolean
gst_pipewire_src_stream_start (GstPipeWireSrc *pwsrc)
{
  const char *error = NULL;
  pw_thread_loop_lock (pwsrc->main_loop);
  GST_DEBUG_OBJECT (pwsrc, "doing stream start");
  while (TRUE) {
    enum pw_stream_state state = pw_stream_get_state (pwsrc->stream, &error);

    GST_DEBUG_OBJECT (pwsrc, "waiting for STREAMING, now %s", pw_stream_state_as_string (state));
    if (state == PW_STREAM_STATE_STREAMING)
      break;

    if (state == PW_STREAM_STATE_ERROR)
      goto start_error;

    if (pw_remote_get_state(pwsrc->remote, &error) == PW_REMOTE_STATE_ERROR)
      goto start_error;

    pw_thread_loop_wait (pwsrc->main_loop);
  }

  parse_stream_properties (pwsrc, pw_stream_get_properties (pwsrc->stream));
  GST_DEBUG_OBJECT (pwsrc, "signal started");
  pwsrc->started = TRUE;
  pw_thread_loop_signal (pwsrc->main_loop, FALSE);
  pw_thread_loop_unlock (pwsrc->main_loop);

  return TRUE;

start_error:
  {
    GST_DEBUG_OBJECT (pwsrc, "error starting stream: %s", error);
    pw_thread_loop_unlock (pwsrc->main_loop);
    return FALSE;
  }
}

static enum pw_stream_state
wait_negotiated (GstPipeWireSrc *this)
{
  enum pw_stream_state state;
  const char *error = NULL;

  pw_thread_loop_lock (this->main_loop);
  while (TRUE) {
    state = pw_stream_get_state (this->stream, &error);

    GST_DEBUG_OBJECT (this, "waiting for started signal, state now %s",
        pw_stream_state_as_string (state));

    if (state == PW_STREAM_STATE_ERROR)
      break;

    if (pw_remote_get_state(this->remote, &error) == PW_REMOTE_STATE_ERROR)
      break;

    if (this->started)
      break;

    pw_thread_loop_wait (this->main_loop);
  }
  GST_DEBUG_OBJECT (this, "got started signal");
  pw_thread_loop_unlock (this->main_loop);

  return state;
}

static gboolean
gst_pipewire_src_negotiate (GstBaseSrc * basesrc)
{
  GstPipeWireSrc *pwsrc = GST_PIPEWIRE_SRC (basesrc);
  GstCaps *thiscaps;
  GstCaps *caps = NULL;
  GstCaps *peercaps = NULL;
  gboolean result = FALSE;
  GPtrArray *possible;
  const char *error = NULL;

  /* first see what is possible on our source pad */
  thiscaps = gst_pad_query_caps (GST_BASE_SRC_PAD (basesrc), NULL);
  GST_DEBUG_OBJECT (basesrc, "caps of src: %" GST_PTR_FORMAT, thiscaps);
  /* nothing or anything is allowed, we're done */
  if (thiscaps == NULL)
    goto no_nego_needed;

  if (G_UNLIKELY (gst_caps_is_empty (thiscaps)))
    goto no_caps;

  /* get the peer caps */
  peercaps = gst_pad_peer_query_caps (GST_BASE_SRC_PAD (basesrc), thiscaps);
  GST_DEBUG_OBJECT (basesrc, "caps of peer: %" GST_PTR_FORMAT, peercaps);
  if (peercaps) {
    /* The result is already a subset of our caps */
    caps = peercaps;
    gst_caps_unref (thiscaps);
  } else {
    /* no peer, work with our own caps then */
    caps = thiscaps;
  }
  if (caps == NULL || gst_caps_is_empty (caps))
    goto no_common_caps;

  GST_DEBUG_OBJECT (basesrc, "have common caps: %" GST_PTR_FORMAT, caps);

  /* open a connection with these caps */
  possible = gst_caps_to_format_all (caps, pwsrc->type->param.idEnumFormat, pwsrc->type->map);
  gst_caps_unref (caps);

  /* first disconnect */
  pw_thread_loop_lock (pwsrc->main_loop);
  if (pw_stream_get_state(pwsrc->stream, &error) != PW_STREAM_STATE_UNCONNECTED) {
    GST_DEBUG_OBJECT (basesrc, "disconnect capture");
    pw_stream_disconnect (pwsrc->stream);
    while (TRUE) {
      enum pw_stream_state state = pw_stream_get_state (pwsrc->stream, &error);

      GST_DEBUG_OBJECT (basesrc, "waiting for UNCONNECTED, now %s", pw_stream_state_as_string (state));
      if (state == PW_STREAM_STATE_UNCONNECTED)
        break;

      if (state == PW_STREAM_STATE_ERROR) {
        g_ptr_array_unref (possible);
        goto connect_error;
      }

      pw_thread_loop_wait (pwsrc->main_loop);
    }
  }

  GST_DEBUG_OBJECT (basesrc, "connect capture with path %s", pwsrc->path);
  pw_stream_connect (pwsrc->stream,
                     PW_DIRECTION_INPUT,
                     pwsrc->path,
                     PW_STREAM_FLAG_AUTOCONNECT,
                     (const struct spa_pod **)possible->pdata,
                     possible->len);
  g_ptr_array_free (possible, TRUE);

  while (TRUE) {
    enum pw_stream_state state = pw_stream_get_state (pwsrc->stream, &error);

    GST_DEBUG_OBJECT (basesrc, "waiting for PAUSED, now %s", pw_stream_state_as_string (state));
    if (state == PW_STREAM_STATE_PAUSED ||
        state == PW_STREAM_STATE_STREAMING)
      break;

    if (state == PW_STREAM_STATE_ERROR)
      goto connect_error;

    if (pw_remote_get_state(pwsrc->remote, &error) == PW_REMOTE_STATE_ERROR)
      goto connect_error;

    pw_thread_loop_wait (pwsrc->main_loop);
  }
  pw_thread_loop_unlock (pwsrc->main_loop);

  result = gst_pipewire_src_stream_start (pwsrc);

  pwsrc->negotiated = result;

  return result;

no_nego_needed:
  {
    GST_DEBUG_OBJECT (basesrc, "no negotiation needed");
    if (thiscaps)
      gst_caps_unref (thiscaps);
    return TRUE;
  }
no_caps:
  {
    GST_ELEMENT_ERROR (basesrc, STREAM, FORMAT,
        ("No supported formats found"),
        ("This element did not produce valid caps"));
    if (thiscaps)
      gst_caps_unref (thiscaps);
    return FALSE;
  }
no_common_caps:
  {
    GST_ELEMENT_ERROR (basesrc, STREAM, FORMAT,
        ("No supported formats found"),
        ("This element does not have formats in common with the peer"));
    if (caps)
      gst_caps_unref (caps);
    return FALSE;
  }
connect_error:
  {
    pw_thread_loop_unlock (pwsrc->main_loop);
    return FALSE;
  }
}

#define SPA_PROP_RANGE(min,max)	2,min,max

static void
on_format_changed (void *data,
                   const struct spa_pod *format)
{
  GstPipeWireSrc *pwsrc = data;
  GstCaps *caps;
  gboolean res;
  struct pw_core *core = pwsrc->core;
  struct pw_type *t = pw_core_get_type(core);

  if (format == NULL) {
    GST_DEBUG_OBJECT (pwsrc, "clear format");
    pw_stream_finish_format (pwsrc->stream, 0, NULL, 0);
    return;
  }

  caps = gst_caps_from_format (format, t->map);
  GST_DEBUG_OBJECT (pwsrc, "we got format %" GST_PTR_FORMAT, caps);
  res = gst_base_src_set_caps (GST_BASE_SRC (pwsrc), caps);
  gst_caps_unref (caps);

  if (res) {
    const struct spa_pod *params[2];
    struct spa_pod_builder b = { NULL };
    uint8_t buffer[512];

    spa_pod_builder_init (&b, buffer, sizeof (buffer));
    params[0] = spa_pod_builder_object (&b,
	t->param.idBuffers, t->param_buffers.Buffers,
	":", t->param_buffers.size,    "iru", 0,  SPA_PROP_RANGE(0, INT32_MAX),
	":", t->param_buffers.stride,  "iru", 0,  SPA_PROP_RANGE(0, INT32_MAX),
	":", t->param_buffers.buffers, "iru", 16, SPA_PROP_RANGE(1, INT32_MAX),
	":", t->param_buffers.align,   "i", 16);

    params[1] = spa_pod_builder_object (&b,
	t->param.idMeta, t->param_meta.Meta,
        ":", t->param_meta.type, "I", t->meta.Header,
        ":", t->param_meta.size, "i", sizeof (struct spa_meta_header));

    GST_DEBUG_OBJECT (pwsrc, "doing finish format");
    pw_stream_finish_format (pwsrc->stream, 0, params, 2);
  } else {
    GST_WARNING_OBJECT (pwsrc, "finish format with error");
    pw_stream_finish_format (pwsrc->stream, -EINVAL, NULL, 0);
  }
}

static gboolean
gst_pipewire_src_unlock (GstBaseSrc * basesrc)
{
  GstPipeWireSrc *pwsrc = GST_PIPEWIRE_SRC (basesrc);

  pw_thread_loop_lock (pwsrc->main_loop);
  GST_DEBUG_OBJECT (pwsrc, "setting flushing");
  pwsrc->flushing = TRUE;
  pw_thread_loop_signal (pwsrc->main_loop, FALSE);
  pw_thread_loop_unlock (pwsrc->main_loop);

  return TRUE;
}

static gboolean
gst_pipewire_src_unlock_stop (GstBaseSrc * basesrc)
{
  GstPipeWireSrc *pwsrc = GST_PIPEWIRE_SRC (basesrc);

  pw_thread_loop_lock (pwsrc->main_loop);
  GST_DEBUG_OBJECT (pwsrc, "unsetting flushing");
  pwsrc->flushing = FALSE;
  pw_thread_loop_unlock (pwsrc->main_loop);

  return TRUE;
}

static gboolean
gst_pipewire_src_event (GstBaseSrc * src, GstEvent * event)
{
  gboolean res = FALSE;

  switch (GST_EVENT_TYPE (event)) {
    case GST_EVENT_CUSTOM_UPSTREAM:
      if (gst_video_event_is_force_key_unit (event)) {
        GstClockTime running_time;
        gboolean all_headers;
        guint count;

        gst_video_event_parse_upstream_force_key_unit (event,
                &running_time, &all_headers, &count);

#if 0
        pw_buffer_builder_init (&b);

        refresh.last_id = 0;
        refresh.request_type = all_headers ? 1 : 0;
        refresh.pts = running_time;
        pw_buffer_builder_add_refresh_request (&b, &refresh);

        pw_buffer_builder_end (&b, &pbuf);

        GST_OBJECT_LOCK (pwsrc);
        if (pwsrc->stream_state == PW_STREAM_STATE_STREAMING) {
          GST_DEBUG_OBJECT (pwsrc, "send refresh request");
          pw_stream_send_buffer (pwsrc->stream, &pbuf);
        }
        GST_OBJECT_UNLOCK (pwsrc);

        pw_buffer_unref (&pbuf);
#endif
        res = TRUE;
      } else {
        res = GST_BASE_SRC_CLASS (parent_class)->event (src, event);
      }
      break;
    default:
      res = GST_BASE_SRC_CLASS (parent_class)->event (src, event);
      break;
  }
  return res;
}

static gboolean
gst_pipewire_src_query (GstBaseSrc * src, GstQuery * query)
{
  gboolean res = FALSE;
  GstPipeWireSrc *pwsrc;

  pwsrc = GST_PIPEWIRE_SRC (src);

  switch (GST_QUERY_TYPE (query)) {
    case GST_QUERY_LATENCY:
      GST_OBJECT_LOCK (pwsrc);
      pwsrc->min_latency = 10000000;
      pwsrc->max_latency = GST_CLOCK_TIME_NONE;
      gst_query_set_latency (query, pwsrc->is_live, pwsrc->min_latency, pwsrc->max_latency);
      GST_OBJECT_UNLOCK (pwsrc);
      res = TRUE;
      break;
    default:
      res = GST_BASE_SRC_CLASS (parent_class)->query (src, query);
      break;
  }
  return res;
}

static GstFlowReturn
gst_pipewire_src_create (GstPushSrc * psrc, GstBuffer ** buffer)
{
  GstPipeWireSrc *pwsrc;
  GstClockTime pts, dts, base_time;
  const char *error = NULL;
  GstBuffer *buf;

  pwsrc = GST_PIPEWIRE_SRC (psrc);

  if (!pwsrc->negotiated)
    goto not_negotiated;

  pw_thread_loop_lock (pwsrc->main_loop);
  while (TRUE) {
    enum pw_stream_state state;

    if (pwsrc->flushing)
      goto streaming_stopped;

    if (pwsrc->stream == NULL)
      goto streaming_error;

    state = pw_stream_get_state (pwsrc->stream, &error);
    if (state == PW_STREAM_STATE_ERROR)
      goto streaming_error;

    if (state != PW_STREAM_STATE_STREAMING)
      goto streaming_stopped;

    buf = g_queue_pop_head (&pwsrc->queue);
    GST_DEBUG ("popped buffer %p", buf);
    if (buf != NULL)
      break;

    pw_thread_loop_wait (pwsrc->main_loop);
  }
  pw_thread_loop_unlock (pwsrc->main_loop);

  gst_buffer_unref (buf);

  if (pwsrc->always_copy) {
    *buffer = gst_buffer_copy_deep (buf);
    gst_buffer_unref (buf);
  }
  else
    *buffer = buf;

  if (pwsrc->is_live)
    base_time = GST_ELEMENT_CAST (psrc)->base_time;
  else
    base_time = 0;

  pts = GST_BUFFER_PTS (*buffer);
  dts = GST_BUFFER_DTS (*buffer);

  if (GST_CLOCK_TIME_IS_VALID (pts))
    pts = (pts >= base_time ? pts - base_time : 0);
  if (GST_CLOCK_TIME_IS_VALID (dts))
    dts = (dts >= base_time ? dts - base_time : 0);

  GST_INFO ("pts %" G_GUINT64_FORMAT ", dts %"G_GUINT64_FORMAT
      ", base-time %"GST_TIME_FORMAT" -> %"GST_TIME_FORMAT", %"GST_TIME_FORMAT,
      GST_BUFFER_PTS (*buffer), GST_BUFFER_DTS (*buffer), GST_TIME_ARGS (base_time),
      GST_TIME_ARGS (pts), GST_TIME_ARGS (dts));

  GST_BUFFER_PTS (*buffer) = pts;
  GST_BUFFER_DTS (*buffer) = dts;

  return GST_FLOW_OK;

not_negotiated:
  {
    return GST_FLOW_NOT_NEGOTIATED;
  }
streaming_error:
  {
    pw_thread_loop_unlock (pwsrc->main_loop);
    return GST_FLOW_ERROR;
  }
streaming_stopped:
  {
    pw_thread_loop_unlock (pwsrc->main_loop);
    return GST_FLOW_FLUSHING;
  }
}

static gboolean
gst_pipewire_src_start (GstBaseSrc * basesrc)
{
  return TRUE;
}

static gboolean
gst_pipewire_src_stop (GstBaseSrc * basesrc)
{
  GstPipeWireSrc *pwsrc;

  pwsrc = GST_PIPEWIRE_SRC (basesrc);

  pw_thread_loop_lock (pwsrc->main_loop);
  clear_queue (pwsrc);
  pw_thread_loop_unlock (pwsrc->main_loop);

  return TRUE;
}

static void
on_remote_state_changed (void *data, enum pw_remote_state old, enum pw_remote_state state, const char *error)
{
  GstPipeWireSrc *pwsrc = data;

  GST_DEBUG ("got remote state %s", pw_remote_state_as_string (state));

  switch (state) {
    case PW_REMOTE_STATE_UNCONNECTED:
    case PW_REMOTE_STATE_CONNECTING:
    case PW_REMOTE_STATE_CONNECTED:
      break;
    case PW_REMOTE_STATE_ERROR:
      GST_ELEMENT_ERROR (pwsrc, RESOURCE, FAILED,
          ("remote error: %s", error), (NULL));
      break;
  }
  pw_thread_loop_signal (pwsrc->main_loop, FALSE);
}

static gboolean
copy_properties (GQuark field_id,
                 const GValue *value,
                 gpointer user_data)
{
  struct pw_properties *properties = user_data;

  if (G_VALUE_HOLDS_STRING (value))
    pw_properties_set (properties,
                       g_quark_to_string (field_id),
                       g_value_get_string (value));
  return TRUE;
}

static const struct pw_remote_events remote_events = {
	PW_VERSION_REMOTE_EVENTS,
	.state_changed = on_remote_state_changed,
};

static const struct pw_stream_events stream_events = {
	PW_VERSION_STREAM_EVENTS,
	.state_changed = on_state_changed,
	.format_changed = on_format_changed,
	.add_buffer = on_add_buffer,
	.remove_buffer = on_remove_buffer,
	.process = on_process,
};

static gboolean
gst_pipewire_src_open (GstPipeWireSrc * pwsrc)
{
  struct pw_properties *props;
  const char *error = NULL;

  if (pw_thread_loop_start (pwsrc->main_loop) < 0)
    goto mainloop_failed;

  pw_thread_loop_lock (pwsrc->main_loop);
  if ((pwsrc->remote = pw_remote_new (pwsrc->core, NULL, 0)) == NULL)
    goto no_remote;

  pw_remote_add_listener (pwsrc->remote,
			  &pwsrc->remote_listener,
			  &remote_events, pwsrc);

  if (pwsrc->fd == -1)
    pw_remote_connect (pwsrc->remote);
  else
    pw_remote_connect_fd (pwsrc->remote, dup(pwsrc->fd));

  while (TRUE) {
    enum pw_remote_state state = pw_remote_get_state(pwsrc->remote, &error);

    GST_DEBUG ("waiting for CONNECTED, now %s", pw_remote_state_as_string (state));
    if (state == PW_REMOTE_STATE_CONNECTED)
      break;

    if (state == PW_REMOTE_STATE_ERROR)
      goto connect_error;

    pw_thread_loop_wait (pwsrc->main_loop);
  }

  if (pwsrc->properties) {
    props = pw_properties_new (NULL, NULL);
    gst_structure_foreach (pwsrc->properties, copy_properties, props);
  } else {
    props = NULL;
  }

  if ((pwsrc->stream = pw_stream_new (pwsrc->remote, pwsrc->client_name, props)) == NULL)
    goto no_stream;


  pw_stream_add_listener(pwsrc->stream,
			 &pwsrc->stream_listener,
			 &stream_events,
			 pwsrc);


  pwsrc->clock = gst_pipewire_clock_new (pwsrc->stream, pwsrc->last_time);
  pw_thread_loop_unlock (pwsrc->main_loop);

  return TRUE;

  /* ERRORS */
mainloop_failed:
  {
    GST_ELEMENT_ERROR (pwsrc, RESOURCE, FAILED, ("error starting mainloop"), (NULL));
    return FALSE;
  }
no_remote:
  {
    GST_ELEMENT_ERROR (pwsrc, RESOURCE, FAILED, ("can't create remote"), (NULL));
    pw_thread_loop_unlock (pwsrc->main_loop);
    return FALSE;
  }
connect_error:
  {
    pw_thread_loop_unlock (pwsrc->main_loop);
    return FALSE;
  }
no_stream:
  {
    GST_ELEMENT_ERROR (pwsrc, RESOURCE, FAILED, ("can't create stream"), (NULL));
    pw_thread_loop_unlock (pwsrc->main_loop);
    return FALSE;
  }
}

static void
gst_pipewire_src_close (GstPipeWireSrc * pwsrc)
{
  clear_queue (pwsrc);

  pw_thread_loop_stop (pwsrc->main_loop);

  pwsrc->last_time = gst_clock_get_time (pwsrc->clock);

  gst_element_post_message (GST_ELEMENT (pwsrc),
    gst_message_new_clock_lost (GST_OBJECT_CAST (pwsrc), pwsrc->clock));

  GST_OBJECT_LOCK (pwsrc);
  GST_PIPEWIRE_CLOCK (pwsrc->clock)->stream = NULL;
  g_clear_object (&pwsrc->clock);
  GST_OBJECT_UNLOCK (pwsrc);

  pw_stream_destroy (pwsrc->stream);
  pwsrc->stream = NULL;

  pw_remote_destroy (pwsrc->remote);
  pwsrc->remote = NULL;

}

static GstStateChangeReturn
gst_pipewire_src_change_state (GstElement * element, GstStateChange transition)
{
  GstStateChangeReturn ret;
  GstPipeWireSrc *this = GST_PIPEWIRE_SRC_CAST (element);

  switch (transition) {
    case GST_STATE_CHANGE_NULL_TO_READY:
      if (!gst_pipewire_src_open (this))
        goto open_failed;
      break;
    case GST_STATE_CHANGE_READY_TO_PAUSED:
      break;
    case GST_STATE_CHANGE_PAUSED_TO_PLAYING:
      /* uncork and start recording */
      break;
    case GST_STATE_CHANGE_PLAYING_TO_PAUSED:
      /* stop recording ASAP by corking */
      break;
    default:
      break;
  }

  ret = GST_ELEMENT_CLASS (parent_class)->change_state (element, transition);

  switch (transition) {
    case GST_STATE_CHANGE_READY_TO_PAUSED:
      if (wait_negotiated (this) == PW_STREAM_STATE_ERROR)
        goto open_failed;

      if (gst_base_src_is_live (GST_BASE_SRC (element)))
        ret = GST_STATE_CHANGE_NO_PREROLL;
      break;
    case GST_STATE_CHANGE_PLAYING_TO_PAUSED:
      break;
    case GST_STATE_CHANGE_PAUSED_TO_READY:
      this->negotiated = FALSE;
      break;
    case GST_STATE_CHANGE_READY_TO_NULL:
      gst_pipewire_src_close (this);
      break;
    default:
      break;
  }
  return ret;

  /* ERRORS */
open_failed:
  {
    return GST_STATE_CHANGE_FAILURE;
  }
}
