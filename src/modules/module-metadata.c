/* PipeWire */
/* SPDX-FileCopyrightText: Copyright © 2019 Wim Taymans */
/* SPDX-License-Identifier: MIT */

#include "config.h"

#include <string.h>
#include <stdio.h>
#include <errno.h>
#include <dlfcn.h>

#include <spa/utils/result.h>
#include <spa/utils/json.h>

#define PW_API_METADATA_IMPL	SPA_EXPORT
#include <pipewire/impl.h>
#include <pipewire/extensions/metadata.h>

/** \page page_module_metadata Metadata
 *
 * Allows clients to export a metadata store to the PipeWire server.
 *
 * Both the client and the server need to load this module for the metadata
 * to be exported.
 *
 * This module creates a new factory and a new export type for the
 * \ref PW_TYPE_INTERFACE_Metadata interface.
 *
 * A client will first create an implementation of the PW_TYPE_INTERFACE_Metadata
 * interface with \ref pw_context_create_metadata(), for example. With the
 * \ref pw_core_export(), this module will create a server side resource to expose
 * the metadata implementation to other clients. Modifications done by the client
 * on the local metadata interface will be visible to all PipeWire clients.
 *
 * It is also possible to use the factory to create metadata in the current
 * processes using a config file fragment.
 *
 * As an argument to the create_object call, a set of properties will
 * control the name of the metadata and some initial values.
 *
 * ## Module Name
 *
 * `libpipewire-module-metadata`
 *
 * ## Module Options
 *
 * This module has no options.
 *
 * ## Properties for the create_object call
 *
 * - `metadata.name`: The name of the new metadata object. If not given, the metadata
 *                    object name will be `default`.
 * - `metadata.values`: A JSON array of objects with initial values for the metadata object.
 *
 *   the `metadata.values` key has the following layout:
 *
 *  \code{.unparsed}
 *   metadata.values = [
 *      { id = <int>  key = <key>  type = <type> value = <object> }
 *      ....
 *   ]
 *  \endcode
 *
 *     - `id`: an optional object id for the metadata, default 0
 *     - `key`: a string, the metadata key
 *     - `type`: an optional metadata value type
 *     - `value`: a JSON item, the metadata value.
 *
 * ## Example configuration
 *
 * The module is usually added to the config file of the main PipeWire daemon and the
 * clients.
 *
 *\code{.unparsed}
 * context.modules = [
 * { name = libpipewire-module-metadata }
 * ]
 *\endcode

 * ## Config objects
 *
 * To create an object from the factory, one can use the \ref pw_core_create_object()
 * method or make an object in the `context.objects` section like in the main PipeWire
 * daemon config file:
 *
 *\code{.unparsed}
 * context.objects = [
 * { factory = metadata
 *     args = {
 *         metadata.name = default
 *         metadata.values = [
 *            { key = default.audio.sink   value = { name = somesink } }
 *            { key = default.audio.source value = { name = somesource } }
 *         ]
 *     }
 * }
 *\endcode
 *
 * This creates a new default metadata store with 2 key/values.
 *
 * ## See also
 *
 * - `pw-metadata`: a tool to manage metadata
 */

#define NAME "metadata"

#define FACTORY_USAGE   "("PW_KEY_METADATA_NAME" = <name> ) "						\
                        "("PW_KEY_METADATA_VALUES" = [ "						\
                        "   { ( id = <int> ) key = <string> ( type = <string> ) value = <json> } "	\
                        "   ..."									\
                        "  ] )"

PW_LOG_TOPIC_STATIC(mod_topic, "mod." NAME);
#define PW_LOG_TOPIC_DEFAULT mod_topic

static const struct spa_dict_item module_props[] = {
	{ PW_KEY_MODULE_AUTHOR, "Wim Taymans <wim.taymans@gmail.com>" },
	{ PW_KEY_MODULE_DESCRIPTION, "Allow clients to create metadata store" },
	{ PW_KEY_MODULE_VERSION, PACKAGE_VERSION },
};


struct pw_metadata *pw_metadata_new(struct pw_context *context, struct pw_resource *resource,
		   struct pw_properties *properties);

struct pw_proxy *pw_core_metadata_export(struct pw_core *core,
		const char *type, const struct spa_dict *props, void *object, size_t user_data_size);

int pw_protocol_native_ext_metadata_init(struct pw_context *context);

struct factory_data {
	struct pw_impl_factory *factory;
	struct spa_hook factory_listener;

	struct pw_impl_module *module;
	struct spa_hook module_listener;

	struct pw_export_type export_metadata;
};

/*
 * [
 *     { ( "id" = <int>, ) "key" = <string> ("type" = <string>) "value" = <json> }
 *     ....
 * ]
 */
static int fill_metadata(struct pw_metadata *metadata, const char *str)
{
	struct spa_json it[2];

	if (spa_json_begin_array(&it[0], str, strlen(str)) <= 0)
		return -EINVAL;

	while (spa_json_enter_object(&it[0], &it[1]) > 0) {
		char key[256], *k = NULL, *v = NULL, *t = NULL;
		int id = 0, len;
		const char *val;

		while ((len = spa_json_object_next(&it[1], key, sizeof(key), &val)) > 0) {
			if (spa_streq(key, "id")) {
				if (spa_json_parse_int(val, len, &id) <= 0)
					return -EINVAL;
			} else if (spa_streq(key, "key")) {
				if ((k = malloc(len+1)) != NULL)
					spa_json_parse_stringn(val, len, k, len+1);
			} else if (spa_streq(key, "type")) {
				if ((t = malloc(len+1)) != NULL)
					spa_json_parse_stringn(val, len, t, len+1);
			} else if (spa_streq(key, "value")) {
				if (spa_json_is_container(val, len))
					len = spa_json_container_len(&it[1], val, len);
				if ((v = malloc(len+1)) != NULL)
					spa_json_parse_stringn(val, len, v, len+1);
			}
		}
		if (k != NULL && v != NULL)
			pw_metadata_set_property(metadata, id, k, t, v);
		free(k);
		free(v);
		free(t);
	}
	return 0;
}

static void *create_object(void *_data,
			   struct pw_resource *resource,
			   const char *type,
			   uint32_t version,
			   struct pw_properties *properties,
			   uint32_t new_id)
{
	struct factory_data *data = _data;
	struct pw_context *context = pw_impl_module_get_context(data->module);
	struct pw_metadata *result;
	struct pw_resource *metadata_resource = NULL;
	struct pw_impl_client *client = resource ? pw_resource_get_client(resource) : NULL;
	const char *str;
	int res;

	if (properties == NULL)
		properties = pw_properties_new(NULL, NULL);
	if (properties == NULL)
		return NULL;

	pw_properties_setf(properties, PW_KEY_FACTORY_ID, "%d",
			pw_impl_factory_get_info(data->factory)->id);
	pw_properties_setf(properties, PW_KEY_MODULE_ID, "%d",
			pw_impl_module_get_info(data->module)->id);

	if (pw_properties_get(properties, PW_KEY_METADATA_NAME) == NULL)
		pw_properties_set(properties, PW_KEY_METADATA_NAME, "default");

	if (client) {
		metadata_resource = pw_resource_new(client, new_id, PW_PERM_ALL, type, version, 0);
		if (metadata_resource == NULL) {
			res = -errno;
			goto error_resource;
		}

		pw_properties_setf(properties, PW_KEY_CLIENT_ID, "%d",
				pw_impl_client_get_info(client)->id);

		result = pw_metadata_new(context, metadata_resource, properties);
		if (result == NULL) {
			properties = NULL;
			res = -errno;
			goto error_node;
		}
	} else {
		struct pw_impl_metadata *impl;

		impl = pw_context_create_metadata(context, NULL, properties, 0);
		if (impl == NULL) {
			properties = NULL;
			res = -errno;
			goto error_node;
		}
		pw_impl_metadata_register(impl, NULL);
		result = pw_impl_metadata_get_implementation(impl);
	}
	if ((str = pw_properties_get(properties, PW_KEY_METADATA_VALUES)) != NULL)
		fill_metadata(result, str);

	return result;

error_resource:
	pw_resource_errorf_id(resource, new_id, res,
				"can't create resource: %s", spa_strerror(res));
	goto error_exit;
error_node:
	pw_resource_errorf_id(resource, new_id, res,
				"can't create metadata: %s", spa_strerror(res));
	goto error_exit_free;

error_exit_free:
	if (metadata_resource)
		pw_resource_remove(metadata_resource);
error_exit:
	pw_properties_free(properties);
	errno = -res;
	return NULL;
}

static const struct pw_impl_factory_implementation impl_factory = {
	PW_VERSION_IMPL_FACTORY_IMPLEMENTATION,
	.create_object = create_object,
};

static void factory_destroy(void *data)
{
	struct factory_data *d = data;
	spa_hook_remove(&d->factory_listener);
	d->factory = NULL;
	if (d->module)
		pw_impl_module_destroy(d->module);
}

static const struct pw_impl_factory_events factory_events = {
	PW_VERSION_IMPL_FACTORY_EVENTS,
	.destroy = factory_destroy,
};

static void module_destroy(void *data)
{
	struct factory_data *d = data;
	spa_hook_remove(&d->module_listener);
	spa_list_remove(&d->export_metadata.link);
	d->module = NULL;
	if (d->factory)
		pw_impl_factory_destroy(d->factory);
}

static void module_registered(void *data)
{
	struct factory_data *d = data;
	struct pw_impl_module *module = d->module;
	struct pw_impl_factory *factory = d->factory;
	struct spa_dict_item items[1];
	char id[16];
	int res;

	snprintf(id, sizeof(id), "%d", pw_global_get_id(pw_impl_module_get_global(module)));
	items[0] = SPA_DICT_ITEM_INIT(PW_KEY_MODULE_ID, id);
	pw_impl_factory_update_properties(factory, &SPA_DICT_INIT(items, 1));

	if ((res = pw_impl_factory_register(factory, NULL)) < 0) {
		pw_log_error("%p: can't register factory: %s", factory, spa_strerror(res));
	}
}

static const struct pw_impl_module_events module_events = {
	PW_VERSION_IMPL_MODULE_EVENTS,
	.destroy = module_destroy,
	.registered = module_registered,
};

SPA_EXPORT
int pipewire__module_init(struct pw_impl_module *module, const char *args)
{
	struct pw_context *context = pw_impl_module_get_context(module);
	struct pw_impl_factory *factory;
	struct factory_data *data;
	int res;

	PW_LOG_TOPIC_INIT(mod_topic);

	if ((res = pw_protocol_native_ext_metadata_init(context)) < 0)
		return res;

	factory = pw_context_create_factory(context,
				 "metadata",
				 PW_TYPE_INTERFACE_Metadata,
				 PW_VERSION_METADATA,
				 pw_properties_new(
                                         PW_KEY_FACTORY_USAGE, FACTORY_USAGE,
                                         NULL),
				 sizeof(*data));
	if (factory == NULL)
		return -errno;

	data = pw_impl_factory_get_user_data(factory);
	data->factory = factory;
	data->module = module;

	pw_log_debug("module %p: new", module);

	pw_impl_factory_set_implementation(factory,
				      &impl_factory,
				      data);

	data->export_metadata.type = PW_TYPE_INTERFACE_Metadata;
	data->export_metadata.func = pw_core_metadata_export;
	if ((res = pw_context_register_export_type(context, &data->export_metadata)) < 0)
		goto error;

	pw_impl_factory_add_listener(factory, &data->factory_listener, &factory_events, data);
	pw_impl_module_add_listener(module, &data->module_listener, &module_events, data);

	pw_impl_module_update_properties(module, &SPA_DICT_INIT_ARRAY(module_props));

	return 0;
error:
	pw_impl_factory_destroy(data->factory);
	return res;
}
