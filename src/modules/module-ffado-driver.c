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
#include <spa/debug/types.h>
#include <spa/pod/builder.h>
#include <spa/param/audio/format-utils.h>
#include <spa/param/latency-utils.h>
#include <spa/param/audio/raw.h>

#include <pipewire/impl.h>
#include <pipewire/i18n.h>
#include <pipewire/thread.h>

#include <libffado/ffado.h>

/** \page page_module_ffado_driver FFADO firewire audio driver
 *
 * The ffado-driver module provides a source or sink using the libffado library for
 * reading and writing to firewire audio devices.
 *
 * ## Module Name
 *
 * `libpipewire-module-ffado-driver`
 *
 * ## Module Options
 *
 * - `driver.mode`: the driver mode, sink|source|duplex, default duplex
 * - `ffado.devices`: array of devices to open, default "hw:0"
 * - `ffado.period-size`: period size,default 1024
 * - `ffado.period-num`: period number,default 3
 * - `ffado.sample-rate`: sample-rate, default 48000
 * - `ffado.slave-mode`: slave mode
 * - `ffado.snoop-mode`: snoop mode
 * - `ffado.verbose`: ffado verbose level
 * - `ffado.rtprio`: ffado realtime priority, this is by default the PipeWire server
 *                   priority + 5
 * - `ffado.realtime`: ffado realtime mode. this requires correctly configured rlimits
 *                     to acquire FIFO scheduling at the ffado.rtprio priority
 * - `latency.internal.input`: extra input latency in frames
 * - `latency.internal.output`: extra output latency in frames
 * - `source.props`: Extra properties for the source filter
 * - `sink.props`: Extra properties for the sink filter
 *
 * ## General options
 *
 * Options with well-known behavior.
 *
 * - \ref PW_KEY_REMOTE_NAME
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
 * context.modules = [
 * {   name = libpipewire-module-ffado-driver
 *     args = {
 *         #driver.mode       = duplex
 *         #ffado.devices     = [ "hw:0" ]
 *         #ffado.period-size = 1024
 *         #ffado.period-num  = 3
 *         #ffado.sample-rate = 48000
 *         #ffado.slave-mode  = false
 *         #ffado.snoop-mode  = false
 *         #ffado.verbose     = 0
 *         #ffado.rtprio      = 65
 *         #ffado.realtime    = true
 *         #latency.internal.input  = 0
 *         #latency.internal.output = 0
 *         #audio.position    = [ FL FR ]
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

#define NAME "ffado-driver"

PW_LOG_TOPIC_STATIC(mod_topic, "mod." NAME);
#define PW_LOG_TOPIC_DEFAULT mod_topic

#define MAX_PORTS	128
#define FFADO_RT_PRIORITY_PACKETIZER_RELATIVE   5

#define DEFAULT_DEVICES		"[ \"hw:0\" ]"
#define DEFAULT_PERIOD_SIZE	1024
#define DEFAULT_PERIOD_NUM	3
#define DEFAULT_SAMPLE_RATE	48000
#define DEFAULT_SLAVE_MODE	false
#define DEFAULT_SNOOP_MODE	false
#define DEFAULT_VERBOSE		0
#define DEFAULT_RTPRIO		(RTPRIO_SERVER + FFADO_RT_PRIORITY_PACKETIZER_RELATIVE)
#define DEFAULT_REALTIME	true

#define DEFAULT_POSITION	"[ FL FR ]"
#define DEFAULT_MIDI_PORTS	1

#define MODULE_USAGE	"( remote.name=<remote> ) "					\
			"( driver.mode=<sink|source|duplex, default duplex> ) "		\
			"( ffado.devices=<devices array, default "DEFAULT_DEVICES"> ) "	\
			"( ffado.period-size=<period size, default 1024> ) "		\
			"( ffado.period-num=<period num, default 3> ) "			\
			"( ffado.sample-rate=<sampe rate, default 48000> ) "		\
			"( ffado.slave-mode=<slave mode, default false> ) "		\
			"( ffado.snoop-mode=<snoop mode, default false> ) "		\
			"( ffado.verbose=<verbose level, default 0> ) "			\
			"( ffado.rtprio=<realtime priority, default "SPA_STRINGIFY(DEFAULT_RTPRIO)"> ) "	\
			"( ffado.realtime=<realtime mode, default true> ) "		\
			"( audio.position=<channel map> ) "				\
			"( source.props=<properties> ) "				\
			"( sink.props=<properties> ) "


static const struct spa_dict_item module_props[] = {
	{ PW_KEY_MODULE_AUTHOR, "Wim Taymans <wim.taymans@gmail.com>" },
	{ PW_KEY_MODULE_DESCRIPTION, "Create an FFADO based driver" },
	{ PW_KEY_MODULE_USAGE, MODULE_USAGE },
	{ PW_KEY_MODULE_VERSION, PACKAGE_VERSION },
};

struct port_data {
	struct port *port;
};

struct port {
	enum spa_direction direction;
	ffado_streaming_stream_type stream_type;
	char name[280];

	struct spa_latency_info latency[2];
	bool latency_changed[2];
	unsigned int is_midi:1;
	unsigned int cleared:1;
	void *buffer;

	uint8_t event_byte;
	uint8_t event_type;
	uint32_t event_time;
	uint8_t event_buffer[512];
	uint32_t event_pos;
	int event_pending;

	struct port_data *data;
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
	uint32_t n_ports;
	struct port *ports[MAX_PORTS];
	struct volume volume;

	unsigned int ready:1;
	unsigned int running:1;
	unsigned int transfered:1;
};

struct impl {
	struct pw_context *context;
	struct pw_loop *main_loop;
	struct pw_loop *data_loop;
	struct spa_system *system;
	struct spa_source *ffado_timer;

	ffado_device_info_t device_info;
	ffado_options_t device_options;
	ffado_device_t *dev;

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

	uint32_t reset_work_id;

	struct spa_io_position *position;

	uint32_t latency[2];

	struct stream source;
	struct stream sink;

	char *devices[FFADO_MAX_SPECSTRINGS];
	uint32_t n_devices;
	int32_t sample_rate;
	int32_t period_size;
	int32_t n_periods;
	bool slave_mode;
	bool snoop_mode;
	uint32_t verbose;
	int32_t rtprio;
	bool realtime;

	uint32_t input_latency;
	uint32_t output_latency;
	uint32_t quantum_limit;

	uint32_t pw_xrun;
	uint32_t ffado_xrun;
	uint32_t frame_time;

	unsigned int do_disconnect:1;
	unsigned int fix_midi:1;
	unsigned int started:1;

	pthread_t thread;

	unsigned int done:1;
	unsigned int triggered:1;
	unsigned int new_xrun:1;
};

static int stop_ffado_device(struct impl *impl);
static int start_ffado_device(struct impl *impl);
static void schedule_reset_ffado_device(struct impl *impl);

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

static void clear_port_buffer(struct port *p, uint32_t n_samples)
{
	if (!p->cleared) {
		if (p->buffer)
			memset(p->buffer, 0, n_samples * sizeof(uint32_t));
		p->cleared = true;
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

static void midi_to_ffado(struct port *p, float *src, uint32_t n_samples)
{
	struct spa_pod *pod;
	struct spa_pod_sequence *seq;
	struct spa_pod_control *c;
	uint32_t i, index = 0, unhandled = 0;
	uint32_t *dst = p->buffer;

	if (src == NULL)
		return;

	if ((pod = spa_pod_from_data(src, n_samples * sizeof(float), 0, n_samples * sizeof(float))) == NULL)
		return;
	if (!spa_pod_is_sequence(pod))
		return;

	seq = (struct spa_pod_sequence*)pod;

	clear_port_buffer(p, n_samples);

	/* first leftovers from previous cycle, always start at offset 0 */
	for (i = 0; i < p->event_pos; i++) {
		dst[index] = 0x01000000 | (uint32_t) p->event_buffer[i];
		index += 8;
	}
	p->event_pos = 0;

	SPA_POD_SEQUENCE_FOREACH(seq, c) {
		switch(c->type) {
		case SPA_CONTROL_Midi:
		{
			uint8_t *data = SPA_POD_BODY(&c->value);
			size_t size = SPA_POD_BODY_SIZE(&c->value);

			if (index < c->offset)
				index = SPA_ROUND_UP_N(c->offset, 8);
			for (i = 0; i < size; i++) {
				if (index >= n_samples) {
					/* keep events that don't fit for the next cycle */
					if (p->event_pos < sizeof(p->event_buffer))
						p->event_buffer[p->event_pos++] = data[i];
					else
						unhandled++;
				}
				else
					dst[index] = 0x01000000 | (uint32_t) data[i];
				index += 8;
			}
			break;
		}
		default:
			break;
		}
	}
	if (unhandled > 0)
		pw_log_warn("%u MIDI events dropped (index %d)", unhandled, index);
	else if (p->event_pos > 0)
		pw_log_debug("%u MIDI events saved (index %d)", p->event_pos, index);
}

static int take_bytes(struct port *p, uint32_t *frame, uint8_t **bytes, size_t *size)
{
	if (p->event_pos == 0)
		return 0;
	*frame = p->event_time;
	*bytes = p->event_buffer;
	*size = p->event_pos;
	return 1;
}

static const int status_len[] = {
	2,		/* noteoff */
	2,		/* noteon */
	2,		/* keypress */
	2,		/* controller */
	1,		/* pgmchange */
	1,		/* chanpress */
	2,		/* pitchbend */
	-1,		/* invalid */
	1,		/* sysex 0xf0 */
	1,		/* qframe 0xf1 */
	2,		/* songpos 0xf2 */
	1,		/* songsel 0xf3 */
	-1,		/* none 0xf4 */
	-1,		/* none 0xf5 */
	0,		/* tune request 0xf6 */
	-1,		/* none 0xf7 */
	0,		/* clock 0xf8 */
	-1,		/* none 0xf9 */
	0,		/* start 0xfa */
	0,		/* continue 0xfb */
	0,		/* stop 0xfc */
	-1,		/* none 0xfd */
	0,		/* sensing 0xfe */
	0,		/* reset 0xff */
};

static int process_byte(struct port *p, uint32_t time, uint8_t byte,
		uint32_t *frame, uint8_t **bytes, size_t *size)
{
	int res = 0;
	if (byte >= 0xf8) {
		if (byte == 0xfd) {
			pw_log_warn("dropping invalid MIDI status bytes %08x", byte);
			return false;
		}
		p->event_byte = byte;
		*frame = time;
		*bytes = &p->event_byte;
		*size = 1;
		return 1;
	}
	if ((byte & 0x80) && (byte != 0xf7 || p->event_type != 8)) {
		if (p->event_pending > 0)
			pw_log_warn("incomplete MIDI message %02x dropped %u time:%u",
					p->event_type, p->event_pending, time);
		/* new command */
		p->event_buffer[0] = byte;
		p->event_time = time;
		if ((byte & 0xf0) == 0xf0) /* system message */
			p->event_type = (byte & 0x0f) + 8;
		else
			p->event_type = (byte >> 4) & 0x07;
		p->event_pos = 1;
		p->event_pending = status_len[p->event_type];
	} else {
		 if (p->event_pending > 0) {
			/* rest of command */
			if (p->event_pos < sizeof(p->event_buffer))
				p->event_buffer[p->event_pos++] = byte;
			if (p->event_type != 8)
				p->event_pending--;
		} else {
			/* running status */
			p->event_buffer[1] = byte;
			p->event_time = time;
			p->event_pending = status_len[p->event_type] - 1;
			p->event_pos = 2;
		}
	}
	if (p->event_pending == 0) {
		res = take_bytes(p, frame, bytes, size);
		if (p->event_type >= 8)
			p->event_type = 7;
	} else if (p->event_type == 8) {
		if (byte == 0xf7 || p->event_pos >= sizeof(p->event_buffer)) {
			res = take_bytes(p, frame, bytes, size);
			p->event_pos = 0;
			if (byte == 0xf7) {
				p->event_pending = 0;
				p->event_type = 7;
			}
		}
	}
	return res;
}

static void ffado_to_midi(struct port *p, float *dst, uint32_t *src, uint32_t size)
{
	struct spa_pod_builder b = { 0, };
	uint32_t i, count;
	struct spa_pod_frame f;

	count = src ? size : 0;

	spa_pod_builder_init(&b, dst, size);
	spa_pod_builder_push_sequence(&b, &f, 0);
	for (i = 0; i < count; i++) {
		uint32_t data = src[i], frame;
		uint8_t *bytes;
		size_t size;

		if ((data & 0xff000000) == 0)
			continue;

		if (process_byte(p, i, data & 0xff, &frame, &bytes, &size)) {
			spa_pod_builder_control(&b, frame, SPA_CONTROL_Midi);
	                spa_pod_builder_bytes(&b, bytes, size);
		}
        }
	spa_pod_builder_pop(&b, &f);
	if (p->event_pending > 0)
		/* make sure the rest of the MIDI message is sent first in the next cycle */
		p->event_time = 0;
}

static inline uint64_t get_time_ns(struct impl *impl)
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

static int set_timeout(struct impl *impl, uint64_t time)
{
	struct timespec timeout, interval;
	timeout.tv_sec = time / SPA_NSEC_PER_SEC;
	timeout.tv_nsec = time % SPA_NSEC_PER_SEC;
	interval.tv_sec = 0;
	interval.tv_nsec = 0;
	pw_loop_update_timer(impl->data_loop,
                                impl->ffado_timer, &timeout, &interval, true);
	return 0;
}

static void stream_destroy(void *d)
{
	struct stream *s = d;
	uint32_t i;
	for (i = 0; i < s->n_ports; i++) {
		struct port *p = s->ports[i];
		if (p != NULL) {
			s->ports[i] = NULL;
			free(p->buffer);
			free(p);
		}
	}
	s->n_ports = 0;
	spa_hook_remove(&s->listener);
	s->filter = NULL;
	s->ready = false;
	s->running = false;
}

static void check_start(struct impl *impl)
{
	if ((!(impl->mode & MODE_SINK) || (impl->sink.ready && impl->sink.running)) &&
	    (!(impl->mode & MODE_SOURCE) || (impl->source.ready && impl->source.running)))
		start_ffado_device(impl);
}

static void stream_state_changed(void *d, enum pw_filter_state old,
		enum pw_filter_state state, const char *error)
{
	struct stream *s = d;
	struct impl *impl = s->impl;
	switch (state) {
	case PW_FILTER_STATE_ERROR:
		pw_log_error("filter state %d error: %s", state, error);
		SPA_FALLTHROUGH;
	case PW_FILTER_STATE_UNCONNECTED:
		pw_impl_module_schedule_destroy(impl->module);
		break;
	case PW_FILTER_STATE_PAUSED:
		s->running = false;
		if (!impl->sink.running && !impl->source.running)
			stop_ffado_device(impl);
		break;
	case PW_FILTER_STATE_STREAMING:
		s->running = true;
		check_start(impl);
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

	pw_log_trace_fp("process %d", impl->triggered);
	if (impl->mode == MODE_SINK && impl->triggered) {
		impl->triggered = false;
		return;
	}

	for (i = 0; i < s->n_ports; i++) {
		struct port *p = s->ports[i];
		float *src;
		if (p == NULL || p->data == NULL)
			continue;

		src = pw_filter_get_dsp_buffer(p->data, n_samples);
		if (src == NULL) {
			clear_port_buffer(p, n_samples);
			continue;
		}

		if (SPA_UNLIKELY(p->is_midi))
			midi_to_ffado(p, src, n_samples);
		else
			do_volume(p->buffer, src, &s->volume, i, n_samples);

		p->cleared = false;
	}
	ffado_streaming_transfer_playback_buffers(impl->dev);
	s->transfered = true;

	if (impl->mode == MODE_SINK) {
		pw_log_trace_fp("done %u", impl->frame_time);
		impl->done = true;
		set_timeout(impl, position->clock.nsec);
	}
}

static void silence_playback(struct impl *impl)
{
	uint32_t i;
	struct stream *s = &impl->sink;

	for (i = 0; i < s->n_ports; i++) {
		struct port *p = s->ports[i];
		if (p != NULL)
			clear_port_buffer(p, impl->period_size);
	}
	ffado_streaming_transfer_playback_buffers(impl->dev);
	s->transfered = true;
}

static void source_process(void *d, struct spa_io_position *position)
{
	struct stream *s = d;
	struct impl *impl = s->impl;
	uint32_t i, n_samples = position->clock.duration;

	pw_log_trace_fp("process %d", impl->triggered);

	if (!impl->triggered) {
		pw_log_trace_fp("done %u", impl->frame_time);
		impl->done = true;
		if (!impl->sink.transfered)
			silence_playback(impl);
		set_timeout(impl, position->clock.nsec);
		return;
	}

	impl->triggered = false;

	ffado_streaming_transfer_capture_buffers(impl->dev);
	s->transfered = true;

	for (i = 0; i < s->n_ports; i++) {
		struct port *p = s->ports[i];
		float *dst;

		if (p == NULL || p->data == NULL || p->buffer == NULL)
			continue;

		dst = pw_filter_get_dsp_buffer(p->data, n_samples);
		if (dst == NULL)
			continue;

		if (SPA_UNLIKELY(p->is_midi))
			ffado_to_midi(p, dst, p->buffer, n_samples);
		else
			do_volume(dst, p->buffer, &s->volume, i, n_samples);
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
		struct port_data *data)
{
	struct port *port = data->port;
	struct spa_latency_info latency;
	bool update = false;
	enum spa_direction direction = port->direction;

	if (param == NULL || spa_latency_parse(param, &latency) < 0)
		return;

	if (spa_latency_info_compare(&port->latency[direction], &latency)) {
		port->latency[direction] = latency;
		port->latency_changed[direction] = update = true;
	}
}

static int make_stream_ports(struct stream *s)
{
	struct impl *impl = s->impl;
	struct pw_properties *props;
	uint8_t buffer[1024];
	struct spa_pod_builder b;
	struct spa_latency_info latency;
	const struct spa_pod *params[2];
	uint32_t i, n_params = 0, n_channels = 0;
	bool is_midi;

	for (i = 0; i < s->n_ports; i++) {
		struct port *port = s->ports[i];
		if (port->data != NULL) {
			free(port->buffer);
			pw_filter_remove_port(port->data);
			port->data = NULL;
		}
	}
	for (i = 0; i < s->n_ports; i++) {
		struct port *port = s->ports[i];
		char channel[32];

		snprintf(channel, sizeof(channel), "AUX%u", n_channels % SPA_AUDIO_MAX_CHANNELS);

		switch (port->stream_type) {
		case ffado_stream_type_audio:
			props = pw_properties_new(
					PW_KEY_FORMAT_DSP, "32 bit float mono audio",
					PW_KEY_PORT_PHYSICAL, "true",
					PW_KEY_PORT_TERMINAL, "true",
					PW_KEY_PORT_NAME, port->name,
					PW_KEY_AUDIO_CHANNEL, channel,
					NULL);
			is_midi = false;
			n_channels++;
			break;
		case ffado_stream_type_midi:
			props = pw_properties_new(
					PW_KEY_FORMAT_DSP, "8 bit raw midi",
					PW_KEY_PORT_NAME, port->name,
					PW_KEY_PORT_PHYSICAL, "true",
					PW_KEY_PORT_TERMINAL, "true",
					PW_KEY_PORT_CONTROL, "true",
					NULL);

			is_midi = true;
			break;
		default:
			pw_log_info("not registering unknown stream %d %s (type %d)", i,
					port->name, port->stream_type);
			continue;

		}
		latency = SPA_LATENCY_INFO(s->direction,
				.min_quantum = 1,
				.max_quantum = 1,
				.min_rate = impl->latency[s->direction],
				.max_rate = impl->latency[s->direction]);

		spa_pod_builder_init(&b, buffer, sizeof(buffer));
		n_params = 0;
		params[n_params++] = spa_latency_build(&b, SPA_PARAM_Latency, &latency);

		port->data = pw_filter_add_port(s->filter,
                        s->direction,
                        PW_FILTER_PORT_FLAG_MAP_BUFFERS,
                        sizeof(struct port_data), props, params, n_params);
		if (port->data == NULL) {
			pw_log_error("Can't create port: %m");
			return -errno;
		}
		port->data->port = port;

		port->latency[s->direction] = latency;
		port->is_midi = is_midi;
		port->buffer = calloc(impl->quantum_limit, sizeof(float));
		if (port->buffer == NULL) {
			pw_log_error("Can't create port buffer: %m");
			return -errno;
		}
	}
	return 0;
}

static void setup_stream_ports(struct stream *s)
{
	struct impl *impl = s->impl;
	uint32_t i;
	for (i = 0; i < s->n_ports; i++) {
		struct port *port = s->ports[i];
		if (s->direction == PW_DIRECTION_INPUT) {
			if (ffado_streaming_set_playback_stream_buffer(impl->dev, i, port->buffer))
				pw_log_error("cannot configure port buffer for %s", port->name);

			if (ffado_streaming_playback_stream_onoff(impl->dev, i, 1))
				pw_log_error("cannot enable port %s", port->name);
		} else {
			if (ffado_streaming_set_capture_stream_buffer(impl->dev, i, port->buffer))
				pw_log_error("cannot configure port buffer for %s", port->name);

			if (ffado_streaming_capture_stream_onoff(impl->dev, i, 1))
				pw_log_error("cannot enable port %s", port->name);
		}
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
			if (make_stream_ports(s) >= 0) {
				s->ready = true;
				check_start(s->impl);
			}
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
	struct spa_latency_info latency;

	spa_zero(latency);
	n_params = 0;
	spa_pod_builder_init(&b, buffer, sizeof(buffer));

	s->filter = pw_filter_new(impl->core, name, pw_properties_copy(s->props));
	if (s->filter == NULL)
		return -errno;

	spa_zero(s->listener);
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

static void destroy_stream(struct stream *s)
{
	if (s->filter)
		pw_filter_destroy(s->filter);
}

static void on_ffado_timeout(void *data, uint64_t expirations)
{
	struct impl *impl = data;
	bool source_running, sink_running;
	uint64_t nsec;
	ffado_wait_response response;

	pw_log_trace_fp("wakeup %d", impl->done);

	if (!impl->done) {
		impl->pw_xrun++;
		impl->new_xrun = true;
		ffado_streaming_reset(impl->dev);
	}
again:
	pw_log_trace_fp("FFADO wait");
	response = ffado_streaming_wait(impl->dev);
	nsec = get_time_ns(impl);

	switch (response) {
	case ffado_wait_ok:
		break;
	case ffado_wait_xrun:
		pw_log_debug("FFADO xrun");
		impl->ffado_xrun++;
		impl->new_xrun = true;
		goto again;
	case ffado_wait_shutdown:
		pw_log_info("FFADO shutdown");
		return;
	case ffado_wait_error:
	default:
		pw_log_error("FFADO error");
		return;
	}
	source_running = impl->source.running && impl->sink.ready;
	sink_running = impl->sink.running && impl->source.ready;

	impl->source.transfered = false;
	impl->sink.transfered = false;

	if (!source_running) {
		ffado_streaming_transfer_capture_buffers(impl->dev);
		impl->source.transfered = true;
	}
	if (!sink_running)
		silence_playback(impl);

	pw_log_trace_fp("process %d %u %u %p %d %"PRIu64, impl->period_size, source_running,
			sink_running, impl->position, impl->frame_time, nsec);

	if (impl->new_xrun) {
		pw_log_warn("Xrun FFADO:%u PipeWire:%u source:%d sink:%d",
				impl->ffado_xrun, impl->pw_xrun, source_running, sink_running);
		impl->new_xrun = false;
	}

	if (impl->position) {
		struct spa_io_clock *c = &impl->position->clock;

#if 0
		if (c->target_duration != (uint64_t) impl->period_size) {
			ffado_streaming_transfer_capture_buffers(impl->dev);
			silence_playback(impl);

			if (ffado_streaming_set_period_size(impl->dev, c->target_duration) != 0) {
				pw_log_warn("can't change period size");
			} else {
				sleep(1);
				impl->period_size = c->target_duration;
			}
			goto again;
		}
#endif

		c->nsec = nsec;
		c->rate = SPA_FRACTION(1, impl->sample_rate);
		c->position += impl->period_size;
		c->duration = impl->period_size;
		c->delay = 0;
		c->rate_diff = 1.0;
		c->next_nsec = nsec + (c->duration * SPA_NSEC_PER_SEC) / impl->sample_rate;

		c->target_rate = c->rate;
		c->target_duration = c->duration;
	}
	if (impl->mode & MODE_SOURCE && source_running) {
		impl->done = false;
		impl->triggered = true;
		set_timeout(impl, nsec + SPA_NSEC_PER_SEC);
		pw_filter_trigger_process(impl->source.filter);
	} else if (impl->mode == MODE_SINK && sink_running) {
		impl->done = false;
		impl->triggered = true;
		set_timeout(impl, nsec + SPA_NSEC_PER_SEC);
		pw_filter_trigger_process(impl->sink.filter);
	} else {
		impl->done = true;
		set_timeout(impl, nsec);
	}
}

static void close_ffado_device(struct impl *impl)
{
	if (impl->dev == NULL)
		return;

	stop_ffado_device(impl);
	ffado_streaming_finish(impl->dev);
	impl->dev = NULL;

	pw_log_info("closed FFADO device %s", impl->devices[0]);
}

static int open_ffado_device(struct impl *impl)
{
	int32_t target_rate, target_period;

	if (impl->dev != NULL)
		return 0;

	if (impl->position) {
		struct spa_io_clock *c = &impl->position->clock;
		target_rate = c->target_rate.denom;
		target_period = c->target_duration;
	} else {
		target_rate = impl->sample_rate;
		target_period = impl->period_size;
	}

	spa_zero(impl->device_info);
	impl->device_info.device_spec_strings = impl->devices;
	impl->device_info.nb_device_spec_strings = impl->n_devices;

	spa_zero(impl->device_options);
	impl->device_options.sample_rate = target_rate;
	impl->device_options.period_size = target_period;
	impl->device_options.nb_buffers = impl->n_periods;
	impl->device_options.realtime = impl->realtime;
	impl->device_options.packetizer_priority = impl->rtprio;
	impl->device_options.verbose = impl->verbose;
	impl->device_options.slave_mode = impl->slave_mode;
	impl->device_options.snoop_mode = impl->snoop_mode;

	impl->dev = ffado_streaming_init(impl->device_info, impl->device_options);
	if (impl->dev == NULL) {
		pw_log_error("can't open FFADO device %s", impl->devices[0]);
		return -EIO;
	}

	if (impl->device_options.realtime) {
		pw_log_info("Streaming thread running with Realtime scheduling, priority %d",
				impl->device_options.packetizer_priority);
	} else {
		pw_log_info("Streaming thread running without Realtime scheduling");
	}

	ffado_streaming_set_audio_datatype(impl->dev, ffado_audio_datatype_float);

	impl->sample_rate = impl->device_options.sample_rate;
	impl->period_size = impl->device_options.period_size;
	impl->source.info.rate = impl->sample_rate;
	impl->sink.info.rate = impl->sample_rate;

	impl->source.n_ports = ffado_streaming_get_nb_capture_streams(impl->dev);
	impl->sink.n_ports = ffado_streaming_get_nb_playback_streams(impl->dev);

	if (impl->source.n_ports == 0 && impl->sink.n_ports == 0) {
		close_ffado_device(impl);
		return -EIO;
	}

	pw_log_info("opened FFADO device %s source:%d sink:%d rate:%d period:%d %p",
			impl->devices[0], impl->source.n_ports, impl->sink.n_ports,
			impl->sample_rate, impl->period_size, impl->position);

	return 0;
}

static int probe_ffado_device(struct impl *impl)
{
	int res;
	uint32_t i, n_channels;
	struct port *port;
	char name[256];

	if ((res = open_ffado_device(impl)) < 0)
		return res;

	n_channels = 0;
	for (i = 0; i < impl->source.n_ports; i++) {
		port = calloc(1, sizeof(struct port));
		if (port == NULL)
			return -errno;

		port->direction = impl->source.direction;
		port->stream_type = ffado_streaming_get_capture_stream_type(impl->dev, i);
		ffado_streaming_get_capture_stream_name(impl->dev, i, name, sizeof(name));
		snprintf(port->name, sizeof(port->name), "%s_out", name);

		switch (port->stream_type) {
		case ffado_stream_type_audio:
			n_channels++;
			break;
		default:
			break;
		}
		impl->source.ports[i] = port;
	}
	if (impl->source.info.channels != n_channels) {
		impl->source.info.channels = n_channels;
		for (i = 0; i < SPA_MIN(impl->source.info.channels, SPA_AUDIO_MAX_CHANNELS); i++)
			impl->source.info.position[i] = SPA_AUDIO_CHANNEL_AUX0 + i;
	}

	n_channels = 0;
	for (i = 0; i < impl->sink.n_ports; i++) {
		port = calloc(1, sizeof(struct port));
		if (port == NULL)
			return -errno;

		port->direction = impl->sink.direction;
		port->stream_type = ffado_streaming_get_playback_stream_type(impl->dev, i);
		ffado_streaming_get_playback_stream_name(impl->dev, i, name, sizeof(name));
		snprintf(port->name, sizeof(port->name), "%s_in", name);

		switch (port->stream_type) {
		case ffado_stream_type_audio:
			n_channels++;
			break;
		default:
			break;
		}
		impl->sink.ports[i] = port;
	}
	if (impl->sink.info.channels != n_channels) {
		impl->sink.info.channels = n_channels;
		for (i = 0; i < SPA_MIN(impl->sink.info.channels, SPA_AUDIO_MAX_CHANNELS); i++)
			impl->sink.info.position[i] = SPA_AUDIO_CHANNEL_AUX0 + i;
	}

	if (impl->mode & MODE_SINK) {
		if ((res = make_stream(&impl->sink, "FFADO Sink")) < 0)
			goto exit;
	}
	if (impl->mode & MODE_SOURCE) {
		if ((res = make_stream(&impl->source, "FFADO Source")) < 0)
			goto exit;
	}
exit:
	close_ffado_device(impl);

	return res;
}


static int start_ffado_device(struct impl *impl)
{
	int res;

	if (impl->started)
		return 0;

	if ((res = open_ffado_device(impl)) < 0)
		return res;

	setup_stream_ports(&impl->source);
	setup_stream_ports(&impl->sink);

	if (ffado_streaming_prepare(impl->dev)) {
		pw_log_error("Could not prepare streaming");
		schedule_reset_ffado_device(impl);
		return -EIO;
	}

	if (ffado_streaming_start(impl->dev)) {
		pw_log_warn("Could not start FFADO streaming, try reset");
		schedule_reset_ffado_device(impl);
		return -EIO;
	}
	pw_log_info("FFADO started streaming");

	impl->started = true;
	impl->done = true;
	set_timeout(impl, get_time_ns(impl));
	return 0;
}

static int stop_ffado_device(struct impl *impl)
{
	if (!impl->started)
		return 0;

	impl->started = false;
	set_timeout(impl, 0);
	if (ffado_streaming_stop(impl->dev))
		pw_log_error("Could not stop FFADO streaming");
	else
		pw_log_info("FFADO stopped streaming");

	close_ffado_device(impl);

	return 0;
}


static void do_reset_ffado(void *obj, void *data, int res, uint32_t id)
{
	struct impl *impl = obj;

	impl->reset_work_id = SPA_ID_INVALID;
	close_ffado_device(impl);
	open_ffado_device(impl);
}

static void schedule_reset_ffado_device(struct impl *impl)
{
	if (impl->reset_work_id != SPA_ID_INVALID)
		return;

	impl->reset_work_id = pw_work_queue_add(pw_context_get_work_queue(impl->context),
						  impl, 0, do_reset_ffado, NULL);
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
	uint32_t i;

	if (impl->reset_work_id != SPA_ID_INVALID)
		pw_work_queue_cancel(pw_context_get_work_queue(impl->context),
				     impl, SPA_ID_INVALID);

	close_ffado_device(impl);

	destroy_stream(&impl->source);
	destroy_stream(&impl->sink);

	if (impl->core && impl->do_disconnect)
		pw_core_disconnect(impl->core);
	if (impl->ffado_timer)
		pw_loop_destroy_source(impl->data_loop, impl->ffado_timer);

	if (impl->data_loop)
		pw_context_release_loop(impl->context, impl->data_loop);

	pw_properties_free(impl->sink.props);
	pw_properties_free(impl->source.props);
	pw_properties_free(impl->props);

	for (i = 0; i < impl->n_devices; i++)
		free(impl->devices[i]);
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

static uint32_t channel_from_name(const char *name)
{
	int i;
	for (i = 0; spa_type_audio_channel[i].name; i++) {
		if (spa_streq(name, spa_debug_type_short_name(spa_type_audio_channel[i].name)))
			return spa_type_audio_channel[i].type;
	}
	return SPA_AUDIO_CHANNEL_UNKNOWN;
}

static void parse_devices(struct impl *impl, const char *val, size_t len)
{
	struct spa_json it[2];
	char v[FFADO_MAX_SPECSTRING_LENGTH];

	spa_json_init(&it[0], val, len);
        if (spa_json_enter_array(&it[0], &it[1]) <= 0)
                spa_json_init(&it[1], val, len);

	impl->n_devices = 0;
	while (spa_json_get_string(&it[1], v, sizeof(v)) > 0 &&
	    impl->n_devices < FFADO_MAX_SPECSTRINGS) {
		impl->devices[impl->n_devices++] = strdup(v);
	}
}

static void parse_position(struct spa_audio_info_raw *info, const char *val, size_t len)
{
	struct spa_json it[2];
	char v[256];

	spa_json_init(&it[0], val, len);
        if (spa_json_enter_array(&it[0], &it[1]) <= 0)
                spa_json_init(&it[1], val, len);

	info->channels = 0;
	while (spa_json_get_string(&it[1], v, sizeof(v)) > 0 &&
	    info->channels < SPA_AUDIO_MAX_CHANNELS) {
		info->position[info->channels++] = channel_from_name(v);
	}
}

static void parse_audio_info(const struct pw_properties *props, struct spa_audio_info_raw *info)
{
	const char *str;

	spa_zero(*info);
	info->format = SPA_AUDIO_FORMAT_F32P;
	info->rate = 0;
	info->channels = pw_properties_get_uint32(props, PW_KEY_AUDIO_CHANNELS, info->channels);
	info->channels = SPA_MIN(info->channels, SPA_AUDIO_MAX_CHANNELS);
	if ((str = pw_properties_get(props, SPA_KEY_AUDIO_POSITION)) != NULL)
		parse_position(info, str, strlen(str));
	if (info->channels == 0)
		parse_position(info, DEFAULT_POSITION, strlen(DEFAULT_POSITION));
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
	str = pw_properties_get(props, "ffado.devices");
	if (str == NULL)
		str = DEFAULT_DEVICES;
	parse_devices(impl, str, strlen(str));

	impl->period_size = pw_properties_get_int32(props,
			"ffado.period-size", DEFAULT_PERIOD_SIZE);
	impl->n_periods = pw_properties_get_int32(props,
			"ffado.period-num", DEFAULT_PERIOD_NUM);
	impl->sample_rate = pw_properties_get_int32(props,
			"ffado.sample-rate", DEFAULT_SAMPLE_RATE);
	impl->slave_mode = pw_properties_get_bool(props,
			"ffado.slave-mode", DEFAULT_SLAVE_MODE);
	impl->snoop_mode = pw_properties_get_bool(props,
			"ffado.snoop-mode", DEFAULT_SNOOP_MODE);
	impl->verbose = pw_properties_get_uint32(props,
			"ffado.verbose", DEFAULT_VERBOSE);
	impl->rtprio = pw_properties_get_uint32(props,
			"ffado.rtprio", DEFAULT_RTPRIO);
	impl->realtime = pw_properties_get_bool(props,
			"ffado.realtime", DEFAULT_REALTIME);
	impl->input_latency = pw_properties_get_uint32(props,
			"latency.internal.input", 0);
	impl->output_latency = pw_properties_get_uint32(props,
			"latency.internal.output", 0);

	impl->quantum_limit = pw_properties_get_uint32(
			pw_context_get_properties(context),
			"default.clock.quantum-limit", 8192u);

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
	impl->data_loop = pw_context_acquire_loop(context, &props->dict);
	impl->system = impl->main_loop->system;
	impl->reset_work_id = SPA_ID_INVALID;

	impl->source.impl = impl;
	impl->source.direction = PW_DIRECTION_OUTPUT;
	impl->sink.impl = impl;
	impl->sink.direction = PW_DIRECTION_INPUT;

	impl->mode = MODE_DUPLEX;
	if ((str = pw_properties_get(props, "driver.mode")) != NULL) {
		if (spa_streq(str, "source")) {
			impl->mode = MODE_SOURCE;
		} else if (spa_streq(str, "sink")) {
			impl->mode = MODE_SINK;
		} else if (spa_streq(str, "duplex")) {
			impl->mode = MODE_DUPLEX;
		} else {
			pw_log_error("invalid driver.mode '%s'", str);
			res = -EINVAL;
			goto error;
		}
	}
	impl->ffado_timer = pw_loop_add_timer(impl->data_loop, on_ffado_timeout, impl);
	if (impl->ffado_timer == NULL) {
		pw_log_error("can't create ffado timer: %m");
		res = -errno;
		goto error;
	}

	pw_properties_set(props, PW_KEY_NODE_LOOP_NAME, impl->data_loop->name);
	if (pw_properties_get(props, PW_KEY_NODE_VIRTUAL) == NULL)
		pw_properties_set(props, PW_KEY_NODE_VIRTUAL, "true");
	if (pw_properties_get(props, PW_KEY_NODE_GROUP) == NULL)
		pw_properties_set(props, PW_KEY_NODE_GROUP, "ffado-group");
	if (pw_properties_get(props, PW_KEY_NODE_LINK_GROUP) == NULL)
		pw_properties_set(props, PW_KEY_NODE_LINK_GROUP, "ffado-group");
	if (pw_properties_get(props, PW_KEY_NODE_PAUSE_ON_IDLE) == NULL)
		pw_properties_set(props, PW_KEY_NODE_PAUSE_ON_IDLE, "false");

	pw_properties_set(impl->sink.props, PW_KEY_MEDIA_CLASS, "Audio/Sink");
	pw_properties_set(impl->sink.props, PW_KEY_PRIORITY_DRIVER, "35000");
	pw_properties_set(impl->sink.props, PW_KEY_PRIORITY_SESSION, "2000");
	pw_properties_set(impl->sink.props, PW_KEY_NODE_NAME, "ffado_sink");
	pw_properties_set(impl->sink.props, PW_KEY_NODE_DESCRIPTION, "FFADO Sink");

	pw_properties_set(impl->source.props, PW_KEY_MEDIA_CLASS, "Audio/Source");
	pw_properties_set(impl->source.props, PW_KEY_PRIORITY_DRIVER, "35001");
	pw_properties_set(impl->source.props, PW_KEY_PRIORITY_SESSION, "2001");
	pw_properties_set(impl->source.props, PW_KEY_NODE_NAME, "ffado_source");
	pw_properties_set(impl->source.props, PW_KEY_NODE_DESCRIPTION, "FFADO Source");

	if ((str = pw_properties_get(props, "sink.props")) != NULL)
		pw_properties_update_string(impl->sink.props, str, strlen(str));
	if ((str = pw_properties_get(props, "source.props")) != NULL)
		pw_properties_update_string(impl->source.props, str, strlen(str));

	copy_props(impl, props, PW_KEY_NODE_LOOP_NAME);
	copy_props(impl, props, PW_KEY_NODE_LINK_GROUP);
	copy_props(impl, props, PW_KEY_NODE_GROUP);
	copy_props(impl, props, PW_KEY_NODE_VIRTUAL);
	copy_props(impl, props, PW_KEY_NODE_PAUSE_ON_IDLE);

	parse_audio_info(impl->source.props, &impl->source.info);
	parse_audio_info(impl->sink.props, &impl->sink.info);

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

	if ((res = probe_ffado_device(impl)) < 0)
		goto error;

	pw_impl_module_add_listener(module, &impl->module_listener, &module_events, impl);

	pw_impl_module_update_properties(module, &SPA_DICT_INIT_ARRAY(module_props));

	return 0;

error:
	impl_destroy(impl);
	return res;
}
