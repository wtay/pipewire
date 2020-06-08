/* PipeWire
 * Copyright (C) 2017 Wim Taymans <wim.taymans@gmail.com>
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

#include <string.h>
#include <stdio.h>
#include <errno.h>
#include <dlfcn.h>

#include "config.h"

#include "pipewire/core.h"
#include "pipewire/interfaces.h"
#include "pipewire/log.h"
#include "pipewire/module.h"

#include "module-client-node/client-node.h"

struct pw_protocol *pw_protocol_native_ext_client_node_init(struct pw_core *core);

struct factory_data {
	struct pw_factory *this;
	struct pw_properties *properties;

	struct spa_hook module_listener;
	uint32_t type_client_node;
};

static void *create_object(void *_data,
			   struct pw_resource *resource,
			   uint32_t type,
			   uint32_t version,
			   struct pw_properties *properties,
			   uint32_t new_id)
{
	struct pw_client_node *node;
	struct pw_resource *node_resource;

	if (resource == NULL)
		goto no_resource;

	node_resource = pw_resource_new(pw_resource_get_client(resource),
					new_id, PW_PERM_RWX, type, version, 0);
	if (node_resource == NULL)
		goto no_mem;

	node = pw_client_node_new(node_resource, properties);
	if (node == NULL)
		goto no_mem;

	return node;

      no_resource:
	pw_log_error("client-node needs a resource");
	goto done;
      no_mem:
	pw_log_error("can't create node");
	pw_resource_error(resource, -ENOMEM, "no memory");
	goto done;
      done:
	if (properties)
		pw_properties_free(properties);
	return NULL;
}

static const struct pw_factory_implementation impl_factory = {
	PW_VERSION_FACTORY_IMPLEMENTATION,
	.create_object = create_object,
};

static void module_destroy(void *data)
{
	struct factory_data *d = data;

	spa_hook_remove(&d->module_listener);

	if (d->properties)
		pw_properties_free(d->properties);

	pw_factory_destroy(d->this);
}

static const struct pw_module_events module_events = {
	PW_VERSION_MODULE_EVENTS,
	.destroy = module_destroy,
};

static int module_init(struct pw_module *module, struct pw_properties *properties)
{
	struct pw_core *core = pw_module_get_core(module);
	struct pw_type *t = pw_core_get_type(core);
	struct pw_factory *factory;
	struct factory_data *data;
	uint32_t type_client_node;

        type_client_node = spa_type_map_get_id(t->map, PW_TYPE_INTERFACE__ClientNode);

	factory = pw_factory_new(core,
				 "client-node",
				 type_client_node,
				 PW_VERSION_CLIENT_NODE,
				 NULL,
				 sizeof(*data));
	if (factory == NULL)
		return -ENOMEM;

	data = pw_factory_get_user_data(factory);
	data->this = factory;
	data->properties = properties;
	data->type_client_node = type_client_node;

	pw_log_debug("module %p: new", module);

	pw_factory_set_implementation(factory,
				      &impl_factory,
				      data);

	pw_protocol_native_ext_client_node_init(core);

	pw_factory_register(factory, NULL, pw_module_get_global(module), NULL);

	pw_module_add_listener(module, &data->module_listener, &module_events, data);

	return 0;
}

SPA_EXPORT
int pipewire__module_init(struct pw_module *module, const char *args)
{
	return module_init(module, NULL);
}
