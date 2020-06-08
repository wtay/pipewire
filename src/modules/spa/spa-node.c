/* PipeWire
 * Copyright (C) 2015 Wim Taymans <wim.taymans@gmail.com>
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

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <string.h>
#include <stdio.h>
#include <dlfcn.h>

#include <spa/node/node.h>
#include <spa/param/props.h>
#include <spa/pod/iter.h>

#include "spa-node.h"
#include "pipewire/node.h"
#include "pipewire/port.h"
#include "pipewire/log.h"
#include "pipewire/private.h"

struct impl {
	struct pw_node *this;

	struct pw_client *owner;
	struct pw_global *parent;

	enum pw_spa_node_flags flags;
	bool async_init;

	void *hnd;
        struct spa_handle *handle;
        struct spa_node *node;          /**< handle to SPA node */
	char *lib;
	char *factory_name;

	struct spa_hook node_listener;

	void *user_data;
};

static void pw_spa_node_destroy(void *data)
{
	struct impl *impl = data;
	struct pw_node *node = impl->this;

	pw_log_debug("spa-node %p: destroy", node);

	spa_hook_remove(&impl->node_listener);
	if (impl->handle) {
		spa_handle_clear(impl->handle);
		free(impl->handle);
	}
	free(impl->lib);
	free(impl->factory_name);
	if (impl->hnd)
		dlclose(impl->hnd);
}

static void complete_init(struct impl *impl)
{
        struct pw_node *this = impl->this;
	if (impl->flags & PW_SPA_NODE_FLAG_DISABLE)
		pw_node_set_enabled(this, false);
	pw_node_register(this, impl->owner, impl->parent, NULL);
	if (impl->flags & PW_SPA_NODE_FLAG_ACTIVATE)
		pw_node_set_active(this, true);
}

static void on_node_done(void *data, uint32_t seq, int res)
{
        struct impl *impl = data;
        struct pw_node *this = impl->this;

	if (impl->async_init) {
		complete_init(impl);
		impl->async_init = false;
	}
        pw_log_debug("spa-node %p: async complete event %d %d", this, seq, res);
}

static const struct pw_node_events node_events = {
	PW_VERSION_NODE_EVENTS,
	.destroy = pw_spa_node_destroy,
	.async_complete = on_node_done,
};

struct pw_node *
pw_spa_node_new(struct pw_core *core,
		struct pw_client *owner,
		struct pw_global *parent,
		const char *name,
		enum pw_spa_node_flags flags,
		struct spa_node *node,
		struct spa_handle *handle,
		struct pw_properties *properties,
		size_t user_data_size)
{
	struct pw_node *this;
	struct impl *impl;
	void *iface = NULL;
	struct pw_type *t = pw_core_get_type(core);
	int res;

	this = pw_node_new(core, name, properties, sizeof(struct impl) + user_data_size);
	if (this == NULL)
		return NULL;

	if (handle) {
		if ((res = spa_handle_get_interface(handle, t->spa_clock, &iface)) < 0)
			iface = NULL;
		this->clock = iface;
	}

	impl = this->user_data;
	impl->this = this;
	impl->owner = owner;
	impl->parent = parent;
	impl->node = node;
	impl->flags = flags;
	impl->async_init = flags & PW_SPA_NODE_FLAG_ASYNC;

	if (user_data_size > 0)
                impl->user_data = SPA_MEMBER(impl, sizeof(struct impl), void);

	pw_node_add_listener(this, &impl->node_listener, &node_events, impl);
	pw_node_set_implementation(this, impl->node);

	if (!impl->async_init)
		complete_init(impl);

	return this;
}

void *pw_spa_node_get_user_data(struct pw_node *node)
{
	struct impl *impl = node->user_data;
	return impl->user_data;
}

static int
setup_props(struct pw_core *core, struct spa_node *spa_node, struct pw_properties *pw_props)
{
	int res;
	struct spa_pod *props;
	void *state = NULL;
	const char *key;
	struct pw_type *t = pw_core_get_type(core);
	uint32_t index = 0;
	uint8_t buf[2048];
	struct spa_pod_builder b = SPA_POD_BUILDER_INIT(buf, sizeof(buf));

	if ((res = spa_node_enum_params(spa_node, t->param.idProps, &index, NULL, &props, &b)) <= 0) {
		pw_log_debug("spa_node_get_props failed: %d", res);
		return res;
	}

	while ((key = pw_properties_iterate(pw_props, &state))) {
		struct spa_pod_prop *prop;
		uint32_t id;

		if (!spa_type_is_a(key, SPA_TYPE_PROPS_BASE))
			continue;

		id = spa_type_map_get_id(t->map, key);
		if (id == SPA_ID_INVALID)
			continue;

		if ((prop = spa_pod_find_prop(props, id))) {
			const char *value = pw_properties_get(pw_props, key);

			if (value == NULL)
				continue;

			pw_log_info("configure prop %s", key);

			switch(prop->body.value.type) {
			case SPA_POD_TYPE_BOOL:
				SPA_POD_VALUE(struct spa_pod_bool, &prop->body.value) =
					pw_properties_parse_bool(value);
				break;
			case SPA_POD_TYPE_ID:
				SPA_POD_VALUE(struct spa_pod_id, &prop->body.value) =
					spa_type_map_get_id(t->map, value);
				break;
			case SPA_POD_TYPE_INT:
				SPA_POD_VALUE(struct spa_pod_int, &prop->body.value) =
					pw_properties_parse_int(value);
				break;
			case SPA_POD_TYPE_LONG:
				SPA_POD_VALUE(struct spa_pod_long, &prop->body.value) =
					pw_properties_parse_int64(value);
				break;
			case SPA_POD_TYPE_FLOAT:
				SPA_POD_VALUE(struct spa_pod_float, &prop->body.value) =
					pw_properties_parse_float(value);
				break;
			case SPA_POD_TYPE_DOUBLE:
				SPA_POD_VALUE(struct spa_pod_double, &prop->body.value) =
					pw_properties_parse_double(value);
				break;
			case SPA_POD_TYPE_STRING:
				break;
			default:
				break;
			}
		}
	}

	if ((res = spa_node_set_param(spa_node, t->param.idProps, 0, props)) < 0) {
		pw_log_debug("spa_node_set_props failed: %d", res);
		return res;
	}
	return 0;
}


struct pw_node *pw_spa_node_load(struct pw_core *core,
				 struct pw_client *owner,
				 struct pw_global *parent,
				 const char *lib,
				 const char *factory_name,
				 const char *name,
				 enum pw_spa_node_flags flags,
				 struct pw_properties *properties,
				 size_t user_data_size)
{
	struct pw_node *this;
	struct impl *impl;
	struct spa_node *spa_node;
	int res;
	struct spa_handle *handle;
	void *hnd;
	uint32_t index;
	spa_handle_factory_enum_func_t enum_func;
	const struct spa_handle_factory *factory;
	void *iface;
	char *filename;
	const char *dir;
	const struct spa_support *support;
	uint32_t n_support;
	struct pw_type *t = pw_core_get_type(core);

	if ((dir = getenv("SPA_PLUGIN_DIR")) == NULL)
		dir = PLUGINDIR;

	asprintf(&filename, "%s/%s.so", dir, lib);

	if ((hnd = dlopen(filename, RTLD_NOW)) == NULL) {
		pw_log_error("can't load %s: %s", filename, dlerror());
		goto open_failed;
	}
	if ((enum_func = dlsym(hnd, SPA_HANDLE_FACTORY_ENUM_FUNC_NAME)) == NULL) {
		pw_log_error("can't find enum function");
		goto no_symbol;
	}

	for (index = 0;;) {
		if ((res = enum_func(&factory, &index)) <= 0) {
			if (res != 0)
				pw_log_error("can't enumerate factories: %s", spa_strerror(res));
			goto enum_failed;
		}
		if (strcmp(factory->name, factory_name) == 0)
			break;
	}

	support = pw_core_get_support(core, &n_support);

	handle = calloc(1, factory->size);
	if ((res = spa_handle_factory_init(factory,
					   handle,
					   properties ? &properties->dict : NULL,
					   support,
					   n_support)) < 0) {
		pw_log_error("can't make factory instance: %d", res);
		goto init_failed;
	}
	if (SPA_RESULT_IS_ASYNC(res))
		flags |= PW_SPA_NODE_FLAG_ASYNC;

	if ((res = spa_handle_get_interface(handle, t->spa_node, &iface)) < 0) {
		pw_log_error("can't get node interface %d", res);
		goto interface_failed;
	}
	spa_node = iface;

	if (properties != NULL) {
		if (setup_props(core, spa_node, properties) < 0) {
			pw_log_debug("Unrecognized properties");
		}
	}

	this = pw_spa_node_new(core, owner, parent, name, flags,
			       spa_node, handle, properties, user_data_size);

	impl = this->user_data;
	impl->hnd = hnd;
	impl->handle = handle;
	impl->lib = filename;
	impl->factory_name = strdup(factory_name);

	return this;

      interface_failed:
	spa_handle_clear(handle);
      init_failed:
	free(handle);
      enum_failed:
      no_symbol:
	dlclose(hnd);
      open_failed:
	free(filename);
	return NULL;
}
