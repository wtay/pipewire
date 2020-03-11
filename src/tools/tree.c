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
#include <limits.h>

#include <spa/utils/result.h>
#include <spa/debug/pod.h>
#include <spa/debug/format.h>
#include <spa/debug/types.h>

#include <pipewire/pipewire.h>

#include "ot.h"
#include "tree.h"

#define NAME "tree"

#define pw_tree_emit_added(d,s) spa_hook_list_call(&d->listener_list, struct pw_tree_events, added, 0, s)
#define pw_tree_emit_updated(d,s) spa_hook_list_call(&d->listener_list, struct pw_tree_events, updated, 0, s)
#define pw_tree_emit_removed(d,s) spa_hook_list_call(&d->listener_list, struct pw_tree_events, removed, 0, s)

struct param {
	struct spa_list link;
	uint32_t id;
	struct spa_pod *param;
};

struct object_info {
	const char *type;
	uint32_t version;
	const void *events;
	void (*destroy) (void *object);
	void (*enter) (void *object, const char *k, struct ot_node *node);
	void (*params) (void *object, uint32_t id);
};

struct global {
	struct spa_list link;
	struct data *data;

	uint32_t id;
	uint32_t permissions;
	const char *type;
	uint32_t version;
	const struct object_info *object_info;
	struct pw_properties *props;

	struct pw_proxy *proxy;
	struct spa_hook proxy_listener;
	struct spa_hook object_listener;

	void *info;
	struct spa_list param_list;
};

struct data {
	struct pw_loop *loop;
	struct pw_context *context;

	struct pw_core *core;
	struct spa_hook core_listener;

	struct pw_registry *registry;
	struct spa_hook registry_listener;

	int pending_seq;
	int last_seq;
	struct spa_list global_list;
	uint32_t n_globals;

	struct spa_hook_list listener_list;
};

struct pw_tree {
	struct data data;
};


static void core_sync(struct data *d)
{
	d->pending_seq = pw_core_sync(d->core, PW_ID_CORE, d->pending_seq);
}
static void core_roundtrip(struct data *d)
{
	core_sync(d);
	while (d->last_seq != d->pending_seq)
		pw_loop_iterate(d->loop, -1);
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

static void
destroy_proxy (void *data)
{
        struct global *gl = data;

	clear_params(&gl->param_list, SPA_ID_INVALID);

	if (gl->object_info && gl->object_info->destroy)
		gl->object_info->destroy(gl->info);

	spa_list_remove(&gl->link);
}

static const struct pw_proxy_events proxy_events = {
	PW_VERSION_PROXY_EVENTS,
	.destroy = destroy_proxy,
};

static int global_bind(struct global *gl)
{
	struct data *d = gl->data;

	gl->proxy = pw_registry_bind(d->registry,
			gl->id, gl->type,
			gl->object_info->version, 0);
	if (gl->proxy == NULL)
		return -errno;

        pw_proxy_add_object_listener(gl->proxy,
			&gl->object_listener,
			gl->object_info->events, gl);
        pw_proxy_add_listener(gl->proxy,
			&gl->proxy_listener,
			&proxy_events, gl);

	core_roundtrip(d);
	return 0;
}

static int global_params(struct global *gl, uint32_t id)
{
	struct data *d = gl->data;

	gl->object_info->params(gl, id);

	core_roundtrip(d);
	return 0;
}

#define ot_set_null(n,k)	(*(n) = OT_INIT_NULL(k))
#define ot_set_bool(n,k,v)	(*(n) = OT_INIT_BOOL(k,v))
#define ot_set_int(n,k,v)	(*(n) = OT_INIT_INT(k,v))
#define ot_set_long(n,k,v)	(*(n) = OT_INIT_LONG(k,v))
#define ot_set_float(n,k,v)	(*(n) = OT_INIT_FLOAT(k,v))
#define ot_set_double(n,k,v)	(*(n) = OT_INIT_DOUBLE(k,v))
#define ot_set_string(n,k,v)	(*(n) = OT_INIT_STRING(k,v))
#define ot_set_array(n,k,i)	(*(n) = OT_INIT_ARRAY(k,i))
#define ot_set_object(n,k,i)	(*(n) = OT_INIT_OBJECT(k,i))

static void ot_set_global_info(struct ot_node *node, const char *k, struct global *gl)
{
	if (gl->object_info == NULL ||
	    gl->object_info->enter == NULL) {
		ot_set_null(node, k);
		return;
	}
	if (gl->info == NULL)
		global_bind(gl);
	if (gl->info == NULL) {
		ot_set_null(node, k);
		return;
	}
	gl->object_info->enter(gl, k, node);
	node->flags = NODE_FLAG_EXPENSIVE;
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
	const struct mask_table *tab = node->extra[0].cp;
	uint32_t n_tab = node->extra[1].i;
	uint32_t i;
	int32_t index;

	if ((index = node->index) < 0)
		index += n_tab;

	for (i = index; i >= 0 && i < n_tab; i++) {
		if (!(tab[i].mask & node->extra[2].l))
			continue;

		ot_set_string(sub, NULL, tab[i].value);
		return 1;
	}
	return 0;
}

static inline void ot_set_mask(struct ot_node *node, const char *k, int64_t mask,
		const struct mask_table *table, uint32_t n_table)
{
	ot_set_array(node, k, ot_mask_iterate);
	node->flags = NODE_FLAG_FLAT;
	node->extra[0].cp = table;
	node->extra[1].i = n_table;
	node->extra[2].l = mask;
}

/**
 * Props
 */
static int ot_props_iterate(struct ot_node *node, struct ot_node *sub)
{
	const struct spa_dict *props = node->extra[0].p;
	const char *key, *val;
	char *end;
	int32_t index;
	long long ll;
	double d;

	if ((index = node->index) < 0)
		index += props->n_items;

	if (index < 0 || index >= (int32_t)props->n_items)
		return 0;

	key = props->items[index].key;
	val = props->items[index].value;

	if (val == NULL) {
		ot_set_null(sub, key);
	} else if (strcmp(val, "true") == 0) {
		ot_set_bool(sub, key, true);
	} else if (strcmp(val, "false") == 0) {
		ot_set_bool(sub, key, false);
	} else {
		errno = 0;
		ll = strtoll(val, &end, 10);
		if (*end == '\0' && errno == 0) {
	                if (ll < INT32_MIN || ll > INT32_MAX)
				ot_set_long(sub, key, ll);
			else
				ot_set_int(sub, key, ll);
		} else {
			errno = 0;
			d = strtod(val, &end);
			if (*end == '\0' && errno == 0)
				ot_set_double(sub, key, d);
			else
				ot_set_string(sub, key, val);
		}
	}
	return 1;
}

static inline void ot_set_props(struct ot_node *node, const char *k, struct spa_dict *props)
{
	if (props) {
		ot_set_object(node, k, ot_props_iterate);
		node->extra[0].p = props;
	} else
		ot_set_null(node, k);
}

/**
 * POD
 */
static int ot_pod_set_value(struct ot_node *node, const struct spa_type_info *info,
		const char *k, uint32_t type, void *body, uint32_t size);

/* pod rectangle */
static int ot_pod_rectangle_iterate(struct ot_node *node, struct ot_node *sub)
{
	const struct spa_rectangle *r = node->extra[0].p;
	int32_t index;

	if ((index = node->index) < 0)
		index += 2;

	switch (index) {
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
	node->extra[0].p = body;
}

/* pod fraction */
static int ot_pod_fraction_iterate(struct ot_node *node, struct ot_node *sub)
{
	const struct spa_fraction *f = node->extra[0].p;
	int32_t index;

	if ((index = node->index) < 0)
		index += 2;

	switch (index) {
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
	node->extra[0].p = body;
}

/* pod choice */
static int ot_pod_choice_iterate(struct ot_node *node, struct ot_node *sub)
{
	int32_t index, n_items = node->extra[5].i;
	const char *label;
	static const char *range_labels[] = { "default", "min", "max", NULL };
	static const char *step_labels[] = { "default", "min", "max", "step", NULL };
	static const char *enum_labels[] = { "default", "alt%u" };
	static const char *flags_labels[] = { "default", "flag%u" };

	if ((index = node->index) < 0)
		index += n_items;
	if (index < 0 || index >= n_items)
		return 0;

	switch (node->extra[0].i) {
	case SPA_CHOICE_Range:
		label = range_labels[SPA_CLAMP(index, 0, 3)];
		break;
	case SPA_CHOICE_Step:
		label = step_labels[SPA_CLAMP(index, 0, 4)];
		break;
	case SPA_CHOICE_Enum:
		label = enum_labels[SPA_CLAMP(index, 0, 1)];
		break;
	case SPA_CHOICE_Flags:
		label = flags_labels[SPA_CLAMP(index, 0, 1)];
		break;
	default:
		return 0;
	}
	if (label == NULL)
		return 0;

	snprintf(node->buffer, sizeof(node->buffer), label, index);

	return ot_pod_set_value(sub, node->extra[4].cp, node->buffer,
			node->extra[2].i,
			SPA_MEMBER(node->extra[1].p, index * node->extra[3].i, void),
			node->extra[3].i);
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
		node->extra[2].i = b->child.type;
		node->extra[3].i = b->child.size;
		node->extra[4].cp = info;
		node->extra[5].i = (size - sizeof(struct spa_pod_choice_body)) / b->child.size;
	}
}

/* pod object */
static const struct spa_pod_prop *find_nth_prop(void *body, uint32_t size, uint32_t nth)
{
	const struct spa_pod_prop *p;
	SPA_POD_OBJECT_BODY_FOREACH(body, size, p)
		if (nth-- == 0)
			return p;
	return NULL;
}

static int ot_pod_object_iterate(struct ot_node *node, struct ot_node *sub)
{
	int32_t index, n_items = node->extra[4].i;
	void *body = (void*)node->extra[1].p;
	uint32_t size = node->extra[2].i;
	const struct spa_type_info *info = node->extra[3].cp, *ii;
	const struct spa_pod_prop *p;

	if ((index = node->index) < 0)
		index += n_items;
	if (index < 0 || index >= n_items)
		return 0;
	if ((p = find_nth_prop(body, size, index)) == NULL)
		return 0;

	ii = spa_debug_type_find(info, p->key);

	return ot_pod_set_value(sub, ii ? ii->values : NULL,
			ii ? spa_debug_type_short_name(ii->name) : "*unknown*",
			p->value.type,
			SPA_POD_CONTENTS(struct spa_pod_prop, p),
			p->value.size);
}

static inline void ot_set_pod_object(struct ot_node *node, const char *k,
		uint32_t type, void *body, uint32_t size)
{
	struct spa_pod_object_body *b = (struct spa_pod_object_body *)body;
	const struct spa_type_info *ti, *ii;
	const struct spa_pod_prop *p;

	ti = spa_debug_type_find(NULL, b->type);
	ii = ti ? spa_debug_type_find(ti->values, 0) : NULL;
	ii = ii ? spa_debug_type_find(ii->values, b->id) : NULL;

	ot_set_object(node, k, ot_pod_object_iterate);
	node->extra[0].i = type;
	node->extra[1].p = body;
	node->extra[2].i = size;
	node->extra[3].cp = ti ? ti->values : NULL;
	node->extra[4].i = 0;
	SPA_POD_OBJECT_BODY_FOREACH(body, size, p)
		node->extra[4].i++;
}

/* pod array */
static int ot_pod_array_iterate(struct ot_node *node, struct ot_node *sub)
{
	int32_t index, n_items = node->extra[4].i;

	if ((index = node->index) < 0)
		index += n_items;
	if (index < 0 || index >= n_items)
		return 0;

	return ot_pod_set_value(sub, node->extra[3].cp, NULL,
			node->extra[1].i,
			SPA_MEMBER(node->extra[0].p, index * node->extra[2].i, void),
			node->extra[2].i);
}

static inline void ot_set_pod_array(struct ot_node *node, const struct spa_type_info *info,
		const char *k, uint32_t type, void *body, uint32_t size)
{
	const struct spa_pod_array_body *b = body;

	ot_set_array(node, k, ot_pod_array_iterate);
	node->flags = b->child.type < SPA_TYPE_Bitmap ? NODE_FLAG_FLAT : 0;
	node->extra[0].p = SPA_MEMBER(b, sizeof(struct spa_pod_array_body), void);
	node->extra[1].i = b->child.type;
	node->extra[2].i = b->child.size;
	node->extra[3].cp = info;
	node->extra[4].i = (size - sizeof(struct spa_pod_array_body)) / b->child.size;
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
	case SPA_TYPE_Rectangle:
		ot_set_pod_rectangle(node, k, type, body, size);
		break;
	case SPA_TYPE_Fraction:
		ot_set_pod_fraction(node, k, type, body, size);
		break;
	case SPA_TYPE_Array:
		ot_set_pod_array(node, info, k, type, body, size);
		break;
	case SPA_TYPE_Choice:
		ot_set_pod_choice(node, info, k, type, body, size);
		break;
	case SPA_TYPE_Object:
		ot_set_pod_object(node, k, type, body, size);
		break;
	case SPA_TYPE_Pointer:
	case SPA_TYPE_Bitmap:
	case SPA_TYPE_Struct:
	case SPA_TYPE_Sequence:
	case SPA_TYPE_Bytes:
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
static int32_t count_params(struct spa_list *param_list, uint32_t id)
{
	int32_t count = 0;
	const struct param *p;
	spa_list_for_each(p, param_list, link)
		if (p->id == id)
			count++;
	return count;
}

static const struct param *find_nth_param(struct spa_list *param_list, uint32_t id, int32_t nth)
{
	const struct param *p;
	spa_list_for_each(p, param_list, link)
		if (p->id == id && nth-- == 0)
			return p;
	return NULL;
}

static int ot_param_list_iterate(struct ot_node *node, struct ot_node *sub)
{
	struct global *gl = node->extra[0].p;
	struct spa_list *param_list = &gl->param_list;
	const struct param *p;
	struct spa_param_info *params = node->extra[1].p;
	uint32_t current = node->extra[2].i;
	uint32_t id = params[current].id;
	int32_t index, n_items;

	if (params[current].user > 0) {
		global_params(gl, id);
		params[current].user = 0;
		node->extra[3].i = count_params(param_list, id);
	}

	n_items = node->extra[3].i;

	if ((index = node->index) < 0)
		index += n_items;
	if (index < 0 || index >= n_items)
		return 0;

	if ((p = find_nth_param(param_list, id, index)) == NULL)
		return 0;

	ot_set_pod(sub, NULL, p->param);
	return 1;
}

/**
 * ParamInfo
 */
static int ot_param_info_iterate(struct ot_node *node, struct ot_node *sub)
{
	struct spa_param_info *params = node->extra[0].p;
	struct global *gl = node->extra[2].p;
	int32_t index, n_params = node->extra[1].i;
	uint32_t id;

	index = node->index;
	if (index < 0)
		index += n_params;
	if (index < 0 || index >= n_params)
		return 0;

	id = params[index].id;

	ot_set_array(sub, spa_debug_type_find_short_name(spa_type_param, id),
			ot_param_list_iterate);

	sub->extra[0].p = gl;
	sub->extra[1].p = params;
	sub->extra[2].i = index;
	sub->extra[3].i = count_params(&gl->param_list, id);
	sub->flags = NODE_FLAG_EXPENSIVE;
	return 1;
}

static inline void ot_set_param_info(struct ot_node *node, const char *k,
		struct spa_param_info *params, uint32_t n_params,
		struct global *gl)
{
	ot_set_object(node, k, ot_param_info_iterate);
	node->extra[0].p = params;
	node->extra[1].i = n_params;
	node->extra[2].p = gl;
}

/**
 * Core
 */
static void core_event_info(void *object, const struct pw_core_info *info)
{
	struct global *gl = object;
	pw_log_debug(NAME" %p: core %d info", gl, gl->id);
	gl->info = pw_core_info_update(gl->info, info);
	core_sync(gl->data);
}

static const struct pw_core_events proxy_core_events = {
	PW_VERSION_CORE,
	.info = core_event_info,
};

static void core_destroy(void *object)
{
	struct global *gl = object;
	if (gl->info)
		pw_core_info_free(gl->info);
}

static int ot_core_iterate(struct ot_node *node, struct ot_node *sub)
{
	struct pw_core_info *i = node->extra[0].p;
	static const struct mask_table change_mask[] = {
		{ PW_CORE_CHANGE_MASK_PROPS, "props" },
	};
	int32_t index;

	if ((index = node->index) < 0)
		index += 8;

	switch(index) {
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
		ot_set_mask(sub, "change-mask",
				i->change_mask, change_mask, SPA_N_ELEMENTS(change_mask));
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
	struct global *gl = data;
	ot_set_object(node, k, ot_core_iterate);
	node->extra[0].p = gl->info;
}

static const struct object_info core_info = {
	.type = PW_TYPE_INTERFACE_Core,
	.version = PW_VERSION_CORE,
	.events = &proxy_core_events,
	.destroy = core_destroy,
	.enter = ot_core_info_enter,
};

/**
 * Module
 */
static void module_event_info(void *object, const struct pw_module_info *info)
{
	struct global *gl = object;

	pw_log_debug(NAME" %p: module %d info", gl, gl->id);
	gl->info = pw_module_info_update(gl->info, info);
	core_sync(gl->data);
}

static const struct pw_module_events module_events = {
	PW_VERSION_MODULE_EVENTS,
	.info = module_event_info,
};

static void module_destroy(void *object)
{
	struct global *gl = object;
	if (gl->info)
		pw_module_info_free(gl->info);
}

static int ot_module_iterate(struct ot_node *node, struct ot_node *sub)
{
	struct pw_module_info *i = node->extra[0].p;
	static const struct mask_table change_mask[] = {
		{ PW_MODULE_CHANGE_MASK_PROPS, "props" },
	};
	int32_t index;

	if ((index = node->index) < 0)
		index += 6;

	switch(index) {
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
		ot_set_mask(sub, "change-mask",
				i->change_mask, change_mask, SPA_N_ELEMENTS(change_mask));
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
	struct global *gl = data;
	ot_set_object(node, k, ot_module_iterate);
	node->extra[0].p = gl->info;
}

static const struct object_info module_info = {
	.type = PW_TYPE_INTERFACE_Module,
	.version = PW_VERSION_MODULE,
	.events = &module_events,
	.destroy = module_destroy,
	.enter = ot_module_info_enter,
};

/**
 * Factory
 */
static void factory_event_info(void *object, const struct pw_factory_info *info)
{
	struct global *gl = object;

	pw_log_debug(NAME" %p: factory %d info", gl, gl->id);
	gl->info = pw_factory_info_update(gl->info, info);
	core_sync(gl->data);
}

static const struct pw_factory_events factory_events = {
	PW_VERSION_FACTORY_EVENTS,
	.info = factory_event_info,
};

static void factory_destroy(void *object)
{
	struct global *gl = object;
	if (gl->info)
		pw_factory_info_free(gl->info);
}

static int ot_factory_iterate(struct ot_node *node, struct ot_node *sub)
{
	struct pw_factory_info *i = node->extra[0].p;
	static const struct mask_table change_mask[] = {
		{ PW_FACTORY_CHANGE_MASK_PROPS, "props" },
	};
	int32_t index;

	if ((index = node->index) < 0)
		index += 6;

	switch(index) {
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
		ot_set_mask(sub, "change-mask",
				i->change_mask, change_mask, SPA_N_ELEMENTS(change_mask));
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
	struct global *gl = data;
	ot_set_object(node, k, ot_factory_iterate);
	node->extra[0].p = gl->info;
}

static const struct object_info factory_info = {
	.type = PW_TYPE_INTERFACE_Factory,
	.version = PW_VERSION_FACTORY,
	.events = &factory_events,
	.destroy = factory_destroy,
	.enter = ot_factory_info_enter,
};

/**
 * Clients
 */
static void client_event_info(void *object, const struct pw_client_info *info)
{
	struct global *gl = object;

	pw_log_debug(NAME" %p: client %d info", gl, gl->id);
	gl->info = pw_client_info_update(gl->info, info);
	core_sync(gl->data);
}

static const struct pw_client_events client_events = {
	PW_VERSION_CLIENT_EVENTS,
	.info = client_event_info,
};

static void client_destroy(void *object)
{
	struct global *gl = object;
	if (gl->info)
		pw_client_info_free(gl->info);
}

static int ot_client_iterate(struct ot_node *node, struct ot_node *sub)
{
	struct pw_client_info *i = node->extra[0].p;
	static const struct mask_table change_mask[] = {
		{ PW_CLIENT_CHANGE_MASK_PROPS, "props" },
	};
	int32_t index;

	if ((index = node->index) < 0)
		index += 3;

	switch(index) {
	case 0:
		ot_set_int(sub, "id", i->id);
		break;
	case 1:
		ot_set_mask(sub, "change-mask",
				i->change_mask, change_mask, SPA_N_ELEMENTS(change_mask));
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
	struct global *gl = data;
	ot_set_object(node, k, ot_client_iterate);
	node->extra[0].p = gl->info;
}

static const struct object_info client_info = {
	.type = PW_TYPE_INTERFACE_Client,
	.version = PW_VERSION_CLIENT,
	.events = &client_events,
	.destroy = client_destroy,
	.enter = ot_client_info_enter,
};

/**
 * Device
 */
static void device_event_info(void *object, const struct pw_device_info *info)
{
	struct global *gl = object;
	uint32_t i;

	pw_log_debug(NAME" %p: device %d info", gl, gl->id);
	info = gl->info = pw_device_info_update(gl->info, info);

	if (info->change_mask & PW_DEVICE_CHANGE_MASK_PARAMS) {
		for (i = 0; i < info->n_params; i++) {
			if (info->params[i].user > 0)
				clear_params(&gl->param_list, info->params[i].id);
			if (!(info->params[i].flags & SPA_PARAM_INFO_READ))
				info->params[i].user = 0;
		}
	}
	core_sync(gl->data);
}

static void device_event_param(void *object, int seq,
		uint32_t id, uint32_t index, uint32_t next,
		const struct spa_pod *param)
{
	struct global *gl = object;
	pw_log_debug(NAME" %p: device %u param %d index:%d", gl, gl->id, id, index);
	add_param(&gl->param_list, id, param);
}

static const struct pw_device_events device_events = {
	PW_VERSION_DEVICE_EVENTS,
	.info = device_event_info,
	.param = device_event_param,
};

static void device_params(void *object, uint32_t id)
{
	struct global *gl = object;
	pw_device_enum_params((struct pw_device*)gl->proxy,
			1, id, 0, UINT32_MAX, NULL);
}

static void device_destroy(void *object)
{
	struct global *gl = object;
	if (gl->info)
		pw_device_info_free(gl->info);
}

static int ot_device_iterate(struct ot_node *node, struct ot_node *sub)
{
	struct global *gl = node->extra[0].p;
	struct pw_device_info *i = gl->info;
	static const struct mask_table change_mask[] = {
		{ PW_DEVICE_CHANGE_MASK_PROPS, "props" },
		{ PW_DEVICE_CHANGE_MASK_PARAMS, "params" },
	};
	int32_t index;

	if ((index = node->index) < 0)
		index += 4;

	switch(index) {
	case 0:
		ot_set_int(sub, "id", i->id);
		break;
	case 1:
		ot_set_mask(sub, "change-mask",
				i->change_mask, change_mask, SPA_N_ELEMENTS(change_mask));
		break;
	case 2:
		ot_set_props(sub, "props", i->props);
		break;
	case 3:
		ot_set_param_info(sub, "params", i->params, i->n_params, gl);
		break;
	default:
		return 0;
	}
	return 1;
}

static void ot_device_info_enter(void *data, const char *k, struct ot_node *node)
{
	struct global *gl = data;
	ot_set_object(node, k, ot_device_iterate);
	node->extra[0].p = gl;
}

static const struct object_info device_info = {
	.type = PW_TYPE_INTERFACE_Device,
	.version = PW_VERSION_DEVICE,
	.events = &device_events,
	.destroy = device_destroy,
	.enter = ot_device_info_enter,
	.params = device_params
};

/**
 * Node
 */
static void node_event_info(void *object, const struct pw_node_info *info)
{
	struct global *gl = object;
	uint32_t i;

	pw_log_debug(NAME" %p: node %d info", gl, gl->id);
	info = gl->info = pw_node_info_update(gl->info, info);

	if (info->change_mask & PW_NODE_CHANGE_MASK_PARAMS) {
		for (i = 0; i < info->n_params; i++) {
			if (info->params[i].user > 0)
				clear_params(&gl->param_list, info->params[i].id);
			if (!(info->params[i].flags & SPA_PARAM_INFO_READ))
				info->params[i].user = 0;
		}
	}
	core_sync(gl->data);
}

static void node_event_param(void *object, int seq,
		uint32_t id, uint32_t index, uint32_t next,
		const struct spa_pod *param)
{
	struct global *gl = object;
	pw_log_debug(NAME" %p: node %u param %d index:%d", gl, gl->id, id, index);
	add_param(&gl->param_list, id, param);
}

static const struct pw_node_events node_events = {
	PW_VERSION_NODE_EVENTS,
	.info = node_event_info,
	.param = node_event_param,
};

static void node_params(void *object, uint32_t id)
{
	struct global *gl = object;
	pw_node_enum_params((struct pw_node*)gl->proxy,
			1, id, 0, UINT32_MAX, NULL);
}

static void node_destroy(void *object)
{
	struct global *gl = object;
	if (gl->info)
		pw_node_info_free(gl->info);
}

static int ot_node_info_iterate(struct ot_node *node, struct ot_node *sub)
{
	struct global *gl = node->extra[0].p;
	struct pw_node_info *i = gl->info;
	static const struct mask_table change_mask[] = {
		{ PW_NODE_CHANGE_MASK_INPUT_PORTS, "n-input-ports" },
		{ PW_NODE_CHANGE_MASK_OUTPUT_PORTS, "n-output-ports" },
		{ PW_NODE_CHANGE_MASK_STATE, "state" },
		{ PW_NODE_CHANGE_MASK_PROPS, "props" },
		{ PW_NODE_CHANGE_MASK_PARAMS, "params" },
	};
	int32_t index;

	if ((index = node->index) < 0)
		index += 10;

	switch(index) {
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
		ot_set_mask(sub, "change-mask",
				i->change_mask, change_mask, SPA_N_ELEMENTS(change_mask));
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
		ot_set_param_info(sub, "params", i->params, i->n_params, gl);
		break;
	default:
		return 0;
	}
	return 1;
}

static void ot_node_info_enter(void *data, const char *k, struct ot_node *node)
{
	struct global *gl = data;
	ot_set_object(node, k, ot_node_info_iterate);
	node->extra[0].p = gl;
}

static const struct object_info node_info = {
	.type = PW_TYPE_INTERFACE_Node,
	.version = PW_VERSION_NODE,
	.events = &node_events,
	.destroy = node_destroy,
	.enter = ot_node_info_enter,
	.params = node_params
};

/**
 * Port
 */
static void port_event_info(void *object, const struct pw_port_info *info)
{
	struct global *gl = object;
	uint32_t i;

	pw_log_debug(NAME" %p: port %u info", gl, gl->id);
	info = gl->info = pw_port_info_update(gl->info, info);

	if (info->change_mask & PW_PORT_CHANGE_MASK_PARAMS) {
		for (i = 0; i < info->n_params; i++) {
			if (info->params[i].user > 0)
				clear_params(&gl->param_list, info->params[i].id);
			if (!(info->params[i].flags & SPA_PARAM_INFO_READ))
				info->params[i].user = 0;
		}
	}
	core_sync(gl->data);
}

static void port_event_param(void *object, int seq,
		uint32_t id, uint32_t index, uint32_t next,
		const struct spa_pod *param)
{
	struct global *gl = object;
	pw_log_debug(NAME" %p: port %u param %d index:%d", gl, gl->id, id, index);
	add_param(&gl->param_list, id, param);
}

static const struct pw_port_events port_events = {
	PW_VERSION_PORT_EVENTS,
	.info = port_event_info,
	.param = port_event_param,
};

static void port_params(void *object, uint32_t id)
{
	struct global *gl = object;
	pw_port_enum_params((struct pw_port*)gl->proxy,
			1, id, 0, UINT32_MAX, NULL);
}

static void port_destroy(void *object)
{
	struct global *gl = object;
	if (gl->info)
		pw_port_info_free(gl->info);
}


static int ot_port_iterate(struct ot_node *node, struct ot_node *sub)
{
	struct global *gl = node->extra[0].p;
	struct pw_port_info *i = gl->info;
	static const struct mask_table change_mask[] = {
		{ PW_PORT_CHANGE_MASK_PROPS, "props" },
		{ PW_PORT_CHANGE_MASK_PARAMS, "params" },
	};
	int32_t index;

	if ((index = node->index) < 0)
		index += 5;

	switch(index) {
	case 0:
		ot_set_int(sub, "id", i->id);
		break;
	case 1:
		ot_set_string(sub, "direction", pw_direction_as_string(i->direction));
		break;
	case 2:
		ot_set_mask(sub, "change-mask",
				i->change_mask, change_mask, SPA_N_ELEMENTS(change_mask));
		break;
	case 3:
		ot_set_props(sub, "props", i->props);
		break;
	case 4:
		ot_set_param_info(sub, "params", i->params, i->n_params, gl);
		break;
	default:
		return 0;
	}
	return 1;
}

static void ot_port_info_enter(void *data, const char *k, struct ot_node *node)
{
	struct global *gl = data;
	ot_set_object(node, k, ot_port_iterate);
	node->extra[0].p = gl;
}

static const struct object_info port_info = {
	.type = PW_TYPE_INTERFACE_Port,
	.version = PW_VERSION_PORT,
	.events = &port_events,
	.destroy = port_destroy,
	.enter = ot_port_info_enter,
	.params = port_params
};

/**
 * Link
 */
static void link_event_info(void *object, const struct pw_link_info *info)
{
	struct global *gl = object;
	pw_log_debug(NAME" %p: link %u info", gl, gl->id);
	gl->info = pw_link_info_update(gl->info, info);
	core_sync(gl->data);
}

static const struct pw_link_events link_events = {
	PW_VERSION_LINK_EVENTS,
	.info = link_event_info,
};

static void link_destroy(void *object)
{
	struct global *gl = object;
	if (gl->info)
		pw_link_info_free(gl->info);
}

static int ot_link_iterate(struct ot_node *node, struct ot_node *sub)
{
	struct pw_link_info *i = node->extra[0].p;
	static const struct mask_table change_mask[] = {
		{ PW_LINK_CHANGE_MASK_STATE, "state" },
		{ PW_LINK_CHANGE_MASK_FORMAT, "format" },
		{ PW_LINK_CHANGE_MASK_PROPS, "props" },
	};
	int32_t index;

	if ((index = node->index) < 0)
		index += 10;

	switch(index) {
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
		ot_set_mask(sub, "change-mask",
				i->change_mask, change_mask, SPA_N_ELEMENTS(change_mask));
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
	const struct global *gl = data;
	ot_set_object(node, k, ot_link_iterate);
	node->extra[0].p = gl->info;
}

static const struct object_info link_info = {
	.type = PW_TYPE_INTERFACE_Link,
	.version = PW_VERSION_LINK,
	.events = &link_events,
	.destroy = link_destroy,
	.enter = ot_link_info_enter,
};

static void on_core_done(void *data, uint32_t id, int seq)
{
	struct data *d = data;
	if (id == PW_ID_CORE)
		d->last_seq = seq;
}


static int ot_global_iterate(struct ot_node *node, struct ot_node *sub)
{
	struct global *gl = node->extra[0].p;
	static const struct mask_table change_mask[] = {
		{ PW_PERM_R, "r" },
		{ PW_PERM_W, "w" },
		{ PW_PERM_X, "x" },
	};
	int32_t index;

	if ((index = node->index) < 0)
		index += 6;

	switch(index) {
	case 0:
		ot_set_int(sub, "id", gl->id);
		break;
	case 1:
		ot_set_string(sub, "type", gl->type);
		break;
	case 2:
		ot_set_int(sub, "version", gl->version);
		break;
	case 3:
		ot_set_mask(sub, "permissions", gl->permissions,
				change_mask, SPA_N_ELEMENTS(change_mask));
		break;
	case 4:
		ot_set_props(sub, "properties", gl->props ? &gl->props->dict : NULL);
		break;
	case 5:
		ot_set_global_info(sub, "info", gl);
		break;
	default:
		return 0;
	}
	return 1;
}

static void ot_global_enter(struct global *gl, const char *k, struct ot_node *node)
{
	ot_set_object(node, k, ot_global_iterate);
	node->extra[0].p = gl;
}

static struct global *global_get_nth(struct spa_list *list, int nth)
{
	struct global *p;
	spa_list_for_each(p, list, link)
		if (nth-- == 0)
			return p;
	return NULL;
}

static int ot_root_iterate(struct ot_node *node, struct ot_node *sub)
{
	struct global *global;
	struct spa_list *global_list = node->extra[0].p;
	int32_t index, n_items = node->extra[1].i;

	if ((index = node->index) < 0)
		index += n_items;
	if (index < 0 || index >= n_items)
		return 0;

	if ((global = global_get_nth(global_list, index)) == NULL)
		return 0;

	ot_global_enter(global, NULL, sub);
	return 1;
}

static inline void ot_root_enter(struct data *data, const char *k, struct ot_node *node)
{
	ot_set_array(node, k, ot_root_iterate);
	node->extra[0].p = &data->global_list;
	node->extra[1].i = data->n_globals;
}

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
	struct global *gl;
	char path[10];

	gl = calloc(1, sizeof(*gl));
	if (gl == NULL) {
		pw_log_error("can't alloc global for %u %s/%d: %m", id, type, version);
		return;
	}
	gl->data = d;
	gl->id = id;
	gl->permissions = permissions;
	gl->type = strdup(type);
	gl->version = version;
	gl->props = props ? pw_properties_new_dict(props) : NULL;
	gl->object_info = find_info(type, version);
	spa_list_init(&gl->param_list);

	spa_list_append(&d->global_list, &gl->link);
	d->n_globals++;

	snprintf(path, sizeof(path), "/%u", id);
	pw_tree_emit_added(d, path);
}

static void registry_event_global_remove(void *object, uint32_t id)
{
        struct data *d = object;
	char path[10];

	snprintf(path, sizeof(path), "/%u", id);
	pw_tree_emit_removed(d, path);
}

static const struct pw_registry_events registry_events = {
	PW_VERSION_REGISTRY_EVENTS,
	.global = registry_event_global,
	.global_remove = registry_event_global_remove,
};

static const struct pw_core_events core_events = {
	PW_VERSION_CORE_EVENTS,
	.done = on_core_done,
};

struct pw_tree * pw_tree_new(struct pw_core *core)
{
	struct pw_tree *tree;
	struct data *data;

	tree = calloc(1, sizeof(*tree));
	if (tree == NULL)
		return NULL;

	data = &tree->data;
	spa_hook_list_init(&data->listener_list);

	data->core = core;
	data->context = pw_core_get_context(core);
	data->loop = pw_context_get_main_loop(data->context);
	spa_list_init(&data->global_list);

	pw_core_add_listener(data->core,
			&data->core_listener,
			&core_events, data);
	data->registry = pw_core_get_registry(data->core,
			PW_VERSION_REGISTRY, 0);
	pw_registry_add_listener(data->registry,
			&data->registry_listener,
			&registry_events, data);
	return tree;
}

void pw_tree_add_listener(struct pw_tree *tree,
		struct spa_hook *listener,
		const struct pw_tree_events *events, void *data)
{
	spa_hook_list_append(&tree->data.listener_list, listener, events, data);
}

int pw_tree_get_root(struct pw_tree *tree, struct ot_node *node)
{
	spa_zero(*node);
	ot_root_enter(&tree->data, NULL, node);
	return 0;
}

void pw_tree_destroy(struct pw_tree *tree)
{
	free(tree);
}
