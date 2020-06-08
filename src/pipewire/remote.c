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

#include <stdio.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <errno.h>
#include <sys/mman.h>

#include <spa/pod/parser.h>

#include "pipewire/pipewire.h"
#include "pipewire/private.h"
#include "pipewire/introspect.h"
#include "pipewire/interfaces.h"
#include "pipewire/remote.h"
#include "pipewire/core.h"
#include "pipewire/module.h"
#include "pipewire/stream.h"

#include "extensions/protocol-native.h"
#include "extensions/client-node.h"

/** \cond */
struct remote {
	struct pw_remote this;
	uint32_t type_client_node;
	struct spa_hook core_listener;
};

struct mem_id {
	uint32_t id;
	int fd;
	uint32_t flags;
	uint32_t ref;
	struct pw_map_range map;
	void *ptr;
};

struct buffer_id {
	struct spa_list link;
	uint32_t id;
	struct spa_buffer *buf;
	struct pw_map_range map;
	void *ptr;
	uint32_t n_mem;
	struct mem_id **mem;
};

struct port {
	struct spa_graph_port output;
	struct spa_graph_port input;

	struct pw_port *port;

	struct pw_array buffer_ids;
	bool in_order;
};

struct node_data {
	struct pw_remote *remote;
	struct pw_core *core;
	struct pw_type *t;
	uint32_t node_id;

	int rtwritefd;
	struct spa_source *rtsocket_source;
        struct pw_client_node_transport *trans;

	struct spa_node out_node_impl;
	struct spa_graph_node out_node;
	struct port *out_ports;
	struct spa_node in_node_impl;
	struct spa_graph_node in_node;
	struct port *in_ports;

        struct pw_array mem_ids;

	struct pw_node *node;
	struct spa_hook node_listener;

        struct pw_client_node_proxy *node_proxy;
	struct spa_hook node_proxy_listener;
	struct spa_hook proxy_listener;
};

/** \endcond */

SPA_EXPORT
const char *pw_remote_state_as_string(enum pw_remote_state state)
{
	switch (state) {
	case PW_REMOTE_STATE_ERROR:
		return "error";
	case PW_REMOTE_STATE_UNCONNECTED:
		return "unconnected";
	case PW_REMOTE_STATE_CONNECTING:
		return "connecting";
	case PW_REMOTE_STATE_CONNECTED:
		return "connected";
	}
	return "invalid-state";
}

static int
pw_remote_update_state(struct pw_remote *remote, enum pw_remote_state state, const char *fmt, ...)
{
	enum pw_remote_state old = remote->state;

	if (old != state) {
		free(remote->error);

		if (fmt) {
			va_list varargs;

			va_start(varargs, fmt);
			if (vasprintf(&remote->error, fmt, varargs) < 0) {
				pw_log_debug("remote %p: error formating message: %m", remote);
				remote->error = NULL;
			}
			va_end(varargs);
		} else {
			remote->error = NULL;
		}
		pw_log_debug("remote %p: update state from %s -> %s (%s)", remote,
			     pw_remote_state_as_string(old),
			     pw_remote_state_as_string(state), remote->error);

		remote->state = state;
		pw_remote_events_state_changed(remote, old, state, remote->error);
	}
	return 0;
}

static void core_event_info(void *data, struct pw_core_info *info)
{
	struct pw_remote *this = data;

	pw_log_debug("remote %p: got core info", this);
	this->info = pw_core_info_update(this->info, info);
	pw_remote_events_info_changed(this, this->info);
}

static void core_event_done(void *data, uint32_t seq)
{
	struct pw_remote *this = data;

	pw_log_debug("remote %p: core event done %d", this, seq);
	if (seq == 0)
		pw_remote_update_state(this, PW_REMOTE_STATE_CONNECTED, NULL);

	pw_remote_events_sync_reply(this, seq);
}

static void core_event_error(void *data, uint32_t id, int res, const char *error, ...)
{
	struct pw_remote *this = data;
	pw_remote_update_state(this, PW_REMOTE_STATE_ERROR, error);
}

static void core_event_remove_id(void *data, uint32_t id)
{
	struct pw_remote *this = data;
	struct pw_proxy *proxy;

	pw_log_debug("remote %p: object remove %u", this, id);
	if ((proxy = pw_map_lookup(&this->objects, id)) == NULL) {
		pw_log_warn("remote %p: asked to remove unknown object id %u", this, id);
		return;
	}
	pw_proxy_destroy(proxy);
	pw_map_remove(&this->objects, id);
}

static void
core_event_update_types(void *data, uint32_t first_id, const char **types, uint32_t n_types)
{
	struct pw_remote *this = data;
	int i;

	for (i = 0; i < n_types; i++, first_id++) {
		uint32_t this_id = spa_type_map_get_id(this->core->type.map, types[i]);
		if (!pw_map_insert_at(&this->types, first_id, PW_MAP_ID_TO_PTR(this_id)))
			pw_log_error("can't add type for client");
	}
}

static const struct pw_core_proxy_events core_proxy_events = {
	PW_VERSION_CORE_PROXY_EVENTS,
	.update_types = core_event_update_types,
	.done = core_event_done,
	.error = core_event_error,
	.remove_id = core_event_remove_id,
	.info = core_event_info,
};

SPA_EXPORT
struct pw_remote *pw_remote_new(struct pw_core *core,
				struct pw_properties *properties,
				size_t user_data_size)
{
	struct remote *impl;
	struct pw_remote *this;
	struct pw_protocol *protocol;
	const char *protocol_name;

	impl = calloc(1, sizeof(struct remote) + user_data_size);
	if (impl == NULL)
		return NULL;

	this = &impl->this;
	pw_log_debug("remote %p: new", impl);

	this->core = core;

	if (user_data_size > 0)
		this->user_data = SPA_MEMBER(impl, sizeof(struct remote), void);

	if (properties == NULL)
		properties = pw_properties_new(NULL, NULL);
	if (properties == NULL)
		goto no_mem;

	pw_fill_remote_properties(core, properties);
	this->properties = properties;

        impl->type_client_node = spa_type_map_get_id(core->type.map, PW_TYPE_INTERFACE__ClientNode);
	this->state = PW_REMOTE_STATE_UNCONNECTED;

	pw_map_init(&this->objects, 64, 32);
	pw_map_init(&this->types, 64, 32);

	spa_list_init(&this->proxy_list);
	spa_list_init(&this->stream_list);

	spa_hook_list_init(&this->listener_list);

	if ((protocol_name = pw_properties_get(properties, PW_REMOTE_PROP_PROTOCOL)) == NULL) {
		if (!pw_module_load(core, "libpipewire-module-protocol-native", NULL, NULL, NULL, NULL))
			goto no_protocol;

		protocol_name = PW_TYPE_PROTOCOL__Native;
	}

	protocol = pw_core_find_protocol(core, protocol_name);
	if (protocol == NULL)
		goto no_protocol;

	this->conn = pw_protocol_new_client(protocol, this, properties);
	if (this->conn == NULL)
		goto no_connection;

	pw_module_load(core, "libpipewire-module-client-node", NULL, NULL, NULL, NULL);

        spa_list_append(&core->remote_list, &this->link);

	return this;

      no_mem:
	pw_log_error("no memory");
	goto exit;
      no_protocol:
	pw_log_error("can't load native protocol");
	goto exit_free_props;
      no_connection:
	pw_log_error("can't create new native protocol connection");
	goto exit_free_props;

      exit_free_props:
	pw_properties_free(properties);
      exit:
	free(impl);
	return NULL;
}

SPA_EXPORT
void pw_remote_destroy(struct pw_remote *remote)
{
	struct remote *impl = SPA_CONTAINER_OF(remote, struct remote, this);
	struct pw_stream *stream;

	pw_log_debug("remote %p: destroy", remote);
	pw_remote_events_destroy(remote);

	if (remote->state != PW_REMOTE_STATE_UNCONNECTED)
		pw_remote_disconnect(remote);

	spa_list_consume(stream, &remote->stream_list, link)
		pw_stream_destroy(stream);

	pw_protocol_client_destroy(remote->conn);

	spa_list_remove(&remote->link);

	pw_log_debug("remote %p: free", remote);
	pw_properties_free(remote->properties);

	free(remote->error);
	free(impl);
}

SPA_EXPORT
struct pw_core *pw_remote_get_core(struct pw_remote *remote)
{
	return remote->core;
}

SPA_EXPORT
const struct pw_properties *pw_remote_get_properties(struct pw_remote *remote)
{
	return remote->properties;
}

SPA_EXPORT
void *pw_remote_get_user_data(struct pw_remote *remote)
{
	return remote->user_data;
}

SPA_EXPORT
enum pw_remote_state pw_remote_get_state(struct pw_remote *remote, const char **error)
{
	if (error)
		*error = remote->error;
	return remote->state;
}

SPA_EXPORT
void pw_remote_add_listener(struct pw_remote *remote,
			    struct spa_hook *listener,
			    const struct pw_remote_events *events,
			    void *data)
{
	spa_hook_list_append(&remote->listener_list, listener, events, data);
}

static int do_connect(struct pw_remote *remote)
{
	struct remote *impl = SPA_CONTAINER_OF(remote, struct remote, this);
	struct pw_proxy dummy;

	dummy.remote = remote;

	remote->core_proxy = (struct pw_core_proxy*)pw_proxy_new(&dummy, remote->core->type.core, 0);
	if (remote->core_proxy == NULL)
		goto no_proxy;

	pw_core_proxy_add_listener(remote->core_proxy, &impl->core_listener, &core_proxy_events, remote);

	pw_core_proxy_hello(remote->core_proxy);
	pw_core_proxy_client_update(remote->core_proxy, &remote->properties->dict);
	pw_core_proxy_sync(remote->core_proxy, 0);

	return 0;

      no_proxy:
	pw_protocol_client_disconnect(remote->conn);
	pw_remote_update_state(remote, PW_REMOTE_STATE_ERROR, "can't connect: no memory");
	return -ENOMEM;
}

SPA_EXPORT
struct pw_core_proxy * pw_remote_get_core_proxy(struct pw_remote *remote)
{
	return remote->core_proxy;
}

SPA_EXPORT
const struct pw_core_info *pw_remote_get_core_info(struct pw_remote *remote)
{
	return remote->info;
}

SPA_EXPORT
struct pw_proxy *pw_remote_find_proxy(struct pw_remote *remote, uint32_t id)
{
	return pw_map_lookup(&remote->objects, id);
}

static void done_connect(void *data, int result)
{
	struct pw_remote *remote = data;
	if (result < 0) {
		pw_remote_update_state(remote, PW_REMOTE_STATE_ERROR, "can't connect: %s",
				spa_strerror(result));
		return;
	}

	do_connect(remote);
}

SPA_EXPORT
int pw_remote_connect(struct pw_remote *remote)
{
	int res;

	pw_remote_update_state(remote, PW_REMOTE_STATE_CONNECTING, NULL);

	if ((res = pw_protocol_client_connect(remote->conn, done_connect, remote)) < 0) {
		pw_remote_update_state(remote, PW_REMOTE_STATE_ERROR,
				"connect failed %s", spa_strerror(res));
		return res;
	}
	return remote->state == PW_REMOTE_STATE_ERROR ? -EIO : 0;
}

SPA_EXPORT
int pw_remote_connect_fd(struct pw_remote *remote, int fd)
{
	int res;

	pw_remote_update_state(remote, PW_REMOTE_STATE_CONNECTING, NULL);

	if ((res = pw_protocol_client_connect_fd(remote->conn, fd, true)) < 0) {
		pw_remote_update_state(remote, PW_REMOTE_STATE_ERROR,
				"connect_fd failed %s", spa_strerror(res));
		return res;
	}

	return do_connect(remote);
}

SPA_EXPORT
int pw_remote_steal_fd(struct pw_remote *remote)
{
	int fd;

	fd = pw_protocol_client_steal_fd(remote->conn);
	pw_remote_disconnect(remote);

	return fd;
}

SPA_EXPORT
int pw_remote_disconnect(struct pw_remote *remote)
{
	struct pw_proxy *proxy;
	struct pw_stream *stream, *s2;

	pw_log_debug("remote %p: disconnect", remote);
	spa_list_for_each_safe(stream, s2, &remote->stream_list, link)
		pw_stream_disconnect(stream);

	spa_list_consume(proxy, &remote->proxy_list, link)
		pw_proxy_destroy(proxy);
	remote->core_proxy = NULL;

	pw_protocol_client_disconnect(remote->conn);

	pw_map_clear(&remote->objects);
	pw_map_clear(&remote->types);
	remote->n_types = 0;

	if (remote->info) {
		pw_core_info_free(remote->info);
		remote->info = NULL;
	}
        pw_remote_update_state(remote, PW_REMOTE_STATE_UNCONNECTED, NULL);

	return 0;
}

static int
do_remove_source(struct spa_loop *loop,
                 bool async, uint32_t seq, const void *data, size_t size, void *user_data)
{
	struct node_data *d = user_data;

	if (d->rtsocket_source) {
		pw_loop_destroy_source(d->core->data_loop, d->rtsocket_source);
		d->rtsocket_source = NULL;
	}
        return 0;
}


static void unhandle_socket(struct pw_proxy *proxy)
{
	struct node_data *data = proxy->user_data;

        pw_loop_invoke(data->core->data_loop,
                       do_remove_source, 1, NULL, 0, true, data);
}

static void handle_rtnode_message(struct pw_proxy *proxy, struct pw_client_node_message *message)
{
	struct node_data *data = proxy->user_data;

	switch (PW_CLIENT_NODE_MESSAGE_TYPE(message)) {
	case PW_CLIENT_NODE_MESSAGE_PROCESS_INPUT:
		pw_log_trace("remote %p: process input", data->remote);
		spa_graph_have_output(data->node->rt.graph, &data->in_node);
		break;

	case PW_CLIENT_NODE_MESSAGE_PROCESS_OUTPUT:
		pw_log_trace("remote %p: process output", data->remote);
		spa_graph_need_input(data->node->rt.graph, &data->out_node);
		break;

	case PW_CLIENT_NODE_MESSAGE_PORT_REUSE_BUFFER:
	{
		struct pw_client_node_message_port_reuse_buffer *rb =
		    (struct pw_client_node_message_port_reuse_buffer *) message;
		uint32_t port_id = rb->body.port_id.value;
		uint32_t buffer_id = rb->body.buffer_id.value;
		struct spa_graph_port *p, *pp;

		spa_list_for_each(p, &data->out_node.ports[SPA_DIRECTION_INPUT], link) {
			if (p->port_id != port_id || (pp = p->peer) == NULL)
				continue;

			spa_node_port_reuse_buffer(pp->node->implementation,
						   pp->port_id, buffer_id);
			break;
		}
		break;
	}
	default:
		pw_log_warn("unexpected node message %d", PW_CLIENT_NODE_MESSAGE_TYPE(message));
		break;
	}
}

static void
on_rtsocket_condition(void *user_data, int fd, enum spa_io mask)
{
	struct pw_proxy *proxy = user_data;
	struct node_data *data = proxy->user_data;

	if (mask & (SPA_IO_ERR | SPA_IO_HUP)) {
		pw_log_warn("got error");
		unhandle_socket(proxy);
		return;
	}

	if (mask & SPA_IO_IN) {
		struct pw_client_node_message message;
		uint64_t cmd;

		if (read(fd, &cmd, sizeof(uint64_t)) != sizeof(uint64_t))
			pw_log_warn("proxy %p: read failed %m", proxy);

		if (cmd > 1)
			pw_log_warn("proxy %p: %ld messages", proxy, cmd);


		while (pw_client_node_transport_next_message(data->trans, &message) == 1) {
			struct pw_client_node_message *msg = alloca(SPA_POD_SIZE(&message));
			pw_client_node_transport_parse_message(data->trans, msg);
			handle_rtnode_message(proxy, msg);
		}
	}
}

static struct mem_id *find_mem(struct pw_array *mem_ids, uint32_t id)
{
	struct mem_id *mid;

	pw_array_for_each(mid, mem_ids) {
		if (mid->id == id)
			return mid;
	}
	return NULL;
}

static void *mem_map(struct node_data *data, struct mem_id *mid, uint32_t offset, uint32_t size)
{
	if (mid->ptr == NULL) {
		pw_map_range_init(&mid->map, offset, size, data->core->sc_pagesize);

		mid->ptr = mmap(NULL, mid->map.size, PROT_READ|PROT_WRITE,
				MAP_SHARED, mid->fd, mid->map.offset);

		if (mid->ptr == MAP_FAILED) {
			pw_log_error("Failed to mmap memory %d %p: %m", size, mid);
			mid->ptr = NULL;
			return NULL;
		}
	}
	return SPA_MEMBER(mid->ptr, mid->map.start, void);
}
static void mem_unmap(struct node_data *data, struct mem_id *mid)
{
	if (mid->ptr != NULL) {
		if (munmap(mid->ptr, mid->map.size) < 0)
			pw_log_warn("failed to unmap: %m");
		mid->ptr = NULL;
	}
}

static void clear_memid(struct node_data *data, struct mem_id *mid)
{
	if (mid->fd != -1) {
		bool has_ref = false;
		int fd;
		struct mem_id *m;

		fd = mid->fd;
		mid->fd = -1;
		mid->id = SPA_ID_INVALID;

		pw_array_for_each(m, &data->mem_ids) {
			if (m->fd == fd) {
				has_ref = true;
				break;
			}
		}
		if (!has_ref) {
			mem_unmap(data, mid);
			close(fd);
		}
	}
}

static void clean_transport(struct pw_proxy *proxy)
{
	struct node_data *data = proxy->user_data;
	struct pw_port *port;
	struct mem_id *mid;

	if (data->trans == NULL)
		return;

	unhandle_socket(proxy);

	spa_list_for_each(port, &data->node->input_ports, link) {
		spa_graph_port_remove(&data->in_ports[port->port_id].output);
		spa_graph_port_remove(&data->in_ports[port->port_id].input);
	}
	spa_list_for_each(port, &data->node->output_ports, link) {
		spa_graph_port_remove(&data->out_ports[port->port_id].output);
		spa_graph_port_remove(&data->out_ports[port->port_id].input);
	}

	pw_array_for_each(mid, &data->mem_ids)
		clear_memid(data, mid);
	pw_array_clear(&data->mem_ids);

	free(data->in_ports);
	free(data->out_ports);
	pw_client_node_transport_destroy(data->trans);
	close(data->rtwritefd);

	data->trans = NULL;
}

static void port_init(struct port *port)
{
        pw_array_init(&port->buffer_ids, 32);
        pw_array_ensure_size(&port->buffer_ids, sizeof(struct buffer_id) * 64);
	port->in_order = true;
}

static struct port *find_port(struct node_data *data, enum spa_direction direction, uint32_t port_id)
{
	if (direction == SPA_DIRECTION_INPUT) {
		if (port_id > data->trans->area->max_input_ports)
			return NULL;
		return &data->in_ports[port_id];
	}
	else {
		if (port_id > data->trans->area->max_output_ports)
			return NULL;
		return &data->out_ports[port_id];
	}
}

static void client_node_add_mem(void *object,
				uint32_t mem_id,
				uint32_t type, int memfd, uint32_t flags)
{
	struct pw_proxy *proxy = object;
	struct node_data *data = proxy->user_data;
	struct mem_id *m;

	m = find_mem(&data->mem_ids, mem_id);
	if (m) {
		pw_log_warn("duplicate mem %u, fd %d, flags %d",
			     mem_id, memfd, flags);
		return;
	}

	m = pw_array_add(&data->mem_ids, sizeof(struct mem_id));
	pw_log_debug("add mem %u, fd %d, flags %d", mem_id, memfd, flags);

	m->id = mem_id;
	m->fd = memfd;
	m->flags = flags;
	m->ref = 0;
	m->map = PW_MAP_RANGE_INIT;
	m->ptr = NULL;
}

static void client_node_transport(void *object, uint32_t node_id,
                                  int readfd, int writefd,
				  struct pw_client_node_transport *transport)
{
	struct pw_proxy *proxy = object;
	struct node_data *data = proxy->user_data;
	struct pw_port *port;
	int i;

	clean_transport(proxy);

	data->node_id = node_id;
	data->trans = transport;

	pw_log_info("remote-node %p: create transport %p with fds %d %d for node %u",
		proxy, data->trans, readfd, writefd, node_id);

	data->in_ports = calloc(data->trans->area->max_input_ports,
				 sizeof(struct port));
	data->out_ports = calloc(data->trans->area->max_output_ports,
				  sizeof(struct port));

	for (i = 0; i < data->trans->area->max_input_ports; i++) {
		port_init(&data->in_ports[i]);
		data->trans->inputs[i] = SPA_IO_BUFFERS_INIT;
		spa_graph_port_init(&data->in_ports[i].input,
				    SPA_DIRECTION_INPUT,
				    i,
				    0,
				    &data->trans->inputs[i]);
		spa_graph_port_init(&data->in_ports[i].output,
				    SPA_DIRECTION_OUTPUT,
				    i,
				    0,
				    &data->trans->inputs[i]);
		spa_graph_port_add(&data->in_node, &data->in_ports[i].output);
		spa_graph_port_link(&data->in_ports[i].output, &data->in_ports[i].input);
		pw_log_info("transport in %d %p", i, &data->trans->inputs[i]);
	}
	spa_list_for_each(port, &data->node->input_ports, link) {
		spa_graph_port_add(&port->rt.mix_node, &data->in_ports[port->port_id].input);
		data->in_ports[port->port_id].port = port;
	}

	for (i = 0; i < data->trans->area->max_output_ports; i++) {
		port_init(&data->out_ports[i]);
		data->trans->outputs[i] = SPA_IO_BUFFERS_INIT;
		spa_graph_port_init(&data->out_ports[i].output,
				    SPA_DIRECTION_OUTPUT,
				    i,
				    0,
				    &data->trans->outputs[i]);
		spa_graph_port_init(&data->out_ports[i].input,
				    SPA_DIRECTION_INPUT,
				    i,
				    0,
				    &data->trans->outputs[i]);
		spa_graph_port_add(&data->out_node, &data->out_ports[i].input);
		spa_graph_port_link(&data->out_ports[i].output, &data->out_ports[i].input);
		pw_log_info("transport out %d %p", i, &data->trans->inputs[i]);
	}
	spa_list_for_each(port, &data->node->output_ports, link) {
		spa_graph_port_add(&port->rt.mix_node, &data->out_ports[port->port_id].output);
		data->out_ports[port->port_id].port = port;
	}

        data->rtwritefd = writefd;
        data->rtsocket_source = pw_loop_add_io(proxy->remote->core->data_loop,
                                               readfd,
                                               SPA_IO_ERR | SPA_IO_HUP,
                                               true, on_rtsocket_condition, proxy);
	if (data->node->active)
		pw_client_node_proxy_set_active(data->node_proxy, true);
}

static void add_port_update(struct pw_proxy *proxy, struct pw_port *port, uint32_t change_mask)
{
	struct node_data *data = proxy->user_data;
	const struct spa_port_info *port_info = NULL;
	struct spa_port_info pi;
	uint32_t n_params = 0;
	struct spa_pod **params = NULL;

	spa_zero(pi);
	if (change_mask & PW_CLIENT_NODE_PORT_UPDATE_PARAMS) {
		uint32_t idx1, idx2, id;
		uint8_t buf[2048];
		struct spa_pod_builder b = { 0 };

		for (idx1 = 0;;) {
			struct spa_pod *param;

			spa_pod_builder_init(&b, buf, sizeof(buf));
                        if (spa_node_port_enum_params(port->node->node,
						      port->spa_direction, port->port_id,
						      data->t->param.idList, &idx1,
						      NULL, &param, &b) <= 0)
                                break;

			spa_pod_object_parse(param,
				":", data->t->param.listId, "I", &id, NULL);

			for (idx2 = 0;; n_params++) {
				spa_pod_builder_init(&b, buf, sizeof(buf));
	                        if (spa_node_port_enum_params(port->node->node,
							      port->spa_direction, port->port_id,
							      id, &idx2,
							      NULL, &param, &b) <= 0)
	                                break;

	                        params = realloc(params, sizeof(struct spa_pod *) * (n_params + 1));
	                        params[n_params] = pw_spa_pod_copy(param);
			}
                }
	}
	if (change_mask & PW_CLIENT_NODE_PORT_UPDATE_INFO) {
		spa_node_port_get_info(port->node->node, port->spa_direction, port->port_id, &port_info);
		pi = * port_info;
		pi.flags &= ~SPA_PORT_INFO_FLAG_CAN_ALLOC_BUFFERS;
	}

        pw_client_node_proxy_port_update(data->node_proxy,
                                         port->spa_direction,
                                         port->port_id,
                                         change_mask,
                                         n_params,
                                         (const struct spa_pod **)params,
					 &pi);
	if (params) {
		while (n_params > 0)
			free(params[--n_params]);
		free(params);
	}
}

static void
client_node_set_param(void *object, uint32_t seq, uint32_t id, uint32_t flags,
		      const struct spa_pod *param)
{
	pw_log_warn("set param not implemented");
}

static void client_node_event(void *object, const struct spa_event *event)
{
	pw_log_warn("unhandled node event %d", SPA_EVENT_TYPE(event));
}


static void node_need_input(void *data)
{
	struct node_data *d = data;
        uint64_t cmd = 1;
	pw_client_node_transport_add_message(d->trans,
				&PW_CLIENT_NODE_MESSAGE_INIT(PW_CLIENT_NODE_MESSAGE_NEED_INPUT));
        write(d->rtwritefd, &cmd, 8);
}

static void node_have_output(void *data)
{
	struct node_data *d = data;
        uint64_t cmd = 1;
        pw_client_node_transport_add_message(d->trans,
                               &PW_CLIENT_NODE_MESSAGE_INIT(PW_CLIENT_NODE_MESSAGE_HAVE_OUTPUT));
        write(d->rtwritefd, &cmd, 8);
}

static void client_node_command(void *object, uint32_t seq, const struct spa_command *command)
{
	struct pw_proxy *proxy = object;
	struct node_data *data = proxy->user_data;
	struct pw_remote *remote = proxy->remote;
	int res;

	if (SPA_COMMAND_TYPE(command) == remote->core->type.command_node.Pause) {
		pw_log_debug("node %p: pause %d", proxy, seq);

		pw_loop_update_io(remote->core->data_loop,
				  data->rtsocket_source,
				  SPA_IO_ERR | SPA_IO_HUP);

		if ((res = spa_node_send_command(data->node->node, command)) < 0)
			pw_log_warn("node %p: pause failed", proxy);

		pw_client_node_proxy_done(data->node_proxy, seq, res);
	}
	else if (SPA_COMMAND_TYPE(command) == remote->core->type.command_node.Start) {
		int i;

		pw_log_debug("node %p: start %d", proxy, seq);

		pw_loop_update_io(remote->core->data_loop,
				  data->rtsocket_source,
				  SPA_IO_IN | SPA_IO_ERR | SPA_IO_HUP);

		if ((res = spa_node_send_command(data->node->node, command)) < 0)
			pw_log_warn("node %p: start failed", proxy);

		/* FIXME we should call process_output on the node and see what its
		 * status is */
		for (i = 0; i < data->trans->area->max_input_ports; i++)
			data->trans->inputs[i].status = SPA_STATUS_NEED_BUFFER;
		node_need_input(data);

		pw_client_node_proxy_done(data->node_proxy, seq, res);
	}
	else if (SPA_COMMAND_TYPE(command) == remote->core->type.command_node.ClockUpdate) {
		struct spa_command_node_clock_update *cu = (__typeof__(cu)) command;

#if 0
		if (cu->body.flags.value & SPA_COMMAND_NODE_CLOCK_UPDATE_FLAG_LIVE) {
			pw_properties_set(stream->properties, PW_STREAM_PROP_IS_LIVE, "1");
			pw_properties_setf(stream->properties,
					   PW_STREAM_PROP_LATENCY_MIN, "%" PRId64,
					   cu->body.latency.value);
		}
		impl->last_ticks = cu->body.ticks.value;
		impl->last_rate = cu->body.rate.value;
		impl->last_monotonic = cu->body.monotonic_time.value;
#endif
	}
	else {
		pw_log_warn("unhandled node command %d", SPA_COMMAND_TYPE(command));
		pw_client_node_proxy_done(data->node_proxy, seq, -ENOTSUP);
	}
}

static void
client_node_add_port(void *object, uint32_t seq, enum spa_direction direction, uint32_t port_id)
{
	pw_log_warn("add port not supported");
}

static void
client_node_remove_port(void *object, uint32_t seq, enum spa_direction direction, uint32_t port_id)
{
	pw_log_warn("remove port not supported");
}

static void
client_node_port_set_param(void *object,
			   uint32_t seq,
			   enum spa_direction direction, uint32_t port_id,
			   uint32_t id, uint32_t flags,
			   const struct spa_pod *param)
{
	struct pw_proxy *proxy = object;
	struct node_data *data = proxy->user_data;
	struct port *port;
	int res;

	port = find_port(data, direction, port_id);
	if (port == NULL || port->port == NULL) {
		res = -EINVAL;
		goto done;
	}

	res = pw_port_set_param(port->port, id, flags, param);
	if (res < 0)
		goto done;

	add_port_update(proxy, port->port,
			PW_CLIENT_NODE_PORT_UPDATE_PARAMS |
			PW_CLIENT_NODE_PORT_UPDATE_INFO);

      done:
	pw_client_node_proxy_done(data->node_proxy, seq, res);
}

static void clear_buffers(struct node_data *data, struct port *port)
{
        struct buffer_id *bid;
	int i;

        pw_log_debug("port %p: clear buffers", port);
	pw_port_use_buffers(port->port, NULL, 0);

        pw_array_for_each(bid, &port->buffer_ids) {
		if (bid->ptr != NULL) {
			if (munmap(bid->ptr, bid->map.size) < 0)
				pw_log_warn("failed to unmap: %m");
		}
		if (bid->mem != NULL) {
			for (i = 0; i < bid->n_mem; i++) {
				if (--bid->mem[i]->ref == 0)
					clear_memid(data, bid->mem[i]);
			}
			bid->mem = NULL;
			bid->n_mem = 0;
		}
		bid->ptr = NULL;
                free(bid->buf);
                bid->buf = NULL;
        }
        port->buffer_ids.size = 0;
}

static void
client_node_port_use_buffers(void *object,
			     uint32_t seq,
			     enum spa_direction direction, uint32_t port_id,
			     uint32_t n_buffers, struct pw_client_node_buffer *buffers)
{
	struct pw_proxy *proxy = object;
	struct node_data *data = proxy->user_data;
	struct buffer_id *bid;
	uint32_t i, j, len;
	struct spa_buffer *b, **bufs;
	struct port *port;
	struct pw_core *core = proxy->remote->core;
	struct pw_type *t = &core->type;
	int res, prot;

	port = find_port(data, direction, port_id);
	if (port == NULL) {
		res = -EINVAL;
		goto done;
	}

	prot = PROT_READ | (direction == SPA_DIRECTION_OUTPUT ? PROT_WRITE : 0);

	/* clear previous buffers */
	clear_buffers(data, port);

	bufs = alloca(n_buffers * sizeof(struct spa_buffer *));

	for (i = 0; i < n_buffers; i++) {
		off_t offset;

		struct mem_id *mid = find_mem(&data->mem_ids, buffers[i].mem_id);
		if (mid == NULL) {
			pw_log_error("unknown memory id %u", buffers[i].mem_id);
			res = -EINVAL;
			goto cleanup;
		}

		len = pw_array_get_len(&port->buffer_ids, struct buffer_id);
		bid = pw_array_add(&port->buffer_ids, sizeof(struct buffer_id));

		pw_map_range_init(&bid->map, buffers[i].offset, buffers[i].size, core->sc_pagesize);

		bid->ptr = mmap(NULL, bid->map.size, prot, MAP_SHARED, mid->fd, bid->map.offset);
		if (bid->ptr == MAP_FAILED) {
			bid->ptr = NULL;
			pw_log_error("Failed to mmap memory %u %u %u %d: %m",
				bid->map.offset, bid->map.size, buffers[i].mem_id, mid->fd);
			res = -errno;
			goto cleanup;
		}
		if (mlock(bid->ptr, bid->map.size) < 0)
			pw_log_warn("Failed to mlock memory %u %u: %m",
					bid->map.offset, bid->map.size);

		b = buffers[i].buffer;

		{
			size_t size;

			size = sizeof(struct spa_buffer);
			size += sizeof(struct mem_id *);
			for (j = 0; j < buffers[i].buffer->n_metas; j++)
				size += sizeof(struct spa_meta);
			for (j = 0; j < buffers[i].buffer->n_datas; j++) {
				size += sizeof(struct spa_data);
				size += sizeof(struct mem_id *);
			}

			b = bid->buf = malloc(size);
			memcpy(b, buffers[i].buffer, sizeof(struct spa_buffer));

			b->metas = SPA_MEMBER(b, sizeof(struct spa_buffer), struct spa_meta);
			b->datas = SPA_MEMBER(b->metas, sizeof(struct spa_meta) * b->n_metas,
				       struct spa_data);
			bid->mem = SPA_MEMBER(b->datas, sizeof(struct spa_data) * b->n_datas,
				       struct mem_id*);
			bid->n_mem = 0;

			mid->ref++;
			bid->mem[bid->n_mem++] = mid;
		}
		bid->id = b->id;

		if (bid->id != len) {
			pw_log_warn("unexpected id %u found, expected %u", bid->id, len);
		}
		pw_log_debug("add buffer %d %d %u %u", mid->id, bid->id, bid->map.offset, bid->map.size);

		offset = bid->map.start;
		for (j = 0; j < b->n_metas; j++) {
			struct spa_meta *m = &b->metas[j];
			memcpy(m, &buffers[i].buffer->metas[j], sizeof(struct spa_meta));
			m->data = SPA_MEMBER(bid->ptr, offset, void);
			offset += m->size;
		}

		for (j = 0; j < b->n_datas; j++) {
			struct spa_data *d = &b->datas[j];

			memcpy(d, &buffers[i].buffer->datas[j], sizeof(struct spa_data));
			d->chunk =
			    SPA_MEMBER(bid->ptr, offset + sizeof(struct spa_chunk) * j,
				       struct spa_chunk);

			if (d->type == t->data.MemFd || d->type == t->data.DmaBuf) {
				uint32_t id = SPA_PTR_TO_UINT32(d->data);
				struct mem_id *bmid = find_mem(&data->mem_ids, id);

				if (bmid == NULL) {
					pw_log_error("unknown buffer mem %u", id);
					res = -EINVAL;
					goto cleanup;
				}

				d->data = NULL;
				d->fd = bmid->fd;
				bmid->ref++;
				bid->mem[bid->n_mem++] = bmid;
				pw_log_debug(" data %d %u -> fd %d", j, bmid->id, bmid->fd);
			} else if (d->type == t->data.MemPtr) {
				d->data = SPA_MEMBER(bid->ptr,
						bid->map.start + SPA_PTR_TO_INT(d->data), void);
				d->fd = -1;
				pw_log_debug(" data %d %u -> mem %p", j, bid->id, d->data);
			} else {
				pw_log_warn("unknown buffer data type %d", d->type);
			}
		}
		bufs[i] = b;
	}

	res = pw_port_use_buffers(port->port, bufs, n_buffers);

      done:
	pw_client_node_proxy_done(data->node_proxy, seq, res);
	return;

     cleanup:
	clear_buffers(data, port);
	goto done;

}

static void
client_node_port_command(void *object,
                         uint32_t direction,
                         uint32_t port_id,
                         const struct spa_command *command)
{
	struct pw_proxy *proxy = object;
	struct node_data *data = proxy->user_data;
	struct port *port;

	port = find_port(data, direction, port_id);
	if (port == NULL)
		return;

	pw_port_send_command(port->port, true, command);
}

static void
client_node_port_set_io(void *object,
                        uint32_t seq,
                        uint32_t direction,
                        uint32_t port_id,
                        uint32_t id,
                        uint32_t memid,
                        uint32_t offset,
                        uint32_t size)
{
	struct pw_proxy *proxy = object;
	struct node_data *data = proxy->user_data;
	struct pw_core *core = proxy->remote->core;
	struct port *port;
	struct mem_id *mid;
	void *ptr;

	port = find_port(data, direction, port_id);
	if (port == NULL)
		return;

	if (memid == SPA_ID_INVALID) {
		ptr = NULL;
		size = 0;
	}
	else {
		mid = find_mem(&data->mem_ids, memid);
		if (mid == NULL) {
			pw_log_warn("unknown memory id %u", memid);
			return;
		}

		if ((ptr = mem_map(data, mid, offset, size)) == NULL)
			return;
	}


	pw_log_debug("port %p: set io %s %p", port, spa_type_map_get_type(core->type.map, id), ptr);

	spa_node_port_set_io(port->port->node->node,
			     direction, port_id,
			     id,
			     ptr,
			     size);
}


static const struct pw_client_node_proxy_events client_node_events = {
	PW_VERSION_CLIENT_NODE_PROXY_EVENTS,
	.add_mem = client_node_add_mem,
	.transport = client_node_transport,
	.set_param = client_node_set_param,
	.event = client_node_event,
	.command = client_node_command,
	.add_port = client_node_add_port,
	.remove_port = client_node_remove_port,
	.port_set_param = client_node_port_set_param,
	.port_use_buffers = client_node_port_use_buffers,
	.port_command = client_node_port_command,
	.port_set_io = client_node_port_set_io,
};

static void do_node_init(struct pw_proxy *proxy)
{
	struct node_data *data = proxy->user_data;
	struct pw_port *port;

        pw_client_node_proxy_update(data->node_proxy,
                                    PW_CLIENT_NODE_UPDATE_MAX_INPUTS |
				    PW_CLIENT_NODE_UPDATE_MAX_OUTPUTS |
				    PW_CLIENT_NODE_UPDATE_PARAMS,
				    data->node->info.max_input_ports,
				    data->node->info.max_output_ports,
				    0, NULL);

	spa_list_for_each(port, &data->node->input_ports, link) {
		add_port_update(proxy, port,
				PW_CLIENT_NODE_PORT_UPDATE_PARAMS |
				PW_CLIENT_NODE_PORT_UPDATE_INFO);
	}
	spa_list_for_each(port, &data->node->output_ports, link) {
		add_port_update(proxy, port,
				PW_CLIENT_NODE_PORT_UPDATE_PARAMS |
				PW_CLIENT_NODE_PORT_UPDATE_INFO);
	}
        pw_client_node_proxy_done(data->node_proxy, 0, 0);
}

static void node_destroy(void *data)
{
	struct node_data *d = data;
	pw_log_debug("%p: destroy", d);
	pw_client_node_proxy_destroy(d->node_proxy);
	pw_proxy_destroy((struct pw_proxy *)d->node_proxy);
	d->node_proxy = NULL;
}

static void node_active_changed(void *data, bool active)
{
	struct node_data *d = data;
	pw_log_debug("active %d", active);
	pw_client_node_proxy_set_active(d->node_proxy, active);
}

static const struct pw_node_events node_events = {
	PW_VERSION_NODE_EVENTS,
	.destroy = node_destroy,
	.active_changed = node_active_changed,
	.need_input = node_need_input,
	.have_output = node_have_output,
};

static void clear_port(struct node_data *data, struct port *port)
{
	clear_buffers(data, port);
	pw_array_clear(&port->buffer_ids);
}

static void node_proxy_destroy(void *_data)
{
	struct node_data *data = _data;
	struct pw_proxy *proxy = (struct pw_proxy*) data->node_proxy;
	int i;

	if (data->trans) {
		for (i = 0; i < data->trans->area->max_input_ports; i++)
			clear_port(data, &data->in_ports[i]);
		for (i = 0; i < data->trans->area->max_output_ports; i++)
			clear_port(data, &data->out_ports[i]);
	}
	clean_transport(proxy);

	spa_hook_remove(&data->node_listener);
}

static const struct pw_proxy_events proxy_events = {
	PW_VERSION_PROXY_EVENTS,
	.destroy = node_proxy_destroy,
};

static int impl_port_reuse_buffer(struct spa_node *node, uint32_t port_id, uint32_t buffer_id)
{
	pw_log_trace("node %p: reuse buffer %d %d", node, port_id, buffer_id);
	return 0;
}

static int impl_process_input(struct spa_node *node)
{
	struct node_data *data = SPA_CONTAINER_OF(node, struct node_data, out_node_impl);
	pw_log_trace("node %p: process input", node);
	node_have_output(data);
	return 0;
}

static int impl_process_output(struct spa_node *node)
{
	struct node_data *data = SPA_CONTAINER_OF(node, struct node_data, in_node_impl);
	pw_log_trace("node %p: process output", node);
	node_need_input(data);
	return 0;
}

static const struct spa_node node_impl = {
	SPA_VERSION_NODE,
	NULL,
	.process_input = impl_process_input,
	.process_output = impl_process_output,
	.port_reuse_buffer = impl_port_reuse_buffer,
};

SPA_EXPORT
struct pw_proxy *pw_remote_export(struct pw_remote *remote,
				  struct pw_node *node)
{
	struct remote *impl = SPA_CONTAINER_OF(remote, struct remote, this);
	struct pw_proxy *proxy;
	struct node_data *data;

	proxy = pw_core_proxy_create_object(remote->core_proxy,
					    "client-node",
					    impl->type_client_node,
					    PW_VERSION_CLIENT_NODE,
					    &node->properties->dict,
					    sizeof(struct node_data));
        if (proxy == NULL)
                return NULL;

	data = pw_proxy_get_user_data(proxy);
	data->remote = remote;
	data->node = node;
	data->core = pw_node_get_core(node);
	data->t = pw_core_get_type(data->core);
	data->node_proxy = (struct pw_client_node_proxy *)proxy;
	data->in_node_impl = node_impl;
	data->out_node_impl = node_impl;

        pw_array_init(&data->mem_ids, 64);
        pw_array_ensure_size(&data->mem_ids, sizeof(struct mem_id) * 64);

	spa_graph_node_init(&data->in_node);
	spa_graph_node_set_implementation(&data->in_node, &data->in_node_impl);
	spa_graph_node_init(&data->out_node);
	spa_graph_node_set_implementation(&data->out_node, &data->out_node_impl);

	pw_proxy_add_listener(proxy, &data->proxy_listener, &proxy_events, data);
	pw_node_add_listener(node, &data->node_listener, &node_events, data);

        pw_client_node_proxy_add_listener(data->node_proxy,
					  &data->node_proxy_listener,
					  &client_node_events,
					  proxy);
        do_node_init(proxy);

	return proxy;
}
