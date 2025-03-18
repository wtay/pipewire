/* PipeWire */
/* SPDX-FileCopyrightText: Copyright © 2021 Wim Taymans */
/* SPDX-License-Identifier: MIT */

#include <string.h>
#include <stdio.h>
#include <errno.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <dlfcn.h>
#include <unistd.h>
#include <limits.h>
#include <math.h>

#include "config.h"

#include <spa/utils/result.h>
#include <spa/utils/string.h>
#include <spa/utils/json.h>
#include <spa/support/cpu.h>
#include <spa/support/plugin-loader.h>
#include <spa/param/latency-utils.h>
#include <spa/param/tag-utils.h>
#include <spa/param/audio/raw.h>
#include <spa/param/audio/raw-json.h>
#include <spa/param/audio/format-utils.h>
#include <spa/pod/dynamic.h>
#include <spa/pod/builder.h>
#include <spa/debug/types.h>
#include <spa/debug/log.h>
#include <spa/filter-graph/filter-graph.h>

#include "audio-plugin.h"
#include "audio-dsp-impl.h"

#undef SPA_LOG_TOPIC_DEFAULT
#define SPA_LOG_TOPIC_DEFAULT &log_topic
SPA_LOG_TOPIC_DEFINE_STATIC(log_topic, "spa.filter-graph");

#define MAX_HNDL 64

#define DEFAULT_RATE	48000

#define spa_filter_graph_emit(hooks,method,version,...)					\
		spa_hook_list_call_simple(hooks, struct spa_filter_graph_events,	\
				method, version, ##__VA_ARGS__)

#define spa_filter_graph_emit_info(hooks,...)		spa_filter_graph_emit(hooks,info, 0, __VA_ARGS__)
#define spa_filter_graph_emit_apply_props(hooks,...)	spa_filter_graph_emit(hooks,apply_props, 0, __VA_ARGS__)
#define spa_filter_graph_emit_props_changed(hooks,...)	spa_filter_graph_emit(hooks,props_changed, 0, __VA_ARGS__)

struct plugin {
	struct spa_list link;
	struct impl *impl;

	int ref;
	char type[256];
	char path[PATH_MAX];

	struct spa_handle *hndl;
	struct spa_fga_plugin *plugin;
	struct spa_list descriptor_list;
};

struct descriptor {
	struct spa_list link;
	int ref;
	struct plugin *plugin;
	char label[256];

	const struct spa_fga_descriptor *desc;

	uint32_t n_input;
	uint32_t n_output;
	uint32_t n_control;
	uint32_t n_notify;
	unsigned long *input;
	unsigned long *output;
	unsigned long *control;
	unsigned long *notify;
	float *default_control;
};

struct port {
	struct spa_list link;
	struct node *node;

	uint32_t idx;
	unsigned long p;

	struct spa_list link_list;
	uint32_t n_links;
	uint32_t external;

	float control_data[MAX_HNDL];
	float *audio_data[MAX_HNDL];
	void *audio_mem[MAX_HNDL];
};

struct node {
	struct spa_list link;
	struct graph *graph;

	struct descriptor *desc;

	char name[256];
	char *config;

	struct port *input_port;
	struct port *output_port;
	struct port *control_port;
	struct port *notify_port;

	uint32_t n_hndl;
	void *hndl[MAX_HNDL];

	unsigned int n_deps;
	unsigned int visited:1;
	unsigned int disabled:1;
	unsigned int control_changed:1;
};

struct link {
	struct spa_list link;

	struct spa_list input_link;
	struct spa_list output_link;

	struct port *output;
	struct port *input;
};

struct graph_port {
	const struct spa_fga_descriptor *desc;
	void **hndl;
	uint32_t port;
	unsigned next:1;
};

struct graph_hndl {
	const struct spa_fga_descriptor *desc;
	void **hndl;
};

struct volume {
	bool mute;
	uint32_t n_volumes;
	float volumes[SPA_AUDIO_MAX_CHANNELS];

	uint32_t n_ports;
	struct port *ports[SPA_AUDIO_MAX_CHANNELS];
	float min[SPA_AUDIO_MAX_CHANNELS];
	float max[SPA_AUDIO_MAX_CHANNELS];
#define SCALE_LINEAR	0
#define SCALE_CUBIC	1
	int scale[SPA_AUDIO_MAX_CHANNELS];
};

struct graph {
	struct impl *impl;

	struct spa_list node_list;
	struct spa_list link_list;

	uint32_t n_input;
	struct graph_port *input;

	uint32_t n_output;
	struct graph_port *output;

	uint32_t n_hndl;
	struct graph_hndl *hndl;

	uint32_t n_control;
	struct port **control_port;

	uint32_t n_input_names;
	char **input_names;

	uint32_t n_output_names;
	char **output_names;

	struct volume volume[2];

	uint32_t n_inputs;
	uint32_t n_outputs;
	uint32_t inputs_position[SPA_AUDIO_MAX_CHANNELS];
	uint32_t n_inputs_position;
	uint32_t outputs_position[SPA_AUDIO_MAX_CHANNELS];
	uint32_t n_outputs_position;

	unsigned activated:1;
	unsigned setup:1;
};

struct impl {
	struct spa_handle handle;
	struct spa_filter_graph filter_graph;
	struct spa_hook_list hooks;

	struct spa_log *log;
	struct spa_cpu *cpu;
	struct spa_fga_dsp *dsp;
	struct spa_plugin_loader *loader;

	uint64_t info_all;
	struct spa_filter_graph_info info;

	struct graph graph;

	uint32_t quantum_limit;
	uint32_t max_align;
	long unsigned rate;

	struct spa_list plugin_list;

	float *silence_data;
	float *discard_data;
};

static inline void print_channels(char *buffer, size_t max_size, uint32_t n_channels, uint32_t *positions)
{
	uint32_t i;
	struct spa_strbuf buf;

	spa_strbuf_init(&buf, buffer, max_size);
	spa_strbuf_append(&buf, "[");
	for (i = 0; i < n_channels; i++) {
		spa_strbuf_append(&buf, "%s%s", i ? "," : "",
			spa_type_audio_channel_to_short_name(positions[i]));
	}
	spa_strbuf_append(&buf, "]");
}

static void emit_filter_graph_info(struct impl *impl, bool full)
{
	uint64_t old = full ? impl->info.change_mask : 0;
	struct graph *graph = &impl->graph;

	if (full)
		impl->info.change_mask = impl->info_all;
	if (impl->info.change_mask || full) {
		char n_inputs[64], n_outputs[64];
		struct spa_dict_item items[6];
		struct spa_dict dict = SPA_DICT(items, 0);
		char in_pos[SPA_AUDIO_MAX_CHANNELS * 8];
		char out_pos[SPA_AUDIO_MAX_CHANNELS * 8];

		snprintf(n_inputs, sizeof(n_inputs), "%d", impl->graph.n_inputs);
		snprintf(n_outputs, sizeof(n_outputs), "%d", impl->graph.n_outputs);

		items[dict.n_items++] = SPA_DICT_ITEM("n_inputs", n_inputs);
		items[dict.n_items++] = SPA_DICT_ITEM("n_outputs", n_outputs);
		if (graph->n_inputs_position) {
			print_channels(in_pos, sizeof(in_pos),
					graph->n_inputs_position, graph->inputs_position);
			items[dict.n_items++] = SPA_DICT_ITEM("inputs.audio.position", in_pos);
		}
		if (graph->n_outputs_position) {
			print_channels(out_pos, sizeof(out_pos),
					graph->n_outputs_position, graph->outputs_position);
			items[dict.n_items++] = SPA_DICT_ITEM("outputs.audio.position", out_pos);
		}
		impl->info.props = &dict;
		spa_filter_graph_emit_info(&impl->hooks, &impl->info);
		impl->info.props = NULL;
		impl->info.change_mask = old;
	}
}
static int
impl_add_listener(void *object,
		struct spa_hook *listener,
		const struct spa_filter_graph_events *events,
		void *data)
{
	struct impl *impl = object;
	struct spa_hook_list save;

	spa_log_trace(impl->log, "%p: add listener %p", impl, listener);
	spa_hook_list_isolate(&impl->hooks, &save, listener, events, data);

	emit_filter_graph_info(impl, true);

	spa_hook_list_join(&impl->hooks, &save);

	return 0;
}

static int impl_process(void *object,
		const void *in[], void *out[], uint32_t n_samples)
{
	struct impl *impl = object;
	struct graph *graph = &impl->graph;
	uint32_t i, j, n_hndl = graph->n_hndl;
	struct graph_port *port;

	for (i = 0, j = 0; i < graph->n_inputs; i++) {
		while (j < graph->n_input) {
			port = &graph->input[j++];
			if (port->desc && in[i])
				port->desc->connect_port(*port->hndl, port->port, (float*)in[i]);
			if (!port->next)
				break;
		}
	}
	for (i = 0; i < graph->n_outputs; i++) {
		if (out[i] == NULL)
			continue;

		port = i < graph->n_output ? &graph->output[i] : NULL;

		if (port && port->desc)
			port->desc->connect_port(*port->hndl, port->port, out[i]);
		else
			memset(out[i], 0, n_samples * sizeof(float));
	}
	for (i = 0; i < n_hndl; i++) {
		struct graph_hndl *hndl = &graph->hndl[i];
		hndl->desc->run(*hndl->hndl, n_samples);
	}
	return 0;
}

static float get_default(struct impl *impl, struct descriptor *desc, uint32_t p)
{
	struct spa_fga_port *port = &desc->desc->ports[p];
	return port->def;
}

static struct node *find_node(struct graph *graph, const char *name)
{
	struct node *node;
	spa_list_for_each(node, &graph->node_list, link) {
		if (spa_streq(node->name, name))
			return node;
	}
	return NULL;
}

/* find a port by name. Valid syntax is:
 *  "<node_name>:<port_name>"
 *  "<node_name>:<port_id>"
 *  "<port_name>"
 *  "<port_id>"
 *  When no node_name is given, the port is assumed in the current node.  */
static struct port *find_port(struct node *node, const char *name, int descriptor)
{
	char *col, *node_name, *port_name, *str;
	struct port *ports;
	const struct spa_fga_descriptor *d;
	uint32_t i, n_ports, port_id = SPA_ID_INVALID;

	str = strdupa(name);
	col = strchr(str, ':');
	if (col != NULL) {
		struct node *find;
		node_name = str;
		port_name = col + 1;
		*col = '\0';
		find = find_node(node->graph, node_name);
		if (find == NULL) {
			/* it's possible that the : is part of the port name,
			 * try again without splitting things up. */
			*col = ':';
			col = NULL;
		} else {
			node = find;
		}
	}
	if (col == NULL) {
		node_name = node->name;
		port_name = str;
	}
	if (node == NULL)
		return NULL;

	if (!spa_atou32(port_name, &port_id, 0))
		port_id = SPA_ID_INVALID;

	if (SPA_FGA_IS_PORT_INPUT(descriptor)) {
		if (SPA_FGA_IS_PORT_CONTROL(descriptor)) {
			ports = node->control_port;
			n_ports = node->desc->n_control;
		} else {
			ports = node->input_port;
			n_ports = node->desc->n_input;
		}
	} else if (SPA_FGA_IS_PORT_OUTPUT(descriptor)) {
		if (SPA_FGA_IS_PORT_CONTROL(descriptor)) {
			ports = node->notify_port;
			n_ports = node->desc->n_notify;
		} else {
			ports = node->output_port;
			n_ports = node->desc->n_output;
		}
	} else
		return NULL;

	d = node->desc->desc;
	for (i = 0; i < n_ports; i++) {
		struct port *port = &ports[i];
		if (i == port_id ||
		    spa_streq(d->ports[port->p].name, port_name))
			return port;
	}
	return NULL;
}

static int impl_enum_prop_info(void *object, uint32_t idx, struct spa_pod_builder *b,
		struct spa_pod **param)
{
	struct impl *impl = object;
	struct graph *graph = &impl->graph;
	struct spa_pod *pod;
	struct spa_pod_frame f[2];
	struct port *port;
	struct node *node;
	struct descriptor *desc;
	const struct spa_fga_descriptor *d;
	struct spa_fga_port *p;
	float def, min, max;
	char name[512];
	uint32_t rate = impl->rate ? impl->rate : DEFAULT_RATE;

	if (idx >= graph->n_control)
		return 0;

	port = graph->control_port[idx];
	node = port->node;
	desc = node->desc;
	d = desc->desc;
	p = &d->ports[port->p];

	if (p->hint & SPA_FGA_HINT_SAMPLE_RATE) {
		def = p->def * rate;
		min = p->min * rate;
		max = p->max * rate;
	} else {
		def = p->def;
		min = p->min;
		max = p->max;
	}

	if (node->name[0] != '\0')
		snprintf(name, sizeof(name), "%s:%s", node->name, p->name);
	else
		snprintf(name, sizeof(name), "%s", p->name);

	spa_pod_builder_push_object(b, &f[0],
			SPA_TYPE_OBJECT_PropInfo, SPA_PARAM_PropInfo);
	spa_pod_builder_add (b,
			SPA_PROP_INFO_name, SPA_POD_String(name),
			0);
	spa_pod_builder_prop(b, SPA_PROP_INFO_type, 0);
	if (p->hint & SPA_FGA_HINT_BOOLEAN) {
		if (min == max) {
			spa_pod_builder_bool(b, def <= 0.0f ? false : true);
		} else  {
			spa_pod_builder_push_choice(b, &f[1], SPA_CHOICE_Enum, 0);
			spa_pod_builder_bool(b, def <= 0.0f ? false : true);
			spa_pod_builder_bool(b, false);
			spa_pod_builder_bool(b, true);
			spa_pod_builder_pop(b, &f[1]);
		}
	} else if (p->hint & SPA_FGA_HINT_INTEGER) {
		if (min == max) {
			spa_pod_builder_int(b, (int32_t)def);
		} else {
			spa_pod_builder_push_choice(b, &f[1], SPA_CHOICE_Range, 0);
			spa_pod_builder_int(b, (int32_t)def);
			spa_pod_builder_int(b, (int32_t)min);
			spa_pod_builder_int(b, (int32_t)max);
			spa_pod_builder_pop(b, &f[1]);
		}
	} else {
		if (min == max) {
			spa_pod_builder_float(b, def);
		} else {
			spa_pod_builder_push_choice(b, &f[1], SPA_CHOICE_Range, 0);
			spa_pod_builder_float(b, def);
			spa_pod_builder_float(b, min);
			spa_pod_builder_float(b, max);
			spa_pod_builder_pop(b, &f[1]);
		}
	}
	spa_pod_builder_prop(b, SPA_PROP_INFO_params, 0);
	spa_pod_builder_bool(b, true);
	pod = spa_pod_builder_pop(b, &f[0]);
	if (pod == NULL)
		return -ENOSPC;
	if (param)
		*param = pod;

	return 1;
}

static int impl_get_props(void *object, struct spa_pod_builder *b, struct spa_pod **props)
{
	struct impl *impl = object;
	struct graph *graph = &impl->graph;
	struct spa_pod_frame f[2];
	uint32_t i;
	char name[512];
	struct spa_pod *res;

	spa_pod_builder_push_object(b, &f[0],
			SPA_TYPE_OBJECT_Props, SPA_PARAM_Props);
	spa_pod_builder_prop(b, SPA_PROP_params, 0);
	spa_pod_builder_push_struct(b, &f[1]);

	for (i = 0; i < graph->n_control; i++) {
		struct port *port = graph->control_port[i];
		struct node *node = port->node;
		struct descriptor *desc = node->desc;
		const struct spa_fga_descriptor *d = desc->desc;
		struct spa_fga_port *p = &d->ports[port->p];

		if (node->name[0] != '\0')
			snprintf(name, sizeof(name), "%s:%s", node->name, p->name);
		else
			snprintf(name, sizeof(name), "%s", p->name);

		spa_pod_builder_string(b, name);
		if (p->hint & SPA_FGA_HINT_BOOLEAN) {
			spa_pod_builder_bool(b, port->control_data[0] <= 0.0f ? false : true);
		} else if (p->hint & SPA_FGA_HINT_INTEGER) {
			spa_pod_builder_int(b, (int32_t)port->control_data[0]);
		} else {
			spa_pod_builder_float(b, port->control_data[0]);
		}
	}
	spa_pod_builder_pop(b, &f[1]);
	res = spa_pod_builder_pop(b, &f[0]);
	if (res == NULL)
		return -ENOSPC;
	if (props)
		*props = res;
	return 1;
}

static int port_set_control_value(struct port *port, float *value, uint32_t id)
{
	struct node *node = port->node;
	struct impl *impl = node->graph->impl;

	struct descriptor *desc = node->desc;
	float old;
	bool changed;

	old = port->control_data[id];
	port->control_data[id] = value ? *value : desc->default_control[port->idx];
	spa_log_info(impl->log, "control %d %d ('%s') from %f to %f", port->idx, id,
			desc->desc->ports[port->p].name, old, port->control_data[id]);
	changed = old != port->control_data[id];
	node->control_changed |= changed;
	return changed ? 1 : 0;
}

static int set_control_value(struct node *node, const char *name, float *value)
{
	struct port *port;
	int count = 0;
	uint32_t i, n_hndl;

	port = find_port(node, name, SPA_FGA_PORT_INPUT | SPA_FGA_PORT_CONTROL);
	if (port == NULL)
		return -ENOENT;

	/* if we don't have any instances yet, set the first control value, we will
	 * copy to other instances later */
	n_hndl = SPA_MAX(1u, port->node->n_hndl);
	for (i = 0; i < n_hndl; i++)
		count += port_set_control_value(port, value, i);

	return count;
}

static int parse_params(struct graph *graph, const struct spa_pod *pod)
{
	struct spa_pod_parser prs;
	struct spa_pod_frame f;
	int res, changed = 0;
	struct node *def_node;

	def_node = spa_list_first(&graph->node_list, struct node, link);

	spa_pod_parser_pod(&prs, pod);
	if (spa_pod_parser_push_struct(&prs, &f) < 0)
		return 0;

	while (true) {
		const char *name;
		float value, *val = NULL;
		double dbl_val;
		bool bool_val;
		int32_t int_val;

		if (spa_pod_parser_get_string(&prs, &name) < 0)
			break;
		if (spa_pod_parser_get_float(&prs, &value) >= 0) {
			val = &value;
		} else if (spa_pod_parser_get_double(&prs, &dbl_val) >= 0) {
			value = (float)dbl_val;
			val = &value;
		} else if (spa_pod_parser_get_int(&prs, &int_val) >= 0) {
			value = int_val;
			val = &value;
		} else if (spa_pod_parser_get_bool(&prs, &bool_val) >= 0) {
			value = bool_val ? 1.0f : 0.0f;
			val = &value;
		} else {
			struct spa_pod *pod;
			spa_pod_parser_get_pod(&prs, &pod);
		}
		if ((res = set_control_value(def_node, name, val)) > 0)
			changed += res;
	}
	return changed;
}

static int impl_reset(void *object)
{
	struct impl *impl = object;
	struct graph *graph = &impl->graph;
	uint32_t i;
	for (i = 0; i < graph->n_hndl; i++) {
		struct graph_hndl *hndl = &graph->hndl[i];
		const struct spa_fga_descriptor *d = hndl->desc;
		if (hndl->hndl == NULL || *hndl->hndl == NULL)
			continue;
		if (d->deactivate)
			d->deactivate(*hndl->hndl);
		if (d->activate)
			d->activate(*hndl->hndl);
	}
	return 0;
}

static void node_control_changed(struct node *node)
{
	const struct spa_fga_descriptor *d = node->desc->desc;
	uint32_t i;

	if (!node->control_changed)
		return;

	for (i = 0; i < node->n_hndl; i++) {
		if (node->hndl[i] == NULL)
			continue;
		if (d->control_changed)
			d->control_changed(node->hndl[i]);
	}
	node->control_changed = false;
}

static int sync_volume(struct graph *graph, struct volume *vol)
{
	uint32_t i;
	int res = 0;

	if (vol->n_ports == 0)
		return 0;
	for (i = 0; i < vol->n_volumes; i++) {
		uint32_t n_port = i % vol->n_ports, n_hndl;
		struct port *p = vol->ports[n_port];
		float v = vol->mute ? 0.0f : vol->volumes[i];
		switch (vol->scale[n_port]) {
		case SCALE_CUBIC:
			v = cbrtf(v);
			break;
		}
		v = v * (vol->max[n_port] - vol->min[n_port]) + vol->min[n_port];

		n_hndl = SPA_MAX(1u, p->node->n_hndl);
		res += port_set_control_value(p, &v, i % n_hndl);
	}
	return res;
}

static int impl_set_props(void *object, enum spa_direction direction, const struct spa_pod *props)
{
	struct impl *impl = object;
	struct spa_pod_object *obj = (struct spa_pod_object *) props;
	struct spa_pod_frame f[1];
	const struct spa_pod_prop *prop;
	struct graph *graph = &impl->graph;
	int changed = 0;
	char buf[1024];
	struct spa_pod_dynamic_builder b;
	struct volume *vol = &graph->volume[direction];
	bool do_volume = false;

	spa_pod_dynamic_builder_init(&b, buf, sizeof(buf), 1024);
	spa_pod_builder_push_object(&b.b, &f[0], SPA_TYPE_OBJECT_Props, SPA_PARAM_Props);

	SPA_POD_OBJECT_FOREACH(obj, prop) {
		switch (prop->key) {
		case SPA_PROP_params:
			changed += parse_params(graph, &prop->value);
			spa_pod_builder_raw_padded(&b.b, prop, SPA_POD_PROP_SIZE(prop));
			break;
		case SPA_PROP_mute:
		{
			bool mute;
			if (spa_pod_get_bool(&prop->value, &mute) == 0) {
				if (vol->mute != mute) {
					vol->mute = mute;
					do_volume = true;
				}
			}
			spa_pod_builder_raw_padded(&b.b, prop, SPA_POD_PROP_SIZE(prop));
			break;
		}
		case SPA_PROP_channelVolumes:
		{
			uint32_t i, n_vols;
			float vols[SPA_AUDIO_MAX_CHANNELS];

			if ((n_vols = spa_pod_copy_array(&prop->value, SPA_TYPE_Float, vols,
					SPA_AUDIO_MAX_CHANNELS)) > 0) {
				if (vol->n_volumes != n_vols)
					do_volume = true;
				vol->n_volumes = n_vols;
				for (i = 0; i < n_vols; i++) {
					float v = vols[i];
					if (v != vol->volumes[i]) {
						vol->volumes[i] = v;
						do_volume = true;
					}
				}
			}
			spa_pod_builder_raw_padded(&b.b, prop, SPA_POD_PROP_SIZE(prop));
			break;
		}
		case SPA_PROP_softVolumes:
		case SPA_PROP_softMute:
			break;
		default:
			spa_pod_builder_raw_padded(&b.b, prop, SPA_POD_PROP_SIZE(prop));
			break;
		}
	}
	if (do_volume && vol->n_ports != 0) {
		float soft_vols[SPA_AUDIO_MAX_CHANNELS];
		uint32_t i;

		for (i = 0; i < vol->n_volumes; i++)
			soft_vols[i] = (vol->mute || vol->volumes[i] == 0.0f) ? 0.0f : 1.0f;

		spa_pod_builder_prop(&b.b, SPA_PROP_softMute, 0);
		spa_pod_builder_bool(&b.b, vol->mute);
		spa_pod_builder_prop(&b.b, SPA_PROP_softVolumes, 0);
		spa_pod_builder_array(&b.b, sizeof(float), SPA_TYPE_Float,
				vol->n_volumes, soft_vols);
		props = spa_pod_builder_pop(&b.b, &f[0]);

		sync_volume(graph, vol);

	} else {
		props = spa_pod_builder_pop(&b.b, &f[0]);
	}
	spa_filter_graph_emit_apply_props(&impl->hooks, direction, props);

	spa_pod_dynamic_builder_clean(&b);

	if (changed > 0) {
		struct node *node;

		spa_list_for_each(node, &graph->node_list, link)
			node_control_changed(node);

		spa_filter_graph_emit_props_changed(&impl->hooks, SPA_DIRECTION_INPUT);
	}
	return 0;
}

static uint32_t count_array(struct spa_json *json)
{
	struct spa_json it = *json;
	char v[256];
	uint32_t count = 0;
	while (spa_json_get_string(&it, v, sizeof(v)) > 0)
		count++;
	return count;
}

static void plugin_unref(struct plugin *hndl)
{
	struct impl *impl = hndl->impl;

	if (--hndl->ref > 0)
		return;

	spa_list_remove(&hndl->link);
	if (hndl->hndl)
		spa_plugin_loader_unload(impl->loader, hndl->hndl);
	free(hndl);
}

static inline const char *split_walk(const char *str, const char *delimiter, size_t * len, const char **state)
{
	const char *s = *state ? *state : str;

	s += strspn(s, delimiter);
	if (*s == '\0')
		return NULL;

	*len = strcspn(s, delimiter);
	*state = s + *len;

	return s;
}

static struct plugin *plugin_load(struct impl *impl, const char *type, const char *path)
{
	struct spa_handle *hndl = NULL;
	struct plugin *plugin;
	char module[PATH_MAX];
	char factory_name[256], dsp_ptr[256];
	void *iface;
	int res;

	spa_list_for_each(plugin, &impl->plugin_list, link) {
		if (spa_streq(plugin->type, type) &&
		    spa_streq(plugin->path, path)) {
			plugin->ref++;
			return plugin;
		}
	}

	spa_scnprintf(module, sizeof(module),
			"filter-graph/libspa-filter-graph-plugin-%s", type);
	spa_scnprintf(factory_name, sizeof(factory_name),
			"filter.graph.plugin.%s", type);
	spa_scnprintf(dsp_ptr, sizeof(dsp_ptr),
			"pointer:%p", impl->dsp);

	hndl = spa_plugin_loader_load(impl->loader, factory_name,
			&SPA_DICT_ITEMS(
				SPA_DICT_ITEM(SPA_KEY_LIBRARY_NAME, module),
				SPA_DICT_ITEM("filter.graph.path", path),
				SPA_DICT_ITEM("filter.graph.audio.dsp", dsp_ptr)));

	if (hndl == NULL) {
		res = -errno;
		spa_log_error(impl->log, "can't load plugin type '%s': %m", type);
		goto exit;
	}
	if ((res = spa_handle_get_interface(hndl, SPA_TYPE_INTERFACE_FILTER_GRAPH_AudioPlugin, &iface)) < 0) {
		spa_log_error(impl->log, "can't find iface '%s': %s",
				SPA_TYPE_INTERFACE_FILTER_GRAPH_AudioPlugin, spa_strerror(res));
		goto exit;
	}
	plugin = calloc(1, sizeof(*plugin));
	if (!plugin) {
		res = -errno;
		goto exit;
	}

	plugin->ref = 1;
	snprintf(plugin->type, sizeof(plugin->type), "%s", type);
	snprintf(plugin->path, sizeof(plugin->path), "%s", path);

	spa_log_info(impl->log, "successfully opened '%s':'%s'", type, path);

	plugin->impl = impl;
	plugin->hndl = hndl;
	plugin->plugin = iface;

	spa_list_init(&plugin->descriptor_list);
	spa_list_append(&impl->plugin_list, &plugin->link);

	return plugin;
exit:
	if (hndl)
		spa_plugin_loader_unload(impl->loader, hndl);
	errno = -res;
	return NULL;
}

static void descriptor_unref(struct descriptor *desc)
{
	if (--desc->ref > 0)
		return;

	spa_list_remove(&desc->link);
	if (desc->desc)
		spa_fga_descriptor_free(desc->desc);
	plugin_unref(desc->plugin);
	free(desc->input);
	free(desc->output);
	free(desc->control);
	free(desc->default_control);
	free(desc->notify);
	free(desc);
}

static struct descriptor *descriptor_load(struct impl *impl, const char *type,
		const char *plugin, const char *label)
{
	struct plugin *pl;
	struct descriptor *desc;
	const struct spa_fga_descriptor *d;
	uint32_t i, n_input, n_output, n_control, n_notify;
	unsigned long p;
	int res;

	if ((pl = plugin_load(impl, type, plugin)) == NULL)
		return NULL;

	spa_list_for_each(desc, &pl->descriptor_list, link) {
		if (spa_streq(desc->label, label)) {
			desc->ref++;

			/*
			 * since ladspa_handle_load() increments the reference count of the handle,
			 * if the descriptor is found, then the handle's reference count
			 * has already been incremented to account for the descriptor,
			 * so we need to unref handle here since we're merely reusing
			 * thedescriptor, not creating a new one
			 */
			plugin_unref(pl);
			return desc;
		}
	}

	desc = calloc(1, sizeof(*desc));
	desc->ref = 1;
	desc->plugin = pl;
	spa_list_init(&desc->link);

	if ((d = spa_fga_plugin_make_desc(pl->plugin, label)) == NULL) {
		spa_log_error(impl->log, "cannot find label %s", label);
		res = -ENOENT;
		goto exit;
	}
	desc->desc = d;
	snprintf(desc->label, sizeof(desc->label), "%s", label);

	n_input = n_output = n_control = n_notify = 0;
	for (p = 0; p < d->n_ports; p++) {
		struct spa_fga_port *fp = &d->ports[p];
		if (SPA_FGA_IS_PORT_AUDIO(fp->flags)) {
			if (SPA_FGA_IS_PORT_INPUT(fp->flags))
				n_input++;
			else if (SPA_FGA_IS_PORT_OUTPUT(fp->flags))
				n_output++;
		} else if (SPA_FGA_IS_PORT_CONTROL(fp->flags)) {
			if (SPA_FGA_IS_PORT_INPUT(fp->flags))
				n_control++;
			else if (SPA_FGA_IS_PORT_OUTPUT(fp->flags))
				n_notify++;
		}
	}
	desc->input = calloc(n_input, sizeof(unsigned long));
	desc->output = calloc(n_output, sizeof(unsigned long));
	desc->control = calloc(n_control, sizeof(unsigned long));
	desc->default_control = calloc(n_control, sizeof(float));
	desc->notify = calloc(n_notify, sizeof(unsigned long));

	for (p = 0; p < d->n_ports; p++) {
		struct spa_fga_port *fp = &d->ports[p];

		if (SPA_FGA_IS_PORT_AUDIO(fp->flags)) {
			if (SPA_FGA_IS_PORT_INPUT(fp->flags)) {
				spa_log_info(impl->log, "using port %lu ('%s') as input %d", p,
						fp->name, desc->n_input);
				desc->input[desc->n_input++] = p;
			}
			else if (SPA_FGA_IS_PORT_OUTPUT(fp->flags)) {
				spa_log_info(impl->log, "using port %lu ('%s') as output %d", p,
						fp->name, desc->n_output);
				desc->output[desc->n_output++] = p;
			}
		} else if (SPA_FGA_IS_PORT_CONTROL(fp->flags)) {
			if (SPA_FGA_IS_PORT_INPUT(fp->flags)) {
				spa_log_info(impl->log, "using port %lu ('%s') as control %d", p,
						fp->name, desc->n_control);
				desc->control[desc->n_control++] = p;
			}
			else if (SPA_FGA_IS_PORT_OUTPUT(fp->flags)) {
				spa_log_info(impl->log, "using port %lu ('%s') as notify %d", p,
						fp->name, desc->n_notify);
				desc->notify[desc->n_notify++] = p;
			}
		}
	}
	if (desc->n_input == 0 && desc->n_output == 0 && desc->n_control == 0 && desc->n_notify == 0) {
		spa_log_error(impl->log, "plugin has no input and no output ports");
		res = -ENOTSUP;
		goto exit;
	}
	for (i = 0; i < desc->n_control; i++) {
		p = desc->control[i];
		desc->default_control[i] = get_default(impl, desc, p);
		spa_log_info(impl->log, "control %d ('%s') default to %f", i,
				d->ports[p].name, desc->default_control[i]);
	}
	spa_list_append(&pl->descriptor_list, &desc->link);

	return desc;

exit:
	descriptor_unref(desc);
	errno = -res;
	return NULL;
}

/**
 * {
 *   ...
 * }
 */
static int parse_config(struct node *node, struct spa_json *config)
{
	const char *val, *s = config->cur;
	struct impl *impl = node->graph->impl;
	int res = 0, len;
	struct spa_error_location loc;

	if ((len = spa_json_next(config, &val)) <= 0) {
		res = -EINVAL;
		goto done;
	}
	if (spa_json_is_null(val, len))
		goto done;

	if (spa_json_is_container(val, len)) {
		len = spa_json_container_len(config, val, len);
		if (len == 0) {
			res = -EINVAL;
			goto done;
		}
	}
	if ((node->config = malloc(len+1)) == NULL) {
		res = -errno;
		goto done;
	}

	spa_json_parse_stringn(val, len, node->config, len+1);
done:
	if (spa_json_get_error(config, s, &loc))
		spa_debug_log_error_location(impl->log, SPA_LOG_LEVEL_WARN,
				&loc, "error: %s", loc.reason);
	return res;
}

/**
 * {
 *   "Reverb tail" = 2.0
 *   ...
 * }
 */
static int parse_control(struct node *node, struct spa_json *control)
{
	struct impl *impl = node->graph->impl;
	char key[256];
	const char *val;
	int len;

	while ((len = spa_json_object_next(control, key, sizeof(key), &val)) > 0) {
		float fl;
		int res;

		if (spa_json_parse_float(val, len, &fl) <= 0) {
			spa_log_warn(impl->log, "control '%s' expects a number, ignoring", key);
		}
		else if ((res = set_control_value(node, key, &fl)) < 0) {
			spa_log_warn(impl->log, "control '%s' can not be set: %s", key, spa_strerror(res));
		}
	}
	return 0;
}

/**
 * output = [name:][portname]
 * input = [name:][portname]
 * ...
 */
static int parse_link(struct graph *graph, struct spa_json *json)
{
	struct impl *impl = graph->impl;
	char key[256];
	char output[256] = "";
	char input[256] = "";
	const char *val;
	struct node *def_in_node, *def_out_node;
	struct port *in_port, *out_port;
	struct link *link;
	int len;

	if (spa_list_is_empty(&graph->node_list)) {
		spa_log_error(impl->log, "can't make links in graph without nodes");
		return -EINVAL;
	}

	while ((len = spa_json_object_next(json, key, sizeof(key), &val)) > 0) {
		if (spa_streq(key, "output")) {
			if (spa_json_parse_stringn(val, len, output, sizeof(output)) <= 0) {
				spa_log_error(impl->log, "output expects a string");
				return -EINVAL;
			}
		}
		else if (spa_streq(key, "input")) {
			if (spa_json_parse_stringn(val, len, input, sizeof(input)) <= 0) {
				spa_log_error(impl->log, "input expects a string");
				return -EINVAL;
			}
		}
		else {
			spa_log_error(impl->log, "unexpected link key '%s'", key);
		}
	}
	def_out_node = spa_list_first(&graph->node_list, struct node, link);
	def_in_node = spa_list_last(&graph->node_list, struct node, link);

	out_port = find_port(def_out_node, output, SPA_FGA_PORT_OUTPUT);
	in_port = find_port(def_in_node, input, SPA_FGA_PORT_INPUT);

	if (out_port == NULL && out_port == NULL) {
		/* try control ports */
		out_port = find_port(def_out_node, output, SPA_FGA_PORT_OUTPUT | SPA_FGA_PORT_CONTROL);
		in_port = find_port(def_in_node, input, SPA_FGA_PORT_INPUT | SPA_FGA_PORT_CONTROL);
	}
	if (in_port == NULL || out_port == NULL) {
		if (out_port == NULL)
			spa_log_error(impl->log, "unknown output port %s", output);
		if (in_port == NULL)
			spa_log_error(impl->log, "unknown input port %s", input);
		return -ENOENT;
	}

	if (in_port->n_links > 0) {
		spa_log_info(impl->log, "Can't have more than 1 link to %s, use a mixer", input);
		return -ENOTSUP;
	}

	if ((link = calloc(1, sizeof(*link))) == NULL)
		return -errno;

	link->output = out_port;
	link->input = in_port;

	spa_log_info(impl->log, "linking %s:%s -> %s:%s",
			out_port->node->name,
			out_port->node->desc->desc->ports[out_port->p].name,
			in_port->node->name,
			in_port->node->desc->desc->ports[in_port->p].name);

	spa_list_append(&out_port->link_list, &link->output_link);
	out_port->n_links++;
	spa_list_append(&in_port->link_list, &link->input_link);
	in_port->n_links++;

	in_port->node->n_deps++;

	spa_list_append(&graph->link_list, &link->link);

	return 0;
}

static void link_free(struct link *link)
{
	spa_list_remove(&link->input_link);
	link->input->n_links--;
	link->input->node->n_deps--;
	spa_list_remove(&link->output_link);
	link->output->n_links--;
	spa_list_remove(&link->link);
	free(link);
}

/**
 * {
 *   control = [name:][portname]
 *   min = <float, default 0.0>
 *   max = <float, default 1.0>
 *   scale = <string, default "linear", options "linear","cubic">
 * }
 */
static int parse_volume(struct graph *graph, struct spa_json *json, enum spa_direction direction)
{
	struct impl *impl = graph->impl;
	char key[256];
	char control[256] = "";
	char scale[64] = "linear";
	float min = 0.0f, max = 1.0f;
	const char *val;
	struct node *def_control;
	struct port *port;
	struct volume *vol = &graph->volume[direction];
	int len;

	if (spa_list_is_empty(&graph->node_list)) {
		spa_log_error(impl->log, "can't set volume in graph without nodes");
		return -EINVAL;
	}
	while ((len = spa_json_object_next(json, key, sizeof(key), &val)) > 0) {
		if (spa_streq(key, "control")) {
			if (spa_json_parse_stringn(val, len, control, sizeof(control)) <= 0) {
				spa_log_error(impl->log, "control expects a string");
				return -EINVAL;
			}
		}
		else if (spa_streq(key, "min")) {
			if (spa_json_parse_float(val, len, &min) <= 0) {
				spa_log_error(impl->log, "min expects a float");
				return -EINVAL;
			}
		}
		else if (spa_streq(key, "max")) {
			if (spa_json_parse_float(val, len, &max) <= 0) {
				spa_log_error(impl->log, "max expects a float");
				return -EINVAL;
			}
		}
		else if (spa_streq(key, "scale")) {
			if (spa_json_parse_stringn(val, len, scale, sizeof(scale)) <= 0) {
				spa_log_error(impl->log, "scale expects a string");
				return -EINVAL;
			}
		}
		else {
			spa_log_error(impl->log, "unexpected volume key '%s'", key);
		}
	}
	if (direction == SPA_DIRECTION_INPUT)
		def_control = spa_list_first(&graph->node_list, struct node, link);
	else
		def_control = spa_list_last(&graph->node_list, struct node, link);

	port = find_port(def_control, control, SPA_FGA_PORT_INPUT | SPA_FGA_PORT_CONTROL);
	if (port == NULL) {
		spa_log_error(impl->log, "unknown control port %s", control);
		return -ENOENT;
	}
	if (vol->n_ports >= SPA_AUDIO_MAX_CHANNELS) {
		spa_log_error(impl->log, "too many volume controls");
		return -ENOSPC;
	}
	if (spa_streq(scale, "linear")) {
		vol->scale[vol->n_ports] = SCALE_LINEAR;
	} else if (spa_streq(scale, "cubic")) {
		vol->scale[vol->n_ports] = SCALE_CUBIC;
	} else {
		spa_log_error(impl->log, "Invalid scale value '%s', use one of linear or cubic", scale);
		return -EINVAL;
	}
	spa_log_info(impl->log, "volume %d: \"%s:%s\" min:%f max:%f scale:%s", vol->n_ports, port->node->name,
			port->node->desc->desc->ports[port->p].name, min, max, scale);

	vol->ports[vol->n_ports] = port;
	vol->min[vol->n_ports] = min;
	vol->max[vol->n_ports] = max;
	vol->n_ports++;

	return 0;
}

/**
 * type = ladspa
 * name = rev
 * plugin = g2reverb
 * label = G2reverb
 * config = {
 *     ...
 * }
 * control = {
 *     ...
 * }
 */
static int load_node(struct graph *graph, struct spa_json *json)
{
	struct impl *impl = graph->impl;
	struct spa_json control, config;
	struct descriptor *desc;
	struct node *node;
	const char *val;
	char key[256];
	char type[256] = "";
	char name[256] = "";
	char plugin[256] = "";
	char label[256] = "";
	bool have_control = false;
	bool have_config = false;
	uint32_t i;
	int res, len;

	while ((len = spa_json_object_next(json, key, sizeof(key), &val)) > 0) {
		if (spa_streq("type", key)) {
			if (spa_json_parse_stringn(val, len, type, sizeof(type)) <= 0) {
				spa_log_error(impl->log, "type expects a string");
				return -EINVAL;
			}
		} else if (spa_streq("name", key)) {
			if (spa_json_parse_stringn(val, len, name, sizeof(name)) <= 0) {
				spa_log_error(impl->log, "name expects a string");
				return -EINVAL;
			}
		} else if (spa_streq("plugin", key)) {
			if (spa_json_parse_stringn(val, len, plugin, sizeof(plugin)) <= 0) {
				spa_log_error(impl->log, "plugin expects a string");
				return -EINVAL;
			}
		} else if (spa_streq("label", key)) {
			if (spa_json_parse_stringn(val, len, label, sizeof(label)) <= 0) {
				spa_log_error(impl->log, "label expects a string");
				return -EINVAL;
			}
		} else if (spa_streq("control", key)) {
			if (!spa_json_is_object(val, len)) {
				spa_log_error(impl->log, "control expects an object");
				return -EINVAL;
			}
			spa_json_enter(json, &control);
			have_control = true;
		} else if (spa_streq("config", key)) {
			config = SPA_JSON_START(json, val);
			have_config = true;
		} else {
			spa_log_warn(impl->log, "unexpected node key '%s'", key);
		}
	}
	if (spa_streq(type, "builtin"))
		snprintf(plugin, sizeof(plugin), "%s", "builtin");
	else if (spa_streq(type, "")) {
		spa_log_error(impl->log, "missing plugin type");
		return -EINVAL;
	}

	spa_log_info(impl->log, "loading type:%s plugin:%s label:%s", type, plugin, label);

	if ((desc = descriptor_load(graph->impl, type, plugin, label)) == NULL)
		return -errno;

	node = calloc(1, sizeof(*node));
	if (node == NULL)
		return -errno;

	node->graph = graph;
	node->desc = desc;
	snprintf(node->name, sizeof(node->name), "%s", name);

	node->input_port = calloc(desc->n_input, sizeof(struct port));
	node->output_port = calloc(desc->n_output, sizeof(struct port));
	node->control_port = calloc(desc->n_control, sizeof(struct port));
	node->notify_port = calloc(desc->n_notify, sizeof(struct port));

	spa_log_info(impl->log, "loaded n_input:%d n_output:%d n_control:%d n_notify:%d",
			desc->n_input, desc->n_output,
			desc->n_control, desc->n_notify);

	for (i = 0; i < desc->n_input; i++) {
		struct port *port = &node->input_port[i];
		port->node = node;
		port->idx = i;
		port->external = SPA_ID_INVALID;
		port->p = desc->input[i];
		spa_list_init(&port->link_list);
	}
	for (i = 0; i < desc->n_output; i++) {
		struct port *port = &node->output_port[i];
		port->node = node;
		port->idx = i;
		port->external = SPA_ID_INVALID;
		port->p = desc->output[i];
		spa_list_init(&port->link_list);
	}
	for (i = 0; i < desc->n_control; i++) {
		struct port *port = &node->control_port[i];
		port->node = node;
		port->idx = i;
		port->external = SPA_ID_INVALID;
		port->p = desc->control[i];
		spa_list_init(&port->link_list);
		port->control_data[0] = desc->default_control[i];
	}
	for (i = 0; i < desc->n_notify; i++) {
		struct port *port = &node->notify_port[i];
		port->node = node;
		port->idx = i;
		port->external = SPA_ID_INVALID;
		port->p = desc->notify[i];
		spa_list_init(&port->link_list);
	}
	if (have_config)
		if ((res = parse_config(node, &config)) < 0)
			spa_log_warn(impl->log, "error parsing config: %s", spa_strerror(res));
	if (have_control)
		parse_control(node, &control);

	spa_list_append(&graph->node_list, &node->link);

	return 0;
}

static void node_cleanup(struct node *node)
{
	const struct spa_fga_descriptor *d = node->desc->desc;
	struct impl *impl = node->graph->impl;
	uint32_t i;

	for (i = 0; i < node->n_hndl; i++) {
		if (node->hndl[i] == NULL)
			continue;
		spa_log_info(impl->log, "cleanup %s %s[%d]", d->name, node->name, i);
		if (d->deactivate)
			d->deactivate(node->hndl[i]);
		d->cleanup(node->hndl[i]);
		node->hndl[i] = NULL;
	}
}

static int port_ensure_data(struct port *port, uint32_t i, uint32_t max_samples)
{
	float *data;
	struct node *node = port->node;
	const struct spa_fga_descriptor *d = node->desc->desc;
	struct impl *impl = node->graph->impl;

	if ((data = port->audio_mem[i]) == NULL) {
		data = calloc(max_samples, sizeof(float) + impl->max_align);
		if (data == NULL) {
			spa_log_error(impl->log, "cannot create port data: %m");
			return -errno;
		}
		port->audio_mem[i] = data;
		port->audio_data[i] = SPA_PTR_ALIGN(data, impl->max_align, void);
	}
	spa_log_info(impl->log, "connect output port %s[%d]:%s %p",
			node->name, i, d->ports[port->p].name, port->audio_data[i]);
	d->connect_port(port->node->hndl[i], port->p, port->audio_data[i]);
	return 0;
}

static void port_free_data(struct port *port, uint32_t i)
{
	free(port->audio_mem[i]);
	port->audio_mem[i] = NULL;
	port->audio_data[i] = NULL;
}

static void node_free(struct node *node)
{
	uint32_t i, j;

	spa_list_remove(&node->link);
	for (i = 0; i < node->n_hndl; i++) {
		for (j = 0; j < node->desc->n_output; j++)
			port_free_data(&node->output_port[j], i);
	}
	node_cleanup(node);
	descriptor_unref(node->desc);
	free(node->input_port);
	free(node->output_port);
	free(node->control_port);
	free(node->notify_port);
	free(node->config);
	free(node);
}

static int impl_deactivate(void *object)
{
	struct impl *impl = object;
	struct graph *graph = &impl->graph;
	struct node *node;

	if (!graph->activated)
		return 0;

	graph->activated = false;
	spa_list_for_each(node, &graph->node_list, link)
		node_cleanup(node);
	return 0;
}

static int setup_graph(struct graph *graph);

static int impl_activate(void *object, const struct spa_dict *props)
{
	struct impl *impl = object;
	struct graph *graph = &impl->graph;
	struct node *node;
	struct port *port;
	struct link *link;
	struct descriptor *desc;
	const struct spa_fga_descriptor *d;
	const struct spa_fga_plugin *p;
	uint32_t i, j, max_samples = impl->quantum_limit, n_ports;
	int res;
	float *sd, *dd, *data;
	const char *rate, *str;

	if (graph->activated)
		return 0;

	graph->activated = true;

	rate = spa_dict_lookup(props, SPA_KEY_AUDIO_RATE);
	impl->rate = rate ? atoi(rate) : DEFAULT_RATE;

	if ((str = spa_dict_lookup(props, "filter-graph.n_inputs")) != NULL) {
		if (spa_atou32(str, &n_ports, 0) &&
		    n_ports != graph->n_inputs) {
			graph->n_inputs = n_ports;
			graph->n_outputs = 0;
			impl->info.change_mask |= SPA_FILTER_GRAPH_CHANGE_MASK_PROPS;
			graph->setup = false;
		}
	}
	if ((str = spa_dict_lookup(props, "filter-graph.n_outputs")) != NULL) {
		if (spa_atou32(str, &n_ports, 0) &&
		    n_ports != graph->n_outputs) {
			graph->n_outputs = n_ports;
			graph->n_inputs = 0;
			impl->info.change_mask |= SPA_FILTER_GRAPH_CHANGE_MASK_PROPS;
			graph->setup = false;
		}
	}
	if (!graph->setup) {
		if ((res = setup_graph(graph)) < 0)
			return res;
		graph->setup = true;
		emit_filter_graph_info(impl, false);
	}

	/* first make instances */
	spa_list_for_each(node, &graph->node_list, link) {
		node_cleanup(node);

		desc = node->desc;
		d = desc->desc;
		p = desc->plugin->plugin;

		for (i = 0; i < node->n_hndl; i++) {
			spa_log_info(impl->log, "instantiate %s %s[%d] rate:%lu", d->name, node->name, i, impl->rate);
			errno = EINVAL;
			if ((node->hndl[i] = d->instantiate(p, d, impl->rate, i, node->config)) == NULL) {
				spa_log_error(impl->log, "cannot create plugin instance %d rate:%lu: %m", i, impl->rate);
				res = -errno;
				goto error;
			}
		}
	}

	/* then link ports */
	spa_list_for_each(node, &graph->node_list, link) {
		desc = node->desc;
		d = desc->desc;
		if (d->flags & SPA_FGA_DESCRIPTOR_SUPPORTS_NULL_DATA) {
			sd = dd = NULL;
		}
		else {
			sd = impl->silence_data;
			dd = impl->discard_data;
		}
		for (i = 0; i < node->n_hndl; i++) {
			for (j = 0; j < desc->n_input; j++) {
				port = &node->input_port[j];
				if (!spa_list_is_empty(&port->link_list)) {
					link = spa_list_first(&port->link_list, struct link, input_link);
					if ((res = port_ensure_data(link->output, i, max_samples)) < 0)
						goto error;
					data = link->output->audio_data[i];
				} else {
					data = sd;
				}
				spa_log_info(impl->log, "connect input port %s[%d]:%s %p",
						node->name, i, d->ports[port->p].name, data);
				d->connect_port(node->hndl[i], port->p, data);
			}
			for (j = 0; j < desc->n_output; j++) {
				port = &node->output_port[j];
				if (port->audio_data[i] == NULL) {
					spa_log_info(impl->log, "connect output port %s[%d]:%s %p",
						node->name, i, d->ports[port->p].name, dd);
					d->connect_port(node->hndl[i], port->p, dd);
				}
			}
			for (j = 0; j < desc->n_control; j++) {
				port = &node->control_port[j];

				if (!spa_list_is_empty(&port->link_list)) {
					link = spa_list_first(&port->link_list, struct link, input_link);
					data = &link->output->control_data[i];
				} else {
					data = &port->control_data[i];
				}
				spa_log_info(impl->log, "connect control port %s[%d]:%s %p",
						node->name, i, d->ports[port->p].name, data);
				d->connect_port(node->hndl[i], port->p, data);
			}
			for (j = 0; j < desc->n_notify; j++) {
				port = &node->notify_port[j];
				spa_log_info(impl->log, "connect notify port %s[%d]:%s %p",
						node->name, i, d->ports[port->p].name,
						&port->control_data[i]);
				d->connect_port(node->hndl[i], port->p, &port->control_data[i]);
			}
		}
	}

	/* now activate */
	spa_list_for_each(node, &graph->node_list, link) {
		desc = node->desc;
		d = desc->desc;

		for (i = 0; i < node->n_hndl; i++) {
			if (d->activate)
				d->activate(node->hndl[i]);
			if (node->control_changed && d->control_changed)
				d->control_changed(node->hndl[i]);
		}
	}

	spa_filter_graph_emit_props_changed(&impl->hooks, SPA_DIRECTION_INPUT);
	return 0;
error:
	impl_deactivate(impl);
	return res;
}

/* any default values for the controls are set in the first instance
 * of the control data. Duplicate this to the other instances now. */
static void setup_node_controls(struct node *node)
{
	uint32_t i, j;
	uint32_t n_hndl = node->n_hndl;
	uint32_t n_ports = node->desc->n_control;
	struct port *ports = node->control_port;

	for (i = 0; i < n_ports; i++) {
		struct port *port = &ports[i];
		for (j = 1; j < n_hndl; j++)
			port->control_data[j] = port->control_data[0];
	}
}

static struct node *find_next_node(struct graph *graph)
{
	struct node *node;
	spa_list_for_each(node, &graph->node_list, link) {
		if (node->n_deps == 0 && !node->visited) {
			node->visited = true;
			return node;
		}
	}
	return NULL;
}

static void unsetup_graph(struct graph *graph)
{
	free(graph->input);
	graph->input = NULL;
	free(graph->output);
	graph->output = NULL;
	free(graph->hndl);
	graph->hndl = NULL;
	free(graph->control_port);
	graph->control_port = NULL;

}
static int setup_graph(struct graph *graph)
{
	struct impl *impl = graph->impl;
	struct node *node, *first, *last;
	struct port *port;
	struct link *link;
	struct graph_port *gp;
	struct graph_hndl *gh;
	uint32_t i, j, n, n_nodes, n_input, n_output, n_control, n_hndl = 0;
	int res;
	struct descriptor *desc;
	const struct spa_fga_descriptor *d;
	char *pname;
	bool allow_unused;

	unsetup_graph(graph);

	first = spa_list_first(&graph->node_list, struct node, link);
	last = spa_list_last(&graph->node_list, struct node, link);

	/* calculate the number of inputs and outputs into the graph.
	 * If we have a list of inputs/outputs, just use them. Otherwise
	 * we count all input ports of the first node and all output
	 * ports of the last node */
	if (graph->n_input_names != 0)
		n_input = graph->n_input_names;
	else
		n_input = first->desc->n_input;

	if (graph->n_output_names != 0)
		n_output = graph->n_output_names;
	else
		n_output = last->desc->n_output;

	/* we allow unconnected ports when not explicitly given and the nodes support
	 * NULL data */
	allow_unused = graph->n_input_names == 0 && graph->n_output_names == 0 &&
	    SPA_FLAG_IS_SET(first->desc->desc->flags, SPA_FGA_DESCRIPTOR_SUPPORTS_NULL_DATA) &&
	    SPA_FLAG_IS_SET(last->desc->desc->flags, SPA_FGA_DESCRIPTOR_SUPPORTS_NULL_DATA);

	if (n_input == 0) {
		spa_log_error(impl->log, "no inputs");
		res = -EINVAL;
		goto error;
	}
	if (n_output == 0) {
		spa_log_error(impl->log, "no outputs");
		res = -EINVAL;
		goto error;
	}
	if (graph->n_inputs == 0)
		graph->n_inputs = impl->info.n_inputs;
	if (graph->n_inputs == 0)
		graph->n_inputs = n_input;

	if (graph->n_outputs == 0)
		graph->n_outputs = impl->info.n_outputs;

	/* compare to the requested number of inputs and duplicate the
	 * graph n_hndl times when needed. */
	n_hndl = graph->n_inputs / n_input;

	if (graph->n_outputs == 0)
		graph->n_outputs = n_output * n_hndl;

	if (n_hndl != graph->n_outputs / n_output) {
		spa_log_error(impl->log, "invalid ports. The input stream has %1$d ports and "
				"the filter has %2$d inputs. The output stream has %3$d ports "
				"and the filter has %4$d outputs. input:%1$d / input:%2$d != "
				"output:%3$d / output:%4$d. Check inputs and outputs objects.",
				graph->n_inputs, n_input,
				graph->n_outputs, n_output);
		res = -EINVAL;
		goto error;
	}
	if (n_hndl > MAX_HNDL) {
		spa_log_error(impl->log, "too many ports. %d > %d", n_hndl, MAX_HNDL);
		res = -EINVAL;
		goto error;
	}
	if (n_hndl == 0) {
		n_hndl = 1;
		if (!allow_unused)
			spa_log_warn(impl->log, "The input stream has %1$d ports and "
				"the filter has %2$d inputs. The output stream has %3$d ports "
				"and the filter has %4$d outputs. Some filter ports will be "
				"unconnected..",
				graph->n_inputs, n_input,
				graph->n_outputs, n_output);

		if (graph->n_outputs == 0)
			graph->n_outputs = n_output * n_hndl;
	}
	spa_log_info(impl->log, "using %d instances %d %d", n_hndl, n_input, n_output);

	/* now go over all nodes and create instances. */
	n_control = 0;
	n_nodes = 0;
	spa_list_for_each(node, &graph->node_list, link) {
		node->n_hndl = n_hndl;
		desc = node->desc;
		n_control += desc->n_control;
		n_nodes++;
		setup_node_controls(node);
	}
	graph->n_input = 0;
	graph->input = calloc(n_input * 16 * n_hndl, sizeof(struct graph_port));
	graph->n_output = 0;
	graph->output = calloc(n_output * n_hndl, sizeof(struct graph_port));

	/* now collect all input and output ports for all the handles. */
	for (i = 0; i < n_hndl; i++) {
		if (graph->n_input_names == 0) {
			desc = first->desc;
			d = desc->desc;
			for (j = 0; j < desc->n_input; j++) {
				gp = &graph->input[graph->n_input++];
				spa_log_info(impl->log, "input port %s[%d]:%s",
						first->name, i, d->ports[desc->input[j]].name);
				gp->desc = d;
				gp->hndl = &first->hndl[i];
				gp->port = desc->input[j];
			}
		} else {
			for (n = 0; n < graph->n_input_names; n++) {
				pname = graph->input_names[n];
				if (spa_streq(pname, "null")) {
					gp = &graph->input[graph->n_input++];
					gp->desc = NULL;
					spa_log_info(impl->log, "ignore input port %d", graph->n_input);
				} else if ((port = find_port(first, pname, SPA_FGA_PORT_INPUT)) == NULL) {
					res = -ENOENT;
					spa_log_error(impl->log, "input port %s not found", pname);
					goto error;
				} else {
					bool disabled = false;

					desc = port->node->desc;
					d = desc->desc;
					if (i == 0 && port->external != SPA_ID_INVALID) {
						spa_log_error(impl->log, "input port %s[%d]:%s already used as input %d, use mixer",
							port->node->name, i, d->ports[port->p].name,
							port->external);
						res = -EBUSY;
						goto error;
					}
					if (port->n_links > 0) {
						spa_log_error(impl->log, "input port %s[%d]:%s already used by link, use mixer",
							port->node->name, i, d->ports[port->p].name);
						res = -EBUSY;
						goto error;
					}

					if (d->flags & SPA_FGA_DESCRIPTOR_COPY) {
						for (j = 0; j < desc->n_output; j++) {
							struct port *p = &port->node->output_port[j];
							struct link *link;

							gp = NULL;
							spa_list_for_each(link, &p->link_list, output_link) {
								struct port *peer = link->input;

								spa_log_info(impl->log, "copy input port %s[%d]:%s",
									port->node->name, i,
									d->ports[port->p].name);
								peer->external = graph->n_input;
								gp = &graph->input[graph->n_input++];
								gp->desc = peer->node->desc->desc;
								gp->hndl = &peer->node->hndl[i];
								gp->port = peer->p;
								gp->next = true;
								disabled = true;
							}
							if (gp != NULL)
								gp->next = false;
						}
						port->node->disabled = disabled;
					}
					if (!disabled) {
						spa_log_info(impl->log, "input port %s[%d]:%s",
							port->node->name, i, d->ports[port->p].name);
						port->external = graph->n_input;
						gp = &graph->input[graph->n_input++];
						gp->desc = d;
						gp->hndl = &port->node->hndl[i];
						gp->port = port->p;
						gp->next = false;
					}
				}
			}
		}
		if (graph->n_output_names == 0) {
			desc = last->desc;
			d = desc->desc;
			for (j = 0; j < desc->n_output; j++) {
				gp = &graph->output[graph->n_output++];
				spa_log_info(impl->log, "output port %s[%d]:%s",
						last->name, i, d->ports[desc->output[j]].name);
				gp->desc = d;
				gp->hndl = &last->hndl[i];
				gp->port = desc->output[j];
			}
		} else {
			for (n = 0; n < graph->n_output_names; n++) {
				pname = graph->output_names[n];
				gp = &graph->output[graph->n_output];
				if (spa_streq(pname, "null")) {
					gp->desc = NULL;
					spa_log_info(impl->log, "silence output port %d", graph->n_output);
				} else if ((port = find_port(last, pname, SPA_FGA_PORT_OUTPUT)) == NULL) {
					res = -ENOENT;
					spa_log_error(impl->log, "output port %s not found", pname);
					goto error;
				} else {
					desc = port->node->desc;
					d = desc->desc;
					if (i == 0 && port->external != SPA_ID_INVALID) {
						spa_log_error(impl->log, "output port %s[%d]:%s already used as output %d, use copy",
							port->node->name, i, d->ports[port->p].name,
							port->external);
						res = -EBUSY;
						goto error;
					}
					if (port->n_links > 0) {
						spa_log_error(impl->log, "output port %s[%d]:%s already used by link, use copy",
							port->node->name, i, d->ports[port->p].name);
						res = -EBUSY;
						goto error;
					}
					spa_log_info(impl->log, "output port %s[%d]:%s",
							port->node->name, i, d->ports[port->p].name);
					port->external = graph->n_output;
					gp->desc = d;
					gp->hndl = &port->node->hndl[i];
					gp->port = port->p;
				}
				graph->n_output++;
			}
		}
	}

	/* order all nodes based on dependencies */
	graph->n_hndl = 0;
	graph->hndl = calloc(n_nodes * n_hndl, sizeof(struct graph_hndl));
	graph->n_control = 0;
	graph->control_port = calloc(n_control, sizeof(struct port *));
	while (true) {
		if ((node = find_next_node(graph)) == NULL)
			break;

		desc = node->desc;
		d = desc->desc;

		if (!node->disabled) {
			for (i = 0; i < n_hndl; i++) {
				gh = &graph->hndl[graph->n_hndl++];
				gh->hndl = &node->hndl[i];
				gh->desc = d;
			}
		}
		for (i = 0; i < desc->n_output; i++) {
			spa_list_for_each(link, &node->output_port[i].link_list, output_link)
				link->input->node->n_deps--;
		}
		for (i = 0; i < desc->n_notify; i++) {
			spa_list_for_each(link, &node->notify_port[i].link_list, output_link)
				link->input->node->n_deps--;
		}

		/* collect all control ports on the graph */
		for (i = 0; i < desc->n_control; i++) {
			graph->control_port[graph->n_control] = &node->control_port[i];
			graph->n_control++;
		}
	}
	res = 0;
error:
	return res;
}

/**
 * filter.graph = {
 *     nodes = [
 *         { ... } ...
 *     ]
 *     links = [
 *         { ... } ...
 *     ]
 *     inputs = [ ]
 *     outputs = [ ]
 *     input.volumes = [
 *         ...
 *     ]
 *     output.volumes = [
 *         ...
 *     ]
 * }
 */
static int load_graph(struct graph *graph, const struct spa_dict *props)
{
	struct impl *impl = graph->impl;
	struct spa_json it[2];
	struct spa_json inputs, outputs, *pinputs = NULL, *poutputs = NULL;
	struct spa_json ivolumes, ovolumes, *pivolumes = NULL, *povolumes = NULL;
	struct spa_json nodes, *pnodes = NULL, links, *plinks = NULL;
	const char *json, *val;
	char key[256];
	int res, len;

	spa_list_init(&graph->node_list);
	spa_list_init(&graph->link_list);

	if ((json = spa_dict_lookup(props, "filter.graph")) == NULL) {
		spa_log_error(impl->log, "missing filter.graph property");
		return -EINVAL;
	}

        if (spa_json_begin_object(&it[0], json, strlen(json)) <= 0) {
		spa_log_error(impl->log, "filter.graph must be an object");
		return -EINVAL;
	}

	while ((len = spa_json_object_next(&it[0], key, sizeof(key), &val)) > 0) {
		if (spa_streq("n_inputs", key)) {
			if (spa_json_parse_int(val, len, &res) <= 0) {
				spa_log_error(impl->log, "%s expects an integer", key);
				return -EINVAL;
			}
			impl->info.n_inputs = res;
		}
		else if (spa_streq("n_outputs", key)) {
			if (spa_json_parse_int(val, len, &res) <= 0) {
				spa_log_error(impl->log, "%s expects an integer", key);
				return -EINVAL;
			}
			impl->info.n_outputs = res;
		}
		else if (spa_streq("inputs.audio.position", key)) {
			if (!spa_json_is_array(val, len) ||
			    (len = spa_json_container_len(&it[0], val, len)) < 0) {
				spa_log_error(impl->log, "%s expects an array", key);
				return -EINVAL;
			}
			spa_audio_parse_position(val, len, graph->inputs_position,
						&graph->n_inputs_position);
			impl->info.n_inputs = graph->n_inputs_position;
		}
		else if (spa_streq("outputs.audio.position", key)) {
			if (!spa_json_is_array(val, len) ||
			    (len = spa_json_container_len(&it[0], val, len)) < 0) {
				spa_log_error(impl->log, "%s expects an array", key);
				return -EINVAL;
			}
			spa_audio_parse_position(val, len, graph->outputs_position,
						&graph->n_outputs_position);
			impl->info.n_outputs = graph->n_outputs_position;
		}
		else if (spa_streq("nodes", key)) {
			if (!spa_json_is_array(val, len)) {
				spa_log_error(impl->log, "%s expects an array", key);
				return -EINVAL;
			}
			spa_json_enter(&it[0], &nodes);
			pnodes = &nodes;
		}
		else if (spa_streq("links", key)) {
			if (!spa_json_is_array(val, len)) {
				spa_log_error(impl->log, "%s expects an array", key);
				return -EINVAL;
			}
			spa_json_enter(&it[0], &links);
			plinks = &links;
		}
		else if (spa_streq("inputs", key)) {
			if (!spa_json_is_array(val, len)) {
				spa_log_error(impl->log, "%s expects an array", key);
				return -EINVAL;
			}
			spa_json_enter(&it[0], &inputs);
			pinputs = &inputs;
		}
		else if (spa_streq("outputs", key)) {
			if (!spa_json_is_array(val, len)) {
				spa_log_error(impl->log, "%s expects an array", key);
				return -EINVAL;
			}
			spa_json_enter(&it[0], &outputs);
			poutputs = &outputs;
		}
		else if (spa_streq("capture.volumes", key) ||
		    spa_streq("input.volumes", key)) {
			if (!spa_json_is_array(val, len)) {
				spa_log_error(impl->log, "%s expects an array", key);
				return -EINVAL;
			}
			spa_json_enter(&it[0], &ivolumes);
			pivolumes = &ivolumes;
		}
		else if (spa_streq("playback.volumes", key) ||
		    spa_streq("output.volumes", key)) {
			if (!spa_json_is_array(val, len)) {
				spa_log_error(impl->log, "%s expects an array", key);
				return -EINVAL;
			}
			spa_json_enter(&it[0], &ovolumes);
			povolumes = &ovolumes;
		} else {
			spa_log_warn(impl->log, "unexpected graph key '%s'", key);
		}
	}
	if (pnodes == NULL) {
		spa_log_error(impl->log, "filter.graph is missing a nodes array");
		return -EINVAL;
	}
	while (spa_json_enter_object(pnodes, &it[1]) > 0) {
		if ((res = load_node(graph, &it[1])) < 0)
			return res;
	}
	if (plinks != NULL) {
		while (spa_json_enter_object(plinks, &it[1]) > 0) {
			if ((res = parse_link(graph, &it[1])) < 0)
				return res;
		}
	}
	if (pivolumes != NULL) {
		while (spa_json_enter_object(pivolumes, &it[1]) > 0) {
			if ((res = parse_volume(graph, &it[1], SPA_DIRECTION_INPUT)) < 0)
				return res;
		}
	}
	if (povolumes != NULL) {
		while (spa_json_enter_object(povolumes, &it[1]) > 0) {
			if ((res = parse_volume(graph, &it[1], SPA_DIRECTION_OUTPUT)) < 0)
				return res;
		}
	}
	if (pinputs != NULL) {
		graph->n_input_names = count_array(pinputs);
		graph->input_names = calloc(graph->n_input_names, sizeof(char *));
		graph->n_input_names = 0;
		while (spa_json_get_string(pinputs, key, sizeof(key)) > 0)
			graph->input_names[graph->n_input_names++] = strdup(key);
	}
	if (poutputs != NULL) {
		graph->n_output_names = count_array(poutputs);
		graph->output_names = calloc(graph->n_output_names, sizeof(char *));
		graph->n_output_names = 0;
		while (spa_json_get_string(poutputs, key, sizeof(key)) > 0)
			graph->output_names[graph->n_output_names++] = strdup(key);
	}
	return 0;
}

static void graph_free(struct graph *graph)
{
	struct link *link;
	struct node *node;
	uint32_t i;

	unsetup_graph(graph);

	spa_list_consume(link, &graph->link_list, link)
		link_free(link);
	spa_list_consume(node, &graph->node_list, link)
		node_free(node);
	for (i = 0; i < graph->n_input_names; i++)
		free(graph->input_names[i]);
	free(graph->input_names);
	for (i = 0; i < graph->n_output_names; i++)
		free(graph->output_names[i]);
	free(graph->output_names);
}

static const struct spa_filter_graph_methods impl_filter_graph = {
	SPA_VERSION_FILTER_GRAPH_METHODS,
	.add_listener = impl_add_listener,
	.enum_prop_info = impl_enum_prop_info,
	.get_props = impl_get_props,
	.set_props = impl_set_props,
	.activate = impl_activate,
	.deactivate = impl_deactivate,
	.reset = impl_reset,
	.process = impl_process,
};

static int impl_get_interface(struct spa_handle *handle, const char *type, void **interface)
{
	struct impl *this;

	spa_return_val_if_fail(handle != NULL, -EINVAL);
	spa_return_val_if_fail(interface != NULL, -EINVAL);

	this = (struct impl *) handle;

	if (spa_streq(type, SPA_TYPE_INTERFACE_FilterGraph))
		*interface = &this->filter_graph;
	else
		return -ENOENT;

	return 0;
}

static int impl_clear(struct spa_handle *handle)
{
	struct impl *impl = (struct impl *) handle;

	graph_free(&impl->graph);

	if (impl->dsp)
		spa_fga_dsp_free(impl->dsp);

	free(impl->silence_data);
	free(impl->discard_data);
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
	struct impl *impl;
	uint32_t i;
	int res;

	handle->get_interface = impl_get_interface;
	handle->clear = impl_clear;

	impl = (struct impl *) handle;
	impl->graph.impl = impl;

	impl->log = spa_support_find(support, n_support, SPA_TYPE_INTERFACE_Log);
	spa_log_topic_init(impl->log, &log_topic);

	impl->cpu = spa_support_find(support, n_support, SPA_TYPE_INTERFACE_CPU);
	impl->max_align = spa_cpu_get_max_align(impl->cpu);

	impl->dsp = spa_fga_dsp_new(impl->cpu ? spa_cpu_get_flags(impl->cpu) : 0);

	impl->loader = spa_support_find(support, n_support, SPA_TYPE_INTERFACE_PluginLoader);

	spa_list_init(&impl->plugin_list);

	for (i = 0; info && i < info->n_items; i++) {
		const char *k = info->items[i].key;
		const char *s = info->items[i].value;
		if (spa_streq(k, "clock.quantum-limit"))
			spa_atou32(s, &impl->quantum_limit, 0);
		if (spa_streq(k, "filter-graph.n_inputs"))
			spa_atou32(s, &impl->info.n_inputs, 0);
		if (spa_streq(k, "filter-graph.n_outputs"))
			spa_atou32(s, &impl->info.n_outputs, 0);
	}
	if (impl->quantum_limit == 0)
		return -EINVAL;

	impl->silence_data = calloc(impl->quantum_limit, sizeof(float));
	if (impl->silence_data == NULL) {
		res = -errno;
		goto error;
	}

	impl->discard_data = calloc(impl->quantum_limit, sizeof(float));
	if (impl->discard_data == NULL) {
		res = -errno;
		goto error;
	}

	if ((res = load_graph(&impl->graph, info)) < 0) {
		spa_log_error(impl->log, "can't load graph: %s", spa_strerror(res));
		goto error;
	}

	impl->filter_graph.iface = SPA_INTERFACE_INIT(
			SPA_TYPE_INTERFACE_FilterGraph,
			SPA_VERSION_FILTER_GRAPH,
			&impl_filter_graph, impl);
	spa_hook_list_init(&impl->hooks);

	return 0;
error:
	free(impl->silence_data);
	free(impl->discard_data);
	return res;
}

static const struct spa_interface_info impl_interfaces[] = {
	{SPA_TYPE_INTERFACE_FilterGraph,},
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

static struct spa_handle_factory spa_filter_graph_factory = {
	SPA_VERSION_HANDLE_FACTORY,
	"filter.graph",
	NULL,
	impl_get_size,
	impl_init,
	impl_enum_interface_info,
};

SPA_EXPORT
int spa_handle_factory_enum(const struct spa_handle_factory **factory, uint32_t *index)
{
	spa_return_val_if_fail(factory != NULL, -EINVAL);
	spa_return_val_if_fail(index != NULL, -EINVAL);

	switch (*index) {
	case 0:
		*factory = &spa_filter_graph_factory;
		break;
	default:
		return 0;
	}
	(*index)++;
	return 1;
}
