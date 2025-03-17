/* PipeWire */
/* SPDX-FileCopyrightText: Copyright © 2021 Wim Taymans */
/* SPDX-License-Identifier: MIT */

#include <string.h>
#include <stdio.h>
#include <errno.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <stdlib.h>
#include <signal.h>
#include <limits.h>
#include <math.h>

#include "config.h"

#include <spa/utils/result.h>
#include <spa/utils/string.h>
#include <spa/utils/json.h>
#include <spa/utils/ratelimit.h>
#include <spa/debug/types.h>
#include <spa/pod/builder.h>
#include <spa/param/audio/format-utils.h>
#include <spa/param/latency-utils.h>
#include <spa/param/audio/raw.h>
#include <spa/param/audio/raw-json.h>
#include <spa/control/ump-utils.h>

#include <pipewire/impl.h>
#include <pipewire/i18n.h>

#include "module-jack-tunnel/weakjack.h"

/** \page page_module_jack_tunnel JACK Tunnel
 *
 * The jack-tunnel module provides a source or sink that tunnels all audio to
 * a JACK server.
 *
 * This module is usually used together with \ref page_module_jackdbus_detect that will
 * automatically load the tunnel with the right parameters based on dbus
 * information.
 *
 * ## Module Name
 *
 * `libpipewire-module-jack-tunnel`
 *
 * ## Module Options
 *
 * - `jack.library`: the libjack to load, by default libjack.so.0 is searched in
 *			JACK_PATH directories and then some standard library paths.
 *			Can be an absolute path.
 * - `jack.server`: the name of the JACK server to tunnel to.
 * - `jack.client-name`: the name of the JACK client.
 * - `jack.connect`: if jack ports should be connected automatically. Can also be
 *                   placed per stream.
 * - `jack.connect-audio`: An array of audio ports to connect to. Can also be placed per
 *                   stream. An empty array will not connect anything, even when
 *                   jack.connect is true.
 * - `jack.connect-midi`: An array of midi ports to connect to. Can also be placed per
 *                   stream. An empty array will not connect anything, even when
 *                   jack.connect is true.
 * - `tunnel.mode`: the tunnel mode, sink|source|duplex, default duplex
 * - `midi.ports`: the number of midi ports. Can also be added to the stream props.
 * - `source.props`: Extra properties for the source filter.
 * - `sink.props`: Extra properties for the sink filter.
 *
 * ## General options
 *
 * Options with well-known behavior.
 *
 * - \ref PW_KEY_REMOTE_NAME
 * - \ref PW_KEY_AUDIO_CHANNELS
 * - \ref SPA_KEY_AUDIO_POSITION
 * - \ref PW_KEY_NODE_NAME
 * - \ref PW_KEY_NODE_DESCRIPTION
 * - \ref PW_KEY_NODE_GROUP
 * - \ref PW_KEY_NODE_VIRTUAL
 * - \ref PW_KEY_MEDIA_CLASS
 * - \ref PW_KEY_TARGET_OBJECT to specify the remote node.name or serial.id to link to
 *
 * ## Example configuration of a duplex sink/source
 *
 *\code{.unparsed}
 * # ~/.config/pipewire/pipewire.conf.d/my-jack-tunnel.conf
 *
 * context.modules = [
 * {   name = libpipewire-module-jack-tunnel
 *     args = {
 *         #jack.library     = libjack.so.0
 *         #jack.server      = null
 *         #jack.client-name = PipeWire
 *         #jack.connect     = true
 *         #jack.connect-audio = [ playback_1 playback_2 ]
 *         #jack.connect-midi = [ midi_playback_1 ]
 *         #tunnel.mode      = duplex
 *         #midi.ports       = 0
 *         #audio.channels   = 2
 *         #audio.position   = [ FL FR ]
 *         source.props = {
 *             # extra sink properties
 *         }
 *         sink.props = {
 *             # extra sink properties
 *         }
 *     }
 * }
 * ]
 *\endcode
 */

#define NAME "jack-tunnel"

PW_LOG_TOPIC_STATIC(mod_topic, "mod." NAME);
#define PW_LOG_TOPIC_DEFAULT mod_topic

#define MAX_PORTS	128

#define DEFAULT_CLIENT_NAME	"PipeWire"
#define DEFAULT_POSITION	"[ FL FR ]"
#define DEFAULT_MIDI_PORTS	1

#define MODULE_USAGE	"( remote.name=<remote> ] "				\
			"( jack.library=<jack library path> ) "			\
			"( jack.server=<server name> ) "			\
			"( jack.client-name=<name of the JACK client> ] "	\
			"( jack.connect=<bool, autoconnect ports> ] "		\
			"( jack.connect-audio=<array, port names to connect> ] "\
			"( jack.connect-midi=<array, port names to connect> ] " \
			"( tunnel.mode=<sink|source|duplex> ] "			\
			"( midi.ports=<number of midi ports> ] "		\
			"( audio.channels=<number of channels> ] "		\
			"( audio.position=<channel map> ] "			\
			"( source.props=<properties> ) "			\
			"( sink.props=<properties> ) "


static const struct spa_dict_item module_props[] = {
	{ PW_KEY_MODULE_AUTHOR, "Wim Taymans <wim.taymans@gmail.com>" },
	{ PW_KEY_MODULE_DESCRIPTION, "Create a JACK tunnel" },
	{ PW_KEY_MODULE_USAGE, MODULE_USAGE },
	{ PW_KEY_MODULE_VERSION, PACKAGE_VERSION },
};

static struct weakjack jack;

struct port {
	jack_port_t *jack_port;

	enum spa_direction direction;
	struct spa_latency_info latency[2];
	bool latency_changed[2];
	unsigned int is_midi:1;
};

struct volume {
	bool mute;
	uint32_t n_volumes;
	float volumes[SPA_AUDIO_MAX_CHANNELS];
};

struct stream {
	struct impl *impl;

	enum spa_direction direction;
	struct pw_properties *props;
	struct pw_filter *filter;
	struct spa_hook listener;
	struct spa_audio_info_raw info;
	uint32_t n_midi;
	uint32_t n_ports;
	struct port *ports[MAX_PORTS];
	struct volume volume;

	unsigned int running:1;
};

struct impl {
	struct pw_context *context;
	struct pw_loop *main_loop;
	struct spa_system *system;

#define MODE_SINK	(1<<0)
#define MODE_SOURCE	(1<<1)
#define MODE_DUPLEX	(MODE_SINK|MODE_SOURCE)
	uint32_t mode;
	struct pw_properties *props;

	struct pw_impl_module *module;

	struct spa_hook module_listener;

	struct pw_core *core;
	struct spa_hook core_proxy_listener;
	struct spa_hook core_listener;

	struct spa_ratelimit rate_limit;

	struct spa_io_position *position;

	struct stream source;
	struct stream sink;

	uint32_t samplerate;

	jack_client_t *client;
	jack_nframes_t current_frames;

	uint32_t pw_xrun;
	uint32_t jack_xrun;

	unsigned int do_disconnect:1;
	unsigned int triggered:1;
	unsigned int done:1;
	unsigned int new_xrun:1;
	unsigned int fix_midi:1;
};

static void reset_volume(struct volume *vol, uint32_t n_volumes)
{
	uint32_t i;
	vol->mute = false;
	vol->n_volumes = n_volumes;
	for (i = 0; i < n_volumes; i++)
		vol->volumes[i] = 1.0f;
}

static inline void do_volume(float *dst, const float *src, struct volume *vol, uint32_t ch, uint32_t n_samples)
{
	float v = vol->mute ? 0.0f : vol->volumes[ch];

	if (v == 0.0f || src == NULL)
		memset(dst, 0, n_samples * sizeof(float));
	else if (v == 1.0f)
		memcpy(dst, src, n_samples * sizeof(float));
	else {
		uint32_t i;
		for (i = 0; i < n_samples; i++)
			dst[i] = src[i] * v;
	}
}

static inline void fix_midi_event(uint8_t *data, size_t size)
{
	/* fixup NoteOn with vel 0 */
	if (size > 2 && (data[0] & 0xF0) == 0x90 && data[2] == 0x00) {
		data[0] = 0x80 + (data[0] & 0x0F);
		data[2] = 0x40;
	}
}

static void midi_to_jack(struct impl *impl, float *dst, float *src, uint32_t n_samples)
{
	struct spa_pod *pod;
	struct spa_pod_sequence *seq;
	struct spa_pod_control *c;
	int res;

	jack.midi_clear_buffer(dst);
	if (src == NULL)
		return;

	if ((pod = spa_pod_from_data(src, n_samples * sizeof(float), 0, n_samples * sizeof(float))) == NULL)
		return;
	if (!spa_pod_is_sequence(pod))
		return;

	seq = (struct spa_pod_sequence*)pod;

	SPA_POD_SEQUENCE_FOREACH(seq, c) {
		uint8_t data[16];
		int size;

		if (c->type != SPA_CONTROL_UMP)
			continue;

		size = spa_ump_to_midi(SPA_POD_BODY(&c->value),
				SPA_POD_BODY_SIZE(&c->value), data, sizeof(data));
		if (size <= 0)
			continue;

		if (impl->fix_midi)
			fix_midi_event(data, size);

		if ((res = jack.midi_event_write(dst, c->offset, data, size)) < 0)
			pw_log_warn("midi %p: can't write event: %s", dst,
					spa_strerror(res));
	}
}

static void jack_to_midi(float *dst, float *src, uint32_t size)
{
	struct spa_pod_builder b = { 0, };
	uint32_t i, count;
	struct spa_pod_frame f;

	count = src ? jack.midi_get_event_count(src) : 0;

	spa_pod_builder_init(&b, dst, size);
	spa_pod_builder_push_sequence(&b, &f, 0);
	for (i = 0; i < count; i++) {
		jack_midi_event_t ev;
		uint64_t state = 0;

		jack.midi_event_get(&ev, src, i);

		while (ev.size > 0) {
			uint32_t ump[4];
			int ump_size = spa_ump_from_midi(&ev.buffer, &ev.size, ump, sizeof(ump), 0, &state);
			if (ump_size <= 0)
				break;

			spa_pod_builder_control(&b, ev.time, SPA_CONTROL_UMP);
	                spa_pod_builder_bytes(&b, ump, ump_size);
		}
	}
	spa_pod_builder_pop(&b, &f);
}

static void stream_destroy(void *d)
{
	struct stream *s = d;
	spa_hook_remove(&s->listener);
	s->filter = NULL;
}

static void stream_state_changed(void *d, enum pw_filter_state old,
		enum pw_filter_state state, const char *error)
{
	struct stream *s = d;
	struct impl *impl = s->impl;
	switch (state) {
	case PW_FILTER_STATE_UNCONNECTED:
		pw_impl_module_schedule_destroy(impl->module);
		break;
	case PW_FILTER_STATE_ERROR:
		pw_log_warn("stream %p: error: %s", s, error);
		break;
	case PW_FILTER_STATE_PAUSED:
		s->running = false;
		break;
	case PW_FILTER_STATE_STREAMING:
		s->running = true;
		break;
	default:
		break;
	}
}

static void sink_process(void *d, struct spa_io_position *position)
{
	struct stream *s = d;
	struct impl *impl = s->impl;
	uint32_t i, n_samples = position->clock.duration;

	if (impl->mode & MODE_SINK && impl->triggered) {
		impl->triggered = false;
		return;
	}

	for (i = 0; i < s->n_ports; i++) {
		struct port *p = s->ports[i];
		float *src, *dst;
		if (p == NULL)
			continue;
		src = pw_filter_get_dsp_buffer(p, n_samples);

		if (p->jack_port == NULL)
			continue;

		dst = jack.port_get_buffer(p->jack_port, n_samples);
		if (dst == NULL)
			continue;

		if (SPA_UNLIKELY(p->is_midi))
			midi_to_jack(impl, dst, src, n_samples);
		else
			do_volume(dst, src, &s->volume, i, n_samples);
	}
	pw_log_trace_fp("done %u %u", impl->current_frames, n_samples);
	if (impl->mode & MODE_SINK) {
		impl->done = true;
		jack.cycle_signal(impl->client, 0);
	}
}

static void source_process(void *d, struct spa_io_position *position)
{
	struct stream *s = d;
	struct impl *impl = s->impl;
	uint32_t i, n_samples = position->clock.duration;

	if (impl->mode == MODE_SOURCE && !impl->triggered) {
		pw_log_trace_fp("done %u", impl->current_frames);
		impl->done = true;
		jack.cycle_signal(impl->client, 0);
		return;
	}
	impl->triggered = false;

	for (i = 0; i < s->n_ports; i++) {
		struct port *p = s->ports[i];
		float *src, *dst;

		if (p == NULL)
			continue;

		dst = pw_filter_get_dsp_buffer(p, n_samples);
		if (dst == NULL || p->jack_port == NULL)
			continue;

		src = jack.port_get_buffer (p->jack_port, n_samples);

		if (SPA_UNLIKELY(p->is_midi))
			jack_to_midi(dst, src, n_samples);
		else
			do_volume(dst, src, &s->volume, i, n_samples);
	}
}

static void stream_io_changed(void *data, void *port_data, uint32_t id, void *area, uint32_t size)
{
	struct stream *s = data;
	struct impl *impl = s->impl;
	if (port_data == NULL) {
		switch (id) {
		case SPA_IO_Position:
			impl->position = area;
			break;
		default:
			break;
		}
	}
}

static void param_latency_changed(struct stream *s, const struct spa_pod *param,
		struct port *port)
{
	struct spa_latency_info latency;
	bool update = false;
	enum spa_direction direction = port->direction;

	if (param == NULL || spa_latency_parse(param, &latency) < 0)
		return;

	if (spa_latency_info_compare(&port->latency[direction], &latency)) {
		port->latency[direction] = latency;
		port->latency_changed[direction] = update = true;
	}
	if (update)
		jack.recompute_total_latencies(s->impl->client);
}

static void make_stream_ports(struct stream *s)
{
	struct impl *impl = s->impl;
	uint32_t i;
	struct pw_properties *props;
	const char *str, *prefix, *type;
	char name[256];
	char **audio_ports = NULL, **midi_ports = NULL;
	unsigned long jack_peer, jack_flags;
	bool do_connect, is_midi, strv_audio = false, strv_midi = false;
	int res, n_audio_ports = 0, n_midi_ports = 0;

	if (s->direction == PW_DIRECTION_INPUT) {
		/* sink */
		jack_peer = JackPortIsInput;
		jack_flags = JackPortIsOutput;
		prefix = "playback";
	} else {
		/* source */
		jack_peer = JackPortIsOutput;
		jack_flags = JackPortIsInput;
		prefix = "capture";
	}

	do_connect = pw_properties_get_bool(s->props, "jack.connect", true);

	str = pw_properties_get(s->props, "jack.connect-audio");
	if (str != NULL) {
		audio_ports = pw_strv_parse(str, strlen(str), INT_MAX, NULL);
		strv_audio = true;
	} else if (do_connect) {
		audio_ports = (char**)jack.get_ports(impl->client, NULL, JACK_DEFAULT_AUDIO_TYPE,
	                                JackPortIsPhysical|jack_peer);
	}
	str = pw_properties_get(s->props, "jack.connect-midi");
	if (str != NULL) {
		midi_ports = pw_strv_parse(str, strlen(str), INT_MAX, NULL);
		strv_midi = true;
	} else if (do_connect) {
		midi_ports = (char**)jack.get_ports(impl->client, NULL, JACK_DEFAULT_MIDI_TYPE,
	                                JackPortIsPhysical|jack_peer);
	}
	for (i = 0; i < s->n_ports; i++) {
		struct port *port = s->ports[i];
		char *link_port = NULL;

		if (port != NULL) {
			s->ports[i] = NULL;
			if (port->jack_port)
				jack.port_unregister(impl->client, port->jack_port);
			pw_filter_remove_port(port);
		}

		if (i < s->info.channels) {
			str = spa_debug_type_find_short_name(spa_type_audio_channel,
					s->info.position[i]);
			if (str)
				snprintf(name, sizeof(name), "%s_%s", prefix, str);
			else
				snprintf(name, sizeof(name), "%s_%d", prefix, i+1);

			props = pw_properties_new(
					PW_KEY_FORMAT_DSP, "32 bit float mono audio",
					PW_KEY_AUDIO_CHANNEL, str ? str : "UNK",
					PW_KEY_PORT_PHYSICAL, "true",
					PW_KEY_PORT_NAME, name,
					NULL);

			type = JACK_DEFAULT_AUDIO_TYPE;
			if (audio_ports && audio_ports[n_audio_ports])
				link_port = audio_ports[n_audio_ports++];

			is_midi = false;
		} else {
			snprintf(name, sizeof(name), "midi_%s_%d", prefix, i - s->info.channels + 1);
			props = pw_properties_new(
					PW_KEY_FORMAT_DSP, "32 bit raw UMP",
					PW_KEY_PORT_NAME, name,
					PW_KEY_PORT_PHYSICAL, "true",
					NULL);

			type = JACK_DEFAULT_MIDI_TYPE;
			if (midi_ports && midi_ports[n_midi_ports])
				link_port = midi_ports[n_midi_ports++];
			is_midi = true;
		}

		port = pw_filter_add_port(s->filter,
                        s->direction,
                        PW_FILTER_PORT_FLAG_MAP_BUFFERS,
                        sizeof(struct port),
			props, NULL, 0);

		port->is_midi = is_midi;
		port->jack_port = jack.port_register (impl->client, name, type, jack_flags, 0);

		if (link_port != NULL) {
			if (jack_flags & JackPortIsOutput) {
				pw_log_info("connecting ports '%s' to '%s'",
							jack.port_name(port->jack_port), link_port);
				if ((res = jack.connect(impl->client, jack.port_name(port->jack_port), link_port)))
					pw_log_warn("cannot connect ports '%s' to '%s': %s",
							jack.port_name(port->jack_port), link_port, strerror(res));
			} else {
				pw_log_info("connecting ports '%s' to '%s'",
							link_port, jack.port_name(port->jack_port));
				if ((res = jack.connect(impl->client, link_port, jack.port_name(port->jack_port))))
					pw_log_warn("cannot connect ports '%s' to '%s': %s",
							link_port, jack.port_name(port->jack_port), strerror(res));
			}
		}
		s->ports[i] = port;
	}
	if (audio_ports) {
		if (strv_audio)
			pw_free_strv(audio_ports);
		else
			jack.free(audio_ports);
	}
	if (midi_ports) {
		if (strv_midi)
			pw_free_strv(midi_ports);
		else
			jack.free(midi_ports);
	}
}

static struct spa_pod *make_props_param(struct spa_pod_builder *b,
		struct volume *vol)
{
	return spa_pod_builder_add_object(b, SPA_TYPE_OBJECT_Props, SPA_PARAM_Props,
			SPA_PROP_mute, SPA_POD_Bool(vol->mute),
			SPA_PROP_channelVolumes, SPA_POD_Array(sizeof(float),
				SPA_TYPE_Float, vol->n_volumes, vol->volumes));
}

static void parse_props(struct stream *s, const struct spa_pod *param)
{
	struct spa_pod_object *obj = (struct spa_pod_object *) param;
	struct spa_pod_prop *prop;
	uint8_t buffer[1024];
	struct spa_pod_builder b;
	const struct spa_pod *params[1];

	SPA_POD_OBJECT_FOREACH(obj, prop) {
		switch (prop->key) {
		case SPA_PROP_mute:
		{
			bool mute;
			if (spa_pod_get_bool(&prop->value, &mute) == 0)
				s->volume.mute = mute;
			break;
		}
		case SPA_PROP_channelVolumes:
		{
			uint32_t n;
			float vols[SPA_AUDIO_MAX_CHANNELS];
			if ((n = spa_pod_copy_array(&prop->value, SPA_TYPE_Float,
					vols, SPA_AUDIO_MAX_CHANNELS)) > 0) {
				s->volume.n_volumes = n;
				for (n = 0; n < s->volume.n_volumes; n++)
					s->volume.volumes[n] = vols[n];
			}
			break;
		}
		default:
			break;
		}
	}
	spa_pod_builder_init(&b, buffer, sizeof(buffer));
	params[0] = make_props_param(&b, &s->volume);

	pw_filter_update_params(s->filter, NULL, params, 1);
}

static void stream_param_changed(void *data, void *port_data, uint32_t id,
		const struct spa_pod *param)
{
	struct stream *s = data;
	if (port_data != NULL) {
		switch (id) {
		case SPA_PARAM_Latency:
			param_latency_changed(s, param, port_data);
			break;
		}
	} else {
		switch (id) {
		case SPA_PARAM_PortConfig:
			pw_log_debug("PortConfig");
			make_stream_ports(s);
			break;
		case SPA_PARAM_Props:
			pw_log_debug("Props");
			parse_props(s, param);
			break;
		}
	}
}

static const struct pw_filter_events sink_events = {
	PW_VERSION_FILTER_EVENTS,
	.destroy = stream_destroy,
	.state_changed = stream_state_changed,
	.param_changed = stream_param_changed,
	.io_changed = stream_io_changed,
	.process = sink_process
};

static const struct pw_filter_events source_events = {
	PW_VERSION_FILTER_EVENTS,
	.destroy = stream_destroy,
	.state_changed = stream_state_changed,
	.param_changed = stream_param_changed,
	.io_changed = stream_io_changed,
	.process = source_process,
};

static int make_stream(struct stream *s, const char *name)
{
	struct impl *impl = s->impl;
	uint32_t n_params;
	const struct spa_pod *params[4];
	uint8_t buffer[1024];
	struct spa_pod_builder b;

	n_params = 0;
	spa_pod_builder_init(&b, buffer, sizeof(buffer));

	s->filter = pw_filter_new(impl->core, name, pw_properties_copy(s->props));
	if (s->filter == NULL)
		return -errno;

	if (s->direction == PW_DIRECTION_INPUT) {
		pw_filter_add_listener(s->filter, &s->listener,
				&sink_events, s);
	} else {
		pw_filter_add_listener(s->filter, &s->listener,
				&source_events, s);
	}

	reset_volume(&s->volume, s->info.channels);

	n_params = 0;
	params[n_params++] = spa_format_audio_raw_build(&b,
			SPA_PARAM_EnumFormat, &s->info);
	params[n_params++] = spa_format_audio_raw_build(&b,
			SPA_PARAM_Format, &s->info);
	params[n_params++] = make_props_param(&b, &s->volume);

	return pw_filter_connect(s->filter,
			PW_FILTER_FLAG_DRIVER |
			PW_FILTER_FLAG_RT_PROCESS |
			PW_FILTER_FLAG_CUSTOM_LATENCY,
			params, n_params);
}

static int create_filters(struct impl *impl)
{
	int res = 0;

	if (impl->mode & MODE_SINK)
		res = make_stream(&impl->sink, "JACK Sink");

	if (impl->mode & MODE_SOURCE)
		res = make_stream(&impl->source, "JACK Source");

	return res;
}

static inline uint64_t get_time_nsec(struct impl *impl)
{
	uint64_t nsec;
	if (impl->sink.filter)
		nsec = pw_filter_get_nsec(impl->sink.filter);
	else if (impl->source.filter)
		nsec = pw_filter_get_nsec(impl->source.filter);
	else
		nsec = 0;
	return nsec;
}

static void *jack_process_thread(void *arg)
{
	struct impl *impl = arg;
	bool source_running, sink_running;
	jack_nframes_t nframes;

	while (true) {
		jack_nframes_t current_frames;
		jack_time_t current_usecs;
		jack_time_t next_usecs;
		float period_usecs;

		nframes = jack.cycle_wait (impl->client);

		jack.get_cycle_times(impl->client,
				&current_frames, &current_usecs,
				&next_usecs, &period_usecs);

		impl->current_frames = current_frames;

		source_running = impl->source.running;
		sink_running = impl->sink.running;

		pw_log_trace_fp("process %d %u %u %p %d", nframes, source_running,
				sink_running, impl->position, current_frames);

		if (impl->new_xrun) {
			int suppressed;
			if ((suppressed = spa_ratelimit_test(&impl->rate_limit, current_usecs)) >= 0) {
				pw_log_warn("Xrun: current_frames:%u JACK:%u PipeWire:%u (%d suppressed)",
						current_frames, impl->jack_xrun, impl->pw_xrun, suppressed);
			}
			impl->new_xrun = false;
		}

		if (impl->position) {
			struct spa_io_clock *c = &impl->position->clock;
			jack_position_t pos;
			uint64_t t1, t2, t3;
			int64_t d1;

			/* convert from JACK (likely MONOTONIC_RAW) to MONOTONIC */
			t1 = get_time_nsec(impl) / 1000;
			t2 = jack.get_time();
			t3 = get_time_nsec(impl) / 1000;
			d1 = t1 + (t3 - t1) / 2 - t2;

			current_usecs += d1;
			next_usecs += d1;

			c->nsec = current_usecs * SPA_NSEC_PER_USEC;
			c->rate = SPA_FRACTION(1, impl->samplerate);
			c->position = current_frames;
			c->duration = nframes;
			c->delay = 0;
			c->rate_diff = 1.0;
			c->next_nsec = next_usecs * SPA_NSEC_PER_USEC;

			c->target_rate = c->rate;
			c->target_duration = c->duration;

			jack.transport_query (impl->client, &pos);
		}
		if (impl->mode & MODE_SINK && sink_running) {
			impl->done = false;
			impl->triggered = true;
			pw_filter_trigger_process(impl->sink.filter);
		} else if (impl->mode == MODE_SOURCE && source_running) {
			impl->done = false;
			impl->triggered = true;
			pw_filter_trigger_process(impl->source.filter);
		} else {
			pw_log_trace_fp("done %d", nframes);
			jack.cycle_signal(impl->client, 0);
		}
	}
	return NULL;
}

static int jack_xrun(void *arg)
{
	struct impl *impl = arg;
	if (impl->done)
		impl->jack_xrun++;
	else
		impl->pw_xrun++;
	impl->new_xrun = true;
	return 0;
}

static int
do_schedule_destroy(struct spa_loop *loop,
	bool async, uint32_t seq, const void *data, size_t size, void *user_data)
{
	struct impl *impl = user_data;
	pw_impl_module_schedule_destroy(impl->module);
	return 0;
}

static void module_schedule_destroy(struct impl *impl)
{
	pw_loop_invoke(impl->main_loop, do_schedule_destroy, 1, NULL, 0, false, impl);
}

static void jack_info_shutdown(jack_status_t code, const char* reason, void *arg)
{
	struct impl *impl = arg;
	pw_log_warn("shutdown: %s (%08x)", reason, code);
	module_schedule_destroy(impl);
}

static void stream_update_latency(struct stream *s)
{
	uint8_t buffer[1024];
	struct spa_pod_builder b;
	const struct spa_pod *params[2];
	uint32_t i, n_params = 0;

	for (i = 0; i < s->n_ports; i++) {
		struct port *port = s->ports[i];
		if (port == NULL)
			continue;
		spa_pod_builder_init(&b, buffer, sizeof(buffer));
		n_params = 0;
		if (port->latency_changed[s->direction]) {
			params[n_params++] = spa_latency_build(&b,
				SPA_PARAM_Latency, &port->latency[s->direction]);
			port->latency_changed[s->direction] = false;
		}
		if (s->filter)
			pw_filter_update_params(s->filter, port, params, n_params);
	}
}

static int
do_update_latency(struct spa_loop *loop,
		bool async, uint32_t seq, const void *data, size_t size, void *user_data)
{
	struct impl *impl = user_data;

	if ((impl->mode & MODE_SINK))
		stream_update_latency(&impl->sink);

	if ((impl->mode & MODE_SOURCE))
		stream_update_latency(&impl->source);

	return 0;
}

static bool stream_handle_latency(struct stream *s, jack_latency_callback_mode_t mode)
{
	uint32_t i;
	struct spa_latency_info latency;
	jack_latency_range_t range;
	bool update = false;
	enum spa_direction other = SPA_DIRECTION_REVERSE(s->direction);
	struct port *port;

	if (mode == JackPlaybackLatency) {
		for (i = 0; i < s->n_ports; i++) {
			port = s->ports[i];
			if (port == NULL || port->jack_port == NULL)
				continue;

			jack.port_get_latency_range(port->jack_port, mode, &range);

			latency = SPA_LATENCY_INFO(s->direction,
					.min_rate = range.min,
					.max_rate = range.max);
			pw_log_debug("port latency %d %d %d", mode, range.min, range.max);

			if (spa_latency_info_compare(&latency, &port->latency[s->direction])) {
				port->latency[s->direction] = latency;
				port->latency_changed[s->direction] = update = true;
			}
		}
	} else if (mode == JackCaptureLatency) {
		for (i = 0; i < s->n_ports; i++) {
			port = s->ports[i];
			if (port == NULL || port->jack_port == NULL)
				continue;
			if (port->latency_changed[other]) {
				range.min = port->latency[other].min_rate;
				range.max = port->latency[other].max_rate;
				jack.port_set_latency_range(port->jack_port, mode, &range);
				port->latency_changed[other] = false;
			}
		}
	}
	return update;
}


static void jack_latency(jack_latency_callback_mode_t mode, void *arg)
{
	struct impl *impl = arg;
	bool update = false;

	if ((impl->mode & MODE_SINK))
		update |= stream_handle_latency(&impl->sink, mode);

	if ((impl->mode & MODE_SOURCE))
		update |= stream_handle_latency(&impl->source, mode);

	if (update)
		pw_loop_invoke(impl->main_loop, do_update_latency, 0, NULL, 0, false, impl);
}

static int create_jack_client(struct impl *impl)
{
	const char *server_name, *client_name;
	jack_options_t options = JackNullOption;
	jack_status_t status;

	server_name = pw_properties_get(impl->props, "jack.server");
	if (server_name != NULL)
		options |= JackServerName;

	client_name = pw_properties_get(impl->props, "jack.client-name");
	if (client_name == NULL)
		client_name = DEFAULT_CLIENT_NAME;

	impl->client = jack.client_open(client_name, options, &status, server_name);
	if (impl->client == NULL) {
		pw_log_error ("jack_client_open() failed 0x%2.0x\n", status);
		return -EIO;
	}
	jack.on_info_shutdown(impl->client, jack_info_shutdown, impl);
	jack.set_process_thread(impl->client, jack_process_thread, impl);
	jack.set_xrun_callback(impl->client, jack_xrun, impl);
	jack.set_latency_callback(impl->client, jack_latency, impl);

	impl->samplerate = jack.get_sample_rate(impl->client);
	impl->source.info.rate = impl->samplerate;
	impl->sink.info.rate = impl->samplerate;

	jack.activate(impl->client);

	return 0;
}

static void core_error(void *data, uint32_t id, int seq, int res, const char *message)
{
	struct impl *impl = data;

	pw_log_error("error id:%u seq:%d res:%d (%s): %s",
			id, seq, res, spa_strerror(res), message);

	if (id == PW_ID_CORE && res == -EPIPE)
		pw_impl_module_schedule_destroy(impl->module);
}

static const struct pw_core_events core_events = {
	PW_VERSION_CORE_EVENTS,
	.error = core_error,
};

static void core_destroy(void *d)
{
	struct impl *impl = d;
	spa_hook_remove(&impl->core_listener);
	impl->core = NULL;
	pw_impl_module_schedule_destroy(impl->module);
}

static const struct pw_proxy_events core_proxy_events = {
	.destroy = core_destroy,
};

static void impl_destroy(struct impl *impl)
{
	if (impl->client) {
		jack.deactivate(impl->client);
		jack.client_close(impl->client);
	}
	if (impl->source.filter)
		pw_filter_destroy(impl->source.filter);
	if (impl->sink.filter)
		pw_filter_destroy(impl->sink.filter);
	if (impl->core && impl->do_disconnect)
		pw_core_disconnect(impl->core);

	pw_properties_free(impl->sink.props);
	pw_properties_free(impl->source.props);
	pw_properties_free(impl->props);

	free(impl);
}

static void module_destroy(void *data)
{
	struct impl *impl = data;
	spa_hook_remove(&impl->module_listener);
	impl_destroy(impl);
}

static const struct pw_impl_module_events module_events = {
	PW_VERSION_IMPL_MODULE_EVENTS,
	.destroy = module_destroy,
};

static void parse_audio_info(const struct pw_properties *props, struct spa_audio_info_raw *info)
{
	spa_audio_info_raw_init_dict_keys(info,
			&SPA_DICT_ITEMS(
				 SPA_DICT_ITEM(SPA_KEY_AUDIO_FORMAT, "F32P"),
				 SPA_DICT_ITEM(SPA_KEY_AUDIO_POSITION, DEFAULT_POSITION)),
			&props->dict,
			SPA_KEY_AUDIO_CHANNELS,
			SPA_KEY_AUDIO_POSITION, NULL);
}

static void copy_props(struct impl *impl, struct pw_properties *props, const char *key)
{
	const char *str;
	if ((str = pw_properties_get(props, key)) != NULL) {
		if (pw_properties_get(impl->sink.props, key) == NULL)
			pw_properties_set(impl->sink.props, key, str);
		if (pw_properties_get(impl->source.props, key) == NULL)
			pw_properties_set(impl->source.props, key, str);
	}
}

SPA_EXPORT
int pipewire__module_init(struct pw_impl_module *module, const char *args)
{
	struct pw_context *context = pw_impl_module_get_context(module);
	struct pw_properties *props = NULL;
	struct impl *impl;
	const char *str;
	int res;

	PW_LOG_TOPIC_INIT(mod_topic);

	impl = calloc(1, sizeof(struct impl));
	if (impl == NULL)
		return -errno;

	pw_log_debug("module %p: new %s", impl, args);

	if (args == NULL)
		args = "";

	props = pw_properties_new_string(args);
	if (props == NULL) {
		res = -errno;
		pw_log_error( "can't create properties: %m");
		goto error;
	}
	impl->props = props;

	if ((str = pw_properties_get(props, "jack.library")) == NULL)
		str = "libjack.so.0";

	if ((res = weakjack_load(&jack, str)) < 0) {
		pw_log_error( "can't load '%s': %s", str, spa_strerror(res));
		goto error;
	}

	impl->sink.props = pw_properties_new(NULL, NULL);
	impl->source.props = pw_properties_new(NULL, NULL);
	if (impl->source.props == NULL || impl->sink.props == NULL) {
		res = -errno;
		pw_log_error( "can't create properties: %m");
		goto error;
	}

	impl->module = module;
	impl->context = context;
	impl->main_loop = pw_context_get_main_loop(context);
	impl->system = impl->main_loop->system;

	impl->rate_limit.interval = 2 * SPA_USEC_PER_SEC;
	impl->rate_limit.burst = 1;

	impl->source.impl = impl;
	impl->source.direction = PW_DIRECTION_OUTPUT;
	impl->sink.impl = impl;
	impl->sink.direction = PW_DIRECTION_INPUT;

	impl->mode = MODE_DUPLEX;
	if ((str = pw_properties_get(props, "tunnel.mode")) != NULL) {
		if (spa_streq(str, "source")) {
			impl->mode = MODE_SOURCE;
		} else if (spa_streq(str, "sink")) {
			impl->mode = MODE_SINK;
		} else if (spa_streq(str, "duplex")) {
			impl->mode = MODE_DUPLEX;
		} else {
			pw_log_error("invalid tunnel.mode '%s'", str);
			res = -EINVAL;
			goto error;
		}
	}

	if (pw_properties_get(props, PW_KEY_NODE_VIRTUAL) == NULL)
		pw_properties_set(props, PW_KEY_NODE_VIRTUAL, "true");
	if (pw_properties_get(props, PW_KEY_NODE_GROUP) == NULL)
		pw_properties_set(props, PW_KEY_NODE_GROUP, "jack-group");
	if (pw_properties_get(props, PW_KEY_NODE_ALWAYS_PROCESS) == NULL)
		pw_properties_set(props, PW_KEY_NODE_ALWAYS_PROCESS, "true");

	pw_properties_set(impl->sink.props, PW_KEY_MEDIA_CLASS, "Audio/Sink");
	pw_properties_set(impl->sink.props, PW_KEY_PRIORITY_DRIVER, "30001");
	pw_properties_set(impl->sink.props, PW_KEY_NODE_NAME, "jack_sink");
	pw_properties_set(impl->sink.props, PW_KEY_NODE_DESCRIPTION, "JACK Sink");

	pw_properties_set(impl->source.props, PW_KEY_MEDIA_CLASS, "Audio/Source");
	pw_properties_set(impl->source.props, PW_KEY_PRIORITY_DRIVER, "30000");
	pw_properties_set(impl->source.props, PW_KEY_NODE_NAME, "jack_source");
	pw_properties_set(impl->source.props, PW_KEY_NODE_DESCRIPTION, "JACK Source");

	if ((str = pw_properties_get(props, "sink.props")) != NULL)
		pw_properties_update_string(impl->sink.props, str, strlen(str));
	if ((str = pw_properties_get(props, "source.props")) != NULL)
		pw_properties_update_string(impl->source.props, str, strlen(str));

	copy_props(impl, props, PW_KEY_AUDIO_CHANNELS);
	copy_props(impl, props, SPA_KEY_AUDIO_POSITION);
	copy_props(impl, props, PW_KEY_NODE_ALWAYS_PROCESS);
	copy_props(impl, props, PW_KEY_NODE_GROUP);
	copy_props(impl, props, PW_KEY_NODE_VIRTUAL);
	copy_props(impl, props, "jack.connect");
	copy_props(impl, props, "jack.connect-audio");
	copy_props(impl, props, "jack.connect-midi");

	parse_audio_info(impl->source.props, &impl->source.info);
	parse_audio_info(impl->sink.props, &impl->sink.info);

	impl->source.n_midi = pw_properties_get_uint32(impl->source.props,
			"midi.ports", DEFAULT_MIDI_PORTS);
	impl->sink.n_midi = pw_properties_get_uint32(impl->sink.props,
			"midi.ports", DEFAULT_MIDI_PORTS);

	impl->source.n_ports = impl->source.n_midi + impl->source.info.channels;
	impl->sink.n_ports = impl->sink.n_midi + impl->sink.info.channels;
	if (impl->source.n_ports > MAX_PORTS || impl->sink.n_ports > MAX_PORTS) {
		pw_log_error("too many ports");
		res = -EINVAL;
		goto error;
	}

	impl->core = pw_context_get_object(impl->context, PW_TYPE_INTERFACE_Core);
	if (impl->core == NULL) {
		str = pw_properties_get(props, PW_KEY_REMOTE_NAME);
		impl->core = pw_context_connect(impl->context,
				pw_properties_new(
					PW_KEY_REMOTE_NAME, str,
					NULL),
				0);
		impl->do_disconnect = true;
	}
	if (impl->core == NULL) {
		res = -errno;
		pw_log_error("can't connect: %m");
		goto error;
	}

	pw_proxy_add_listener((struct pw_proxy*)impl->core,
			&impl->core_proxy_listener,
			&core_proxy_events, impl);
	pw_core_add_listener(impl->core,
			&impl->core_listener,
			&core_events, impl);

	if ((res = create_jack_client(impl)) < 0)
		goto error;

	if ((res = create_filters(impl)) < 0)
		goto error;

	pw_impl_module_add_listener(module, &impl->module_listener, &module_events, impl);

	pw_impl_module_update_properties(module, &SPA_DICT_INIT_ARRAY(module_props));

	return 0;

error:
	impl_destroy(impl);
	return res;
}
