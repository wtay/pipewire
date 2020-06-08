/* PipeWire
 * Copyright (C) 2016 Wim Taymans <wim.taymans@gmail.com>
 * Copyright (C) 2019 Red Hat Inc.
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
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>

#include "config.h"

#include <dbus/dbus.h>

#include <spa/support/dbus.h>

#include "pipewire/core.h"
#include "pipewire/interfaces.h"
#include "pipewire/link.h"
#include "pipewire/log.h"
#include "pipewire/module.h"
#include "pipewire/utils.h"

enum media_role {
	MEDIA_ROLE_NONE = 0,
	MEDIA_ROLE_CAMERA = 1 << 0,
};
#define MEDIA_ROLE_ALL (MEDIA_ROLE_CAMERA)

struct impl {
	struct pw_core *core;
	struct pw_type *type;
	struct pw_properties *properties;

	struct spa_dbus_connection *conn;
	DBusConnection *bus;

	struct spa_hook core_listener;
	struct spa_hook module_listener;

	struct spa_list client_list;

	DBusPendingCall *portal_pid_pending;
	pid_t portal_pid;
};

struct client_info {
	struct spa_list link;
	struct impl *impl;
	struct pw_client *client;
	struct spa_hook client_listener;
        struct spa_list resources;

	bool portal_managed;
	bool setup_complete;
	bool is_portal;
	char *app_id;
	enum media_role media_roles;
	enum media_role allowed_media_roles;
};

static struct client_info *find_client_info(struct impl *impl, struct pw_client *client)
{
	struct client_info *info;

	spa_list_for_each(info, &impl->client_list, link) {
		if (info->client == client)
			return info;
	}
	return NULL;
}

static void client_info_free(struct client_info *cinfo)
{
	spa_hook_remove(&cinfo->client_listener);
	spa_list_remove(&cinfo->link);
	free(cinfo->app_id);
	free(cinfo);
}

static enum media_role media_role_from_string(const char *media_role_str)
{
	if (strcmp(media_role_str, "Camera") == 0)
		return MEDIA_ROLE_CAMERA;
	else
		return -1;
}

static enum media_role parse_media_roles(const char *media_types_str)
{
	enum media_role media_roles = 0;
	char *buf_orig;
	char *buf;

	buf_orig = strdup(media_types_str);
	buf = buf_orig;
	while (buf) {
		char *media_role_str;
		enum media_role media_role;

		media_role_str = buf;
		strsep(&buf, ",");

		media_role = media_role_from_string(media_role_str);
		if (media_role != -1) {
			media_roles |= MEDIA_ROLE_CAMERA;
		}
		else {
			pw_log_debug("Client specified unknown media role '%s'",
				     media_role_str);
		}
	}
	free(buf_orig);

	return media_roles;
}

static enum media_role media_role_from_properties(const struct pw_properties *props)
{
	const char *media_class_str;
	const char *media_role_str;

	media_class_str = pw_properties_get(props, "media.class");
	media_role_str = pw_properties_get(props, "media.role");

	if (media_class_str == NULL)
		return -1;

	if (media_role_str == NULL)
		return -1;

	if (strcmp(media_class_str, "Video/Source") != 0)
		return -1;

	return media_role_from_string(media_role_str);
}

static void check_portal_managed(struct client_info *cinfo)
{
	struct impl *impl = cinfo->impl;
	const struct pw_properties *props;

	if (impl->portal_pid == 0)
		return;

	props = pw_client_get_properties(cinfo->client);
	if (props) {
		const char *pid_str;
		pid_t pid;

		pid_str = pw_properties_get(props, PW_CLIENT_PROP_UCRED_PID);

		pid = atoi(pid_str);

		if (pid == impl->portal_pid) {
			cinfo->portal_managed = true;

			pw_log_debug("module %p: portal managed client %p added",
				     impl, cinfo->client);
		}
	}
}

static int
set_global_permissions(void *data, struct pw_global *global)
{
	struct client_info *cinfo = data;
	struct impl *impl = cinfo->impl;
	struct pw_client *client = cinfo->client;
	const struct pw_properties *props;
	struct spa_dict_item items[1];
	int n_items = 0;
	char perms[16];
	bool set_permission;
	bool allowed = false;

	props = pw_global_get_properties(global);

	if (pw_global_get_type(global) == impl->type->core) {
		set_permission = true;
		allowed = true;
	}
	else if (props) {
		if (pw_global_get_type(global) == impl->type->factory) {
			const char *factory_name;

			factory_name = pw_properties_get(props, "factory.name");
			if (factory_name &&
			    strcmp(factory_name, "client-node") == 0) {
				set_permission = true;
				allowed = true;
			}
			else {
				set_permission = false;
			}
		}
		else if (pw_global_get_type(global) == impl->type->module) {
			set_permission = true;
			allowed = true;
		}
		else if (pw_global_get_type(global) == impl->type->node) {
			enum media_role media_role;

			media_role = media_role_from_properties(props);

			if (media_role == -1) {
				set_permission = false;
			}
			else if (cinfo->allowed_media_roles & media_role) {
				set_permission = true;
				allowed = true;
			}
			else if (cinfo->media_roles & media_role) {
				set_permission = true;
				allowed = false;
			}
			else {
				set_permission = false;
			}
		}
		else {
			set_permission = false;
		}
	}
	else {
		set_permission = false;
	}

	if (set_permission) {
		snprintf(perms, sizeof(perms),
			 "%d:%c--", pw_global_get_id(global), allowed ? 'r' : '-');
		items[n_items++] =
			SPA_DICT_ITEM_INIT(PW_CORE_PROXY_PERMISSIONS_GLOBAL,
					   perms);
		pw_client_update_permissions(client,
					     &SPA_DICT_INIT(items, n_items));
	}

	return 0;
}

static bool
check_permission_allowed(DBusMessageIter *iter)
{
	bool allowed = false;

	while (dbus_message_iter_get_arg_type (iter) != DBUS_TYPE_INVALID) {
		const char *permission_value;

		dbus_message_iter_get_basic(iter, &permission_value);

		if (strcmp(permission_value, "yes") == 0) {
			allowed = true;
			break;
		}
		dbus_message_iter_next(iter);
	}

	return allowed;
}

static void do_permission_store_check(struct client_info *cinfo)
{
	struct impl *impl = cinfo->impl;
	struct pw_client *client = cinfo->client;
	DBusMessage *m = NULL, *r = NULL;
	DBusError error;
	DBusMessageIter msg_iter;
	const char *table;
	const char *id;
	DBusMessageIter r_iter;
	DBusMessageIter permissions_iter;

	if (cinfo->app_id == NULL) {
		pw_log_debug("Ignoring portal check for broken portal managed client %p",
			     client);
		goto err_not_allowed;
	}

	if (cinfo->media_roles == 0) {
		pw_log_debug("Ignoring portal check for portal client %p with static permissions",
			     client);
		pw_core_for_each_global(cinfo->impl->core,
					set_global_permissions,
					cinfo);
		return;
	}

	if (strcmp(cinfo->app_id, "") == 0) {
		pw_log_debug("Ignoring portal check for non-sandboxed portal client %p",
			     client);
		cinfo->allowed_media_roles = MEDIA_ROLE_ALL;
		pw_core_for_each_global(cinfo->impl->core,
					set_global_permissions,
					cinfo);
		return;
	}

	cinfo->allowed_media_roles = MEDIA_ROLE_NONE;

	dbus_error_init(&error);

	m = dbus_message_new_method_call("org.freedesktop.impl.portal.PermissionStore",
					 "/org/freedesktop/impl/portal/PermissionStore",
					 "org.freedesktop.impl.portal.PermissionStore",
					 "Lookup");

	dbus_message_iter_init_append(m, &msg_iter);
	table = "devices";
	dbus_message_iter_append_basic(&msg_iter, DBUS_TYPE_STRING, &table);
	id = "camera";
	dbus_message_iter_append_basic(&msg_iter, DBUS_TYPE_STRING, &id);

	if (!(r = dbus_connection_send_with_reply_and_block(impl->bus, m, -1, &error))) {
		pw_log_error("Failed to call permission store: %s", error.message);
		dbus_error_free(&error);
		goto err_not_allowed;
	}

	dbus_message_unref(m);

	dbus_message_iter_init(r, &r_iter);
	dbus_message_iter_recurse(&r_iter, &permissions_iter);
	while (dbus_message_iter_get_arg_type(&permissions_iter) !=
	       DBUS_TYPE_INVALID) {
		DBusMessageIter permissions_entry_iter;
		const char *app_id;
		DBusMessageIter permission_values_iter;
		bool camera_allowed;

		dbus_message_iter_recurse(&permissions_iter,
					  &permissions_entry_iter);
		dbus_message_iter_get_basic(&permissions_entry_iter, &app_id);

		if (strcmp(app_id, cinfo->app_id) != 0) {
			dbus_message_iter_next(&permissions_iter);
			continue;
		}

		dbus_message_iter_next(&permissions_entry_iter);
		dbus_message_iter_recurse(&permissions_entry_iter,
					  &permission_values_iter);

		camera_allowed = check_permission_allowed(&permission_values_iter);
		cinfo->allowed_media_roles |=
			camera_allowed ? MEDIA_ROLE_CAMERA : MEDIA_ROLE_NONE;
		pw_core_for_each_global(cinfo->impl->core,
					set_global_permissions,
					cinfo);

		break;
	}

	dbus_message_unref(r);

	return;

      err_not_allowed:
	pw_resource_error(pw_client_get_core_resource(client), -EPERM, "not allowed");
	return;
}

static void client_info_changed(void *data, struct pw_client_info *info)
{
	struct client_info *cinfo = data;
	const struct pw_properties *properties;
	const char *is_portal;
	const char *app_id;
	const char *media_roles;

	if (!cinfo->portal_managed)
		return;

	if (info->props == NULL)
		return;

	if (cinfo->setup_complete)
		return;
	cinfo->setup_complete = true;

	properties = pw_client_get_properties(cinfo->client);
	if (properties == NULL) {
		pw_log_error("Portal managed client didn't have any properties");
		return;
	}

	is_portal = pw_properties_get(properties,
				      "pipewire.access.portal.is_portal");
	if (is_portal != NULL && strcmp(is_portal, "yes") == 0) {
		pw_log_debug("module %p: client %p is the portal itself",
			     cinfo->impl, cinfo->client);
		cinfo->is_portal = true;
		return;
	};

	app_id = pw_properties_get(properties,
				   "pipewire.access.portal.app_id");
	if (app_id == NULL) {
		pw_log_error("Portal managed client didn't set app_id");
		return;
	}
	media_roles = pw_properties_get(properties,
					"pipewire.access.portal.media_roles");

	if (media_roles == NULL) {
		pw_log_error("Portal managed client didn't set media_roles");
		return;
	}

	cinfo->app_id = strdup(app_id);
	cinfo->media_roles = parse_media_roles(media_roles);

	pw_log_debug("module %p: client %p with app_id '%s' set to portal access",
		     cinfo->impl, cinfo->client, cinfo->app_id);
	do_permission_store_check(cinfo);
}

static const struct pw_client_events client_events = {
	PW_VERSION_CLIENT_EVENTS,
	.info_changed = client_info_changed
};

static void
core_global_added(void *data, struct pw_global *global)
{
	struct impl *impl = data;
	struct client_info *cinfo;

	if (pw_global_get_type(global) == impl->type->client) {
		struct pw_client *client = pw_global_get_object(global);

		cinfo = calloc(1, sizeof(struct client_info));
		cinfo->impl = impl;
		cinfo->client = client;
		pw_client_add_listener(client, &cinfo->client_listener, &client_events, cinfo);

		spa_list_append(&impl->client_list, &cinfo->link);

		check_portal_managed(cinfo);
	}
	else {
		spa_list_for_each(cinfo, &impl->client_list, link) {
			if (cinfo->portal_managed &&
			    !cinfo->is_portal)
				set_global_permissions(cinfo, global);
		}
	}
}

static void
core_global_removed(void *data, struct pw_global *global)
{
	struct impl *impl = data;

	if (pw_global_get_type(global) == impl->type->client) {
		struct pw_client *client = pw_global_get_object(global);
		struct client_info *cinfo;

		if ((cinfo = find_client_info(impl, client)))
			client_info_free(cinfo);

		pw_log_debug("module %p: client %p removed", impl, client);
	}
}

static const struct pw_core_events core_events = {
	PW_VERSION_CORE_EVENTS,
	.global_added = core_global_added,
	.global_removed = core_global_removed,
};

static void module_destroy(void *data)
{
	struct impl *impl = data;
	struct client_info *info, *t;

	spa_hook_remove(&impl->core_listener);
	spa_hook_remove(&impl->module_listener);

	spa_dbus_connection_destroy(impl->conn);

	spa_list_for_each_safe(info, t, &impl->client_list, link)
		client_info_free(info);

	if (impl->properties)
		pw_properties_free(impl->properties);

	free(impl);
}

static const struct pw_module_events module_events = {
	PW_VERSION_MODULE_EVENTS,
	.destroy = module_destroy,
};

static void on_portal_pid_received(DBusPendingCall *pending,
				   void *user_data)
{
	struct impl *impl = user_data;
	DBusMessage *m;
	DBusError error;
	uint32_t portal_pid = 0;

	m = dbus_pending_call_steal_reply(pending);
	dbus_pending_call_unref(pending);
	impl->portal_pid_pending = NULL;

	if (!m) {
		pw_log_error("Failed to receive portal pid");
		return;
	}

	dbus_error_init(&error);
	dbus_message_get_args(m, &error, DBUS_TYPE_UINT32, &portal_pid,
			      DBUS_TYPE_INVALID);
	dbus_message_unref(m);

	if (dbus_error_is_set(&error)) {
		impl->portal_pid = 0;
	}
	else {
		struct client_info *cinfo;

		impl->portal_pid = portal_pid;

		spa_list_for_each(cinfo, &impl->client_list, link) {
			if (cinfo->portal_managed)
				continue;

			check_portal_managed(cinfo);
		}
	}
}

static void update_portal_pid(struct impl *impl)
{
	DBusMessage *m;
	const char *name;
	DBusPendingCall *pending;

	impl->portal_pid = 0;

	m = dbus_message_new_method_call("org.freedesktop.DBus",
					 "/",
					 "org.freedesktop.DBus",
					 "GetConnectionUnixProcessID");

	name = "org.freedesktop.portal.Desktop";
	dbus_message_append_args(m,
				 DBUS_TYPE_STRING, &name,
				 DBUS_TYPE_INVALID);

	dbus_connection_send_with_reply(impl->bus, m, &pending, -1);
	dbus_pending_call_set_notify(pending, on_portal_pid_received, impl, NULL);
	if (impl->portal_pid_pending != NULL) {
		dbus_pending_call_cancel(impl->portal_pid_pending);
		dbus_pending_call_unref(impl->portal_pid_pending);
	}
	impl->portal_pid_pending = pending;
}

static DBusHandlerResult name_owner_changed_handler(DBusConnection *connection,
						    DBusMessage *message,
						    void *user_data)
{
	struct impl *impl = user_data;
	const char *name;
	const char *old_owner;
	const char *new_owner;

	if (!dbus_message_is_signal(message, "org.freedesktop.DBus",
				   "NameOwnerChanged"))
		return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;

	if (!dbus_message_get_args(message, NULL,
				   DBUS_TYPE_STRING, &name,
				   DBUS_TYPE_STRING, &old_owner,
				   DBUS_TYPE_STRING, &new_owner,
				   DBUS_TYPE_INVALID)) {
		pw_log_error("Failed to get OwnerChanged args");
		return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
	}

	if (strcmp(name, "org.freedesktop.portal.Desktop") != 0)
		return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;

	if (strcmp(new_owner, "") == 0) {
		impl->portal_pid = 0;
		if (impl->portal_pid_pending != NULL) {
			dbus_pending_call_cancel(impl->portal_pid_pending);
			dbus_pending_call_unref(impl->portal_pid_pending);
		}
	}
	else {
		update_portal_pid(impl);
	}

	return DBUS_HANDLER_RESULT_HANDLED;
}

static DBusHandlerResult permission_store_changed_handler(DBusConnection *connection,
							  DBusMessage *message,
							  void *user_data)
{
	struct impl *impl = user_data;
	struct client_info *cinfo;
	DBusMessageIter iter;
	const char *table;
	const char *id;
	dbus_bool_t deleted;
	DBusMessageIter permissions_iter;

	if (!dbus_message_is_signal(message, "org.freedesktop.impl.portal.PermissionStore",
				   "Changed"))
		return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;

	spa_list_for_each(cinfo, &impl->client_list, link) {
		if (!cinfo->portal_managed)
			continue;

		cinfo->allowed_media_roles = MEDIA_ROLE_NONE;
	}

	dbus_message_iter_init(message, &iter);
	dbus_message_iter_get_basic(&iter, &table);

	dbus_message_iter_next(&iter);
	dbus_message_iter_get_basic(&iter, &id);

	if (strcmp(table, "devices") != 0 || strcmp(id, "camera") != 0)
		return DBUS_HANDLER_RESULT_HANDLED;

	dbus_message_iter_next(&iter);
	dbus_message_iter_get_basic(&iter, &deleted);

	dbus_message_iter_next(&iter);
	/* data variant (ignored) */

	dbus_message_iter_next(&iter);
	dbus_message_iter_recurse(&iter, &permissions_iter);
	while (dbus_message_iter_get_arg_type(&permissions_iter) !=
	       DBUS_TYPE_INVALID) {
		DBusMessageIter permissions_entry_iter;
		const char *app_id;
		DBusMessageIter permission_values_iter;
		bool camera_allowed;

		dbus_message_iter_recurse(&permissions_iter,
					  &permissions_entry_iter);
		dbus_message_iter_get_basic(&permissions_entry_iter, &app_id);

		dbus_message_iter_next(&permissions_entry_iter);
		dbus_message_iter_recurse(&permissions_entry_iter,
					  &permission_values_iter);

		camera_allowed = check_permission_allowed(&permission_values_iter);

		spa_list_for_each(cinfo, &impl->client_list, link) {
			if (!cinfo->portal_managed)
				continue;

			if (cinfo->is_portal)
				continue;

			if (cinfo->app_id == NULL ||
			    strcmp(cinfo->app_id, app_id) != 0)
				continue;

			if (!(cinfo->media_roles & MEDIA_ROLE_CAMERA))
				continue;

			if (camera_allowed)
				cinfo->allowed_media_roles |= MEDIA_ROLE_CAMERA;
			pw_core_for_each_global(cinfo->impl->core,
						set_global_permissions,
						cinfo);
		}

		dbus_message_iter_next(&permissions_iter);
	}

	return DBUS_HANDLER_RESULT_HANDLED;
}

static int init_dbus_connection(struct impl *impl)
{
	DBusError error;

	impl->bus = spa_dbus_connection_get(impl->conn);

	dbus_error_init(&error);

	dbus_bus_add_match(impl->bus,
			   "type='signal',\
			   sender='org.freedesktop.DBus',\
			   interface='org.freedesktop.DBus',\
			   member='NameOwnerChanged'",
			   &error);
	if (dbus_error_is_set(&error)) {
		pw_log_error("Failed to add name owner changed listener: %s",
			     error.message);
		dbus_error_free(&error);
		return -1;
	}

	dbus_bus_add_match(impl->bus,
			   "type='signal',\
			   sender='org.freedesktop.impl.portal.PermissionStore',\
			   interface='org.freedesktop.impl.portal.PermissionStore',\
			   member='Changed'",
			   &error);
	if (dbus_error_is_set(&error)) {
		pw_log_error("Failed to add permission store changed listener: %s",
			     error.message);
		dbus_error_free(&error);
		return -1;
	}

	dbus_connection_add_filter(impl->bus, name_owner_changed_handler,
				   impl, NULL);
	dbus_connection_add_filter(impl->bus, permission_store_changed_handler,
				   impl, NULL);
	update_portal_pid(impl);

	return 0;
}

static int module_init(struct pw_module *module, struct pw_properties *properties)
{
	struct pw_core *core = pw_module_get_core(module);
	struct impl *impl;
	struct spa_dbus *dbus;
	const struct spa_support *support;
	uint32_t n_support;

	support = pw_core_get_support(core, &n_support);

	dbus = spa_support_find(support, n_support, SPA_TYPE__DBus);
        if (dbus == NULL)
                return -ENOTSUP;

	impl = calloc(1, sizeof(struct impl));
	if (impl == NULL)
		return -ENOMEM;

	pw_log_debug("module %p: new", impl);

	impl->core = core;
	impl->type = pw_core_get_type(core);
	impl->properties = properties;

	impl->conn = spa_dbus_get_connection(dbus, SPA_DBUS_TYPE_SESSION);
	if (impl->conn == NULL)
		goto error;

	if (init_dbus_connection(impl) != 0)
		goto error;

	spa_list_init(&impl->client_list);

	pw_core_add_listener(core, &impl->core_listener, &core_events, impl);
	pw_module_add_listener(module, &impl->module_listener, &module_events, impl);

	return 0;

      error:
	free(impl);
	pw_log_error("Failed to connect to system bus");
	return -ENOMEM;
}

SPA_EXPORT
int pipewire__module_init(struct pw_module *module, const char *args)
{
	return module_init(module, NULL);
}
