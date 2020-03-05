/* PipeWire
 *
 * Copyright © 2020 Wim Taymans
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice (including the next
 * paragraph) shall be included in all copies or substantial portions of the
 * Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
 * DEALINGS IN THE SOFTWARE.
 */

#include <stdio.h>
#include <signal.h>
#include <float.h>

#include <spa/utils/result.h>
#include <spa/debug/pod.h>
#include <spa/debug/format.h>
#include <spa/debug/types.h>

#include <pipewire/pipewire.h>

#include "ot.h"
#include "json.h"

#define NAME "dump"

struct param {
	struct spa_list link;
	uint32_t id;
	int seq;
	struct spa_pod *param;
};

struct data {
	struct pw_main_loop *loop;
	struct pw_context *context;

	struct pw_core *core;
	struct spa_hook core_listener;

	struct pw_registry *registry;
	struct spa_hook registry_listener;

	int pending_seq;
	struct spa_list proxy_list;
	uint32_t level;
};

struct object_info {
	const char *type;
	uint32_t version;
	const void *events;
	void (*destroy) (void *object);
	void (*enter) (void *object, const char *k, struct ot_node *node);
};

struct proxy_data {
	struct spa_list link;
	struct data *data;

	struct pw_proxy *proxy;

	uint32_t id;
	uint32_t permissions;
	const struct object_info *object_info;
	uint32_t version;
	struct pw_properties *props;

	void *info;

	struct spa_hook proxy_listener;
	struct spa_hook object_listener;

	struct spa_list param_list;
};

static void core_sync(struct data *d)
{
	d->pending_seq = pw_core_sync(d->core, PW_ID_CORE, d->pending_seq);
}

static uint32_t clear_params(struct spa_list *param_list, uint32_t id)
{
	struct param *p, *t;
	uint32_t count = 0;

	spa_list_for_each_safe(p, t, param_list, link) {
		if (id == SPA_ID_INVALID || p->id == id) {
			spa_list_remove(&p->link);
			free(p);
			count++;
		}
	}
	return count;
}

static struct param *add_param(struct spa_list *param_list,
		uint32_t id, const struct spa_pod *param)
{
	struct param *p;

	if (param == NULL || !spa_pod_is_object(param)) {
		errno = EINVAL;
		return NULL;
	}
	if (id == SPA_ID_INVALID)
		id = SPA_POD_OBJECT_ID(param);

	p = malloc(sizeof(struct param) + SPA_POD_SIZE(param));
	if (p == NULL)
		return NULL;

	p->id = id;
	p->param = SPA_MEMBER(p, sizeof(struct param), struct spa_pod);
	memcpy(p->param, param, SPA_POD_SIZE(param));

	spa_list_append(param_list, &p->link);

	return p;
}

/**
 * Mask
 */
struct mask_table {
	uint32_t mask;
	const char *value;
};

static int ot_mask_iterate(struct ot_node *node, struct ot_node *sub)
{
	const struct mask_table *tab = node->extra[1].cp;
	uint32_t i;

	for (i = node->extra[0].i; tab[i].value; i++) {
		if (!(tab[i].mask & node->extra[2].l))
			continue;

		ot_set_string(sub, NULL, tab[i].value);
		node->extra[0].i = i + 1;
		return 1;
	}
	return 0;
}

static inline void ot_set_mask(struct ot_node *node, const char *k, int64_t mask, const struct mask_table *table)
{
	ot_set_array(node, k, ot_mask_iterate);
	node->flags = NODE_FLAG_FLAT;
	node->extra[0].i = 0;
	node->extra[1].cp = table;
	node->extra[2].l = mask;
}

/**
 * Props
 */
static int ot_props_iterate(struct ot_node *node, struct ot_node *sub)
{
	uint32_t current = node->extra[0].i;
	const struct spa_dict *props = node->extra[1].p;
	const char *key, *val;
        char *end;
	long long ll;
	double d;

	if (props == NULL || current >= props->n_items)
		return 0;

	key = props->items[current].key;
	val = props->items[current].value;

	node->extra[0].i++;

	if (val == NULL) {
		ot_set_null(sub, key);
	} else if (strcmp(val, "true") == 0) {
		ot_set_bool(sub, key, true);
	} else if (strcmp(val, "false") == 0) {
		ot_set_bool(sub, key, false);
	} else {
		ll = strtoll(val, &end, 10);
		if (*end == '\0') {
	                if (ll < INT32_MIN || ll > INT32_MAX) {
				ot_set_long(sub, key, ll);
			} else {
				ot_set_int(sub, key, ll);
			}
			return 1;
		}
	}
        d = strtod(val, &end);
	if (*end == '\0') {
		ot_set_double(sub, key, d);
		return 1;
	}
	ot_set_string(sub, key, val);
	return 1;
}

static inline void ot_set_props(struct ot_node *node, const char *k, struct spa_dict *props)
{
	ot_set_object(node, k, ot_props_iterate);
	node->extra[0].i = 0;
	node->extra[1].p = props;
}

/**
 * POD
 */
static int ot_pod_set_value(struct ot_node *node, const struct spa_type_info *info,
		const char *k, uint32_t type, void *body, uint32_t size);

/* pod rectangle */
static int ot_pod_rectangle_iterate(struct ot_node *node, struct ot_node *sub)
{
	const struct spa_rectangle *r = node->extra[1].p;
	switch (node->extra[3].i++) {
	case 0:
		ot_set_int(sub, "width", r->width);
		break;
	case 1:
		ot_set_int(sub, "height", r->height);
		break;
	default:
		return 0;
	}
	return 1;
}

static inline void ot_set_pod_rectangle(struct ot_node *node, const char *k,
		uint32_t type, void *body, uint32_t size)
{
	ot_set_object(node, k, ot_pod_rectangle_iterate);
	node->flags = NODE_FLAG_FLAT;
	node->extra[0].i = type;
	node->extra[1].p = body;
	node->extra[2].i = size;
	node->extra[3].i = 0;
}

/* pod fraction */
static int ot_pod_fraction_iterate(struct ot_node *node, struct ot_node *sub)
{
	const struct spa_fraction *f = node->extra[1].p;
	switch (node->extra[3].i++) {
	case 0:
		ot_set_int(sub, "num", f->num);
		break;
	case 1:
		ot_set_int(sub, "denom", f->denom);
		break;
	default:
		return 0;
	}
	return 1;
}

static inline void ot_set_pod_fraction(struct ot_node *node, const char *k,
		uint32_t type, void *body, uint32_t size)
{
	ot_set_object(node, k, ot_pod_fraction_iterate);
	node->flags = NODE_FLAG_FLAT;
	node->extra[0].i = type;
	node->extra[1].p = body;
	node->extra[2].i = size;
	node->extra[3].i = 0;
}

/* pod choice */
static int ot_pod_choice_iterate(struct ot_node *node, struct ot_node *sub)
{
	void *current = (void*)node->extra[1].p;
	uint32_t idx = node->extra[6].i;
	const char *label;
	static const char *range_labels[] = { "default", "min", "max" };
	static const char *step_labels[] = { "default", "min", "max", "step" };

	if (current >= node->extra[2].p)
		return 0;

	switch (node->extra[0].i) {
	case SPA_CHOICE_Range:
		if (idx >= SPA_N_ELEMENTS(range_labels))
			return 0;
		label = range_labels[idx];
		break;
	case SPA_CHOICE_Step:
		if (idx >= SPA_N_ELEMENTS(step_labels))
			return 0;
		label = step_labels[idx];
		break;
	case SPA_CHOICE_Enum:
		if (idx == 0) {
			label = "default";
		} else {
			snprintf(node->buffer, sizeof(node->buffer), "alt%u", idx);
			label = node->buffer;
		}
		break;
	case SPA_CHOICE_Flags:
		if (idx == 0) {
			label = "default";
		} else {
			snprintf(node->buffer, sizeof(node->buffer), "flag%u", idx);
			label = node->buffer;
		}
		break;
	default:
		return 0;
	}

	node->extra[6].i++;
	node->extra[1].p = SPA_MEMBER(node->extra[1].p, node->extra[4].i, void);

	return ot_pod_set_value(sub, node->extra[5].cp, label,
			node->extra[3].i, current, node->extra[4].i);
}

static inline void ot_set_pod_choice(struct ot_node *node, const struct spa_type_info *info,
	const char *k, uint32_t type, void *body, uint32_t size)
{
	const struct spa_pod_choice_body *b = body;

	if (b->type == SPA_CHOICE_None) {
		ot_pod_set_value(node, info, k,
				b->child.type,
				SPA_POD_CONTENTS(struct spa_pod, &b->child),
				b->child.size);
	} else {
		ot_set_object(node, k, ot_pod_choice_iterate);
		node->extra[0].i = b->type;
		node->extra[1].p = SPA_MEMBER(b, sizeof(struct spa_pod_choice_body), void);
		node->extra[2].p = SPA_MEMBER(b, size, void);
		node->extra[3].i = b->child.type;
		node->extra[4].i = b->child.size;
		node->extra[5].cp = info;
		node->extra[6].i = 0;
	}
}

/* pod object */
static int ot_pod_object_iterate(struct ot_node *node, struct ot_node *sub)
{
	void *body = (void*)node->extra[1].p;
	uint32_t size = node->extra[2].i;
	const struct spa_type_info *info = node->extra[3].cp, *ii;
	const struct spa_pod_prop *p = node->extra[4].p;

	if (!spa_pod_prop_is_inside(body, size, p))
		return 0;

	ii = spa_debug_type_find(info, p->key);

	node->extra[4].p = spa_pod_prop_next(node->extra[4].p);

	return ot_pod_set_value(sub, ii ? ii->values : NULL,
			ii ? spa_debug_type_short_name(ii->name) : "unknown",
			p->value.type,
			SPA_POD_CONTENTS(struct spa_pod_prop, p),
			p->value.size);
}

static inline void ot_set_pod_object(struct ot_node *node, const char *k,
		uint32_t type, void *body, uint32_t size)
{
	struct spa_pod_object_body *b = (struct spa_pod_object_body *)body;
	const struct spa_type_info *ti, *ii;

	ti = spa_debug_type_find(NULL, b->type);
	ii = ti ? spa_debug_type_find(ti->values, 0) : NULL;
	ii = ii ? spa_debug_type_find(ii->values, b->id) : NULL;

	ot_set_object(node, k, ot_pod_object_iterate);
	node->extra[0].i = type;
	node->extra[1].p = body;
	node->extra[2].i = size;
	node->extra[3].cp = ti ? ti->values : NULL;
	node->extra[4].p = spa_pod_prop_first(body);
}

/* pod array */
static int ot_pod_array_iterate(struct ot_node *node, struct ot_node *sub)
{
	void *current = (void*)node->extra[1].p;

	if (current >= node->extra[2].p)
		return 0;

	node->extra[6].i++;
	node->extra[1].p = SPA_MEMBER(current, node->extra[4].i, void);

	return ot_pod_set_value(sub, node->extra[5].cp, NULL,
			node->extra[3].i, current, node->extra[4].i);
}

static inline void ot_set_pod_array(struct ot_node *node, const struct spa_type_info *info,
		const char *k, uint32_t type, void *body, uint32_t size)
{
	const struct spa_pod_array_body *b = body;

	ot_set_array(node, k, ot_pod_array_iterate);
	node->flags = b->child.type < SPA_TYPE_Bitmap ? NODE_FLAG_FLAT : 0;
	node->extra[1].p = SPA_MEMBER(b, sizeof(struct spa_pod_array_body), void);
	node->extra[2].p = SPA_MEMBER(b, size, void);
	node->extra[3].i = b->child.type;
	node->extra[4].i = b->child.size;
	node->extra[5].cp = info;
	node->extra[6].i = 0;
}

/* pod */
static int ot_pod_set_value(struct ot_node *node, const struct spa_type_info *info,
		const char *k, uint32_t type, void *body, uint32_t size)
{
	switch (type) {
	case SPA_TYPE_Bool:
		ot_set_bool(node, k, *(int32_t*)body ? true : false);
		break;
	case SPA_TYPE_Id:
		ot_set_string(node, k, spa_debug_type_find_short_name(info, *(int32_t*)body));
		break;
	case SPA_TYPE_Int:
		ot_set_int(node, k, *(int32_t*)body);
		break;
	case SPA_TYPE_Long:
		ot_set_long(node, k, *(int64_t*)body);
		break;
	case SPA_TYPE_Float:
		ot_set_double(node, k, *(float*)body);
		break;
	case SPA_TYPE_Double:
		ot_set_double(node, k, *(double*)body);
		break;
	case SPA_TYPE_String:
		ot_set_string(node, k, (char*)body);
		break;
	case SPA_TYPE_Fd:
		ot_set_int(node, k, *(int*)body);
		break;
	case SPA_TYPE_Pointer:
		ot_set_null(node, k);
		break;
	case SPA_TYPE_Rectangle:
		ot_set_pod_rectangle(node, k, type, body, size);
		break;
	case SPA_TYPE_Fraction:
		ot_set_pod_fraction(node, k, type, body, size);
		break;
	case SPA_TYPE_Bitmap:
		ot_set_null(node, k);
		break;
	case SPA_TYPE_Array:
		ot_set_pod_array(node, info, k, type, body, size);
		break;
	case SPA_TYPE_Choice:
		ot_set_pod_choice(node, info, k, type, body, size);
		break;
	case SPA_TYPE_Struct:
		ot_set_null(node, k);
		break;
	case SPA_TYPE_Object:
		ot_set_pod_object(node, k, type, body, size);
		break;
	case SPA_TYPE_Sequence:
		ot_set_null(node, k);
		break;
	case SPA_TYPE_Bytes:
		ot_set_null(node, k);
		break;
	case SPA_TYPE_None:
		ot_set_null(node, k);
		break;
	default:
		ot_set_null(node, k);
		return 0;
	}
	return 1;
}

static inline void ot_set_pod(struct ot_node *node, const char *k,
		struct spa_pod *pod)
{
	ot_pod_set_value(node, SPA_TYPE_ROOT, k,
                        SPA_POD_TYPE(pod),
                        SPA_POD_BODY(pod),
                        SPA_POD_BODY_SIZE(pod));
}

/**
 * Param list
 */
static int ot_param_list_iterate(struct ot_node *node, struct ot_node *sub)
{
	const struct spa_list *param_list = node->extra[1].cp;
	const struct param *p;
	uint32_t id = node->extra[2].i;

	do {
		p = node->extra[0].cp;

		if (spa_list_is_end(p, param_list, link))
			return 0;

		node->extra[0].cp = spa_list_next(p, link);
	} while (p->id != id);

	ot_set_pod(sub, NULL, p->param);
	return 1;
}

/**
 * ParamInfo
 */
static int ot_param_info_iterate(struct ot_node *node, struct ot_node *sub)
{
	const struct spa_list *list = node->extra[3].cp;
	uint32_t current = node->extra[0].i;
	const struct spa_param_info *params = node->extra[1].p;
	uint32_t n_params = node->extra[2].i;

	if (current >= n_params)
		return 0;

	ot_set_array(sub, spa_debug_type_find_short_name(spa_type_param, params[current].id),
			ot_param_list_iterate);
	sub->extra[0].p = spa_list_first(list, struct param, link);
	sub->extra[1].cp = list;
	sub->extra[2].i = params[current].id;
	sub->extra[3].p = node->extra[3].p;

	node->extra[0].i++;
	return 1;
}

static inline void ot_set_param_info(struct ot_node *node, const char *k,
		struct spa_param_info *params, uint32_t n_params, const struct spa_list *list)
{
	ot_set_object(node, k, &ot_param_info_iterate);
	node->extra[0].i = 0;
	node->extra[1].p = params;
	node->extra[2].i = n_params;
	node->extra[3].cp = list;
}

/**
 * Core
 */
static void core_event_info(void *object, const struct pw_core_info *info)
{
	struct proxy_data *data = object;
	pw_log_debug(NAME" %p: core %d info", data, data->id);
	data->info = pw_core_info_update(data->info, info);
	core_sync(data->data);
}

static const struct pw_core_events proxy_core_events = {
	PW_VERSION_CORE,
	.info = core_event_info,
};

static void core_destroy(void *object)
{
	struct proxy_data *data = object;
	if (data->info)
		pw_core_info_free(data->info);
}

static int ot_core_iterate(struct ot_node *node, struct ot_node *sub)
{
	struct pw_core_info *i = node->extra[1].p;
	static const struct mask_table change_mask[] = {
		{ PW_CORE_CHANGE_MASK_PROPS, "props" },
		{ 0, NULL }
	};

	switch(node->extra[0].i++) {
	case 0:
		ot_set_int(sub, "id", i->id);
		break;
	case 1:
		ot_set_int(sub, "cookie", i->cookie);
		break;
	case 2:
		ot_set_string(sub, "user-name", i->user_name);
		break;
	case 3:
		ot_set_string(sub, "host-name", i->host_name);
		break;
	case 4:
		ot_set_string(sub, "version", i->version);
		break;
	case 5:
		ot_set_string(sub, "name", i->name);
		break;
	case 6:
		ot_set_mask(sub, "change-mask", i->change_mask, change_mask);
		break;
	case 7:
		ot_set_props(sub, "props", i->props);
		break;
	default:
		return 0;
	}
	return 1;
}

static void ot_core_info_enter(void *data, const char *k, struct ot_node *node)
{
	struct proxy_data *pd = data;
	ot_set_object(node, k, &ot_core_iterate);
	node->extra[0].i = 0;
	node->extra[1].p = pd->info;
}

static const struct object_info core_info = {
	.type = PW_TYPE_INTERFACE_Core,
	.version = PW_VERSION_CORE,
	.events = &proxy_core_events,
	.destroy = core_destroy,
	.enter = &ot_core_info_enter,
};

/**
 * Module
 */
static void module_event_info(void *object, const struct pw_module_info *info)
{
	struct proxy_data *data = object;

	pw_log_debug(NAME" %p: module %d info", data, data->id);
	data->info = pw_module_info_update(data->info, info);
	core_sync(data->data);
}

static const struct pw_module_events module_events = {
	PW_VERSION_MODULE_EVENTS,
	.info = module_event_info,
};

static void module_destroy(void *object)
{
	struct proxy_data *data = object;
	if (data->info)
		pw_module_info_free(data->info);
}

static int ot_module_iterate(struct ot_node *node, struct ot_node *sub)
{
	struct pw_module_info *i = node->extra[1].p;
	static const struct mask_table change_mask[] = {
		{ PW_MODULE_CHANGE_MASK_PROPS, "props" },
		{ 0, NULL }
	};

	switch(node->extra[0].i++) {
	case 0:
		ot_set_int(sub, "id", i->id);
		break;
	case 1:
		ot_set_string(sub, "name", i->name);
		break;
	case 2:
		ot_set_string(sub, "filename", i->filename);
		break;
	case 3:
		ot_set_string(sub, "args", i->args);
		break;
	case 4:
		ot_set_mask(sub, "change-mask", i->change_mask, change_mask);
		break;
	case 5:
		ot_set_props(sub, "props", i->props);
		break;
	default:
		return 0;
	}
	return 1;
}

static void ot_module_info_enter(void *data, const char *k, struct ot_node *node)
{
	struct proxy_data *pd = data;
	ot_set_object(node, k, ot_module_iterate);
	node->extra[0].i = 0;
	node->extra[1].p = pd->info;
}

static const struct object_info module_info = {
	.type = PW_TYPE_INTERFACE_Module,
	.version = PW_VERSION_MODULE,
	.events = &module_events,
	.destroy = module_destroy,
	.enter = &ot_module_info_enter,
};

/**
 * Factory
 */
static void factory_event_info(void *object, const struct pw_factory_info *info)
{
	struct proxy_data *data = object;

	pw_log_debug(NAME" %p: factory %d info", data, data->id);
	data->info = pw_factory_info_update(data->info, info);
	core_sync(data->data);
}

static const struct pw_factory_events factory_events = {
	PW_VERSION_FACTORY_EVENTS,
	.info = factory_event_info,
};

static void factory_destroy(void *object)
{
	struct proxy_data *data = object;
	if (data->info)
		pw_factory_info_free(data->info);
}

static int ot_factory_iterate(struct ot_node *node, struct ot_node *sub)
{
	struct pw_factory_info *i = node->extra[1].p;
	static const struct mask_table change_mask[] = {
		{ PW_FACTORY_CHANGE_MASK_PROPS, "props" },
		{ 0, NULL }
	};

	switch(node->extra[0].i++) {
	case 0:
		ot_set_int(sub, "id", i->id);
		break;
	case 1:
		ot_set_string(sub, "name", i->name);
		break;
	case 2:
		ot_set_string(sub, "type", i->type);
		break;
	case 3:
		ot_set_int(sub, "version", i->version);
		break;
	case 4:
		ot_set_mask(sub, "change-mask", i->change_mask, change_mask);
		break;
	case 5:
		ot_set_props(sub, "props", i->props);
		break;
	default:
		return 0;
	}
	return 1;
}

static void ot_factory_info_enter(void *data, const char *k, struct ot_node *node)
{
	struct proxy_data *pd = data;
	ot_set_object(node, k, &ot_factory_iterate);
	node->extra[0].i = 0;
	node->extra[1].p = pd->info;
}

static const struct object_info factory_info = {
	.type = PW_TYPE_INTERFACE_Factory,
	.version = PW_VERSION_FACTORY,
	.events = &factory_events,
	.destroy = factory_destroy,
	.enter = &ot_factory_info_enter,
};

/**
 * Clients
 */
static void client_event_info(void *object, const struct pw_client_info *info)
{
	struct proxy_data *data = object;

	pw_log_debug(NAME" %p: client %d info", data, data->id);
	data->info = pw_client_info_update(data->info, info);
	core_sync(data->data);
}

static const struct pw_client_events client_events = {
	PW_VERSION_CLIENT_EVENTS,
	.info = client_event_info,
};

static void client_destroy(void *object)
{
	struct proxy_data *data = object;
	if (data->info)
		pw_client_info_free(data->info);
}

static int ot_client_iterate(struct ot_node *node, struct ot_node *sub)
{
	struct pw_client_info *i = node->extra[1].p;
	static const struct mask_table change_mask[] = {
		{ PW_CLIENT_CHANGE_MASK_PROPS, "props" },
		{ 0, NULL }
	};

	switch(node->extra[0].i++) {
	case 0:
		ot_set_int(sub, "id", i->id);
		break;
	case 1:
		ot_set_mask(sub, "change-mask", i->change_mask, change_mask);
		break;
	case 2:
		ot_set_props(sub, "props", i->props);
		break;
	default:
		return 0;
	}
	return 1;
}

static void ot_client_info_enter(void *data, const char *k, struct ot_node *node)
{
	struct proxy_data *pd = data;
	ot_set_object(node, k, &ot_client_iterate);
	node->extra[0].i = 0;
	node->extra[1].p = pd->info;
}

static const struct object_info client_info = {
	.type = PW_TYPE_INTERFACE_Client,
	.version = PW_VERSION_CLIENT,
	.events = &client_events,
	.destroy = client_destroy,
	.enter = &ot_client_info_enter,
};

/**
 * Device
 */
static void device_event_info(void *object, const struct pw_device_info *info)
{
	struct proxy_data *data = object;
	uint32_t i;

	pw_log_debug(NAME" %p: device %d info", data, data->id);
	data->info = pw_device_info_update(data->info, info);

	if (info->change_mask & PW_DEVICE_CHANGE_MASK_PARAMS) {
		for (i = 0; i < info->n_params; i++) {
			if (!(info->params[i].flags & SPA_PARAM_INFO_READ))
				continue;
			clear_params(&data->param_list, info->params[i].id);
			pw_device_enum_params((struct pw_device*)data->proxy,
					1, info->params[i].id, 0, UINT32_MAX, NULL);
		}
	}
	core_sync(data->data);
}

static void device_event_param(void *object, int seq,
		uint32_t id, uint32_t index, uint32_t next,
		const struct spa_pod *param)
{
	struct proxy_data *data = object;
	pw_log_debug(NAME" %p: device %u param %d index:%d", data, data->id, id, index);
	add_param(&data->param_list, id, param);
}

static const struct pw_device_events device_events = {
	PW_VERSION_DEVICE_EVENTS,
	.info = device_event_info,
	.param = device_event_param,
};

static void device_destroy(void *object)
{
	struct proxy_data *data = object;
	if (data->info)
		pw_device_info_free(data->info);
}

static int ot_device_iterate(struct ot_node *node, struct ot_node *sub)
{
	struct pw_device_info *i = node->extra[1].p;
	struct spa_list *param_list = node->extra[2].p;
	static const struct mask_table change_mask[] = {
		{ PW_DEVICE_CHANGE_MASK_PROPS, "props" },
		{ PW_DEVICE_CHANGE_MASK_PARAMS, "params" },
		{ 0, NULL }
	};

	switch(node->extra[0].i++) {
	case 0:
		ot_set_int(sub, "id", i->id);
		break;
	case 1:
		ot_set_mask(sub, "change-mask", i->change_mask, change_mask);
		break;
	case 2:
		ot_set_props(sub, "props", i->props);
		break;
	case 3:
		ot_set_param_info(sub, "params", i->params, i->n_params, param_list);
		break;
	default:
		return 0;
	}
	return 1;
}

static void ot_device_info_enter(void *data, const char *k, struct ot_node *node)
{
	struct proxy_data *pd = data;
	ot_set_object(node, k, &ot_device_iterate);
	node->extra[0].i = 0;
	node->extra[1].p = pd->info;
	node->extra[2].p = &pd->param_list;
}

static const struct object_info device_info = {
	.type = PW_TYPE_INTERFACE_Device,
	.version = PW_VERSION_DEVICE,
	.events = &device_events,
	.destroy = device_destroy,
	.enter = &ot_device_info_enter
};

/**
 * Node
 */
static void node_event_info(void *object, const struct pw_node_info *info)
{
	struct proxy_data *data = object;
	uint32_t i;

	pw_log_debug(NAME" %p: node %d info", data, data->id);
	data->info = pw_node_info_update(data->info, info);

	if (info->change_mask & PW_NODE_CHANGE_MASK_PARAMS) {
		for (i = 0; i < info->n_params; i++) {
			if (!(info->params[i].flags & SPA_PARAM_INFO_READ))
				continue;
			clear_params(&data->param_list, info->params[i].id);
			pw_node_enum_params((struct pw_node*)data->proxy,
					1, info->params[i].id, 0, UINT32_MAX, NULL);
		}
	}
	core_sync(data->data);
}

static void node_event_param(void *object, int seq,
		uint32_t id, uint32_t index, uint32_t next,
		const struct spa_pod *param)
{
	struct proxy_data *data = object;
	pw_log_debug(NAME" %p: node %u param %d index:%d", data, data->id, id, index);
	add_param(&data->param_list, id, param);
}

static const struct pw_node_events node_events = {
	PW_VERSION_NODE_EVENTS,
	.info = node_event_info,
	.param = node_event_param,
};

static void node_destroy(void *object)
{
	struct proxy_data *data = object;
	if (data->info)
		pw_node_info_free(data->info);
}

static int ot_node_info_iterate(struct ot_node *node, struct ot_node *sub)
{
	struct pw_node_info *i = node->extra[1].p;
	struct spa_list *param_list = node->extra[2].p;
	static const struct mask_table change_mask[] = {
		{ PW_NODE_CHANGE_MASK_INPUT_PORTS, "n-input-ports" },
		{ PW_NODE_CHANGE_MASK_OUTPUT_PORTS, "n-output-ports" },
		{ PW_NODE_CHANGE_MASK_STATE, "state" },
		{ PW_NODE_CHANGE_MASK_PROPS, "props" },
		{ PW_NODE_CHANGE_MASK_PARAMS, "params" },
		{ 0, NULL }
	};

	switch(node->extra[0].i++) {
	case 0:
		ot_set_int(sub, "id", i->id);
		break;
	case 1:
		ot_set_int(sub, "max-input-ports", i->max_input_ports);
		break;
	case 2:
		ot_set_int(sub, "max-output-ports", i->max_output_ports);
		break;
	case 3:
		ot_set_mask(sub, "change-mask", i->change_mask, change_mask);
		break;
	case 4:
		ot_set_int(sub, "n-input-ports", i->n_input_ports);
		break;
	case 5:
		ot_set_int(sub, "n-output-ports", i->n_output_ports);
		break;
	case 6:
		ot_set_string(sub, "state", pw_node_state_as_string(i->state));
		break;
	case 7:
		ot_set_string(sub, "error", i->error);
		break;
	case 8:
		ot_set_props(sub, "props", i->props);
		break;
	case 9:
		ot_set_param_info(sub, "params", i->params, i->n_params, param_list);
		break;
	default:
		return 0;
	}
	return 1;
}

static void ot_node_info_enter(void *data, const char *k, struct ot_node *node)
{
	struct proxy_data *pd = data;
	ot_set_object(node, k, &ot_node_info_iterate);
	node->extra[0].i = 0;
	node->extra[1].p = pd->info;
	node->extra[2].p = &pd->param_list;
}

static const struct object_info node_info = {
	.type = PW_TYPE_INTERFACE_Node,
	.version = PW_VERSION_NODE,
	.events = &node_events,
	.destroy = node_destroy,
	.enter = &ot_node_info_enter
};

/**
 * Port
 */
static void port_event_info(void *object, const struct pw_port_info *info)
{
	struct proxy_data *data = object;
	uint32_t i;

	pw_log_debug(NAME" %p: port %u info", data, data->id);
	data->info = pw_port_info_update(data->info, info);

	if (info->change_mask & PW_PORT_CHANGE_MASK_PARAMS) {
		for (i = 0; i < info->n_params; i++) {
			if (!(info->params[i].flags & SPA_PARAM_INFO_READ))
				continue;
			clear_params(&data->param_list, info->params[i].id);
			pw_port_enum_params((struct pw_port*)data->proxy,
					1, info->params[i].id, 0, UINT32_MAX, NULL);
		}
	}
	core_sync(data->data);
}

static void port_event_param(void *object, int seq,
		uint32_t id, uint32_t index, uint32_t next,
		const struct spa_pod *param)
{
	struct proxy_data *data = object;
	pw_log_debug(NAME" %p: port %u param %d index:%d", data, data->id, id, index);
	add_param(&data->param_list, id, param);
}

static const struct pw_port_events port_events = {
	PW_VERSION_PORT_EVENTS,
	.info = port_event_info,
	.param = port_event_param,
};

static void port_destroy(void *object)
{
	struct proxy_data *data = object;
	if (data->info)
		pw_port_info_free(data->info);
}


static int ot_port_iterate(struct ot_node *node, struct ot_node *sub)
{
	struct pw_port_info *i = node->extra[1].p;
	struct spa_list *param_list = node->extra[2].p;
	static const struct mask_table change_mask[] = {
		{ PW_PORT_CHANGE_MASK_PROPS, "props" },
		{ PW_PORT_CHANGE_MASK_PARAMS, "params" },
		{ 0, NULL }
	};

	switch(node->extra[0].i++) {
	case 0:
		ot_set_int(sub, "id", i->id);
		break;
	case 1:
		ot_set_string(sub, "direction", pw_direction_as_string(i->direction));
		break;
	case 2:
		ot_set_mask(sub, "change-mask", i->change_mask, change_mask);
		break;
	case 3:
		ot_set_props(sub, "props", i->props);
		break;
	case 4:
		ot_set_param_info(sub, "params", i->params, i->n_params, param_list);
		break;
	default:
		return 0;
	}
	return 1;
}

static void ot_port_info_enter(void *data, const char *k, struct ot_node *node)
{
	struct proxy_data *pd = data;
	ot_set_object(node, k, &ot_port_iterate);
	node->extra[0].i = 0;
	node->extra[1].p = pd->info;
	node->extra[2].p = &pd->param_list;
}

static const struct object_info port_info = {
	.type = PW_TYPE_INTERFACE_Port,
	.version = PW_VERSION_PORT,
	.events = &port_events,
	.destroy = port_destroy,
	.enter = &ot_port_info_enter
};

/**
 * Link
 */
static void link_event_info(void *object, const struct pw_link_info *info)
{
	struct proxy_data *data = object;
	pw_log_debug(NAME" %p: link %u info", data, data->id);
	data->info = pw_link_info_update(data->info, info);
	core_sync(data->data);
}

static const struct pw_link_events link_events = {
	PW_VERSION_LINK_EVENTS,
	.info = link_event_info,
};

static void link_destroy(void *object)
{
	struct proxy_data *data = object;
	if (data->info)
		pw_link_info_free(data->info);
}

static int ot_link_iterate(struct ot_node *node, struct ot_node *sub)
{
	struct pw_link_info *i = node->extra[1].p;
	static const struct mask_table change_mask[] = {
		{ PW_LINK_CHANGE_MASK_STATE, "state" },
		{ PW_LINK_CHANGE_MASK_FORMAT, "format" },
		{ PW_LINK_CHANGE_MASK_PROPS, "props" },
		{ 0, NULL }
	};

	switch(node->extra[0].i++) {
	case 0:
		ot_set_int(sub, "id", i->id);
		break;
	case 1:
		ot_set_int(sub, "output-node-id", i->output_node_id);
		break;
	case 2:
		ot_set_int(sub, "output-port-id", i->output_port_id);
		break;
	case 3:
		ot_set_int(sub, "input-node-id", i->input_node_id);
		break;
	case 4:
		ot_set_int(sub, "input-port-id", i->input_port_id);
		break;
	case 5:
		ot_set_mask(sub, "change-mask", i->change_mask, change_mask);
		break;
	case 6:
		ot_set_string(sub, "state", pw_link_state_as_string(i->state));
		break;
	case 7:
		ot_set_string(sub, "error", i->error);
		break;
	case 8:
		ot_set_pod(sub, "format", i->format);
		break;
	case 9:
		ot_set_props(sub, "props", i->props);
		break;
	default:
		return 0;
	}
	return 1;
}

static void ot_link_info_enter(void *data, const char *k, struct ot_node *node)
{
	const struct proxy_data *pd = data;
	ot_set_object(node, k, &ot_link_iterate);
	node->extra[0].i = 0;
	node->extra[1].p = pd->info;
}

static const struct object_info link_info = {
	.type = PW_TYPE_INTERFACE_Link,
	.version = PW_VERSION_LINK,
	.events = &link_events,
	.destroy = link_destroy,
	.enter = &ot_link_info_enter,
};

static void on_core_done(void *data, uint32_t id, int seq)
{
	struct data *d = data;

	if (id == PW_ID_CORE && d->pending_seq == seq)
		pw_main_loop_quit(d->loop);
}

static const struct mask_table ot_permission_masks[] = {
	{ PW_PERM_R, "r" },
	{ PW_PERM_W, "w" },
	{ PW_PERM_X, "x" },
	{ 0, NULL }
};

static int ot_global_iterate(struct ot_node *node, struct ot_node *sub)
{
	struct proxy_data *pd = node->extra[1].p;

	switch(node->extra[0].i++) {
	case 0:
		ot_set_int(sub, "id", pd->id);
		break;
	case 1:
		ot_set_string(sub, "type", pd->object_info->type);
		break;
	case 2:
		ot_set_int(sub, "version", pd->version);
		break;
	case 3:
		ot_set_mask(sub, "permissions", pd->permissions, ot_permission_masks);
		break;
	case 4:
		ot_set_props(sub, "properties", &pd->props->dict);
		break;
	case 5:
		if (pd->object_info->enter == NULL)
			return 0;
		pd->object_info->enter(pd, "info", sub);
		break;
	default:
		return 0;
	}
	return 1;
}

static void ot_global_enter(struct proxy_data *pd, const char *k, struct ot_node *node)
{
	ot_set_object(node, k, ot_global_iterate);
	node->extra[0].i = 0;
	node->extra[1].p = pd;
}

static int ot_root_iterate(struct ot_node *node, struct ot_node *sub)
{
	struct proxy_data *current = node->extra[0].p;

	if (spa_list_is_end(current, node->extra[1].p, link))
		return 0;

	node->extra[0].p = spa_list_next(current, link);
	ot_global_enter(current, NULL, sub);
	return 1;
}

static inline void ot_root_enter(struct data *data, const char *k, struct ot_node *node)
{
	struct spa_list *list = &data->proxy_list;
	ot_set_array(node, k, ot_root_iterate);
	node->extra[0].p = spa_list_first(list, struct proxy_data, link);
	node->extra[1].p = list;
}

static void
destroy_proxy (void *data)
{
        struct proxy_data *pd = data;

	clear_params(&pd->param_list, SPA_ID_INVALID);

	if (pd->object_info->destroy)
		pd->object_info->destroy(pd->info);

	spa_list_remove(&pd->link);
}

static const struct pw_proxy_events proxy_events = {
	PW_VERSION_PROXY_EVENTS,
	.destroy = destroy_proxy,
};

static const struct object_info *objects[] =
{
	&core_info,
	&module_info,
	&factory_info,
	&client_info,
	&device_info,
	&node_info,
	&port_info,
	&link_info,
};

static const struct object_info *find_info(const char *type, uint32_t version)
{
	size_t i;

	for (i = 0; i < SPA_N_ELEMENTS(objects); i++) {
		if (strcmp(objects[i]->type, type) == 0 &&
		    objects[i]->version <= version)
			return objects[i];
	}
	return NULL;
}

static void registry_event_global(void *data, uint32_t id,
				  uint32_t permissions, const char *type, uint32_t version,
				  const struct spa_dict *props)
{
        struct data *d = data;
        struct pw_proxy *proxy;
	struct proxy_data *pd;
	const struct object_info *info;

	info = find_info(type, version);
	if (info == NULL) {
		pw_log_error("unknown type %s", type);
		return;
	}

        proxy = pw_registry_bind(d->registry, id, type,
				       info->version, sizeof(struct proxy_data));
        if (proxy == NULL)
                goto no_mem;

	pd = pw_proxy_get_user_data(proxy);
	pd->data = d;
	pd->proxy = proxy;
	pd->id = id;
	pd->permissions = permissions;
	pd->version = version;
	pd->object_info = info;
	pd->props = props ? pw_properties_new_dict(props) : NULL;
	spa_list_init(&pd->param_list);

	spa_list_append(&d->proxy_list, &pd->link);

        pw_proxy_add_object_listener(proxy, &pd->object_listener, info->events, pd);
        pw_proxy_add_listener(proxy, &pd->proxy_listener, &proxy_events, pd);
        return;

      no_mem:
        printf("failed to create proxy");
        return;
}

static void registry_event_global_remove(void *object, uint32_t id)
{
	printf("removed:\n");
	printf("\tid: %u\n", id);
}

static const struct pw_registry_events registry_events = {
	PW_VERSION_REGISTRY_EVENTS,
	.global = registry_event_global,
	.global_remove = registry_event_global_remove,
};

static void on_core_error(void *_data, uint32_t id, int seq, int res, const char *message)
{
	struct data *data = _data;

	pw_log_error("error id:%u seq:%d res:%d (%s): %s",
			id, seq, res, spa_strerror(res), message);

	if (id == PW_ID_CORE)
		pw_main_loop_quit(data->loop);
}

static const struct pw_core_events core_events = {
	PW_VERSION_CORE_EVENTS,
	.done = on_core_done,
	.error = on_core_error,
};

static void do_quit(void *data, int signal_number)
{
	struct data *d = data;
	pw_main_loop_quit(d->loop);
}

int main(int argc, char *argv[])
{
	struct data data = { 0 };
	struct pw_loop *l;
	struct pw_properties *props = NULL;
	struct ot_node root;

	pw_init(&argc, &argv);

	data.loop = pw_main_loop_new(NULL);
	if (data.loop == NULL)
		return -1;

	l = pw_main_loop_get_loop(data.loop);
	pw_loop_add_signal(l, SIGINT, do_quit, &data);
	pw_loop_add_signal(l, SIGTERM, do_quit, &data);

	data.context = pw_context_new(l, NULL, 0);
	if (data.context == NULL)
		return -1;

	spa_list_init(&data.proxy_list);

	if (argc > 1)
		props = pw_properties_new(PW_KEY_REMOTE_NAME, argv[1], NULL);

	data.core = pw_context_connect(data.context, props, 0);
	if (data.core == NULL)
		return -1;

	pw_core_add_listener(data.core,
			&data.core_listener,
			&core_events, &data);
	data.registry = pw_core_get_registry(data.core,
			PW_VERSION_REGISTRY, 0);
	pw_registry_add_listener(data.registry,
			&data.registry_listener,
			&registry_events, &data);

	pw_main_loop_run(data.loop);

	ot_root_enter(&data, NULL, &root);
	ot_json_dump(&root, 0);

	pw_context_destroy(data.context);
	pw_main_loop_destroy(data.loop);

	return 0;
}
