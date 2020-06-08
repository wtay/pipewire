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

#define _GNU_SOURCE         /* See feature_test_macros(7) */

#include <unistd.h>
#include <errno.h>
#include <stdio.h>
#include <signal.h>
#include <string.h>

#include <spa/debug/pod.h>
#include <spa/debug/format.h>

#include <pipewire/pipewire.h>
#include <pipewire/command.h>
#include <pipewire/interfaces.h>
#include <pipewire/type.h>

static const char WHITESPACE[] = " \t";

struct remote_data;

struct data {
	struct pw_main_loop *loop;
	struct pw_core *core;
	struct pw_type *t;

	struct spa_list remotes;
	struct remote_data *current;

	struct pw_map vars;
};

struct global {
	struct remote_data *rd;
	uint32_t id;
	uint32_t parent_id;
	uint32_t permissions;
	uint32_t type;
	uint32_t version;
	struct pw_proxy *proxy;
	bool info_pending;
	struct pw_properties *properties;
};

struct remote_data {
	struct spa_list link;
	struct data *data;

	const char *name;
	uint32_t id;

	struct pw_remote *remote;
	struct spa_hook remote_listener;

	struct pw_core_proxy *core_proxy;
	struct pw_registry_proxy *registry_proxy;
	struct spa_hook registry_listener;

	struct pw_map globals;
};

struct proxy_data;

typedef void (*info_func_t) (struct proxy_data *pd);

struct proxy_data {
	struct remote_data *rd;
	struct global *global;
	struct pw_proxy *proxy;
        void *info;
	info_func_t info_func;
        pw_destroy_t destroy;
        struct spa_hook proxy_listener;
        struct spa_hook proxy_proxy_listener;
};

struct command {
	const char *name;
	const char *description;
	bool (*func) (struct data *data, const char *cmd, char *args, char **error);
};

static int pw_split_ip(char *str, const char *delimiter, int max_tokens, char *tokens[])
{
	const char *state = NULL;
	char *s;
	size_t len;
	int n = 0;

        s = (char *)pw_split_walk(str, delimiter, &len, &state);
        while (s && n + 1 < max_tokens) {
		s[len] = '\0';
		tokens[n++] = s;
                s = (char*)pw_split_walk(str, delimiter, &len, &state);
        }
        if (s) {
		tokens[n++] = s;
        }
        return n;
}

static struct pw_properties *parse_props(char *str)
{
	const char *state = NULL;
	char *s, *p[3];
	size_t len, n;
	struct pw_properties *props = NULL;

	while (true) {
		s = (char *)pw_split_walk(str, WHITESPACE, &len, &state);
		if (s == NULL)
			break;

		s[len] = '\0';
		n = pw_split_ip(s, "=", 2, p);
		if (n == 2) {
			if (props == NULL)
				props = pw_properties_new(p[0], p[1], NULL);
			else
				pw_properties_set(props, p[0], p[1]);
		}
	}
	return props;
}

static void print_properties(struct spa_dict *props, char mark, bool header)
{
	const struct spa_dict_item *item;

	if (header)
		fprintf(stdout, "%c\tproperties:\n", mark);
	if (props == NULL || props->n_items == 0) {
		if (header)
			fprintf(stdout, "\t\tnone\n");
		return;
	}

	spa_dict_for_each(item, props) {
		fprintf(stdout, "%c\t\t%s = \"%s\"\n", mark, item->key, item->value);
	}
}

static bool do_not_implemented(struct data *data, const char *cmd, char *args, char **error)
{
        asprintf(error, "Command \"%s\" not yet implemented", cmd);
	return false;
}

static bool do_help(struct data *data, const char *cmd, char *args, char **error);
static bool do_load_module(struct data *data, const char *cmd, char *args, char **error);
static bool do_list_objects(struct data *data, const char *cmd, char *args, char **error);
static bool do_connect(struct data *data, const char *cmd, char *args, char **error);
static bool do_disconnect(struct data *data, const char *cmd, char *args, char **error);
static bool do_list_remotes(struct data *data, const char *cmd, char *args, char **error);
static bool do_switch_remote(struct data *data, const char *cmd, char *args, char **error);
static bool do_info(struct data *data, const char *cmd, char *args, char **error);
static bool do_create_node(struct data *data, const char *cmd, char *args, char **error);
static bool do_destroy(struct data *data, const char *cmd, char *args, char **error);
static bool do_create_link(struct data *data, const char *cmd, char *args, char **error);
static bool do_export_node(struct data *data, const char *cmd, char *args, char **error);
static bool do_node_params(struct data *data, const char *cmd, char *args, char **error);
static bool do_port_params(struct data *data, const char *cmd, char *args, char **error);

static struct command command_list[] = {
	{ "help", "Show this help", do_help },
	{ "load-module", "Load a module. <module-name> [<module-arguments>]", do_load_module },
	{ "unload-module", "Unload a module. <module-var>", do_not_implemented },
	{ "connect", "Connect to a remote. [<remote-name>]", do_connect },
	{ "disconnect", "Disconnect from a remote. [<remote-var>]", do_disconnect },
	{ "list-remotes", "List connected remotes.", do_list_remotes },
	{ "switch-remote", "Switch between current remotes. [<remote-var>]", do_switch_remote },
	{ "list-objects", "List objects or current remote.", do_list_objects },
	{ "info", "Get info about an object. <object-id>|all", do_info },
	{ "create-node", "Create a node from a factory. <factory-name> [<properties>]", do_create_node },
	{ "destroy", "Destroy a global object. <object-id>", do_destroy },
	{ "create-link", "Create a link between nodes. <node-id> <port-id> <node-id> <port-id> [<properties>]", do_create_link },
	{ "export-node", "Export a local node to the current remote. <node-id> [remote-var]", do_export_node },
	{ "node-params", "Enumerate params of a node <node-id> [<param-id-name>]", do_node_params },
	{ "port-params", "Enumerate params of a port <port-id> [<param-id-name>]", do_port_params },
};

static bool do_help(struct data *data, const char *cmd, char *args, char **error)
{
	int i;

	fprintf(stdout, "Available commands:\n");
	for (i = 0; i < SPA_N_ELEMENTS(command_list); i++) {
		fprintf(stdout, "\t%-20.20s\t%s\n", command_list[i].name, command_list[i].description);
	}
	return true;
}

static bool do_load_module(struct data *data, const char *cmd, char *args, char **error)
{
	struct pw_module *module;
	char *a[2];
	int n;
	uint32_t id;

	n = pw_split_ip(args, WHITESPACE, 2, a);
	if (n < 1) {
		asprintf(error, "%s <module-name> [<module-arguments>]", cmd);
		return false;
	}

	module = pw_module_load(data->core, a[0], n == 2 ? a[1] : NULL, NULL, NULL, NULL);
	if (module == NULL) {
		asprintf(error, "Could not load module");
		return false;
	}

	id = pw_map_insert_new(&data->vars, module);
	fprintf(stdout, "%d = @module:%d\n", id, pw_global_get_id(pw_module_get_global(module)));

	return true;
}

static void on_info_changed(void *_data, const struct pw_core_info *info)
{
	struct remote_data *rd = _data;
	rd->name = info->name;
	fprintf(stdout, "remote %d is named '%s'\n", rd->id, rd->name);
}

static void show_prompt(struct remote_data *rd)
{
	fprintf(stdout, "%s>>", rd->name);
	fflush(stdout);
}

static void on_sync_reply(void *_data, uint32_t seq)
{
	struct remote_data *rd = _data;

	switch (seq) {
	case 1:
		show_prompt(rd);
		break;
	default:
		break;
	}
}

static int print_global(void *obj, void *data)
{
	struct global *global = obj;
	struct pw_type *t;

	if (global == NULL)
		return 0;

	t = global->rd->data->t;

	fprintf(stdout, "\tid %d, parent %d, type %s/%d\n", global->id, global->parent_id,
					spa_type_map_get_type(t->map, global->type),
					global->version);
	if (global->properties)
		print_properties(&global->properties->dict, ' ', false);

	return 0;
}

static void registry_event_global(void *data, uint32_t id, uint32_t parent_id,
				  uint32_t permissions, uint32_t type, uint32_t version,
				  const struct spa_dict *props)
{
	struct remote_data *rd = data;
	struct global *global;
	size_t size;

	global = calloc(1, sizeof(struct global));
	global->rd = rd;
	global->id = id;
	global->parent_id = parent_id;
	global->permissions = permissions;
	global->type = type;
	global->version = version;
	global->properties = props ? pw_properties_new_dict(props) : NULL;

	fprintf(stdout, "remote %d added global: ", rd->id);
	print_global(global, NULL);

	size = pw_map_get_size(&rd->globals);
	while (id > size)
		pw_map_insert_at(&rd->globals, size++, NULL);
	pw_map_insert_at(&rd->globals, id, global);
}

static int destroy_global(void *obj, void *data)
{
	struct global *global = obj;

	if (global == NULL)
		return 0;

	pw_map_remove(&global->rd->globals, global->id);
	if (global->properties)
		pw_properties_free(global->properties);
	free(global);
	return 0;
}

static void registry_event_global_remove(void *data, uint32_t id)
{
	struct remote_data *rd = data;
	struct global *global;

	global = pw_map_lookup(&rd->globals, id);
	if (global == NULL) {
		fprintf(stdout, "remote %d removed unknown global %d\n", rd->id, id);
		return;
	}

	fprintf(stdout, "remote %d removed global: ", rd->id);
	print_global(global, NULL);
	destroy_global(global, rd);
}

static const struct pw_registry_proxy_events registry_events = {
	PW_VERSION_REGISTRY_PROXY_EVENTS,
	.global = registry_event_global,
	.global_remove = registry_event_global_remove,
};


static void on_remote_destroy(void *_data)
{
	struct remote_data *rd = _data;
	struct data *data = rd->data;

	spa_list_remove(&rd->link);

	pw_map_remove(&data->vars, rd->id);
	pw_map_for_each(&rd->globals, destroy_global, rd);

	if (data->current == rd)
		data->current = NULL;
}

static void on_state_changed(void *_data, enum pw_remote_state old,
			     enum pw_remote_state state, const char *error)
{
	struct remote_data *rd = _data;
	struct data *data = rd->data;
	struct pw_type *t = data->t;

	switch (state) {
	case PW_REMOTE_STATE_ERROR:
		fprintf(stderr, "remote %d error: %s\n", rd->id, error);
		pw_main_loop_quit(data->loop);
		break;

	case PW_REMOTE_STATE_CONNECTED:
		fprintf(stdout, "remote %d state: \"%s\"\n", rd->id, pw_remote_state_as_string(state));
		rd->core_proxy = pw_remote_get_core_proxy(rd->remote);
		rd->registry_proxy = pw_core_proxy_get_registry(rd->core_proxy,
								t->registry,
								PW_VERSION_REGISTRY, 0);
		pw_registry_proxy_add_listener(rd->registry_proxy,
					       &rd->registry_listener,
					       &registry_events, rd);
		pw_core_proxy_sync(rd->core_proxy, 1);
		break;

	default:
		fprintf(stdout, "remote %d state: \"%s\"\n", rd->id, pw_remote_state_as_string(state));
		break;
	}
}

static const struct pw_remote_events remote_events = {
	PW_VERSION_REMOTE_EVENTS,
	.destroy = on_remote_destroy,
	.info_changed = on_info_changed,
	.sync_reply = on_sync_reply,
	.state_changed = on_state_changed,
};


static bool do_connect(struct data *data, const char *cmd, char *args, char **error)
{
	char *a[1];
        int n;
	struct pw_remote *remote;
	struct pw_properties *props = NULL;
	struct remote_data *rd;

	n = pw_split_ip(args, WHITESPACE, 1, a);
	if (n == 1) {
		props = pw_properties_new(PW_REMOTE_PROP_REMOTE_NAME, a[0], NULL);
	}
	remote = pw_remote_new(data->core, props, sizeof(struct remote_data));

	rd = pw_remote_get_user_data(remote);
	rd->remote = remote;
	rd->data = data;
	pw_map_init(&rd->globals, 64, 16);
	rd->id = pw_map_insert_new(&data->vars, rd);
	spa_list_append(&data->remotes, &rd->link);

	fprintf(stdout, "%d = @remote:%p\n", rd->id, remote);
	data->current = rd;

	pw_remote_add_listener(remote, &rd->remote_listener, &remote_events, rd);
	pw_remote_connect(remote);

	return true;
}

static bool do_disconnect(struct data *data, const char *cmd, char *args, char **error)
{
	char *a[1];
        int n;
	uint32_t idx;
	struct remote_data *rd = data->current;

	n = pw_split_ip(args, WHITESPACE, 1, a);
	if (n >= 1) {
		idx = atoi(a[0]);
		rd = pw_map_lookup(&data->vars, idx);
		if (rd == NULL)
			goto no_remote;

	}
	pw_remote_disconnect(rd->remote);
	pw_remote_destroy(rd->remote);

	if (data->current == NULL) {
		if (spa_list_is_empty(&data->remotes)) {
			return true;
		}
		data->current = spa_list_last(&data->remotes, struct remote_data, link);
	}

	return true;

      no_remote:
        asprintf(error, "Remote %d does not exist", idx);
	return false;
}

static bool do_list_remotes(struct data *data, const char *cmd, char *args, char **error)
{
	struct remote_data *rd;

	spa_list_for_each(rd, &data->remotes, link)
		fprintf(stdout, "\t%d = @remote:%p '%s'\n", rd->id, rd->remote, rd->name);

	return true;
}

static bool do_switch_remote(struct data *data, const char *cmd, char *args, char **error)
{
	char *a[1];
        int n, idx = 0;
	struct remote_data *rd;

	n = pw_split_ip(args, WHITESPACE, 1, a);
	if (n == 1)
		idx = atoi(a[0]);

	rd = pw_map_lookup(&data->vars, idx);
	if (rd == NULL)
		goto no_remote;

	spa_list_remove(&rd->link);
	spa_list_append(&data->remotes, &rd->link);
	data->current = rd;

	return true;

      no_remote:
        asprintf(error, "Remote %d does not exist", idx);
	return false;
}

#define MARK_CHANGE(f) ((((info)->change_mask & (1 << (f)))) ? '*' : ' ')

static void info_global(struct proxy_data *pd)
{
	struct global *global = pd->global;
	struct pw_type *t = pd->rd->data->t;

	if (global == NULL)
		return;

	fprintf(stdout, "\tid: %d\n", global->id);
	fprintf(stdout, "\tparent_id: %d\n", global->parent_id);
	fprintf(stdout, "\tpermissions: %c%c%c\n", global->permissions & PW_PERM_R ? 'r' : '-',
					  global->permissions & PW_PERM_W ? 'w' : '-',
					  global->permissions & PW_PERM_X ? 'x' : '-');
	fprintf(stdout, "\ttype: %s/%d\n", spa_type_map_get_type(t->map, global->type), pd->global->version);
}

static void info_core(struct proxy_data *pd)
{
	struct pw_core_info *info = pd->info;

	info_global(pd);
	fprintf(stdout, "%c\tuser-name: \"%s\"\n", MARK_CHANGE(0), info->user_name);
	fprintf(stdout, "%c\thost-name: \"%s\"\n", MARK_CHANGE(1), info->host_name);
	fprintf(stdout, "%c\tversion: \"%s\"\n", MARK_CHANGE(2), info->version);
	fprintf(stdout, "%c\tname: \"%s\"\n", MARK_CHANGE(3), info->name);
	fprintf(stdout, "%c\tcookie: %u\n", MARK_CHANGE(4), info->cookie);
	print_properties(info->props, MARK_CHANGE(5), true);
	info->change_mask = 0;
}

static void info_module(struct proxy_data *pd)
{
	struct pw_module_info *info = pd->info;

	info_global(pd);
	fprintf(stdout, "%c\tname: \"%s\"\n", MARK_CHANGE(0), info->name);
	fprintf(stdout, "%c\tfilename: \"%s\"\n", MARK_CHANGE(1), info->filename);
	fprintf(stdout, "%c\targs: \"%s\"\n", MARK_CHANGE(2), info->args);
	print_properties(info->props, MARK_CHANGE(3), true);
	info->change_mask = 0;
}

static void info_node(struct proxy_data *pd)
{
	struct pw_node_info *info = pd->info;

	info_global(pd);
	fprintf(stdout, "%c\tname: \"%s\"\n", MARK_CHANGE(0), info->name);
	fprintf(stdout, "%c\tinput ports: %u/%u\n", MARK_CHANGE(1),
			info->n_input_ports, info->max_input_ports);
	fprintf(stdout, "%c\toutput ports: %u/%u\n", MARK_CHANGE(2),
			info->n_output_ports, info->max_output_ports);
	fprintf(stdout, "%c\tstate: \"%s\"", MARK_CHANGE(3), pw_node_state_as_string(info->state));
	if (info->state == PW_NODE_STATE_ERROR && info->error)
		fprintf(stdout, " \"%s\"\n", info->error);
	else
		fprintf(stdout, "\n");
	print_properties(info->props, MARK_CHANGE(4), true);
	fprintf(stdout, "%c\tenum_params\n", MARK_CHANGE(5));
	info->change_mask = 0;
}

static void info_port(struct proxy_data *pd)
{
	struct pw_port_info *info = pd->info;

	info_global(pd);
	fprintf(stdout, "%c\tname: \"%s\"\n", MARK_CHANGE(0), info->name);
	print_properties(info->props, MARK_CHANGE(1), true);
	fprintf(stdout, "%c\tenum_params\n", MARK_CHANGE(2));
	info->change_mask = 0;
}

static void info_factory(struct proxy_data *pd)
{
	struct pw_factory_info *info = pd->info;
	struct pw_type *t = pd->rd->data->t;

	info_global(pd);
	fprintf(stdout, "\tname: \"%s\"\n", info->name);
	fprintf(stdout, "\tobject-type: %s/%d\n", spa_type_map_get_type(t->map, info->type), info->version);
	print_properties(info->props, MARK_CHANGE(0), true);
	info->change_mask = 0;
}

static void info_client(struct proxy_data *pd)
{
	struct pw_client_info *info = pd->info;

	info_global(pd);
	print_properties(info->props, MARK_CHANGE(0), true);
	info->change_mask = 0;
}

static void info_link(struct proxy_data *pd)
{
	struct pw_link_info *info = pd->info;

	info_global(pd);
	fprintf(stdout, "%c\toutput-node-id: %u\n", MARK_CHANGE(0), info->output_node_id);
	fprintf(stdout, "%c\toutput-port-id: %u\n", MARK_CHANGE(0), info->output_port_id);
	fprintf(stdout, "%c\tinput-node-id: %u\n", MARK_CHANGE(1), info->input_node_id);
	fprintf(stdout, "%c\tinput-port-id: %u\n", MARK_CHANGE(1), info->input_port_id);
	fprintf(stdout, "%c\tformat:\n", MARK_CHANGE(2));
	if (info->format)
		spa_debug_format(2, pd->rd->data->t->map, info->format);
	else
		fprintf(stdout, "\t\tnone\n");
	print_properties(info->props, MARK_CHANGE(3), true);
	info->change_mask = 0;
}


static void core_event_info(void *object, struct pw_core_info *info)
{
	struct proxy_data *pd = object;
	struct remote_data *rd = pd->rd;
	if (pd->info)
		fprintf(stdout, "remote %d core %d changed\n", rd->id, info->id);
	pd->info = pw_core_info_update(pd->info, info);
	if (pd->global == NULL)
		pd->global = pw_map_lookup(&rd->globals, info->id);
	if (pd->global && pd->global->info_pending) {
		info_core(pd);
		pd->global->info_pending = false;
	}
}

static const struct pw_core_proxy_events core_events = {
	PW_VERSION_CORE_PROXY_EVENTS,
	.info = core_event_info
};


static void module_event_info(void *object, struct pw_module_info *info)
{
	struct proxy_data *pd = object;
	struct remote_data *rd = pd->rd;
	if (pd->info)
		fprintf(stdout, "remote %d module %d changed\n", rd->id, info->id);
	pd->info = pw_module_info_update(pd->info, info);
	if (pd->global == NULL)
		pd->global = pw_map_lookup(&rd->globals, info->id);
	if (pd->global && pd->global->info_pending) {
		info_module(pd);
		pd->global->info_pending = false;
	}
}

static const struct pw_module_proxy_events module_events = {
	PW_VERSION_MODULE_PROXY_EVENTS,
	.info = module_event_info
};

static void node_event_info(void *object, struct pw_node_info *info)
{
	struct proxy_data *pd = object;
	struct remote_data *rd = pd->rd;
	if (pd->info)
		fprintf(stdout, "remote %d node %d changed\n", rd->id, info->id);
	pd->info = pw_node_info_update(pd->info, info);
	if (pd->global == NULL)
		pd->global = pw_map_lookup(&rd->globals, info->id);
	if (pd->global && pd->global->info_pending) {
		info_node(pd);
		pd->global->info_pending = false;
	}
}

static void node_event_param(void *object, uint32_t id, uint32_t index, uint32_t next,
		const struct spa_pod *param)
{
        struct proxy_data *data = object;
	struct remote_data *rd = data->rd;
	struct pw_type *t = rd->data->t;

	fprintf(stdout, "remote %d node %d param %d index %d\n",
			rd->id, data->global->id, id, index);

	if (spa_pod_is_object_type(param, t->spa_format))
		spa_debug_format(2, t->map, param);
	else
		spa_debug_pod(2, t->map, param);
}

static const struct pw_node_proxy_events node_events = {
	PW_VERSION_NODE_PROXY_EVENTS,
	.info = node_event_info,
	.param = node_event_param
};


static void port_event_info(void *object, struct pw_port_info *info)
{
	struct proxy_data *pd = object;
	struct remote_data *rd = pd->rd;
	if (pd->info)
		fprintf(stdout, "remote %d port %d changed\n", rd->id, info->id);
	pd->info = pw_port_info_update(pd->info, info);
	if (pd->global == NULL)
		pd->global = pw_map_lookup(&rd->globals, info->id);
	if (pd->global && pd->global->info_pending) {
		info_port(pd);
		pd->global->info_pending = false;
	}
}

static void port_event_param(void *object, uint32_t id, uint32_t index, uint32_t next,
		const struct spa_pod *param)
{
        struct proxy_data *data = object;
	struct remote_data *rd = data->rd;
	struct pw_type *t = rd->data->t;

	fprintf(stdout, "remote %d port %d param %d index %d\n",
			rd->id, data->global->id, id, index);

	if (spa_pod_is_object_type(param, t->spa_format))
		spa_debug_format(2, t->map, param);
	else
		spa_debug_pod(2, t->map, param);
}

static const struct pw_port_proxy_events port_events = {
	PW_VERSION_PORT_PROXY_EVENTS,
	.info = port_event_info,
	.param = port_event_param
};

static void factory_event_info(void *object, struct pw_factory_info *info)
{
	struct proxy_data *pd = object;
	struct remote_data *rd = pd->rd;
	if (pd->info)
		fprintf(stdout, "remote %d factory %d changed\n", rd->id, info->id);
	pd->info = pw_factory_info_update(pd->info, info);
	if (pd->global == NULL)
		pd->global = pw_map_lookup(&rd->globals, info->id);
	if (pd->global && pd->global->info_pending) {
		info_factory(pd);
		pd->global->info_pending = false;
	}
}

static const struct pw_factory_proxy_events factory_events = {
	PW_VERSION_FACTORY_PROXY_EVENTS,
	.info = factory_event_info
};

static void client_event_info(void *object, struct pw_client_info *info)
{
	struct proxy_data *pd = object;
	struct remote_data *rd = pd->rd;
	if (pd->info)
		fprintf(stdout, "remote %d client %d changed\n", rd->id, info->id);
	pd->info = pw_client_info_update(pd->info, info);
	if (pd->global == NULL)
		pd->global = pw_map_lookup(&rd->globals, info->id);
	if (pd->global && pd->global->info_pending) {
		info_client(pd);
		pd->global->info_pending = false;
	}
}

static const struct pw_client_proxy_events client_events = {
	PW_VERSION_CLIENT_PROXY_EVENTS,
	.info = client_event_info
};

static void link_event_info(void *object, struct pw_link_info *info)
{
	struct proxy_data *pd = object;
	struct remote_data *rd = pd->rd;
	if (pd->info)
		fprintf(stdout, "remote %d link %d changed\n", rd->id, info->id);
	pd->info = pw_link_info_update(pd->info, info);
	if (pd->global == NULL)
		pd->global = pw_map_lookup(&rd->globals, info->id);
	if (pd->global && pd->global->info_pending) {
		info_link(pd);
		pd->global->info_pending = false;
	}
}

static const struct pw_link_proxy_events link_events = {
	PW_VERSION_LINK_PROXY_EVENTS,
	.info = link_event_info
};

static void
destroy_proxy (void *data)
{
	struct proxy_data *pd = data;

	if (pd->info == NULL)
		return;

	if (pd->global)
		pd->global->proxy = NULL;

	if (pd->destroy)
		pd->destroy(pd->info);
	pd->info = NULL;
}

static const struct pw_proxy_events proxy_events = {
        PW_VERSION_PROXY_EVENTS,
        .destroy = destroy_proxy,
};

static bool do_list_objects(struct data *data, const char *cmd, char *args, char **error)
{
	struct remote_data *rd = data->current;
	pw_map_for_each(&rd->globals, print_global, NULL);
	return true;
}

static bool bind_global(struct remote_data *rd, struct global *global, char **error)
{
	struct pw_type *t = rd->data->t;
        const void *events;
        uint32_t client_version;
	info_func_t info_func;
        pw_destroy_t destroy;
	struct proxy_data *pd;
	struct pw_proxy *proxy;

	if (global->type == t->core) {
		events = &core_events;
		client_version = PW_VERSION_CORE;
		destroy = (pw_destroy_t) pw_core_info_free;
		info_func = info_core;
	}
	else if (global->type == t->module) {
		events = &module_events;
		client_version = PW_VERSION_MODULE;
		destroy = (pw_destroy_t) pw_module_info_free;
		info_func = info_module;
	}
	else if (global->type == t->node) {
		events = &node_events;
		client_version = PW_VERSION_NODE;
		destroy = (pw_destroy_t) pw_node_info_free;
		info_func = info_node;
	}
	else if (global->type == t->port) {
		events = &port_events;
		client_version = PW_VERSION_PORT;
		destroy = (pw_destroy_t) pw_port_info_free;
		info_func = info_port;
	}
	else if (global->type == t->factory) {
		events = &factory_events;
		client_version = PW_VERSION_FACTORY;
		destroy = (pw_destroy_t) pw_factory_info_free;
		info_func = info_factory;
	}
	else if (global->type == t->client) {
		events = &client_events;
		client_version = PW_VERSION_CLIENT;
		destroy = (pw_destroy_t) pw_client_info_free;
		info_func = info_client;
	}
	else if (global->type == t->link) {
		events = &link_events;
		client_version = PW_VERSION_LINK;
		destroy = (pw_destroy_t) pw_link_info_free;
		info_func = info_link;
	}
	else {
		asprintf(error, "unsupported type %s", spa_type_map_get_type(t->map, global->type));
		return false;
	}

	proxy = pw_registry_proxy_bind(rd->registry_proxy,
				       global->id,
				       global->type,
				       client_version,
				       sizeof(struct proxy_data));

	pd = pw_proxy_get_user_data(proxy);
	pd->rd = rd;
	pd->global = global;
	pd->proxy = proxy;
        pd->info_func = info_func;
        pd->destroy = destroy;
        pw_proxy_add_proxy_listener(proxy, &pd->proxy_proxy_listener, events, pd);
        pw_proxy_add_listener(proxy, &pd->proxy_listener, &proxy_events, pd);

	global->proxy = proxy;

	return true;
}

static bool do_global_info(struct global *global, char **error)
{
	struct remote_data *rd = global->rd;
	struct proxy_data *pd;

	if (global->proxy == NULL) {
		if (!bind_global(rd, global, error))
			return false;
		global->info_pending = true;
	} else {
		pd = pw_proxy_get_user_data(global->proxy);
		if (pd->info_func)
			pd->info_func(pd);
	}
	return true;
}
static int do_global_info_all(void *obj, void *data)
{
	struct global *global = obj;
	char *error;

	if (global == NULL)
		return 0;

	if (!do_global_info(global, &error)) {
		fprintf(stderr, "info: %s\n", error);
		free(error);
	}
	return 0;
}

static bool do_info(struct data *data, const char *cmd, char *args, char **error)
{
	struct remote_data *rd = data->current;
	char *a[1];
        int n;
	uint32_t id;
	struct global *global;

	n = pw_split_ip(args, WHITESPACE, 1, a);
	if (n < 1) {
		asprintf(error, "%s <object-id>|all", cmd);
		return false;
	}
	if (strcmp(a[0], "all") == 0) {
		pw_map_for_each(&rd->globals, do_global_info_all, NULL);
	}
	else {
		id = atoi(a[0]);
		global = pw_map_lookup(&rd->globals, id);
		if (global == NULL) {
			asprintf(error, "%s: unknown global %d", cmd, id);
			return false;
		}
		return do_global_info(global, error);
	}
	return true;
}

static bool do_create_node(struct data *data, const char *cmd, char *args, char **error)
{
	struct remote_data *rd = data->current;
	char *a[2];
        int n;
	uint32_t id;
	struct pw_type *t = data->t;
	struct pw_proxy *proxy;
	struct pw_properties *props = NULL;
	struct proxy_data *pd;

	n = pw_split_ip(args, WHITESPACE, 2, a);
	if (n < 1) {
		asprintf(error, "%s <factory-name> [<properties>]", cmd);
		return false;
	}
	if (n == 2)
		props = parse_props(a[1]);

	proxy = pw_core_proxy_create_object(rd->core_proxy, a[0],
					    t->node, PW_VERSION_NODE,
					    props ? &props->dict : NULL,
					    sizeof(struct proxy_data));

	pd = pw_proxy_get_user_data(proxy);
	pd->rd = rd;
	pd->proxy = proxy;
        pd->destroy = (pw_destroy_t) pw_node_info_free;
        pw_proxy_add_proxy_listener(proxy, &pd->proxy_proxy_listener, &node_events, pd);
        pw_proxy_add_listener(proxy, &pd->proxy_listener, &proxy_events, pd);

	id = pw_map_insert_new(&data->vars, proxy);
	fprintf(stdout, "%d = @proxy:%d\n", id, pw_proxy_get_id(proxy));

	return true;
}

static bool do_destroy(struct data *data, const char *cmd, char *args, char **error)
{
	struct remote_data *rd = data->current;
	char *a[1];
        int n;
	uint32_t id;
	struct global *global;

	n = pw_split_ip(args, WHITESPACE, 1, a);
	if (n < 1) {
		asprintf(error, "%s <object-id>", cmd);
		return false;
	}
	id = atoi(a[0]);
	global = pw_map_lookup(&rd->globals, id);
	if (global == NULL) {
		asprintf(error, "%s: unknown global %d", cmd, id);
		return false;
	}
	pw_core_proxy_destroy(rd->core_proxy, id);

	return true;
}

static bool do_create_link(struct data *data, const char *cmd, char *args, char **error)
{
	struct remote_data *rd = data->current;
	char *a[5];
        int n;
	uint32_t id;
	struct pw_type *t = data->t;
	struct pw_proxy *proxy;
	struct pw_properties *props = NULL;
	struct proxy_data *pd;

	n = pw_split_ip(args, WHITESPACE, 5, a);
	if (n < 4) {
		asprintf(error, "%s <node-id> <port> <node-id> <port> [<properties>]", cmd);
		return false;
	}
	if (n == 5)
		props = parse_props(a[4]);
	else
		props = pw_properties_new(NULL, NULL);

	pw_properties_set(props, PW_LINK_OUTPUT_NODE_ID, a[0]);
	pw_properties_set(props, PW_LINK_OUTPUT_PORT_ID, a[1]);
	pw_properties_set(props, PW_LINK_INPUT_NODE_ID, a[2]);
	pw_properties_set(props, PW_LINK_INPUT_PORT_ID, a[3]);

	proxy = (struct pw_proxy*)pw_core_proxy_create_object(rd->core_proxy,
					  "link-factory",
					  t->link,
					  PW_VERSION_LINK,
					  props ? &props->dict : NULL,
					  sizeof(struct proxy_data));

	pd = pw_proxy_get_user_data(proxy);
	pd->rd = rd;
	pd->proxy = proxy;
        pd->destroy = (pw_destroy_t) pw_link_info_free;
        pw_proxy_add_proxy_listener(proxy, &pd->proxy_proxy_listener, &link_events, pd);
        pw_proxy_add_listener(proxy, &pd->proxy_listener, &proxy_events, pd);

	id = pw_map_insert_new(&data->vars, proxy);
	fprintf(stdout, "%d = @proxy:%d\n", id, pw_proxy_get_id((struct pw_proxy*)proxy));

	return true;
}

static bool do_export_node(struct data *data, const char *cmd, char *args, char **error)
{
	struct pw_type *t = data->t;
	struct remote_data *rd = data->current;
	struct pw_global *global;
	struct pw_node *node;
	struct pw_proxy *proxy;
	char *a[2];
	int n, idx;
	uint32_t id;

	n = pw_split_ip(args, WHITESPACE, 2, a);
	if (n < 1) {
		asprintf(error, "%s <node-id> [<remote-var>]", cmd);
		return false;
	}
	if (n == 2) {
		idx = atoi(a[1]);
		rd = pw_map_lookup(&data->vars, idx);
		if (rd == NULL)
			goto no_remote;
	}

	global = pw_core_find_global(data->core, atoi(a[0]));
	if (global == NULL) {
		asprintf(error, "object %d does not exist", atoi(a[0]));
		return false;
	}
	if (pw_global_get_type(global) != t->node) {
		asprintf(error, "object %d is not a node", atoi(a[0]));
		return false;
	}
	node = pw_global_get_object(global);
	proxy = pw_remote_export(rd->remote, node);

	id = pw_map_insert_new(&data->vars, proxy);
	fprintf(stdout, "%d = @proxy:%d\n", id, pw_proxy_get_id((struct pw_proxy*)proxy));

	return true;

      no_remote:
        asprintf(error, "Remote %d does not exist", idx);
	return false;
}

static bool do_node_params(struct data *data, const char *cmd, char *args, char **error)
{
	struct pw_type *t = data->t;
	struct remote_data *rd = data->current;
	char *a[2];
        int n;
	uint32_t id, param_id;
	struct global *global;

	n = pw_split_ip(args, WHITESPACE, 2, a);
	if (n < 1) {
		asprintf(error, "%s <object-id> [<param-id-name>]", cmd);
		return false;
	}
	if (n == 2)
		param_id = spa_type_map_get_id(t->map, a[1]);
	else
		param_id = t->param.idList;

	id = atoi(a[0]);
	global = pw_map_lookup(&rd->globals, id);
	if (global == NULL) {
		asprintf(error, "%s: unknown global %d", cmd, id);
		return false;
	}
	if (global->type != t->node) {
		asprintf(error, "object %d is not a node", atoi(a[0]));
		return false;
	}
	pw_node_proxy_enum_params((struct pw_node_proxy*)global->proxy,
			param_id, 0, 0, NULL);

	return true;
}

static bool do_port_params(struct data *data, const char *cmd, char *args, char **error)
{
	struct pw_type *t = data->t;
	struct remote_data *rd = data->current;
	char *a[2];
        int n;
	uint32_t id, param_id;
	struct global *global;

	n = pw_split_ip(args, WHITESPACE, 2, a);
	if (n < 1) {
		asprintf(error, "%s <object-id> [<param-id-name>]", cmd);
		return false;
	}
	if (n == 2)
		param_id = spa_type_map_get_id(t->map, a[1]);
	else
		param_id = t->param.idList;

	id = atoi(a[0]);
	global = pw_map_lookup(&rd->globals, id);
	if (global == NULL) {
		asprintf(error, "%s: unknown global %d", cmd, id);
		return false;
	}
	if (global->type != t->port) {
		asprintf(error, "object %d is not a port", atoi(a[0]));
		return false;
	}
	if (global->proxy == NULL) {
		if (!bind_global(rd, global, error))
			return false;
	}

	pw_port_proxy_enum_params((struct pw_port_proxy*)global->proxy,
			param_id, 0, 0, NULL);

	return true;
}

static bool parse(struct data *data, char *buf, size_t size, char **error)
{
	char *a[2];
	int i, n;
	char *p, *cmd, *args;

	if ((p = strchr(buf, '#')))
		*p = '\0';

	p = pw_strip(buf, "\n\r \t");

	if (*p == '\0')
		return true;

	n = pw_split_ip(p, WHITESPACE, 2, a);
	if (n < 1)
		return true;

	cmd = a[0];
	args = n > 1 ? a[1] : "";

	for (i = 0; i < SPA_N_ELEMENTS(command_list); i++) {
		if (!strcmp(command_list[i].name, cmd)) {
			return command_list[i].func(data, cmd, args, error);
		}
	}
        asprintf(error, "Command \"%s\" does not exist. Type 'help' for usage.", cmd);
	return false;
}

static void do_input(void *data, int fd, enum spa_io mask)
{
	struct data *d = data;
	char buf[4096], *error;
	ssize_t r;

	if (mask & SPA_IO_IN) {
		while (true) {
			r = read(fd, buf, sizeof(buf)-1);
			if (r < 0) {
				if (errno == EAGAIN)
					continue;
				perror("read");
				r = 0;
				break;
			}
			break;
		}
		if (r == 0) {
			fprintf(stdout, "\n");
			pw_main_loop_quit(d->loop);
			return;
		}
		buf[r] = '\0';

		if (!parse(d, buf, r, &error)) {
			fprintf(stdout, "Error: \"%s\"\n", error);
			free(error);
		}
		if (d->current == NULL)
			pw_main_loop_quit(d->loop);
		else if (d->current->core_proxy)
			pw_core_proxy_sync(d->current->core_proxy, 1);
	}
}

static void do_quit(void *data, int signal_number)
{
	struct data *d = data;
	pw_main_loop_quit(d->loop);
}

int main(int argc, char *argv[])
{
	struct data data = { 0 };
	struct pw_loop *l;
	const struct pw_core_info *info;
	char *error, args[128];

	pw_init(&argc, &argv);

	data.loop = pw_main_loop_new(NULL);
	l = pw_main_loop_get_loop(data.loop);
	pw_loop_add_signal(l, SIGINT, do_quit, &data);
	pw_loop_add_signal(l, SIGTERM, do_quit, &data);

	spa_list_init(&data.remotes);
	pw_map_init(&data.vars, 64, 16);

	data.core = pw_core_new(l, pw_properties_new(PW_CORE_PROP_DAEMON, "1", NULL));
	data.t = pw_core_get_type(data.core);
	info = pw_core_get_info(data.core);

	pw_module_load(data.core, "libpipewire-module-link-factory", NULL, NULL, NULL, NULL);

	pw_loop_add_io(l, STDIN_FILENO, SPA_IO_IN|SPA_IO_HUP, false, do_input, &data);

	fprintf(stdout, "Welcome to PipeWire \"%s\" version %s. Type 'help' for usage.\n", info->name, info->version);

	snprintf(args, sizeof(args), "%s", info->name);
	do_connect(&data, "connect", args, &error);

	pw_main_loop_run(data.loop);

	pw_core_destroy(data.core);
	pw_main_loop_destroy(data.loop);

	return 0;
}
