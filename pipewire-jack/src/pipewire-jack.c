/* PipeWire */
/* SPDX-FileCopyrightText: Copyright © 2018 Wim Taymans */
/* SPDX-FileCopyrightText: Copyright © 2024 Nedko Arnaudov */
/* SPDX-License-Identifier: MIT */

#include "config.h"

#include <stdio.h>
#include <unistd.h>
#include <sys/mman.h>
#include <regex.h>
#include <math.h>
#include <threads.h>

#include <jack/jack.h>
#include <jack/intclient.h>
#include <jack/session.h>
#include <jack/thread.h>
#include <jack/midiport.h>
#include <jack/uuid.h>
#include <jack/metadata.h>

#include <spa/support/cpu.h>
#include <spa/param/audio/format-utils.h>
#include <spa/param/video/format-utils.h>
#include <spa/param/latency-utils.h>
#include <spa/debug/types.h>
#include <spa/debug/pod.h>
#include <spa/utils/json.h>
#include <spa/utils/result.h>
#include <spa/utils/string.h>
#include <spa/utils/ringbuffer.h>
#include <spa/control/ump-utils.h>

#include <pipewire/pipewire.h>
#include <pipewire/private.h>
#include <pipewire/thread.h>
#include <pipewire/data-loop.h>

#include "pipewire/extensions/client-node.h"
#include "pipewire/extensions/metadata.h"
#include "pipewire-jack-extensions.h"

/* use 512KB stack per thread - the default is way too high to be feasible
 * with mlockall() on many systems */
#define THREAD_STACK 524288

#define DEFAULT_RT_MAX	RTPRIO_CLIENT

#define JACK_CLIENT_NAME_SIZE		256
#define JACK_PORT_NAME_SIZE		256
#define JACK_PORT_TYPE_SIZE             32
#define MONITOR_EXT			" Monitor"

#define MAX_MIX				1024
#define MAX_CLIENT_PORTS		768

#define MAX_ALIGN			32
#define MAX_BUFFERS			2
#define MAX_BUFFER_DATAS		1u

#define REAL_JACK_PORT_NAME_SIZE (JACK_CLIENT_NAME_SIZE + JACK_PORT_NAME_SIZE)

PW_LOG_TOPIC_STATIC(jack_log_topic, "jack");
#define PW_LOG_TOPIC_DEFAULT jack_log_topic

#define TYPE_ID_AUDIO	0
#define TYPE_ID_VIDEO	1
#define TYPE_ID_MIDI	2
#define TYPE_ID_OSC	3
#define TYPE_ID_UMP	4
#define TYPE_ID_OTHER	5

#define TYPE_ID_IS_EVENT(t)	((t) >= TYPE_ID_MIDI && (t) <= TYPE_ID_UMP)
#define TYPE_ID_CAN_OSC(t)	((t) == TYPE_ID_MIDI || (t) == TYPE_ID_OSC)
#define TYPE_ID_IS_HIDDEN(t)	((t) >= TYPE_ID_OTHER)
#define TYPE_ID_IS_COMPATIBLE(a,b)(((a) == (b)) || (TYPE_ID_IS_EVENT(a) && TYPE_ID_IS_EVENT(b)))

#define SELF_CONNECT_ALLOW	0
#define SELF_CONNECT_FAIL_EXT	-1
#define SELF_CONNECT_IGNORE_EXT	1
#define SELF_CONNECT_FAIL_ALL	-2
#define SELF_CONNECT_IGNORE_ALL	2

#define OTHER_CONNECT_ALLOW	1
#define OTHER_CONNECT_FAIL	-1
#define OTHER_CONNECT_IGNORE	0

#define NOTIFY_BUFFER_SIZE	(1u<<13)
#define NOTIFY_BUFFER_MASK	(NOTIFY_BUFFER_SIZE-1)

struct notify {
#define NOTIFY_ACTIVE_FLAG		(1<<0)

#define NOTIFY_TYPE_NONE		((0<<4)|NOTIFY_ACTIVE_FLAG)
#define NOTIFY_TYPE_REGISTRATION	((1<<4))
#define NOTIFY_TYPE_PORTREGISTRATION	((2<<4)|NOTIFY_ACTIVE_FLAG)
#define NOTIFY_TYPE_CONNECT		((3<<4)|NOTIFY_ACTIVE_FLAG)
#define NOTIFY_TYPE_BUFFER_FRAMES	((4<<4)|NOTIFY_ACTIVE_FLAG)
#define NOTIFY_TYPE_SAMPLE_RATE		((5<<4)|NOTIFY_ACTIVE_FLAG)
#define NOTIFY_TYPE_FREEWHEEL		((6<<4)|NOTIFY_ACTIVE_FLAG)
#define NOTIFY_TYPE_SHUTDOWN		((7<<4)|NOTIFY_ACTIVE_FLAG)
#define NOTIFY_TYPE_LATENCY		((8<<4)|NOTIFY_ACTIVE_FLAG)
#define NOTIFY_TYPE_TOTAL_LATENCY	((9<<4)|NOTIFY_ACTIVE_FLAG)
	int type;
	struct object *object;
	int arg1;
	const char *msg;
};

struct client;
struct port;

struct globals {
	jack_thread_creator_t creator;
	pthread_mutex_t lock;
	struct pw_array descriptions;
	struct spa_list free_objects;
	struct spa_thread_utils *thread_utils;
	uint32_t max_frames;
};

static struct globals globals;
static bool mlock_warned = false;

#define MIDI_SCRATCH_FRAMES	8192
static thread_local float midi_scratch[MIDI_SCRATCH_FRAMES];


#define OBJECT_CHUNK		8
#define RECYCLE_THRESHOLD	128

typedef void (*mix_func) (float *dst, float *src[], uint32_t n_src, bool aligned, uint32_t n_samples);

struct object {
	struct spa_list link;

	struct client *client;

#define INTERFACE_Invalid	0
#define INTERFACE_Port		1
#define INTERFACE_Node		2
#define INTERFACE_Link		3
#define INTERFACE_Client	4
	uint32_t type;
	uint32_t id;
	uint32_t serial;

	union {
		struct {
			char name[1024];
			int32_t pid;
		} pwclient;
		struct {
			char name[JACK_CLIENT_NAME_SIZE+1];
			char node_name[512];
			int32_t priority;
			uint32_t client_id;
			unsigned is_jack:1;
			unsigned is_running:1;
		} node;
		struct {
			uint32_t src;
			uint32_t dst;
			uint32_t src_serial;
			uint32_t dst_serial;
			bool src_ours;
			bool dst_ours;
			struct port *our_input;
			struct port *our_output;
		} port_link;
		struct {
			unsigned long flags;
			char name[REAL_JACK_PORT_NAME_SIZE+1];
			char alias1[REAL_JACK_PORT_NAME_SIZE+1];
			char alias2[REAL_JACK_PORT_NAME_SIZE+1];
			char system[REAL_JACK_PORT_NAME_SIZE+1];
			uint32_t system_id;
			uint32_t type_id;
			uint32_t node_id;
			uint32_t monitor_requests;
			int32_t priority;
			struct port *port;
			bool is_monitor;
			struct object *node;
			struct spa_latency_info latency[2];
		} port;
	};
	struct pw_proxy *proxy;
	struct spa_hook proxy_listener;
	struct spa_hook object_listener;
	int registered;
	unsigned int visible;
	unsigned int removing:1;
	unsigned int removed:1;
	unsigned int to_free:1;
};

struct midi_buffer {
#define MIDI_BUFFER_MAGIC 0x900df00d
	uint32_t magic;
	int32_t buffer_size;
	uint32_t nframes;
	int32_t write_pos;
	uint32_t event_count;
	uint32_t lost_events;
};

#define MIDI_INLINE_MAX	4

struct midi_event {
	uint16_t time;
        uint16_t size;
        union {
		uint32_t byte_offset;
		uint8_t inline_data[MIDI_INLINE_MAX];
	};
};

struct buffer {
	struct spa_list link;
#define BUFFER_FLAG_OUT		(1<<0)
#define BUFFER_FLAG_MAPPED	(1<<1)
	uint32_t flags;
	uint32_t id;

	struct spa_data datas[MAX_BUFFER_DATAS];
	uint32_t n_datas;

	struct pw_memmap *mem[MAX_BUFFER_DATAS+1];
	uint32_t n_mem;
};

struct mix {
	struct spa_list link;
	struct spa_list port_link;
	uint32_t id;
	uint32_t peer_id;
	struct port *port;
	struct port *peer_port;

	struct spa_io_buffers *io[2];

	struct spa_list queue;
	struct buffer buffers[MAX_BUFFERS];
	uint32_t n_buffers;

	unsigned int to_free:1;
};

struct port {
	bool valid;
	struct spa_list link;

	struct client *client;

	enum spa_direction direction;
	uint32_t port_id;
	struct object *object;
	struct pw_properties *props;
	struct spa_port_info info;
#define IDX_EnumFormat	0
#define IDX_Buffers	1
#define IDX_IO		2
#define IDX_Format	3
#define IDX_Latency	4
#define N_PORT_PARAMS	5
	struct spa_param_info params[N_PORT_PARAMS];

	struct spa_io_buffers io[2];
	struct spa_list mix;
	uint32_t n_mix;
	struct mix *global_mix;

	struct port *tied;

	unsigned int empty_out:1;
	unsigned int zeroed:1;
	unsigned int to_free:1;

	void *(*get_buffer) (struct port *p, jack_nframes_t frames);

	float *emptyptr;
	float empty[];
};

struct link {
	struct spa_list link;
	struct spa_list target_link;
	struct client *client;
	uint32_t node_id;
	struct pw_memmap *mem;
	struct pw_node_activation *activation;
	int signalfd;
	void (*trigger) (struct link *l, uint64_t nsec);
};

struct context {
	struct pw_loop *l;
	struct pw_thread_loop *loop;	/* thread_lock protects all below */
	struct pw_context *context;
	struct pw_loop *nl;
	struct pw_thread_loop *notify;

	struct spa_thread_utils *old_thread_utils;
	struct spa_thread_utils thread_utils;
	pthread_mutex_t lock;		/* protects map and lists below, in addition to thread_lock */
	struct spa_list objects;
	uint32_t free_count;
};

#define GET_DIRECTION(f)	((f) & JackPortIsInput ? SPA_DIRECTION_INPUT : SPA_DIRECTION_OUTPUT)

#define GET_PORT(c,d,p)		(pw_map_lookup(&c->ports[d], p))

struct metadata {
	struct pw_metadata *proxy;
	struct spa_hook proxy_listener;
	struct spa_hook listener;

	char default_audio_sink[1024];
	char default_audio_source[1024];
};

struct frame_times {
	uint64_t frames;
	uint64_t nsec;
	uint64_t next_nsec;
	uint32_t buffer_frames;
	uint32_t sample_rate;
	double rate_diff;
};

struct client {
	char name[JACK_CLIENT_NAME_SIZE+1];

	struct context context;

	char *server_name;
	char *load_name;		/* load module name */
	char *load_init;		/* initialization string */
	jack_uuid_t session_id;		/* requested session_id */

	struct pw_loop *l;
	struct pw_data_loop *loop;
	struct pw_properties *props;

	struct pw_core *core;
	struct spa_hook core_listener;
	struct pw_mempool *pool;
	int pending_sync;
	int last_sync;
	int last_res;

	struct spa_node_info info;

	struct pw_registry *registry;
	struct spa_hook registry_listener;

	struct pw_client_node *node;
	struct spa_hook node_listener;
        struct spa_hook proxy_listener;

	struct metadata *metadata;
	struct metadata *settings;

	uint32_t node_id;
	uint32_t serial;
	struct object *object;

	struct spa_source *socket_source;
	struct spa_source *notify_source;
	void *notify_buffer;
	struct spa_ringbuffer notify_ring;

	JackThreadCallback thread_callback;
	void *thread_arg;
	JackThreadInitCallback thread_init_callback;
	void *thread_init_arg;
	JackShutdownCallback shutdown_callback;
	void *shutdown_arg;
	JackInfoShutdownCallback info_shutdown_callback;
	void *info_shutdown_arg;
	JackProcessCallback process_callback;
	void *process_arg;
	JackFreewheelCallback freewheel_callback;
	void *freewheel_arg;
	JackBufferSizeCallback bufsize_callback;
	void *bufsize_arg;
	JackSampleRateCallback srate_callback;
	void *srate_arg;
	JackClientRegistrationCallback registration_callback;
	void *registration_arg;
	JackPortRegistrationCallback portregistration_callback;
	void *portregistration_arg;
	JackPortConnectCallback connect_callback;
	void *connect_arg;
	JackPortRenameCallback rename_callback;
	void *rename_arg;
	JackGraphOrderCallback graph_callback;
	void *graph_arg;
	JackXRunCallback xrun_callback;
	void *xrun_arg;
	JackLatencyCallback latency_callback;
	void *latency_arg;
	JackSyncCallback sync_callback;
	void *sync_arg;
	JackTimebaseCallback timebase_callback;
	void *timebase_arg;
	JackPropertyChangeCallback property_callback;
	void *property_arg;

	struct spa_io_position *position;
	uint32_t sample_rate;
	uint32_t buffer_frames;
	struct spa_fraction latency;

	struct spa_list mix;
	struct spa_list free_mix;

	struct spa_list free_ports;
	struct pw_map ports[2];
	uint32_t n_ports;

	struct spa_list links;
	uint32_t driver_id;
	struct pw_node_activation *driver_activation;

	struct pw_memmap *mem;
	struct pw_node_activation *activation;
	uint32_t xrun_count;

	struct {
		struct spa_io_position *position;
		struct pw_node_activation *driver_activation;
		struct spa_list target_links;
		unsigned int prepared:1;
		unsigned int first:1;
		unsigned int thread_entered:1;
	} rt;

	pthread_mutex_t rt_lock;
	unsigned int rt_locked:1;
	unsigned int data_locked:1;

	unsigned int started:1;
	unsigned int active:1;
	unsigned int destroyed:1;
	unsigned int has_transport:1;
	unsigned int allow_mlock:1;
	unsigned int warn_mlock:1;
	unsigned int timeowner_conditional:1;
	unsigned int show_monitor:1;
	unsigned int show_midi:1;
	unsigned int merge_monitor:1;
	unsigned int short_name:1;
	unsigned int filter_name:1;
	unsigned int freewheeling:1;
	unsigned int locked_process:1;
	unsigned int default_as_system:1;
	int self_connect_mode;
	int other_connect_mode;
	int rt_max;
	unsigned int fix_midi_events:1;
	unsigned int global_buffer_size:1;
	unsigned int global_sample_rate:1;
	unsigned int passive_links:1;
	unsigned int pending_callbacks:1;
	int frozen_callbacks;
	char filter_char;
	uint32_t max_ports;
	unsigned int fill_aliases:1;
	unsigned int writable_input:1;
	unsigned int async:1;
	unsigned int flag_midi2:1;

	uint32_t max_frames;
	uint32_t max_align;
	mix_func mix_function;

	jack_position_t jack_position;
	jack_transport_state_t jack_state;
	struct frame_times jack_times;
};

#define return_val_if_fail(expr, val)				\
({								\
	if (SPA_UNLIKELY(!(expr))) {				\
		pw_log_warn("'%s' failed at %s:%u %s()",	\
			#expr , __FILE__, __LINE__, __func__);	\
		return (val);					\
	}							\
})

#define return_if_fail(expr)					\
({								\
	if (SPA_UNLIKELY(!(expr))) {				\
		pw_log_warn("'%s' failed at %s:%u %s()",	\
			#expr , __FILE__, __LINE__, __func__);	\
		return;						\
	}							\
})

static int do_sync(struct client *client);
static struct object *find_by_serial(struct client *c, uint32_t serial);

#include "metadata.c"

int pw_jack_match_rules(const char *rules, size_t size, const struct spa_dict *props,
		int (*matched) (void *data, const char *action, const char *val, int len),
		void *data);

static struct object * alloc_object(struct client *c, int type)
{
	struct object *o;
	int i;

	pthread_mutex_lock(&globals.lock);
	if (spa_list_is_empty(&globals.free_objects)) {
		o = calloc(OBJECT_CHUNK, sizeof(struct object));
		if (o == NULL) {
			pthread_mutex_unlock(&globals.lock);
			return NULL;
		}
		o[0].to_free = true;
		for (i = 0; i < OBJECT_CHUNK; i++)
			spa_list_append(&globals.free_objects, &o[i].link);
	}
	o = spa_list_first(&globals.free_objects, struct object, link);
	spa_list_remove(&o->link);
	pthread_mutex_unlock(&globals.lock);

	o->client = c;
	o->removed = false;
	o->type = type;
	pw_log_debug("%p: object:%p type:%d", c, o, type);

	return o;
}

static void recycle_objects(struct client *c, uint32_t remain)
{
	struct object *o, *t;
	pthread_mutex_lock(&globals.lock);
	spa_list_for_each_safe(o, t, &c->context.objects, link) {
		pw_log_debug("%p: recycle object:%p remived:%d type:%d id:%u/%u %u/%u",
				c, o, o->removed, o->type, o->id, o->serial,
				c->context.free_count, remain);
		if (o->removed) {
			spa_list_remove(&o->link);
			memset(o, 0, sizeof(struct object));
			spa_list_append(&globals.free_objects, &o->link);
			if (--c->context.free_count == remain)
				break;
		}
	}
	pthread_mutex_unlock(&globals.lock);
}

/* JACK clients expect the objects to hang around after
 * they are unregistered and freed. We mark the object removed and
 * move it to the end of the queue. */
static void free_object(struct client *c, struct object *o)
{
	pw_log_debug("%p: object:%p type:%d %u/%u", c, o, o->type,
			c->context.free_count, RECYCLE_THRESHOLD);
	pthread_mutex_lock(&c->context.lock);
	spa_list_remove(&o->link);
	o->removed = true;
	o->id = SPA_ID_INVALID;
	spa_list_append(&c->context.objects, &o->link);
	if (++c->context.free_count >= RECYCLE_THRESHOLD)
		recycle_objects(c, RECYCLE_THRESHOLD / 2);
	pthread_mutex_unlock(&c->context.lock);

}

static inline struct object *port_to_object(const jack_port_t *port)
{
	return (struct object*)port;
}
static inline jack_port_t *object_to_port(struct object *o)
{
	return (jack_port_t*)o;
}

struct io_info {
	struct mix *mix;
	void *data;
	size_t size;
};

static int
do_mix_set_io(struct spa_loop *loop, bool async, uint32_t seq,
		const void *data, size_t size, void *user_data)
{
	const struct io_info *info = data;
	struct port *port = info->mix->port;
	if (info->data) {
		if (info->size >= sizeof(struct spa_io_async_buffers)) {
			struct spa_io_async_buffers *ab = info->data;
			info->mix->io[0] = &ab->buffers[port->direction];
			info->mix->io[1] = &ab->buffers[port->direction^1];
		} else if (info->size >= sizeof(struct spa_io_buffers)) {
			info->mix->io[0] = info->data;
			info->mix->io[1] = info->data;
		} else {
			info->mix->io[0] = NULL;
			info->mix->io[1] = NULL;
		}
		if (port->n_mix++ == 0 && port->global_mix != NULL) {
			port->global_mix->io[0] = &port->io[0];
			port->global_mix->io[1] = &port->io[1];
		}
	} else {
		info->mix->io[0] = NULL;
		info->mix->io[1] = NULL;
		if (port->n_mix > 0 && --port->n_mix == 0 && port->global_mix != NULL) {
			port->global_mix->io[0] = NULL;
			port->global_mix->io[1] = NULL;
		}
	}
	return 0;
}

static inline void mix_set_io(struct mix *mix, void *data, size_t size)
{
	struct io_info info = { .mix = mix, .data = data, .size = size };
	pw_data_loop_invoke(mix->port->client->loop,
		do_mix_set_io, SPA_ID_INVALID, &info, sizeof(info), false, NULL);
}

static void init_mix(struct mix *mix, uint32_t mix_id, struct port *port, uint32_t peer_id)
{
	pw_log_debug("create %p mix:%d peer:%d", port, mix_id, peer_id);
	mix->id = mix_id;
	mix->peer_id = peer_id;
	mix->port = port;
	mix->peer_port = NULL;
	mix->io[0] = mix->io[1] = NULL;
	mix->n_buffers = 0;
	spa_list_init(&mix->queue);
	if (mix_id == SPA_ID_INVALID) {
		port->global_mix = mix;
		if (port->n_mix > 0)
			mix_set_io(port->global_mix, &port->io, sizeof(port->io));
	}
}
static struct mix *find_mix_peer(struct client *c, uint32_t peer_id)
{
	struct mix *mix;
	spa_list_for_each(mix, &c->mix, link) {
		if (mix->peer_id == peer_id)
			return mix;
	}
	return NULL;
}

static struct mix *find_port_peer(struct port *port, uint32_t peer_id)
{
	struct mix *mix;
	spa_list_for_each(mix, &port->mix, port_link) {
		pw_log_trace("%p %d %d", port, mix->peer_id, peer_id);
		if (mix->peer_id == peer_id)
			return mix;
	}
	return NULL;
}

static struct mix *find_mix(struct client *c, struct port *port, uint32_t mix_id)
{
	struct mix *mix;

	spa_list_for_each(mix, &port->mix, port_link) {
		if (mix->id == mix_id)
			return mix;
	}
	return NULL;
}

static struct mix *create_mix(struct client *c, struct port *port,
		uint32_t mix_id, uint32_t peer_id)
{
	struct mix *mix;
	uint32_t i;

	if (spa_list_is_empty(&c->free_mix)) {
		mix = calloc(OBJECT_CHUNK, sizeof(struct mix));
		if (mix == NULL)
			return NULL;
		mix[0].to_free = true;
		for (i = 0; i < OBJECT_CHUNK; i++)
			spa_list_append(&c->free_mix, &mix[i].link);
	}
	mix = spa_list_first(&c->free_mix, struct mix, link);
	spa_list_remove(&mix->link);
	spa_list_append(&c->mix, &mix->link);

	spa_list_append(&port->mix, &mix->port_link);

	init_mix(mix, mix_id, port, peer_id);

	return mix;
}

static int clear_buffers(struct client *c, struct mix *mix)
{
	struct port *port = mix->port;
	struct buffer *b;
	uint32_t i, j;

	pw_log_debug("%p: port %p clear buffers", c, port);

	for (i = 0; i < mix->n_buffers; i++) {
		b = &mix->buffers[i];

		for (j = 0; j < b->n_mem; j++)
			pw_memmap_free(b->mem[j]);

		b->n_mem = 0;
	}
	mix->n_buffers = 0;
	spa_list_init(&mix->queue);
	return 0;
}

static void free_mix(struct client *c, struct mix *mix)
{
	struct port *port = mix->port;

	clear_buffers(c, mix);
	spa_list_remove(&mix->port_link);
	if (mix->id == SPA_ID_INVALID)
		port->global_mix = NULL;
	spa_list_remove(&mix->link);
	spa_list_append(&c->free_mix, &mix->link);
}

static struct port * alloc_port(struct client *c, enum spa_direction direction)
{
	struct port *p;
	struct object *o;
	uint32_t i, port_size;

	if (c->n_ports >= c->max_ports) {
		errno = ENOSPC;
		return NULL;
	}

	if (spa_list_is_empty(&c->free_ports)) {
		port_size = sizeof(struct port) + (c->max_frames * sizeof(float)) + c->max_align;

		p = calloc(OBJECT_CHUNK, port_size);
		if (p == NULL)
			return NULL;
		p[0].to_free = true;
		for (i = 0; i < OBJECT_CHUNK; i++) {
			struct port *t = SPA_PTROFF(p, port_size * i, struct port);
			spa_list_append(&c->free_ports, &t->link);
		}
	}
	p = spa_list_first(&c->free_ports, struct port, link);
	spa_list_remove(&p->link);

	o = alloc_object(c, INTERFACE_Port);
	if (o == NULL)
		return NULL;

	o->id = SPA_ID_INVALID;
	o->port.node_id = c->node_id;
	o->port.port = p;
	o->port.latency[SPA_DIRECTION_INPUT] = SPA_LATENCY_INFO(SPA_DIRECTION_INPUT);
	o->port.latency[SPA_DIRECTION_OUTPUT] = SPA_LATENCY_INFO(SPA_DIRECTION_OUTPUT);

	p->valid = true;
	p->zeroed = false;
	p->client = c;
	p->object = o;
	spa_list_init(&p->mix);
	p->props = pw_properties_new(NULL, NULL);

	p->direction = direction;
	p->emptyptr = SPA_PTR_ALIGN(p->empty, c->max_align, float);
	p->port_id = pw_map_insert_new(&c->ports[direction], p);
	c->n_ports++;

	pthread_mutex_lock(&c->context.lock);
	spa_list_append(&c->context.objects, &o->link);
	pthread_mutex_unlock(&c->context.lock);

	return p;
}

static void free_port(struct client *c, struct port *p, bool free)
{
	struct mix *m;

	spa_list_consume(m, &p->mix, port_link)
		free_mix(c, m);

	c->n_ports--;
	pw_map_remove(&c->ports[p->direction], p->port_id);
	pw_properties_free(p->props);
	spa_list_append(&c->free_ports, &p->link);
	if (free)
		free_object(c, p->object);
	else
		p->object->removing = true;
}

static struct object *find_node(struct client *c, const char *name)
{
	struct object *o;

	spa_list_for_each(o, &c->context.objects, link) {
		if (o->removing || o->removed || o->type != INTERFACE_Node)
			continue;
		if (spa_streq(o->node.name, name))
			return o;
	}
	return NULL;
}

static bool is_port_default(struct client *c, struct object *o)
{
	struct object *ot;

	if (c->metadata == NULL)
		return false;

	if ((ot = o->port.node) != NULL &&
	    (spa_streq(ot->node.node_name, c->metadata->default_audio_source) ||
	     spa_streq(ot->node.node_name, c->metadata->default_audio_sink)))
		return true;

	return false;
}

static inline bool client_port_visible(struct client *c, struct object *o)
{
	if (o->port.port != NULL && o->port.port->client == c)
		return true;
	return o->visible;
}

static struct object *find_port_by_name(struct client *c, const char *name)
{
	struct object *o;

	spa_list_for_each(o, &c->context.objects, link) {
		if (o->type != INTERFACE_Port || o->removed ||
		    (!client_port_visible(c, o)))
			continue;
		if (spa_streq(o->port.name, name) ||
		    spa_streq(o->port.alias1, name) ||
		    spa_streq(o->port.alias2, name))
			return o;
		if (is_port_default(c, o) && spa_streq(o->port.system, name))
			return o;
	}
	return NULL;
}

static struct object *find_by_id(struct client *c, uint32_t id)
{
	struct object *o;
	spa_list_for_each(o, &c->context.objects, link) {
		if (o->id == id)
			return o;
	}
	return NULL;
}

static struct object *find_by_serial(struct client *c, uint32_t serial)
{
	struct object *o;
	spa_list_for_each(o, &c->context.objects, link) {
		if (o->serial == serial)
			return o;
	}
	return NULL;
}

static struct object *find_id(struct client *c, uint32_t id, bool valid)
{
	struct object *o = find_by_id(c, id);
	if (o != NULL && (!valid || o->client == c))
		return o;
	return NULL;
}

static struct object *find_type(struct client *c, uint32_t id, uint32_t type, bool valid)
{
	struct object *o = find_id(c, id, valid);
	if (o != NULL && o->type == type)
		return o;
	return NULL;
}

static struct object *find_client(struct client *c, uint32_t client_id)
{
	return find_type(c, client_id, INTERFACE_Client, false);
}

static struct object *find_link(struct client *c, uint32_t src, uint32_t dst)
{
	struct object *l;

	spa_list_for_each(l, &c->context.objects, link) {
		if (l->type != INTERFACE_Link || l->removed)
			continue;
		if (l->port_link.src == src &&
		    l->port_link.dst == dst) {
			return l;
		}
	}
	return NULL;
}

#if defined (__SSE__)
#include <xmmintrin.h>
static void mix_sse(float *dst, float *src[], uint32_t n_src, bool aligned, uint32_t n_samples)
{
	uint32_t i, n, unrolled;
	__m128 in[1];

	if (SPA_IS_ALIGNED(dst, 16) && aligned)
		unrolled = n_samples & ~3;
	else
		unrolled = 0;

	for (n = 0; n < unrolled; n += 4) {
		in[0] = _mm_load_ps(&src[0][n]);
		for (i = 1; i < n_src; i++)
			in[0] = _mm_add_ps(in[0], _mm_load_ps(&src[i][n]));
		_mm_store_ps(&dst[n], in[0]);
	}
	for (; n < n_samples; n++) {
		in[0] = _mm_load_ss(&src[0][n]);
		for (i = 1; i < n_src; i++)
			in[0] = _mm_add_ss(in[0], _mm_load_ss(&src[i][n]));
		_mm_store_ss(&dst[n], in[0]);
	}
}
#endif

static void mix_c(float *dst, float *src[], uint32_t n_src, bool aligned, uint32_t n_samples)
{
	uint32_t n, i;
	for (n = 0; n < n_samples; n++)  {
		float t = src[0][n];
		for (i = 1; i < n_src; i++)
			t += src[i][n];
		dst[n] = t;
	}
}

SPA_EXPORT
void jack_get_version(int *major_ptr, int *minor_ptr, int *micro_ptr, int *proto_ptr)
{
	if (major_ptr)
		*major_ptr = 3;
	if (minor_ptr)
		*minor_ptr = PW_MAJOR;
	if (micro_ptr)
		*micro_ptr = PW_MINOR;
	if (proto_ptr)
		*proto_ptr = PW_MICRO;
}

#define do_callback_expr(c,expr,callback,do_emit,...)		\
({								\
	if (c->callback && do_emit) {				\
		pw_thread_loop_unlock(c->context.loop);		\
		if (c->locked_process)				\
			pthread_mutex_lock(&c->rt_lock);	\
		(expr);						\
		pw_log_debug("emit " #callback);		\
		c->callback(__VA_ARGS__);			\
		if (c->locked_process)				\
			pthread_mutex_unlock(&c->rt_lock);	\
		pw_thread_loop_lock(c->context.loop);		\
	} else {						\
		(expr);						\
		pw_log_debug("skip " #callback			\
			" cb:%p do_emit:%d", c->callback,	\
			do_emit);				\
	}							\
})

#define do_callback(c,callback,do_emit,...) do_callback_expr(c,(void)0,callback,do_emit,__VA_ARGS__)

#define do_rt_callback_res(c,callback,...)			\
({								\
	int res = 0;						\
	if (c->callback) {					\
		if (pthread_mutex_trylock(&c->rt_lock) == 0) {	\
			c->rt_locked = true;			\
			res = c->callback(__VA_ARGS__);		\
			c->rt_locked = false;			\
			pthread_mutex_unlock(&c->rt_lock);	\
		} else {					\
			pw_log_debug("skip " #callback		\
				" cb:%p", c->callback);		\
		}						\
	}							\
	res;							\
})

SPA_EXPORT
const char *
jack_get_version_string(void)
{
	static char name[1024];
	int major, minor, micro, proto;
	jack_get_version(&major, &minor, &micro, &proto);
	snprintf(name, sizeof(name), "%d.%d.%d.%d (using PipeWire %s)",
			major, minor, micro, proto, pw_get_library_version());
	return name;
}

#define freeze_callbacks(c)		\
({					\
	(c)->frozen_callbacks++;	\
 })

#define check_callbacks(c)							\
({										\
	if ((c)->frozen_callbacks == 0 && (c)->pending_callbacks)		\
		pw_loop_signal_event((c)->context.nl, (c)->notify_source);	\
 })
#define thaw_callbacks(c)							\
({										\
	(c)->frozen_callbacks--;						\
	check_callbacks(c);							\
 })

static void on_notify_event(void *data, uint64_t count)
{
	struct client *c = data;
	struct object *o;
	int32_t avail;
	uint32_t index;
	struct notify *notify;
	bool do_graph = false, do_recompute_capture = false, do_recompute_playback = false;

	pw_thread_loop_lock(c->context.loop);
	if (c->frozen_callbacks != 0 || !c->pending_callbacks)
		goto done;

	pw_log_debug("%p: enter active:%u", c, c->active);

	c->pending_callbacks = false;

	freeze_callbacks(c);

	avail = spa_ringbuffer_get_read_index(&c->notify_ring, &index);
	while (avail > 0) {
		notify = SPA_PTROFF(c->notify_buffer, index & NOTIFY_BUFFER_MASK, struct notify);

		o = notify->object;
		pw_log_debug("%p: dequeue notify index:%08x %p type:%d %p arg1:%d", c,
				index, notify, notify->type, o, notify->arg1);

		switch (notify->type) {
		case NOTIFY_TYPE_REGISTRATION:
			if (o->registered == notify->arg1)
				break;
			pw_log_debug("%p: node %u %s %u", c, o->serial,
					o->node.name, notify->arg1);
			do_callback(c, registration_callback, true,
					o->node.name,
					notify->arg1,
					c->registration_arg);
			break;
		case NOTIFY_TYPE_PORTREGISTRATION:
			if (o->registered == notify->arg1)
				break;
			pw_log_debug("%p: port %u %s %u", c, o->serial,
					o->port.name, notify->arg1);
			do_callback(c, portregistration_callback, c->active,
					o->serial,
					notify->arg1,
					c->portregistration_arg);
			break;
		case NOTIFY_TYPE_CONNECT:
			if (o->registered == notify->arg1)
				break;
			pw_log_debug("%p: link %u %u -> %u %u", c, o->serial,
					o->port_link.src_serial,
					o->port_link.dst, notify->arg1);
			do_callback(c, connect_callback, c->active,
					o->port_link.src_serial,
					o->port_link.dst_serial,
					notify->arg1,
					c->connect_arg);

			do_graph = true;
			do_recompute_capture = do_recompute_playback = true;
			break;
		case NOTIFY_TYPE_BUFFER_FRAMES:
			pw_log_debug("%p: buffer frames %d -> %d", c, c->buffer_frames, notify->arg1);
			if (c->buffer_frames != (uint32_t)notify->arg1) {
				do_callback_expr(c, c->buffer_frames = notify->arg1,
						bufsize_callback, c->active,
						notify->arg1, c->bufsize_arg);
				do_recompute_capture = do_recompute_playback = true;
			}
			break;
		case NOTIFY_TYPE_SAMPLE_RATE:
			pw_log_debug("%p: sample rate %d -> %d", c, c->sample_rate, notify->arg1);
			if (c->sample_rate != (uint32_t)notify->arg1) {
				do_callback_expr(c, c->sample_rate = notify->arg1,
						srate_callback, c->active,
						notify->arg1, c->srate_arg);
			}
			break;
		case NOTIFY_TYPE_FREEWHEEL:
			pw_log_debug("%p: freewheel %d", c, notify->arg1);
			do_callback(c, freewheel_callback, c->active,
					notify->arg1, c->freewheel_arg);
			break;
		case NOTIFY_TYPE_SHUTDOWN:
			pw_log_debug("%p: shutdown %d %s", c, notify->arg1, notify->msg);
			if (c->info_shutdown_callback)
				do_callback(c, info_shutdown_callback, c->active,
						notify->arg1, notify->msg,
						c->info_shutdown_arg);
			else
				do_callback(c, shutdown_callback, c->active, c->shutdown_arg);
			break;
		case NOTIFY_TYPE_LATENCY:
			pw_log_debug("%p: latency %d", c, notify->arg1);
			if (notify->arg1 == JackCaptureLatency)
				do_recompute_capture = true;
			else if (notify->arg1 == JackPlaybackLatency)
				do_recompute_playback = true;
			break;
		case NOTIFY_TYPE_TOTAL_LATENCY:
			pw_log_debug("%p: total latency", c);
			do_recompute_capture = do_recompute_playback = true;
			break;
		default:
			break;
		}
		if (o != NULL) {
			o->registered = notify->arg1;
			if (notify->arg1 == 0 && o->removing) {
				o->removing = false;
				free_object(c, o);
			}
		}
		avail -= sizeof(struct notify);
		index += sizeof(struct notify);
		spa_ringbuffer_read_update(&c->notify_ring, index);
	}
	if (do_recompute_capture)
		do_callback(c, latency_callback, c->active, JackCaptureLatency, c->latency_arg);
	if (do_recompute_playback)
		do_callback(c, latency_callback, c->active, JackPlaybackLatency, c->latency_arg);
	if (do_graph)
		do_callback(c, graph_callback, c->active, c->graph_arg);

	thaw_callbacks(c);
done:
	pw_log_debug("%p: leave", c);
	pw_thread_loop_unlock(c->context.loop);
}

static int queue_notify(struct client *c, int type, struct object *o, int arg1, const char *msg)
{
	int32_t filled;
	uint32_t index;
	struct notify *notify;
	bool emit = false;
	int res = 0;

	switch (type) {
	case NOTIFY_TYPE_REGISTRATION:
		emit = c->registration_callback != NULL && o != NULL;
		break;
	case NOTIFY_TYPE_PORTREGISTRATION:
		emit = c->portregistration_callback != NULL && o != NULL;
		o->visible = arg1;
		break;
	case NOTIFY_TYPE_CONNECT:
		emit = c->connect_callback != NULL && o != NULL;
		break;
	case NOTIFY_TYPE_BUFFER_FRAMES:
		emit = c->bufsize_callback != NULL;
		break;
	case NOTIFY_TYPE_SAMPLE_RATE:
		emit = c->srate_callback != NULL;
		break;
	case NOTIFY_TYPE_FREEWHEEL:
		emit = c->freewheel_callback != NULL;
		break;
	case NOTIFY_TYPE_SHUTDOWN:
		emit = c->info_shutdown_callback != NULL || c->shutdown_callback != NULL;
		break;
	case NOTIFY_TYPE_LATENCY:
	case NOTIFY_TYPE_TOTAL_LATENCY:
		emit = c->latency_callback != NULL;
		break;
	default:
		break;
	}
	if (!emit || ((type & NOTIFY_ACTIVE_FLAG) && !c->active)) {
		switch (type) {
		case NOTIFY_TYPE_BUFFER_FRAMES:
			if (!emit) {
				c->buffer_frames = arg1;
				queue_notify(c, NOTIFY_TYPE_TOTAL_LATENCY, NULL, 0, NULL);
			}
			break;
		case NOTIFY_TYPE_SAMPLE_RATE:
			if (!emit)
				c->sample_rate = arg1;
			break;
		}
		pw_log_debug("%p: skip notify %08x active:%d emit:%d", c, type,
				c->active, emit);
		if (o != NULL) {
			o->registered = arg1;
			if (arg1 == 0 && o->removing) {
				o->removing = false;
				free_object(c, o);
			}
		}
		return res;
	}

	pthread_mutex_lock(&c->context.lock);
	filled = spa_ringbuffer_get_write_index(&c->notify_ring, &index);
	if (filled < 0 || filled + sizeof(struct notify) > NOTIFY_BUFFER_SIZE) {
		pw_log_warn("%p: notify queue full %d", c, type);
		res = -ENOSPC;
		goto done;
	}

	notify = SPA_PTROFF(c->notify_buffer, index & NOTIFY_BUFFER_MASK, struct notify);
	notify->type = type;
	notify->object = o;
	notify->arg1 = arg1;
	notify->msg = msg;
	pw_log_debug("%p: queue notify index:%08x %p type:%d %p arg1:%d msg:%s", c,
				index, notify, notify->type, o, notify->arg1, notify->msg);
	index += sizeof(struct notify);
	spa_ringbuffer_write_update(&c->notify_ring, index);
	c->pending_callbacks = true;
	check_callbacks(c);
done:
	pthread_mutex_unlock(&c->context.lock);
	return res;
}

static void on_sync_reply(void *data, uint32_t id, int seq)
{
	struct client *client = data;
	if (id != PW_ID_CORE)
		return;
	client->last_sync = seq;
	if (client->pending_sync == seq)
		pw_thread_loop_signal(client->context.loop, false);
}

static void on_error(void *data, uint32_t id, int seq, int res, const char *message)
{
	struct client *client = data;

	pw_log_warn("%p: error id:%u seq:%d res:%d (%s): %s", client,
			id, seq, res, spa_strerror(res), message);

	if (id == PW_ID_CORE) {
		/* This happens when we did something on a proxy that
		 * was destroyed on the server already */
		if (res == -ENOENT)
			return;

		client->last_res = res;
		if (res == -EPIPE && !client->destroyed) {
			queue_notify(client, NOTIFY_TYPE_SHUTDOWN,
					NULL, JackFailure | JackServerError,
					"JACK server has been closed");
		}
	}
	pw_thread_loop_signal(client->context.loop, false);
}

static const struct pw_core_events core_events = {
	PW_VERSION_CORE_EVENTS,
	.done = on_sync_reply,
	.error = on_error,
};

static int do_sync(struct client *client)
{
	bool in_data_thread = pw_data_loop_in_thread(client->loop);

	if (pw_thread_loop_in_thread(client->context.loop)) {
		pw_log_warn("sync requested from callback");
		return 0;
	}
	if (client->last_res == -EPIPE)
		return -EPIPE;

	client->last_res = 0;
	client->pending_sync = pw_proxy_sync((struct pw_proxy*)client->core, client->pending_sync);
	if (client->pending_sync < 0)
		return client->pending_sync;

	while (true) {
		if (in_data_thread) {
			if (client->rt_locked)
				pthread_mutex_unlock(&client->rt_lock);
			client->data_locked = true;
		}
	        pw_thread_loop_wait(client->context.loop);

		if (in_data_thread) {
			client->data_locked = false;
			if (client->rt_locked)
				pthread_mutex_lock(&client->rt_lock);
		}

		if (client->last_res < 0)
			return client->last_res;

		if (client->pending_sync == client->last_sync)
			break;
	}
	return 0;
}

static void on_node_removed(void *data)
{
	struct client *client = data;
	pw_proxy_destroy((struct pw_proxy*)client->node);
}

static void on_node_destroy(void *data)
{
	struct client *client = data;
	client->node = NULL;
	spa_hook_remove(&client->proxy_listener);
	spa_hook_remove(&client->node_listener);
}

static void on_node_bound_props(void *data, uint32_t global_id, const struct spa_dict *props)
{
	struct client *client = data;
	client->node_id = global_id;
	if (props)
		pw_properties_update(client->props, props);
}

static const struct pw_proxy_events node_proxy_events = {
	PW_VERSION_PROXY_EVENTS,
	.removed = on_node_removed,
	.destroy = on_node_destroy,
	.bound_props = on_node_bound_props,
};

static struct link *find_activation(struct spa_list *links, uint32_t node_id)
{
	struct link *l;

	spa_list_for_each(l, links, link) {
		if (l->node_id == node_id)
			return l;
	}
	return NULL;
}

static void client_remove_source(struct client *c)
{
	if (c->socket_source) {
		pw_loop_destroy_source(c->l, c->socket_source);
		c->socket_source = NULL;
	}
}

static inline void queue_buffer(struct client *c, struct mix *mix, uint32_t id)
{
	struct buffer *b;

	b = &mix->buffers[id];

	if (SPA_FLAG_IS_SET(b->flags, BUFFER_FLAG_OUT)) {
		pw_log_trace_fp("%p: port %p: recycle buffer %d", c, mix->port, id);
		spa_list_append(&mix->queue, &b->link);
		SPA_FLAG_CLEAR(b->flags, BUFFER_FLAG_OUT);
	}
}

static inline struct buffer *dequeue_buffer(struct client *c, struct mix *mix)
{
	struct buffer *b;

	if (SPA_UNLIKELY(spa_list_is_empty(&mix->queue)))
		return NULL;

	b = spa_list_first(&mix->queue, struct buffer, link);
	spa_list_remove(&b->link);
	SPA_FLAG_SET(b->flags, BUFFER_FLAG_OUT);
	pw_log_trace_fp("%p: port %p: dequeue buffer %d", c, mix->port, b->id);

	return b;
}

static inline bool is_osc(jack_midi_event_t *ev)
{
	return ev->size >= 1 && (ev->buffer[0] == '#' || ev->buffer[0] == '/');
}

static size_t convert_from_event(void *midi, void *buffer, size_t size, uint32_t type)
{
	struct spa_pod_builder b = { 0, };
	uint32_t i, count;
	struct spa_pod_frame f;
	uint32_t event_type;

	switch (type) {
	case TYPE_ID_MIDI:
	case TYPE_ID_OSC:
		/* we handle MIDI as OSC, check below */
		event_type = SPA_CONTROL_OSC;
		break;
	case TYPE_ID_UMP:
		event_type = SPA_CONTROL_UMP;
		break;
	default:
		return 0;
	}
	count = jack_midi_get_event_count(midi);

	spa_pod_builder_init(&b, buffer, size);
	spa_pod_builder_push_sequence(&b, &f, 0);

	for (i = 0; i < count; i++) {
		jack_midi_event_t ev;
		jack_midi_event_get(&ev, midi, i);

		if (type != TYPE_ID_MIDI || is_osc(&ev)) {
			/* no midi port or it's OSC */
			spa_pod_builder_control(&b, ev.time, event_type);
			spa_pod_builder_bytes(&b, ev.buffer, ev.size);
		} else {
			/* midi port and it's not OSC, convert to UMP */
			uint8_t *data = ev.buffer;
			size_t size = ev.size;
			uint64_t state = 0;

			while (size > 0) {
				uint32_t ump[4];
				int ump_size = spa_ump_from_midi(&data, &size,
						ump, sizeof(ump), 0, &state);
				if (ump_size <= 0)
					break;
				spa_pod_builder_control(&b, ev.time, SPA_CONTROL_UMP);
				spa_pod_builder_bytes(&b, ump, ump_size);
			}
		}
	}
	spa_pod_builder_pop(&b, &f);
	return b.state.offset;
}

static inline int event_compare(uint8_t s1, uint8_t s2)
{
	/* 11 (controller) > 12 (program change) >
	 * 8 (note off) > 9 (note on) > 10 (aftertouch) >
	 * 13 (channel pressure) > 14 (pitch bend) */
	static int priotab[] = { 5,4,3,7,6,2,1,0 };
	if ((s1 & 0xf) != (s2 & 0xf))
		return 0;
	return priotab[(s2>>4) & 7] - priotab[(s1>>4) & 7];
}

static inline int event_sort(struct spa_pod_control *a, struct spa_pod_control *b)
{
	if (a->offset < b->offset)
		return -1;
	if (a->offset > b->offset)
		return 1;
	if (a->type != b->type)
		return 0;
	switch(a->type) {
	case SPA_CONTROL_Midi:
	{
		uint8_t *sa = SPA_POD_BODY(&a->value), *sb = SPA_POD_BODY(&b->value);
		if (SPA_POD_BODY_SIZE(&a->value) < 1 || SPA_POD_BODY_SIZE(&b->value) < 1)
			return 0;
		return event_compare(sa[0], sb[0]);
	}
	case SPA_CONTROL_UMP:
	{
		uint32_t *sa = SPA_POD_BODY(&a->value), *sb = SPA_POD_BODY(&b->value);
		if (SPA_POD_BODY_SIZE(&a->value) < 4 || SPA_POD_BODY_SIZE(&b->value) < 4)
			return 0;
		if ((sa[0] >> 28) != 2 || (sa[0] >> 28) != 4 ||
		    (sb[0] >> 28) != 2 || (sb[0] >> 28) != 4)
			return 0;
		return event_compare(sa[0] >> 16, sb[0] >> 16);
	}
	default:
		return 0;
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

static inline jack_midi_data_t* midi_event_reserve(void *port_buffer,
                        jack_nframes_t  time, size_t data_size)
{
	struct midi_buffer *mb = port_buffer;
	uint8_t *res = NULL;

	/* Check if data_size is >0 and there is enough space in the buffer for the event. */
	if (SPA_UNLIKELY(data_size <= 0)) {
		pw_log_warn("midi %p: data_size:%zd", port_buffer, data_size);
	} else if (SPA_UNLIKELY(jack_midi_max_event_size (port_buffer) < data_size)) {
		pw_log_warn("midi %p: event too large: data_size:%zd", port_buffer, data_size);
	} else {
		struct midi_event *events = SPA_PTROFF(mb, sizeof(*mb), struct midi_event);
		struct midi_event *ev = &events[mb->event_count];

		ev->time = time;
		ev->size = data_size;
		if (SPA_LIKELY(data_size <= MIDI_INLINE_MAX)) {
			res = ev->inline_data;
		} else {
			mb->write_pos += data_size;
			ev->byte_offset = mb->buffer_size - 1 - mb->write_pos;
			res = SPA_PTROFF(mb, ev->byte_offset, uint8_t);
		}
		mb->event_count += 1;
	}
	return res;
}

static inline int midi_event_append(void *port_buffer, const jack_midi_data_t *data, size_t data_size)
{
	struct midi_buffer *mb = port_buffer;
	struct midi_event *events = SPA_PTROFF(mb, sizeof(*mb), struct midi_event);
	struct midi_event *ev;
	size_t old_size;
	uint8_t *old, *buf;

	ev = &events[--mb->event_count];
	mb->write_pos -= ev->size;
	old_size = ev->size;
	if (old_size <= MIDI_INLINE_MAX)
		old = ev->inline_data;
	else
		old = SPA_PTROFF(mb, ev->byte_offset, uint8_t);
	buf = midi_event_reserve(port_buffer, ev->time, old_size + data_size);
	if (SPA_UNLIKELY(buf == NULL))
		return -ENOBUFS;
	memmove(buf, old, old_size);
	memcpy(buf+old_size, data, data_size);
	return 0;
}

static inline int midi_event_write(void *port_buffer,
                      jack_nframes_t time,
                      const jack_midi_data_t *data,
                      size_t data_size, bool fix)
{
	jack_midi_data_t *retbuf = midi_event_reserve (port_buffer, time, data_size);
	if (SPA_UNLIKELY(retbuf == NULL))
		return -ENOBUFS;
	memcpy (retbuf, data, data_size);
	if (fix)
		fix_midi_event(retbuf, data_size);
	return 0;
}

static void convert_to_event(struct spa_pod_sequence **seq, uint32_t n_seq, void *midi, bool fix, uint32_t type)
{
	struct spa_pod_control *c[n_seq];
	uint64_t state = 0;
	uint32_t i;
	int res = 0;
	bool in_sysex = false;

	for (i = 0; i < n_seq; i++)
		c[i] = spa_pod_control_first(&seq[i]->body);

	while (true) {
		struct spa_pod_control *next = NULL;
		uint32_t next_index = 0;

		for (i = 0; i < n_seq; i++) {
			if (!spa_pod_control_is_inside(&seq[i]->body,
						SPA_POD_BODY_SIZE(seq[i]), c[i]))
				continue;

			if (next == NULL || event_sort(c[i], next) <= 0) {
				next = c[i];
				next_index = i;
			}
		}
		if (SPA_UNLIKELY(next == NULL))
			break;

		switch(next->type) {
		case SPA_CONTROL_OSC:
			if (!TYPE_ID_CAN_OSC(type))
				break;
			SPA_FALLTHROUGH;
		case SPA_CONTROL_Midi:
		{
			uint8_t *data = SPA_POD_BODY(&next->value);
			size_t size = SPA_POD_BODY_SIZE(&next->value);

			if (type == TYPE_ID_UMP) {
				while (size > 0) {
					uint32_t ump[4];
					int ump_size = spa_ump_from_midi(&data, &size, ump, sizeof(ump), 0, &state);
					if (ump_size <= 0)
						break;
					if ((res = midi_event_write(midi, next->offset,
								(uint8_t*)ump, ump_size, false)) < 0)
						break;
				}
			} else {
				res = midi_event_write(midi, next->offset, data, size, fix);
			}
			if (res < 0)
				pw_log_warn("midi %p: can't write event: %s", midi,
						spa_strerror(res));
			break;
		}
		case SPA_CONTROL_UMP:
		{
			void *data = SPA_POD_BODY(&next->value);
			size_t size = SPA_POD_BODY_SIZE(&next->value);
			uint8_t ev[32];
			bool was_sysex = in_sysex;

			if (type == TYPE_ID_MIDI) {
				int ev_size = spa_ump_to_midi(data, size, ev, sizeof(ev));
				if (ev_size <= 0)
					break;

				size = ev_size;
				data = ev;

				if (!in_sysex && ev[0] == 0xf0)
					in_sysex = true;
				if (in_sysex && ev[ev_size-1] == 0xf7)
					in_sysex = false;

			} else if (type != TYPE_ID_UMP)
				break;

			if (was_sysex)
				res = midi_event_append(midi, data, size);
			else
				res = midi_event_write(midi, next->offset, data, size, fix);

			if (res < 0)
				pw_log_warn("midi %p: can't write event: %s", midi,
						spa_strerror(res));
		}
		}
		c[next_index] = spa_pod_control_next(c[next_index]);
	}
}


static inline void *get_buffer_output(struct port *p, uint32_t frames, uint32_t stride, struct buffer **buf)
{
	struct mix *mix;
	struct client *c = p->client;
	void *ptr = NULL;
	struct buffer *b;
	struct spa_data *d;
	struct spa_io_buffers *io;
	uint32_t cycle = p->client->rt.position->clock.cycle & 1;

	if (frames == 0 || !p->valid)
		return NULL;

	if (SPA_UNLIKELY((mix = p->global_mix) == NULL))
		return NULL;

	pw_log_trace_fp("%p: port %s %d get buffer %d n_buffers:%d io:%p",
			c, p->object->port.name, p->port_id, frames,
			mix->n_buffers, mix->io);

	if (SPA_UNLIKELY((io = mix->io[cycle]) == NULL || mix->n_buffers == 0))
		return NULL;

	if (io->status == SPA_STATUS_HAVE_DATA &&
	    io->buffer_id < mix->n_buffers) {
		b = &mix->buffers[io->buffer_id];
		d = &b->datas[0];
	} else {
		if (mix->n_buffers == 1) {
			b = &mix->buffers[0];
		} else {
			if (io->buffer_id < mix->n_buffers)
				queue_buffer(c, mix, io->buffer_id);
			b = dequeue_buffer(c, mix);

			if (SPA_UNLIKELY(b == NULL)) {
				pw_log_warn("port %p: out of buffers %d", p, mix->n_buffers);
				io->buffer_id = SPA_ID_INVALID;
				return NULL;
			}
		}
		d = &b->datas[0];
		d->chunk->offset = 0;
		d->chunk->size = c->buffer_frames * sizeof(float);
		d->chunk->stride = stride;

		io->buffer_id = b->id;
		io->status = SPA_STATUS_HAVE_DATA;
	}
	ptr = d->data;
	if (buf)
		*buf = b;
	return ptr;
}

static inline void process_empty(struct port *p, uint32_t frames)
{
	struct client *c = p->client;
	void *ptr, *src = p->emptyptr;
	struct port *tied = p->tied;
	uint32_t type = p->object->port.type_id;

	if (SPA_UNLIKELY(tied != NULL)) {
		if ((src = tied->get_buffer(tied, frames)) == NULL)
			src = p->emptyptr;
	}

	switch (type) {
	case TYPE_ID_AUDIO:
		ptr = get_buffer_output(p, frames, sizeof(float), NULL);
		if (SPA_LIKELY(ptr != NULL))
			memcpy(ptr, src, frames * sizeof(float));
		break;
	case TYPE_ID_MIDI:
	case TYPE_ID_OSC:
	case TYPE_ID_UMP:
	{
		struct buffer *b;
		ptr = get_buffer_output(p, c->max_frames, 1, &b);
		if (SPA_LIKELY(ptr != NULL)) {
			/* first build the complete pod in scratch memory, then copy it
			 * to the target buffer. This makes it possible for multiple threads
			 * to do this concurrently */
			b->datas[0].chunk->size = convert_from_event(src, midi_scratch,
					MIDI_SCRATCH_FRAMES * sizeof(float), type);
			memcpy(ptr, midi_scratch, b->datas[0].chunk->size);
		}
		break;
	}
	default:
		pw_log_warn("port %p: unhandled format %d", p, p->object->port.type_id);
		break;
	}
}

static void prepare_output(struct port *p, uint32_t frames, uint32_t cycle)
{
	struct mix *mix;
	struct spa_io_buffers *io;

	if (SPA_UNLIKELY(p->empty_out || p->tied))
		process_empty(p, frames);

	if (p->global_mix == NULL || (io = p->global_mix->io[cycle]) == NULL)
		return;

	spa_list_for_each(mix, &p->mix, port_link) {
		if (SPA_LIKELY(mix->io[cycle] != NULL))
			*mix->io[cycle] = *io;
	}
}

static void complete_process(struct client *c, uint32_t frames)
{
	struct port *p;
	struct mix *mix;
	union pw_map_item *item;
	uint32_t cycle = c->rt.position->clock.cycle & 1;

	pw_array_for_each(item, &c->ports[SPA_DIRECTION_OUTPUT].items) {
                if (pw_map_item_is_free(item))
			continue;
		p = item->data;
		if (!p->valid)
			continue;
		prepare_output(p, frames, cycle);
		p->io[cycle].status = SPA_STATUS_NEED_DATA;
	}
	pw_array_for_each(item, &c->ports[SPA_DIRECTION_INPUT].items) {
                if (pw_map_item_is_free(item))
			continue;
		p = item->data;
		if (!p->valid)
			continue;
		spa_list_for_each(mix, &p->mix, port_link) {
			if (SPA_LIKELY(mix->io[cycle] != NULL))
				mix->io[cycle]->status = SPA_STATUS_NEED_DATA;
		}
        }
}

static inline void debug_position(struct client *c, jack_position_t *p)
{
#define pw_log_custom pw_log_trace_fp
	pw_log_custom("usecs:       %"PRIu64, p->usecs);
	pw_log_custom("frame_rate:  %u", p->frame_rate);
	pw_log_custom("frame:       %u", p->frame);
	pw_log_custom("valid:       %08x", p->valid);

	if (p->valid & JackPositionBBT) {
		pw_log_custom("BBT");
		pw_log_custom(" bar:              %u", p->bar);
		pw_log_custom(" beat:             %u", p->beat);
		pw_log_custom(" tick:             %u", p->tick);
		pw_log_custom(" bar_start_tick:   %f", p->bar_start_tick);
		pw_log_custom(" beats_per_bar:    %f", p->beats_per_bar);
		pw_log_custom(" beat_type:        %f", p->beat_type);
		pw_log_custom(" ticks_per_beat:   %f", p->ticks_per_beat);
		pw_log_custom(" beats_per_minute: %f", p->beats_per_minute);
	}
	if (p->valid & JackPositionTimecode) {
		pw_log_custom("Timecode:");
		pw_log_custom(" frame_time:       %f", p->frame_time);
		pw_log_custom(" next_time:        %f", p->next_time);
	}
	if (p->valid & JackBBTFrameOffset) {
		pw_log_custom("BBTFrameOffset:");
		pw_log_custom(" bbt_offset:       %u", p->bbt_offset);
	}
	if (p->valid & JackAudioVideoRatio) {
		pw_log_custom("AudioVideoRatio:");
		pw_log_custom(" audio_frames_per_video_frame: %f", p->audio_frames_per_video_frame);
	}
	if (p->valid & JackVideoFrameOffset) {
		pw_log_custom("JackVideoFrameOffset:");
		pw_log_custom(" video_offset:     %u", p->video_offset);
	}
#undef pw_log_custom
}

static inline void jack_to_position(jack_position_t *s, struct pw_node_activation *a)
{
	struct spa_io_segment *d = &a->segment;

	if (s->valid & JackPositionBBT) {
		d->bar.flags = SPA_IO_SEGMENT_BAR_FLAG_VALID;
		if (s->valid & JackBBTFrameOffset)
			d->bar.offset = s->bbt_offset;
		else
			d->bar.offset = 0;
		d->bar.signature_num = s->beats_per_bar;
		d->bar.signature_denom = s->beat_type;
		d->bar.ticks_per_beat = s->ticks_per_beat;
		d->bar.bar_start_tick = s->bar_start_tick;
		d->bar.bpm = s->beats_per_minute;
		d->bar.beat = s->bar * s->beats_per_bar + (s->beat-1) +
			(s->tick / s->ticks_per_beat);
	}
}

static inline jack_transport_state_t position_to_jack(struct pw_node_activation *a,
		jack_position_t *d, struct frame_times *t)
{
	struct spa_io_position *s = &a->position;
	jack_transport_state_t state;
	struct spa_io_segment *seg = &s->segments[0];
	uint64_t running;

	switch (s->state) {
	default:
	case SPA_IO_POSITION_STATE_STOPPED:
		state = JackTransportStopped;
		break;
	case SPA_IO_POSITION_STATE_STARTING:
		state = JackTransportStarting;
		break;
	case SPA_IO_POSITION_STATE_RUNNING:
		if (seg->flags & SPA_IO_SEGMENT_FLAG_LOOPING)
			state = JackTransportLooping;
		else
			state = JackTransportRolling;
		break;
	}
	if (SPA_UNLIKELY(d == NULL))
		return state;

	d->unique_1++;
	t->frames = s->clock.position;
	t->nsec = s->clock.nsec;
	d->usecs = t->nsec / SPA_NSEC_PER_USEC;
	t->next_nsec = s->clock.next_nsec;
	t->rate_diff = s->clock.rate_diff;
	t->buffer_frames = s->clock.duration;
	d->frame_rate = t->sample_rate = s->clock.rate.denom;

	if ((int64_t)s->clock.position < s->offset) {
		d->frame = seg->position;
	} else {
		running = s->clock.position - s->offset;
		if (running >= seg->start &&
		    (seg->duration == 0 || running < seg->start + seg->duration))
			d->frame = (unsigned int)((running - seg->start) * seg->rate + seg->position);
		else
			d->frame = seg->position;
	}
	d->valid = 0;
	if (a->segment_owner[0] && SPA_FLAG_IS_SET(seg->bar.flags, SPA_IO_SEGMENT_BAR_FLAG_VALID)) {
		double abs_beat;
		long beats;

		d->valid |= JackPositionBBT;

		d->bbt_offset = seg->bar.offset;
		if (seg->bar.offset)
			d->valid |= JackBBTFrameOffset;

		d->beats_per_bar = seg->bar.signature_num;
		d->beat_type = seg->bar.signature_denom;
		d->ticks_per_beat = seg->bar.ticks_per_beat;
		d->bar_start_tick = seg->bar.bar_start_tick;
		d->beats_per_minute = seg->bar.bpm;

		abs_beat = seg->bar.beat;

		d->bar = (int32_t) (abs_beat / d->beats_per_bar);
		beats = (long int) (d->bar * d->beats_per_bar);
		d->beat = (int32_t) (abs_beat - beats);
		beats += d->beat;
		d->tick = (int32_t) ((abs_beat - beats) * d->ticks_per_beat);
		d->beat++;
	}
	d->unique_2 = d->unique_1;
	return state;
}

static inline int check_buffer_frames(struct client *c, struct spa_io_position *pos)
{
	uint32_t buffer_frames = pos->clock.duration;
	if (SPA_UNLIKELY(buffer_frames != c->buffer_frames)) {
		pw_log_info("%p: bufferframes old:%d new:%d cb:%p", c,
				c->buffer_frames, buffer_frames, c->bufsize_callback);
		if (c->buffer_frames != (uint32_t)-1)
			queue_notify(c, NOTIFY_TYPE_BUFFER_FRAMES, NULL, buffer_frames, NULL);
		else
			c->buffer_frames = buffer_frames;
	}
	return c->buffer_frames == buffer_frames;
}

static inline int check_sample_rate(struct client *c, struct spa_io_position *pos)
{
	uint32_t sample_rate = pos->clock.rate.denom;
	if (SPA_UNLIKELY(sample_rate != c->sample_rate)) {
		pw_log_info("%p: sample_rate old:%d new:%d cb:%p", c,
				c->sample_rate, sample_rate, c->srate_callback);
		if (c->sample_rate != (uint32_t)-1)
			queue_notify(c, NOTIFY_TYPE_SAMPLE_RATE, NULL, sample_rate, NULL);
		else
			c->sample_rate = sample_rate;
	}
	return c->sample_rate == sample_rate;
}

static inline uint32_t cycle_run(struct client *c)
{
	uint64_t cmd;
	int fd = c->socket_source->fd;
	struct spa_io_position *pos = c->rt.position;
	struct pw_node_activation *activation = c->activation;
	struct pw_node_activation *driver = c->rt.driver_activation;

	while (true) {
		if (SPA_UNLIKELY(read(fd, &cmd, sizeof(cmd)) != sizeof(cmd))) {
			if (errno == EINTR)
				continue;
			if (errno == EWOULDBLOCK || errno == EAGAIN)
				return 0;
			pw_log_warn("%p: read failed %m", c);
		}
		break;
	}
	if (SPA_UNLIKELY(cmd > 1)) {
		pw_log_info("%p: missed %"PRIu64" wakeups", c, cmd - 1);
		activation->xrun_count += cmd - 1;
		activation->xrun_time = activation->awake_time;
		activation->xrun_delay = 0;
		activation->max_delay = SPA_MAX(activation->max_delay, 0u);
	}

	if (!SPA_ATOMIC_CAS(activation->status,
				PW_NODE_ACTIVATION_TRIGGERED,
				PW_NODE_ACTIVATION_AWAKE))
		return 0;

	activation->awake_time = get_time_ns(c->l->system);

	if (SPA_UNLIKELY(c->rt.first)) {
		if (c->thread_init_callback)
			c->thread_init_callback(c->thread_init_arg);
		c->rt.first = false;
	}

	if (SPA_UNLIKELY(pos == NULL)) {
		pw_log_error("%p: missing position", c);
		return 0;
	}

	if (check_buffer_frames(c, pos) == 0)
		return 0;
	if (check_sample_rate(c, pos) == 0)
		return 0;

	if (SPA_LIKELY(driver)) {
		c->jack_state = position_to_jack(driver, &c->jack_position, &c->jack_times);

		if (SPA_UNLIKELY(activation->pending_sync)) {
			if (c->sync_callback == NULL ||
			    c->sync_callback(c->jack_state, &c->jack_position, c->sync_arg))
				activation->pending_sync = false;
		}
		if (SPA_UNLIKELY(c->xrun_count != driver->xrun_count &&
		    c->xrun_count != 0 && c->xrun_callback))
			c->xrun_callback(c->xrun_arg);
		c->xrun_count = driver->xrun_count;
	}
	pw_log_trace_fp("%p: wait %"PRIu64" frames:%d rate:%d pos:%d delay:%"PRIi64" corr:%f", c,
			activation->awake_time, c->buffer_frames, c->sample_rate,
			c->jack_position.frame, pos->clock.delay, pos->clock.rate_diff);

	return c->buffer_frames;
}

static inline uint32_t cycle_wait(struct client *c)
{
	int res;
	uint32_t nframes;

	do {
		res = pw_data_loop_wait(c->loop, -1);
		if (SPA_UNLIKELY(res <= 0)) {
			pw_log_warn("%p: wait error %m", c);
			return 0;
		}
		nframes = cycle_run(c);
	} while (!nframes);

	return nframes;
}

static void trigger_link_v1(struct link *l, uint64_t nsec)
{
	struct client *c = l->client;
	struct pw_node_activation *a = l->activation;
	struct pw_node_activation_state *state = &a->state[0];
	uint64_t cmd = 1;

	pw_log_trace_fp("%p: link %p-%d %p %d/%d", c, l, l->node_id, state,
			state->pending, state->required);

	if (pw_node_activation_state_dec(state)) {
		if (SPA_ATOMIC_CAS(a->status,
					PW_NODE_ACTIVATION_NOT_TRIGGERED,
					PW_NODE_ACTIVATION_TRIGGERED)) {
			a->signal_time = nsec;

			pw_log_trace_fp("%p: signal %p %p", c, l, state);

			if (SPA_UNLIKELY(write(l->signalfd, &cmd, sizeof(cmd)) != sizeof(cmd)))
				pw_log_warn("%p: write failed %m", c);
		}
	}
}

static void trigger_link_v0(struct link *l, uint64_t nsec)
{
	struct client *c = l->client;
	struct pw_node_activation *a = l->activation;
	struct pw_node_activation_state *state = &a->state[0];
	uint64_t cmd = 1;

	pw_log_trace_fp("%p: link %p-%d %p %d/%d", c, l, l->node_id, state,
			state->pending, state->required);

	if (pw_node_activation_state_dec(state)) {
		SPA_ATOMIC_STORE(a->status, PW_NODE_ACTIVATION_TRIGGERED);
		a->signal_time = nsec;

		pw_log_trace_fp("%p: signal %p %p", c, l, state);

		if (SPA_UNLIKELY(write(l->signalfd, &cmd, sizeof(cmd)) != sizeof(cmd)))
			pw_log_warn("%p: write failed %m", c);
	}
}

static inline void deactivate_link(struct client *c, struct link *l, uint64_t trigger)
{
	if (!c->async && trigger != 0)
		l->trigger(l, trigger);
}

static inline void signal_sync(struct client *c)
{
	uint64_t nsec;
	struct link *l;
	struct pw_node_activation *activation = c->activation;
	int old_status;

	complete_process(c, c->buffer_frames);

	nsec = get_time_ns(c->l->system);
	old_status = SPA_ATOMIC_XCHG(activation->status, PW_NODE_ACTIVATION_FINISHED);
	activation->finish_time = nsec;

	if (c->async || old_status != PW_NODE_ACTIVATION_AWAKE)
		return;

	spa_list_for_each(l, &c->rt.target_links, target_link)
		l->trigger(l, nsec);
}

static inline void cycle_signal(struct client *c, int status)
{
	struct pw_node_activation *driver = c->rt.driver_activation;
	struct pw_node_activation *activation = c->activation;

	if (SPA_LIKELY(status == 0)) {
		if (c->timebase_callback && driver && driver->segment_owner[0] == c->node_id) {
			if (activation->pending_new_pos ||
			    c->jack_state == JackTransportRolling ||
			    c->jack_state == JackTransportLooping) {
				c->timebase_callback(c->jack_state,
						     c->buffer_frames,
						     &c->jack_position,
						     activation->pending_new_pos,
						     c->timebase_arg);

				activation->pending_new_pos = false;

				debug_position(c, &c->jack_position);
				jack_to_position(&c->jack_position, activation);
			}
		}
	}
	signal_sync(c);
}

static void
on_rtsocket_condition(void *data, int fd, uint32_t mask)
{
	struct client *c = data;

	if (SPA_UNLIKELY(mask & (SPA_IO_ERR | SPA_IO_HUP))) {
		pw_log_warn("%p: got error", c);
		client_remove_source(c);
		return;
	}
	if (SPA_UNLIKELY(c->thread_callback)) {
		if (!c->rt.thread_entered) {
			c->rt.thread_entered = true;
			c->thread_callback(c->thread_arg);
		}
	} else if (SPA_LIKELY(mask & SPA_IO_IN)) {
		uint32_t buffer_frames;
		int status = 0;

		buffer_frames = cycle_run(c);

		if (buffer_frames > 0)
			status = do_rt_callback_res(c, process_callback, buffer_frames, c->process_arg);

		cycle_signal(c, status);
	}
}

static void free_link(struct link *link)
{
	pw_log_debug("free link %p", link);
	pw_memmap_free(link->mem);
	close(link->signalfd);
	free(link);
}

static int
do_clean_transport(struct spa_loop *loop,
                  bool async, uint32_t seq, const void *data, size_t size, void *user_data)
{
	struct client *c = user_data;
	struct link *l;
	pw_log_debug("%p: clean transport", c);
	client_remove_source(c);
	spa_list_consume(l, &c->rt.target_links, target_link)
		spa_list_remove(&l->target_link);
	return 0;
}

static void clean_transport(struct client *c)
{
	struct link *l;

	if (!c->has_transport)
		return;

	/* We assume the data-loop is unlocked now and can process our
	 * clean function. This is reasonable, the cleanup function is run when
	 * closing the client, which should join the data-thread. */
	pw_data_loop_invoke(c->loop, do_clean_transport, 1, NULL, 0, true, c);

	spa_list_consume(l, &c->links, link) {
		spa_list_remove(&l->link);
		free_link(l);
	}
	c->has_transport = false;
}

static int client_node_transport(void *data,
                           int readfd, int writefd,
			   uint32_t mem_id, uint32_t offset, uint32_t size)
{
	struct client *c = (struct client *) data;

	clean_transport(c);

	c->mem = pw_mempool_map_id(c->pool, mem_id,
				PW_MEMMAP_FLAG_READWRITE, offset, size, NULL);
	if (c->mem == NULL) {
		pw_log_debug("%p: can't map activation: %m", c);
		return -errno;
	}
	c->activation = c->mem->ptr;

	pw_log_debug("%p: create client transport with fds %d %d for node %u",
			c, readfd, writefd, c->node_id);

	c->activation->client_version = PW_VERSION_NODE_ACTIVATION;

	close(writefd);
	c->socket_source = pw_loop_add_io(c->l,
					  readfd,
					  SPA_IO_ERR | SPA_IO_HUP,
					  true, on_rtsocket_condition, c);

	c->has_transport = true;
	c->position = &c->activation->position;
	pw_thread_loop_signal(c->context.loop, false);

	return 0;
}

static int client_node_set_param(void *data,
			uint32_t id, uint32_t flags,
			const struct spa_pod *param)
{
	struct client *c = (struct client *) data;
	pw_proxy_error((struct pw_proxy*)c->node, -ENOTSUP, "not supported");
	return -ENOTSUP;
}

static int install_timeowner(struct client *c)
{
	struct pw_node_activation *a;
	uint32_t owner;

	if (!c->timebase_callback)
		return 0;

	if ((a = c->driver_activation) == NULL)
		return -EIO;

	pw_log_debug("%p: activation %p", c, a);

	/* was ok */
	owner = SPA_ATOMIC_LOAD(a->segment_owner[0]);
	if (owner == c->node_id)
		return 0;

	/* try to become owner */
	if (c->timeowner_conditional) {
		if (!SPA_ATOMIC_CAS(a->segment_owner[0], 0, c->node_id)) {
			pw_log_debug("%p: owner:%u id:%u", c, owner, c->node_id);
			return -EBUSY;
		}
	} else {
		SPA_ATOMIC_STORE(a->segment_owner[0], c->node_id);
	}

	pw_log_debug("%p: timebase installed for id:%u", c, c->node_id);

	return 0;
}

static int
do_update_driver_activation(struct spa_loop *loop,
                bool async, uint32_t seq, const void *data, size_t size, void *user_data)
{
	struct client *c = user_data;
	c->rt.position = c->position;
	c->rt.driver_activation = c->driver_activation;
	if (c->position) {
		pw_log_debug("%p: driver:%d clock:%s", c,
				c->driver_id, c->position->clock.name);
		check_sample_rate(c, c->position);
		check_buffer_frames(c, c->position);
	}
	return 0;
}

static int update_driver_activation(struct client *c)
{
	jack_client_t *client = (jack_client_t*)c;
	struct link *link;
	bool freewheeling;

	pw_log_debug("%p: driver %d", c, c->driver_id);

	freewheeling = SPA_FLAG_IS_SET(c->position->clock.flags, SPA_IO_CLOCK_FLAG_FREEWHEEL);
	if (c->freewheeling != freewheeling) {
		jack_native_thread_t thr = jack_client_thread_id(client);

		c->freewheeling = freewheeling;
		if (freewheeling && thr) {
			jack_drop_real_time_scheduling(thr);
		}

		queue_notify(c, NOTIFY_TYPE_FREEWHEEL, NULL, freewheeling, NULL);

		if (!freewheeling && thr) {
			jack_acquire_real_time_scheduling(thr,
					jack_client_real_time_priority(client));
		}
	}

	link = find_activation(&c->links, c->driver_id);
	c->driver_activation = link ? link->activation : NULL;
	pw_data_loop_invoke(c->loop,
                       do_update_driver_activation, SPA_ID_INVALID, NULL, 0, false, c);
	install_timeowner(c);

	return 0;
}

static int
do_memmap_free(struct spa_loop *loop,
                bool async, uint32_t seq, const void *data, size_t size, void *user_data)
{
	struct client *c = user_data;
	struct pw_memmap *mm = *((struct pw_memmap **)data);
	pw_log_trace("memmap %p free", mm);
	pw_memmap_free(mm);
	pw_core_set_paused(c->core, false);
	return 0;
}

static int
do_queue_memmap_free(struct spa_loop *loop,
                bool async, uint32_t seq, const void *data, size_t size, void *user_data)
{
	struct client *c = user_data;
	pw_loop_invoke(c->context.l, do_memmap_free, 0, data, size, false, c);
	return 0;
}

static void queue_memmap_free(struct client *c, struct pw_memmap *mem)
{
	if (mem != NULL) {
		mem->tag[0] = SPA_ID_INVALID;
		pw_core_set_paused(c->core, true);
		pw_data_loop_invoke(c->loop,
			do_queue_memmap_free, SPA_ID_INVALID, &mem, sizeof(&mem), false, c);
	}
}

static int client_node_set_io(void *data,
			uint32_t id,
			uint32_t mem_id,
			uint32_t offset,
			uint32_t size)
{
	struct client *c = (struct client *) data;
	struct pw_memmap *old, *mm;
	void *ptr;
	uint32_t tag[5] = { c->node_id, id, };

	old = pw_mempool_find_tag(c->pool, tag, sizeof(tag));

	if (mem_id == SPA_ID_INVALID) {
		mm = ptr = NULL;
	} else {
		mm = pw_mempool_map_id(c->pool, mem_id,
				PW_MEMMAP_FLAG_READWRITE, offset, size, tag);
		if (mm == NULL) {
                        pw_log_warn("%p: can't map memory id %u: %m", c, mem_id);
			return -errno;
		}
		ptr = mm->ptr;
	}
	pw_log_debug("%p: set io %s %p", c,
			spa_debug_type_find_name(spa_type_io, id), ptr);

	switch (id) {
	case SPA_IO_Position:
		c->position = ptr;
		c->driver_id = ptr ? c->position->clock.id : SPA_ID_INVALID;
		update_driver_activation(c);
		c->activation->active_driver_id = c->driver_id;
		queue_memmap_free(c, old);
		old = NULL;
		break;
	default:
		break;
	}
	pw_memmap_free(old);

	return 0;
}

static int client_node_event(void *data, const struct spa_event *event)
{
	return -ENOTSUP;
}

static int do_prepare_client(struct spa_loop *loop, bool async, uint32_t seq,
		const void *data, size_t size, void *user_data)
{
	struct client *c = user_data;

	pw_log_debug("%p prepared:%d ", c, c->rt.prepared);
	if (c->rt.prepared)
		return 0;

	SPA_ATOMIC_STORE(c->activation->status, PW_NODE_ACTIVATION_FINISHED);
	pw_loop_update_io(c->l,
			  c->socket_source,
			  SPA_IO_IN | SPA_IO_ERR | SPA_IO_HUP);

	c->rt.first = true;
	c->rt.thread_entered = false;
	c->rt.prepared = true;
	return 0;
}

static int do_unprepare_client(struct spa_loop *loop, bool async, uint32_t seq,
		const void *data, size_t size, void *user_data)
{
	struct client *c = user_data;
	int old_state;
	uint64_t trigger = 0;
	struct link *l;

	pw_log_debug("%p prepared:%d ", c, c->rt.prepared);

	old_state = SPA_ATOMIC_XCHG(c->activation->status, PW_NODE_ACTIVATION_INACTIVE);
	if (old_state != PW_NODE_ACTIVATION_FINISHED)
		trigger = get_time_ns(c->l->system);

	if (!c->rt.prepared)
		return 0;

	spa_list_for_each(l, &c->rt.target_links, target_link) {
		if (!c->async && trigger != 0)
			l->trigger(l, trigger);
	}

	pw_loop_update_io(c->l,
			  c->socket_source, SPA_IO_ERR | SPA_IO_HUP);

	c->rt.prepared = false;
	return 0;
}

static int client_node_command(void *data, const struct spa_command *command)
{
	struct client *c = (struct client *) data;

	pw_log_debug("%p: got command %d", c, SPA_COMMAND_TYPE(command));

	switch (SPA_NODE_COMMAND_ID(command)) {
	case SPA_NODE_COMMAND_Suspend:
	case SPA_NODE_COMMAND_Pause:
		if (c->started) {
			pw_data_loop_invoke(c->loop,
				do_unprepare_client, SPA_ID_INVALID, NULL, 0, false, c);
			c->started = false;
		}
		break;

	case SPA_NODE_COMMAND_Start:
		if (!c->started) {
			pw_data_loop_invoke(c->loop,
				do_prepare_client, SPA_ID_INVALID, NULL, 0, false, c);
			c->started = true;
		}
		break;
	default:
		pw_log_warn("%p: unhandled node command %d", c, SPA_COMMAND_TYPE(command));
		pw_proxy_errorf((struct pw_proxy*)c->node, -ENOTSUP,
				"unhandled command %d", SPA_COMMAND_TYPE(command));
	}
	return 0;
}

static int client_node_add_port(void *data,
                          enum spa_direction direction,
                          uint32_t port_id, const struct spa_dict *props)
{
	struct client *c = (struct client *) data;
	pw_proxy_error((struct pw_proxy*)c->node, -ENOTSUP, "add port not supported");
	return -ENOTSUP;
}

static int client_node_remove_port(void *data,
                             enum spa_direction direction,
                             uint32_t port_id)
{
	struct client *c = (struct client *) data;
	pw_proxy_error((struct pw_proxy*)c->node, -ENOTSUP, "remove port not supported");
	return -ENOTSUP;
}

static int param_enum_format(struct client *c, struct port *p,
		struct spa_pod **param, struct spa_pod_builder *b)
{
	switch (p->object->port.type_id) {
	case TYPE_ID_AUDIO:
		*param = spa_pod_builder_add_object(b,
			SPA_TYPE_OBJECT_Format, SPA_PARAM_EnumFormat,
			SPA_FORMAT_mediaType,      SPA_POD_Id(SPA_MEDIA_TYPE_audio),
			SPA_FORMAT_mediaSubtype,   SPA_POD_Id(SPA_MEDIA_SUBTYPE_dsp),
	                SPA_FORMAT_AUDIO_format,   SPA_POD_Id(SPA_AUDIO_FORMAT_DSP_F32));
		break;
	case TYPE_ID_UMP:
	case TYPE_ID_OSC:
	case TYPE_ID_MIDI:
		*param = spa_pod_builder_add_object(b,
			SPA_TYPE_OBJECT_Format, SPA_PARAM_EnumFormat,
			SPA_FORMAT_mediaType,      SPA_POD_Id(SPA_MEDIA_TYPE_application),
			SPA_FORMAT_mediaSubtype,   SPA_POD_Id(SPA_MEDIA_SUBTYPE_control));
		break;
	case TYPE_ID_VIDEO:
		*param = spa_pod_builder_add_object(b,
			SPA_TYPE_OBJECT_Format, SPA_PARAM_EnumFormat,
			SPA_FORMAT_mediaType,      SPA_POD_Id(SPA_MEDIA_TYPE_video),
			SPA_FORMAT_mediaSubtype,   SPA_POD_Id(SPA_MEDIA_SUBTYPE_dsp),
	                SPA_FORMAT_VIDEO_format,   SPA_POD_Id(SPA_VIDEO_FORMAT_DSP_F32));
		break;
	default:
		return -EINVAL;
	}
	return 1;
}

static int param_format(struct client *c, struct port *p,
		struct spa_pod **param, struct spa_pod_builder *b)
{
	switch (p->object->port.type_id) {
	case TYPE_ID_AUDIO:
		*param = spa_pod_builder_add_object(b,
				SPA_TYPE_OBJECT_Format, SPA_PARAM_Format,
				SPA_FORMAT_mediaType,      SPA_POD_Id(SPA_MEDIA_TYPE_audio),
				SPA_FORMAT_mediaSubtype,   SPA_POD_Id(SPA_MEDIA_SUBTYPE_dsp),
		                SPA_FORMAT_AUDIO_format,   SPA_POD_Id(SPA_AUDIO_FORMAT_DSP_F32));
		break;
	case TYPE_ID_MIDI:
	case TYPE_ID_OSC:
	case TYPE_ID_UMP:
		*param = spa_pod_builder_add_object(b,
				SPA_TYPE_OBJECT_Format, SPA_PARAM_Format,
				SPA_FORMAT_mediaType,      SPA_POD_Id(SPA_MEDIA_TYPE_application),
				SPA_FORMAT_mediaSubtype,   SPA_POD_Id(SPA_MEDIA_SUBTYPE_control));
		break;
	case TYPE_ID_VIDEO:
		*param = spa_pod_builder_add_object(b,
				SPA_TYPE_OBJECT_Format, SPA_PARAM_Format,
				SPA_FORMAT_mediaType,      SPA_POD_Id(SPA_MEDIA_TYPE_video),
				SPA_FORMAT_mediaSubtype,   SPA_POD_Id(SPA_MEDIA_SUBTYPE_dsp),
		                SPA_FORMAT_VIDEO_format,   SPA_POD_Id(SPA_VIDEO_FORMAT_DSP_F32));
		break;
	default:
		return -EINVAL;
	}
	return 1;
}

static int param_buffers(struct client *c, struct port *p,
		struct spa_pod **param, struct spa_pod_builder *b)
{
	switch (p->object->port.type_id) {
	case TYPE_ID_AUDIO:
	case TYPE_ID_MIDI:
	case TYPE_ID_OSC:
	case TYPE_ID_UMP:
		*param = spa_pod_builder_add_object(b,
			SPA_TYPE_OBJECT_ParamBuffers, SPA_PARAM_Buffers,
			SPA_PARAM_BUFFERS_buffers, SPA_POD_CHOICE_RANGE_Int(1, 1, MAX_BUFFERS),
			SPA_PARAM_BUFFERS_blocks,  SPA_POD_Int(1),
			SPA_PARAM_BUFFERS_size,    SPA_POD_CHOICE_STEP_Int(
								c->max_frames * sizeof(float),
								sizeof(float),
								INT32_MAX,
								sizeof(float)),
			SPA_PARAM_BUFFERS_stride,  SPA_POD_Int(p->object->port.type_id == TYPE_ID_AUDIO ?
									sizeof(float) : 1));
		break;
	case TYPE_ID_VIDEO:
		*param = spa_pod_builder_add_object(b,
			SPA_TYPE_OBJECT_ParamBuffers, SPA_PARAM_Buffers,
			SPA_PARAM_BUFFERS_buffers, SPA_POD_CHOICE_RANGE_Int(1, 1, MAX_BUFFERS),
			SPA_PARAM_BUFFERS_blocks,  SPA_POD_Int(1),
			SPA_PARAM_BUFFERS_size,    SPA_POD_CHOICE_RANGE_Int(
								320 * 240 * 4 * 4,
								0,
								INT32_MAX),
			SPA_PARAM_BUFFERS_stride,  SPA_POD_CHOICE_RANGE_Int(4, 4, INT32_MAX));
		break;
	default:
		return -EINVAL;
	}
	return 1;
}

static int param_io(struct client *c, struct port *p,
		struct spa_pod **param, struct spa_pod_builder *b)
{
	*param = spa_pod_builder_add_object(b,
		SPA_TYPE_OBJECT_ParamIO, SPA_PARAM_IO,
		SPA_PARAM_IO_id,	SPA_POD_Id(SPA_IO_Buffers),
		SPA_PARAM_IO_size,	SPA_POD_Int(sizeof(struct spa_io_buffers)));
	return 1;
}

static int param_io_async(struct client *c, struct port *p,
		struct spa_pod **param, struct spa_pod_builder *b)
{
	*param = spa_pod_builder_add_object(b,
		SPA_TYPE_OBJECT_ParamIO, SPA_PARAM_IO,
		SPA_PARAM_IO_id,	SPA_POD_Id(SPA_IO_AsyncBuffers),
		SPA_PARAM_IO_size,	SPA_POD_Int(sizeof(struct spa_io_async_buffers)));
	return 1;
}
static int param_latency(struct client *c, struct port *p,
		struct spa_pod **param, struct spa_pod_builder *b)
{
	*param = spa_latency_build(b, SPA_PARAM_Latency,
			&p->object->port.latency[p->direction]);
	return 1;
}

static int param_latency_other(struct client *c, struct port *p,
		struct spa_pod **param, struct spa_pod_builder *b)
{
	*param = spa_latency_build(b, SPA_PARAM_Latency,
			&p->object->port.latency[SPA_DIRECTION_REVERSE(p->direction)]);
	return 1;
}

/* called from thread-loop */
static int port_set_format(struct client *c, struct port *p,
		uint32_t flags, const struct spa_pod *param)
{
	struct spa_pod *params[7];
	uint8_t buffer[4096];
	struct spa_pod_builder b = SPA_POD_BUILDER_INIT(buffer, sizeof(buffer));

	if (param == NULL) {
		struct mix *mix;

		pw_log_debug("%p: port %p clear format", c, p);

		spa_list_for_each(mix, &p->mix, port_link)
			clear_buffers(c, mix);

		p->params[IDX_Format] = SPA_PARAM_INFO(SPA_PARAM_Format, SPA_PARAM_INFO_WRITE);
	}
	else {
		struct spa_audio_info info = { 0 };
		if (spa_format_parse(param, &info.media_type, &info.media_subtype) < 0)
			return -EINVAL;

		switch (info.media_type) {
		case SPA_MEDIA_TYPE_audio:
		{
			if (info.media_subtype != SPA_MEDIA_SUBTYPE_dsp)
				return -EINVAL;

			if (spa_format_audio_dsp_parse(param, &info.info.dsp) < 0)
				return -EINVAL;
			if (info.info.dsp.format != SPA_AUDIO_FORMAT_DSP_F32)
				return -EINVAL;
			break;
		}
		case SPA_MEDIA_TYPE_application:
			if (info.media_subtype != SPA_MEDIA_SUBTYPE_control)
				return -EINVAL;
			break;
		case SPA_MEDIA_TYPE_video:
		{
			struct spa_video_info vinfo = { 0 };

			if (info.media_subtype != SPA_MEDIA_SUBTYPE_dsp)
				return -EINVAL;
			if (spa_format_video_dsp_parse(param, &vinfo.info.dsp) < 0)
				return -EINVAL;
			if (vinfo.info.dsp.format != SPA_VIDEO_FORMAT_DSP_F32)
				return -EINVAL;
			break;
		}
		default:
			return -EINVAL;
		}
		p->params[IDX_Format] = SPA_PARAM_INFO(SPA_PARAM_Format, SPA_PARAM_INFO_READWRITE);
	}

	pw_log_debug("port %s: update", p->object->port.name);

	p->info.change_mask |= SPA_PORT_CHANGE_MASK_PARAMS;

	param_enum_format(c, p, &params[0], &b);
	param_format(c, p, &params[1], &b);
	param_buffers(c, p, &params[2], &b);
	param_io(c, p, &params[3], &b);
	param_io_async(c, p, &params[4], &b);
	param_latency(c, p, &params[5], &b);
	param_latency_other(c, p, &params[6], &b);

	pw_client_node_port_update(c->node,
					 p->direction,
					 p->port_id,
					 PW_CLIENT_NODE_PORT_UPDATE_PARAMS |
					 PW_CLIENT_NODE_PORT_UPDATE_INFO,
					 SPA_N_ELEMENTS(params),
					 (const struct spa_pod **) params,
					 &p->info);
	p->info.change_mask = 0;
	return 0;
}

/* called from thread-loop */
static void port_update_latency(struct port *p)
{
	struct client *c = p->client;
	struct spa_pod *params[7];
	uint8_t buffer[4096];
	struct spa_pod_builder b = SPA_POD_BUILDER_INIT(buffer, sizeof(buffer));

	param_enum_format(c, p, &params[0], &b);
	param_format(c, p, &params[1], &b);
	param_buffers(c, p, &params[2], &b);
	param_io(c, p, &params[3], &b);
	param_io_async(c, p, &params[4], &b);
	param_latency(c, p, &params[5], &b);
	param_latency_other(c, p, &params[6], &b);

	pw_log_debug("port %s: update", p->object->port.name);

	p->info.change_mask |= SPA_PORT_CHANGE_MASK_PARAMS;
	p->params[IDX_Latency].flags ^= SPA_PARAM_INFO_SERIAL;

	pw_client_node_port_update(c->node,
					 p->direction,
					 p->port_id,
					 PW_CLIENT_NODE_PORT_UPDATE_PARAMS |
					 PW_CLIENT_NODE_PORT_UPDATE_INFO,
					 SPA_N_ELEMENTS(params),
					 (const struct spa_pod **) params,
					 &p->info);
	p->info.change_mask = 0;
}

static void port_check_latency(struct port *p, const struct spa_latency_info *latency)
{
	struct spa_latency_info *current;
	struct client *c = p->client;
	struct object *o = p->object;

	current = &o->port.latency[latency->direction];
	if (spa_latency_info_compare(current, latency) == 0)
		return;
	*current = *latency;

	pw_log_info("%p: %s update %s latency %f-%f %d-%d %"PRIu64"-%"PRIu64, c,
			o->port.name,
			latency->direction == SPA_DIRECTION_INPUT ? "playback" : "capture",
			latency->min_quantum, latency->max_quantum,
			latency->min_rate, latency->max_rate,
			latency->min_ns, latency->max_ns);
	port_update_latency(p);
}

/* called from thread-loop */
static void default_latency(struct client *c, enum spa_direction direction,
		struct spa_latency_info *latency)
{
	enum spa_direction other;
	union pw_map_item *item;
	struct port *p;

	other = SPA_DIRECTION_REVERSE(direction);

	spa_latency_info_combine_start(latency, direction);

	pw_array_for_each(item, &c->ports[other].items) {
                if (pw_map_item_is_free(item))
			continue;
		p = item->data;
		spa_latency_info_combine(latency, &p->object->port.latency[direction]);
	}

	spa_latency_info_combine_finish(latency);
}

/* called from thread-loop */
static void default_latency_callback(jack_latency_callback_mode_t mode, struct client *c)
{
	struct spa_latency_info latency;
	union pw_map_item *item;
	enum spa_direction direction;
	struct port *p;

	if (mode == JackPlaybackLatency)
		direction = SPA_DIRECTION_INPUT;
	else
		direction = SPA_DIRECTION_OUTPUT;

	default_latency(c, direction, &latency);

	pw_array_for_each(item, &c->ports[direction].items) {
                if (pw_map_item_is_free(item))
			continue;
		p = item->data;
		port_check_latency(p, &latency);
	}
}

/* called from thread-loop */
static int port_set_latency(struct client *c, struct port *p,
		uint32_t flags, const struct spa_pod *param)
{
	struct spa_latency_info info;
	jack_latency_callback_mode_t mode;
	struct spa_latency_info *current;
	int res;

	if (param == NULL)
		info = SPA_LATENCY_INFO(SPA_DIRECTION_REVERSE(p->direction));
	else if ((res = spa_latency_parse(param, &info)) < 0)
		return res;
	if (info.direction == p->direction)
		return 0;

	current = &p->object->port.latency[info.direction];
	if (spa_latency_info_compare(current, &info) == 0)
		return 0;

	*current = info;

	pw_log_info("port %s: set %s latency %f-%f %d-%d %"PRIu64"-%"PRIu64, p->object->port.name,
			info.direction == SPA_DIRECTION_INPUT ? "playback" : "capture",
			info.min_quantum, info.max_quantum,
			info.min_rate, info.max_rate,
			info.min_ns, info.max_ns);


	if (info.direction == SPA_DIRECTION_INPUT)
		mode = JackPlaybackLatency;
	else
		mode = JackCaptureLatency;

	if (c->latency_callback)
		queue_notify(c, NOTIFY_TYPE_LATENCY, NULL, mode, NULL);
	else
		default_latency_callback(mode, c);

	port_update_latency(p);

	return 0;
}

/* called from thread-loop */
static int client_node_port_set_param(void *data,
                                enum spa_direction direction,
                                uint32_t port_id,
                                uint32_t id, uint32_t flags,
                                const struct spa_pod *param)
{
	struct client *c = (struct client *) data;
	struct port *p = GET_PORT(c, direction, port_id);

	if (p == NULL || !p->valid)
		return -EINVAL;

	pw_log_info("client %p: port %s %d.%d id:%d (%s) %p", c, p->object->port.name,
			direction, port_id, id,
			spa_debug_type_find_name(spa_type_param, id), param);

	switch (id) {
	case SPA_PARAM_Format:
		return port_set_format(c, p, flags, param);
		break;
	case SPA_PARAM_Latency:
		return port_set_latency(c, p, flags, param);
	default:
		break;
	}
	return 0;
}

static void midi_init_buffer(void *data, uint32_t max_frames, uint32_t nframes)
{
	struct midi_buffer *mb = data;
	mb->magic = MIDI_BUFFER_MAGIC;
	mb->buffer_size = max_frames * sizeof(float);
	mb->nframes = nframes;
	mb->write_pos = 0;
	mb->event_count = 0;
	mb->lost_events = 0;
}

static inline void *init_buffer(struct port *p, uint32_t nframes)
{
	struct client *c = p->client;
	void *data = p->emptyptr;
	if (p->zeroed)
		return data;

	if (TYPE_ID_IS_EVENT(p->object->port.type_id)) {
		struct midi_buffer *mb = data;
		midi_init_buffer(data, c->max_frames, nframes);
		pw_log_debug("port %p: init midi buffer size:%d frames:%d", p,
				mb->buffer_size, nframes);
	} else
		memset(data, 0, c->max_frames * sizeof(float));

	p->zeroed = true;
	return data;
}

static int client_node_port_use_buffers(void *data,
                                  enum spa_direction direction,
                                  uint32_t port_id,
                                  uint32_t mix_id,
                                  uint32_t flags,
                                  uint32_t n_buffers,
                                  struct pw_client_node_buffer *buffers)
{
	struct client *c = (struct client *) data;
	struct port *p = GET_PORT(c, direction, port_id);
	struct buffer *b;
	uint32_t i, j, fl;
	int res;
	struct mix *mix;

	if (p == NULL || !p->valid) {
		res = -EINVAL;
		goto done;
	}
	if ((mix = find_mix(c, p, mix_id)) == NULL) {
		res = -ENOMEM;
		goto done;
	}

	pw_log_debug("%p: port %p %d %d.%d use_buffers %d", c, p, direction,
			port_id, mix_id, n_buffers);

	if (n_buffers > MAX_BUFFERS) {
		pw_log_error("%p: too many buffers %u > %u", c, n_buffers, MAX_BUFFERS);
		res = -ENOSPC;
		goto done;
	}

	fl = PW_MEMMAP_FLAG_READ;
	/* Make the buffer writable when output. Some apps write to the input buffer
	 * so we want to make them writable as well if the option is selected.
	 * We can't use a PRIVATE mapping here because then we might not see changes
	 * in the buffer by other apps (see mmap man page). */
	if (direction == SPA_DIRECTION_OUTPUT ||
	    (p->object->port.type_id != TYPE_ID_VIDEO && c->writable_input))
		fl |= PW_MEMMAP_FLAG_WRITE;

	/* clear previous buffers */
	clear_buffers(c, mix);

	for (i = 0; i < n_buffers; i++) {
		off_t offset;
		struct spa_buffer *buf;
		struct pw_memmap *mm;

		mm = pw_mempool_map_id(c->pool, buffers[i].mem_id,
				fl, buffers[i].offset, buffers[i].size, NULL);
		if (mm == NULL) {
			pw_log_warn("%p: can't map memory id %u: %m", c, buffers[i].mem_id);
			continue;
		}

		buf = buffers[i].buffer;

		b = &mix->buffers[i];
		b->id = i;
		b->flags = 0;
		b->n_mem = 0;
		b->mem[b->n_mem++] = mm;

		pw_log_debug("%p: add buffer id:%u offset:%u size:%u map:%p ptr:%p",
				c, buffers[i].mem_id, buffers[i].offset,
				buffers[i].size, mm, mm->ptr);

		offset = 0;
		for (j = 0; j < buf->n_metas; j++) {
			struct spa_meta *m = &buf->metas[j];
			offset += SPA_ROUND_UP_N(m->size, 8);
		}

		b->n_datas = SPA_MIN(buf->n_datas, MAX_BUFFER_DATAS);

		for (j = 0; j < b->n_datas; j++) {
			struct spa_data *d = &b->datas[j];

			memcpy(d, &buf->datas[j], sizeof(struct spa_data));
			d->chunk =
			    SPA_PTROFF(mm->ptr, offset + sizeof(struct spa_chunk) * j,
				       struct spa_chunk);

			if (d->type == SPA_DATA_MemId) {
				uint32_t mem_id = SPA_PTR_TO_UINT32(d->data);
				struct pw_memblock *bm;
				struct pw_memmap *bmm;

				bm = pw_mempool_find_id(c->pool, mem_id);
				if (bm == NULL) {
					pw_log_error("%p: unknown buffer mem %u", c, mem_id);
					res = -ENODEV;
					goto done;

				}

				d->fd = bm->fd;
				d->type = bm->type;
				d->data = NULL;

				bmm = pw_memblock_map(bm, fl, d->mapoffset, d->maxsize, NULL);
				if (bmm == NULL) {
					res = -errno;
					pw_log_error("%p: failed to map buffer mem %m", c);
					d->data = NULL;
					goto done;
				}
				b->mem[b->n_mem++] = bmm;
				d->data = bmm->ptr;

				pw_log_debug("%p: data %d %u -> fd %d %d",
						c, j, bm->id, bm->fd, d->maxsize);
			} else if (d->type == SPA_DATA_MemPtr) {
				int offs = SPA_PTR_TO_INT(d->data);
				d->data = SPA_PTROFF(mm->ptr, offs, void);
				d->fd = -1;
				pw_log_debug("%p: data %d %u -> mem %p %d",
						c, j, b->id, d->data, d->maxsize);
			} else {
				pw_log_warn("unknown buffer data type %d", d->type);
			}
			if (c->allow_mlock && mlock(d->data, d->maxsize) < 0) {
				if (errno != ENOMEM  || !mlock_warned) {
					pw_log(c->warn_mlock ? SPA_LOG_LEVEL_WARN : SPA_LOG_LEVEL_DEBUG,
						"%p: Failed to mlock memory %p %u: %s", c,
						d->data, d->maxsize,
						errno == ENOMEM ?
						"This is not a problem but for best performance, "
						"consider increasing RLIMIT_MEMLOCK" : strerror(errno));
					mlock_warned |= errno == ENOMEM;
				}
			}
		}
		SPA_FLAG_SET(b->flags, BUFFER_FLAG_OUT);
		if (direction == SPA_DIRECTION_OUTPUT)
			queue_buffer(c, mix, b->id);

	}
	pw_log_debug("%p: have %d buffers", c, n_buffers);
	mix->n_buffers = n_buffers;
	res = 0;

done:
	if (res < 0)
		pw_proxy_errorf((struct pw_proxy*)c->node, res,
				"port_use_buffers(%u:%u:%u): %s", direction, port_id,
				mix_id, spa_strerror(res));
	return res;
}

static int client_node_port_set_io(void *data,
                             enum spa_direction direction,
                             uint32_t port_id,
                             uint32_t mix_id,
                             uint32_t id,
                             uint32_t mem_id,
                             uint32_t offset,
                             uint32_t size)
{
	struct client *c = (struct client *) data;
	struct port *p = GET_PORT(c, direction, port_id);
        struct pw_memmap *mm, *old;
        struct mix *mix;
	uint32_t tag[5] = { c->node_id, direction, port_id, mix_id, id };
        void *ptr;
	int res = 0;

	if (p == NULL || !p->valid) {
		res = -EINVAL;
		goto exit;
	}

	if ((mix = find_mix(c, p, mix_id)) == NULL) {
		res = -ENOMEM;
		goto exit;
	}

	old = pw_mempool_find_tag(c->pool, tag, sizeof(tag));

        if (mem_id == SPA_ID_INVALID) {
                mm = ptr = NULL;
		size = 0;
        } else {
		mm = pw_mempool_map_id(c->pool, mem_id,
				PW_MEMMAP_FLAG_READWRITE, offset, size, tag);
                if (mm == NULL) {
                        pw_log_warn("%p: can't map memory id %u: %m", c, mem_id);
			res = -EINVAL;
                        goto exit_free;
                }
		ptr = mm->ptr;
        }

	pw_log_debug("%p: port %p mix:%d set io:%s id:%u ptr:%p", c, p, mix_id,
			spa_debug_type_find_name(spa_type_io, id), id, ptr);

	switch (id) {
	case SPA_IO_Buffers:
	case SPA_IO_AsyncBuffers:
		mix_set_io(mix, ptr, size);
		queue_memmap_free(c, old);
		old = NULL;
		break;
	default:
		break;
	}

exit_free:
	pw_memmap_free(old);
exit:
	if (res < 0)
		pw_proxy_errorf((struct pw_proxy*)c->node, res,
				"port_set_io(%u:%u:%u %u): %s", direction, port_id,
				mix_id, id, spa_strerror(res));
	return res;
}

static int
do_add_link(struct spa_loop *loop,
                bool async, uint32_t seq, const void *data, size_t size, void *user_data)
{
	struct link *link = user_data;
	struct client *c = link->client;
	pw_log_trace("link %p", link);
	spa_list_append(&c->rt.target_links, &link->target_link);
	return 0;
}

static int
do_remove_link(struct spa_loop *loop,
                bool async, uint32_t seq, const void *data, size_t size, void *user_data)
{
	struct link *link = user_data;
	struct client *c = link->client;

	pw_log_trace("link %p", link);
	spa_list_remove(&link->target_link);

	if (c->rt.prepared) {
		int old_state = SPA_ATOMIC_LOAD(c->activation->status);
		uint64_t trigger = 0;
		if (old_state != PW_NODE_ACTIVATION_FINISHED)
			trigger = get_time_ns(c->l->system);
		deactivate_link(c, link, trigger);
	}
	return 0;
}

static int
do_free_link(struct spa_loop *loop,
                bool async, uint32_t seq, const void *data, size_t size, void *user_data)
{
	struct client *c = user_data;
	struct link *l = *((struct link **)data);
	free_link(l);
	pw_core_set_paused(c->core, false);
	return 0;
}

static int
do_queue_free_link(struct spa_loop *loop,
                bool async, uint32_t seq, const void *data, size_t size, void *user_data)
{
	struct client *c = user_data;
	pw_loop_invoke(c->context.l, do_free_link, 0, data, size, false, c);
	return 0;
}

static void queue_free_link(struct client *c, struct link *l)
{
	pw_core_set_paused(c->core, true);
	pw_data_loop_invoke(c->loop,
		do_queue_free_link, SPA_ID_INVALID, &l, sizeof(&l), false, c);
}

static int client_node_set_activation(void *data,
                             uint32_t node_id,
                             int signalfd,
                             uint32_t mem_id,
                             uint32_t offset,
                             uint32_t size)
{
	struct client *c = (struct client *) data;
	struct pw_memmap *mm;
	struct link *link;
	void *ptr;
	int res = 0;

	if (mem_id == SPA_ID_INVALID) {
		mm = ptr = NULL;
		size = 0;
	}
	else {
		mm = pw_mempool_map_id(c->pool, mem_id,
				PW_MEMMAP_FLAG_READWRITE, offset, size, NULL);
		if (mm == NULL) {
			pw_log_warn("%p: can't map memory id %u: %m", c, mem_id);
			res = -EINVAL;
			goto exit;
		}
		ptr = mm->ptr;
	}

	if (c->node_id == node_id) {
		pw_log_debug("%p: our activation %u: %u %u %u %p", c, node_id,
				mem_id, offset, size, ptr);
	} else {
		pw_log_debug("%p: set activation %u: %u %u %u %p", c, node_id,
				mem_id, offset, size, ptr);
	}

	if (ptr) {
		link = calloc(1, sizeof(struct link));
		if (link == NULL) {
			res = -errno;
			goto exit;
		}
		link->client = c;
		link->node_id = node_id;
		link->mem = mm;
		link->activation = ptr;
		link->signalfd = signalfd;
		link->trigger = link->activation->server_version < 1 ? trigger_link_v0 : trigger_link_v1;
		spa_list_append(&c->links, &link->link);

		pw_data_loop_invoke(c->loop,
                       do_add_link, SPA_ID_INVALID, NULL, 0, false, link);
	}
	else {
		link = find_activation(&c->links, node_id);
		if (link == NULL) {
			res = -EINVAL;
			goto exit;
		}
		spa_list_remove(&link->link);

		pw_data_loop_invoke(c->loop,
                       do_remove_link, SPA_ID_INVALID, NULL, 0, false, link);
		queue_free_link(c, link);
	}

	if (c->driver_id == node_id)
		update_driver_activation(c);

      exit:
	if (res < 0)
		pw_proxy_errorf((struct pw_proxy*)c->node, res,
				"set_activation(%u): %s", node_id, spa_strerror(res));
	return res;
}

static int client_node_port_set_mix_info(void *data,
                                  enum spa_direction direction,
                                  uint32_t port_id,
                                  uint32_t mix_id,
                                  uint32_t peer_id,
                                  const struct spa_dict *props)
{
	struct client *c = (struct client *) data;
	struct port *p = GET_PORT(c, direction, port_id);
	struct mix *mix;
	int res = 0;

	if (p == NULL || !p->valid) {
		res = peer_id == SPA_ID_INVALID ? 0 : -EINVAL;
		goto exit;
	}

	mix = find_mix(c, p, mix_id);

	pw_log_debug("%p: port %p mix:%d peer_id:%u info:%p", c, p, mix_id,
			peer_id, props);

	if (peer_id == SPA_ID_INVALID) {
		if (mix == NULL) {
			res = -ENOENT;
			goto exit;
		}
		free_mix(c, mix);
	} else {
		if (mix != NULL) {
			res = -EEXIST;
			goto exit;
		}
		mix = create_mix(c, p, mix_id, peer_id);
	}
exit:
	if (res < 0)
		pw_proxy_errorf((struct pw_proxy*)c->node, res,
				"set_mix_info(%u:%u:%u %u): %s", direction, port_id,
				mix_id, peer_id, spa_strerror(res));
	return res;
}

static const struct pw_client_node_events client_node_events = {
	PW_VERSION_CLIENT_NODE_EVENTS,
	.transport = client_node_transport,
	.set_param = client_node_set_param,
	.set_io = client_node_set_io,
	.event = client_node_event,
	.command = client_node_command,
	.add_port = client_node_add_port,
	.remove_port = client_node_remove_port,
	.port_set_param = client_node_port_set_param,
	.port_use_buffers = client_node_port_use_buffers,
	.port_set_io = client_node_port_set_io,
	.set_activation = client_node_set_activation,
	.port_set_mix_info = client_node_port_set_mix_info,
};

#define CHECK(expression,label)						\
do {									\
	if ((errno = expression) != 0) {				\
		res = -errno;						\
		pw_log_error(#expression ": %s", strerror(errno));	\
		goto label;						\
	}								\
} while(false);

static struct spa_thread *impl_create(void *object,
			const struct spa_dict *props,
			void *(*start)(void*), void *arg)
{
	struct client *c = (struct client *) object;
	struct spa_dict_item *items;
	struct spa_dict copy;
	char creator_ptr[64];

	pw_log_info("create thread");
	if (globals.creator != NULL) {
		uint32_t i, n_items = props ? props->n_items : 0;

		items = alloca((n_items + 1) * sizeof(*items));

		for (i = 0; i < n_items; i++)
			items[i] = props->items[i];

		snprintf(creator_ptr, sizeof(creator_ptr), "pointer:%p", globals.creator);
		items[n_items++] = SPA_DICT_ITEM_INIT(SPA_KEY_THREAD_CREATOR,
				creator_ptr);

		copy = SPA_DICT_INIT(items, n_items);
		props = &copy;
	}
	return spa_thread_utils_create(c->context.old_thread_utils, props, start, arg);
}

static int impl_join(void *object,
		struct spa_thread *thread, void **retval)
{
	struct client *c = (struct client *) object;
	pw_log_info("join thread");
	return spa_thread_utils_join(c->context.old_thread_utils, thread, retval);
}

static int impl_acquire_rt(void *object, struct spa_thread *thread, int priority)
{
	struct client *c = (struct client *) object;
	return spa_thread_utils_acquire_rt(c->context.old_thread_utils, thread, priority);
}

static int impl_drop_rt(void *object, struct spa_thread *thread)
{
	struct client *c = (struct client *) object;
	return spa_thread_utils_drop_rt(c->context.old_thread_utils, thread);
}

static struct spa_thread_utils_methods thread_utils_impl = {
	SPA_VERSION_THREAD_UTILS_METHODS,
	.create = impl_create,
	.join = impl_join,
	.acquire_rt = impl_acquire_rt,
	.drop_rt = impl_drop_rt,
};

static jack_port_type_id_t string_to_type(const char *port_type)
{
	if (spa_streq(JACK_DEFAULT_AUDIO_TYPE, port_type))
		return TYPE_ID_AUDIO;
	else if (spa_streq(JACK_DEFAULT_VIDEO_TYPE, port_type))
		return TYPE_ID_VIDEO;
	else if (spa_streq(JACK_DEFAULT_MIDI_TYPE, port_type))
		return TYPE_ID_MIDI;
	else if (spa_streq(JACK_DEFAULT_OSC_TYPE, port_type))
		return TYPE_ID_OSC;
	else if (spa_streq(JACK_DEFAULT_UMP_TYPE, port_type))
		return TYPE_ID_UMP;
	else if (spa_streq("other", port_type))
		return TYPE_ID_OTHER;
	else
		return SPA_ID_INVALID;
}

static const char* type_to_string(jack_port_type_id_t type_id)
{
	switch(type_id) {
	case TYPE_ID_AUDIO:
		return JACK_DEFAULT_AUDIO_TYPE;
	case TYPE_ID_VIDEO:
		return JACK_DEFAULT_VIDEO_TYPE;
	case TYPE_ID_MIDI:
	case TYPE_ID_OSC:
	case TYPE_ID_UMP:
		/* all returned as MIDI */
		return JACK_DEFAULT_MIDI_TYPE;
	case TYPE_ID_OTHER:
		return "other";
	default:
		return NULL;
	}
}

static const char* type_to_format_dsp(jack_port_type_id_t type_id)
{
	switch(type_id) {
	case TYPE_ID_AUDIO:
		return JACK_DEFAULT_AUDIO_TYPE;
	case TYPE_ID_VIDEO:
		return JACK_DEFAULT_VIDEO_TYPE;
	case TYPE_ID_OSC:
		return JACK_DEFAULT_OSC_TYPE;
	case TYPE_ID_MIDI:
		return JACK_DEFAULT_MIDI_TYPE;
	case TYPE_ID_UMP:
		return JACK_DEFAULT_UMP_TYPE;
	default:
		return NULL;
	}
}

static bool type_is_dsp(jack_port_type_id_t type_id)
{
	switch(type_id) {
	case TYPE_ID_AUDIO:
	case TYPE_ID_MIDI:
	case TYPE_ID_VIDEO:
	case TYPE_ID_OSC:
	case TYPE_ID_UMP:
		return true;
	default:
		return false;
	}
}

static jack_uuid_t client_make_uuid(uint32_t id, bool monitor)
{
	jack_uuid_t uuid = 0x2; /* JackUUIDClient */
	uuid = (uuid << 32) | (id + 1);
	if (monitor)
		uuid |= (1 << 30);
	pw_log_debug("uuid %d -> %"PRIu64, id, uuid);
	return uuid;
}

static int metadata_property(void *data, uint32_t id,
		const char *key, const char *type, const char *value)
{
	struct client *c = (struct client *) data;
	struct object *o;
	jack_uuid_t uuid;

	pw_log_debug("set id:%u key:'%s' value:'%s' type:'%s'", id, key, value, type);

	if (id == PW_ID_CORE) {
		if (key == NULL || spa_streq(key, "default.audio.sink")) {
			if (value != NULL) {
				if (spa_json_str_object_find(value, strlen(value), "name",
						c->metadata->default_audio_sink,
						sizeof(c->metadata->default_audio_sink)) < 0)
					value = NULL;
			}
			if (value == NULL)
				c->metadata->default_audio_sink[0] = '\0';
		}
		if (key == NULL || spa_streq(key, "default.audio.source")) {
			if (value != NULL) {
				if (spa_json_str_object_find(value, strlen(value), "name",
						c->metadata->default_audio_source,
						sizeof(c->metadata->default_audio_source)) < 0)
					value = NULL;
			}
			if (value == NULL)
				c->metadata->default_audio_source[0] = '\0';
		}
	} else {
		if ((o = find_id(c, id, true)) == NULL)
			return -EINVAL;

		switch (o->type) {
		case INTERFACE_Node:
			uuid = client_make_uuid(o->serial, false);
			break;
		case INTERFACE_Port:
			uuid = jack_port_uuid_generate(o->serial);
			break;
		default:
			return -EINVAL;
		}
		update_property(c, uuid, key, type, value);
	}

	return 0;
}

static const struct pw_metadata_events metadata_events = {
	PW_VERSION_METADATA_EVENTS,
	.property = metadata_property
};

static void metadata_proxy_removed(void *data)
{
	struct client *c = data;
	pw_proxy_destroy((struct pw_proxy*)c->metadata->proxy);
}

static void metadata_proxy_destroy(void *data)
{
	struct client *c = data;
	spa_hook_remove(&c->metadata->proxy_listener);
	spa_hook_remove(&c->metadata->listener);
	c->metadata = NULL;
}

static const struct pw_proxy_events metadata_proxy_events = {
	PW_VERSION_PROXY_EVENTS,
	.removed = metadata_proxy_removed,
	.destroy = metadata_proxy_destroy,
};

static void settings_proxy_removed(void *data)
{
	struct client *c = data;
	pw_proxy_destroy((struct pw_proxy*)c->settings->proxy);
}

static void settings_proxy_destroy(void *data)
{
	struct client *c = data;
	spa_hook_remove(&c->settings->proxy_listener);
	c->settings = NULL;
}

static const struct pw_proxy_events settings_proxy_events = {
	PW_VERSION_PROXY_EVENTS,
	.removed = settings_proxy_removed,
	.destroy = settings_proxy_destroy,
};
static void proxy_removed(void *data)
{
	struct object *o = data;
	pw_proxy_destroy(o->proxy);
}

static void proxy_destroy(void *data)
{
	struct object *o = data;
	spa_hook_remove(&o->proxy_listener);
	spa_hook_remove(&o->object_listener);
	o->proxy = NULL;
}

static const struct pw_proxy_events proxy_events = {
	PW_VERSION_PROXY_EVENTS,
	.removed = proxy_removed,
	.destroy = proxy_destroy,
};

static bool node_is_active(struct client *c, struct object *n)
{
	return !n->node.is_jack ||
		(c->node_id == n->id ? c->active : n->node.is_running);
}

static void node_info(void *data, const struct pw_node_info *info)
{
	struct object *n = data;
	struct client *c = n->client;
	bool active;

	if (info->change_mask & PW_NODE_CHANGE_MASK_PROPS) {
		/* JACK clients always need ALWAYS_PROCESS=true or else they don't
		 * conform to the JACK API. We would try to hide the ports of
		 * PAUSED JACK clients, for example, even if they are active. */
		const char *str = spa_dict_lookup(info->props, PW_KEY_NODE_ALWAYS_PROCESS);
		n->node.is_jack = str ? spa_atob(str) : false;
	}

	n->node.is_running = info->state == PW_NODE_STATE_RUNNING;
	active = node_is_active(c, n);

	pw_log_debug("DSP node %d %08"PRIx64" jack:%u state change %s running:%d", info->id,
			info->change_mask, n->node.is_jack,
			pw_node_state_as_string(info->state), n->node.is_running);

	if (info->change_mask & PW_NODE_CHANGE_MASK_STATE) {
		struct object *p, *l;
		spa_list_for_each(p, &c->context.objects, link) {
			if (p->type != INTERFACE_Port || p->removed ||
			    p->port.node_id != info->id)
				continue;
			if (active)
				queue_notify(c, NOTIFY_TYPE_PORTREGISTRATION, p, 1, NULL);
			else {
				spa_list_for_each(l, &c->context.objects, link) {
					if (l->type != INTERFACE_Link || l->removed ||
					    (l->port_link.src_serial != p->serial &&
					     l->port_link.dst_serial != p->serial))
						continue;
					queue_notify(c, NOTIFY_TYPE_CONNECT, l, 0, NULL);
				}
				queue_notify(c, NOTIFY_TYPE_PORTREGISTRATION, p, 0, NULL);
			}
		}
	}
}

static const struct pw_node_events node_events = {
	PW_VERSION_NODE_EVENTS,
	.info = node_info,
};

static void port_param(void *data, int seq,
			uint32_t id, uint32_t index, uint32_t next,
			const struct spa_pod *param)
{
	struct object *o = data;

	switch (id) {
	case SPA_PARAM_Latency:
	{
		struct spa_latency_info info;
		if (spa_latency_parse(param, &info) < 0)
			return;
		o->port.latency[info.direction] = info;
		break;
	}
	default:
		break;
	}
}

static const struct pw_port_events port_events = {
	PW_VERSION_PORT_EVENTS,
	.param = port_param,
};

#define FILTER_NAME	" ()[].:*$"
#define FILTER_PORT	" ()[].*$"

static void filter_name(char *str, const char *filter, char filter_char)
{
	char *p;
	for (p = str; *p; p++) {
		if (strchr(filter, *p) != NULL)
			*p = filter_char;
	}
}

static void registry_event_global(void *data, uint32_t id,
                                  uint32_t permissions, const char *type, uint32_t version,
                                  const struct spa_dict *props)
{
	struct client *c = (struct client *) data;
	struct object *o, *ot, *op;
	const char *str;
	bool do_emit = true, do_sync = false;
	uint32_t serial;
	const char *app;

	if (props == NULL)
		return;

	str = spa_dict_lookup(props, PW_KEY_OBJECT_SERIAL);
	if (!spa_atou32(str, &serial, 0))
		serial = SPA_ID_INVALID;

	pw_log_debug("new %s id:%u serial:%u", type, id, serial);

	if (spa_streq(type, PW_TYPE_INTERFACE_Client)) {
		app = spa_dict_lookup(props, PW_KEY_APP_NAME);

		if ((str = spa_dict_lookup(props, PW_KEY_SEC_PID)) != NULL) {
			pw_log_debug("%p: pid of \"%s\" is \"%s\"", c, app, str);
		} else {
			pw_log_debug("%p: pid of \"%s\" is unknown", c, app);
		}

		o = alloc_object(c, INTERFACE_Client);
		if (o == NULL)
			goto exit;

		o->pwclient.pid = (int32_t)atoi(str);
		snprintf(o->pwclient.name, sizeof(o->pwclient.name), "%s", app);

		pw_log_debug("%p: add pw client %d (%s) pid %llu", c, id, app, (unsigned long long)o->pwclient.pid);

		pthread_mutex_lock(&c->context.lock);
		spa_list_append(&c->context.objects, &o->link);
		pthread_mutex_unlock(&c->context.lock);
	}
	else if (spa_streq(type, PW_TYPE_INTERFACE_Node)) {
		const char *node_name;
		char tmp[JACK_CLIENT_NAME_SIZE+1];

		o = alloc_object(c, INTERFACE_Node);
		if (o == NULL)
			goto exit;

		if ((str = spa_dict_lookup(props, PW_KEY_CLIENT_ID)) != NULL)
			o->node.client_id = atoi(str);

		node_name = spa_dict_lookup(props, PW_KEY_NODE_NAME);

		snprintf(o->node.node_name, sizeof(o->node.node_name),
				"%s", node_name);

		app = spa_dict_lookup(props, PW_KEY_APP_NAME);

		if (c->short_name) {
			str = spa_dict_lookup(props, PW_KEY_NODE_NICK);
			if (str == NULL)
				str = spa_dict_lookup(props, PW_KEY_NODE_DESCRIPTION);
		} else {
			str = spa_dict_lookup(props, PW_KEY_NODE_DESCRIPTION);
			if (str == NULL)
				str = spa_dict_lookup(props, PW_KEY_NODE_NICK);
		}
		if (str == NULL)
			str = node_name;
		if (str == NULL)
			str = spa_dict_lookup(props, PW_KEY_OBJECT_PATH);
		if (str == NULL)
			str = "node";

		if (app && !spa_streq(app, str))
			snprintf(tmp, sizeof(tmp), "%s/%s", app, str);
		else
			snprintf(tmp, sizeof(tmp), "%s", str);

		if (c->filter_name)
			filter_name(tmp, FILTER_NAME, c->filter_char);

		ot = find_node(c, tmp);
		if (ot != NULL && o->node.client_id != ot->node.client_id) {
			snprintf(o->node.name, sizeof(o->node.name), "%.*s-%d",
					(int)(sizeof(tmp)-11), tmp, id);
		} else {
			do_emit = ot == NULL;
			snprintf(o->node.name, sizeof(o->node.name), "%s", tmp);
		}
		if (id == c->node_id) {
			pw_log_debug("%p: add our node %d", c, id);
			snprintf(c->name, sizeof(c->name), "%s", o->node.name);
			c->object = o;
			c->serial = serial;
		}

		if ((str = spa_dict_lookup(props, PW_KEY_PRIORITY_SESSION)) != NULL)
			o->node.priority = pw_properties_parse_int(str);
		if ((str = spa_dict_lookup(props, PW_KEY_CLIENT_API)) != NULL)
			o->node.is_jack = spa_streq(str, "jack");

		pw_log_debug("%p: add node %d", c, id);

		if (o->node.is_jack) {
			o->proxy = pw_registry_bind(c->registry,
				id, type, PW_VERSION_NODE, 0);
			if (o->proxy) {
				pw_proxy_add_listener(o->proxy,
						&o->proxy_listener, &proxy_events, o);
				pw_proxy_add_object_listener(o->proxy,
						&o->object_listener, &node_events, o);
				do_sync = true;
			}
		}
		pthread_mutex_lock(&c->context.lock);
		spa_list_append(&c->context.objects, &o->link);
		pthread_mutex_unlock(&c->context.lock);
	}
	else if (spa_streq(type, PW_TYPE_INTERFACE_Port)) {
		const struct spa_dict_item *item;
		unsigned long flags = 0;
		jack_port_type_id_t type_id;
		uint32_t node_id;
		bool is_monitor = false;
		char tmp[REAL_JACK_PORT_NAME_SIZE+1];

		if ((str = spa_dict_lookup(props, PW_KEY_FORMAT_DSP)) == NULL)
			str = "other";
		if ((type_id = string_to_type(str)) == SPA_ID_INVALID)
			goto exit;

		if ((str = spa_dict_lookup(props, PW_KEY_NODE_ID)) == NULL)
			goto exit;

		node_id = atoi(str);

		if ((str = spa_dict_lookup(props, PW_KEY_PORT_EXTRA)) != NULL &&
		    spa_strstartswith(str, "jack:flags:"))
			flags = atoi(str+11);

		if ((str = spa_dict_lookup(props, PW_KEY_PORT_NAME)) == NULL)
			goto exit;

		if (type_id == TYPE_ID_UMP && c->flag_midi2)
			flags |= JackPortIsMIDI2;

		spa_dict_for_each(item, props) {
			if (spa_streq(item->key, PW_KEY_PORT_DIRECTION)) {
				if (spa_streq(item->value, "in"))
					flags |= JackPortIsInput;
				else if (spa_streq(item->value, "out"))
					flags |= JackPortIsOutput;
			}
			else if (spa_streq(item->key, PW_KEY_PORT_PHYSICAL)) {
				if (pw_properties_parse_bool(item->value))
					flags |= JackPortIsPhysical;
			}
			else if (spa_streq(item->key, PW_KEY_PORT_TERMINAL)) {
				if (pw_properties_parse_bool(item->value))
					flags |= JackPortIsTerminal;
			}
			else if (spa_streq(item->key, PW_KEY_PORT_CONTROL)) {
				if (pw_properties_parse_bool(item->value))
					type_id = TYPE_ID_MIDI;
			}
			else if (spa_streq(item->key, PW_KEY_PORT_MONITOR)) {
				is_monitor = pw_properties_parse_bool(item->value);
			}
		}
		if (is_monitor && !c->show_monitor)
			goto exit;
		if (TYPE_ID_IS_EVENT(type_id) && !c->show_midi)
			goto exit;

		o = NULL;
		if (node_id == c->node_id) {
			snprintf(tmp, sizeof(tmp), "%s:%s", c->name, str);
			o = find_port_by_name(c, tmp);
			if (o != NULL)
				pw_log_info("%p: %s found our port %p", c, tmp, o);
		}
		if (o == NULL) {
			if ((ot = find_type(c, node_id, INTERFACE_Node, true)) == NULL)
				goto exit;

			o = alloc_object(c, INTERFACE_Port);
			if (o == NULL)
				goto exit;

			o->port.system_id = 0;
			o->port.priority = ot->node.priority;
			o->port.node = ot;
			o->port.latency[SPA_DIRECTION_INPUT] = SPA_LATENCY_INFO(SPA_DIRECTION_INPUT);
			o->port.latency[SPA_DIRECTION_OUTPUT] = SPA_LATENCY_INFO(SPA_DIRECTION_OUTPUT);

			do_emit = node_is_active(c, ot);

			o->proxy = pw_registry_bind(c->registry,
				id, type, PW_VERSION_PORT, 0);
			if (o->proxy) {
				uint32_t ids[1] = { SPA_PARAM_Latency };

				pw_proxy_add_listener(o->proxy,
						&o->proxy_listener, &proxy_events, o);
				pw_proxy_add_object_listener(o->proxy,
						&o->object_listener, &port_events, o);

				if (type_is_dsp(type_id))
					pw_port_subscribe_params((struct pw_port*)o->proxy,
							ids, 1);
				do_sync = true;
			}
			pthread_mutex_lock(&c->context.lock);
			spa_list_append(&c->context.objects, &o->link);
			pthread_mutex_unlock(&c->context.lock);

			if (is_monitor && !c->merge_monitor)
				snprintf(tmp, sizeof(tmp), "%.*s%s:%s",
					(int)(JACK_CLIENT_NAME_SIZE-(sizeof(MONITOR_EXT)-1)),
					ot->node.name, MONITOR_EXT, str);
			else
				snprintf(tmp, sizeof(tmp), "%s:%s", ot->node.name, str);

			if (c->filter_name)
				filter_name(tmp, FILTER_PORT, c->filter_char);

			op = find_port_by_name(c, tmp);
			if (op != NULL)
				snprintf(o->port.name, sizeof(o->port.name), "%.*s-%u",
						(int)(sizeof(tmp)-11), tmp, serial);
			else
				snprintf(o->port.name, sizeof(o->port.name), "%s", tmp);

			o->port.type_id = type_id;
		}

		if (c->fill_aliases) {
			if ((str = spa_dict_lookup(props, PW_KEY_OBJECT_PATH)) != NULL)
				snprintf(o->port.alias1, sizeof(o->port.alias1), "%s", str);

			if ((str = spa_dict_lookup(props, PW_KEY_PORT_ALIAS)) != NULL)
				snprintf(o->port.alias2, sizeof(o->port.alias2), "%s", str);
		}

		if ((str = spa_dict_lookup(props, PW_KEY_PORT_ID)) != NULL) {
			o->port.system_id = atoi(str);
			snprintf(o->port.system, sizeof(o->port.system), "system:%s_%d",
					flags & JackPortIsInput ? "playback" :
					is_monitor ? "monitor" : "capture",
					o->port.system_id+1);
		}

		o->port.flags = flags;
		o->port.node_id = node_id;
		o->port.is_monitor = is_monitor;

		pw_log_debug("%p: %p add port %d name:%s %d", c, o, id,
				o->port.name, type_id);
	}
	else if (spa_streq(type, PW_TYPE_INTERFACE_Link)) {
		struct object *p;

		o = alloc_object(c, INTERFACE_Link);
		if (o == NULL)
			goto exit;

		pthread_mutex_lock(&c->context.lock);
		spa_list_append(&c->context.objects, &o->link);
		pthread_mutex_unlock(&c->context.lock);

		if ((str = spa_dict_lookup(props, PW_KEY_LINK_OUTPUT_PORT)) == NULL)
			goto exit_free;
		o->port_link.src = pw_properties_parse_int(str);

		if ((p = find_type(c, o->port_link.src, INTERFACE_Port, true)) == NULL)
			goto exit_free;
		o->port_link.src_serial = p->serial;

		o->port_link.src_ours = p->port.port != NULL &&
			p->port.port->client == c;
		if (o->port_link.src_ours)
			o->port_link.our_output = p->port.port;

		if ((str = spa_dict_lookup(props, PW_KEY_LINK_INPUT_PORT)) == NULL)
			goto exit_free;
		o->port_link.dst = pw_properties_parse_int(str);

		if ((p = find_type(c, o->port_link.dst, INTERFACE_Port, true)) == NULL)
			goto exit_free;
		o->port_link.dst_serial = p->serial;

		o->port_link.dst_ours = p->port.port != NULL &&
			p->port.port->client == c;
		if (o->port_link.dst_ours)
			o->port_link.our_input = p->port.port;

		if (o->port_link.our_input != NULL &&
		    o->port_link.our_output != NULL) {
			struct mix *mix;
			mix = find_port_peer(o->port_link.our_output, o->port_link.dst);
			if (mix != NULL)
				mix->peer_port = o->port_link.our_input;
			mix = find_port_peer(o->port_link.our_input, o->port_link.src);
			if (mix != NULL)
				mix->peer_port = o->port_link.our_output;
		}
		pw_log_debug("%p: add link %d %u/%u->%u/%u", c, id,
				o->port_link.src, o->port_link.src_serial,
				o->port_link.dst, o->port_link.dst_serial);
	}
	else if (spa_streq(type, PW_TYPE_INTERFACE_Metadata)) {
		struct pw_proxy *proxy;

		if (c->metadata != NULL)
			goto exit;
		if ((str = spa_dict_lookup(props, PW_KEY_METADATA_NAME)) == NULL)
			goto exit;

		if (spa_streq(str, "default")) {
			proxy = pw_registry_bind(c->registry,
					id, type, PW_VERSION_METADATA, sizeof(struct metadata));

			c->metadata = pw_proxy_get_user_data(proxy);
			c->metadata->proxy = (struct pw_metadata*)proxy;
			c->metadata->default_audio_sink[0] = '\0';
			c->metadata->default_audio_source[0] = '\0';

			pw_proxy_add_listener(proxy,
					&c->metadata->proxy_listener,
					&metadata_proxy_events, c);
			pw_metadata_add_listener(c->metadata->proxy,
					&c->metadata->listener,
					&metadata_events, c);
			do_sync = true;
		} else if (spa_streq(str, "settings")) {
			proxy = pw_registry_bind(c->registry,
					id, type, PW_VERSION_METADATA, sizeof(struct metadata));

			c->settings = pw_proxy_get_user_data(proxy);
			c->settings->proxy = (struct pw_metadata*)proxy;
			pw_proxy_add_listener(proxy,
					&c->settings->proxy_listener,
					&settings_proxy_events, c);
			do_sync = true;
		}
		goto exit;
	}
	else {
		goto exit;
	}

	o->id = id;
	o->serial = serial;

	switch (o->type) {
	case INTERFACE_Node:
		pw_log_info("%p: client added \"%s\" emit:%d", c, o->node.name, do_emit);
		if (do_emit)
			queue_notify(c, NOTIFY_TYPE_REGISTRATION, o, 1, NULL);
		break;

	case INTERFACE_Port:
		pw_log_info("%p: port added %u/%u \"%s\" emit:%d", c, o->id,
				o->serial, o->port.name, do_emit);
		if (do_emit)
			queue_notify(c, NOTIFY_TYPE_PORTREGISTRATION, o, 1, NULL);
		break;

	case INTERFACE_Link:
		pw_log_info("%p: link %u %u/%u -> %u/%u added", c,
				o->id, o->port_link.src, o->port_link.src_serial,
				o->port_link.dst, o->port_link.dst_serial);
		if (do_emit)
			queue_notify(c, NOTIFY_TYPE_CONNECT, o, 1, NULL);
		break;
	}

      exit:
	if (do_sync)
		c->pending_sync = pw_proxy_sync((struct pw_proxy*)c->core,
				c->pending_sync);
	return;
      exit_free:
	free_object(c, o);
	return;
}

static void registry_event_global_remove(void *data, uint32_t id)
{
	struct client *c = (struct client *) data;
	struct object *o;

	pw_log_debug("%p: removed: %u", c, id);

	if ((o = find_id(c, id, true)) == NULL)
		return;

	if (o->proxy) {
		pw_proxy_destroy(o->proxy);
		o->proxy = NULL;
	}
	o->removing = true;

	switch (o->type) {
	case INTERFACE_Client:
		free_object(c, o);
		break;
	case INTERFACE_Node:
		if (c->metadata) {
			if (spa_streq(o->node.node_name, c->metadata->default_audio_sink))
				c->metadata->default_audio_sink[0] = '\0';
			if (spa_streq(o->node.node_name, c->metadata->default_audio_source))
				c->metadata->default_audio_source[0] = '\0';
		}
		if (find_node(c, o->node.name) == NULL) {
			pw_log_info("%p: client %u removed \"%s\"", c, o->id, o->node.name);
			queue_notify(c, NOTIFY_TYPE_REGISTRATION, o, 0, NULL);
		} else {
			free_object(c, o);
		}
		break;
	case INTERFACE_Port:
		pw_log_info("%p: port %u/%u removed \"%s\"", c, o->id, o->serial, o->port.name);
		queue_notify(c, NOTIFY_TYPE_PORTREGISTRATION, o, 0, NULL);
		break;
	case INTERFACE_Link:
		if (find_type(c, o->port_link.src, INTERFACE_Port, true) != NULL &&
		    find_type(c, o->port_link.dst, INTERFACE_Port, true) != NULL) {
			pw_log_info("%p: link %u %u/%u -> %u/%u removed", c, o->id,
					o->port_link.src, o->port_link.src_serial,
					o->port_link.dst, o->port_link.dst_serial);
			queue_notify(c, NOTIFY_TYPE_CONNECT, o, 0, NULL);
		} else {
			pw_log_warn("unlink between unknown ports %d and %d",
					o->port_link.src, o->port_link.dst);
			free_object(c, o);
		}
		break;
	}

	return;
}

static const struct pw_registry_events registry_events = {
        PW_VERSION_REGISTRY_EVENTS,
        .global = registry_event_global,
        .global_remove = registry_event_global_remove,
};

static void varargs_parse (struct client *c, jack_options_t options, va_list ap)
{
	if ((options & JackServerName))
		c->server_name = va_arg(ap, char *);
	if ((options & JackLoadName))
		c->load_name = va_arg(ap, char *);
	if ((options & JackLoadInit))
		c->load_init = va_arg(ap, char *);
	if ((options & JackSessionID)) {
		char *sid = va_arg(ap, char *);
		if (sid) {
			const long long id = atoll(sid);
			if (id > 0)
				c->session_id = id;
		}
	}
}


static int execute_match(void *data, const char *location, const char *action,
		const char *val, size_t len)
{
	struct client *client = data;
	if (spa_streq(action, "update-props"))
		pw_properties_update_string(client->props, val, len);
	return 1;
}

static struct client * g_first_client;

SPA_EXPORT
jack_client_t * jack_client_open (const char *client_name,
                                  jack_options_t options,
                                  jack_status_t *status_ptr, ...)
{
	struct client *client;
	const struct spa_support *support;
	uint32_t n_support;
	const char *str;
	struct spa_cpu *cpu_iface;
	const struct pw_properties *props;
	va_list ap;
        jack_status_t status;
        if (getenv("PIPEWIRE_NOJACK") != NULL ||
            getenv("PIPEWIRE_INTERNAL") != NULL ||
	    spa_strstartswith(pw_get_library_version(), "0.2"))
		goto disabled;

	return_val_if_fail(client_name != NULL, NULL);

	client = calloc(1, sizeof(struct client));
	if (client == NULL)
		goto disabled;

	pw_log_info("%p: open '%s' options:%d", client, client_name, options);

	va_start(ap, status_ptr);
	varargs_parse(client, options, ap);
	va_end(ap);

	snprintf(client->name, sizeof(client->name), "pw-%s", client_name);

	pthread_mutex_init(&client->context.lock, NULL);
	spa_list_init(&client->context.objects);

	client->node_id = SPA_ID_INVALID;

	client->buffer_frames = (uint32_t)-1;
	client->sample_rate = (uint32_t)-1;
	client->latency = SPA_FRACTION(-1, -1);

	spa_list_init(&client->mix);
	spa_list_init(&client->free_mix);

	spa_list_init(&client->free_ports);
	pw_map_init(&client->ports[SPA_DIRECTION_INPUT], 32, 32);
	pw_map_init(&client->ports[SPA_DIRECTION_OUTPUT], 32, 32);

	spa_list_init(&client->links);
	client->driver_id = SPA_ID_INVALID;

	spa_list_init(&client->rt.target_links);
	pthread_mutex_init(&client->rt_lock, NULL);

	if (client->server_name != NULL &&
	    spa_streq(client->server_name, "default"))
		client->server_name = NULL;

	client->props = pw_properties_new(
			PW_KEY_LOOP_CANCEL, "true",
			PW_KEY_REMOTE_NAME, client->server_name,
			PW_KEY_CLIENT_NAME, client_name,
			PW_KEY_CLIENT_API, "jack",
			PW_KEY_CONFIG_NAME, "jack.conf",
			NULL);
	if (client->props == NULL)
		goto no_props;

	client->context.loop = pw_thread_loop_new(client->name, NULL);
	if (client->context.loop == NULL)
		goto no_props;
	client->context.l = pw_thread_loop_get_loop(client->context.loop);
	client->context.context = pw_context_new(
			client->context.l,
			pw_properties_copy(client->props),
			0);
	if (client->context.context == NULL)
		goto no_props;

	client->context.notify = pw_thread_loop_new(client->name, NULL);
	if (client->context.notify == NULL)
		goto no_props;
	client->context.nl = pw_thread_loop_get_loop(client->context.notify);

	globals.max_frames = client->max_frames = client->context.context->settings.clock_quantum_limit;

	client->notify_source = pw_loop_add_event(client->context.nl,
			on_notify_event, client);
	client->notify_buffer = calloc(1, NOTIFY_BUFFER_SIZE + sizeof(struct notify));
	spa_ringbuffer_init(&client->notify_ring);

	pw_context_conf_update_props(client->context.context,
			"jack.properties", client->props);

	props = pw_context_get_properties(client->context.context);

	client->allow_mlock = pw_properties_get_bool(props, "mem.allow-mlock", true);
	client->warn_mlock = pw_properties_get_bool(props, "mem.warn-mlock", false);

	pw_context_conf_section_match_rules(client->context.context, "jack.rules",
			&props->dict, execute_match, client);

	support = pw_context_get_support(client->context.context, &n_support);

	client->mix_function = mix_c;
	cpu_iface = spa_support_find(support, n_support, SPA_TYPE_INTERFACE_CPU);
	if (cpu_iface) {
#if defined (__SSE__)
		uint32_t flags = spa_cpu_get_flags(cpu_iface);
		if (flags & SPA_CPU_FLAG_SSE)
			client->mix_function = mix_sse;
#endif
		client->max_align = spa_cpu_get_max_align(cpu_iface);
	} else {
		client->max_align = MAX_ALIGN;
	}
	client->context.old_thread_utils =
		pw_context_get_object(client->context.context,
				SPA_TYPE_INTERFACE_ThreadUtils);
	if (client->context.old_thread_utils == NULL)
		client->context.old_thread_utils = pw_thread_utils_get();

	globals.thread_utils = client->context.old_thread_utils;

	client->context.thread_utils.iface = SPA_INTERFACE_INIT(
			SPA_TYPE_INTERFACE_ThreadUtils,
			SPA_VERSION_THREAD_UTILS,
			&thread_utils_impl, client);

	client->loop = pw_context_get_data_loop(client->context.context);
	client->l = pw_data_loop_get_loop(client->loop);
	pw_data_loop_stop(client->loop);

	pw_context_set_object(client->context.context,
			SPA_TYPE_INTERFACE_ThreadUtils,
			&client->context.thread_utils);

	pw_thread_loop_start(client->context.loop);

	pw_thread_loop_lock(client->context.loop);

        client->core = pw_context_connect(client->context.context,
				pw_properties_copy(client->props), 0);
	if (client->core == NULL)
		goto server_failed;

	client->pool = pw_core_get_mempool(client->core);

	pw_core_add_listener(client->core,
			&client->core_listener,
			&core_events, client);
	client->registry = pw_core_get_registry(client->core,
			PW_VERSION_REGISTRY, 0);
	pw_registry_add_listener(client->registry,
			&client->registry_listener,
			&registry_events, client);

	if ((str = getenv("PIPEWIRE_PROPS")) != NULL)
		pw_properties_update_string(client->props, str, strlen(str));
	if ((str = getenv("PIPEWIRE_QUANTUM")) != NULL) {
		struct spa_fraction q;
		if (sscanf(str, "%u/%u", &q.num, &q.denom) == 2 && q.denom != 0) {
			pw_properties_setf(client->props, PW_KEY_NODE_FORCE_RATE,
					"%u", q.denom);
			pw_properties_setf(client->props, PW_KEY_NODE_FORCE_QUANTUM,
					"%u", q.num);
		} else {
			pw_log_warn("invalid PIPEWIRE_QUANTUM: %s", str);
		}
	}
	if ((str = getenv("PIPEWIRE_LATENCY")) != NULL)
		pw_properties_set(client->props, PW_KEY_NODE_LATENCY, str);
	if ((str = getenv("PIPEWIRE_RATE")) != NULL)
		pw_properties_set(client->props, PW_KEY_NODE_RATE, str);
	if ((str = getenv("PIPEWIRE_LINK_PASSIVE")) != NULL)
		pw_properties_set(client->props, "jack.passive-links", str);

	if ((str = pw_properties_get(client->props, PW_KEY_NODE_LATENCY)) != NULL) {
		uint32_t num, denom;
		if (sscanf(str, "%u/%u", &num, &denom) == 2 && denom != 0) {
			client->latency = SPA_FRACTION(num, denom);
		}
	}
	if (pw_properties_get(client->props, PW_KEY_NODE_NAME) == NULL)
		pw_properties_set(client->props, PW_KEY_NODE_NAME, client_name);
	if (pw_properties_get(client->props, PW_KEY_NODE_GROUP) == NULL)
		pw_properties_setf(client->props, PW_KEY_NODE_GROUP, "group.dsp.0");
	if (pw_properties_get(client->props, PW_KEY_NODE_DESCRIPTION) == NULL)
		pw_properties_set(client->props, PW_KEY_NODE_DESCRIPTION, client_name);
	if (pw_properties_get(client->props, PW_KEY_MEDIA_TYPE) == NULL)
		pw_properties_set(client->props, PW_KEY_MEDIA_TYPE, "Audio");
	if (pw_properties_get(client->props, PW_KEY_MEDIA_CATEGORY) == NULL)
		pw_properties_set(client->props, PW_KEY_MEDIA_CATEGORY, "Duplex");
	if (pw_properties_get(client->props, PW_KEY_MEDIA_ROLE) == NULL)
		pw_properties_set(client->props, PW_KEY_MEDIA_ROLE, "DSP");
	if (pw_properties_get(client->props, PW_KEY_NODE_ALWAYS_PROCESS) == NULL)
		pw_properties_set(client->props, PW_KEY_NODE_ALWAYS_PROCESS, "true");
	if (pw_properties_get(client->props, PW_KEY_NODE_LOCK_QUANTUM) == NULL)
		pw_properties_set(client->props, PW_KEY_NODE_LOCK_QUANTUM, "true");
	pw_properties_set(client->props, PW_KEY_NODE_TRANSPORT_SYNC, "true");

	client->node = pw_core_create_object(client->core,
				"client-node",
				PW_TYPE_INTERFACE_ClientNode,
				PW_VERSION_CLIENT_NODE,
				&client->props->dict,
				0);
	if (client->node == NULL)
		goto init_failed;

	pw_client_node_add_listener(client->node,
			&client->node_listener, &client_node_events, client);
        pw_proxy_add_listener((struct pw_proxy*)client->node,
			&client->proxy_listener, &node_proxy_events, client);

	client->info = SPA_NODE_INFO_INIT();
	client->info.max_input_ports = UINT32_MAX;
	client->info.max_output_ports = UINT32_MAX;
	client->info.change_mask = SPA_NODE_CHANGE_MASK_FLAGS |
		SPA_NODE_CHANGE_MASK_PROPS;
	client->info.flags = SPA_NODE_FLAG_RT;
	client->info.props = &client->props->dict;

	pw_client_node_update(client->node,
			PW_CLIENT_NODE_UPDATE_INFO,
			0, NULL, &client->info);
	client->info.change_mask = 0;

	client->show_monitor = pw_properties_get_bool(client->props, "jack.show-monitor", true);
	client->show_midi = pw_properties_get_bool(client->props, "jack.show-midi", true);
	client->merge_monitor = pw_properties_get_bool(client->props, "jack.merge-monitor", true);
	client->short_name = pw_properties_get_bool(client->props, "jack.short-name", false);
	client->filter_name = pw_properties_get_bool(client->props, "jack.filter-name", false);
	client->passive_links = pw_properties_get_bool(client->props, "jack.passive-links", false);
	client->filter_char = ' ';
	if ((str = pw_properties_get(client->props, "jack.filter-char")) != NULL && str[0] != '\0')
		client->filter_char = str[0];
	client->locked_process = pw_properties_get_bool(client->props, "jack.locked-process", true);
	client->default_as_system = pw_properties_get_bool(client->props, "jack.default-as-system", false);
	client->fix_midi_events = pw_properties_get_bool(client->props, "jack.fix-midi-events", true);
	client->global_buffer_size = pw_properties_get_bool(client->props, "jack.global-buffer-size", false);
	client->global_sample_rate = pw_properties_get_bool(client->props, "jack.global-sample-rate", false);
	client->max_ports = pw_properties_get_uint32(client->props, "jack.max-client-ports", MAX_CLIENT_PORTS);
	client->fill_aliases = pw_properties_get_bool(client->props, "jack.fill-aliases", false);
	client->writable_input = pw_properties_get_bool(client->props, "jack.writable-input", true);
	client->async = pw_properties_get_bool(client->props, PW_KEY_NODE_ASYNC, false);
	client->flag_midi2 = pw_properties_get_bool(client->props, "jack.flag-midi2", false);

	client->self_connect_mode = SELF_CONNECT_ALLOW;
	if ((str = pw_properties_get(client->props, "jack.self-connect-mode")) != NULL) {
		if (spa_streq(str, "fail-external"))
			client->self_connect_mode = SELF_CONNECT_FAIL_EXT;
		else if (spa_streq(str, "ignore-external"))
			client->self_connect_mode = SELF_CONNECT_IGNORE_EXT;
		else if (spa_streq(str, "fail-all"))
			client->self_connect_mode = SELF_CONNECT_FAIL_ALL;
		else if (spa_streq(str, "ignore-all"))
			client->self_connect_mode = SELF_CONNECT_IGNORE_ALL;
	}
	client->other_connect_mode = OTHER_CONNECT_ALLOW;
	if ((str = pw_properties_get(client->props, "jack.other-connect-mode")) != NULL) {
		if (spa_streq(str, "fail"))
			client->other_connect_mode = OTHER_CONNECT_FAIL;
		else if (spa_streq(str, "ignore"))
			client->other_connect_mode = OTHER_CONNECT_IGNORE;
	}

	client->rt_max = pw_properties_get_int32(client->props, "rt.prio", DEFAULT_RT_MAX);

	status = 0;
	if (status_ptr)
		*status_ptr = status;

	client->pending_sync = pw_proxy_sync((struct pw_proxy*)client->core, client->pending_sync);

	while (true) {
	        pw_thread_loop_wait(client->context.loop);

		if (client->last_res < 0)
			goto init_failed;

		if (client->pending_sync == client->last_sync)
			break;
	}

	if (!spa_streq(client->name, client_name)) {
		status |= JackNameNotUnique;
		if (status_ptr)
			*status_ptr = status;
		if (options & JackUseExactName)
			goto exit_unlock;
	}
	pw_thread_loop_unlock(client->context.loop);

	if (g_first_client == NULL)
		g_first_client = client;

	pw_thread_loop_start(client->context.notify);

	pw_log_info("%p: opened", client);
	return (jack_client_t *)client;

no_props:
	status = JackFailure | JackInitFailure;
	if (status_ptr)
		*status_ptr = status;
	goto exit;
init_failed:
	status = JackFailure | JackInitFailure;
	if (status_ptr)
		*status_ptr = status;
	goto exit_unlock;
server_failed:
	status = JackFailure | JackServerFailed;
	if (status_ptr)
		*status_ptr = status;
	goto exit_unlock;
exit_unlock:
	pw_thread_loop_unlock(client->context.loop);
exit:
	pw_log_info("%p: error %d", client, status);
	jack_client_close((jack_client_t *) client);
	return NULL;
disabled:
	pw_log_warn("JACK is disabled");
	status = JackFailure | JackInitFailure;
	if (status_ptr)
		*status_ptr = status;
	return NULL;
}

SPA_EXPORT
jack_client_t * jack_client_new (const char *client_name)
{
	jack_options_t options = JackUseExactName;
	jack_status_t status;

        if (getenv("JACK_START_SERVER") == NULL)
		options |= JackNoStartServer;

	return jack_client_open(client_name, options, &status, NULL);
}

SPA_EXPORT
int jack_client_close (jack_client_t *client)
{
	struct client *c = (struct client *) client;
	struct object *o;
	union pw_map_item *item;
	struct mix *m, *tm;
	struct port *p, *tp;
	int res;

	return_val_if_fail(c != NULL, -EINVAL);

	pw_log_info("%p: close", client);

	if (g_first_client == c)
		g_first_client = NULL;

	c->destroyed = true;

	res = jack_deactivate(client);

	clean_transport(c);

	if (c->context.loop) {
		pw_loop_invoke(c->context.l, NULL, 0, NULL, 0, false, c);
		pw_thread_loop_stop(c->context.loop);
	}
	if (c->context.notify) {
		queue_notify(c, NOTIFY_TYPE_REGISTRATION, c->object, 0, NULL);
		pw_loop_invoke(c->context.nl, NULL, 0, NULL, 0, false, c);
		pw_thread_loop_stop(c->context.notify);
	}

	if (c->registry) {
		spa_hook_remove(&c->registry_listener);
		pw_proxy_destroy((struct pw_proxy*)c->registry);
	}
	if (c->metadata && c->metadata->proxy) {
		pw_proxy_destroy((struct pw_proxy*)c->metadata->proxy);
	}
	if (c->settings && c->settings->proxy) {
		pw_proxy_destroy((struct pw_proxy*)c->settings->proxy);
	}

	if (c->core) {
		spa_hook_remove(&c->core_listener);
		pw_core_disconnect(c->core);
	}

	globals.thread_utils = pw_thread_utils_get();

	if (c->context.context)
		pw_context_destroy(c->context.context);

	if (c->notify_source)
		pw_loop_destroy_source(c->context.nl, c->notify_source);
	free(c->notify_buffer);

	if (c->context.loop)
		pw_thread_loop_destroy(c->context.loop);
	if (c->context.notify)
		pw_thread_loop_destroy(c->context.notify);

	pw_log_debug("%p: free", client);

	pw_array_for_each(item, &c->ports[SPA_DIRECTION_OUTPUT].items) {
                if (pw_map_item_is_free(item))
			continue;
		free_port(c, item->data, false);
	}
	pw_array_for_each(item, &c->ports[SPA_DIRECTION_INPUT].items) {
                if (pw_map_item_is_free(item))
			continue;
		free_port(c, item->data, false);
	}
	pthread_mutex_lock(&globals.lock);
	spa_list_consume(o, &c->context.objects, link) {
		bool to_free = o->to_free;
		spa_list_remove(&o->link);
		memset(o, 0, sizeof(struct object));
		o->to_free = to_free;
		spa_list_append(&globals.free_objects, &o->link);
	}
	pthread_mutex_unlock(&globals.lock);

	spa_list_for_each_safe(m, tm, &c->free_mix, link) {
		if (!m->to_free)
			spa_list_remove(&m->link);
	}
	spa_list_consume(m, &c->free_mix, link) {
		spa_list_remove(&m->link);
		free(m);
	}
	spa_list_for_each_safe(p, tp, &c->free_ports, link) {
		if (!p->to_free)
			spa_list_remove(&p->link);
	}
	spa_list_consume(p, &c->free_ports, link) {
		spa_list_remove(&p->link);
		free(p);
	}
	pw_map_clear(&c->ports[SPA_DIRECTION_INPUT]);
	pw_map_clear(&c->ports[SPA_DIRECTION_OUTPUT]);

	pthread_mutex_destroy(&c->context.lock);
	pthread_mutex_destroy(&c->rt_lock);
	pw_properties_free(c->props);
	free(c);

	return res;
}

SPA_EXPORT
jack_intclient_t jack_internal_client_handle (jack_client_t *client,
		const char *client_name, jack_status_t *status)
{
	struct client *c = (struct client *) client;
	return_val_if_fail(c != NULL, 0);
	if (status)
		*status = JackNoSuchClient | JackFailure;
	return 0;
}

SPA_EXPORT
jack_intclient_t jack_internal_client_load (jack_client_t *client,
		const char *client_name, jack_options_t options,
		jack_status_t *status, ...)
{
	struct client *c = (struct client *) client;
	return_val_if_fail(c != NULL, 0);
	if (status)
		*status = JackNoSuchClient | JackFailure;
	return 0;
}

SPA_EXPORT
jack_status_t jack_internal_client_unload (jack_client_t *client,
        jack_intclient_t intclient)
{
	struct client *c = (struct client *) client;
	return_val_if_fail(c != NULL, 0);
	return JackFailure | JackNoSuchClient;
}

SPA_EXPORT
char *jack_get_internal_client_name (jack_client_t *client,
		jack_intclient_t intclient)
{
	struct client *c = (struct client *) client;
	return_val_if_fail(c != NULL, NULL);
	return strdup(c->name);
}

SPA_EXPORT
int jack_client_name_size (void)
{
	/* The JACK API specifies that this value includes the final NULL character. */
	pw_log_trace("%d", JACK_CLIENT_NAME_SIZE+1);
	return JACK_CLIENT_NAME_SIZE+1;
}

SPA_EXPORT
char * jack_get_client_name (jack_client_t *client)
{
	struct client *c = (struct client *) client;
	return_val_if_fail(c != NULL, NULL);
	return c->name;
}

SPA_EXPORT
char *jack_get_uuid_for_client_name (jack_client_t *client,
                                     const char    *client_name)
{
	struct client *c = (struct client *) client;
	struct object *o;
	char *uuid = NULL;
	bool monitor;

	return_val_if_fail(c != NULL, NULL);
	return_val_if_fail(client_name != NULL, NULL);

	monitor = spa_strendswith(client_name, MONITOR_EXT);

	pthread_mutex_lock(&c->context.lock);

	spa_list_for_each(o, &c->context.objects, link) {
		if (o->type != INTERFACE_Node)
			continue;
		if (spa_streq(o->node.name, client_name) ||
		    (monitor && spa_strneq(o->node.name, client_name,
			    strlen(client_name) - strlen(MONITOR_EXT)))) {
			uuid = spa_aprintf( "%" PRIu64, client_make_uuid(o->serial, monitor));
			break;
		}
	}
	pw_log_debug("%p: name %s -> %s", client, client_name, uuid);
	pthread_mutex_unlock(&c->context.lock);
	return uuid;
}

SPA_EXPORT
char *jack_get_client_name_by_uuid (jack_client_t *client,
                                    const char    *client_uuid )
{
	struct client *c = (struct client *) client;
	struct object *o;
	jack_uuid_t uuid;
	char *name = NULL;
	bool monitor;

	return_val_if_fail(c != NULL, NULL);
	return_val_if_fail(client_uuid != NULL, NULL);

	if (jack_uuid_parse(client_uuid, &uuid) < 0)
		return NULL;

	monitor = uuid & (1 << 30);

	pthread_mutex_lock(&c->context.lock);
	spa_list_for_each(o, &c->context.objects, link) {
		if (o->type != INTERFACE_Node)
			continue;
		if (client_make_uuid(o->serial, monitor) == uuid) {
			pw_log_debug("%p: uuid %s (%"PRIu64")-> %s",
					client, client_uuid, uuid, o->node.name);
			name = spa_aprintf("%s%s", o->node.name, monitor ? MONITOR_EXT : "");
			break;
		}
	}
	pthread_mutex_unlock(&c->context.lock);
	return name;
}

SPA_EXPORT
int jack_internal_client_new (const char *client_name,
                              const char *load_name,
                              const char *load_init)
{
	pw_log_warn("not implemented %s %s %s", client_name, load_name, load_init);
	return -ENOTSUP;
}

SPA_EXPORT
void jack_internal_client_close (const char *client_name)
{
	pw_log_warn("not implemented %s", client_name);
}

static int do_activate(struct client *c)
{
	int res;
	pw_client_node_set_active(c->node, true);
	res = do_sync(c);
	return res;
}

static int
do_emit_buffer_size(struct spa_loop *loop,
                  bool async, uint32_t seq, const void *data, size_t size, void *user_data)
{
	struct client *c = user_data;
	c->buffer_frames = c->rt.position->clock.duration;
	pw_log_debug("%p: emit buffersize %d", c, c->buffer_frames);
	c->bufsize_callback(c->buffer_frames, c->bufsize_arg);
	return 0;
}

SPA_EXPORT
int jack_activate (jack_client_t *client)
{
	struct client *c = (struct client *) client;
	struct object *o;
	int res = 0;

	return_val_if_fail(c != NULL, -EINVAL);

	pw_log_info("%p: active:%d", c, c->active);

	if (c->active)
		return 0;

	pw_thread_loop_lock(c->context.loop);
	freeze_callbacks(c);

	/* reemit buffer_frames */
	c->buffer_frames = 0;

	pw_data_loop_start(c->loop);
	c->active = true;

	if ((res = do_activate(c)) < 0)
		goto done;

	c->activation->pending_new_pos = true;
	c->activation->pending_sync = true;

	spa_list_for_each(o, &c->context.objects, link) {
		if (o->type != INTERFACE_Port || o->port.port == NULL ||
		    o->port.port->client != c || !o->port.port->valid)
			continue;
		o->registered = 0;
		queue_notify(c, NOTIFY_TYPE_PORTREGISTRATION, o, 1, NULL);
	}
done:
	if (res < 0) {
		c->active = false;
		pw_data_loop_stop(c->loop);
	} else if (SPA_LIKELY(c->bufsize_callback != NULL)) {
		pw_thread_loop_unlock(c->context.loop);
		pw_data_loop_invoke(c->loop,
				do_emit_buffer_size, SPA_ID_INVALID, NULL, 0, true, c);
		pw_thread_loop_lock(c->context.loop);
	}
	pw_log_debug("%p: activate result:%d", c, res);
	thaw_callbacks(c);
	pw_thread_loop_unlock(c->context.loop);

	return res;
}

SPA_EXPORT
int jack_deactivate (jack_client_t *client)
{
	struct object *o;
	struct client *c = (struct client *) client;
	int res;

	return_val_if_fail(c != NULL, -EINVAL);

	pw_log_info("%p: active:%d", c, c->active);

	if (!c->active)
		return 0;

	pw_thread_loop_lock(c->context.loop);
	freeze_callbacks(c);

	pw_data_loop_stop(c->loop);

	pw_client_node_set_active(c->node, false);

	spa_list_for_each(o, &c->context.objects, link) {
		if (o->type != INTERFACE_Link || o->removed)
			continue;
		if (o->port_link.src_ours || o->port_link.dst_ours)
			pw_registry_destroy(c->registry, o->id);
	}

	spa_list_for_each(o, &c->context.objects, link) {
		if (o->type != INTERFACE_Port || o->port.port == NULL ||
		    o->port.port->client != c || !o->port.port->valid)
			continue;
		queue_notify(c, NOTIFY_TYPE_PORTREGISTRATION, o, 0, NULL);
	}
	c->activation->pending_new_pos = false;
	c->activation->pending_sync = false;

	c->active = false;

	res = do_sync(c);

	thaw_callbacks(c);
	pw_thread_loop_unlock(c->context.loop);

	return res;
}

SPA_EXPORT
int jack_get_client_pid (const char *name)
{
	struct object *on, *oc;

	if (g_first_client == NULL) return 0;

	on = find_node(g_first_client, name);
	if (on == NULL) {
		pw_log_warn("unknown (jack-client) node \"%s\"", name);
		return 0;
	}

	oc = find_client(g_first_client, on->node.client_id);
	if (oc == NULL) {
		pw_log_warn("unknown (pw) client %d", (int)on->node.client_id);
		return 0;
	}

	pw_log_info("pid %d (%s)", (int)oc->pwclient.pid, oc->pwclient.name);

	return (int)oc->pwclient.pid;
}

SPA_EXPORT
jack_native_thread_t jack_client_thread_id (jack_client_t *client)
{
	struct client *c = (struct client *) client;

	return_val_if_fail(c != NULL, (pthread_t){0});

	return (jack_native_thread_t)pw_data_loop_get_thread(c->loop);
}

SPA_EXPORT
int jack_is_realtime (jack_client_t *client)
{
	struct client *c = (struct client *) client;
	return_val_if_fail(c != NULL, 0);
	return !c->freewheeling;
}

SPA_EXPORT
jack_nframes_t jack_thread_wait (jack_client_t *client, int status)
{
	pw_log_error("%p: jack_thread_wait: deprecated, use jack_cycle_wait/jack_cycle_signal", client);
	return 0;
}

SPA_EXPORT
jack_nframes_t jack_cycle_wait (jack_client_t* client)
{
	struct client *c = (struct client *) client;
	jack_nframes_t res;

	return_val_if_fail(c != NULL, 0);

	res = cycle_wait(c);
	pw_log_trace("%p: result:%d", c, res);
	return res;
}

SPA_EXPORT
void jack_cycle_signal (jack_client_t* client, int status)
{
	struct client *c = (struct client *) client;

	return_if_fail(c != NULL);

	pw_log_trace("%p: status:%d", c, status);
	cycle_signal(c, status);
}

SPA_EXPORT
int jack_set_process_thread(jack_client_t* client, JackThreadCallback thread_callback, void *arg)
{
	struct client *c = (struct client *) client;

	return_val_if_fail(c != NULL, -EINVAL);

	if (c->active) {
		pw_log_error("%p: can't set callback on active client", c);
		return -EIO;
	} else if (c->process_callback) {
		pw_log_error("%p: process callback was already set", c);
		return -EIO;
	}
	pw_log_debug("%p: %p %p", c, thread_callback, arg);
	c->thread_callback = thread_callback;
	c->thread_arg = arg;
	return 0;
}

SPA_EXPORT
int jack_set_thread_init_callback (jack_client_t *client,
                                   JackThreadInitCallback thread_init_callback,
                                   void *arg)
{
	struct client *c = (struct client *) client;

	return_val_if_fail(c != NULL, -EINVAL);

	pw_log_debug("%p: %p %p", c, thread_init_callback, arg);
	c->thread_init_callback = thread_init_callback;
	c->thread_init_arg = arg;
	return 0;
}

SPA_EXPORT
void jack_on_shutdown (jack_client_t *client,
                       JackShutdownCallback shutdown_callback, void *arg)
{
	struct client *c = (struct client *) client;

	return_if_fail(c != NULL);

	if (c->active) {
		pw_log_error("%p: can't set callback on active client", c);
	} else {
		pw_log_debug("%p: %p %p", c, shutdown_callback, arg);
		c->shutdown_callback = shutdown_callback;
		c->shutdown_arg = arg;
	}
}

SPA_EXPORT
void jack_on_info_shutdown (jack_client_t *client,
                            JackInfoShutdownCallback shutdown_callback, void *arg)
{
	struct client *c = (struct client *) client;

	return_if_fail(c != NULL);

	if (c->active) {
		pw_log_error("%p: can't set callback on active client", c);
	} else {
		pw_log_debug("%p: %p %p", c, shutdown_callback, arg);
		c->info_shutdown_callback = shutdown_callback;
		c->info_shutdown_arg = arg;
	}
}

SPA_EXPORT
int jack_set_process_callback (jack_client_t *client,
                               JackProcessCallback process_callback,
                               void *arg)
{
	struct client *c = (struct client *) client;

	return_val_if_fail(c != NULL, -EINVAL);

	if (c->active) {
		pw_log_error("%p: can't set callback on active client", c);
		return -EIO;
	} else if (c->thread_callback) {
		pw_log_error("%p: thread callback was already set", c);
		return -EIO;
	}

	pw_log_debug("%p: %p %p", c, process_callback, arg);
	c->process_callback = process_callback;
	c->process_arg = arg;
	return 0;
}

SPA_EXPORT
int jack_set_freewheel_callback (jack_client_t *client,
                                 JackFreewheelCallback freewheel_callback,
                                 void *arg)
{
	struct client *c = (struct client *) client;

	return_val_if_fail(c != NULL, -EINVAL);

	if (c->active) {
		pw_log_error("%p: can't set callback on active client", c);
		return -EIO;
	}
	pw_log_debug("%p: %p %p", c, freewheel_callback, arg);
	c->freewheel_callback = freewheel_callback;
	c->freewheel_arg = arg;
	return 0;
}

SPA_EXPORT
int jack_set_buffer_size_callback (jack_client_t *client,
                                   JackBufferSizeCallback bufsize_callback,
                                   void *arg)
{
	struct client *c = (struct client *) client;

	return_val_if_fail(c != NULL, -EINVAL);

	if (c->active) {
		pw_log_error("%p: can't set callback on active client", c);
		return -EIO;
	}
	pw_log_debug("%p: %p %p", c, bufsize_callback, arg);
	c->bufsize_callback = bufsize_callback;
	c->bufsize_arg = arg;
	return 0;
}

SPA_EXPORT
int jack_set_sample_rate_callback (jack_client_t *client,
                                   JackSampleRateCallback srate_callback,
                                   void *arg)
{
	struct client *c = (struct client *) client;

	return_val_if_fail(c != NULL, -EINVAL);

	if (c->active) {
		pw_log_error("%p: can't set callback on active client", c);
		return -EIO;
	}
	pw_log_debug("%p: %p %p", c, srate_callback, arg);
	c->srate_callback = srate_callback;
	c->srate_arg = arg;
	if (c->srate_callback && c->sample_rate != (uint32_t)-1)
		c->srate_callback(c->sample_rate, c->srate_arg);
	return 0;
}

SPA_EXPORT
int jack_set_client_registration_callback (jack_client_t *client,
                                            JackClientRegistrationCallback
                                            registration_callback, void *arg)
{
	struct client *c = (struct client *) client;

	return_val_if_fail(c != NULL, -EINVAL);

	if (c->active) {
		pw_log_error("%p: can't set callback on active client", c);
		return -EIO;
	}
	pw_log_debug("%p: %p %p", c, registration_callback, arg);
	c->registration_callback = registration_callback;
	c->registration_arg = arg;
	return 0;
}

SPA_EXPORT
int jack_set_port_registration_callback (jack_client_t *client,
                                          JackPortRegistrationCallback
                                          registration_callback, void *arg)
{
	struct client *c = (struct client *) client;

	return_val_if_fail(c != NULL, -EINVAL);

	if (c->active) {
		pw_log_error("%p: can't set callback on active client", c);
		return -EIO;
	}
	pw_log_debug("%p: %p %p", c, registration_callback, arg);
	c->portregistration_callback = registration_callback;
	c->portregistration_arg = arg;
	return 0;
}


SPA_EXPORT
int jack_set_port_connect_callback (jack_client_t *client,
                                    JackPortConnectCallback
                                    connect_callback, void *arg)
{
	struct client *c = (struct client *) client;

	return_val_if_fail(c != NULL, -EINVAL);

	if (c->active) {
		pw_log_error("%p: can't set callback on active client", c);
		return -EIO;
	}
	pw_log_debug("%p: %p %p", c, connect_callback, arg);
	c->connect_callback = connect_callback;
	c->connect_arg = arg;
	return 0;
}

SPA_EXPORT
int jack_set_port_rename_callback (jack_client_t *client,
                                   JackPortRenameCallback rename_callback,
				   void *arg)
{
	struct client *c = (struct client *) client;

	return_val_if_fail(c != NULL, -EINVAL);

	if (c->active) {
		pw_log_error("%p: can't set callback on active client", c);
		return -EIO;
	}
	pw_log_debug("%p: %p %p", c, rename_callback, arg);
	c->rename_callback = rename_callback;
	c->rename_arg = arg;
	return 0;
}

SPA_EXPORT
int jack_set_graph_order_callback (jack_client_t *client,
                                   JackGraphOrderCallback graph_callback,
                                   void *data)
{
	struct client *c = (struct client *) client;

	return_val_if_fail(c != NULL, -EINVAL);

	if (c->active) {
		pw_log_error("%p: can't set callback on active client", c);
		return -EIO;
	}
	pw_log_debug("%p: %p %p", c, graph_callback, data);
	c->graph_callback = graph_callback;
	c->graph_arg = data;
	return 0;
}

SPA_EXPORT
int jack_set_xrun_callback (jack_client_t *client,
                            JackXRunCallback xrun_callback, void *arg)
{
	struct client *c = (struct client *) client;

	return_val_if_fail(c != NULL, -EINVAL);

	if (c->active) {
		pw_log_error("%p: can't set callback on active client", c);
		return -EIO;
	}
	pw_log_debug("%p: %p %p", c, xrun_callback, arg);
	c->xrun_callback = xrun_callback;
	c->xrun_arg = arg;
	return 0;
}

SPA_EXPORT
int jack_set_latency_callback (jack_client_t *client,
			       JackLatencyCallback latency_callback,
			       void *data)
{
	struct client *c = (struct client *) client;

	return_val_if_fail(c != NULL, -EINVAL);

	if (c->active) {
		pw_log_error("%p: can't set callback on active client", c);
		return -EIO;
	}
	pw_log_debug("%p: %p %p", c, latency_callback, data);
	c->latency_callback = latency_callback;
	c->latency_arg = data;
	return 0;
}

SPA_EXPORT
int jack_set_freewheel(jack_client_t* client, int onoff)
{
	struct client *c = (struct client *) client;
	const char *str;

	pw_log_info("%p: freewheel %d", client, onoff);

	pw_thread_loop_lock(c->context.loop);
	str = pw_properties_get(c->props, PW_KEY_NODE_GROUP);
	if (str != NULL) {
		char *p = strstr(str, ",pipewire.freewheel");
		if (p == NULL)
			p = strstr(str, "pipewire.freewheel");
		if (p == NULL && onoff)
			pw_properties_setf(c->props, PW_KEY_NODE_GROUP,
					"%s,pipewire.freewheel", str);
		else if (p != NULL && !onoff) {
			pw_log_info("%s %d %s %.*s", p, (int)(p - str),
					str, (int)(p - str), str);
			pw_properties_setf(c->props, PW_KEY_NODE_GROUP,
					"%.*s", (int)(p - str), str);
		}
	} else {
		pw_properties_set(c->props, PW_KEY_NODE_GROUP,
				onoff ? "pipewire.freewheel" : "");
	}

	c->info.change_mask |= SPA_NODE_CHANGE_MASK_PROPS;
	c->info.props = &c->props->dict;

	pw_client_node_update(c->node,
                                    PW_CLIENT_NODE_UPDATE_INFO,
				    0, NULL, &c->info);
	c->info.change_mask = 0;
	pw_thread_loop_unlock(c->context.loop);

	return 0;
}

SPA_EXPORT
int jack_set_buffer_size (jack_client_t *client, jack_nframes_t nframes)
{
	struct client *c = (struct client *) client;

	return_val_if_fail(c != NULL, -EINVAL);

	pw_log_info("%p: buffer-size %u", client, nframes);

	pw_thread_loop_lock(c->context.loop);
	if (c->global_buffer_size && c->settings && c->settings->proxy) {
		char val[256];
		snprintf(val, sizeof(val), "%u", nframes == 1 ? 0: nframes);
		pw_metadata_set_property(c->settings->proxy, 0,
				"clock.force-quantum", "", val);
	} else {
		pw_properties_setf(c->props, PW_KEY_NODE_FORCE_QUANTUM, "%u", nframes);

		c->info.change_mask |= SPA_NODE_CHANGE_MASK_PROPS;
		c->info.props = &c->props->dict;

		pw_client_node_update(c->node,
	                                    PW_CLIENT_NODE_UPDATE_INFO,
					    0, NULL, &c->info);
		c->info.change_mask = 0;
	}
	pw_thread_loop_unlock(c->context.loop);

	return 0;
}

SPA_EXPORT
int jack_set_sample_rate (jack_client_t *client, jack_nframes_t nframes)
{
	struct client *c = (struct client *) client;

	return_val_if_fail(c != NULL, -EINVAL);

	pw_log_info("%p: sample-size %u", client, nframes);

	pw_thread_loop_lock(c->context.loop);
	if (c->global_sample_rate && c->settings && c->settings->proxy) {
		char val[256];
		snprintf(val, sizeof(val), "%u", nframes);
		pw_metadata_set_property(c->settings->proxy, 0,
				"clock.force-rate", "", val);
	} else {
		pw_properties_setf(c->props, PW_KEY_NODE_FORCE_RATE, "%u", nframes);

		c->info.change_mask |= SPA_NODE_CHANGE_MASK_PROPS;
		c->info.props = &c->props->dict;

		pw_client_node_update(c->node,
	                                    PW_CLIENT_NODE_UPDATE_INFO,
					    0, NULL, &c->info);
		c->info.change_mask = 0;
	}
	pw_thread_loop_unlock(c->context.loop);

	return 0;
}

SPA_EXPORT
jack_nframes_t jack_get_sample_rate (jack_client_t *client)
{
	struct client *c = (struct client *) client;
	jack_nframes_t res = -1;

	return_val_if_fail(c != NULL, 0);

	if (!c->active)
		res = c->latency.denom;
	if (c->active || res == (uint32_t)-1) {
		res = c->sample_rate;
		if (res == (uint32_t)-1) {
			if (c->rt.position)
				res = c->rt.position->clock.rate.denom;
			else if (c->position)
				res = c->position->clock.rate.denom;
		}
	}
	c->sample_rate = res;
	pw_log_trace_fp("sample_rate: %u", res);
	return res;
}

SPA_EXPORT
jack_nframes_t jack_get_buffer_size (jack_client_t *client)
{
	struct client *c = (struct client *) client;
	jack_nframes_t res = -1;

	return_val_if_fail(c != NULL, 0);

	if (!c->active)
		res = c->latency.num;
	if (c->active || res == (uint32_t)-1) {
		res = c->buffer_frames;
		if (res == (uint32_t)-1) {
			if (c->rt.position)
				res = c->rt.position->clock.duration;
			else if (c->position)
				res = c->position->clock.duration;
		}
	}
	c->buffer_frames = res;
	pw_log_debug("buffer_frames: %u", res);
	return res;
}

SPA_EXPORT
int jack_engine_takeover_timebase (jack_client_t *client)
{
	pw_log_error("%p: deprecated", client);
	return 0;
}

SPA_EXPORT
float jack_cpu_load (jack_client_t *client)
{
	struct client *c = (struct client *) client;
	float res = 0.0f;

	return_val_if_fail(c != NULL, 0.0);

	if (c->driver_activation)
		res = c->driver_activation->cpu_load[0] * 100.0f;

	pw_log_trace("%p: cpu load %f", client, res);
	return res;
}

#include "statistics.c"

static void *get_buffer_input_float(struct port *p, jack_nframes_t frames);
static void *get_buffer_input_midi(struct port *p, jack_nframes_t frames);
static void *get_buffer_input_empty(struct port *p, jack_nframes_t frames);
static void *get_buffer_output_float(struct port *p, jack_nframes_t frames);
static void *get_buffer_output_midi(struct port *p, jack_nframes_t frames);
static void *get_buffer_output_empty(struct port *p, jack_nframes_t frames);

SPA_EXPORT
jack_port_t * jack_port_register (jack_client_t *client,
                                  const char *port_name,
                                  const char *port_type,
                                  unsigned long flags,
                                  unsigned long buffer_frames)
{
	struct client *c = (struct client *) client;
	enum spa_direction direction;
	struct object *o;
	jack_port_type_id_t type_id;
	uint8_t buffer[1024];
	struct spa_pod_builder b = SPA_POD_BUILDER_INIT(buffer, sizeof(buffer));
	struct spa_pod *params[7];
	uint32_t n_params = 0;
	struct port *p;
	int res, len;
	char name[REAL_JACK_PORT_NAME_SIZE+1];

	return_val_if_fail(c != NULL, NULL);
	return_val_if_fail(port_name != NULL && strlen(port_name) != 0, NULL);
	return_val_if_fail(port_type != NULL, NULL);

	pw_log_info("%p: port register \"%s:%s\" \"%s\" %08lx %ld",
			c, c->name, port_name, port_type, flags, buffer_frames);

	if (flags & JackPortIsInput)
		direction = PW_DIRECTION_INPUT;
	else if (flags & JackPortIsOutput)
		direction = PW_DIRECTION_OUTPUT;
	else {
		pw_log_warn("invalid port flags %lu for %s", flags, port_name);
		return NULL;
	}

	if ((type_id = string_to_type(port_type)) == SPA_ID_INVALID) {
		pw_log_warn("unknown port type %s", port_type);
		return NULL;
	}
	if (type_id == TYPE_ID_MIDI && (flags & JackPortIsMIDI2))
		type_id = TYPE_ID_UMP;

	len = snprintf(name, sizeof(name), "%s:%s", c->name, port_name);
	if (len < 0 || (size_t)len >= sizeof(name)) {
		pw_log_warn("%p: name \"%s:%s\" too long", c,
				c->name, port_name);
		return NULL;
	}
	pthread_mutex_lock(&c->context.lock);
	o = find_port_by_name(c, name);
	pthread_mutex_unlock(&c->context.lock);
	if (o != NULL) {
		pw_log_warn("%p: name \"%s\" already exists", c, name);
		return NULL;
	}

	if ((p = alloc_port(c, direction)) == NULL) {
		pw_log_warn("can't allocate port %s: %m", port_name);
		return NULL;
	}

	o = p->object;
	o->port.flags = flags;
	strcpy(o->port.name, name);
	o->port.type_id = type_id;

	init_buffer(p, c->max_frames);

	if (direction == SPA_DIRECTION_INPUT) {
		switch (type_id) {
		case TYPE_ID_AUDIO:
		case TYPE_ID_VIDEO:
			p->get_buffer = get_buffer_input_float;
			break;
		case TYPE_ID_MIDI:
		case TYPE_ID_OSC:
		case TYPE_ID_UMP:
			p->get_buffer = get_buffer_input_midi;
			break;
		default:
			p->get_buffer = get_buffer_input_empty;
			break;
		}
	} else {
		switch (type_id) {
		case TYPE_ID_AUDIO:
		case TYPE_ID_VIDEO:
			p->get_buffer = get_buffer_output_float;
			break;
		case TYPE_ID_MIDI:
		case TYPE_ID_OSC:
		case TYPE_ID_UMP:
			p->get_buffer = get_buffer_output_midi;
			break;
		default:
			p->get_buffer = get_buffer_output_empty;
			break;
		}
	}

	pw_log_debug("%p: port %p", c, p);

	spa_list_init(&p->mix);

	pw_properties_set(p->props, PW_KEY_FORMAT_DSP, type_to_format_dsp(type_id));
	pw_properties_set(p->props, PW_KEY_PORT_NAME, port_name);
	if (flags > 0x1f) {
		pw_properties_setf(p->props, PW_KEY_PORT_EXTRA,
				"jack:flags:%lu", flags & ~0x1f);
	}
	if (flags & JackPortIsPhysical)
		pw_properties_set(p->props, PW_KEY_PORT_PHYSICAL, "true");
	if (flags & JackPortIsTerminal)
		pw_properties_set(p->props, PW_KEY_PORT_TERMINAL, "true");

	p->info = SPA_PORT_INFO_INIT();
	p->info.change_mask |= SPA_PORT_CHANGE_MASK_FLAGS;
	p->info.flags = SPA_PORT_FLAG_NO_REF;
	p->info.change_mask |= SPA_PORT_CHANGE_MASK_PROPS;
	p->info.props = &p->props->dict;
	p->info.change_mask |= SPA_PORT_CHANGE_MASK_PARAMS;
	p->params[IDX_EnumFormat] = SPA_PARAM_INFO(SPA_PARAM_EnumFormat, SPA_PARAM_INFO_READ);
	p->params[IDX_Buffers] = SPA_PARAM_INFO(SPA_PARAM_Buffers, SPA_PARAM_INFO_READ);
	p->params[IDX_IO] = SPA_PARAM_INFO(SPA_PARAM_IO, SPA_PARAM_INFO_READ);
	p->params[IDX_Format] = SPA_PARAM_INFO(SPA_PARAM_Format, SPA_PARAM_INFO_WRITE);
	p->params[IDX_Latency] = SPA_PARAM_INFO(SPA_PARAM_Latency, SPA_PARAM_INFO_READWRITE);
	p->info.params = p->params;
	p->info.n_params = N_PORT_PARAMS;

	param_enum_format(c, p, &params[n_params++], &b);
	param_buffers(c, p, &params[n_params++], &b);
	param_io(c, p, &params[n_params++], &b);
	param_io_async(c, p, &params[n_params++], &b);
	param_latency(c, p, &params[n_params++], &b);
	param_latency_other(c, p, &params[n_params++], &b);

	pw_thread_loop_lock(c->context.loop);
	if (create_mix(c, p, SPA_ID_INVALID, SPA_ID_INVALID) == NULL) {
		res = -errno;
		pw_log_warn("can't create mix for port %s: %m", port_name);
		pw_thread_loop_unlock(c->context.loop);
		goto error_free;
	}

	freeze_callbacks(c);

	pw_client_node_port_update(c->node,
					 direction,
					 p->port_id,
					 PW_CLIENT_NODE_PORT_UPDATE_PARAMS |
					 PW_CLIENT_NODE_PORT_UPDATE_INFO,
					 n_params,
					 (const struct spa_pod **) params,
					 &p->info);

	p->info.change_mask = 0;

	res = do_sync(c);

	thaw_callbacks(c);
	pw_log_debug("%p: port %p done", c, p);
	pw_thread_loop_unlock(c->context.loop);

	if (res < 0) {
		pw_log_warn("can't create port %s: %s", port_name,
				spa_strerror(res));
		goto error_free;
	}

	return object_to_port(o);

error_free:
	free_port(c, p, true);
	return NULL;
}

static int
do_free_port(struct spa_loop *loop,
                  bool async, uint32_t seq, const void *data, size_t size, void *user_data)
{
	struct port *p = user_data;
	struct client *c = p->client;
	free_port(c, p, !c->active);
	return 0;
}

static int
do_invalidate_port(struct spa_loop *loop,
                  bool async, uint32_t seq, const void *data, size_t size, void *user_data)
{
	struct port *p = user_data;
	struct client *c = p->client;
	p->valid = false;
	pw_loop_invoke(c->context.l, do_free_port, 0, NULL, 0, false, p);
	return 0;
}

SPA_EXPORT
int jack_port_unregister (jack_client_t *client, jack_port_t *port)
{
	struct client *c = (struct client *) client;
	struct object *o = port_to_object(port);
	struct port *p;
	int res;

	return_val_if_fail(c != NULL, -EINVAL);
	return_val_if_fail(o != NULL, -EINVAL);

	pw_thread_loop_lock(c->context.loop);
	freeze_callbacks(c);

	p = o->port.port;
	if (o->type != INTERFACE_Port || p == NULL || !p->valid ||
	    o->client != c) {
		pw_log_error("%p: invalid port %p", client, port);
		res = -EINVAL;
		goto done;
	}
	pw_data_loop_invoke(c->loop, do_invalidate_port, 1, NULL, 0, false, p);

	pw_log_info("%p: port %p unregister \"%s\"", client, port, o->port.name);

	pw_client_node_port_update(c->node,
					 p->direction,
					 p->port_id,
					 0, 0, NULL, NULL);

	res = do_sync(c);
	if (res < 0) {
		pw_log_warn("can't unregister port %s: %s", o->port.name,
				spa_strerror(res));
	}
done:
	thaw_callbacks(c);
	pw_thread_loop_unlock(c->context.loop);

	return res;
}

static struct buffer *get_mix_buffer(struct client *c, struct mix *mix, jack_nframes_t frames)
{
	struct spa_io_buffers *io;
	uint32_t cycle = c->rt.position->clock.cycle & 1;

	if (mix->peer_port != NULL)
		prepare_output(mix->peer_port, frames, cycle);

	io = mix->io[cycle];
	if (io == NULL ||
	    io->status != SPA_STATUS_HAVE_DATA ||
	    io->buffer_id >= mix->n_buffers)
		return NULL;

	return &mix->buffers[io->buffer_id];
}

static inline void *get_buffer_data(struct buffer *b, jack_nframes_t frames)
{
	struct spa_data *d;
	uint32_t offset, size;

	d = &b->datas[0];
	offset = SPA_MIN(d->chunk->offset, d->maxsize);
	size = SPA_MIN(d->chunk->size, d->maxsize - offset);
	if (size / sizeof(float) < frames)
		return NULL;
	return SPA_PTROFF(d->data, offset, void);
}

static void *get_buffer_input_float(struct port *p, jack_nframes_t frames)
{
	struct mix *mix;
	struct buffer *b;
	void *ptr = NULL;
	float *mix_ptr[MAX_MIX], *np;
	uint32_t n_ptr = 0;
	bool ptr_aligned = true;
	struct client *c = p->client;

	spa_list_for_each(mix, &p->mix, port_link) {
		if (mix->id == SPA_ID_INVALID)
			continue;

		pw_log_trace_fp("%p: port %s mix %d.%d get buffer %d",
				c, p->object->port.name, p->port_id, mix->id, frames);

		if ((b = get_mix_buffer(c, mix, frames)) == NULL)
			continue;

		if ((np = get_buffer_data(b, frames)) == NULL)
			continue;

		if (!SPA_IS_ALIGNED(np, 16))
			ptr_aligned = false;

		mix_ptr[n_ptr++] = np;
		if (n_ptr == MAX_MIX)
			break;
	}
	if (n_ptr == 1) {
		ptr = mix_ptr[0];
	} else if (n_ptr > 1) {
		ptr = p->emptyptr;
		c->mix_function(ptr, mix_ptr, n_ptr, ptr_aligned, frames);
		p->zeroed = false;
	}
	if (ptr == NULL)
		ptr = init_buffer(p, frames);
	return ptr;
}

static void *get_buffer_input_midi(struct port *p, jack_nframes_t frames)
{
	struct mix *mix;
	void *ptr = p->emptyptr;
	struct midi_buffer *mb = (struct midi_buffer*)midi_scratch;
	struct spa_pod_sequence *seq[MAX_MIX];
	uint32_t n_seq = 0;

	spa_list_for_each(mix, &p->mix, port_link) {
		struct spa_data *d;
		struct buffer *b;
		void *pod;

		if (mix->id == SPA_ID_INVALID)
			continue;

		pw_log_trace_fp("%p: port %p mix %d.%d get buffer %d",
				p->client, p, p->port_id, mix->id, frames);

		if ((b = get_mix_buffer(p->client, mix, frames)) == NULL)
			continue;

		d = &b->datas[0];

		if ((pod = spa_pod_from_data(d->data, d->maxsize, d->chunk->offset, d->chunk->size)) == NULL)
			continue;
		if (!spa_pod_is_sequence(pod))
			continue;

		seq[n_seq++] = pod;
		if (n_seq == MAX_MIX)
			break;
	}
	midi_init_buffer(mb, MIDI_SCRATCH_FRAMES, frames);
	/* first convert to a thread local scratch buffer, then memcpy into
	 * the per port buffer. This makes it possible to call this function concurrently
	 * but also have different pointers per port */
	convert_to_event(seq, n_seq, mb, p->client->fix_midi_events, p->object->port.type_id);
	memcpy(ptr, mb, sizeof(struct midi_buffer) + (mb->event_count
                              * sizeof(struct midi_event)));
	if (mb->write_pos) {
		size_t offs = mb->buffer_size - 1 - mb->write_pos;
		memcpy(SPA_PTROFF(ptr, offs, void), SPA_PTROFF(mb, offs, void), mb->write_pos);
	}
	return ptr;
}

static void *get_buffer_output_float(struct port *p, jack_nframes_t frames)
{
	void *ptr;

	ptr = get_buffer_output(p, frames, sizeof(float), NULL);
	if (SPA_UNLIKELY(p->empty_out = (ptr == NULL)))
		ptr = p->emptyptr;
	return ptr;
}

static void *get_buffer_output_midi(struct port *p, jack_nframes_t frames)
{
	p->empty_out = true;
	return p->emptyptr;
}

static void *get_buffer_output_empty(struct port *p, jack_nframes_t frames)
{
	p->empty_out = true;
	return p->emptyptr;
}

static void *get_buffer_input_empty(struct port *p, jack_nframes_t frames)
{
	return init_buffer(p, frames);
}

SPA_EXPORT
void * jack_port_get_buffer (jack_port_t *port, jack_nframes_t frames)
{
	struct object *o = port_to_object(port);
	struct port *p = NULL;
	void *ptr = NULL;
	struct client *c;

	return_val_if_fail(o != NULL, NULL);

	c = o->client;
	if (o->type != INTERFACE_Port || c == NULL)
		goto done;

	if (frames > c->max_frames)
		goto done;

	if ((p = o->port.port) == NULL) {
		struct mix *mix;
		struct buffer *b;

		if ((mix = find_mix_peer(c, o->id)) == NULL)
			goto done;

		pw_log_trace("peer mix: %p %d", mix, mix->peer_id);

		if ((b = get_mix_buffer(c, mix, frames)) == NULL)
			goto done;

		if (TYPE_ID_IS_EVENT(o->port.type_id)) {
			struct spa_pod_sequence *seq[1];
			struct spa_data *d;
			void *pod;

			ptr = midi_scratch;
			midi_init_buffer(ptr, MIDI_SCRATCH_FRAMES, frames);

			d = &b->datas[0];
			if ((pod = spa_pod_from_data(d->data, d->maxsize,
							d->chunk->offset, d->chunk->size)) == NULL)
				goto done;
			if (!spa_pod_is_sequence(pod))
				goto done;
			seq[0] = pod;
			convert_to_event(seq, 1, ptr, c->fix_midi_events, o->port.type_id);
		} else {
			ptr = get_buffer_data(b, frames);
		}
	} else if (p->valid) {
		ptr = p->get_buffer(p, frames);
	}
done:
	pw_log_trace_fp("%p: port:%p buffer:%p frames:%d", c, p, ptr, frames);
	return ptr;
}

SPA_EXPORT
jack_uuid_t jack_port_uuid (const jack_port_t *port)
{
	struct object *o = port_to_object(port);
	return_val_if_fail(o != NULL, 0);
	return jack_port_uuid_generate(o->serial);
}

static const char *port_name(struct object *o)
{
	const char *name;
	struct client *c = o->client;
	if (c == NULL)
		return NULL;
	if (c->default_as_system && is_port_default(c, o))
		name = o->port.system;
	else
		name = o->port.name;
	return name;
}

SPA_EXPORT
const char * jack_port_name (const jack_port_t *port)
{
	struct object *o = port_to_object(port);
	return_val_if_fail(o != NULL, NULL);
	if (o->type != INTERFACE_Port)
		return NULL;
	return port_name(o);
}

SPA_EXPORT
const char * jack_port_short_name (const jack_port_t *port)
{
	struct object *o = port_to_object(port);
	return_val_if_fail(o != NULL, NULL);
	if (o->type != INTERFACE_Port)
		return NULL;
	return strchr(port_name(o), ':') + 1;
}

SPA_EXPORT
int jack_port_flags (const jack_port_t *port)
{
	struct object *o = port_to_object(port);
	return_val_if_fail(o != NULL, 0);
	if (o->type != INTERFACE_Port)
		return 0;
	return o->port.flags;
}

SPA_EXPORT
const char * jack_port_type (const jack_port_t *port)
{
	struct object *o = port_to_object(port);
	return_val_if_fail(o != NULL, NULL);
	if (o->type != INTERFACE_Port)
		return NULL;
	return type_to_string(o->port.type_id);
}

SPA_EXPORT
jack_port_type_id_t jack_port_type_id (const jack_port_t *port)
{
	struct object *o = port_to_object(port);
	return_val_if_fail(o != NULL, 0);
	if (o->type != INTERFACE_Port)
		return TYPE_ID_OTHER;
	return o->port.type_id;
}

SPA_EXPORT
int jack_port_is_mine (const jack_client_t *client, const jack_port_t *port)
{
	struct object *o = port_to_object(port);
	return_val_if_fail(o != NULL, 0);
	return o->type == INTERFACE_Port &&
		o->port.port != NULL &&
		o->port.port->client == (struct client*)client;
}

SPA_EXPORT
int jack_port_connected (const jack_port_t *port)
{
	struct object *o = port_to_object(port);
	struct client *c;
	struct object *l;
	int res = 0;

	return_val_if_fail(o != NULL, 0);
	if (o->type != INTERFACE_Port || o->client == NULL)
		return 0;

	c = o->client;

	pthread_mutex_lock(&c->context.lock);
	spa_list_for_each(l, &c->context.objects, link) {
		if (l->type != INTERFACE_Link || l->removed)
			continue;
		if (l->port_link.src_serial == o->serial ||
		    l->port_link.dst_serial == o->serial)
			res++;
	}
	pthread_mutex_unlock(&c->context.lock);

	pw_log_debug("%p: id:%u/%u res:%d", port, o->id, o->serial, res);

	return res;
}

SPA_EXPORT
int jack_port_connected_to (const jack_port_t *port,
                            const char *port_name)
{
	struct object *o = port_to_object(port);
	struct client *c;
	struct object *p, *l;
	int res = 0;

	return_val_if_fail(o != NULL, 0);
	return_val_if_fail(port_name != NULL, 0);
	if (o->type != INTERFACE_Port || o->client == NULL)
		return 0;

	c = o->client;

	pthread_mutex_lock(&c->context.lock);

	p = find_port_by_name(c, port_name);
	if (p == NULL)
		goto exit;

	if (GET_DIRECTION(p->port.flags) == GET_DIRECTION(o->port.flags))
		goto exit;

	if (p->port.flags & JackPortIsOutput) {
		l = p;
		p = o;
		o = l;
	}
	if ((l = find_link(c, o->id, p->id)) != NULL)
		res = 1;

     exit:
	pthread_mutex_unlock(&c->context.lock);
	pw_log_debug("%p: id:%u/%u name:%s res:%d", port, o->id,
			o->serial, port_name, res);

	return res;
}

SPA_EXPORT
const char ** jack_port_get_connections (const jack_port_t *port)
{
	struct object *o = port_to_object(port);

	return_val_if_fail(o != NULL, NULL);
	if (o->type != INTERFACE_Port || o->client == NULL)
		return NULL;

	return jack_port_get_all_connections((jack_client_t *)o->client, port);
}

SPA_EXPORT
const char ** jack_port_get_all_connections (const jack_client_t *client,
                                             const jack_port_t *port)
{
	struct client *c = (struct client *) client;
	struct object *o = port_to_object(port);
	struct object *p, *l;
	const char **res;
	int count = 0;
	struct pw_array tmp;

	return_val_if_fail(c != NULL, NULL);
	return_val_if_fail(o != NULL, NULL);

	pw_array_init(&tmp, sizeof(void*) * 32);

	pthread_mutex_lock(&c->context.lock);
	spa_list_for_each(l, &c->context.objects, link) {
		if (l->type != INTERFACE_Link || l->removed)
			continue;
		if (l->port_link.src_serial == o->serial)
			p = find_type(c, l->port_link.dst, INTERFACE_Port, true);
		else if (l->port_link.dst_serial == o->serial)
			p = find_type(c, l->port_link.src, INTERFACE_Port, true);
		else
			continue;

		if (p == NULL)
			continue;

		pw_array_add_ptr(&tmp, (void*)port_name(p));
		count++;
	}
	pthread_mutex_unlock(&c->context.lock);

	if (count == 0) {
		pw_array_clear(&tmp);
		res = NULL;
	} else {
		pw_array_add_ptr(&tmp, NULL);
		res = tmp.data;
	}
	return res;
}

SPA_EXPORT
int jack_port_tie (jack_port_t *src, jack_port_t *dst)
{
	struct object *s = port_to_object(src);
	struct object *d = port_to_object(dst);
	struct port *sp, *dp;

	sp = s->port.port;
	dp = d->port.port;
	if (sp == NULL || !sp->valid ||
	    dp == NULL || !dp->valid ||
	    sp->client != dp->client)
		return -EINVAL;

	dp->tied = sp;
	return 0;
}

SPA_EXPORT
int jack_port_untie (jack_port_t *port)
{
	struct object *o = port_to_object(port);
	struct port *p;

	p = o->port.port;
	if (p == NULL || !p->valid)
		return -EINVAL;
	p->tied = NULL;
	return 0;
}

SPA_EXPORT
int jack_port_set_name (jack_port_t *port, const char *port_name)
{
	pw_log_warn("deprecated");
	return 0;
}

SPA_EXPORT
int jack_port_rename (jack_client_t* client, jack_port_t *port, const char *port_name)
{
	struct client *c = (struct client *) client;
	struct object *o = port_to_object(port);
	struct port *p;
	int res = 0;

	return_val_if_fail(c != NULL, -EINVAL);
	return_val_if_fail(o != NULL, -EINVAL);
	return_val_if_fail(port_name != NULL, -EINVAL);

	pw_thread_loop_lock(c->context.loop);

	pw_log_info("%p: port rename %p %s -> %s:%s",
			client, port, o->port.name, c->name, port_name);

	p = o->port.port;
	if (p == NULL || !p->valid) {
		res = -EINVAL;
		goto done;
	}

	pw_properties_set(p->props, PW_KEY_PORT_NAME, port_name);
	snprintf(o->port.name, sizeof(o->port.name), "%s:%s", c->name, port_name);

	p->info.change_mask |= SPA_PORT_CHANGE_MASK_PROPS;
	p->info.props = &p->props->dict;

	pw_client_node_port_update(c->node,
					 p->direction,
					 p->port_id,
					 PW_CLIENT_NODE_PORT_UPDATE_INFO,
					 0, NULL,
					 &p->info);
	p->info.change_mask = 0;

done:
	pw_thread_loop_unlock(c->context.loop);

	return res;
}

SPA_EXPORT
int jack_port_set_alias (jack_port_t *port, const char *alias)
{
	struct object *o = port_to_object(port);
	struct client *c;
	struct port *p;
	const char *key;
	int res = 0;

	return_val_if_fail(o != NULL, -EINVAL);
	return_val_if_fail(alias != NULL, -EINVAL);

	c = o->client;
	if (o->type != INTERFACE_Port || c == NULL)
		return -EINVAL;

	pw_thread_loop_lock(c->context.loop);

	p = o->port.port;
	if (p == NULL || !p->valid) {
		res = -EINVAL;
		goto done;
	}

	if (o->port.alias1[0] == '\0') {
		key = PW_KEY_OBJECT_PATH;
		snprintf(o->port.alias1, sizeof(o->port.alias1), "%s", alias);
	}
	else if (o->port.alias2[0] == '\0') {
		key = PW_KEY_PORT_ALIAS;
		snprintf(o->port.alias2, sizeof(o->port.alias2), "%s", alias);
	}
	else {
		res = -1;
		goto done;
	}

	pw_properties_set(p->props, key, alias);

	p->info.change_mask |= SPA_PORT_CHANGE_MASK_PROPS;
	p->info.props = &p->props->dict;

	pw_client_node_port_update(c->node,
					 p->direction,
					 p->port_id,
					 PW_CLIENT_NODE_PORT_UPDATE_INFO,
					 0, NULL,
					 &p->info);
	p->info.change_mask = 0;

done:
	pw_thread_loop_unlock(c->context.loop);

	return res;
}

SPA_EXPORT
int jack_port_unset_alias (jack_port_t *port, const char *alias)
{
	struct object *o = port_to_object(port);
	struct client *c;
	struct port *p;
	const char *key;
	int res = 0;

	return_val_if_fail(o != NULL, -EINVAL);
	return_val_if_fail(alias != NULL, -EINVAL);

	c = o->client;
	if (o->type != INTERFACE_Port || c == NULL)
		return -EINVAL;

	pw_thread_loop_lock(c->context.loop);
	p = o->port.port;
	if (p == NULL || !p->valid) {
		res = -EINVAL;
		goto done;
	}

	if (spa_streq(o->port.alias1, alias))
		key = PW_KEY_OBJECT_PATH;
	else if (spa_streq(o->port.alias2, alias))
		key = PW_KEY_PORT_ALIAS;
	else {
		res = -1;
		goto done;
	}

	pw_properties_set(p->props, key, NULL);

	p->info.change_mask |= SPA_PORT_CHANGE_MASK_PROPS;
	p->info.props = &p->props->dict;

	pw_client_node_port_update(c->node,
					 p->direction,
					 p->port_id,
					 PW_CLIENT_NODE_PORT_UPDATE_INFO,
					 0, NULL,
					 &p->info);
	p->info.change_mask = 0;

done:
	pw_thread_loop_unlock(c->context.loop);

	return res;
}

SPA_EXPORT
int jack_port_get_aliases (const jack_port_t *port, char* const aliases[2])
{
	struct object *o = port_to_object(port);
	int res = 0;

	return_val_if_fail(o != NULL, -EINVAL);
	return_val_if_fail(aliases != NULL, -EINVAL);
	return_val_if_fail(aliases[0] != NULL, -EINVAL);
	return_val_if_fail(aliases[1] != NULL, -EINVAL);

	if (o->port.alias1[0] != '\0') {
		snprintf(aliases[0], REAL_JACK_PORT_NAME_SIZE+1, "%s", o->port.alias1);
		res++;
	}
	if (o->port.alias2[0] != '\0') {
		snprintf(aliases[1], REAL_JACK_PORT_NAME_SIZE+1, "%s", o->port.alias2);
		res++;
	}

	return res;
}

SPA_EXPORT
int jack_port_request_monitor (jack_port_t *port, int onoff)
{
	struct object *o = port_to_object(port);

	return_val_if_fail(o != NULL, -EINVAL);

	if (onoff)
		o->port.monitor_requests++;
	else if (o->port.monitor_requests > 0)
		o->port.monitor_requests--;
	return 0;
}

SPA_EXPORT
int jack_port_request_monitor_by_name (jack_client_t *client,
                                       const char *port_name, int onoff)
{
	struct client *c = (struct client *) client;
	struct object *p;

	return_val_if_fail(c != NULL, -EINVAL);
	return_val_if_fail(port_name != NULL, -EINVAL);

	pthread_mutex_lock(&c->context.lock);
	p = find_port_by_name(c, port_name);
	pthread_mutex_unlock(&c->context.lock);

	if (p == NULL) {
		pw_log_error("%p: jack_port_request_monitor_by_name called"
				" with an incorrect port %s", client, port_name);
		return -1;
	}

	return jack_port_request_monitor(object_to_port(p), onoff);
}

SPA_EXPORT
int jack_port_ensure_monitor (jack_port_t *port, int onoff)
{
	struct object *o = port_to_object(port);

	return_val_if_fail(o != NULL, -EINVAL);

	if (onoff) {
		if (o->port.monitor_requests == 0)
			o->port.monitor_requests++;
	} else {
		if (o->port.monitor_requests > 0)
			o->port.monitor_requests = 0;
	}
	return 0;
}

SPA_EXPORT
int jack_port_monitoring_input (jack_port_t *port)
{
	struct object *o = port_to_object(port);
	return_val_if_fail(o != NULL, -EINVAL);
	return o->port.monitor_requests > 0;
}

static void link_proxy_error(void *data, int seq, int res, const char *message)
{
	int *link_res = data;
	*link_res = res;
}

static const struct pw_proxy_events link_proxy_events = {
	PW_VERSION_PROXY_EVENTS,
	.error = link_proxy_error,
};

static int check_connect(struct client *c, struct object *src, struct object *dst)
{
	int src_self, dst_self, sum;

	src_self = src->port.node_id == c->node_id ? 1 : 0;
	dst_self = dst->port.node_id == c->node_id ? 1 : 0;
	sum = src_self + dst_self;

	pw_log_debug("sum %d %d", sum, c->self_connect_mode);

	/* check for other connection first */
	if (sum == 0)
		return c->other_connect_mode;

	if (c->self_connect_mode == SELF_CONNECT_ALLOW)
		return 1;

	/* internal connection */
	if (sum == 2 &&
	    (c->self_connect_mode == SELF_CONNECT_FAIL_EXT ||
	     c->self_connect_mode == SELF_CONNECT_IGNORE_EXT))
		return 1;

	/* failure -> -1 */
	if (c->self_connect_mode < 0)
		return -1;

	/* ignore -> 0 */
	return 0;
}

SPA_EXPORT
int jack_connect (jack_client_t *client,
                  const char *source_port,
                  const char *destination_port)
{
	struct client *c = (struct client *) client;
	struct object *src, *dst;
	struct spa_dict props;
	struct spa_dict_item items[6];
	struct pw_proxy *proxy;
	struct spa_hook listener;
	char val[4][16];
	int res, link_res = 0;

	return_val_if_fail(c != NULL, EINVAL);
	return_val_if_fail(source_port != NULL, EINVAL);
	return_val_if_fail(destination_port != NULL, EINVAL);

	pw_log_info("%p: connect %s %s", client, source_port, destination_port);

	pw_thread_loop_lock(c->context.loop);
	freeze_callbacks(c);

	src = find_port_by_name(c, source_port);
	dst = find_port_by_name(c, destination_port);

	if (src == NULL || dst == NULL ||
	    !(src->port.flags & JackPortIsOutput) ||
	    !(dst->port.flags & JackPortIsInput) ||
	    !TYPE_ID_IS_COMPATIBLE(src->port.type_id, dst->port.type_id)) {
		res = -EINVAL;
		goto exit;
	}
	if ((res = check_connect(c, src, dst)) != 1)
		goto exit;

	snprintf(val[0], sizeof(val[0]), "%d", src->port.node_id);
	snprintf(val[1], sizeof(val[1]), "%d", src->id);
	snprintf(val[2], sizeof(val[2]), "%d", dst->port.node_id);
	snprintf(val[3], sizeof(val[3]), "%d", dst->id);

	props = SPA_DICT_INIT(items, 0);
	items[props.n_items++] = SPA_DICT_ITEM_INIT(PW_KEY_LINK_OUTPUT_NODE, val[0]);
	items[props.n_items++] = SPA_DICT_ITEM_INIT(PW_KEY_LINK_OUTPUT_PORT, val[1]);
	items[props.n_items++] = SPA_DICT_ITEM_INIT(PW_KEY_LINK_INPUT_NODE, val[2]);
	items[props.n_items++] = SPA_DICT_ITEM_INIT(PW_KEY_LINK_INPUT_PORT, val[3]);
	items[props.n_items++] = SPA_DICT_ITEM_INIT(PW_KEY_OBJECT_LINGER, "true");
	if (c->passive_links)
		items[props.n_items++] = SPA_DICT_ITEM_INIT(PW_KEY_LINK_PASSIVE, "true");

	proxy = pw_core_create_object(c->core,
				    "link-factory",
				    PW_TYPE_INTERFACE_Link,
				    PW_VERSION_LINK,
				    &props,
				    0);
	if (proxy == NULL) {
		res = -errno;
		goto exit;
	}

	spa_zero(listener);
	pw_proxy_add_listener(proxy, &listener, &link_proxy_events, &link_res);

	res = do_sync(c);

	spa_hook_remove(&listener);

	if (link_res < 0)
		res = link_res;

	pw_proxy_destroy(proxy);

exit:
	pw_log_debug("%p: connect %s %s done %d", client, source_port, destination_port, res);
	thaw_callbacks(c);
	pw_thread_loop_unlock(c->context.loop);

	return -res;
}

SPA_EXPORT
int jack_disconnect (jack_client_t *client,
                     const char *source_port,
                     const char *destination_port)
{
	struct client *c = (struct client *) client;
	struct object *src, *dst, *l;
	int res;

	return_val_if_fail(c != NULL, -EINVAL);
	return_val_if_fail(source_port != NULL, -EINVAL);
	return_val_if_fail(destination_port != NULL, -EINVAL);

	pw_log_info("%p: disconnect %s %s", client, source_port, destination_port);

	pw_thread_loop_lock(c->context.loop);
	freeze_callbacks(c);

	src = find_port_by_name(c, source_port);
	dst = find_port_by_name(c, destination_port);

	pw_log_debug("%p: %d %d", client, src->id, dst->id);

	if (src == NULL || dst == NULL ||
	    !(src->port.flags & JackPortIsOutput) ||
	    !(dst->port.flags & JackPortIsInput)) {
		res = -EINVAL;
		goto exit;
	}

	if ((res = check_connect(c, src, dst)) != 1)
		goto exit;

	if ((l = find_link(c, src->id, dst->id)) == NULL) {
		res = -ENOENT;
		goto exit;
	}

	pw_registry_destroy(c->registry, l->id);

	res = do_sync(c);

      exit:
	thaw_callbacks(c);
	pw_thread_loop_unlock(c->context.loop);

	return -res;
}

SPA_EXPORT
int jack_port_disconnect (jack_client_t *client, jack_port_t *port)
{
	struct client *c = (struct client *) client;
	struct object *o = port_to_object(port);
	struct object *l;
	int res;

	return_val_if_fail(c != NULL, -EINVAL);
	return_val_if_fail(o != NULL, -EINVAL);

	pw_log_debug("%p: disconnect %p", client, port);

	pw_thread_loop_lock(c->context.loop);
	freeze_callbacks(c);

	spa_list_for_each(l, &c->context.objects, link) {
		if (l->type != INTERFACE_Link || l->removed)
			continue;
		if (l->port_link.src_serial == o->serial ||
		    l->port_link.dst_serial == o->serial) {
			pw_registry_destroy(c->registry, l->id);
		}
	}
	res = do_sync(c);

	thaw_callbacks(c);
	pw_thread_loop_unlock(c->context.loop);

	return -res;
}

SPA_EXPORT
int jack_port_name_size(void)
{
	return REAL_JACK_PORT_NAME_SIZE+1;
}

SPA_EXPORT
int jack_port_type_size(void)
{
	return JACK_PORT_TYPE_SIZE+1;
}

SPA_EXPORT
size_t jack_port_type_get_buffer_size (jack_client_t *client, const char *port_type)
{
	struct client *c = (struct client *) client;

	return_val_if_fail(client != NULL, 0);
	return_val_if_fail(port_type != NULL, 0);

	if (spa_streq(JACK_DEFAULT_AUDIO_TYPE, port_type))
		return jack_get_buffer_size(client) * sizeof(float);
	else if (spa_streq(JACK_DEFAULT_MIDI_TYPE, port_type))
		return c->max_frames * sizeof(float);
	else if (spa_streq(JACK_DEFAULT_OSC_TYPE, port_type))
		return c->max_frames * sizeof(float);
	else if (spa_streq(JACK_DEFAULT_UMP_TYPE, port_type))
		return c->max_frames * sizeof(float);
	else if (spa_streq(JACK_DEFAULT_VIDEO_TYPE, port_type))
		return 320 * 240 * 4 * sizeof(float);
	else
		return 0;
}

SPA_EXPORT
void jack_port_set_latency (jack_port_t *port, jack_nframes_t frames)
{
	struct object *o = port_to_object(port);
	struct client *c;
	jack_latency_range_t range = { frames, frames };

	return_if_fail(o != NULL);
	c = o->client;

	pw_log_debug("%p: %s set latency %d", c, o->port.name, frames);

	if (o->port.flags & JackPortIsOutput) {
		jack_port_set_latency_range(port, JackCaptureLatency, &range);
        }
        if (o->port.flags & JackPortIsInput) {
		jack_port_set_latency_range(port, JackPlaybackLatency, &range);
        }
}

SPA_EXPORT
void jack_port_get_latency_range (jack_port_t *port, jack_latency_callback_mode_t mode, jack_latency_range_t *range)
{
	struct object *o = port_to_object(port);
	struct client *c;
	jack_nframes_t nframes, rate;
	int direction;
	struct spa_latency_info *info;
	int64_t min, max;

	return_if_fail(o != NULL);
	c = o->client;

	if (o->type != INTERFACE_Port || c == NULL) {
		range->min = range->max = 0;
		return;
	}

	if (mode == JackCaptureLatency)
		direction = SPA_DIRECTION_OUTPUT;
	else
		direction = SPA_DIRECTION_INPUT;

	nframes = jack_get_buffer_size((jack_client_t*)c);
	rate = jack_get_sample_rate((jack_client_t*)c);
	info = &o->port.latency[direction];

	min = (int64_t)(info->min_quantum * nframes) +
		info->min_rate +
		(info->min_ns * (int64_t)rate) / (int64_t)SPA_NSEC_PER_SEC;
	max = (int64_t)(info->max_quantum * nframes) +
		info->max_rate +
		(info->max_ns * (int64_t)rate) / (int64_t)SPA_NSEC_PER_SEC;

	range->min = SPA_MAX(min, 0);
	range->max = SPA_MAX(max, 0);

	pw_log_debug("%p: %s get %d latency range %d %d", c, o->port.name,
			mode, range->min, range->max);
}

static int
do_port_check_latency(struct spa_loop *loop,
		bool async, uint32_t seq, const void *data, size_t size, void *user_data)
{
	struct port *p = user_data;
	const struct spa_latency_info *latency = data;
	port_check_latency(p, latency);
	return 0;
}

SPA_EXPORT
void jack_port_set_latency_range (jack_port_t *port, jack_latency_callback_mode_t mode, jack_latency_range_t *range)
{
	struct object *o = port_to_object(port);
	struct client *c;
	enum spa_direction direction;
	struct spa_latency_info latency;
	jack_nframes_t nframes;
	struct port *p;

	return_if_fail(o != NULL);
	if (o->type != INTERFACE_Port || o->client == NULL)
		return;
	c = o->client;

	if (mode == JackCaptureLatency)
		direction = SPA_DIRECTION_OUTPUT;
	else
		direction = SPA_DIRECTION_INPUT;

	pw_log_info("%p: %s set %d latency range %d %d", c, o->port.name, mode, range->min, range->max);

	latency = SPA_LATENCY_INFO(direction);

	nframes = jack_get_buffer_size((jack_client_t*)c);
	if (nframes == 0)
		nframes = 1;

	latency.min_rate = range->min;
	if (latency.min_rate >= (int32_t)nframes) {
		latency.min_quantum = latency.min_rate / nframes;
		latency.min_rate %= nframes;
	}

	latency.max_rate = range->max;
	if (latency.max_rate >= (int32_t)nframes) {
		latency.max_quantum = latency.max_rate / nframes;
		latency.max_rate %= nframes;
	}

	if ((p = o->port.port) == NULL)
		return;

	pw_loop_invoke(c->context.l, do_port_check_latency, 0,
			&latency, sizeof(latency), false, p);
}

SPA_EXPORT
int jack_recompute_total_latencies (jack_client_t *client)
{
	struct client *c = (struct client *) client;
	return queue_notify(c, NOTIFY_TYPE_TOTAL_LATENCY, NULL, 0, NULL);
}

static jack_nframes_t port_get_latency (jack_port_t *port)
{
	struct object *o = port_to_object(port);
	jack_latency_range_t range = { 0, 0 };

	return_val_if_fail(o != NULL, 0);

	if (o->port.flags & JackPortIsOutput) {
		jack_port_get_latency_range(port, JackCaptureLatency, &range);
        }
        if (o->port.flags & JackPortIsInput) {
		jack_port_get_latency_range(port, JackPlaybackLatency, &range);
        }
	return (range.min + range.max) / 2;
}

SPA_EXPORT
jack_nframes_t jack_port_get_latency (jack_port_t *port)
{
	return port_get_latency(port);
}

SPA_EXPORT
jack_nframes_t jack_port_get_total_latency (jack_client_t *client,
					    jack_port_t *port)
{
	return port_get_latency(port);
}

SPA_EXPORT
int jack_recompute_total_latency (jack_client_t *client, jack_port_t* port)
{
	pw_log_warn("%p: not implemented %p", client, port);
	return 0;
}

static int port_compare_func(const void *v1, const void *v2)
{
	const struct object *const*o1 = v1, *const*o2 = v2;
	struct client *c = (*o1)->client;
	int res;
	bool is_cap1, is_cap2, is_def1 = false, is_def2 = false;

	is_cap1 = ((*o1)->port.flags & JackPortIsOutput) == JackPortIsOutput &&
		!(*o1)->port.is_monitor;
	is_cap2 = ((*o2)->port.flags & JackPortIsOutput) == JackPortIsOutput &&
		!(*o2)->port.is_monitor;

	if (c->metadata) {
		struct object *ot1, *ot2;

		ot1 = (*o1)->port.node;

		if (is_cap1)
			is_def1 = ot1 != NULL && spa_streq(ot1->node.node_name,
					c->metadata->default_audio_source);
		else if (!is_cap1)
			is_def1 = ot1 != NULL && spa_streq(ot1->node.node_name,
					c->metadata->default_audio_sink);
		ot2 = (*o2)->port.node;

		if (is_cap2)
			is_def2 = ot2 != NULL && spa_streq(ot2->node.node_name,
					c->metadata->default_audio_source);
		else if (!is_cap2)
			is_def2 = ot2 != NULL && spa_streq(ot2->node.node_name,
					c->metadata->default_audio_sink);
	}
	if ((*o1)->port.type_id != (*o2)->port.type_id)
		res = (*o1)->port.type_id - (*o2)->port.type_id;
	else if ((is_cap1 || is_cap2) && is_cap1 != is_cap2)
		res = is_cap2 - is_cap1;
	else if ((is_def1 || is_def2) && is_def1 != is_def2)
		res = is_def2 - is_def1;
	else if ((*o1)->port.priority != (*o2)->port.priority)
		res = (*o2)->port.priority - (*o1)->port.priority;
	else if ((res = (*o1)->port.node_id - (*o2)->port.node_id) == 0) {
		if ((*o1)->port.is_monitor != (*o2)->port.is_monitor)
			res = (*o1)->port.is_monitor - (*o2)->port.is_monitor;
		if (res == 0)
			res = (*o1)->port.system_id - (*o2)->port.system_id;
		if (res == 0)
			res = (*o1)->serial - (*o2)->serial;
	}
	pw_log_debug("port %s<->%s type:%d<->%d def:%d<->%d prio:%d<->%d id:%d<->%d res:%d",
			(*o1)->port.name, (*o2)->port.name,
			(*o1)->port.type_id, (*o2)->port.type_id,
			is_def1, is_def2,
			(*o1)->port.priority, (*o2)->port.priority,
			(*o1)->serial, (*o2)->serial, res);
	return res;
}

SPA_EXPORT
const char ** jack_get_ports (jack_client_t *client,
                              const char *port_name_pattern,
                              const char *type_name_pattern,
                              unsigned long flags)
{
	struct client *c = (struct client *) client;
	const char **res;
	struct object *o;
	struct pw_array tmp;
	const char *str;
	uint32_t i, count;
	int r;
	regex_t port_regex, type_regex;

	return_val_if_fail(c != NULL, NULL);

	str = getenv("PIPEWIRE_NODE");

	if (port_name_pattern && port_name_pattern[0]) {
		if ((r = regcomp(&port_regex, port_name_pattern, REG_EXTENDED | REG_NOSUB)) != 0) {
			pw_log_error("can't compile regex %s: %d", port_name_pattern, r);
			return NULL;
		}
	}
	if (type_name_pattern && type_name_pattern[0]) {
		if ((r = regcomp(&type_regex, type_name_pattern, REG_EXTENDED | REG_NOSUB)) != 0) {
			pw_log_error("can't compile regex %s: %d", type_name_pattern, r);
			return NULL;
		}
	}

	pw_log_debug("%p: ports target:%s name:\"%s\" type:\"%s\" flags:%08lx", c, str,
			port_name_pattern, type_name_pattern, flags);

	pthread_mutex_lock(&c->context.lock);
	pw_array_init(&tmp, sizeof(void*) * 32);
	count = 0;

	spa_list_for_each(o, &c->context.objects, link) {
		if (o->type != INTERFACE_Port || o->removed || !o->visible)
			continue;
		pw_log_debug("%p: check port type:%d flags:%08lx name:\"%s\"", c,
				o->port.type_id, o->port.flags, o->port.name);
		if (TYPE_ID_IS_HIDDEN(o->port.type_id))
			continue;
		if (!SPA_FLAG_IS_SET(o->port.flags, flags))
			continue;
		if (str != NULL && o->port.node != NULL) {
			if (!spa_strstartswith(o->port.name, str) &&
			    o->port.node->serial != atoll(str))
				continue;
		}

		if (port_name_pattern && port_name_pattern[0]) {
			bool match;
			match = regexec(&port_regex, o->port.name, 0, NULL, 0) == 0;
			if (!match && is_port_default(c, o))
				match = regexec(&port_regex, o->port.system, 0, NULL, 0) == 0;
			if (!match)
				continue;
		}
		if (type_name_pattern && type_name_pattern[0]) {
			if (regexec(&type_regex, type_to_string(o->port.type_id),
						0, NULL, 0) == REG_NOMATCH)
				continue;
		}
		pw_log_debug("%p: port \"%s\" prio:%d matches (%d)",
				c, o->port.name, o->port.priority, count);

		pw_array_add_ptr(&tmp, o);
		count++;
	}
	pthread_mutex_unlock(&c->context.lock);

	if (count > 0) {
		qsort(tmp.data, count, sizeof(struct object *), port_compare_func);
		pw_array_add_ptr(&tmp, NULL);
		res = tmp.data;
		for (i = 0; i < count; i++)
			res[i] = port_name((struct object*)res[i]);
	} else {
		pw_array_clear(&tmp);
		res = NULL;
	}

	if (port_name_pattern && port_name_pattern[0])
		regfree(&port_regex);
	if (type_name_pattern && type_name_pattern[0])
		regfree(&type_regex);

	return res;
}

SPA_EXPORT
jack_port_t * jack_port_by_name (jack_client_t *client, const char *port_name)
{
	struct client *c = (struct client *) client;
	struct object *res;

	return_val_if_fail(c != NULL, NULL);

	pthread_mutex_lock(&c->context.lock);
	res = find_port_by_name(c, port_name);
	pthread_mutex_unlock(&c->context.lock);

	if (res == NULL)
		pw_log_info("%p: port \"%s\" not found", c, port_name);

	return object_to_port(res);
}

SPA_EXPORT
jack_port_t * jack_port_by_id (jack_client_t *client,
                               jack_port_id_t port_id)
{
	struct client *c = (struct client *) client;
	struct object *res = NULL;

	return_val_if_fail(c != NULL, NULL);

	pthread_mutex_lock(&c->context.lock);
	res = find_by_serial(c, port_id);
	if (res && res->type != INTERFACE_Port)
		res = NULL;
	pw_log_debug("%p: port %d -> %p", c, port_id, res);
	pthread_mutex_unlock(&c->context.lock);

	if (res == NULL)
		pw_log_info("%p: port %d not found", c, port_id);

	return object_to_port(res);
}

static inline void get_frame_times(struct client *c, struct frame_times *times)
{
	jack_unique_t u1;
	uint32_t count = 0;
	do {
		u1 = c->jack_position.unique_1;
		*times = c->jack_times;
		if (++count == 10) {
			pw_log_warn("could not get snapshot %" PRIu64 " %" PRIu64, u1, c->jack_position.unique_2);
			break;
		}
	} while (u1 != c->jack_position.unique_2);
}

SPA_EXPORT
jack_nframes_t jack_frames_since_cycle_start (const jack_client_t *client)
{
	struct client *c = (struct client *) client;
	struct frame_times times;
	int64_t diff;

	return_val_if_fail(c != NULL, 0);

	get_frame_times(c, &times);
	diff = get_time_ns(c->l->system) - times.nsec;
	return (jack_nframes_t) floor(((double)times.sample_rate * diff) / SPA_NSEC_PER_SEC);
}

SPA_EXPORT
jack_nframes_t jack_frame_time (const jack_client_t *client)
{
	return jack_time_to_frames(client, jack_get_time());
}

SPA_EXPORT
jack_nframes_t jack_last_frame_time (const jack_client_t *client)
{
	struct client *c = (struct client *) client;
	struct frame_times times;

	return_val_if_fail(c != NULL, 0);

	get_frame_times(c, &times);

	return times.frames;
}

SPA_EXPORT
int jack_get_cycle_times(const jack_client_t *client,
                        jack_nframes_t *current_frames,
                        jack_time_t    *current_usecs,
                        jack_time_t    *next_usecs,
                        float          *period_usecs)
{
	struct client *c = (struct client *) client;
	struct frame_times times;

	return_val_if_fail(c != NULL, -EINVAL);

	get_frame_times(c, &times);

	if (times.sample_rate == 0 || times.rate_diff == 0.0)
		return -1;

	*current_frames = times.frames;
	*next_usecs = times.next_nsec / SPA_NSEC_PER_USEC;
	*period_usecs = (float)(times.buffer_frames *
			SPA_USEC_PER_SEC / (times.sample_rate * times.rate_diff));
	*current_usecs = *next_usecs - (jack_time_t)*period_usecs;

	pw_log_trace("%p: %d %"PRIu64" %"PRIu64" %f", c, *current_frames,
			*current_usecs, *next_usecs, *period_usecs);
	return 0;
}

SPA_EXPORT
jack_time_t jack_frames_to_time(const jack_client_t *client, jack_nframes_t frames)
{
	struct client *c = (struct client *) client;
	struct frame_times times;

	return_val_if_fail(c != NULL, -EINVAL);

	get_frame_times(c, &times);

	if (times.buffer_frames == 0 || times.sample_rate == 0 || times.rate_diff == 0.0)
		return 0;

	uint32_t nf = (uint32_t)times.frames;
	uint64_t nw = times.next_nsec/SPA_NSEC_PER_USEC;
	uint64_t dp = (uint64_t)(times.buffer_frames *
			(float)SPA_USEC_PER_SEC / (times.sample_rate * times.rate_diff));
	uint64_t w = nw - dp;
	int32_t df = frames - nf;
	return w + (int64_t)rint((double) df * (double) dp / times.buffer_frames);
}

SPA_EXPORT
jack_nframes_t jack_time_to_frames(const jack_client_t *client, jack_time_t usecs)
{
	struct client *c = (struct client *) client;
	struct frame_times times;

	return_val_if_fail(c != NULL, -EINVAL);

	get_frame_times(c, &times);

	if (times.sample_rate == 0 || times.rate_diff == 0.0)
		return 0;

	uint32_t nf = (uint32_t)times.frames;
	uint64_t nw = times.next_nsec/SPA_NSEC_PER_USEC;
	uint64_t dp = (uint64_t)(times.buffer_frames *
			(float)SPA_USEC_PER_SEC / (times.sample_rate * times.rate_diff));
	uint64_t w = nw - dp;
	int64_t du = usecs - w;
	return nf + (int32_t)rint((double)du / (double)dp * times.buffer_frames);
}

SPA_EXPORT
jack_time_t jack_get_time(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return SPA_TIMESPEC_TO_USEC(&ts);
}

SPA_EXPORT
void default_jack_error_callback(const char *desc)
{
	pw_log_error("pw jack error: %s",desc);
}

SPA_EXPORT
void silent_jack_error_callback(const char *desc)
{
}

SPA_EXPORT
void (*jack_error_callback)(const char *msg);

SPA_EXPORT
void jack_set_error_function (void (*func)(const char *))
{
	jack_error_callback = (func == NULL) ? &default_jack_error_callback : func;
}

SPA_EXPORT
void default_jack_info_callback(const char *desc)
{
	pw_log_info("pw jack info: %s", desc);
}

SPA_EXPORT
void silent_jack_info_callback(const char *desc)
{
}

SPA_EXPORT
void (*jack_info_callback)(const char *msg);


SPA_EXPORT
void jack_set_info_function (void (*func)(const char *))
{
	jack_info_callback = (func == NULL) ? &default_jack_info_callback : func;
}

SPA_EXPORT
void jack_free(void* ptr)
{
	free(ptr);
}

SPA_EXPORT
int jack_release_timebase (jack_client_t *client)
{
	struct client *c = (struct client *) client;
	struct pw_node_activation *a;

	return_val_if_fail(c != NULL, -EINVAL);

	if ((a = c->driver_activation) == NULL)
		return -EIO;

	if (!SPA_ATOMIC_CAS(a->segment_owner[0], c->node_id, 0))
		return -EINVAL;

	c->timebase_callback = NULL;
	c->timebase_arg = NULL;
	c->activation->pending_new_pos = false;

	return 0;
}

SPA_EXPORT
int jack_set_sync_callback (jack_client_t *client,
			    JackSyncCallback sync_callback,
			    void *arg)
{
	int res = 0;
	struct client *c = (struct client *) client;

	return_val_if_fail(c != NULL, -EINVAL);

	pw_thread_loop_lock(c->context.loop);
	freeze_callbacks(c);

	c->sync_callback = sync_callback;
	c->sync_arg = arg;

	if ((res = do_activate(c)) < 0)
		goto done;

	c->activation->pending_sync = true;
done:
	thaw_callbacks(c);
	pw_thread_loop_unlock(c->context.loop);

	return res;
}

SPA_EXPORT
int jack_set_sync_timeout (jack_client_t *client,
			   jack_time_t timeout)
{
	int res = 0;
	struct client *c = (struct client *) client;
	struct pw_node_activation *a;

	return_val_if_fail(c != NULL, -EINVAL);

	pw_thread_loop_lock(c->context.loop);

	if ((a = c->activation) == NULL)
		res = -EIO;
	else
		a->sync_timeout = timeout;
	pw_thread_loop_unlock(c->context.loop);

	return res;
}

SPA_EXPORT
int  jack_set_timebase_callback (jack_client_t *client,
				 int conditional,
				 JackTimebaseCallback timebase_callback,
				 void *arg)
{
	int res = 0;
	struct client *c = (struct client *) client;

	return_val_if_fail(c != NULL, -EINVAL);
	return_val_if_fail(timebase_callback != NULL, -EINVAL);

	pw_thread_loop_lock(c->context.loop);
	freeze_callbacks(c);

	c->timebase_callback = timebase_callback;
	c->timebase_arg = arg;
	c->timeowner_conditional = conditional;
	install_timeowner(c);

	pw_log_debug("%p: timebase set id:%u", c, c->node_id);

	if ((res = do_activate(c)) < 0)
		goto done;

	c->activation->pending_new_pos = true;
done:
	thaw_callbacks(c);
	pw_thread_loop_unlock(c->context.loop);

	return res;
}

SPA_EXPORT
int  jack_transport_locate (jack_client_t *client,
			    jack_nframes_t frame)
{
	jack_position_t pos;
	pos.frame = frame;
	pos.valid = (jack_position_bits_t)0;
	return jack_transport_reposition(client, &pos);
}

SPA_EXPORT
jack_transport_state_t jack_transport_query (const jack_client_t *client,
					     jack_position_t *pos)
{
	struct client *c = (struct client *) client;
	jack_transport_state_t state;
	jack_unique_t u1;
	uint32_t count = 0;

	return_val_if_fail(c != NULL, JackTransportStopped);

	do {
		u1 = c->jack_position.unique_1;
		state = c->jack_state;
		if (pos != NULL)
			*pos = c->jack_position;
		if (++count == 10) {
			pw_log_warn("could not get snapshot %" PRIu64 " %" PRIu64, u1, c->jack_position.unique_2);
			break;
		}
	} while (u1 != c->jack_position.unique_2);

	return state;
}

SPA_EXPORT
jack_nframes_t jack_get_current_transport_frame (const jack_client_t *client)
{
	struct client *c = (struct client *) client;
	jack_transport_state_t state;
	jack_nframes_t res;
	jack_position_t pos;

	return_val_if_fail(c != NULL, -EINVAL);

	state = jack_transport_query(client, &pos);
	res = pos.frame;

	if (state == JackTransportRolling) {
		float usecs = get_time_ns(c->l->system)/1000 - pos.usecs;
		res += (jack_nframes_t)floor((((float) pos.frame_rate) / 1000000.0f) * usecs);
	}
	return res;
}

SPA_EXPORT
int  jack_transport_reposition (jack_client_t *client,
				const jack_position_t *pos)
{
	struct client *c = (struct client *) client;
	struct pw_node_activation *a, *na;

	return_val_if_fail(c != NULL, -EINVAL);

	a = c->rt.driver_activation;
	na = c->activation;
	if (!a || !na)
		return -EIO;

	if (pos->valid & ~(JackPositionBBT|JackPositionTimecode))
		return -EINVAL;

	pw_log_debug("frame:%u", pos->frame);
	spa_zero(na->reposition);
	na->reposition.flags = 0;
	na->reposition.start = 0;
	na->reposition.duration = 0;
	na->reposition.position = pos->frame;
	na->reposition.rate = 1.0;
	SPA_ATOMIC_STORE(a->reposition_owner, c->node_id);

	return 0;
}

static void update_command(struct client *c, uint32_t command)
{
	struct pw_node_activation *a = c->rt.driver_activation;
	if (!a)
		return;
	SPA_ATOMIC_STORE(a->command, command);
}

static int transport_update(struct client* c, int active)
{
	pw_log_info("%p: transport %d", c, active);

	pw_thread_loop_lock(c->context.loop);
	pw_properties_set(c->props, PW_KEY_NODE_SYNC, "true");
	pw_properties_set(c->props, PW_KEY_NODE_TRANSPORT,
			active ? "true" : "false");

	c->info.change_mask |= SPA_NODE_CHANGE_MASK_PROPS;
	c->info.props = &c->props->dict;

	pw_client_node_update(c->node,
                                    PW_CLIENT_NODE_UPDATE_INFO,
				    0, NULL, &c->info);
	c->info.change_mask = 0;

	pw_properties_set(c->props, PW_KEY_NODE_TRANSPORT, NULL);
	pw_thread_loop_unlock(c->context.loop);

	return 0;
}

SPA_EXPORT
void jack_transport_start (jack_client_t *client)
{
	struct client *c = (struct client *) client;
	return_if_fail(c != NULL);
	if (c->activation->server_version < 1)
		update_command(c, PW_NODE_ACTIVATION_COMMAND_START);
	else
		transport_update(c, true);
}

SPA_EXPORT
void jack_transport_stop (jack_client_t *client)
{
	struct client *c = (struct client *) client;
	return_if_fail(c != NULL);
	if (c->activation->server_version < 1)
		update_command(c, PW_NODE_ACTIVATION_COMMAND_STOP);
	else
		transport_update(c, false);
}

SPA_EXPORT
void jack_get_transport_info (jack_client_t *client,
			      jack_transport_info_t *tinfo)
{
	pw_log_error("%p: deprecated", client);
	if (tinfo)
		memset(tinfo, 0, sizeof(jack_transport_info_t));
}

SPA_EXPORT
void jack_set_transport_info (jack_client_t *client,
			      jack_transport_info_t *tinfo)
{
	pw_log_error("%p: deprecated", client);
	if (tinfo)
		memset(tinfo, 0, sizeof(jack_transport_info_t));
}

SPA_EXPORT
int jack_set_session_callback (jack_client_t       *client,
                               JackSessionCallback  session_callback,
                               void                *arg)
{
	struct client *c = (struct client *) client;

	return_val_if_fail(c != NULL, -EINVAL);

	if (c->active) {
		pw_log_error("%p: can't set callback on active client", c);
		return -EIO;
	}
	pw_log_warn("%p: not implemented", client);
	return -ENOTSUP;
}

SPA_EXPORT
int jack_session_reply (jack_client_t        *client,
                        jack_session_event_t *event)
{
	pw_log_warn("%p: not implemented", client);
	return -ENOTSUP;
}


SPA_EXPORT
void jack_session_event_free (jack_session_event_t *event)
{
	if (event) {
		free((void *)event->session_dir);
		free((void *)event->client_uuid);
		free(event->command_line);
		free(event);
	}
}

SPA_EXPORT
char *jack_client_get_uuid (jack_client_t *client)
{
	struct client *c = (struct client *) client;

	return_val_if_fail(c != NULL, NULL);

	return spa_aprintf("%"PRIu64, client_make_uuid(c->serial, false));
}

SPA_EXPORT
jack_session_command_t *jack_session_notify (
        jack_client_t*             client,
        const char                *target,
        jack_session_event_type_t  type,
        const char                *path)
{
	struct client *c = (struct client *) client;
	jack_session_command_t *cmds;
	return_val_if_fail(c != NULL, NULL);
	pw_log_warn("not implemented");
	cmds = calloc(1, sizeof(jack_session_command_t));
	return cmds;
}

SPA_EXPORT
void jack_session_commands_free (jack_session_command_t *cmds)
{
	int i;
	if (cmds == NULL)
		return;

	for (i = 0; cmds[i].uuid != NULL; i++) {
		free((char*)cmds[i].client_name);
		free((char*)cmds[i].command);
		free((char*)cmds[i].uuid);
	}
	free(cmds);
}

SPA_EXPORT
int jack_reserve_client_name (jack_client_t *client,
                          const char    *name,
                          const char    *uuid)
{
	struct client *c = (struct client *) client;
	return_val_if_fail(c != NULL, -1);
	pw_log_warn("not implemented");
	return 0;
}

SPA_EXPORT
int jack_client_has_session_callback (jack_client_t *client, const char *client_name)
{
	struct client *c = (struct client *) client;
	return_val_if_fail(c != NULL, -1);
	return 0;
}


SPA_EXPORT
int jack_client_real_time_priority (jack_client_t * client)
{
	return jack_client_max_real_time_priority(client) - 5;
}

SPA_EXPORT
int jack_client_max_real_time_priority (jack_client_t *client)
{
	struct client *c = (struct client *) client;
	int min, max;

	return_val_if_fail(c != NULL, -1);

	spa_thread_utils_get_rt_range(&c->context.thread_utils, NULL, &min, &max);
	return SPA_MIN(max, c->rt_max) - 1;
}

SPA_EXPORT
int jack_acquire_real_time_scheduling (jack_native_thread_t thread, int priority)
{
	struct spa_thread *t = (struct spa_thread*)thread;
	pw_log_info("acquire %p", t);
	return_val_if_fail(globals.thread_utils != NULL, -1);
	return_val_if_fail(t != NULL, -1);
	return spa_thread_utils_acquire_rt(globals.thread_utils, t, priority);
}

SPA_EXPORT
int jack_drop_real_time_scheduling (jack_native_thread_t thread)
{
	struct spa_thread *t = (struct spa_thread*)thread;
	pw_log_info("drop %p", t);
	return_val_if_fail(globals.thread_utils != NULL, -1);
	return_val_if_fail(t != NULL, -1);
	return spa_thread_utils_drop_rt(globals.thread_utils, t);
}

/**
 * Create a thread for JACK or one of its clients.  The thread is
 * created executing @a start_routine with @a arg as its sole
 * argument.
 *
 * @param client the JACK client for whom the thread is being created. May be
 * NULL if the client is being created within the JACK server.
 * @param thread place to return POSIX thread ID.
 * @param priority thread priority, if realtime.
 * @param realtime true for the thread to use realtime scheduling.  On
 * some systems that may require special privileges.
 * @param start_routine function the thread calls when it starts.
 * @param arg parameter passed to the @a start_routine.
 *
 * @returns 0, if successful; otherwise some error number.
 */
SPA_EXPORT
int jack_client_create_thread (jack_client_t* client,
                               jack_native_thread_t *thread,
                               int priority,
                               int realtime,	/* boolean */
                               void *(*start_routine)(void*),
                               void *arg)
{
	struct client *c = (struct client *) client;
	int res = 0;
	struct spa_thread *thr;

	return_val_if_fail(client != NULL, -EINVAL);
	return_val_if_fail(thread != NULL, -EINVAL);
	return_val_if_fail(start_routine != NULL, -EINVAL);

	pw_log_info("client %p: create thread rt:%d prio:%d", client, realtime, priority);

	thr = spa_thread_utils_create(&c->context.thread_utils, NULL, start_routine, arg);
	if (thr == NULL)
		res = -errno;
	*thread = (pthread_t)thr;

	if (res != 0) {
		pw_log_warn("client %p: create RT thread failed: %s",
				client, strerror(res));
	} else if (realtime) {
		/* Try to acquire RT scheduling, we don't fail here but the
		 * function will emit a warning. Real JACK fails here. */
		jack_acquire_real_time_scheduling(*thread, priority);
	}
	return res;
}

SPA_EXPORT
int jack_client_stop_thread(jack_client_t* client, jack_native_thread_t thread)
{
	struct client *c = (struct client *) client;
	void* status;

	if (thread == (jack_native_thread_t)NULL)
		return -EINVAL;

	return_val_if_fail(client != NULL, -EINVAL);

	pw_log_debug("join thread %p", (void *) thread);
	spa_thread_utils_join(&c->context.thread_utils, (struct spa_thread*)thread, &status);
	pw_log_debug("stopped thread %p", (void *) thread);
	return 0;
}

SPA_EXPORT
int jack_client_kill_thread(jack_client_t* client, jack_native_thread_t thread)
{
	struct client *c = (struct client *) client;
	void* status;

	if (thread == (jack_native_thread_t)NULL)
		return -EINVAL;

	return_val_if_fail(client != NULL, -EINVAL);

	pw_log_debug("cancel thread %p", (void *) thread);
	pthread_cancel(thread);
	pw_log_debug("join thread %p", (void *) thread);
	spa_thread_utils_join(&c->context.thread_utils, (struct spa_thread*)thread, &status);
	pw_log_debug("stopped thread %p", (void *) thread);
	return 0;
}

SPA_EXPORT
void jack_set_thread_creator (jack_thread_creator_t creator)
{
	globals.creator = creator;
}

static inline uint8_t * midi_event_data (void* port_buffer,
                      const struct midi_event* event)
{
        if (SPA_LIKELY(event->size <= MIDI_INLINE_MAX))
                return (uint8_t *)event->inline_data;
        else
                return SPA_PTROFF(port_buffer, event->byte_offset, uint8_t);
}

SPA_EXPORT
uint32_t jack_midi_get_event_count(void* port_buffer)
{
	struct midi_buffer *mb = port_buffer;
	if (mb == NULL || mb->magic != MIDI_BUFFER_MAGIC)
		return 0;
	return mb->event_count;
}

SPA_EXPORT
int jack_midi_event_get(jack_midi_event_t *event,
			void        *port_buffer,
			uint32_t    event_index)
{
	struct midi_buffer *mb = port_buffer;
	struct midi_event *ev = SPA_PTROFF(mb, sizeof(*mb), struct midi_event);
	if (mb == NULL || mb->magic != MIDI_BUFFER_MAGIC)
		return -EINVAL;
	if (event_index >= mb->event_count)
		return -ENOBUFS;
	ev += event_index;
	event->time = ev->time;
	event->size = ev->size;
	event->buffer = midi_event_data (port_buffer, ev);
	return 0;
}

SPA_EXPORT
void jack_midi_clear_buffer(void *port_buffer)
{
	struct midi_buffer *mb = port_buffer;
	if (mb == NULL || mb->magic != MIDI_BUFFER_MAGIC)
		return;
	mb->event_count = 0;
	mb->write_pos = 0;
	mb->lost_events = 0;
}

SPA_EXPORT
void jack_midi_reset_buffer(void *port_buffer)
{
	midi_init_buffer(port_buffer, globals.max_frames, globals.max_frames);
}

SPA_EXPORT
size_t jack_midi_max_event_size(void* port_buffer)
{
	struct midi_buffer *mb = port_buffer;
	size_t buffer_size;

	if (mb == NULL || mb->magic != MIDI_BUFFER_MAGIC)
		return 0;

	buffer_size = mb->buffer_size;

        /* (event_count + 1) below accounts for jack_midi_port_internal_event_t
         * which would be needed to store the next event */
        size_t used_size = sizeof(struct midi_buffer)
                           + mb->write_pos
                           + ((mb->event_count + 1)
                              * sizeof(struct midi_event));

        if (SPA_UNLIKELY(used_size > buffer_size)) {
                return 0;
        } else if (SPA_LIKELY((buffer_size - used_size) < MIDI_INLINE_MAX)) {
                return MIDI_INLINE_MAX;
        } else {
                return buffer_size - used_size;
        }
}

static inline int midi_buffer_check(void *port_buffer, jack_nframes_t  time)
{
	struct midi_buffer *mb = port_buffer;
	struct midi_event *events;

	if (SPA_UNLIKELY(mb == NULL)) {
		pw_log_warn("port buffer is NULL");
		return -EINVAL;
	}
	if (SPA_UNLIKELY(mb->magic != MIDI_BUFFER_MAGIC)) {
		pw_log_warn("port buffer is invalid");
		return -EINVAL;
	}
	if (SPA_UNLIKELY(time >= mb->nframes)) {
		pw_log_warn("midi %p: time:%d frames:%d", port_buffer, time, mb->nframes);
		return -EINVAL;
	}
	events = SPA_PTROFF(mb, sizeof(*mb), struct midi_event);
	if (SPA_UNLIKELY(mb->event_count > 0 && time < events[mb->event_count - 1].time)) {
		pw_log_warn("midi %p: time:%d ev:%d", port_buffer, time, mb->event_count);
		return -EINVAL;
	}
	return 0;
}

SPA_EXPORT
jack_midi_data_t* jack_midi_event_reserve(void *port_buffer,
                        jack_nframes_t  time,
                        size_t data_size)
{
	struct midi_buffer *mb = port_buffer;
	jack_midi_data_t *res;

	if (midi_buffer_check(port_buffer, time) < 0)
		goto failed;

	res = midi_event_reserve(port_buffer, time, data_size);
	if (res != NULL)
		return res;
failed:
	mb->lost_events++;
	return NULL;
}

SPA_EXPORT
int jack_midi_event_write(void *port_buffer,
                      jack_nframes_t time,
                      const jack_midi_data_t *data,
                      size_t data_size)
{
	jack_midi_data_t *ptr;
	int res;

	if ((res = midi_buffer_check(port_buffer, time)) < 0)
		return res;

	if ((ptr = midi_event_reserve(port_buffer, time, data_size)) == NULL)
		return -ENOBUFS;

	memcpy (ptr, data, data_size);
	return 0;
}

SPA_EXPORT
uint32_t jack_midi_get_lost_event_count(void *port_buffer)
{
	struct midi_buffer *mb = port_buffer;
	if (mb == NULL || mb->magic != MIDI_BUFFER_MAGIC)
		return 0;
	return mb->lost_events;
}

/** extensions */

SPA_EXPORT
int jack_get_video_image_size(jack_client_t *client, jack_image_size_t *size)
{
	struct client *c = (struct client *) client;
	struct pw_node_activation *a;

	return_val_if_fail(c != NULL, 0);

	a = c->rt.driver_activation;
	if (SPA_UNLIKELY(a == NULL))
		a = c->activation;
	if (SPA_UNLIKELY(a == NULL))
		return -EIO;

	if (SPA_UNLIKELY(!(a->position.video.flags & SPA_IO_VIDEO_SIZE_VALID)))
		return -EIO;

	size->width = a->position.video.size.width;
	size->height = a->position.video.size.height;
	size->stride = a->position.video.stride;
	size->flags = 0;
	return size->stride * size->height;
}


static void reg(void) __attribute__ ((constructor));
static void reg(void)
{
	pw_init(NULL, NULL);
	PW_LOG_TOPIC_INIT(jack_log_topic);
	pthread_mutex_init(&globals.lock, NULL);
	pw_array_init(&globals.descriptions, 16);
	spa_list_init(&globals.free_objects);
}
static void unreg(void) __attribute__ ((destructor));
static void unreg(void)
{
	struct object *o, *to;
	pthread_mutex_lock(&globals.lock);
	spa_list_for_each_safe(o, to, &globals.free_objects, link) {
		if (!o->to_free)
			spa_list_remove(&o->link);
	}
	spa_list_consume(o, &globals.free_objects, link) {
		spa_list_remove(&o->link);
		free(o);
	}
	pthread_mutex_unlock(&globals.lock);
	pw_deinit();
}
