/* Spa */
/* SPDX-FileCopyrightText: Copyright © 2022 Wim Taymans */
/* SPDX-License-Identifier: MIT */

#include <errno.h>
#include <string.h>
#include <stdio.h>
#include <limits.h>

#include <spa/support/plugin.h>
#include <spa/support/cpu.h>
#include <spa/support/loop.h>
#include <spa/support/log.h>
#include <spa/support/plugin-loader.h>
#include <spa/utils/result.h>
#include <spa/utils/list.h>
#include <spa/utils/json.h>
#include <spa/utils/names.h>
#include <spa/utils/string.h>
#include <spa/utils/ratelimit.h>
#include <spa/node/node.h>
#include <spa/node/io.h>
#include <spa/node/utils.h>
#include <spa/node/keys.h>
#include <spa/param/audio/format-utils.h>
#include <spa/param/audio/raw-json.h>
#include <spa/param/param.h>
#include <spa/param/latency-utils.h>
#include <spa/param/tag-utils.h>
#include <spa/pod/filter.h>
#include <spa/pod/dynamic.h>
#include <spa/debug/types.h>
#include <spa/control/ump-utils.h>
#include <spa/filter-graph/filter-graph.h>

#include "volume-ops.h"
#include "fmt-ops.h"
#include "channelmix-ops.h"
#include "resample.h"
#include "wavfile.h"

#undef SPA_LOG_TOPIC_DEFAULT
#define SPA_LOG_TOPIC_DEFAULT &log_topic
SPA_LOG_TOPIC_DEFINE_STATIC(log_topic, "spa.audioconvert");

#define DEFAULT_RATE		48000
#define DEFAULT_CHANNELS	2

#define MAX_ALIGN	FMT_OPS_MAX_ALIGN
#define MAX_BUFFERS	32
#define MAX_DATAS	SPA_AUDIO_MAX_CHANNELS
#define MAX_PORTS	(SPA_AUDIO_MAX_CHANNELS+1)
#define MAX_STAGES	64
#define MAX_GRAPH	9	/* 8 active + 1 replacement slot */

#define DEFAULT_MUTE		false
#define DEFAULT_VOLUME		VOLUME_NORM
#define DEFAULT_MIN_VOLUME	0.0
#define DEFAULT_MAX_VOLUME	10.0

struct volumes {
	bool mute;
	uint32_t n_volumes;
	float volumes[SPA_AUDIO_MAX_CHANNELS];
};

static void init_volumes(struct volumes *vol)
{
	uint32_t i;
	vol->mute = DEFAULT_MUTE;
	vol->n_volumes = 0;
	for (i = 0; i < SPA_AUDIO_MAX_CHANNELS; i++)
		vol->volumes[i] = DEFAULT_VOLUME;
}

struct volume_ramp_params {
	unsigned int volume_ramp_samples;
	unsigned int volume_ramp_step_samples;
	unsigned int volume_ramp_time;
	unsigned int volume_ramp_step_time;
	enum spa_audio_volume_ramp_scale scale;
};

struct props {
	float volume;
	float min_volume;
	float max_volume;
	float prev_volume;
	uint32_t n_channels;
	uint32_t channel_map[SPA_AUDIO_MAX_CHANNELS];
	struct volumes channel;
	struct volumes soft;
	struct volumes monitor;
	struct volume_ramp_params vrp;
	unsigned int have_soft_volume:1;
	unsigned int mix_disabled:1;
	unsigned int resample_disabled:1;
	unsigned int resample_quality;
	double rate;
	char wav_path[512];
	unsigned int lock_volumes:1;
	unsigned int filter_graph_disabled:1;
};

static void props_reset(struct props *props)
{
	uint32_t i;
	props->volume = DEFAULT_VOLUME;
	props->min_volume = DEFAULT_MIN_VOLUME;
	props->max_volume = DEFAULT_MAX_VOLUME;
	props->n_channels = 0;
	for (i = 0; i < SPA_AUDIO_MAX_CHANNELS; i++)
		props->channel_map[i] = SPA_AUDIO_CHANNEL_UNKNOWN;
	init_volumes(&props->channel);
	init_volumes(&props->soft);
	init_volumes(&props->monitor);
	props->have_soft_volume = false;
	props->mix_disabled = false;
	props->resample_disabled = false;
	props->resample_quality = RESAMPLE_DEFAULT_QUALITY;
	props->rate = 1.0;
	spa_zero(props->wav_path);
	props->lock_volumes = false;
	props->filter_graph_disabled = false;
}

struct buffer {
	uint32_t id;
#define BUFFER_FLAG_QUEUED	(1<<0)
	uint32_t flags;
	struct spa_list link;
	struct spa_buffer *buf;
	void *datas[MAX_DATAS];
};

struct port {
	uint32_t direction;
	uint32_t id;

	struct spa_io_buffers *io;

	uint64_t info_all;
	struct spa_port_info info;
#define IDX_EnumFormat	0
#define IDX_Meta	1
#define IDX_IO		2
#define IDX_Format	3
#define IDX_Buffers	4
#define IDX_Latency	5
#define IDX_Tag		6
#define N_PORT_PARAMS	7
	struct spa_param_info params[N_PORT_PARAMS];
	char position[16];

	struct buffer buffers[MAX_BUFFERS];
	uint32_t n_buffers;

	struct spa_latency_info latency[2];
	unsigned int have_latency:1;

	struct spa_audio_info format;
	unsigned int valid:1;
	unsigned int have_format:1;
	unsigned int is_dsp:1;
	unsigned int is_monitor:1;
	unsigned int is_control:1;

	uint32_t blocks;
	uint32_t stride;
	uint32_t maxsize;

	const struct spa_pod_sequence *ctrl;
	uint32_t ctrl_offset;

	struct spa_list queue;
};

struct dir {
	struct port *ports[MAX_PORTS];
	uint32_t n_ports;

	enum spa_direction direction;
	enum spa_param_port_config_mode mode;

	struct spa_audio_info format;
	unsigned int have_format:1;
	unsigned int have_profile:1;
	struct spa_pod *tag;

	uint32_t remap[MAX_PORTS];

	struct convert conv;
	unsigned int need_remap:1;
	unsigned int is_passthrough:1;
	unsigned int control:1;
};

struct stage_context {
#define CTX_DATA_SRC		0
#define CTX_DATA_DST		1
#define CTX_DATA_REMAP_DST	2
#define CTX_DATA_REMAP_SRC	3
#define CTX_DATA_TMP_0		4
#define CTX_DATA_TMP_1		5
#define CTX_DATA_MAX		6
	void **datas[CTX_DATA_MAX];
	uint32_t in_samples;
	uint32_t n_samples;
	uint32_t n_out;
	uint32_t src_idx;
	uint32_t dst_idx;
	uint32_t final_idx;
	struct port *ctrlport;
};

struct stage {
	struct impl *impl;
	bool passthrough;
	uint32_t in_idx;
	uint32_t out_idx;
	void *data;
	void (*run) (struct stage *stage, struct stage_context *c);
};

struct filter_graph {
	struct impl *impl;
	int order;
	struct spa_handle *handle;
	struct spa_filter_graph *graph;
	struct spa_hook listener;
	uint32_t n_inputs;
	uint32_t inputs_position[SPA_AUDIO_MAX_CHANNELS];
	uint32_t n_outputs;
	uint32_t outputs_position[SPA_AUDIO_MAX_CHANNELS];
	bool active;
};

struct impl {
	struct spa_handle handle;
	struct spa_node node;

	struct spa_log *log;
	struct spa_cpu *cpu;
	struct spa_loop *data_loop;
	struct spa_plugin_loader *loader;

	uint32_t n_graph;
	uint32_t graph_index[MAX_GRAPH];

	struct filter_graph filter_graph[MAX_GRAPH];
	int in_filter_props;
	int filter_props_count;

	struct stage stages[MAX_STAGES];
	uint32_t n_stages;

	uint32_t cpu_flags;
	uint32_t max_align;
	uint32_t quantum_limit;
	enum spa_direction direction;

	struct spa_ratelimit rate_limit;

	struct props props;

	struct spa_io_position *io_position;
	struct spa_io_rate_match *io_rate_match;

	uint64_t info_all;
	struct spa_node_info info;
#define IDX_EnumPortConfig	0
#define IDX_PortConfig		1
#define IDX_PropInfo		2
#define IDX_Props		3
#define N_NODE_PARAMS		4
	struct spa_param_info params[N_NODE_PARAMS];

	struct spa_hook_list hooks;

	unsigned int monitor:1;
	unsigned int monitor_channel_volumes:1;

	struct dir dir[2];
	struct channelmix mix;
	struct resample resample;
	struct volume volume;
	double rate_scale;
	struct spa_pod_sequence *vol_ramp_sequence;
	uint32_t vol_ramp_offset;

	uint32_t in_offset;
	uint32_t out_offset;
	unsigned int started:1;
	unsigned int setup:1;
	unsigned int resample_peaks:1;
	unsigned int is_passthrough:1;
	unsigned int ramp_volume:1;
	unsigned int drained:1;
	unsigned int rate_adjust:1;
	unsigned int port_ignore_latency:1;
	unsigned int monitor_passthrough:1;
	unsigned int resample_passthrough:1;

	bool recalc;

	char group_name[128];

	uint32_t scratch_size;
	uint32_t scratch_ports;
	float *empty;
	float *scratch;
	float *tmp[2];
	float *tmp_datas[2][MAX_PORTS];

	struct wav_file *wav_file;
};

#define CHECK_PORT(this,d,p)		((p) < this->dir[d].n_ports)
#define GET_PORT(this,d,p)		(this->dir[d].ports[p])
#define GET_IN_PORT(this,p)		GET_PORT(this,SPA_DIRECTION_INPUT,p)
#define GET_OUT_PORT(this,p)		GET_PORT(this,SPA_DIRECTION_OUTPUT,p)

#define PORT_IS_DSP(this,d,p)		(GET_PORT(this,d,p)->is_dsp)
#define PORT_IS_CONTROL(this,d,p)	(GET_PORT(this,d,p)->is_control)

static void set_volume(struct impl *this);

static void emit_node_info(struct impl *this, bool full)
{
	uint64_t old = full ? this->info.change_mask : 0;

	if (full)
		this->info.change_mask = this->info_all;
	if (this->info.change_mask) {
		if (this->info.change_mask & SPA_NODE_CHANGE_MASK_PARAMS) {
			SPA_FOR_EACH_ELEMENT_VAR(this->params, p) {
				if (p->user > 0) {
					p->flags ^= SPA_PARAM_INFO_SERIAL;
					p->user = 0;
				}
			}
		}
		spa_node_emit_info(&this->hooks, &this->info);
		this->info.change_mask = old;
	}
}

static void emit_port_info(struct impl *this, struct port *port, bool full)
{
	uint64_t old = full ? port->info.change_mask : 0;

	if (full)
		port->info.change_mask = port->info_all;
	if (port->info.change_mask) {
		struct spa_dict_item items[5];
		uint32_t n_items = 0;

		if (PORT_IS_DSP(this, port->direction, port->id)) {
			items[n_items++] = SPA_DICT_ITEM_INIT(SPA_KEY_FORMAT_DSP, "32 bit float mono audio");
			items[n_items++] = SPA_DICT_ITEM_INIT(SPA_KEY_AUDIO_CHANNEL, port->position);
			if (port->is_monitor)
				items[n_items++] = SPA_DICT_ITEM_INIT(SPA_KEY_PORT_MONITOR, "true");
			if (this->port_ignore_latency)
				items[n_items++] = SPA_DICT_ITEM_INIT(SPA_KEY_PORT_IGNORE_LATENCY, "true");
		} else if (PORT_IS_CONTROL(this, port->direction, port->id)) {
			items[n_items++] = SPA_DICT_ITEM_INIT(SPA_KEY_PORT_NAME, "control");
			items[n_items++] = SPA_DICT_ITEM_INIT(SPA_KEY_FORMAT_DSP, "32 bit raw UMP");
		}
		if (this->group_name[0] != '\0')
			items[n_items++] = SPA_DICT_ITEM_INIT(SPA_KEY_PORT_GROUP, this->group_name);
		port->info.props = &SPA_DICT_INIT(items, n_items);

		if (port->info.change_mask & SPA_PORT_CHANGE_MASK_PARAMS) {
			SPA_FOR_EACH_ELEMENT_VAR(port->params, p) {
				if (p->user > 0) {
					p->flags ^= SPA_PARAM_INFO_SERIAL;
					p->user = 0;
				}
			}
		}
		spa_node_emit_port_info(&this->hooks, port->direction, port->id, &port->info);
		port->info.change_mask = old;
	}
}

static int init_port(struct impl *this, enum spa_direction direction, uint32_t port_id,
		uint32_t position, bool is_dsp, bool is_monitor, bool is_control)
{
	struct port *port = GET_PORT(this, direction, port_id);
	const char *name;

	spa_assert(port_id < MAX_PORTS);

	if (port == NULL) {
		port = calloc(1, sizeof(struct port));
		if (port == NULL)
			return -errno;
		this->dir[direction].ports[port_id] = port;
	}
	port->direction = direction;
	port->id = port_id;
	port->latency[SPA_DIRECTION_INPUT] = SPA_LATENCY_INFO(SPA_DIRECTION_INPUT);
	port->latency[SPA_DIRECTION_OUTPUT] = SPA_LATENCY_INFO(SPA_DIRECTION_OUTPUT);

	name = spa_debug_type_find_short_name(spa_type_audio_channel, position);
	snprintf(port->position, sizeof(port->position), "%s", name ? name : "UNK");

	port->info_all = SPA_PORT_CHANGE_MASK_FLAGS |
			SPA_PORT_CHANGE_MASK_PROPS |
			SPA_PORT_CHANGE_MASK_PARAMS;
	port->info = SPA_PORT_INFO_INIT();
	port->info.flags = SPA_PORT_FLAG_NO_REF |
		SPA_PORT_FLAG_DYNAMIC_DATA;
	port->params[IDX_EnumFormat] = SPA_PARAM_INFO(SPA_PARAM_EnumFormat, SPA_PARAM_INFO_READ);
	port->params[IDX_Meta] = SPA_PARAM_INFO(SPA_PARAM_Meta, SPA_PARAM_INFO_READ);
	port->params[IDX_IO] = SPA_PARAM_INFO(SPA_PARAM_IO, SPA_PARAM_INFO_READ);
	port->params[IDX_Format] = SPA_PARAM_INFO(SPA_PARAM_Format, SPA_PARAM_INFO_WRITE);
	port->params[IDX_Buffers] = SPA_PARAM_INFO(SPA_PARAM_Buffers, 0);
	port->params[IDX_Latency] = SPA_PARAM_INFO(SPA_PARAM_Latency, SPA_PARAM_INFO_READWRITE);
	port->params[IDX_Tag] = SPA_PARAM_INFO(SPA_PARAM_Tag, SPA_PARAM_INFO_READWRITE);
	port->info.params = port->params;
	port->info.n_params = N_PORT_PARAMS;

	port->n_buffers = 0;
	port->have_format = false;
	port->is_monitor = is_monitor;
	port->is_dsp = is_dsp;
	if (port->is_dsp) {
		port->format.media_type = SPA_MEDIA_TYPE_audio;
		port->format.media_subtype = SPA_MEDIA_SUBTYPE_dsp;
		port->format.info.dsp.format = SPA_AUDIO_FORMAT_DSP_F32;
		port->blocks = 1;
		port->stride = 4;
	}
	port->is_control = is_control;
	if (port->is_control) {
		port->format.media_type = SPA_MEDIA_TYPE_application;
		port->format.media_subtype = SPA_MEDIA_SUBTYPE_control;
		port->blocks = 1;
		port->stride = 1;
	}
	port->valid = true;
	spa_list_init(&port->queue);

	spa_log_debug(this->log, "%p: add port %d:%d position:%s %d %d %d",
			this, direction, port_id, port->position, is_dsp,
			is_monitor, is_control);
	emit_port_info(this, port, true);

	return 0;
}

static int deinit_port(struct impl *this, enum spa_direction direction, uint32_t port_id)
{
	struct port *port = GET_PORT(this, direction, port_id);
	if (port == NULL || !port->valid)
		return -ENOENT;
	port->valid = false;
	spa_node_emit_port_info(&this->hooks, direction, port_id, NULL);
	return 0;
}

static int impl_node_enum_params(void *object, int seq,
				 uint32_t id, uint32_t start, uint32_t num,
				 const struct spa_pod *filter)
{
	struct impl *this = object;
	struct spa_pod *param;
	struct spa_pod_builder b = { 0 };
	uint8_t buffer[4096];
	struct spa_result_node_params result;
	uint32_t count = 0;
	int res;

	spa_return_val_if_fail(this != NULL, -EINVAL);
	spa_return_val_if_fail(num != 0, -EINVAL);

	result.id = id;
	result.next = start;
      next:
	result.index = result.next++;

	spa_pod_builder_init(&b, buffer, sizeof(buffer));

	switch (id) {
	case SPA_PARAM_EnumPortConfig:
	{
		struct dir *dir;
		switch (result.index) {
		case 0:
			dir = &this->dir[SPA_DIRECTION_INPUT];;
			break;
		case 1:
			dir = &this->dir[SPA_DIRECTION_OUTPUT];;
			break;
		default:
			return 0;
		}
		param = spa_pod_builder_add_object(&b,
			SPA_TYPE_OBJECT_ParamPortConfig, id,
			SPA_PARAM_PORT_CONFIG_direction, SPA_POD_Id(dir->direction),
			SPA_PARAM_PORT_CONFIG_mode,      SPA_POD_CHOICE_ENUM_Id(4,
				SPA_PARAM_PORT_CONFIG_MODE_none,
				SPA_PARAM_PORT_CONFIG_MODE_none,
				SPA_PARAM_PORT_CONFIG_MODE_dsp,
				SPA_PARAM_PORT_CONFIG_MODE_convert),
			SPA_PARAM_PORT_CONFIG_monitor,   SPA_POD_CHOICE_Bool(false),
			SPA_PARAM_PORT_CONFIG_control,   SPA_POD_CHOICE_Bool(false));
		break;
	}
	case SPA_PARAM_PortConfig:
	{
		struct dir *dir;
		struct spa_pod_frame f[1];

		switch (result.index) {
		case 0:
			dir = &this->dir[SPA_DIRECTION_INPUT];;
			break;
		case 1:
			dir = &this->dir[SPA_DIRECTION_OUTPUT];;
			break;
		default:
			return 0;
		}
		spa_pod_builder_push_object(&b, &f[0], SPA_TYPE_OBJECT_ParamPortConfig, id);
		spa_pod_builder_add(&b,
			SPA_PARAM_PORT_CONFIG_direction, SPA_POD_Id(dir->direction),
			SPA_PARAM_PORT_CONFIG_mode,      SPA_POD_Id(dir->mode),
			SPA_PARAM_PORT_CONFIG_monitor,   SPA_POD_Bool(this->monitor),
			SPA_PARAM_PORT_CONFIG_control,   SPA_POD_Bool(dir->control),
			0);

		if (dir->have_format) {
			spa_pod_builder_prop(&b, SPA_PARAM_PORT_CONFIG_format, 0);
			spa_format_audio_raw_build(&b, SPA_PARAM_PORT_CONFIG_format,
					&dir->format.info.raw);
		}
		param = spa_pod_builder_pop(&b, &f[0]);
		break;
	}
	case SPA_PARAM_PropInfo:
	{
		struct props *p = &this->props;
		struct spa_pod_frame f[2];

		switch (result.index) {
		case 0:
			param = spa_pod_builder_add_object(&b,
				SPA_TYPE_OBJECT_PropInfo, id,
				SPA_PROP_INFO_id,   SPA_POD_Id(SPA_PROP_volume),
				SPA_PROP_INFO_description, SPA_POD_String("Volume"),
				SPA_PROP_INFO_type, SPA_POD_CHOICE_RANGE_Float(p->volume,
					DEFAULT_MIN_VOLUME, DEFAULT_MAX_VOLUME));
			break;
		case 1:
			param = spa_pod_builder_add_object(&b,
				SPA_TYPE_OBJECT_PropInfo, id,
				SPA_PROP_INFO_id,   SPA_POD_Id(SPA_PROP_mute),
				SPA_PROP_INFO_description, SPA_POD_String("Mute"),
				SPA_PROP_INFO_type, SPA_POD_CHOICE_Bool(p->channel.mute));
			break;
		case 2:
			param = spa_pod_builder_add_object(&b,
				SPA_TYPE_OBJECT_PropInfo, id,
				SPA_PROP_INFO_id,   SPA_POD_Id(SPA_PROP_channelVolumes),
				SPA_PROP_INFO_description, SPA_POD_String("Channel Volumes"),
				SPA_PROP_INFO_type, SPA_POD_CHOICE_RANGE_Float(p->volume,
					DEFAULT_MIN_VOLUME, DEFAULT_MAX_VOLUME),
				SPA_PROP_INFO_container, SPA_POD_Id(SPA_TYPE_Array));
			break;
		case 3:
			param = spa_pod_builder_add_object(&b,
				SPA_TYPE_OBJECT_PropInfo, id,
				SPA_PROP_INFO_id,   SPA_POD_Id(SPA_PROP_channelMap),
				SPA_PROP_INFO_description, SPA_POD_String("Channel Map"),
				SPA_PROP_INFO_type, SPA_POD_Id(SPA_AUDIO_CHANNEL_UNKNOWN),
				SPA_PROP_INFO_container, SPA_POD_Id(SPA_TYPE_Array));
			break;
		case 4:
			param = spa_pod_builder_add_object(&b,
				SPA_TYPE_OBJECT_PropInfo, id,
				SPA_PROP_INFO_id,   SPA_POD_Id(SPA_PROP_monitorMute),
				SPA_PROP_INFO_description, SPA_POD_String("Monitor Mute"),
				SPA_PROP_INFO_type, SPA_POD_CHOICE_Bool(p->monitor.mute));
			break;
		case 5:
			param = spa_pod_builder_add_object(&b,
				SPA_TYPE_OBJECT_PropInfo, id,
				SPA_PROP_INFO_id,   SPA_POD_Id(SPA_PROP_monitorVolumes),
				SPA_PROP_INFO_description, SPA_POD_String("Monitor Volumes"),
				SPA_PROP_INFO_type, SPA_POD_CHOICE_RANGE_Float(p->volume,
					DEFAULT_MIN_VOLUME, DEFAULT_MAX_VOLUME),
				SPA_PROP_INFO_container, SPA_POD_Id(SPA_TYPE_Array));
			break;
		case 6:
			param = spa_pod_builder_add_object(&b,
				SPA_TYPE_OBJECT_PropInfo, id,
				SPA_PROP_INFO_id,   SPA_POD_Id(SPA_PROP_softMute),
				SPA_PROP_INFO_description, SPA_POD_String("Soft Mute"),
				SPA_PROP_INFO_type, SPA_POD_CHOICE_Bool(p->soft.mute));
			break;
		case 7:
			param = spa_pod_builder_add_object(&b,
				SPA_TYPE_OBJECT_PropInfo, id,
				SPA_PROP_INFO_id,   SPA_POD_Id(SPA_PROP_softVolumes),
				SPA_PROP_INFO_description, SPA_POD_String("Soft Volumes"),
				SPA_PROP_INFO_type, SPA_POD_CHOICE_RANGE_Float(p->volume,
					DEFAULT_MIN_VOLUME, DEFAULT_MAX_VOLUME),
				SPA_PROP_INFO_container, SPA_POD_Id(SPA_TYPE_Array));
			break;
		case 8:
			param = spa_pod_builder_add_object(&b,
				SPA_TYPE_OBJECT_PropInfo, id,
				SPA_PROP_INFO_name, SPA_POD_String("monitor.channel-volumes"),
				SPA_PROP_INFO_description, SPA_POD_String("Monitor channel volume"),
				SPA_PROP_INFO_type, SPA_POD_CHOICE_Bool(
					this->monitor_channel_volumes),
				SPA_PROP_INFO_params, SPA_POD_Bool(true));
			break;
		case 9:
			param = spa_pod_builder_add_object(&b,
				SPA_TYPE_OBJECT_PropInfo, id,
				SPA_PROP_INFO_name, SPA_POD_String("channelmix.disable"),
				SPA_PROP_INFO_description, SPA_POD_String("Disable Channel mixing"),
				SPA_PROP_INFO_type, SPA_POD_CHOICE_Bool(p->mix_disabled),
				SPA_PROP_INFO_params, SPA_POD_Bool(true));
			break;
		case 10:
			param = spa_pod_builder_add_object(&b,
				SPA_TYPE_OBJECT_PropInfo, id,
				SPA_PROP_INFO_name, SPA_POD_String("channelmix.min-volume"),
				SPA_PROP_INFO_description, SPA_POD_String("Minimum volume level"),
				SPA_PROP_INFO_type, SPA_POD_CHOICE_RANGE_Float(p->min_volume,
					DEFAULT_MIN_VOLUME, DEFAULT_MAX_VOLUME),
				SPA_PROP_INFO_params, SPA_POD_Bool(true));
			break;
		case 11:
			param = spa_pod_builder_add_object(&b,
				SPA_TYPE_OBJECT_PropInfo, id,
				SPA_PROP_INFO_name, SPA_POD_String("channelmix.max-volume"),
				SPA_PROP_INFO_description, SPA_POD_String("Maximum volume level"),
				SPA_PROP_INFO_type, SPA_POD_CHOICE_RANGE_Float(p->max_volume,
					DEFAULT_MIN_VOLUME, DEFAULT_MAX_VOLUME),
				SPA_PROP_INFO_params, SPA_POD_Bool(true));
			break;
		case 12:
			param = spa_pod_builder_add_object(&b,
				SPA_TYPE_OBJECT_PropInfo, id,
				SPA_PROP_INFO_name, SPA_POD_String("channelmix.normalize"),
				SPA_PROP_INFO_description, SPA_POD_String("Normalize Volumes"),
				SPA_PROP_INFO_type, SPA_POD_CHOICE_Bool(
					SPA_FLAG_IS_SET(this->mix.options, CHANNELMIX_OPTION_NORMALIZE)),
				SPA_PROP_INFO_params, SPA_POD_Bool(true));
			break;
		case 13:
			param = spa_pod_builder_add_object(&b,
				SPA_TYPE_OBJECT_PropInfo, id,
				SPA_PROP_INFO_name, SPA_POD_String("channelmix.mix-lfe"),
				SPA_PROP_INFO_description, SPA_POD_String("Mix LFE into channels"),
				SPA_PROP_INFO_type, SPA_POD_CHOICE_Bool(
					SPA_FLAG_IS_SET(this->mix.options, CHANNELMIX_OPTION_MIX_LFE)),
				SPA_PROP_INFO_params, SPA_POD_Bool(true));
			break;
		case 14:
			param = spa_pod_builder_add_object(&b,
				SPA_TYPE_OBJECT_PropInfo, id,
				SPA_PROP_INFO_name, SPA_POD_String("channelmix.upmix"),
				SPA_PROP_INFO_description, SPA_POD_String("Enable upmixing"),
				SPA_PROP_INFO_type, SPA_POD_CHOICE_Bool(
					SPA_FLAG_IS_SET(this->mix.options, CHANNELMIX_OPTION_UPMIX)),
				SPA_PROP_INFO_params, SPA_POD_Bool(true));
			break;
		case 15:
			param = spa_pod_builder_add_object(&b,
				SPA_TYPE_OBJECT_PropInfo, id,
				SPA_PROP_INFO_name, SPA_POD_String("channelmix.lfe-cutoff"),
				SPA_PROP_INFO_description, SPA_POD_String("LFE cutoff frequency"),
				SPA_PROP_INFO_type, SPA_POD_CHOICE_RANGE_Float(
					this->mix.lfe_cutoff, 0.0, 1000.0),
				SPA_PROP_INFO_params, SPA_POD_Bool(true));
			break;
		case 16:
			param = spa_pod_builder_add_object(&b,
				SPA_TYPE_OBJECT_PropInfo, id,
				SPA_PROP_INFO_name, SPA_POD_String("channelmix.fc-cutoff"),
				SPA_PROP_INFO_description, SPA_POD_String("FC cutoff frequency (Hz)"),
				SPA_PROP_INFO_type, SPA_POD_CHOICE_RANGE_Float(
					this->mix.fc_cutoff, 0.0, 48000.0),
				SPA_PROP_INFO_params, SPA_POD_Bool(true));
			break;
		case 17:
			param = spa_pod_builder_add_object(&b,
				SPA_TYPE_OBJECT_PropInfo, id,
				SPA_PROP_INFO_name, SPA_POD_String("channelmix.rear-delay"),
				SPA_PROP_INFO_description, SPA_POD_String("Rear channels delay (ms)"),
				SPA_PROP_INFO_type, SPA_POD_CHOICE_RANGE_Float(
					this->mix.rear_delay, 0.0, 1000.0),
				SPA_PROP_INFO_params, SPA_POD_Bool(true));
			break;
		case 18:
			param = spa_pod_builder_add_object(&b,
				SPA_TYPE_OBJECT_PropInfo, id,
				SPA_PROP_INFO_name, SPA_POD_String("channelmix.stereo-widen"),
				SPA_PROP_INFO_description, SPA_POD_String("Stereo widen"),
				SPA_PROP_INFO_type, SPA_POD_CHOICE_RANGE_Float(
					this->mix.widen, 0.0, 1.0),
				SPA_PROP_INFO_params, SPA_POD_Bool(true));
			break;
		case 19:
			param = spa_pod_builder_add_object(&b,
				SPA_TYPE_OBJECT_PropInfo, id,
				SPA_PROP_INFO_name, SPA_POD_String("channelmix.hilbert-taps"),
				SPA_PROP_INFO_description, SPA_POD_String("Taps for phase shift of rear"),
				SPA_PROP_INFO_type, SPA_POD_CHOICE_RANGE_Int(
					this->mix.hilbert_taps, 0, MAX_TAPS),
				SPA_PROP_INFO_params, SPA_POD_Bool(true));
			break;
		case 20:
			spa_pod_builder_push_object(&b, &f[0], SPA_TYPE_OBJECT_PropInfo, id);
			spa_pod_builder_add(&b,
				SPA_PROP_INFO_name, SPA_POD_String("channelmix.upmix-method"),
				SPA_PROP_INFO_description, SPA_POD_String("Upmix method to use"),
				SPA_PROP_INFO_type, SPA_POD_String(
					channelmix_upmix_info[this->mix.upmix].label),
				SPA_PROP_INFO_params, SPA_POD_Bool(true),
				0);

			spa_pod_builder_prop(&b, SPA_PROP_INFO_labels, 0);
			spa_pod_builder_push_struct(&b, &f[1]);
			SPA_FOR_EACH_ELEMENT_VAR(channelmix_upmix_info, i) {
				spa_pod_builder_string(&b, i->label);
				spa_pod_builder_string(&b, i->description);
			}
			spa_pod_builder_pop(&b, &f[1]);
			param = spa_pod_builder_pop(&b, &f[0]);
			break;
		case 21:
			param = spa_pod_builder_add_object(&b,
				SPA_TYPE_OBJECT_PropInfo, id,
				SPA_PROP_INFO_id, SPA_POD_Id(SPA_PROP_rate),
				SPA_PROP_INFO_description, SPA_POD_String("Rate scaler"),
				SPA_PROP_INFO_type, SPA_POD_CHOICE_RANGE_Double(p->rate, 0.0, 10.0));
			break;
		case 22:
			param = spa_pod_builder_add_object(&b,
				SPA_TYPE_OBJECT_PropInfo, id,
				SPA_PROP_INFO_id, SPA_POD_Id(SPA_PROP_quality),
				SPA_PROP_INFO_name, SPA_POD_String("resample.quality"),
				SPA_PROP_INFO_description, SPA_POD_String("Resample Quality"),
				SPA_PROP_INFO_type, SPA_POD_CHOICE_RANGE_Int(p->resample_quality, 0, 14),
				SPA_PROP_INFO_params, SPA_POD_Bool(true));
			break;
		case 23:
			param = spa_pod_builder_add_object(&b,
				SPA_TYPE_OBJECT_PropInfo, id,
				SPA_PROP_INFO_name, SPA_POD_String("resample.disable"),
				SPA_PROP_INFO_description, SPA_POD_String("Disable Resampling"),
				SPA_PROP_INFO_type, SPA_POD_CHOICE_Bool(p->resample_disabled),
				SPA_PROP_INFO_params, SPA_POD_Bool(true));
			break;
		case 24:
			param = spa_pod_builder_add_object(&b,
				SPA_TYPE_OBJECT_PropInfo, id,
				SPA_PROP_INFO_name, SPA_POD_String("dither.noise"),
				SPA_PROP_INFO_description, SPA_POD_String("Add noise bits"),
				SPA_PROP_INFO_type, SPA_POD_CHOICE_RANGE_Int(this->dir[1].conv.noise_bits, 0, 16),
				SPA_PROP_INFO_params, SPA_POD_Bool(true));
			break;
		case 25:
			spa_pod_builder_push_object(&b, &f[0], SPA_TYPE_OBJECT_PropInfo, id);
			spa_pod_builder_add(&b,
				SPA_PROP_INFO_name, SPA_POD_String("dither.method"),
				SPA_PROP_INFO_description, SPA_POD_String("The dithering method"),
				SPA_PROP_INFO_type, SPA_POD_String(
					dither_method_info[this->dir[1].conv.method].label),
				SPA_PROP_INFO_params, SPA_POD_Bool(true),
				0);
			spa_pod_builder_prop(&b, SPA_PROP_INFO_labels, 0);
			spa_pod_builder_push_struct(&b, &f[1]);
			SPA_FOR_EACH_ELEMENT_VAR(dither_method_info, i) {
				spa_pod_builder_string(&b, i->label);
				spa_pod_builder_string(&b, i->description);
			}
			spa_pod_builder_pop(&b, &f[1]);
			param = spa_pod_builder_pop(&b, &f[0]);
			break;
		case 26:
			param = spa_pod_builder_add_object(&b,
				SPA_TYPE_OBJECT_PropInfo, id,
				SPA_PROP_INFO_name, SPA_POD_String("debug.wav-path"),
				SPA_PROP_INFO_description, SPA_POD_String("Path to WAV file"),
				SPA_PROP_INFO_type, SPA_POD_String(p->wav_path),
				SPA_PROP_INFO_params, SPA_POD_Bool(true));
			break;
		case 27:
			param = spa_pod_builder_add_object(&b,
				SPA_TYPE_OBJECT_PropInfo, id,
				SPA_PROP_INFO_name, SPA_POD_String("channelmix.lock-volumes"),
				SPA_PROP_INFO_description, SPA_POD_String("Disable volume updates"),
				SPA_PROP_INFO_type, SPA_POD_CHOICE_Bool(p->lock_volumes),
				SPA_PROP_INFO_params, SPA_POD_Bool(true));
			break;
		case 28:
			param = spa_pod_builder_add_object(&b,
				SPA_TYPE_OBJECT_PropInfo, id,
				SPA_PROP_INFO_name, SPA_POD_String("audioconvert.filter-graph.disable"),
				SPA_PROP_INFO_description, SPA_POD_String("Disable Filter graph updates"),
				SPA_PROP_INFO_type, SPA_POD_CHOICE_Bool(p->filter_graph_disabled),
				SPA_PROP_INFO_params, SPA_POD_Bool(true));
			break;
		case 29:
			param = spa_pod_builder_add_object(&b,
				SPA_TYPE_OBJECT_PropInfo, id,
				SPA_PROP_INFO_name, SPA_POD_String("audioconvert.filter-graph"),
				SPA_PROP_INFO_description, SPA_POD_String("A filter graph to load"),
				SPA_PROP_INFO_type, SPA_POD_String(""),
				SPA_PROP_INFO_params, SPA_POD_Bool(true));
			break;
		default:
			if (this->filter_graph[0].graph) {
				res = spa_filter_graph_enum_prop_info(this->filter_graph[0].graph,
						result.index - 30, &b, &param);
				if (res <= 0)
					return res;
			} else
				return 0;
		}
		break;
	}

	case SPA_PARAM_Props:
	{
		struct props *p = &this->props;
		struct spa_pod_frame f[2];

		switch (result.index) {
		case 0:
			spa_pod_builder_push_object(&b, &f[0],
                                SPA_TYPE_OBJECT_Props, id);
			spa_pod_builder_add(&b,
				SPA_PROP_volume,		SPA_POD_Float(p->volume),
				SPA_PROP_mute,			SPA_POD_Bool(p->channel.mute),
				SPA_PROP_channelVolumes,	SPA_POD_Array(sizeof(float),
									SPA_TYPE_Float,
									p->channel.n_volumes,
									p->channel.volumes),
				SPA_PROP_channelMap,		SPA_POD_Array(sizeof(uint32_t),
									SPA_TYPE_Id,
									p->n_channels,
									p->channel_map),
				SPA_PROP_softMute,		SPA_POD_Bool(p->soft.mute),
				SPA_PROP_softVolumes,		SPA_POD_Array(sizeof(float),
									SPA_TYPE_Float,
									p->soft.n_volumes,
									p->soft.volumes),
				SPA_PROP_monitorMute,		SPA_POD_Bool(p->monitor.mute),
				SPA_PROP_monitorVolumes,	SPA_POD_Array(sizeof(float),
									SPA_TYPE_Float,
									p->monitor.n_volumes,
									p->monitor.volumes),
				0);
			spa_pod_builder_prop(&b, SPA_PROP_params, 0);
			spa_pod_builder_push_struct(&b, &f[1]);
			spa_pod_builder_string(&b, "monitor.channel-volumes");
			spa_pod_builder_bool(&b, this->monitor_channel_volumes);
			spa_pod_builder_string(&b, "channelmix.disable");
			spa_pod_builder_bool(&b, this->props.mix_disabled);
			spa_pod_builder_string(&b, "channelmix.min-volume");
			spa_pod_builder_float(&b, this->props.min_volume);
			spa_pod_builder_string(&b, "channelmix.max-volume");
			spa_pod_builder_float(&b, this->props.max_volume);
			spa_pod_builder_string(&b, "channelmix.normalize");
			spa_pod_builder_bool(&b, SPA_FLAG_IS_SET(this->mix.options,
						CHANNELMIX_OPTION_NORMALIZE));
			spa_pod_builder_string(&b, "channelmix.mix-lfe");
			spa_pod_builder_bool(&b, SPA_FLAG_IS_SET(this->mix.options,
						CHANNELMIX_OPTION_MIX_LFE));
			spa_pod_builder_string(&b, "channelmix.upmix");
			spa_pod_builder_bool(&b, SPA_FLAG_IS_SET(this->mix.options,
						CHANNELMIX_OPTION_UPMIX));
			spa_pod_builder_string(&b, "channelmix.lfe-cutoff");
			spa_pod_builder_float(&b, this->mix.lfe_cutoff);
			spa_pod_builder_string(&b, "channelmix.fc-cutoff");
			spa_pod_builder_float(&b, this->mix.fc_cutoff);
			spa_pod_builder_string(&b, "channelmix.rear-delay");
			spa_pod_builder_float(&b, this->mix.rear_delay);
			spa_pod_builder_string(&b, "channelmix.stereo-widen");
			spa_pod_builder_float(&b, this->mix.widen);
			spa_pod_builder_string(&b, "channelmix.hilbert-taps");
			spa_pod_builder_int(&b, this->mix.hilbert_taps);
			spa_pod_builder_string(&b, "channelmix.upmix-method");
			spa_pod_builder_string(&b, channelmix_upmix_info[this->mix.upmix].label);
			spa_pod_builder_string(&b, "resample.quality");
			spa_pod_builder_int(&b, p->resample_quality);
			spa_pod_builder_string(&b, "resample.disable");
			spa_pod_builder_bool(&b, p->resample_disabled);
			spa_pod_builder_string(&b, "dither.noise");
			spa_pod_builder_int(&b, this->dir[1].conv.noise_bits);
			spa_pod_builder_string(&b, "dither.method");
			spa_pod_builder_string(&b, dither_method_info[this->dir[1].conv.method].label);
			spa_pod_builder_string(&b, "debug.wav-path");
			spa_pod_builder_string(&b, p->wav_path);
			spa_pod_builder_string(&b, "channelmix.lock-volumes");
			spa_pod_builder_bool(&b, p->lock_volumes);
			spa_pod_builder_string(&b, "audioconvert.filter-graph.disable");
			spa_pod_builder_bool(&b, p->filter_graph_disabled);
			spa_pod_builder_string(&b, "audioconvert.filter-graph");
			spa_pod_builder_string(&b, "");
			spa_pod_builder_pop(&b, &f[1]);
			param = spa_pod_builder_pop(&b, &f[0]);
			break;
		default:
			if (result.index > MAX_GRAPH)
				return 0;

			if (this->filter_graph[result.index-1].graph == NULL)
				goto next;

			res = spa_filter_graph_get_props(this->filter_graph[result.index-1].graph,
						&b, &param);
			if (res < 0)
				return res;
			if (res == 0)
				goto next;
			break;
		}
		break;
	}
	default:
		return 0;
	}

	if (spa_pod_filter(&b, &result.param, param, filter) < 0)
		goto next;

	spa_node_emit_result(&this->hooks, seq, 0, SPA_RESULT_TYPE_NODE_PARAMS, &result);

	if (++count != num)
		goto next;

	return 0;
}

static int impl_node_set_io(void *object, uint32_t id, void *data, size_t size)
{
	struct impl *this = object;

	spa_return_val_if_fail(this != NULL, -EINVAL);

	spa_log_debug(this->log, "%p: io %d %p/%zd", this, id, data, size);

	switch (id) {
	case SPA_IO_Position:
		this->io_position = data;
		break;
	default:
		return -ENOENT;
	}
	return 0;
}

static void graph_info(void *object, const struct spa_filter_graph_info *info)
{
	struct filter_graph *g = object;
	struct spa_dict *props = info->props;
	uint32_t i;

	if (!g->active)
		return;

	g->n_inputs = info->n_inputs;
	g->n_outputs = info->n_outputs;
	for (i = 0; props && i < props->n_items; i++) {
		const char *k = props->items[i].key;
		const char *s = props->items[i].value;
		if (spa_streq(k, "n_inputs"))
			spa_atou32(s, &g->n_inputs, 0);
		else if (spa_streq(k, "n_outputs"))
			spa_atou32(s, &g->n_outputs, 0);
		else if (spa_streq(k, "inputs.audio.position"))
			spa_audio_parse_position(s, strlen(s),
					g->inputs_position, &g->n_inputs);
		else if (spa_streq(k, "outputs.audio.position"))
			spa_audio_parse_position(s, strlen(s),
					g->outputs_position, &g->n_outputs);
	}
}

static int apply_props(struct impl *impl, const struct spa_pod *props);

static void graph_apply_props(void *object, enum spa_direction direction, const struct spa_pod *props)
{
	struct filter_graph *g = object;
	struct impl *impl = g->impl;
	if (!g->active)
		return;
	if (apply_props(impl, props) > 0)
		emit_node_info(impl, false);
}

static void graph_props_changed(void *object, enum spa_direction direction)
{
	struct filter_graph *g = object;
	struct impl *impl = g->impl;
	if (!g->active)
		return;
	impl->info.change_mask |= SPA_NODE_CHANGE_MASK_PARAMS;
	impl->params[IDX_Props].user++;
}

struct spa_filter_graph_events graph_events = {
	SPA_VERSION_FILTER_GRAPH_EVENTS,
	.info = graph_info,
	.apply_props = graph_apply_props,
	.props_changed = graph_props_changed,
};

static int setup_filter_graph(struct impl *this, struct filter_graph *g,
		uint32_t channels, uint32_t *position)
{
	int res;
	char rate_str[64], in_ports[64];
	struct dir *dir;

	if (g == NULL || g->graph == NULL)
		return 0;

	dir = &this->dir[SPA_DIRECTION_REVERSE(this->direction)];
	snprintf(rate_str, sizeof(rate_str), "%d", dir->format.info.raw.rate);
	if (channels) {
		snprintf(in_ports, sizeof(in_ports), "%d", channels);
		g->n_inputs = channels;
		if (position) {
			memcpy(g->inputs_position, position, sizeof(uint32_t) * channels);
			memcpy(g->outputs_position, position, sizeof(uint32_t) * channels);
		}
	}

	spa_filter_graph_deactivate(g->graph);
	res = spa_filter_graph_activate(g->graph,
				     &SPA_DICT_ITEMS(
					     SPA_DICT_ITEM(SPA_KEY_AUDIO_RATE, rate_str),
					     SPA_DICT_ITEM("filter-graph.n_inputs", channels ? in_ports : NULL)));

	return res;
}

static int do_sync_filter_graph(struct spa_loop *loop, bool async, uint32_t seq,
		const void *data, size_t size, void *user_data)
{
	struct impl *impl = user_data;
	uint32_t i, j;
	impl->n_graph = 0;
	for (i = 0; i < MAX_GRAPH; i++) {
		struct filter_graph *g = &impl->filter_graph[i];
		if (g->graph == NULL || !g->active)
			continue;
		impl->graph_index[impl->n_graph++] = i;

		for (j = impl->n_graph-1; j > 0; j--) {
			if (impl->filter_graph[impl->graph_index[j]].order >=
			    impl->filter_graph[impl->graph_index[j-1]].order)
				break;
			SPA_SWAP(impl->graph_index[j], impl->graph_index[j-1]);
		}
	}
	impl->recalc = true;
	return 0;
}

static void clean_filter_handles(struct impl *impl, bool force)
{
	uint32_t i;
	for (i = 0; i < MAX_GRAPH; i++) {
		struct filter_graph *g = &impl->filter_graph[i];
		if (!g->active || force) {
			if (g->graph)
				spa_hook_remove(&g->listener);
			if (g->handle)
				spa_plugin_loader_unload(impl->loader, g->handle);
			spa_zero(*g);
		}
	}
}

static int load_filter_graph(struct impl *impl, const char *graph, int order)
{
	char qlimit[64];
	int res;
	void *iface;
	struct spa_handle *new_handle = NULL;
	uint32_t i, idx, n_graph;
	struct filter_graph *pending, *old_active = NULL;

	if (impl->props.filter_graph_disabled)
		return -EPERM;

	/* find graph spot */
	idx = SPA_ID_INVALID;
	n_graph = 0;
	for (i = 0; i < MAX_GRAPH; i++) {
		pending = &impl->filter_graph[i];
		/* find the first free spot for our new filter */
		if (!pending->active && idx == SPA_ID_INVALID)
			idx = i;
		/* deactivate an existing filter of the same order */
		if (pending->active) {
			if (pending->order == order)
				old_active = pending;
			else
				n_graph++;
		}
	}
	/* we can at most have MAX_GRAPH-1 active filters */
	if (n_graph >= MAX_GRAPH-1)
		return -ENOSPC;

	pending = &impl->filter_graph[idx];
	pending->impl = impl;
	pending->order = order;

	if (graph != NULL && graph[0] != '\0') {
		snprintf(qlimit, sizeof(qlimit), "%u", impl->quantum_limit);

		new_handle = spa_plugin_loader_load(impl->loader, "filter.graph",
				&SPA_DICT_ITEMS(
					SPA_DICT_ITEM(SPA_KEY_LIBRARY_NAME, "filter-graph/libspa-filter-graph"),
					SPA_DICT_ITEM("clock.quantum-limit", qlimit),
					SPA_DICT_ITEM("filter.graph", graph)));
		if (new_handle == NULL)
			goto error;

		res = spa_handle_get_interface(new_handle, SPA_TYPE_INTERFACE_FilterGraph, &iface);
		if (res < 0 || iface == NULL)
			goto error;

		/* prepare new filter and swap it */
		pending->graph = iface;
		res = setup_filter_graph(impl, pending, 0, NULL);
		if (res < 0) {
			pending->graph = NULL;
			goto error;
		}
		pending->active = true;
		spa_log_info(impl->log, "loading filter-graph order:%d in %d active:%d",
				order, idx, n_graph + 1);
	} else {
		pending->active = false;
		spa_log_info(impl->log, "removing filter-graph order:%d active:%d",
				order, n_graph);
	}
	if (old_active)
		old_active->active = false;

	/* we call this here on the pending_graph so that the n_input/n_output is updated
	 * before we switch */
	if (pending->active)
		spa_filter_graph_add_listener(pending->graph,
				&pending->listener, &graph_events, pending);

	spa_loop_invoke(impl->data_loop, do_sync_filter_graph, 0, NULL, 0, true, impl);

	if (pending->active)
		pending->handle = new_handle;

	if (impl->in_filter_props == 0)
		clean_filter_handles(impl, false);

	impl->info.change_mask |= SPA_NODE_CHANGE_MASK_PARAMS;
	impl->params[IDX_PropInfo].user++;
	impl->params[IDX_Props].user++;

	return 0;
error:
	if (new_handle != NULL)
		spa_plugin_loader_unload(impl->loader, new_handle);
	return -ENOTSUP;
}

static int audioconvert_set_param(struct impl *this, const char *k, const char *s)
{
	int res;

	if (spa_streq(k, "monitor.channel-volumes"))
		this->monitor_channel_volumes = spa_atob(s);
	else if (spa_streq(k, "channelmix.disable"))
		this->props.mix_disabled = spa_atob(s);
	else if (spa_streq(k, "channelmix.min-volume"))
		spa_atof(s, &this->props.min_volume);
	else if (spa_streq(k, "channelmix.max-volume"))
		spa_atof(s, &this->props.max_volume);
	else if (spa_streq(k, "channelmix.normalize"))
		SPA_FLAG_UPDATE(this->mix.options, CHANNELMIX_OPTION_NORMALIZE, spa_atob(s));
	else if (spa_streq(k, "channelmix.mix-lfe"))
		SPA_FLAG_UPDATE(this->mix.options, CHANNELMIX_OPTION_MIX_LFE, spa_atob(s));
	else if (spa_streq(k, "channelmix.upmix"))
		SPA_FLAG_UPDATE(this->mix.options, CHANNELMIX_OPTION_UPMIX, spa_atob(s));
	else if (spa_streq(k, "channelmix.lfe-cutoff"))
		spa_atof(s, &this->mix.lfe_cutoff);
	else if (spa_streq(k, "channelmix.fc-cutoff"))
		spa_atof(s, &this->mix.fc_cutoff);
	else if (spa_streq(k, "channelmix.rear-delay"))
		spa_atof(s, &this->mix.rear_delay);
	else if (spa_streq(k, "channelmix.stereo-widen"))
		spa_atof(s, &this->mix.widen);
	else if (spa_streq(k, "channelmix.hilbert-taps"))
		spa_atou32(s, &this->mix.hilbert_taps, 0);
	else if (spa_streq(k, "channelmix.upmix-method"))
		this->mix.upmix = channelmix_upmix_from_label(s);
	else if (spa_streq(k, "resample.quality"))
		this->props.resample_quality = atoi(s);
	else if (spa_streq(k, "resample.disable"))
		this->props.resample_disabled = spa_atob(s);
	else if (spa_streq(k, "dither.noise"))
		spa_atou32(s, &this->dir[1].conv.noise_bits, 0);
	else if (spa_streq(k, "dither.method"))
		this->dir[1].conv.method = dither_method_from_label(s);
	else if (spa_streq(k, "debug.wav-path")) {
		spa_scnprintf(this->props.wav_path,
				sizeof(this->props.wav_path), "%s", s ? s : "");
	}
	else if (spa_streq(k, "channelmix.lock-volumes"))
		this->props.lock_volumes = spa_atob(s);
	else if (spa_strstartswith(k, "audioconvert.filter-graph")) {
		int order = atoi(k+ strlen("audioconvert.filter-graph."));
		if ((res = load_filter_graph(this, s, order)) < 0) {
			spa_log_warn(this->log, "Can't load filter-graph %d: %s",
					order, spa_strerror(res));
		}
	}
	else
		return 0;
	return 1;
}

static int parse_prop_params(struct impl *this, struct spa_pod *params)
{
	struct spa_pod_parser prs;
	struct spa_pod_frame f;
	int changed = 0;

	spa_pod_parser_pod(&prs, params);
	if (spa_pod_parser_push_struct(&prs, &f) < 0)
		return 0;

	while (true) {
		const char *name;
		struct spa_pod *pod;
		char value[512];

		if (spa_pod_parser_get_string(&prs, &name) < 0)
			break;

		if (spa_pod_parser_get_pod(&prs, &pod) < 0)
			break;

		if (spa_pod_is_string(pod)) {
			spa_pod_copy_string(pod, sizeof(value), value);
		} else if (spa_pod_is_float(pod)) {
			spa_dtoa(value, sizeof(value),
					SPA_POD_VALUE(struct spa_pod_float, pod));
		} else if (spa_pod_is_double(pod)) {
			spa_dtoa(value, sizeof(value),
					SPA_POD_VALUE(struct spa_pod_double, pod));
		} else if (spa_pod_is_int(pod)) {
			snprintf(value, sizeof(value), "%d",
					SPA_POD_VALUE(struct spa_pod_int, pod));
		} else if (spa_pod_is_bool(pod)) {
			snprintf(value, sizeof(value), "%s",
					SPA_POD_VALUE(struct spa_pod_bool, pod) ?
					"true" : "false");
		} else if (spa_pod_is_none(pod)) {
			spa_zero(value);
		} else
			continue;

		spa_log_info(this->log, "key:'%s' val:'%s'", name, value);
		changed += audioconvert_set_param(this, name, value);
	}
	if (changed) {
		channelmix_init(&this->mix);
	}
	return changed;
}

static int get_ramp_samples(struct impl *this)
{
	struct volume_ramp_params *vrp = &this->props.vrp;
	int samples = -1;

	if (vrp->volume_ramp_samples)
		samples = vrp->volume_ramp_samples;
	else if (vrp->volume_ramp_time) {
		struct dir *d = &this->dir[SPA_DIRECTION_OUTPUT];
		unsigned int sample_rate = d->format.info.raw.rate;
		samples = (vrp->volume_ramp_time * sample_rate) / 1000;
		spa_log_info(this->log, "volume ramp samples calculated from time is %d", samples);
	}
	if (!samples)
		samples = -1;

	return samples;

}

static int get_ramp_step_samples(struct impl *this)
{
	struct volume_ramp_params *vrp = &this->props.vrp;
	int samples = -1;

	if (vrp->volume_ramp_step_samples)
		samples = vrp->volume_ramp_step_samples;
	else if (vrp->volume_ramp_step_time) {
		struct dir *d = &this->dir[SPA_DIRECTION_OUTPUT];
		int sample_rate = d->format.info.raw.rate;
		/* convert the step time which is in nano seconds to seconds */
		samples = (vrp->volume_ramp_step_time/1000) * (sample_rate/1000);
		spa_log_debug(this->log, "volume ramp step samples calculated from time is %d", samples);
	}
	if (!samples)
		samples = -1;

	return samples;

}

static double get_volume_at_scale(struct impl *this, double value)
{
	struct volume_ramp_params *vrp = &this->props.vrp;
	if (vrp->scale == SPA_AUDIO_VOLUME_RAMP_LINEAR || vrp->scale == SPA_AUDIO_VOLUME_RAMP_INVALID)
		return value;
	else if (vrp->scale == SPA_AUDIO_VOLUME_RAMP_CUBIC)
		return (value * value * value);

	return 0.0;
}

static struct spa_pod *generate_ramp_up_seq(struct impl *this)
{
	struct spa_pod_dynamic_builder b;
	struct spa_pod_frame f[1];
	struct props *p = &this->props;
	double volume_accum = p->prev_volume;
	int ramp_samples = get_ramp_samples(this);
	int ramp_step_samples = get_ramp_step_samples(this);
	double volume_step = ((p->volume - p->prev_volume) / (ramp_samples / ramp_step_samples));
	uint32_t volume_offs = 0;

	spa_pod_dynamic_builder_init(&b, NULL, 0, 4096);

	spa_pod_builder_push_sequence(&b.b, &f[0], 0);
	spa_log_info(this->log, "generating ramp up sequence from %f to %f with a"
		" step value %f at scale %d", p->prev_volume, p->volume, volume_step, p->vrp.scale);
	do {
		spa_log_trace(this->log, "volume accum %f", get_volume_at_scale(this, volume_accum));
		spa_pod_builder_control(&b.b, volume_offs, SPA_CONTROL_Properties);
		spa_pod_builder_add_object(&b.b,
				SPA_TYPE_OBJECT_Props, 0,
				SPA_PROP_volume,
				SPA_POD_Float(get_volume_at_scale(this, volume_accum)));
		volume_accum += volume_step;
		volume_offs += ramp_step_samples;
	} while (volume_accum < p->volume);
	return spa_pod_builder_pop(&b.b, &f[0]);
}

static struct spa_pod *generate_ramp_down_seq(struct impl *this)
{
	struct spa_pod_dynamic_builder b;
	struct spa_pod_frame f[1];
	int ramp_samples = get_ramp_samples(this);
	int ramp_step_samples = get_ramp_step_samples(this);
	struct props *p = &this->props;
	double volume_accum = p->prev_volume;
	double volume_step = ((p->prev_volume - p->volume) / (ramp_samples / ramp_step_samples));
	uint32_t volume_offs = 0;

	spa_pod_dynamic_builder_init(&b, NULL, 0, 4096);

	spa_pod_builder_push_sequence(&b.b, &f[0], 0);
	spa_log_info(this->log, "generating ramp down sequence from %f to %f with a"
		" step value %f at scale %d", p->prev_volume, p->volume, volume_step, p->vrp.scale);
	do {
		spa_log_trace(this->log, "volume accum %f", get_volume_at_scale(this, volume_accum));
		spa_pod_builder_control(&b.b, volume_offs, SPA_CONTROL_Properties);
		spa_pod_builder_add_object(&b.b,
				SPA_TYPE_OBJECT_Props, 0,
				SPA_PROP_volume,
				SPA_POD_Float(get_volume_at_scale(this, volume_accum)));

		volume_accum -= volume_step;
		volume_offs += ramp_step_samples;
	} while (volume_accum > p->volume);
	return spa_pod_builder_pop(&b.b, &f[0]);
}

static struct volume_ramp_params *reset_volume_ramp_params(struct impl *this)
{
	if (!this->vol_ramp_sequence) {
		struct volume_ramp_params *vrp = &this->props.vrp;
		spa_zero(this->props.vrp);
		return vrp;
	}
	return 0;
}

static int apply_props(struct impl *this, const struct spa_pod *param)
{
	struct spa_pod_prop *prop;
	struct spa_pod_object *obj = (struct spa_pod_object *) param;
	struct props *p = &this->props;
	bool have_channel_volume = false;
	bool have_soft_volume = false;
	int changed = 0;
	int vol_ramp_params_changed = 0;
	struct volume_ramp_params *vrp = reset_volume_ramp_params(this);
	uint32_t n;
	int32_t value;
	uint32_t id;

	SPA_POD_OBJECT_FOREACH(obj, prop) {
		switch (prop->key) {
		case SPA_PROP_volume:
			p->prev_volume = p->volume;

			if (!p->lock_volumes &&
			    spa_pod_get_float(&prop->value, &p->volume) == 0) {
				spa_log_debug(this->log, "%p new volume %f", this, p->volume);
				changed++;
			}
			break;
		case SPA_PROP_mute:
			if (!p->lock_volumes &&
			    spa_pod_get_bool(&prop->value, &p->channel.mute) == 0) {
				have_channel_volume = true;
				changed++;
			}
			break;
		case SPA_PROP_volumeRampSamples:
			if (this->vol_ramp_sequence) {
				spa_log_error(this->log, "%p volume ramp sequence is being "
						"applied try again", this);
				break;
			}

			if (spa_pod_get_int(&prop->value, &value) == 0 && value) {
				vrp->volume_ramp_samples = value;
				spa_log_info(this->log, "%p volume ramp samples %d", this, value);
				vol_ramp_params_changed++;
			}
			break;
		case SPA_PROP_volumeRampStepSamples:
			if (this->vol_ramp_sequence) {
				spa_log_error(this->log, "%p volume ramp sequence is being "
						"applied try again", this);
				break;
			}

			if (spa_pod_get_int(&prop->value, &value) == 0 && value) {
				vrp->volume_ramp_step_samples = value;
				spa_log_info(this->log, "%p volume ramp step samples is %d",
						this, value);
			}
			break;
		case SPA_PROP_volumeRampTime:
			if (this->vol_ramp_sequence) {
				spa_log_error(this->log, "%p volume ramp sequence is being "
						"applied try again", this);
				break;
			}

			if (spa_pod_get_int(&prop->value, &value) == 0 && value) {
				vrp->volume_ramp_time = value;
				spa_log_info(this->log, "%p volume ramp time %d", this, value);
				vol_ramp_params_changed++;
			}
			break;
		case SPA_PROP_volumeRampStepTime:
			if (this->vol_ramp_sequence) {
				spa_log_error(this->log, "%p volume ramp sequence is being "
						"applied try again", this);
				break;
			}

			if (spa_pod_get_int(&prop->value, &value) == 0 && value) {
				vrp->volume_ramp_step_time = value;
				spa_log_info(this->log, "%p volume ramp time %d", this, value);
			}
			break;
		case SPA_PROP_volumeRampScale:
			if (this->vol_ramp_sequence) {
				spa_log_error(this->log, "%p volume ramp sequence is being "
						"applied try again", this);
				break;
			}

			if (spa_pod_get_id(&prop->value, &id) == 0 && id) {
				vrp->scale = id;
				spa_log_info(this->log, "%p volume ramp scale %d", this, id);
			}
			break;
		case SPA_PROP_channelVolumes:
			if (!p->lock_volumes &&
			    (n = spa_pod_copy_array(&prop->value, SPA_TYPE_Float,
					p->channel.volumes, SPA_AUDIO_MAX_CHANNELS)) > 0) {
				have_channel_volume = true;
				p->channel.n_volumes = n;
				changed++;
			}
			break;
		case SPA_PROP_channelMap:
			if ((n = spa_pod_copy_array(&prop->value, SPA_TYPE_Id,
					p->channel_map, SPA_AUDIO_MAX_CHANNELS)) > 0) {
				p->n_channels = n;
				changed++;
			}
			break;
		case SPA_PROP_softMute:
			if (!p->lock_volumes &&
			    spa_pod_get_bool(&prop->value, &p->soft.mute) == 0) {
				have_soft_volume = true;
				changed++;
			}
			break;
		case SPA_PROP_softVolumes:
			if (!p->lock_volumes &&
			    (n = spa_pod_copy_array(&prop->value, SPA_TYPE_Float,
					p->soft.volumes, SPA_AUDIO_MAX_CHANNELS)) > 0) {
				have_soft_volume = true;
				p->soft.n_volumes = n;
				changed++;
			}
			break;
		case SPA_PROP_monitorMute:
			if (spa_pod_get_bool(&prop->value, &p->monitor.mute) == 0)
				changed++;
			break;
		case SPA_PROP_monitorVolumes:
			if ((n = spa_pod_copy_array(&prop->value, SPA_TYPE_Float,
					p->monitor.volumes, SPA_AUDIO_MAX_CHANNELS)) > 0) {
				p->monitor.n_volumes = n;
				changed++;
			}
			break;
		case SPA_PROP_rate:
			if (spa_pod_get_double(&prop->value, &p->rate) == 0 &&
			    !this->rate_adjust && p->rate != 1.0) {
				this->rate_adjust = true;
				spa_log_info(this->log, "%p: activating adaptive resampler",
						this);
			}
			break;
		case SPA_PROP_params:
			if (this->filter_props_count == 0)
				changed += parse_prop_params(this, &prop->value);
			break;
		default:
			break;
		}
	}
	if (changed) {
		if (have_soft_volume)
			p->have_soft_volume = true;
		else if (have_channel_volume)
			p->have_soft_volume = false;

		set_volume(this);
		this->recalc = true;
	}

	if (!p->lock_volumes && vol_ramp_params_changed) {
		void *sequence = NULL;
		if (p->volume == p->prev_volume)
			spa_log_error(this->log, "no change in volume, cannot ramp volume");
		else if (p->volume > p->prev_volume)
			sequence = generate_ramp_up_seq(this);
		else
			sequence = generate_ramp_down_seq(this);

		if (!sequence)
			spa_log_error(this->log, "unable to generate sequence");

		this->vol_ramp_sequence = (struct spa_pod_sequence *) sequence;
		this->vol_ramp_offset = 0;
		this->recalc = true;
	}
	return changed;
}

static int apply_midi(struct impl *this, const struct spa_pod *value)
{
	struct props *p = &this->props;
	uint8_t data[8];
	int size;

	size = spa_ump_to_midi(SPA_POD_BODY(value), SPA_POD_BODY_SIZE(value),
			data, sizeof(data));
	if (size < 3)
		return -EINVAL;

	if ((data[0] & 0xf0) != 0xb0 || data[1] != 7)
		return 0;

	p->volume = data[2] / 127.0f;
	set_volume(this);
	return 1;
}

static int reconfigure_mode(struct impl *this, enum spa_param_port_config_mode mode,
		enum spa_direction direction, bool monitor, bool control, struct spa_audio_info *info)
{
	struct dir *dir;
	uint32_t i;

	dir = &this->dir[direction];

	if (dir->have_profile && this->monitor == monitor && dir->mode == mode &&
	    dir->control == control &&
	    (info == NULL || memcmp(&dir->format, info, sizeof(*info)) == 0))
		return 0;

	spa_log_debug(this->log, "%p: port config direction:%d monitor:%d "
			"control:%d mode:%d %d", this, direction, monitor,
			control, mode, dir->n_ports);

	for (i = 0; i < dir->n_ports; i++) {
		deinit_port(this, direction, i);
		if (this->monitor && direction == SPA_DIRECTION_INPUT)
			deinit_port(this, SPA_DIRECTION_OUTPUT, i+1);
	}

	this->monitor = monitor;
	this->setup = false;
	dir->control = control;
	dir->have_profile = true;
	dir->mode = mode;

	switch (mode) {
	case SPA_PARAM_PORT_CONFIG_MODE_dsp:
	{
		if (info) {
			dir->n_ports = info->info.raw.channels;
			dir->format = *info;
			dir->format.info.raw.format = SPA_AUDIO_FORMAT_DSP_F32;
			dir->format.info.raw.rate = 0;
			dir->have_format = true;
		} else {
			dir->n_ports = 0;
		}

		if (this->monitor && direction == SPA_DIRECTION_INPUT)
			this->dir[SPA_DIRECTION_OUTPUT].n_ports = dir->n_ports + 1;

		for (i = 0; i < dir->n_ports; i++) {
			init_port(this, direction, i, info->info.raw.position[i], true, false, false);
			if (this->monitor && direction == SPA_DIRECTION_INPUT)
				init_port(this, SPA_DIRECTION_OUTPUT, i+1,
					info->info.raw.position[i], true, true, false);
		}
		break;
	}
	case SPA_PARAM_PORT_CONFIG_MODE_convert:
	{
		dir->n_ports = 1;
		dir->have_format = false;
		init_port(this, direction, 0, 0, false, false, false);
		break;
	}
	case SPA_PARAM_PORT_CONFIG_MODE_none:
		break;
	default:
		return -ENOTSUP;
	}
	if (direction == SPA_DIRECTION_INPUT && dir->control) {
		i = dir->n_ports++;
		init_port(this, direction, i, 0, false, false, true);
	}

	this->info.change_mask |= SPA_NODE_CHANGE_MASK_FLAGS | SPA_NODE_CHANGE_MASK_PARAMS;
	this->info.flags &= ~SPA_NODE_FLAG_NEED_CONFIGURE;
	this->params[IDX_Props].user++;
	this->params[IDX_PortConfig].user++;
	return 0;
}

static int impl_node_set_param(void *object, uint32_t id, uint32_t flags,
			       const struct spa_pod *param)
{
	struct impl *this = object;

	spa_return_val_if_fail(this != NULL, -EINVAL);

	if (param == NULL)
		return 0;

	switch (id) {
	case SPA_PARAM_PortConfig:
	{
		struct spa_audio_info info = { 0, }, *infop = NULL;
		struct spa_pod *format = NULL;
		enum spa_direction direction;
		enum spa_param_port_config_mode mode;
		bool monitor = false, control = false;
		int res;

		if (spa_pod_parse_object(param,
				SPA_TYPE_OBJECT_ParamPortConfig, NULL,
				SPA_PARAM_PORT_CONFIG_direction,	SPA_POD_Id(&direction),
				SPA_PARAM_PORT_CONFIG_mode,		SPA_POD_Id(&mode),
				SPA_PARAM_PORT_CONFIG_monitor,		SPA_POD_OPT_Bool(&monitor),
				SPA_PARAM_PORT_CONFIG_control,		SPA_POD_OPT_Bool(&control),
				SPA_PARAM_PORT_CONFIG_format,		SPA_POD_OPT_Pod(&format)) < 0)
			return -EINVAL;

		if (format) {
			if (!spa_pod_is_object_type(format, SPA_TYPE_OBJECT_Format))
				return -EINVAL;

			if ((res = spa_format_parse(format, &info.media_type, &info.media_subtype)) < 0)
				return res;

			if (info.media_type != SPA_MEDIA_TYPE_audio ||
			    info.media_subtype != SPA_MEDIA_SUBTYPE_raw)
				return -EINVAL;

			if (spa_format_audio_raw_parse(format, &info.info.raw) < 0)
				return -EINVAL;

			if (info.info.raw.channels == 0 ||
			    info.info.raw.channels > SPA_AUDIO_MAX_CHANNELS)
				return -EINVAL;

			infop = &info;
		}

		if ((res = reconfigure_mode(this, mode, direction, monitor, control, infop)) < 0)
			return res;

		emit_node_info(this, false);
		break;
	}
	case SPA_PARAM_Props:
	{
		uint32_t i;
		bool have_graph = false;
		this->filter_props_count = 0;
		for (i = 0; i < MAX_GRAPH; i++) {
			struct filter_graph *g = &this->filter_graph[i];
			if (!g->active)
				continue;

			have_graph = true;

			this->in_filter_props++;
			spa_filter_graph_set_props(g->graph,
					SPA_DIRECTION_INPUT, param);
			this->filter_props_count++;
			this->in_filter_props--;
		}
		if (!have_graph && apply_props(this, param) > 0)
			emit_node_info(this, false);

		clean_filter_handles(this, false);
		break;
	}
	default:
		return -ENOENT;
	}
	return 0;
}

static int int32_cmp(const void *v1, const void *v2)
{
	int32_t a1 = *(int32_t*)v1;
	int32_t a2 = *(int32_t*)v2;
	if (a1 == 0 && a2 != 0)
		return 1;
	if (a2 == 0 && a1 != 0)
		return -1;
	return a1 - a2;
}

static int setup_in_convert(struct impl *this)
{
	uint32_t i, j;
	struct dir *in = &this->dir[SPA_DIRECTION_INPUT];
	struct spa_audio_info src_info, dst_info;
	int res;
	bool remap = false;

	src_info = in->format;
	dst_info = src_info;
	dst_info.info.raw.format = SPA_AUDIO_FORMAT_DSP_F32;

	spa_log_info(this->log, "%p: %s/%d@%d->%s/%d@%d", this,
			spa_debug_type_find_name(spa_type_audio_format, src_info.info.raw.format),
			src_info.info.raw.channels,
			src_info.info.raw.rate,
			spa_debug_type_find_name(spa_type_audio_format, dst_info.info.raw.format),
			dst_info.info.raw.channels,
			dst_info.info.raw.rate);

	qsort(dst_info.info.raw.position, dst_info.info.raw.channels,
					sizeof(uint32_t), int32_cmp);

	for (i = 0; i < src_info.info.raw.channels; i++) {
		for (j = 0; j < dst_info.info.raw.channels; j++) {
			if (src_info.info.raw.position[i] !=
			    dst_info.info.raw.position[j])
				continue;
			in->remap[i] = j;
			if (i != j)
				remap = true;
			spa_log_debug(this->log, "%p: channel %d (%d) -> %d (%s -> %s)", this,
					i, in->remap[i], j,
					spa_debug_type_find_short_name(spa_type_audio_channel,
						src_info.info.raw.position[i]),
					spa_debug_type_find_short_name(spa_type_audio_channel,
						dst_info.info.raw.position[j]));
			dst_info.info.raw.position[j] = -1;
			break;
		}
	}
	if (in->conv.free)
		convert_free(&in->conv);

	in->conv.src_fmt = src_info.info.raw.format;
	in->conv.dst_fmt = dst_info.info.raw.format;
	in->conv.n_channels = dst_info.info.raw.channels;
	in->conv.cpu_flags = this->cpu_flags;
	in->need_remap = remap;

	if ((res = convert_init(&in->conv)) < 0)
		return res;

	spa_log_debug(this->log, "%p: got converter features %08x:%08x passthrough:%d remap:%d %s", this,
			this->cpu_flags, in->conv.cpu_flags, in->conv.is_passthrough,
			remap, in->conv.func_name);

	return 0;
}

static void fix_volumes(struct impl *this, struct volumes *vols, uint32_t channels)
{
	float s;
	uint32_t i;
	spa_log_debug(this->log, "%p %d -> %d", this, vols->n_volumes, channels);
	if (vols->n_volumes > 0) {
		s = 0.0f;
		for (i = 0; i < vols->n_volumes; i++)
			s += vols->volumes[i];
		s /= vols->n_volumes;
	} else {
		s = 1.0f;
	}
	vols->n_volumes = channels;
	for (i = 0; i < vols->n_volumes; i++)
		vols->volumes[i] = s;
}

static int remap_volumes(struct impl *this, const struct spa_audio_info *info)
{
	struct props *p = &this->props;
	uint32_t i, j, target = info->info.raw.channels;

	for (i = 0; i < p->n_channels; i++) {
		for (j = i; j < target; j++) {
			spa_log_debug(this->log, "%d %d: %d <-> %d", i, j,
					p->channel_map[i], info->info.raw.position[j]);
			if (p->channel_map[i] != info->info.raw.position[j])
				continue;
			if (i != j) {
				SPA_SWAP(p->channel_map[i], p->channel_map[j]);
				SPA_SWAP(p->channel.volumes[i], p->channel.volumes[j]);
				SPA_SWAP(p->soft.volumes[i], p->soft.volumes[j]);
				SPA_SWAP(p->monitor.volumes[i], p->monitor.volumes[j]);
			}
			break;
		}
	}
	p->n_channels = target;
	for (i = 0; i < p->n_channels; i++)
		p->channel_map[i] = info->info.raw.position[i];

	if (target == 0)
		return 0;
	if (p->channel.n_volumes != target)
		fix_volumes(this, &p->channel, target);
	if (p->soft.n_volumes != target)
		fix_volumes(this, &p->soft, target);
	if (p->monitor.n_volumes != target)
		fix_volumes(this, &p->monitor, target);

	return 1;
}

static void set_volume(struct impl *this)
{
	struct volumes *vol;
	uint32_t i;
	float volumes[SPA_AUDIO_MAX_CHANNELS];
	struct dir *dir = &this->dir[this->direction];

	spa_log_debug(this->log, "%p set volume %f have_format:%d", this, this->props.volume, dir->have_format);

	if (dir->have_format)
		remap_volumes(this, &dir->format);

	if (this->mix.set_volume == NULL)
		return;

	if (this->props.have_soft_volume)
		vol = &this->props.soft;
	else
		vol = &this->props.channel;

	for (i = 0; i < vol->n_volumes; i++)
		volumes[i] = SPA_CLAMPF(vol->volumes[dir->remap[i]],
				this->props.min_volume, this->props.max_volume);

	channelmix_set_volume(&this->mix,
			SPA_CLAMPF(this->props.volume, this->props.min_volume, this->props.max_volume),
			vol->mute, vol->n_volumes, volumes);

	this->info.change_mask |= SPA_NODE_CHANGE_MASK_PARAMS;
	this->params[IDX_Props].user++;
}

static char *format_position(char *str, size_t len, uint32_t channels, uint32_t *position)
{
	uint32_t i, idx = 0;
	for (i = 0; i < channels; i++)
		idx += snprintf(str + idx, len - idx, "%s%s", i == 0 ? "" : " ",
				spa_debug_type_find_short_name(spa_type_audio_channel,
					position[i]));
	return str;
}

static int setup_channelmix(struct impl *this, uint32_t channels, uint32_t *position)
{
	struct dir *in = &this->dir[SPA_DIRECTION_INPUT];
	struct dir *out = &this->dir[SPA_DIRECTION_OUTPUT];
	uint32_t i, src_chan, dst_chan, p;
	uint64_t src_mask, dst_mask;
	char str[1024];
	int res;

	src_chan = channels;
	dst_chan = out->format.info.raw.channels;

	for (i = 0, src_mask = 0; i < src_chan; i++) {
		p = position[i];
		src_mask |= 1ULL << (p < 64 ? p : 0);
	}
	for (i = 0, dst_mask = 0; i < dst_chan; i++) {
		p = out->format.info.raw.position[i];
		dst_mask |= 1ULL << (p < 64 ? p : 0);
	}

	/* if we needed a channel conversion but we already did one before this
	 * stage, assume we are now with the dst layout */
	if ((out->format.info.raw.channels != in->format.info.raw.channels) &&
	    channels != in->format.info.raw.channels)
		src_mask = dst_mask;

	spa_log_info(this->log, "in  %s (%016"PRIx64")", format_position(str, sizeof(str),
				src_chan, position), src_mask);
	spa_log_info(this->log, "out %s (%016"PRIx64")", format_position(str, sizeof(str),
				dst_chan, out->format.info.raw.position), dst_mask);

	spa_log_info(this->log, "%p: %s/%d@%d->%s/%d@%d %08"PRIx64":%08"PRIx64, this,
			spa_debug_type_find_name(spa_type_audio_format, SPA_AUDIO_FORMAT_DSP_F32),
			src_chan,
			in->format.info.raw.rate,
			spa_debug_type_find_name(spa_type_audio_format, SPA_AUDIO_FORMAT_DSP_F32),
			dst_chan,
			in->format.info.raw.rate,
			src_mask, dst_mask);

	if (this->props.mix_disabled &&
	    (src_chan != dst_chan || src_mask != dst_mask))
		return -EPERM;

	this->mix.src_chan = src_chan;
	this->mix.src_mask = src_mask;
	this->mix.dst_chan = dst_chan;
	this->mix.dst_mask = dst_mask;
	this->mix.cpu_flags = this->cpu_flags;
	this->mix.log = this->log;
	this->mix.freq = in->format.info.raw.rate;

	if ((res = channelmix_init(&this->mix)) < 0)
		return res;

	set_volume(this);

	spa_log_debug(this->log, "%p: got channelmix features %08x:%08x flags:%08x %s",
			this, this->cpu_flags, this->mix.cpu_flags,
			this->mix.flags, this->mix.func_name);
	return 0;
}

static int setup_resample(struct impl *this)
{
	struct dir *in = &this->dir[SPA_DIRECTION_INPUT];
	struct dir *out = &this->dir[SPA_DIRECTION_OUTPUT];
	int res;
	uint32_t channels;

	if (this->direction == SPA_DIRECTION_INPUT)
		channels = in->format.info.raw.channels;
	else
		channels = out->format.info.raw.channels;

	spa_log_info(this->log, "%p: %s/%d@%d->%s/%d@%d", this,
			spa_debug_type_find_name(spa_type_audio_format, SPA_AUDIO_FORMAT_DSP_F32),
			channels,
			in->format.info.raw.rate,
			spa_debug_type_find_name(spa_type_audio_format, SPA_AUDIO_FORMAT_DSP_F32),
			channels,
			out->format.info.raw.rate);

	if (this->props.resample_disabled && !this->resample_peaks &&
	    in->format.info.raw.rate != out->format.info.raw.rate)
		return -EPERM;

	if (this->resample.free)
		resample_free(&this->resample);

	this->resample.channels = channels;
	this->resample.i_rate = in->format.info.raw.rate;
	this->resample.o_rate = out->format.info.raw.rate;
	this->resample.log = this->log;
	this->resample.quality = this->props.resample_quality;
	this->resample.cpu_flags = this->cpu_flags;

	this->rate_adjust = this->props.rate != 1.0;

	if (this->resample_peaks)
		res = resample_peaks_init(&this->resample);
	else
		res = resample_native_init(&this->resample);

	spa_log_debug(this->log, "%p: got resample features %08x:%08x %s",
			this, this->cpu_flags, this->resample.cpu_flags,
			this->resample.func_name);
	return res;
}

static int calc_width(struct spa_audio_info *info)
{
	switch (info->info.raw.format) {
	case SPA_AUDIO_FORMAT_U8:
	case SPA_AUDIO_FORMAT_U8P:
	case SPA_AUDIO_FORMAT_S8:
	case SPA_AUDIO_FORMAT_S8P:
	case SPA_AUDIO_FORMAT_ULAW:
	case SPA_AUDIO_FORMAT_ALAW:
		return 1;
	case SPA_AUDIO_FORMAT_S16P:
	case SPA_AUDIO_FORMAT_S16:
	case SPA_AUDIO_FORMAT_S16_OE:
		return 2;
	case SPA_AUDIO_FORMAT_S24P:
	case SPA_AUDIO_FORMAT_S24:
	case SPA_AUDIO_FORMAT_S24_OE:
		return 3;
	case SPA_AUDIO_FORMAT_F64P:
	case SPA_AUDIO_FORMAT_F64:
	case SPA_AUDIO_FORMAT_F64_OE:
		return 8;
	default:
		return 4;
	}
}

static int setup_out_convert(struct impl *this)
{
	uint32_t i, j;
	struct dir *out = &this->dir[SPA_DIRECTION_OUTPUT];
	struct spa_audio_info src_info, dst_info;
	int res;
	bool remap = false;

	dst_info = out->format;
	src_info = dst_info;
	src_info.info.raw.format = SPA_AUDIO_FORMAT_DSP_F32;

	spa_log_info(this->log, "%p: %s/%d@%d->%s/%d@%d", this,
			spa_debug_type_find_name(spa_type_audio_format, src_info.info.raw.format),
			src_info.info.raw.channels,
			src_info.info.raw.rate,
			spa_debug_type_find_name(spa_type_audio_format, dst_info.info.raw.format),
			dst_info.info.raw.channels,
			dst_info.info.raw.rate);

	qsort(src_info.info.raw.position, src_info.info.raw.channels,
					sizeof(uint32_t), int32_cmp);

	for (i = 0; i < src_info.info.raw.channels; i++) {
		for (j = 0; j < dst_info.info.raw.channels; j++) {
			if (src_info.info.raw.position[i] !=
			    dst_info.info.raw.position[j])
				continue;
			out->remap[i] = j;
			if (i != j)
				remap = true;

			spa_log_debug(this->log, "%p: channel %d (%d) -> %d (%s -> %s)", this,
					i, out->remap[i], j,
					spa_debug_type_find_short_name(spa_type_audio_channel,
						src_info.info.raw.position[i]),
					spa_debug_type_find_short_name(spa_type_audio_channel,
						dst_info.info.raw.position[j]));
			dst_info.info.raw.position[j] = -1;
			break;
		}
	}
	if (out->conv.free)
		convert_free(&out->conv);

	out->conv.src_fmt = src_info.info.raw.format;
	out->conv.dst_fmt = dst_info.info.raw.format;
	out->conv.rate = dst_info.info.raw.rate;
	out->conv.n_channels = dst_info.info.raw.channels;
	out->conv.cpu_flags = this->cpu_flags;
	out->need_remap = remap;

	if ((res = convert_init(&out->conv)) < 0)
		return res;

	spa_log_debug(this->log, "%p: got converter features %08x:%08x quant:%d:%d"
			" passthrough:%d remap:%d %s", this,
			this->cpu_flags, out->conv.cpu_flags, out->conv.method,
			out->conv.noise_bits, out->conv.is_passthrough, remap, out->conv.func_name);

	return 0;
}

static void free_tmp(struct impl *this)
{
	uint32_t i;

	spa_log_debug(this->log, "free tmp %d", this->scratch_size);

	free(this->empty);
	this->empty = NULL;
	this->scratch_size = 0;
	this->scratch_ports = 0;
	free(this->scratch);
	this->scratch = NULL;
	free(this->tmp[0]);
	this->tmp[0] = NULL;
	free(this->tmp[1]);
	this->tmp[1] = NULL;
	for (i = 0; i < MAX_PORTS; i++) {
		this->tmp_datas[0][i] = NULL;
		this->tmp_datas[1][i] = NULL;
	}
}

static int ensure_tmp(struct impl *this, uint32_t maxsize, uint32_t maxports)
{
	if (maxsize > this->scratch_size || maxports > this->scratch_ports) {
		float *empty, *scratch, *tmp[2];
		uint32_t i;

		spa_log_debug(this->log, "resize tmp %d -> %d", this->scratch_size, maxsize);

		if ((empty = realloc(this->empty, maxsize + MAX_ALIGN)) != NULL)
			this->empty = empty;
		if ((scratch = realloc(this->scratch, maxsize + MAX_ALIGN)) != NULL)
			this->scratch = scratch;
		if ((tmp[0] = realloc(this->tmp[0], (maxsize + MAX_ALIGN) * maxports)) != NULL)
			this->tmp[0] = tmp[0];
		if ((tmp[1] = realloc(this->tmp[1], (maxsize + MAX_ALIGN) * maxports)) != NULL)
			this->tmp[1] = tmp[1];

		if (empty == NULL || scratch == NULL || tmp[0] == NULL || tmp[1] == NULL) {
			free_tmp(this);
			return -ENOMEM;
		}
		memset(this->empty, 0, maxsize + MAX_ALIGN);
		this->scratch_size = maxsize;
		this->scratch_ports = maxports;

		for (i = 0; i < maxports; i++) {
			this->tmp_datas[0][i] = SPA_PTROFF(tmp[0], maxsize * i, void);
			this->tmp_datas[0][i] = SPA_PTR_ALIGN(this->tmp_datas[0][i], MAX_ALIGN, void);
			this->tmp_datas[1][i] = SPA_PTROFF(tmp[1], maxsize * i, void);
			this->tmp_datas[1][i] = SPA_PTR_ALIGN(this->tmp_datas[1][i], MAX_ALIGN, void);
		}
	}
	return 0;
}

static uint32_t resample_update_rate_match(struct impl *this, bool passthrough, uint32_t size, uint32_t queued)
{
	uint32_t delay, match_size;
	int32_t delay_frac;

	if (passthrough) {
		delay = 0;
		delay_frac = 0;
		match_size = size;
	} else {
		/* Only apply rate_scale if we're working in DSP mode (i.e. in driver rate) */
		double scale = this->dir[SPA_DIRECTION_REVERSE(this->direction)].mode == SPA_PARAM_PORT_CONFIG_MODE_dsp ?
			this->rate_scale : 1.0;
		double rate = scale / this->props.rate;
		double fdelay;

		if (this->io_rate_match &&
		    SPA_FLAG_IS_SET(this->io_rate_match->flags, SPA_IO_RATE_MATCH_FLAG_ACTIVE))
			rate *= this->io_rate_match->rate;
		resample_update_rate(&this->resample, rate);
		fdelay = resample_delay(&this->resample) + resample_phase(&this->resample);
		if (this->direction == SPA_DIRECTION_INPUT) {
			match_size = resample_in_len(&this->resample, size);
		} else {
			fdelay *= rate * this->resample.o_rate / this->resample.i_rate;
			match_size = resample_out_len(&this->resample, size);
		}

		delay = (uint32_t)round(fdelay);
		delay_frac = (int32_t)((fdelay - delay) * 1e9);
	}
	match_size -= SPA_MIN(match_size, queued);

	spa_log_trace_fp(this->log, "%p: next match %u %u %u", this, match_size, size, queued);

	if (this->io_rate_match) {
		this->io_rate_match->delay = delay + queued;
		this->io_rate_match->delay_frac = delay_frac;
		this->io_rate_match->size = match_size;
	}
	return match_size;
}

static inline bool resample_is_passthrough(struct impl *this)
{
	if (this->props.resample_disabled)
		return true;
	if (this->resample.i_rate != this->resample.o_rate)
		return false;
	if (this->rate_scale != 1.0)
		return false;
	if (this->rate_adjust)
		return false;
	if (this->io_rate_match != NULL &&
	    SPA_FLAG_IS_SET(this->io_rate_match->flags, SPA_IO_RATE_MATCH_FLAG_ACTIVE))
		return false;
	return true;
}

static int setup_convert(struct impl *this)
{
	struct dir *in, *out;
	uint32_t i, rate, maxsize, maxports, duration, channels, *position;
	struct port *p;
	int res;

	in = &this->dir[SPA_DIRECTION_INPUT];
	out = &this->dir[SPA_DIRECTION_OUTPUT];

	spa_log_debug(this->log, "%p: setup:%d in_format:%d out_format:%d", this,
			this->setup, in->have_format, out->have_format);

	if (this->setup)
		return 0;

	if (!in->have_format || !out->have_format)
		return -EINVAL;

	if (this->io_position != NULL) {
		rate = this->io_position->clock.target_rate.denom;
		duration = this->io_position->clock.target_duration;
	} else {
		rate = DEFAULT_RATE;
		duration = this->quantum_limit;
	}

	/* in DSP mode we always convert to the DSP rate */
	if (in->mode == SPA_PARAM_PORT_CONFIG_MODE_dsp)
		in->format.info.raw.rate = rate;
	if (out->mode == SPA_PARAM_PORT_CONFIG_MODE_dsp)
		out->format.info.raw.rate = rate;

	/* try to passthrough the rates */
	if (in->format.info.raw.rate == 0)
		in->format.info.raw.rate = out->format.info.raw.rate;
	else if (out->format.info.raw.rate == 0)
		out->format.info.raw.rate = in->format.info.raw.rate;

	/* try to passthrough the channels */
	if (in->format.info.raw.channels == 0)
		in->format.info.raw.channels = out->format.info.raw.channels;
	else if (out->format.info.raw.channels == 0)
		out->format.info.raw.channels = in->format.info.raw.channels;

	if (in->format.info.raw.rate == 0 || out->format.info.raw.rate == 0)
		return -EINVAL;
	if (in->format.info.raw.channels == 0 || out->format.info.raw.channels == 0)
		return -EINVAL;

	channels = in->format.info.raw.channels;
	position = in->format.info.raw.position;
	maxports = SPA_MAX(in->format.info.raw.channels, out->format.info.raw.channels);

	if ((res = setup_in_convert(this)) < 0)
		return res;
	for (i = 0; i < this->n_graph; i++) {
		struct filter_graph *g = &this->filter_graph[this->graph_index[i]];
		if (!g->active)
			continue;
		if ((res = setup_filter_graph(this, g, channels, position)) < 0)
			return res;
		channels = g->n_outputs;
		position = g->outputs_position;
		maxports = SPA_MAX(maxports, channels);
	}
	if ((res = setup_channelmix(this, channels, position)) < 0)
		return res;
	if ((res = setup_resample(this)) < 0)
		return res;
	if ((res = setup_out_convert(this)) < 0)
		return res;

	maxsize = this->quantum_limit * sizeof(float);
	for (i = 0; i < in->n_ports; i++) {
		p = GET_IN_PORT(this, i);
		maxsize = SPA_MAX(maxsize, p->maxsize);
	}
	for (i = 0; i < out->n_ports; i++) {
		p = GET_OUT_PORT(this, i);
		maxsize = SPA_MAX(maxsize, p->maxsize);
	}
	if ((res = ensure_tmp(this, maxsize, maxports)) < 0)
		return res;

	resample_update_rate_match(this, resample_is_passthrough(this), duration, 0);

	this->setup = true;
	this->recalc = true;

	emit_node_info(this, false);

	return 0;
}

static void reset_node(struct impl *this)
{
	uint32_t i;
	for (i = 0; i < MAX_GRAPH; i++) {
		struct filter_graph *g = &this->filter_graph[i];
		if (g->graph)
			spa_filter_graph_deactivate(g->graph);
	}
	if (this->resample.reset)
		resample_reset(&this->resample);
	this->in_offset = 0;
	this->out_offset = 0;
}

static int impl_node_send_command(void *object, const struct spa_command *command)
{
	struct impl *this = object;
	int res;

	spa_return_val_if_fail(this != NULL, -EINVAL);
	spa_return_val_if_fail(command != NULL, -EINVAL);

	switch (SPA_NODE_COMMAND_ID(command)) {
	case SPA_NODE_COMMAND_Start:
		if (this->started)
			return 0;
		if ((res = setup_convert(this)) < 0)
			return res;
		this->started = true;
		break;
	case SPA_NODE_COMMAND_Suspend:
		this->setup = false;
		SPA_FALLTHROUGH;
	case SPA_NODE_COMMAND_Pause:
		this->started = false;
		break;
	case SPA_NODE_COMMAND_Flush:
		reset_node(this);
		break;
	default:
		return -ENOTSUP;
	}
	return 0;
}

static int
impl_node_add_listener(void *object,
		struct spa_hook *listener,
		const struct spa_node_events *events,
		void *data)
{
	struct impl *this = object;
	uint32_t i;
	struct spa_hook_list save;
	struct port *p;

	spa_return_val_if_fail(this != NULL, -EINVAL);

	spa_log_trace(this->log, "%p: add listener %p", this, listener);
	spa_hook_list_isolate(&this->hooks, &save, listener, events, data);

	emit_node_info(this, true);
	for (i = 0; i < this->dir[SPA_DIRECTION_INPUT].n_ports; i++) {
		if ((p = GET_IN_PORT(this, i)) && p->valid)
			emit_port_info(this, p, true);
	}
	for (i = 0; i < this->dir[SPA_DIRECTION_OUTPUT].n_ports; i++) {
		if ((p = GET_OUT_PORT(this, i)) && p->valid)
			emit_port_info(this, p, true);
	}
	spa_hook_list_join(&this->hooks, &save);

	return 0;
}

static int
impl_node_set_callbacks(void *object,
			const struct spa_node_callbacks *callbacks,
			void *user_data)
{
	return 0;
}

static int impl_node_add_port(void *object, enum spa_direction direction, uint32_t port_id,
		const struct spa_dict *props)
{
	return -ENOTSUP;
}

static int
impl_node_remove_port(void *object, enum spa_direction direction, uint32_t port_id)
{
	return -ENOTSUP;
}

static int port_enum_formats(void *object,
			     enum spa_direction direction, uint32_t port_id,
			     uint32_t index,
			     struct spa_pod **param,
			     struct spa_pod_builder *builder)
{
	struct impl *this = object;

	switch (index) {
	case 0:
		if (PORT_IS_DSP(this, direction, port_id)) {
			struct spa_audio_info_dsp info;
			info.format = SPA_AUDIO_FORMAT_DSP_F32;
			*param = spa_format_audio_dsp_build(builder,
				SPA_PARAM_EnumFormat, &info);
		} else if (PORT_IS_CONTROL(this, direction, port_id)) {
			*param = spa_pod_builder_add_object(builder,
				SPA_TYPE_OBJECT_Format, SPA_PARAM_EnumFormat,
				SPA_FORMAT_mediaType,      SPA_POD_Id(SPA_MEDIA_TYPE_application),
				SPA_FORMAT_mediaSubtype,   SPA_POD_Id(SPA_MEDIA_SUBTYPE_control),
				SPA_FORMAT_CONTROL_types,  SPA_POD_CHOICE_FLAGS_Int(
					(1u<<SPA_CONTROL_UMP) | (1u<<SPA_CONTROL_Properties)));
		} else {
			struct spa_pod_frame f[1];
			uint32_t rate = this->io_position ?
				this->io_position->clock.target_rate.denom : DEFAULT_RATE;

			spa_pod_builder_push_object(builder, &f[0],
					SPA_TYPE_OBJECT_Format, SPA_PARAM_EnumFormat);
			spa_pod_builder_add(builder,
				SPA_FORMAT_mediaType,      SPA_POD_Id(SPA_MEDIA_TYPE_audio),
				SPA_FORMAT_mediaSubtype,   SPA_POD_Id(SPA_MEDIA_SUBTYPE_raw),
				SPA_FORMAT_AUDIO_format,   SPA_POD_CHOICE_ENUM_Id(25,
							SPA_AUDIO_FORMAT_F32P,
							SPA_AUDIO_FORMAT_F32P,
							SPA_AUDIO_FORMAT_F32,
							SPA_AUDIO_FORMAT_F32_OE,
							SPA_AUDIO_FORMAT_F64P,
							SPA_AUDIO_FORMAT_F64,
							SPA_AUDIO_FORMAT_F64_OE,
							SPA_AUDIO_FORMAT_S32P,
							SPA_AUDIO_FORMAT_S32,
							SPA_AUDIO_FORMAT_S32_OE,
							SPA_AUDIO_FORMAT_S24_32P,
							SPA_AUDIO_FORMAT_S24_32,
							SPA_AUDIO_FORMAT_S24_32_OE,
							SPA_AUDIO_FORMAT_S24P,
							SPA_AUDIO_FORMAT_S24,
							SPA_AUDIO_FORMAT_S24_OE,
							SPA_AUDIO_FORMAT_S16P,
							SPA_AUDIO_FORMAT_S16,
							SPA_AUDIO_FORMAT_S16_OE,
							SPA_AUDIO_FORMAT_S8P,
							SPA_AUDIO_FORMAT_S8,
							SPA_AUDIO_FORMAT_U8P,
							SPA_AUDIO_FORMAT_U8,
							SPA_AUDIO_FORMAT_ULAW,
							SPA_AUDIO_FORMAT_ALAW),
				0);
			if (!this->props.resample_disabled) {
				spa_pod_builder_add(builder,
					SPA_FORMAT_AUDIO_rate,     SPA_POD_CHOICE_RANGE_Int(
						rate, 1, INT32_MAX),
					0);
			}
			spa_pod_builder_add(builder,
				SPA_FORMAT_AUDIO_channels, SPA_POD_CHOICE_RANGE_Int(
					DEFAULT_CHANNELS, 1, SPA_AUDIO_MAX_CHANNELS),
				0);
			*param = spa_pod_builder_pop(builder, &f[0]);
		}
		break;
	default:
		return 0;
	}
	return 1;
}

static int
impl_node_port_enum_params(void *object, int seq,
			   enum spa_direction direction, uint32_t port_id,
			   uint32_t id, uint32_t start, uint32_t num,
			   const struct spa_pod *filter)
{
	struct impl *this = object;
	struct port *port;
	struct spa_pod *param;
	struct spa_pod_builder b = { 0 };
	uint8_t buffer[4096];
	struct spa_result_node_params result;
	uint32_t count = 0;
	int res;

	spa_return_val_if_fail(this != NULL, -EINVAL);
	spa_return_val_if_fail(num != 0, -EINVAL);

	spa_log_debug(this->log, "%p: enum params port %d.%d %d %u",
			this, direction, port_id, seq, id);

	spa_return_val_if_fail(CHECK_PORT(this, direction, port_id), -EINVAL);

	port = GET_PORT(this, direction, port_id);

	result.id = id;
	result.next = start;
      next:
	result.index = result.next++;

	spa_pod_builder_init(&b, buffer, sizeof(buffer));

	switch (id) {
	case SPA_PARAM_EnumFormat:
		if ((res = port_enum_formats(object, direction, port_id, result.index, &param, &b)) <= 0)
			return res;
		break;
	case SPA_PARAM_Format:
		if (!port->have_format)
			return -EIO;
		if (result.index > 0)
			return 0;

		if (PORT_IS_DSP(this, direction, port_id))
			param = spa_format_audio_dsp_build(&b, id, &port->format.info.dsp);
		else if (PORT_IS_CONTROL(this, direction, port_id))
			param = spa_pod_builder_add_object(&b,
				SPA_TYPE_OBJECT_Format,  id,
				SPA_FORMAT_mediaType,    SPA_POD_Id(SPA_MEDIA_TYPE_application),
				SPA_FORMAT_mediaSubtype, SPA_POD_Id(SPA_MEDIA_SUBTYPE_control),
				SPA_FORMAT_CONTROL_types,  SPA_POD_Int(
					(1u<<SPA_CONTROL_UMP) | (1u<<SPA_CONTROL_Properties)));
		else
			param = spa_format_audio_raw_build(&b, id, &port->format.info.raw);
		break;
	case SPA_PARAM_Buffers:
	{
		uint32_t size;

		if (!port->have_format)
			return -EIO;
		if (result.index > 0)
			return 0;

		size = this->quantum_limit;

		if (!PORT_IS_DSP(this, direction, port_id)) {
			uint32_t irate, orate;
			struct dir *dir = &this->dir[direction];

			/* Convert ports are scaled so that they can always
			 * provide one quantum of data. irate is the rate of the
			 * data before it goes into the resampler. */
			irate = dir->format.info.raw.rate;
			/* scale the size for adaptive resampling */
			size += size/2;

			/* collect the other port rate. This is the output of the resampler
			 * and is usually one quantum. */
			dir = &this->dir[SPA_DIRECTION_REVERSE(direction)];
			if (dir->mode == SPA_PARAM_PORT_CONFIG_MODE_dsp)
				orate = this->io_position ? this->io_position->clock.target_rate.denom : DEFAULT_RATE;
			else
				orate = dir->format.info.raw.rate;

			/* scale the buffer size when we can. Only do this when we downsample because
			 * then we need to ask more input data for one quantum. */
			if (irate != 0 && orate != 0 && irate > orate)
				size = SPA_SCALE32_UP(size, irate, orate);
		}

		param = spa_pod_builder_add_object(&b,
			SPA_TYPE_OBJECT_ParamBuffers, id,
			SPA_PARAM_BUFFERS_buffers, SPA_POD_CHOICE_RANGE_Int(1, 1, MAX_BUFFERS),
			SPA_PARAM_BUFFERS_blocks,  SPA_POD_Int(port->blocks),
			SPA_PARAM_BUFFERS_size,    SPA_POD_CHOICE_RANGE_Int(
								size * port->stride,
								16 * port->stride,
								INT32_MAX),
			SPA_PARAM_BUFFERS_stride,  SPA_POD_Int(port->stride));
		break;
	}
	case SPA_PARAM_Meta:
		switch (result.index) {
		case 0:
			param = spa_pod_builder_add_object(&b,
				SPA_TYPE_OBJECT_ParamMeta, id,
				SPA_PARAM_META_type, SPA_POD_Id(SPA_META_Header),
				SPA_PARAM_META_size, SPA_POD_Int(sizeof(struct spa_meta_header)));
			break;
		default:
			return 0;
		}
		break;
	case SPA_PARAM_IO:
		switch (result.index) {
		case 0:
			param = spa_pod_builder_add_object(&b,
				SPA_TYPE_OBJECT_ParamIO, id,
				SPA_PARAM_IO_id,   SPA_POD_Id(SPA_IO_Buffers),
				SPA_PARAM_IO_size, SPA_POD_Int(sizeof(struct spa_io_buffers)));
			break;
		default:
			return 0;
		}
		break;
	case SPA_PARAM_Latency:
		switch (result.index) {
		case 0: case 1:
		{
			uint32_t idx = result.index;
			param = spa_latency_build(&b, id, &port->latency[idx]);
			break;
		}
		default:
			return 0;
		}
		break;
	case SPA_PARAM_Tag:
		switch (result.index) {
		case 0: case 1:
		{
			uint32_t idx = result.index;
			if (port->is_monitor)
				idx = idx ^ 1;
			param = this->dir[idx].tag;
			if (param == NULL)
				goto next;
			break;
		}
		default:
			return 0;
		}
		break;
	default:
		return -ENOENT;
	}

	if (spa_pod_filter(&b, &result.param, param, filter) < 0)
		goto next;

	spa_node_emit_result(&this->hooks, seq, 0, SPA_RESULT_TYPE_NODE_PARAMS, &result);

	if (++count != num)
		goto next;

	return 0;
}

static int clear_buffers(struct impl *this, struct port *port)
{
	if (port->n_buffers > 0) {
		spa_log_debug(this->log, "%p: clear buffers %p", this, port);
		port->n_buffers = 0;
		spa_list_init(&port->queue);
	}
	return 0;
}

static int port_set_latency(void *object,
			   enum spa_direction direction,
			   uint32_t port_id,
			   uint32_t flags,
			   const struct spa_pod *latency)
{
	struct impl *this = object;
	struct port *port, *oport;
	enum spa_direction other = SPA_DIRECTION_REVERSE(direction);
	struct spa_latency_info info;
	bool have_latency, emit = false;;
	uint32_t i;

	spa_log_debug(this->log, "%p: set latency direction:%d id:%d %p",
			this, direction, port_id, latency);

	port = GET_PORT(this, direction, port_id);
	if (latency == NULL) {
		info = SPA_LATENCY_INFO(other);
		have_latency = false;
	} else {
		if (spa_latency_parse(latency, &info) < 0 ||
		    info.direction != other)
			return -EINVAL;
		have_latency = true;
	}
	emit = spa_latency_info_compare(&info, &port->latency[other]) != 0 ||
	    port->have_latency == have_latency;

	port->latency[other] = info;
	port->have_latency = have_latency;

	spa_log_debug(this->log, "%p: set %s latency %f-%f %d-%d %"PRIu64"-%"PRIu64, this,
			info.direction == SPA_DIRECTION_INPUT ? "input" : "output",
			info.min_quantum, info.max_quantum,
			info.min_rate, info.max_rate,
			info.min_ns, info.max_ns);

	if (this->monitor_passthrough) {
		if (port->is_monitor)
			oport = GET_PORT(this, other, port_id-1);
		else if (this->monitor && direction == SPA_DIRECTION_INPUT)
			oport = GET_PORT(this, other, port_id+1);
		else
			return 0;

		if (oport != NULL &&
		    spa_latency_info_compare(&info, &oport->latency[other]) != 0) {
			oport->latency[other] = info;
			oport->info.change_mask |= SPA_PORT_CHANGE_MASK_PARAMS;
			oport->params[IDX_Latency].user++;
			emit_port_info(this, oport, false);
		}
	} else {
		spa_latency_info_combine_start(&info, other);
		for (i = 0; i < this->dir[direction].n_ports; i++) {
			oport = GET_PORT(this, direction, i);
			if ((oport->is_monitor) || !oport->have_latency)
				continue;
			spa_log_debug(this->log, "%p: combine %d", this, i);
			spa_latency_info_combine(&info, &oport->latency[other]);
		}
		spa_latency_info_combine_finish(&info);

		spa_log_debug(this->log, "%p: combined %s latency %f-%f %d-%d %"PRIu64"-%"PRIu64, this,
				info.direction == SPA_DIRECTION_INPUT ? "input" : "output",
				info.min_quantum, info.max_quantum,
				info.min_rate, info.max_rate,
				info.min_ns, info.max_ns);

		for (i = 0; i < this->dir[other].n_ports; i++) {
			oport = GET_PORT(this, other, i);
			if (oport->is_monitor)
				continue;
			spa_log_debug(this->log, "%p: change %d", this, i);
			if (spa_latency_info_compare(&info, &oport->latency[other]) != 0) {
				oport->latency[other] = info;
				oport->info.change_mask |= SPA_PORT_CHANGE_MASK_PARAMS;
				oport->params[IDX_Latency].user++;
				emit_port_info(this, oport, false);
			}
		}
	}
	if (emit) {
		port->info.change_mask |= SPA_PORT_CHANGE_MASK_PARAMS;
		port->params[IDX_Latency].user++;
		emit_port_info(this, port, false);
	}
	return 0;
}

static int port_set_tag(void *object,
			   enum spa_direction direction,
			   uint32_t port_id,
			   uint32_t flags,
			   const struct spa_pod *tag)
{
	struct impl *this = object;
	struct port *port, *oport;
	enum spa_direction other = SPA_DIRECTION_REVERSE(direction);
	uint32_t i;

	spa_log_debug(this->log, "%p: set tag direction:%d id:%d %p",
			this, direction, port_id, tag);

	port = GET_PORT(this, direction, port_id);
	if (port->is_monitor && !this->monitor_passthrough)
		return 0;

	if (tag != NULL) {
		struct spa_tag_info info;
		void *state = NULL;
		if (spa_tag_parse(tag, &info, &state) < 0 ||
		    info.direction != other)
			return -EINVAL;
	}
	if (spa_tag_compare(tag, this->dir[other].tag) != 0) {
		free(this->dir[other].tag);
		this->dir[other].tag = tag ? spa_pod_copy(tag) : NULL;

		for (i = 0; i < this->dir[other].n_ports; i++) {
			oport = GET_PORT(this, other, i);
			oport->info.change_mask |= SPA_PORT_CHANGE_MASK_PARAMS;
			oport->params[IDX_Tag].user++;
			emit_port_info(this, oport, false);
		}
	}
	port->info.change_mask |= SPA_PORT_CHANGE_MASK_PARAMS;
	port->params[IDX_Tag].user++;
	emit_port_info(this, port, false);
	return 0;
}

static int port_set_format(void *object,
			   enum spa_direction direction,
			   uint32_t port_id,
			   uint32_t flags,
			   const struct spa_pod *format)
{
	struct impl *this = object;
	struct port *port;
	int res;

	port = GET_PORT(this, direction, port_id);

	spa_log_debug(this->log, "%p: %d:%d set format", this, direction, port_id);

	if (format == NULL) {
		port->have_format = false;
		clear_buffers(this, port);
	} else {
		struct spa_audio_info info = { 0 };

		if ((res = spa_format_parse(format, &info.media_type, &info.media_subtype)) < 0) {
			spa_log_error(this->log, "can't parse format %s", spa_strerror(res));
			return res;
		}
		if (PORT_IS_DSP(this, direction, port_id)) {
			if (info.media_type != SPA_MEDIA_TYPE_audio ||
			    info.media_subtype != SPA_MEDIA_SUBTYPE_dsp) {
				spa_log_error(this->log, "unexpected types %d/%d",
						info.media_type, info.media_subtype);
				return -EINVAL;
			}
			if ((res = spa_format_audio_dsp_parse(format, &info.info.dsp)) < 0) {
				spa_log_error(this->log, "can't parse format %s", spa_strerror(res));
				return res;
			}
			if (info.info.dsp.format != SPA_AUDIO_FORMAT_DSP_F32) {
				spa_log_error(this->log, "unexpected format %d<->%d",
					info.info.dsp.format, SPA_AUDIO_FORMAT_DSP_F32);
				return -EINVAL;
			}
			port->blocks = 1;
			port->stride = 4;
		}
		else if (PORT_IS_CONTROL(this, direction, port_id)) {
			if (info.media_type != SPA_MEDIA_TYPE_application ||
			    info.media_subtype != SPA_MEDIA_SUBTYPE_control) {
				spa_log_error(this->log, "unexpected types %d/%d",
						info.media_type, info.media_subtype);
				return -EINVAL;
			}
			port->blocks = 1;
			port->stride = 1;
		}
		else {
			if (info.media_type != SPA_MEDIA_TYPE_audio ||
			    info.media_subtype != SPA_MEDIA_SUBTYPE_raw) {
				spa_log_error(this->log, "unexpected types %d/%d",
						info.media_type, info.media_subtype);
				return -EINVAL;
			}
			if ((res = spa_format_audio_raw_parse(format, &info.info.raw)) < 0) {
				spa_log_error(this->log, "can't parse format %s", spa_strerror(res));
				return res;
			}
			if (info.info.raw.format == 0 ||
			    (!this->props.resample_disabled && info.info.raw.rate == 0) ||
			    info.info.raw.channels == 0 ||
			    info.info.raw.channels > SPA_AUDIO_MAX_CHANNELS) {
				spa_log_error(this->log, "invalid format:%d rate:%d channels:%d",
						info.info.raw.format, info.info.raw.rate,
						info.info.raw.channels);
				return -EINVAL;
			}
			port->stride = calc_width(&info);
			if (SPA_AUDIO_FORMAT_IS_PLANAR(info.info.raw.format)) {
				port->blocks = info.info.raw.channels;
			} else {
				port->stride *= info.info.raw.channels;
				port->blocks = 1;
			}
			this->dir[direction].format = info;
			this->dir[direction].have_format = true;
			this->setup = false;
		}
		port->format = info;
		port->have_format = true;

		spa_log_debug(this->log, "%p: %d %d %d", this,
				port_id, port->stride, port->blocks);
	}

	port->info.change_mask |= SPA_PORT_CHANGE_MASK_PARAMS;
	if (port->have_format) {
		port->params[IDX_Format] = SPA_PARAM_INFO(SPA_PARAM_Format, SPA_PARAM_INFO_READWRITE);
		port->params[IDX_Buffers] = SPA_PARAM_INFO(SPA_PARAM_Buffers, SPA_PARAM_INFO_READ);
	} else {
		port->params[IDX_Format] = SPA_PARAM_INFO(SPA_PARAM_Format, SPA_PARAM_INFO_WRITE);
		port->params[IDX_Buffers] = SPA_PARAM_INFO(SPA_PARAM_Buffers, 0);
	}
	emit_port_info(this, port, false);

	return 0;
}


static int
impl_node_port_set_param(void *object,
			 enum spa_direction direction, uint32_t port_id,
			 uint32_t id, uint32_t flags,
			 const struct spa_pod *param)
{
	struct impl *this = object;

	spa_return_val_if_fail(this != NULL, -EINVAL);

	spa_log_debug(this->log, "%p: set param port %d.%d %u",
			this, direction, port_id, id);

	spa_return_val_if_fail(CHECK_PORT(this, direction, port_id), -EINVAL);

	switch (id) {
	case SPA_PARAM_Latency:
		return port_set_latency(this, direction, port_id, flags, param);
	case SPA_PARAM_Tag:
		return port_set_tag(this, direction, port_id, flags, param);
	case SPA_PARAM_Format:
		return port_set_format(this, direction, port_id, flags, param);
	default:
		return -ENOENT;
	}
}

static inline void queue_buffer(struct impl *this, struct port *port, uint32_t id)
{
	struct buffer *b = &port->buffers[id];

	spa_log_trace_fp(this->log, "%p: queue buffer %d on port %d %d",
			this, id, port->id, b->flags);
	if (SPA_FLAG_IS_SET(b->flags, BUFFER_FLAG_QUEUED))
		return;

	spa_list_append(&port->queue, &b->link);
	SPA_FLAG_SET(b->flags, BUFFER_FLAG_QUEUED);
}

static inline struct buffer *peek_buffer(struct impl *this, struct port *port)
{
	struct buffer *b;

	if (spa_list_is_empty(&port->queue))
		return NULL;

	b = spa_list_first(&port->queue, struct buffer, link);
	spa_log_trace_fp(this->log, "%p: peek buffer %d/%d on port %d %u",
			this, b->id, port->n_buffers, port->id, b->flags);
	return b;
}

static inline void dequeue_buffer(struct impl *this, struct port *port, struct buffer *b)
{
	spa_log_trace_fp(this->log, "%p: dequeue buffer %d on port %d %u",
			this, b->id, port->id, b->flags);
	if (!SPA_FLAG_IS_SET(b->flags, BUFFER_FLAG_QUEUED))
		return;
	spa_list_remove(&b->link);
	SPA_FLAG_CLEAR(b->flags, BUFFER_FLAG_QUEUED);
}

static int
impl_node_port_use_buffers(void *object,
			   enum spa_direction direction,
			   uint32_t port_id,
			   uint32_t flags,
			   struct spa_buffer **buffers,
			   uint32_t n_buffers)
{
	struct impl *this = object;
	struct port *port;
	uint32_t i, j, maxsize;

	spa_return_val_if_fail(this != NULL, -EINVAL);

	spa_return_val_if_fail(CHECK_PORT(this, direction, port_id), -EINVAL);

	port = GET_PORT(this, direction, port_id);

	spa_log_debug(this->log, "%p: use buffers %d on port %d:%d",
			this, n_buffers, direction, port_id);

	clear_buffers(this, port);

	if (n_buffers > 0 && !port->have_format)
		return -EIO;
	if (n_buffers > MAX_BUFFERS)
		return -ENOSPC;

	maxsize = this->quantum_limit * sizeof(float);

	for (i = 0; i < n_buffers; i++) {
		struct buffer *b;
		uint32_t n_datas = buffers[i]->n_datas;
		struct spa_data *d = buffers[i]->datas;

		b = &port->buffers[i];
		b->id = i;
		b->flags = 0;
		b->buf = buffers[i];

		if (n_datas != port->blocks) {
			spa_log_error(this->log, "%p: invalid blocks %d on buffer %d",
					this, n_datas, i);
			return -EINVAL;
		}

		for (j = 0; j < n_datas; j++) {
			if (d[j].data == NULL) {
				spa_log_error(this->log, "%p: invalid memory %d on buffer %d %d %p",
						this, j, i, d[j].type, d[j].data);
				return -EINVAL;
			}
			if (!SPA_IS_ALIGNED(d[j].data, this->max_align)) {
				spa_log_warn(this->log, "%p: memory %d on buffer %d not aligned",
						this, j, i);
			}
			if (direction == SPA_DIRECTION_OUTPUT &&
			    !SPA_FLAG_IS_SET(d[j].flags, SPA_DATA_FLAG_DYNAMIC))
				this->is_passthrough = false;

			b->datas[j] = d[j].data;

			maxsize = SPA_MAX(maxsize, d[j].maxsize);
		}
		if (direction == SPA_DIRECTION_OUTPUT)
			queue_buffer(this, port, i);
	}
	port->maxsize = maxsize;
	port->n_buffers = n_buffers;

	return 0;
}

struct io_data {
	struct port *port;
	void *data;
	size_t size;
};

static int do_set_port_io(struct spa_loop *loop, bool async, uint32_t seq,
		const void *data, size_t size, void *user_data)
{
	const struct io_data *d = user_data;
	d->port->io = d->data;
	return 0;
}

static int
impl_node_port_set_io(void *object,
		      enum spa_direction direction, uint32_t port_id,
		      uint32_t id, void *data, size_t size)
{
	struct impl *this = object;
	struct port *port;

	spa_return_val_if_fail(this != NULL, -EINVAL);

	spa_log_debug(this->log, "%p: set io %d on port %d:%d %p",
			this, id, direction, port_id, data);

	spa_return_val_if_fail(CHECK_PORT(this, direction, port_id), -EINVAL);

	port = GET_PORT(this, direction, port_id);

	switch (id) {
	case SPA_IO_Buffers:
		if (this->data_loop) {
			struct io_data d = { .port = port, .data = data, .size = size };
			spa_loop_invoke(this->data_loop, do_set_port_io, 0, NULL, 0, true, &d);
		}
		else
			port->io = data;
		break;
	case SPA_IO_RateMatch:
		this->io_rate_match = data;
		break;
	default:
		return -ENOENT;
	}
	return 0;
}

static int impl_node_port_reuse_buffer(void *object, uint32_t port_id, uint32_t buffer_id)
{
	struct impl *this = object;
	struct port *port;

	spa_return_val_if_fail(this != NULL, -EINVAL);
	spa_return_val_if_fail(CHECK_PORT(this, SPA_DIRECTION_OUTPUT, port_id), -EINVAL);

	port = GET_OUT_PORT(this, port_id);
	queue_buffer(this, port, buffer_id);

	return 0;
}

static int channelmix_process_apply_sequence(struct impl *this,
			const struct spa_pod_sequence *sequence, uint32_t *processed_offset,
			void *SPA_RESTRICT dst[], const void *SPA_RESTRICT src[],
			uint32_t n_samples)
{
	struct spa_pod_control *c, *prev = NULL;
	uint32_t avail_samples = n_samples;
	uint32_t i;
	const float *s[MAX_PORTS], **ss = (const float**) src;
	float *d[MAX_PORTS], **sd = (float **) dst;
	const struct spa_pod_sequence_body *body = &(sequence)->body;
	uint32_t size = SPA_POD_BODY_SIZE(sequence);
	bool end = false;

	c = spa_pod_control_first(body);
	while (true) {
		uint32_t chunk;

		if (c == NULL || !spa_pod_control_is_inside(body, size, c)) {
			c = NULL;
			end = true;
		}
		if (avail_samples == 0)
			break;

		/* ignore old control offsets */
		if (c != NULL) {
			if (c->offset <= *processed_offset) {
				prev = c;
				if (c != NULL)
					c = spa_pod_control_next(c);
				continue;
			}
			chunk = SPA_MIN(avail_samples, c->offset - *processed_offset);
			spa_log_trace_fp(this->log, "%p: process %d-%d %d/%d", this,
					*processed_offset, c->offset, chunk, avail_samples);
		} else {
			chunk = avail_samples;
			spa_log_trace_fp(this->log, "%p: process remain %d", this, chunk);
		}

		if (prev) {
			switch (prev->type) {
			case SPA_CONTROL_UMP:
				apply_midi(this, &prev->value);
				break;
			case SPA_CONTROL_Properties:
				apply_props(this, &prev->value);
				break;
			default:
				continue;
			}
		}
		if (ss == (const float**)src && chunk != avail_samples) {
			for (i = 0; i < this->mix.src_chan; i++)
				s[i] = ss[i];
			for (i = 0; i < this->mix.dst_chan; i++)
				d[i] = sd[i];
			ss = s;
			sd = d;
		}

		channelmix_process(&this->mix, (void**)sd, (const void**)ss, chunk);

		if (chunk != avail_samples) {
			for (i = 0; i < this->mix.src_chan; i++)
				ss[i] += chunk;
			for (i = 0; i < this->mix.dst_chan; i++)
				sd[i] += chunk;
		}
		avail_samples -= chunk;
		*processed_offset += chunk;
	}
	return end ? 1 : 0;
}

static inline uint32_t resample_get_in_size(struct impl *this, bool passthrough, uint32_t out_size)
{
	uint32_t match_size = passthrough ? out_size : resample_in_len(&this->resample, out_size);
	spa_log_trace_fp(this->log, "%p: current match %u", this, match_size);
	return match_size;
}

static uint64_t get_time_ns(struct impl *impl)
{
	struct timespec now;
	if (clock_gettime(CLOCK_MONOTONIC, &now) < 0)
		return 0;
	return SPA_TIMESPEC_TO_NSEC(&now);
}

static void run_wav_stage(struct stage *stage, struct stage_context *c)
{
	struct impl *impl = stage->impl;
	const void **src = (const void **)c->datas[stage->in_idx];

	if (SPA_UNLIKELY(impl->props.wav_path[0])) {
		if (impl->wav_file == NULL) {
			struct wav_file_info info;

			info.info = impl->dir[impl->direction].format;

			impl->wav_file = wav_file_open(impl->props.wav_path,
					"w", &info);
			if (impl->wav_file == NULL)
				spa_log_warn(impl->log, "can't open wav path: %m");
		}
		if (impl->wav_file) {
			wav_file_write(impl->wav_file, src, c->n_samples);
		} else {
			spa_zero(impl->props.wav_path);
		}
	} else if (impl->wav_file != NULL) {
		wav_file_close(impl->wav_file);
		impl->wav_file = NULL;
		impl->recalc = true;
	}
}

static void add_wav_stage(struct impl *impl, struct stage_context *ctx)
{
	struct stage *s = &impl->stages[impl->n_stages];
	s->impl = impl;
	s->passthrough = false;
	s->in_idx = ctx->src_idx;
	s->out_idx = ctx->src_idx;
	s->data = NULL;
	s->run = run_wav_stage;
	spa_log_trace(impl->log, "%p: stage %d", impl, impl->n_stages);
	impl->n_stages++;
}

static void run_dst_remap_stage(struct stage *s, struct stage_context *c)
{
	struct impl *impl = s->impl;
	struct dir *dir = &impl->dir[SPA_DIRECTION_OUTPUT];
	uint32_t i;
	for (i = 0; i < dir->conv.n_channels; i++) {
		c->datas[s->out_idx][i] = c->datas[s->in_idx][dir->remap[i]];
		spa_log_trace_fp(impl->log, "%p: output remap %d -> %d", impl, i, dir->remap[i]);
	}
}
static void add_dst_remap_stage(struct impl *impl, struct stage_context *ctx)
{
	struct stage *s = &impl->stages[impl->n_stages];
	s->impl = impl;
	s->passthrough = false;
	s->in_idx = ctx->dst_idx;
	s->out_idx = CTX_DATA_REMAP_DST;
	s->data = NULL;
	s->run = run_dst_remap_stage;
	spa_log_trace(impl->log, "%p: stage %d", impl, impl->n_stages);
	impl->n_stages++;
	ctx->dst_idx = CTX_DATA_REMAP_DST;
	ctx->final_idx = CTX_DATA_REMAP_DST;
}

static void run_src_remap_stage(struct stage *s, struct stage_context *c)
{
	struct impl *impl = s->impl;
	struct dir *dir = &impl->dir[SPA_DIRECTION_INPUT];
	uint32_t i;
	for (i = 0; i < dir->conv.n_channels; i++) {
		c->datas[s->out_idx][dir->remap[i]] = c->datas[s->in_idx][i];
		spa_log_trace_fp(impl->log, "%p: input remap %d -> %d", impl, dir->remap[i], i);
	}
}
static void add_src_remap_stage(struct impl *impl, struct stage_context *ctx)
{
	struct stage *s = &impl->stages[impl->n_stages];
	s->impl = impl;
	s->passthrough = false;
	s->in_idx = ctx->src_idx;
	s->out_idx = CTX_DATA_REMAP_SRC;
	s->data = NULL;
	s->run = run_src_remap_stage;
	spa_log_trace(impl->log, "%p: stage %d", impl, impl->n_stages);
	impl->n_stages++;
	ctx->src_idx = CTX_DATA_REMAP_SRC;
}

static void run_src_convert_stage(struct stage *s, struct stage_context *c)
{
	struct impl *impl = s->impl;
	struct dir *dir = &impl->dir[SPA_DIRECTION_INPUT];
	void *remap_src_datas[MAX_PORTS], **dst;

	spa_log_trace_fp(impl->log, "%p: input convert %d", impl, c->n_samples);
	if (dir->need_remap) {
		uint32_t i;
		for (i = 0; i < dir->conv.n_channels; i++) {
			remap_src_datas[i] = c->datas[s->out_idx][dir->remap[i]];
			spa_log_trace_fp(impl->log, "%p: input remap %d -> %d", impl, dir->remap[i], i);
		}
		dst = remap_src_datas;
	} else {
		dst = c->datas[s->out_idx];
	}
	convert_process(&dir->conv, dst, (const void**)c->datas[s->in_idx], c->n_samples);
}
static void add_src_convert_stage(struct impl *impl, struct stage_context *ctx)
{
	struct stage *s = &impl->stages[impl->n_stages];
	s->impl = impl;
	s->passthrough = false;
	s->in_idx = ctx->src_idx;
	s->out_idx = ctx->dst_idx;
	s->data = NULL;
	s->run = run_src_convert_stage;
	spa_log_trace(impl->log, "%p: stage %d", impl, impl->n_stages);
	impl->n_stages++;
	ctx->src_idx = ctx->dst_idx;
}

static void run_resample_stage(struct stage *s, struct stage_context *c)
{
	struct impl *impl = s->impl;
	uint32_t in_len = c->n_samples;
	uint32_t out_len = c->n_out;

	resample_process(&impl->resample, (const void**)c->datas[s->in_idx], &in_len,
			c->datas[s->out_idx], &out_len);

	spa_log_trace_fp(impl->log, "%p: resample %d/%d -> %d/%d", impl,
				c->n_samples, in_len, c->n_out, out_len);
	c->in_samples = in_len;
	c->n_samples = out_len;
}
static void add_resample_stage(struct impl *impl, struct stage_context *ctx)
{
	struct stage *s = &impl->stages[impl->n_stages];
	s->impl = impl;
	s->passthrough = false;
	s->in_idx = ctx->src_idx;
	s->out_idx = ctx->dst_idx;
	s->data = NULL;
	s->run = run_resample_stage;
	spa_log_trace(impl->log, "%p: stage %d", impl, impl->n_stages);
	impl->n_stages++;
	ctx->src_idx = ctx->dst_idx;
}

static void run_channelmix_stage(struct stage *s, struct stage_context *c)
{
	struct impl *impl = s->impl;
	void **out_datas = c->datas[s->out_idx];
	const void **in_datas = (const void**)c->datas[s->in_idx];
	struct port *ctrlport = c->ctrlport;

	spa_log_trace_fp(impl->log, "%p: channelmix %d", impl, c->n_samples);
	if (ctrlport != NULL && ctrlport->ctrl != NULL) {
		if (channelmix_process_apply_sequence(impl, ctrlport->ctrl,
					&ctrlport->ctrl_offset, out_datas, in_datas, c->n_samples) == 1) {
			ctrlport->io->status = SPA_STATUS_OK;
			ctrlport->ctrl = NULL;
		}
	} else if (impl->vol_ramp_sequence) {
		if (channelmix_process_apply_sequence(impl, impl->vol_ramp_sequence,
				&impl->vol_ramp_offset, out_datas, in_datas, c->n_samples) == 1) {
			free(impl->vol_ramp_sequence);
			impl->vol_ramp_sequence = NULL;
		}
	} else {
		channelmix_process(&impl->mix, out_datas, in_datas, c->n_samples);
	}
}

static void run_filter_stage(struct stage *s, struct stage_context *c)
{
	struct filter_graph *fg = s->data;

	spa_log_trace_fp(s->impl->log, "%p: filter-graph %d", s->impl, c->n_samples);
	spa_filter_graph_process(fg->graph, (const void **)c->datas[s->in_idx],
			c->datas[s->out_idx], c->n_samples);
}
static void add_filter_stage(struct impl *impl, uint32_t i, struct filter_graph *fg, struct stage_context *ctx)
{
	struct stage *s = &impl->stages[impl->n_stages];
	s->impl = impl;
	s->passthrough = false;
	s->in_idx = ctx->src_idx;
	s->out_idx = ctx->dst_idx;
	s->data = fg;
	s->run = run_filter_stage;
	spa_log_trace(impl->log, "%p: stage %d", impl, impl->n_stages);
	impl->n_stages++;
	ctx->src_idx = ctx->dst_idx;
}

static void add_channelmix_stage(struct impl *impl, struct stage_context *ctx)
{
	struct stage *s = &impl->stages[impl->n_stages];
	s->impl = impl;
	s->passthrough = false;
	s->in_idx = ctx->src_idx;
	s->out_idx = ctx->dst_idx;
	s->data = NULL;
	s->run = run_channelmix_stage;
	spa_log_trace(impl->log, "%p: stage %d", impl, impl->n_stages);
	impl->n_stages++;
	ctx->src_idx = ctx->dst_idx;
}

static void run_dst_convert_stage(struct stage *s, struct stage_context *c)
{
	struct impl *impl = s->impl;
	struct dir *dir = &impl->dir[SPA_DIRECTION_OUTPUT];
	void *remap_datas[MAX_PORTS], **src;

	spa_log_trace_fp(impl->log, "%p: output convert %d", impl, c->n_samples);
	if (dir->need_remap) {
		uint32_t i;
		for (i = 0; i < dir->conv.n_channels; i++) {
			remap_datas[dir->remap[i]] = c->datas[s->in_idx][i];
			spa_log_trace_fp(impl->log, "%p: output remap %d -> %d", impl, i, dir->remap[i]);
		}
		src = remap_datas;
	} else {
		src = c->datas[s->in_idx];
	}
	convert_process(&dir->conv, c->datas[s->out_idx], (const void **)src, c->n_samples);
}
static void add_dst_convert_stage(struct impl *impl, struct stage_context *ctx)
{
	struct stage *s = &impl->stages[impl->n_stages];
	s->impl = impl;
	s->passthrough = false;
	s->in_idx = ctx->src_idx;
	s->out_idx = ctx->final_idx;
	s->data = NULL;
	s->run = run_dst_convert_stage;
	spa_log_trace(impl->log, "%p: stage %d", impl, impl->n_stages);
	impl->n_stages++;
	ctx->src_idx = s->out_idx;
}

static void recalc_stages(struct impl *this, struct stage_context *ctx)
{
	struct dir *dir;
	bool filter_passthrough, in_passthrough, mix_passthrough, resample_passthrough, out_passthrough;
	int tmp = 0;
	struct port *ctrlport = ctx->ctrlport;
	bool in_need_remap, out_need_remap;
	uint32_t i;

	this->recalc = false;
	this->n_stages = 0;

	dir = &this->dir[SPA_DIRECTION_INPUT];
	in_passthrough = dir->conv.is_passthrough;
	in_need_remap = dir->need_remap;

	dir = &this->dir[SPA_DIRECTION_OUTPUT];
	out_passthrough = dir->conv.is_passthrough;
	out_need_remap = dir->need_remap;

	resample_passthrough = resample_is_passthrough(this);
	filter_passthrough = this->n_graph == 0;
	this->resample_passthrough = resample_passthrough;
	mix_passthrough = SPA_FLAG_IS_SET(this->mix.flags, CHANNELMIX_FLAG_IDENTITY) &&
		(ctrlport == NULL || ctrlport->ctrl == NULL) && (this->vol_ramp_sequence == NULL);

	if (in_passthrough && filter_passthrough && mix_passthrough && resample_passthrough)
		out_passthrough = false;

	if (out_passthrough && out_need_remap)
		add_dst_remap_stage(this, ctx);

	if (this->direction == SPA_DIRECTION_INPUT &&
	    (this->props.wav_path[0] || this->wav_file != NULL))
		add_wav_stage(this, ctx);

	if (!in_passthrough) {
		if (filter_passthrough && mix_passthrough && resample_passthrough && out_passthrough)
			ctx->dst_idx = ctx->final_idx;
		else
			ctx->dst_idx = CTX_DATA_TMP_0 + ((tmp++) & 1);

		add_src_convert_stage(this, ctx);
	} else {
		if (in_need_remap)
			add_src_remap_stage(this, ctx);
	}

	if (this->direction == SPA_DIRECTION_INPUT) {
		if (!resample_passthrough) {
			if (filter_passthrough && mix_passthrough && out_passthrough)
				ctx->dst_idx = ctx->final_idx;
			else
				ctx->dst_idx = CTX_DATA_TMP_0 + ((tmp++) & 1);

			add_resample_stage(this, ctx);
			resample_passthrough = true;
		}
	}
	if (!filter_passthrough) {
		for (i = 0; i < this->n_graph; i++) {
			struct filter_graph *fg = &this->filter_graph[this->graph_index[i]];

			if (mix_passthrough && resample_passthrough && out_passthrough &&
			    i + 1 == this->n_graph)
				ctx->dst_idx = ctx->final_idx;
			else
				ctx->dst_idx = CTX_DATA_TMP_0 + ((tmp++) & 1);

			add_filter_stage(this, i, fg, ctx);
		}
	}
	if (!mix_passthrough) {
		if (resample_passthrough && out_passthrough)
			ctx->dst_idx = ctx->final_idx;
		else
			ctx->dst_idx = CTX_DATA_TMP_0 + ((tmp++) & 1);

		add_channelmix_stage(this, ctx);
	}
	if (this->direction == SPA_DIRECTION_OUTPUT) {
		if (!resample_passthrough) {
			if (out_passthrough)
				ctx->dst_idx = ctx->final_idx;
			else
				ctx->dst_idx = CTX_DATA_TMP_0 + ((tmp++) & 1);

			add_resample_stage(this, ctx);
		}
	}
	if (!out_passthrough) {
		add_dst_convert_stage(this, ctx);
	}
	if (this->direction == SPA_DIRECTION_OUTPUT &&
	    (this->props.wav_path[0] || this->wav_file != NULL))
		add_wav_stage(this, ctx);

	spa_log_trace(this->log, "got %u processing stages", this->n_stages);
}

static int impl_node_process(void *object)
{
	struct impl *this = object;
	const void *src_datas[MAX_PORTS];
	void *dst_datas[MAX_PORTS], *remap_src_datas[MAX_PORTS], *remap_dst_datas[MAX_PORTS];
	uint32_t i, j, n_src_datas = 0, n_dst_datas = 0, n_mon_datas = 0, remap;
	uint32_t n_samples, max_in, n_out, max_out, quant_samples;
	struct port *port, *ctrlport = NULL;
	struct buffer *buf, *out_bufs[MAX_PORTS];
	struct spa_data *bd;
	struct dir *dir;
	int res = 0, suppressed;
	bool in_avail = false, flush_in = false, flush_out = false;
	bool draining = false, in_empty = this->out_offset == 0;
	struct spa_io_buffers *io;
	const struct spa_pod_sequence *ctrl = NULL;
	uint64_t current_time;
	struct stage_context ctx;

	/* calculate quantum scale, this is how many samples we need to produce or
	 * consume. Also update the rate scale, this is sent to the resampler to adjust
	 * the rate, either when the graph clock changed or when the user adjusted the
	 * rate.  */
	if (SPA_LIKELY(this->io_position)) {
		double r =  this->rate_scale;

		current_time = this->io_position->clock.nsec;
		quant_samples = this->io_position->clock.duration;
		if (this->direction == SPA_DIRECTION_INPUT) {
			if (this->io_position->clock.rate.denom != this->resample.o_rate)
				r = (double) this->io_position->clock.rate.denom / this->resample.o_rate;
			else
				r = 1.0;
		} else {
			if (this->io_position->clock.rate.denom != this->resample.i_rate)
				r = (double) this->resample.i_rate / this->io_position->clock.rate.denom;
			else
				r = 1.0;
		}
		if (this->rate_scale != r) {
			spa_log_info(this->log, "scale graph:%u in:%u out:%u scale:%f->%f",
					this->io_position->clock.rate.denom,
					this->resample.i_rate, this->resample.o_rate,
					this->rate_scale, r);
			this->rate_scale = r;
		}
	}
	else {
		current_time = get_time_ns(this);
		quant_samples = this->quantum_limit;
	}

	dir = &this->dir[SPA_DIRECTION_INPUT];
	max_in = UINT32_MAX;

	/* collect input port data */
	for (i = 0; i < dir->n_ports; i++) {
		port = GET_IN_PORT(this, i);

		if (SPA_UNLIKELY((io = port->io) == NULL)) {
			spa_log_trace_fp(this->log, "%p: no io on input port %d",
					this, port->id);
			buf = NULL;
		} else if (SPA_UNLIKELY(io->status != SPA_STATUS_HAVE_DATA)) {
			if (io->status & SPA_STATUS_DRAINED) {
				spa_log_debug(this->log, "%p: port %d drained", this, port->id);
				in_avail = flush_in = draining = true;
			} else {
				spa_log_trace_fp(this->log, "%p: empty input port %d %p %d %d %d",
						this, port->id, io, io->status, io->buffer_id,
						port->n_buffers);
				this->drained = false;
			}
			buf = NULL;
		} else if (SPA_UNLIKELY(io->buffer_id >= port->n_buffers)) {
			spa_log_trace_fp(this->log, "%p: invalid input buffer port %d %p %d %d %d",
					this, port->id, io, io->status, io->buffer_id,
					port->n_buffers);
			io->status = -EINVAL;
			buf = NULL;
		} else {
			spa_log_trace_fp(this->log, "%p: input buffer port %d io:%p status:%d id:%d n:%d",
					this, port->id, io, io->status, io->buffer_id,
					port->n_buffers);
			buf = &port->buffers[io->buffer_id];
		}

		if (SPA_UNLIKELY(buf == NULL)) {
			for (j = 0; j < port->blocks; j++) {
				if (port->is_control) {
					spa_log_trace_fp(this->log, "%p: empty control %d", this,
							i * port->blocks + j);
				} else {
					remap = n_src_datas++;
					src_datas[remap] = SPA_PTR_ALIGN(this->empty, MAX_ALIGN, void);
					spa_log_trace_fp(this->log, "%p: empty input %d->%d", this,
							i * port->blocks + j, remap);
					max_in = SPA_MIN(max_in, this->scratch_size / port->stride);
				}
			}
		} else {
			in_avail = true;
			for (j = 0; j < port->blocks; j++) {
				uint32_t offs, size;

				bd = &buf->buf->datas[j];

				offs = SPA_MIN(bd->chunk->offset, bd->maxsize);
				size = SPA_MIN(bd->maxsize - offs, bd->chunk->size);
				if (!SPA_FLAG_IS_SET(bd->chunk->flags, SPA_CHUNK_FLAG_EMPTY))
					in_empty = false;

				if (SPA_UNLIKELY(port->is_control)) {
					spa_log_trace_fp(this->log, "%p: control %d", this,
							i * port->blocks + j);
					ctrlport = port;
					ctrl = spa_pod_from_data(bd->data, bd->maxsize,
							bd->chunk->offset, bd->chunk->size);
					if (ctrl && !spa_pod_is_sequence(&ctrl->pod))
						ctrl = NULL;
					if (ctrl != ctrlport->ctrl) {
						ctrlport->ctrl = ctrl;
						ctrlport->ctrl_offset = 0;
						this->recalc = true;
					}
				} else  {
					max_in = SPA_MIN(max_in, size / port->stride);

					remap = n_src_datas++;
					offs += this->in_offset * port->stride;
					src_datas[remap] = SPA_PTROFF(bd->data, offs, void);

					spa_log_trace_fp(this->log, "%p: input %d:%d:%d %d %d %d->%d", this,
							offs, size, port->stride, this->in_offset, max_in,
							i * port->blocks + j, remap);
				}
			}
		}
	}
	bool resample_passthrough = resample_is_passthrough(this);
	if (this->resample_passthrough != resample_passthrough)
		this->recalc = true;

	/* calculate how many samples we are going to produce. */
	if (this->direction == SPA_DIRECTION_INPUT) {
		/* in split mode we need to output exactly the size of the
		 * duration so we don't try to flush early */
		max_out = quant_samples;
		if (!in_avail || this->drained) {
			n_out = max_out - SPA_MIN(max_out, this->out_offset);
			/* no input, ask for more, update rate-match first */
			resample_update_rate_match(this, resample_passthrough, n_out, 0);
			spa_log_trace_fp(this->log, "%p: no input drained:%d", this, this->drained);
			res |= this->drained ? SPA_STATUS_DRAINED : SPA_STATUS_NEED_DATA;
			return res;
		}
		flush_out = false;
	} else {
		/* in merge mode we consume one duration of samples and
		 * always output the resulting data */
		max_out = this->quantum_limit;
		flush_out = true;
	}

	dir = &this->dir[SPA_DIRECTION_OUTPUT];
	/* collect output ports and monitor ports data */
	for (i = 0; i < dir->n_ports; i++) {
		port = GET_OUT_PORT(this, i);

		if (SPA_UNLIKELY((io = port->io) == NULL ||
		    io->status == SPA_STATUS_HAVE_DATA)) {
			buf = NULL;
		}
		else {
			if (SPA_LIKELY(io->buffer_id < port->n_buffers))
				queue_buffer(this, port, io->buffer_id);

			buf = peek_buffer(this, port);
			if (buf == NULL && port->n_buffers > 0 &&
			    (suppressed = spa_ratelimit_test(&this->rate_limit, current_time)) >= 0) {
				spa_log_warn(this->log, "%p: (%d suppressed) out of buffers on port %d %d",
					this, suppressed, port->id, port->n_buffers);
			}
		}
		out_bufs[i] = buf;

		if (SPA_UNLIKELY(buf == NULL)) {
			for (j = 0; j < port->blocks; j++) {
				if (port->is_monitor) {
					remap = n_mon_datas++;
					spa_log_trace_fp(this->log, "%p: empty monitor %d", this,
						remap);
				} else if (port->is_control) {
					spa_log_trace_fp(this->log, "%p: empty control %d", this, j);
				} else {
					remap = n_dst_datas++;
					dst_datas[remap] = SPA_PTR_ALIGN(this->scratch, MAX_ALIGN, void);
					spa_log_trace_fp(this->log, "%p: empty output %d->%d", this,
						i * port->blocks + j, remap);
					max_out = SPA_MIN(max_out, this->scratch_size / port->stride);
				}
			}
		} else {
			for (j = 0; j < port->blocks; j++) {
				bd = &buf->buf->datas[j];

				bd->chunk->offset = 0;
				bd->chunk->size = 0;
				if (port->is_monitor) {
					float volume;
					uint32_t mon_max;

					remap = n_mon_datas++;
					volume = this->props.monitor.mute ?
						0.0f : this->props.monitor.volumes[remap];
					if (this->monitor_channel_volumes)
						volume *= this->props.channel.mute ? 0.0f :
							this->props.channel.volumes[remap];

					volume = SPA_CLAMPF(volume, this->props.min_volume,
							this->props.max_volume);

					mon_max = SPA_MIN(bd->maxsize / port->stride, max_in);

					volume_process(&this->volume, bd->data, src_datas[remap],
							volume, mon_max);

					bd->chunk->size = mon_max * port->stride;
					bd->chunk->stride = port->stride;

					spa_log_trace_fp(this->log, "%p: monitor %d %d", this,
							remap, mon_max);

					dequeue_buffer(this, port, buf);
					io->status = SPA_STATUS_HAVE_DATA;
					io->buffer_id = buf->id;
					res |= SPA_STATUS_HAVE_DATA;
				} else if (SPA_UNLIKELY(port->is_control)) {
					spa_log_trace_fp(this->log, "%p: control %d", this, j);
				} else {
					remap = n_dst_datas++;
					dst_datas[remap] = SPA_PTROFF(bd->data,
							this->out_offset * port->stride, void);
					max_out = SPA_MIN(max_out, bd->maxsize / port->stride);

					spa_log_trace_fp(this->log, "%p: output %d offs:%d %d->%d", this,
							max_out, this->out_offset,
							i * port->blocks + j, remap);
				}
			}
		}
	}


	/* calculate how many samples at most we are going to consume. If we're
	 * draining, we consume as much as we can. Otherwise we consume what is
	 * left. */
	if (SPA_UNLIKELY(draining))
		n_samples = SPA_MIN(max_in, this->quantum_limit);
	else {
		n_samples = max_in - SPA_MIN(max_in, this->in_offset);
	}
	/* we only need to output the remaining samples */
	n_out = max_out - SPA_MIN(max_out, this->out_offset);

	/* calculate how many samples we are going to consume. */
	if (this->direction == SPA_DIRECTION_INPUT) {
		/* figure out how much input samples we need to consume */
		n_samples = SPA_MIN(n_samples,
				resample_get_in_size(this, resample_passthrough, n_out));
	} else {
		/* in merge mode we consume one duration of samples */
		n_samples = SPA_MIN(n_samples, quant_samples);
		flush_in = true;
	}

	ctx.datas[CTX_DATA_SRC] = (void **)src_datas;
	ctx.datas[CTX_DATA_DST] = dst_datas;
	ctx.datas[CTX_DATA_REMAP_DST] = remap_dst_datas;
	ctx.datas[CTX_DATA_REMAP_SRC] = remap_src_datas;
	ctx.datas[CTX_DATA_TMP_0] = (void**)this->tmp_datas[0];
	ctx.datas[CTX_DATA_TMP_1] = (void**)this->tmp_datas[1];
	ctx.in_samples = n_samples;
	ctx.n_samples = n_samples;
	ctx.n_out = n_out;
	ctx.ctrlport = ctrlport;

	if (SPA_UNLIKELY(this->recalc)) {
		ctx.src_idx = CTX_DATA_SRC;
		ctx.dst_idx = CTX_DATA_DST;
		ctx.final_idx = CTX_DATA_DST;
		recalc_stages(this, &ctx);
	}

	for (i = 0; i < this->n_stages; i++) {
		struct stage *s = &this->stages[i];
		s->run(s, &ctx);
	}
	this->in_offset += ctx.in_samples;
	this->out_offset += ctx.n_samples;

	spa_log_trace_fp(this->log, "%d/%d  %d/%d %d->%d", this->in_offset, max_in,
			this->out_offset, max_out, n_samples, n_out);

	dir = &this->dir[SPA_DIRECTION_INPUT];
	if (SPA_LIKELY(this->in_offset >= max_in || flush_in)) {
		/* return input buffers */
		for (i = 0; i < dir->n_ports; i++) {
			port = GET_IN_PORT(this, i);
			if (port->is_control)
				continue;
			if (SPA_UNLIKELY((io = port->io) == NULL))
				continue;
			spa_log_trace_fp(this->log, "return: input %d %d", port->id, io->buffer_id);
			if (!draining)
				io->status = SPA_STATUS_NEED_DATA;
		}
		this->in_offset = 0;
		max_in = 0;
		res |= SPA_STATUS_NEED_DATA;
	}

	dir = &this->dir[SPA_DIRECTION_OUTPUT];
	if (SPA_LIKELY(this->out_offset > 0 && (this->out_offset >= max_out || flush_out))) {
		/* queue output buffers */
		for (i = 0; i < dir->n_ports; i++) {
			port = GET_OUT_PORT(this, i);
			if (SPA_UNLIKELY(port->is_monitor || port->is_control))
				continue;
			if (SPA_UNLIKELY((io = port->io) == NULL))
				continue;

			if (SPA_UNLIKELY((buf = out_bufs[i]) == NULL))
				continue;

			dequeue_buffer(this, port, buf);

			for (j = 0; j < port->blocks; j++) {
				bd = &buf->buf->datas[j];
				bd->chunk->size = this->out_offset * port->stride;
				bd->chunk->stride = port->stride;
				SPA_FLAG_UPDATE(bd->chunk->flags, SPA_CHUNK_FLAG_EMPTY, in_empty);
				spa_log_trace_fp(this->log, "out: offs:%d stride:%d size:%d",
						this->out_offset, port->stride, bd->chunk->size);
			}
			io->status = SPA_STATUS_HAVE_DATA;
			io->buffer_id = buf->id;
		}
		res |= SPA_STATUS_HAVE_DATA;
		this->drained = draining;
		this->out_offset = 0;
	}
	else if (n_samples == 0 && this->resample_peaks) {
		for (i = 0; i < dir->n_ports; i++) {
			port = GET_OUT_PORT(this, i);
			if (port->is_monitor || port->is_control)
				continue;
			if (SPA_UNLIKELY((io = port->io) == NULL))
				continue;

			io->status = SPA_STATUS_HAVE_DATA;
			io->buffer_id = SPA_ID_INVALID;
			res |= SPA_STATUS_HAVE_DATA;
			spa_log_trace_fp(this->log, "%p: no output buffer", this);
		}
	}
	{
		uint32_t size, queued;

		if (this->direction == SPA_DIRECTION_INPUT) {
			size = max_out - this->out_offset;
			queued = max_in - this->in_offset;
		} else {
			size = quant_samples;
			queued = 0;
		}
		if (resample_update_rate_match(this, resample_passthrough,
					size, queued) > 0)
			res |= SPA_STATUS_NEED_DATA;
	}

	return res;
}

static const struct spa_node_methods impl_node = {
	SPA_VERSION_NODE_METHODS,
	.add_listener = impl_node_add_listener,
	.set_callbacks = impl_node_set_callbacks,
	.enum_params = impl_node_enum_params,
	.set_param = impl_node_set_param,
	.set_io = impl_node_set_io,
	.send_command = impl_node_send_command,
	.add_port = impl_node_add_port,
	.remove_port = impl_node_remove_port,
	.port_enum_params = impl_node_port_enum_params,
	.port_set_param = impl_node_port_set_param,
	.port_use_buffers = impl_node_port_use_buffers,
	.port_set_io = impl_node_port_set_io,
	.port_reuse_buffer = impl_node_port_reuse_buffer,
	.process = impl_node_process,
};

static int impl_get_interface(struct spa_handle *handle, const char *type, void **interface)
{
	struct impl *this;

	spa_return_val_if_fail(handle != NULL, -EINVAL);
	spa_return_val_if_fail(interface != NULL, -EINVAL);

	this = (struct impl *) handle;

	if (spa_streq(type, SPA_TYPE_INTERFACE_Node))
		*interface = &this->node;
	else
		return -ENOENT;

	return 0;
}

static void free_dir(struct dir *dir)
{
	uint32_t i;
	for (i = 0; i < MAX_PORTS; i++)
		free(dir->ports[i]);
	if (dir->conv.free)
		convert_free(&dir->conv);
	free(dir->tag);
}

static int impl_clear(struct spa_handle *handle)
{
	struct impl *this;

	spa_return_val_if_fail(handle != NULL, -EINVAL);

	this = (struct impl *) handle;

	free_dir(&this->dir[SPA_DIRECTION_INPUT]);
	free_dir(&this->dir[SPA_DIRECTION_OUTPUT]);

	free_tmp(this);

	clean_filter_handles(this, true);

	if (this->resample.free)
		resample_free(&this->resample);
	if (this->wav_file != NULL)
		wav_file_close(this->wav_file);
	free (this->vol_ramp_sequence);
	return 0;
}

static size_t
impl_get_size(const struct spa_handle_factory *factory,
	      const struct spa_dict *params)
{
	return sizeof(struct impl);
}

static int
impl_init(const struct spa_handle_factory *factory,
	  struct spa_handle *handle,
	  const struct spa_dict *info,
	  const struct spa_support *support,
	  uint32_t n_support)
{
	struct impl *this;
	uint32_t i;
	const char *str;
	bool filter_graph_disabled;

	spa_return_val_if_fail(factory != NULL, -EINVAL);
	spa_return_val_if_fail(handle != NULL, -EINVAL);

	handle->get_interface = impl_get_interface;
	handle->clear = impl_clear;

	this = (struct impl *) handle;

	this->data_loop = spa_support_find(support, n_support, SPA_TYPE_INTERFACE_DataLoop);
	this->log = spa_support_find(support, n_support, SPA_TYPE_INTERFACE_Log);
	spa_log_topic_init(this->log, &log_topic);

	this->cpu = spa_support_find(support, n_support, SPA_TYPE_INTERFACE_CPU);
	if (this->cpu) {
		this->cpu_flags = spa_cpu_get_flags(this->cpu);
		this->max_align = SPA_MIN(MAX_ALIGN, spa_cpu_get_max_align(this->cpu));
	}
	this->loader = spa_support_find(support, n_support, SPA_TYPE_INTERFACE_PluginLoader);

	props_reset(&this->props);
	filter_graph_disabled = this->props.filter_graph_disabled;

	this->rate_limit.interval = 2 * SPA_NSEC_PER_SEC;
	this->rate_limit.burst = 1;

	this->mix.options = CHANNELMIX_OPTION_UPMIX | CHANNELMIX_OPTION_MIX_LFE;
	this->mix.upmix = CHANNELMIX_UPMIX_NONE;
	this->mix.log = this->log;
	this->mix.lfe_cutoff = 0.0f;
	this->mix.fc_cutoff = 0.0f;
	this->mix.rear_delay = 0.0f;
	this->mix.widen = 0.0f;

	if (info && (str = spa_dict_lookup(info, "clock.quantum-limit")) != NULL)
		spa_atou32(str, &this->quantum_limit, 0);

	for (i = 0; info && i < info->n_items; i++) {
		const char *k = info->items[i].key;
		const char *s = info->items[i].value;
		if (spa_streq(k, "resample.peaks"))
			this->resample_peaks = spa_atob(s);
		else if (spa_streq(k, "resample.prefill"))
			SPA_FLAG_UPDATE(this->resample.options,
				RESAMPLE_OPTION_PREFILL, spa_atob(s));
		else if (spa_streq(k, "convert.direction")) {
			if (spa_streq(s, "output"))
				this->direction = SPA_DIRECTION_OUTPUT;
			else
				this->direction = SPA_DIRECTION_INPUT;
		}
		else if (spa_streq(k, SPA_KEY_AUDIO_POSITION)) {
			if (s != NULL)
				spa_audio_parse_position(s, strlen(s), this->props.channel_map,
						&this->props.n_channels);
		}
		else if (spa_streq(k, SPA_KEY_PORT_IGNORE_LATENCY))
			this->port_ignore_latency = spa_atob(s);
		else if (spa_streq(k, SPA_KEY_PORT_GROUP))
			spa_scnprintf(this->group_name, sizeof(this->group_name), "%s", s);
		else if (spa_streq(k, "monitor.passthrough"))
			this->monitor_passthrough = spa_atob(s);
		else if (spa_streq(k, "audioconvert.filter-graph.disable"))
			filter_graph_disabled = spa_atob(s);
		else
			audioconvert_set_param(this, k, s);
	}
	this->props.filter_graph_disabled = filter_graph_disabled;
	this->props.channel.n_volumes = this->props.n_channels;
	this->props.soft.n_volumes = this->props.n_channels;
	this->props.monitor.n_volumes = this->props.n_channels;

	this->dir[SPA_DIRECTION_INPUT].direction = SPA_DIRECTION_INPUT;
	this->dir[SPA_DIRECTION_OUTPUT].direction = SPA_DIRECTION_OUTPUT;

	this->node.iface = SPA_INTERFACE_INIT(
			SPA_TYPE_INTERFACE_Node,
			SPA_VERSION_NODE,
			&impl_node, this);
	spa_hook_list_init(&this->hooks);

	this->info_all = SPA_NODE_CHANGE_MASK_FLAGS |
			SPA_NODE_CHANGE_MASK_PARAMS;
	this->info = SPA_NODE_INFO_INIT();
	this->info.max_input_ports = MAX_PORTS;
	this->info.max_output_ports = MAX_PORTS;
	this->info.flags = SPA_NODE_FLAG_RT |
		SPA_NODE_FLAG_IN_PORT_CONFIG |
		SPA_NODE_FLAG_OUT_PORT_CONFIG |
		SPA_NODE_FLAG_NEED_CONFIGURE;
	this->params[IDX_EnumPortConfig] = SPA_PARAM_INFO(SPA_PARAM_EnumPortConfig, SPA_PARAM_INFO_READ);
	this->params[IDX_PortConfig] = SPA_PARAM_INFO(SPA_PARAM_PortConfig, SPA_PARAM_INFO_READWRITE);
	this->params[IDX_PropInfo] = SPA_PARAM_INFO(SPA_PARAM_PropInfo, SPA_PARAM_INFO_READ);
	this->params[IDX_Props] = SPA_PARAM_INFO(SPA_PARAM_Props, SPA_PARAM_INFO_READWRITE);
	this->info.params = this->params;
	this->info.n_params = N_NODE_PARAMS;

	this->volume.cpu_flags = this->cpu_flags;
	volume_init(&this->volume);

	this->rate_scale = 1.0;

	reconfigure_mode(this, SPA_PARAM_PORT_CONFIG_MODE_convert, SPA_DIRECTION_INPUT, false, false, NULL);
	reconfigure_mode(this, SPA_PARAM_PORT_CONFIG_MODE_convert, SPA_DIRECTION_OUTPUT, false, false, NULL);

	return 0;
}

static const struct spa_interface_info impl_interfaces[] = {
	{SPA_TYPE_INTERFACE_Node,},
};

static int
impl_enum_interface_info(const struct spa_handle_factory *factory,
			 const struct spa_interface_info **info,
			 uint32_t *index)
{
	spa_return_val_if_fail(factory != NULL, -EINVAL);
	spa_return_val_if_fail(info != NULL, -EINVAL);
	spa_return_val_if_fail(index != NULL, -EINVAL);

	switch (*index) {
	case 0:
		*info = &impl_interfaces[*index];
		break;
	default:
		return 0;
	}
	(*index)++;
	return 1;
}

const struct spa_handle_factory spa_audioconvert_factory = {
	SPA_VERSION_HANDLE_FACTORY,
	SPA_NAME_AUDIO_CONVERT,
	NULL,
	impl_get_size,
	impl_init,
	impl_enum_interface_info,
};
