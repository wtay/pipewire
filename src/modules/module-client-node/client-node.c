/* PipeWire */
/* SPDX-FileCopyrightText: Copyright © 2018 Wim Taymans */
/* SPDX-License-Identifier: MIT */

#include <string.h>
#include <stddef.h>
#include <stdio.h>
#include <errno.h>
#include <unistd.h>
#include <time.h>

#include <spa/support/system.h>
#include <spa/node/node.h>
#include <spa/node/utils.h>
#include <spa/pod/filter.h>
#include <spa/pod/dynamic.h>
#include <spa/pod/parser.h>
#include <spa/debug/types.h>

#include <pipewire/pipewire.h>
#include "pipewire/private.h"

#include "modules/spa/spa-node.h"
#include "client-node.h"

PW_LOG_TOPIC_EXTERN(mod_topic);
#define PW_LOG_TOPIC_DEFAULT mod_topic

/** \cond */

#define MAX_BUFFERS	64
#define MAX_METAS	16u
#define MAX_DATAS	64u
#define AREA_SLOT	(sizeof(struct spa_io_async_buffers))
#define AREA_SIZE	(4096u / AREA_SLOT)
#define MAX_AREAS	32

#define CHECK_FREE_PORT(impl,d,p)	(p <= pw_map_get_size(&impl->ports[d]) && !CHECK_PORT(impl,d,p))
#define CHECK_PORT(impl,d,p)		(pw_map_lookup(&impl->ports[d], p) != NULL)
#define GET_PORT(impl,d,p)		(pw_map_lookup(&impl->ports[d], p))

#define CHECK_PORT_BUFFER(impl,b,p)      (b < p->n_buffers)

struct buffer {
	struct spa_buffer *outbuf;
	struct spa_buffer buffer;
	struct spa_meta metas[MAX_METAS];
	struct spa_data datas[MAX_DATAS];
	struct pw_memblock *mem;
};

struct mix {
	uint32_t mix_id;
	struct port *port;
	uint32_t peer_id;
	uint32_t n_buffers;
	uint32_t impl_mix_id;
	struct buffer buffers[MAX_BUFFERS];
};

struct params {
	uint32_t n_params;
	struct spa_pod **params;
};

struct port {
	struct pw_impl_port *port;
	struct impl *impl;

	enum spa_direction direction;
	uint32_t id;

	struct spa_node mix_node;
	struct spa_hook_list mix_hooks;

	struct spa_port_info info;
	struct pw_properties *properties;

	struct params params;

	unsigned int removed:1;
	unsigned int destroyed:1;

	struct pw_map mix;
};

struct impl {
	struct pw_impl_client_node this;

	struct pw_context *context;
	struct pw_mempool *context_pool;

	struct spa_node node;

	struct spa_log *log;
	struct spa_loop *data_loop;
	struct spa_system *data_system;

	struct spa_hook_list hooks;
	struct spa_callbacks callbacks;

	struct pw_resource *resource;
	struct pw_impl_client *client;
	struct pw_mempool *client_pool;

	struct spa_source data_source;

	struct pw_map ports[2];

	struct port dummy;

	struct params params;

	struct pw_map io_map;
	struct pw_array io_areas;

	struct pw_memblock *activation;

	struct spa_hook node_listener;
	struct spa_hook resource_listener;
	struct spa_hook object_listener;

	uint32_t node_id;

	uint32_t bind_node_version;
	uint32_t bind_node_id;
};

#define pw_client_node_resource(r,m,v,...)	\
	pw_resource_call_res(r,struct pw_client_node_events,m,v,__VA_ARGS__)

#define pw_client_node_resource_transport(r,...)	\
	pw_client_node_resource(r,transport,0,__VA_ARGS__)
#define pw_client_node_resource_set_param(r,...)	\
	pw_client_node_resource(r,set_param,0,__VA_ARGS__)
#define pw_client_node_resource_set_io(r,...)	\
	pw_client_node_resource(r,set_io,0,__VA_ARGS__)
#define pw_client_node_resource_event(r,...)	\
	pw_client_node_resource(r,event,0,__VA_ARGS__)
#define pw_client_node_resource_command(r,...)	\
	pw_client_node_resource(r,command,0,__VA_ARGS__)
#define pw_client_node_resource_add_port(r,...)	\
	pw_client_node_resource(r,add_port,0,__VA_ARGS__)
#define pw_client_node_resource_remove_port(r,...)	\
	pw_client_node_resource(r,remove_port,0,__VA_ARGS__)
#define pw_client_node_resource_port_set_param(r,...)	\
	pw_client_node_resource(r,port_set_param,0,__VA_ARGS__)
#define pw_client_node_resource_port_use_buffers(r,...)	\
	pw_client_node_resource(r,port_use_buffers,0,__VA_ARGS__)
#define pw_client_node_resource_port_set_io(r,...)	\
	pw_client_node_resource(r,port_set_io,0,__VA_ARGS__)
#define pw_client_node_resource_set_activation(r,...)	\
	pw_client_node_resource(r,set_activation,0,__VA_ARGS__)
#define pw_client_node_resource_port_set_mix_info(r,...)	\
	pw_client_node_resource(r,port_set_mix_info,1,__VA_ARGS__)

static int update_params(struct params *p, uint32_t n_params, const struct spa_pod **params)
{
	uint32_t i;
	for (i = 0; i < p->n_params; i++)
		free(p->params[i]);
	p->n_params = n_params;
	if (p->n_params == 0) {
		free(p->params);
		p->params = NULL;
	} else {
		struct spa_pod **np;
		np = pw_reallocarray(p->params, p->n_params, sizeof(struct spa_pod *));
		if (np == NULL) {
			pw_log_error("%p: can't realloc: %m", p);
			free(p->params);
			p->params = NULL;
			p->n_params = 0;
			return -errno;
		}
		p->params = np;
	}
	for (i = 0; i < p->n_params; i++)
		p->params[i] = params[i] ? spa_pod_copy(params[i]) : NULL;
	return 0;
}

static int
do_port_use_buffers(struct impl *impl,
		    enum spa_direction direction,
		    uint32_t port_id,
		    uint32_t mix_id,
		    uint32_t flags,
		    struct spa_buffer **buffers,
		    uint32_t n_buffers);

/** \endcond */

static struct mix *find_mix(struct port *p, uint32_t mix_id)
{
	if (mix_id == SPA_ID_INVALID)
		mix_id = 0;
	else
		mix_id++;

	return pw_map_lookup(&p->mix, mix_id);
}

static struct mix *create_mix(struct port *p, uint32_t mix_id)
{
	struct mix *mix = NULL;
	size_t len;
	int res;

	if (mix_id == SPA_ID_INVALID)
		mix_id = 0;
	else
		mix_id++;

	if (pw_map_lookup(&p->mix, mix_id) != NULL) {
		errno = EEXIST;
		return NULL;
	}

	/* pad map size */
	for (len = pw_map_get_size(&p->mix); len < mix_id; ++len)
		if ((res = pw_map_insert_at(&p->mix, len, NULL)) < 0)
			goto fail;

	mix = calloc(1, sizeof(struct mix));
	if (mix == NULL)
		return NULL;
	if ((res = pw_map_insert_at(&p->mix, mix_id, mix)) < 0)
		goto fail;

	mix->mix_id = mix_id;
	mix->port = p;
	mix->n_buffers = 0;
	mix->impl_mix_id = SPA_ID_INVALID;
	return mix;

fail:
	free(mix);
	errno = -res;
	return NULL;
}

static void clear_data(struct impl *impl, struct spa_data *d)
{
	switch (d->type) {
	case SPA_DATA_MemId:
	{
		uint32_t id;
		struct pw_memblock *m;

		id = SPA_PTR_TO_UINT32(d->data);
		m = pw_mempool_find_id(impl->client_pool, id);
		if (m) {
			pw_log_debug("%p: mem %d", impl, m->id);
			pw_memblock_unref(m);
		}
		break;
	}
	case SPA_DATA_MemFd:
	case SPA_DATA_DmaBuf:
		pw_log_debug("%p: close fd:%d", impl, (int)d->fd);
		close(d->fd);
		break;
	default:
		break;
	}
}

static void clear_buffer(struct impl *impl, struct spa_buffer *b)
{
	uint32_t i;
	for (i = 0; i < b->n_datas; i++)
		clear_data(impl, &b->datas[i]);
}

static int clear_buffers(struct impl *impl, struct mix *mix)
{
	uint32_t i;
	for (i = 0; i < mix->n_buffers; i++) {
		struct buffer *b = &mix->buffers[i];

		spa_log_debug(impl->log, "%p: clear buffer %d", impl, i);
		clear_buffer(impl, &b->buffer);
		pw_memblock_unref(b->mem);
	}
	mix->n_buffers = 0;
	return 0;
}

static void free_mix(struct port *p, struct mix *mix)
{
	struct impl *impl = p->impl;

	if (mix == NULL)
		return;

	if (mix->n_buffers) {
		/* this shouldn't happen */
		spa_log_warn(impl->log, "%p: mix port-id:%u freeing leaked buffers", impl, mix->mix_id - 1u);
	}

	clear_buffers(impl, mix);

	/* never realloc so it's safe to call from pw_map_foreach */
	if (mix->mix_id < pw_map_get_size(&p->mix))
		pw_map_insert_at(&p->mix, mix->mix_id, NULL);

	free(mix);
}

static void mix_clear(struct impl *impl, struct mix *mix)
{
	struct port *port = mix->port;

	do_port_use_buffers(impl, port->direction, port->id,
			mix->mix_id, 0, NULL, 0);

	free_mix(port, mix);
}

static int impl_node_enum_params(void *object, int seq,
				 uint32_t id, uint32_t start, uint32_t num,
				 const struct spa_pod *filter)
{
	struct impl *impl = object;
	uint8_t buffer[1024];
	struct spa_pod_dynamic_builder b;
	struct spa_result_node_params result;
	uint32_t count = 0;
	bool found = false;

	spa_return_val_if_fail(impl != NULL, -EINVAL);
	spa_return_val_if_fail(num != 0, -EINVAL);

	result.id = id;
	result.next = 0;

	while (true) {
		struct spa_pod *param;

		result.index = result.next++;
		if (result.index >= impl->params.n_params)
			break;

		param = impl->params.params[result.index];

		if (param == NULL || !spa_pod_is_object_id(param, id))
			continue;

		found = true;

		if (result.index < start)
			continue;

		spa_pod_dynamic_builder_init(&b, buffer, sizeof(buffer), 4096);
		if (spa_pod_filter(&b.b, &result.param, param, filter) == 0) {
			pw_log_debug("%p: %d param %u", impl, seq, result.index);
			spa_node_emit_result(&impl->hooks, seq, 0, SPA_RESULT_TYPE_NODE_PARAMS, &result);
			count++;
		}
		spa_pod_dynamic_builder_clean(&b);

		if (count == num)
			break;
	}
	return found ? 0 : -ENOENT;
}

static int impl_node_set_param(void *object, uint32_t id, uint32_t flags,
			       const struct spa_pod *param)
{
	struct impl *impl = object;

	spa_return_val_if_fail(impl != NULL, -EINVAL);

	if (impl->resource == NULL)
		return param == NULL ? 0 : -EIO;

	return pw_client_node_resource_set_param(impl->resource, id, flags, param);
}

static int impl_node_set_io(void *object, uint32_t id, void *data, size_t size)
{
	struct impl *impl = object;
	struct pw_memmap *mm, *old;
	uint32_t memid, mem_offset, mem_size;
	uint32_t tag[5] = { impl->node_id, id, };

	if (impl->this.flags & 1)
		return 0;

	old = pw_mempool_find_tag(impl->client_pool, tag, sizeof(tag));

	if (data) {
		mm = pw_mempool_import_map(impl->client_pool,
				impl->context_pool, data, size, tag);
		if (mm == NULL)
			return -errno;

		mem_offset = mm->offset;
		memid = mm->block->id;
		mem_size = size;
	}
	else {
		memid = SPA_ID_INVALID;
		mem_offset = mem_size = 0;
	}
	pw_memmap_free(old);

	if (impl->resource == NULL)
		return data == NULL ? 0 : -EIO;

	return pw_client_node_resource_set_io(impl->resource,
				       id,
				       memid,
				       mem_offset, mem_size);
}

static int impl_node_send_command(void *object, const struct spa_command *command)
{
	struct impl *impl = object;
	uint32_t id;

	spa_return_val_if_fail(impl != NULL, -EINVAL);
	spa_return_val_if_fail(command != NULL, -EINVAL);

	id = SPA_NODE_COMMAND_ID(command);
	pw_log_debug("%p: send command %d (%s)", impl, id,
		    spa_debug_type_find_name(spa_type_node_command_id, id));

	if (impl->resource == NULL)
		return -EIO;

	return pw_client_node_resource_command(impl->resource, command);
}


static void emit_port_info(struct impl *impl, struct port *port)
{
	spa_node_emit_port_info(&impl->hooks,
				port->direction, port->id, &port->info);
}

static int impl_node_add_listener(void *object,
		struct spa_hook *listener,
		const struct spa_node_events *events,
		void *data)
{
	struct impl *impl = object;
	struct spa_hook_list save;
	union pw_map_item *item;

	spa_return_val_if_fail(impl != NULL, -EINVAL);

	spa_hook_list_isolate(&impl->hooks, &save, listener, events, data);

	pw_array_for_each(item, &impl->ports[SPA_DIRECTION_INPUT].items) {
		if (item->data)
			emit_port_info(impl, item->data);
	}
	pw_array_for_each(item, &impl->ports[SPA_DIRECTION_OUTPUT].items) {
		if (item->data)
			emit_port_info(impl, item->data);
	}
	spa_hook_list_join(&impl->hooks, &save);

	return 0;
}

static int
impl_node_set_callbacks(void *object,
			const struct spa_node_callbacks *callbacks,
			void *data)
{
	struct impl *impl = object;

	spa_return_val_if_fail(impl != NULL, -EINVAL);

	impl->callbacks = SPA_CALLBACKS_INIT(callbacks, data);

	return 0;
}

static int
impl_node_sync(void *object, int seq)
{
	struct impl *impl = object;

	spa_return_val_if_fail(impl != NULL, -EINVAL);

	pw_log_debug("%p: sync", impl);

	if (impl->resource == NULL)
		return -EIO;

	return pw_resource_ping(impl->resource, seq);
}

static void
do_update_port(struct impl *impl,
	       struct port *port,
	       uint32_t change_mask,
	       uint32_t n_params,
	       const struct spa_pod **params,
	       const struct spa_port_info *info)
{
	if (change_mask & PW_CLIENT_NODE_PORT_UPDATE_PARAMS) {
		spa_log_debug(impl->log, "%p: port %u update %d params", impl, port->id, n_params);
		update_params(&port->params, n_params, params);
	}

	if (change_mask & PW_CLIENT_NODE_PORT_UPDATE_INFO) {
		pw_properties_free(port->properties);
		port->properties = NULL;
		port->info.props = NULL;
		port->info.n_params = 0;
		port->info.params = NULL;

		if (info) {
			port->info = *info;
			if (info->props) {
				port->properties = pw_properties_new_dict(info->props);
				port->info.props = &port->properties->dict;
			}
			port->info.n_params = 0;
			port->info.params = NULL;
			spa_node_emit_port_info(&impl->hooks, port->direction, port->id, info);
		}
	}
}

static int mix_clear_cb(void *item, void *data)
{
	if (item)
		mix_clear(data, item);
	return 0;
}

static void
clear_port(struct impl *impl, struct port *port)
{
	spa_log_debug(impl->log, "%p: clear port %p", impl, port);

	do_update_port(impl, port,
		       PW_CLIENT_NODE_PORT_UPDATE_PARAMS |
		       PW_CLIENT_NODE_PORT_UPDATE_INFO, 0, NULL, NULL);

	pw_map_for_each(&port->mix, mix_clear_cb, impl);
	pw_map_clear(&port->mix);
	pw_map_init(&port->mix, 0, 2);

	pw_map_insert_at(&impl->ports[port->direction], port->id, NULL);

	if (!port->removed)
		spa_node_emit_port_info(&impl->hooks, port->direction, port->id, NULL);
}

static int
impl_node_add_port(void *object, enum spa_direction direction, uint32_t port_id,
		const struct spa_dict *props)
{
	struct impl *impl = object;

	spa_return_val_if_fail(impl != NULL, -EINVAL);
	spa_return_val_if_fail(CHECK_FREE_PORT(impl, direction, port_id), -EINVAL);

	if (impl->resource == NULL)
		return -EIO;

	return pw_client_node_resource_add_port(impl->resource, direction, port_id, props);
}

static int
impl_node_remove_port(void *object, enum spa_direction direction, uint32_t port_id)
{
	struct impl *impl = object;

	spa_return_val_if_fail(impl != NULL, -EINVAL);
	spa_return_val_if_fail(CHECK_PORT(impl, direction, port_id), -EINVAL);

	if (impl->resource == NULL)
		return -EIO;

	return pw_client_node_resource_remove_port(impl->resource, direction, port_id);
}

static int
node_port_enum_params(struct impl *impl, int seq,
			   enum spa_direction direction, uint32_t port_id,
			   uint32_t id, uint32_t start, uint32_t num,
			   const struct spa_pod *filter, struct spa_hook_list *hooks)
{
	struct port *port;
	uint8_t buffer[1024];
	struct spa_pod_dynamic_builder b;
	struct spa_result_node_params result;
	uint32_t count = 0;
	bool found = false;

	spa_return_val_if_fail(impl != NULL, -EINVAL);
	spa_return_val_if_fail(num != 0, -EINVAL);

	port = GET_PORT(impl, direction, port_id);
	spa_return_val_if_fail(port != NULL, -EINVAL);

	pw_log_debug("%p: seq:%d port %d.%d id:%u start:%u num:%u n_params:%d",
			impl, seq, direction, port_id, id, start, num, port->params.n_params);

	result.id = id;
	result.next = 0;

	while (true) {
		struct spa_pod *param;

		result.index = result.next++;
		if (result.index >= port->params.n_params)
			break;

		param = port->params.params[result.index];

		if (param == NULL || !spa_pod_is_object_id(param, id))
			continue;

		found = true;

		if (result.index < start)
			continue;

		spa_pod_dynamic_builder_init(&b, buffer, sizeof(buffer), 4096);
		if (spa_pod_filter(&b.b, &result.param, param, filter) == 0) {
			pw_log_debug("%p: %d param %u", impl, seq, result.index);
			spa_node_emit_result(hooks, seq, 0, SPA_RESULT_TYPE_NODE_PARAMS, &result);
			count++;
		}
		spa_pod_dynamic_builder_clean(&b);

		if (count == num)
			break;
	}
	return found ? 0 : -ENOENT;
}

static int
impl_node_port_enum_params(void *object, int seq,
			   enum spa_direction direction, uint32_t port_id,
			   uint32_t id, uint32_t start, uint32_t num,
			   const struct spa_pod *filter)
{
	struct impl *impl = object;
	return node_port_enum_params(impl, seq, direction, port_id, id,
			start, num, filter, &impl->hooks);
}

static int clear_buffers_cb(void *item, void *data)
{
	if (item)
		clear_buffers(data, item);
	return 0;
}

static int
impl_node_port_set_param(void *object,
			 enum spa_direction direction, uint32_t port_id,
			 uint32_t id, uint32_t flags,
			 const struct spa_pod *param)
{
	struct impl *impl = object;
	struct port *port;

	spa_return_val_if_fail(impl != NULL, -EINVAL);

	port = GET_PORT(impl, direction, port_id);
	if(port == NULL)
		return param == NULL ? 0 : -EINVAL;

	pw_log_debug("%p: port %d.%d set param %s %d", impl,
			direction, port_id,
			spa_debug_type_find_name(spa_type_param, id), id);

	if (id == SPA_PARAM_Format)
		pw_map_for_each(&port->mix, clear_buffers_cb, impl);

	if (impl->resource == NULL)
		return param == NULL ? 0 : -EIO;

	return pw_client_node_resource_port_set_param(impl->resource,
					       direction, port_id,
					       id, flags,
					       param);
}

static int do_port_set_io(struct impl *impl,
			  enum spa_direction direction, uint32_t port_id,
			  uint32_t mix_id,
			  uint32_t id, void *data, size_t size)
{
	uint32_t memid, mem_offset, mem_size;
	struct port *port;
	struct mix *mix;
	uint32_t tag[5] = { impl->node_id, direction, port_id, mix_id, id };
	struct pw_memmap *mm, *old;

	pw_log_debug("%p: %s port %d.%d set io %p %zd", impl,
			direction == SPA_DIRECTION_INPUT ? "input" : "output",
			port_id, mix_id, data, size);

	old = pw_mempool_find_tag(impl->client_pool, tag, sizeof(tag));

	port = GET_PORT(impl, direction, port_id);
	if (port == NULL) {
		pw_memmap_free(old);
		return data == NULL ? 0 : -EINVAL;
	}

	if ((mix = find_mix(port, mix_id)) == NULL) {
		pw_memmap_free(old);
		return -EINVAL;
	}

	if (data) {
		mm = pw_mempool_import_map(impl->client_pool,
				impl->context_pool, data, size, tag);
		if (mm == NULL)
			return -errno;

		mem_offset = mm->offset;
		memid = mm->block->id;
		mem_size = size;
	}
	else {
		memid = SPA_ID_INVALID;
		mem_offset = mem_size = 0;
	}
	pw_memmap_free(old);

	if (impl->resource == NULL)
		return data == NULL ? 0 : -EIO;

	return pw_client_node_resource_port_set_io(impl->resource,
					    direction, port_id,
					    mix_id,
					    id,
					    memid,
					    mem_offset, mem_size);
}

static int
impl_node_port_set_io(void *object,
		      enum spa_direction direction,
		      uint32_t port_id,
		      uint32_t id,
		      void *data, size_t size)
{
	/* ignore io on the node itself, we only care about the io on the
	 * port mixers, the io on the node ports itself is handled on the
	 * client side */
	return -EINVAL;
}

static int
do_port_use_buffers(struct impl *impl,
		    enum spa_direction direction,
		    uint32_t port_id,
		    uint32_t mix_id,
		    uint32_t flags,
		    struct spa_buffer **buffers,
		    uint32_t n_buffers)
{
	struct port *p;
	struct mix *mix;
	uint32_t i, j;
	struct pw_client_node_buffer *mb;

	p = GET_PORT(impl, direction, port_id);
	if (p == NULL)
		return n_buffers == 0 ? 0 : -EINVAL;

	if (n_buffers > MAX_BUFFERS)
		return -ENOSPC;

	spa_log_debug(impl->log, "%p: %s port %d.%d use buffers %p %u flags:%08x", impl,
			direction == SPA_DIRECTION_INPUT ? "input" : "output",
			port_id, mix_id, buffers, n_buffers, flags);

	if (direction == SPA_DIRECTION_OUTPUT)
		mix_id = SPA_ID_INVALID;

	if ((mix = find_mix(p, mix_id)) == NULL)
		return -EINVAL;

	clear_buffers(impl, mix);

	if (n_buffers > 0) {
		mb = alloca(n_buffers * sizeof(struct pw_client_node_buffer));
	} else {
		mb = NULL;
	}

	if (impl->resource == NULL)
		return n_buffers == 0 ? 0 : -EIO;

	if (p->destroyed)
		return 0;

	for (i = 0; i < n_buffers; i++) {
		struct buffer *b = &mix->buffers[i];
		struct pw_memblock *mem, *m;
		void *baseptr, *endptr;

		b->outbuf = buffers[i];
		memcpy(&b->buffer, buffers[i], sizeof(struct spa_buffer));
		b->buffer.datas = b->datas;
		b->buffer.metas = b->metas;

		if (buffers[i]->n_metas > 0)
			baseptr = buffers[i]->metas[0].data;
		else if (buffers[i]->n_datas > 0)
			baseptr = buffers[i]->datas[0].chunk;
		else
			return -EINVAL;

		if ((mem = pw_mempool_find_ptr(impl->context_pool, baseptr)) == NULL)
			return -EINVAL;

		endptr = SPA_PTROFF(baseptr, buffers[i]->n_datas * sizeof(struct spa_chunk), void);
		for (j = 0; j < buffers[i]->n_metas; j++) {
			endptr = SPA_PTROFF(endptr, SPA_ROUND_UP_N(buffers[i]->metas[j].size, 8), void);
		}
		for (j = 0; j < buffers[i]->n_datas; j++) {
			struct spa_data *d = &buffers[i]->datas[j];
			if (d->type == SPA_DATA_MemPtr) {
				if ((m = pw_mempool_find_ptr(impl->context_pool, d->data)) == NULL ||
				    m != mem)
					return -EINVAL;
				endptr = SPA_MAX(endptr, SPA_PTROFF(d->data, d->maxsize, void));
			}
		}
		if (endptr > SPA_PTROFF(baseptr, mem->size, void))
			return -EINVAL;

		m = pw_mempool_import_block(impl->client_pool, mem);
		if (m == NULL)
			return -errno;

		b->mem = m;

		mb[i].buffer = &b->buffer;
		mb[i].mem_id = m->id;
		mb[i].offset = SPA_PTRDIFF(baseptr, mem->map->ptr);
		mb[i].size = SPA_PTRDIFF(endptr, baseptr);
		spa_log_debug(impl->log, "%p: buffer %d %d %d %d", impl, i, mb[i].mem_id,
				mb[i].offset, mb[i].size);

		b->buffer.n_metas = SPA_MIN(buffers[i]->n_metas, MAX_METAS);
		for (j = 0; j < b->buffer.n_metas; j++)
			memcpy(&b->buffer.metas[j], &buffers[i]->metas[j], sizeof(struct spa_meta));

		b->buffer.n_datas = SPA_MIN(buffers[i]->n_datas, MAX_DATAS);
		for (j = 0; j < b->buffer.n_datas; j++) {
			struct spa_data *d = &buffers[i]->datas[j];

			memcpy(&b->datas[j], d, sizeof(struct spa_data));

			if (flags & SPA_NODE_BUFFERS_FLAG_ALLOC)
				continue;

			switch (d->type) {
			case SPA_DATA_DmaBuf:
			case SPA_DATA_MemFd:
			case SPA_DATA_SyncObj:
			{
				uint32_t flags = PW_MEMBLOCK_FLAG_DONT_CLOSE;

				if (!(d->flags & SPA_DATA_FLAG_MAPPABLE))
					flags |= PW_MEMBLOCK_FLAG_UNMAPPABLE;
				if (d->flags & SPA_DATA_FLAG_READABLE)
					flags |= PW_MEMBLOCK_FLAG_READABLE;
				if (d->flags & SPA_DATA_FLAG_WRITABLE)
					flags |= PW_MEMBLOCK_FLAG_WRITABLE;

				spa_log_debug(impl->log, "mem %d type:%d fd:%d", j, d->type, (int)d->fd);
				m = pw_mempool_import(impl->client_pool,
					flags, d->type, d->fd);
				if (m == NULL)
					return -errno;

				b->datas[j].type = SPA_DATA_MemId;
				b->datas[j].data = SPA_UINT32_TO_PTR(m->id);
				break;
			}
			case SPA_DATA_MemPtr:
				spa_log_debug(impl->log, "mem %d %zd", j, SPA_PTRDIFF(d->data, baseptr));
				b->datas[j].data = SPA_INT_TO_PTR(SPA_PTRDIFF(d->data, baseptr));
				break;
			default:
				b->datas[j].type = SPA_ID_INVALID;
				b->datas[j].data = NULL;
				spa_log_error(impl->log, "invalid memory type %d", d->type);
				break;
			}
		}
	}
	mix->n_buffers = n_buffers;

	return pw_client_node_resource_port_use_buffers(impl->resource,
						 direction, port_id, mix_id, flags,
						 n_buffers, mb);
}

static int
impl_node_port_use_buffers(void *object,
			   enum spa_direction direction,
			   uint32_t port_id,
			   uint32_t flags,
			   struct spa_buffer **buffers,
			   uint32_t n_buffers)
{
	struct impl *impl = object;

	spa_return_val_if_fail(impl != NULL, -EINVAL);

	return do_port_use_buffers(impl, direction, port_id,
			SPA_ID_INVALID, flags, buffers, n_buffers);
}

static int
impl_node_port_reuse_buffer(void *object, uint32_t port_id, uint32_t buffer_id)
{
	struct impl *impl = object;

	spa_return_val_if_fail(impl != NULL, -EINVAL);
	spa_return_val_if_fail(CHECK_PORT(impl, SPA_DIRECTION_OUTPUT, port_id), -EINVAL);

	spa_log_trace_fp(impl->log, "reuse buffer %d", buffer_id);

	return -ENOTSUP;
}

static int impl_node_process(void *object)
{
	struct impl *impl = object;
	struct pw_impl_node *n = impl->this.node;
	struct timespec ts;

	/* this should not be called, we call the exported node
	 * directly */
	spa_log_warn(impl->log, "exported node activation");
	spa_system_clock_gettime(impl->data_system, CLOCK_MONOTONIC, &ts);
	n->rt.target.activation->status = PW_NODE_ACTIVATION_TRIGGERED;
	n->rt.target.activation->signal_time = SPA_TIMESPEC_TO_NSEC(&ts);

	if (SPA_UNLIKELY(spa_system_eventfd_write(n->rt.target.system, n->rt.target.fd, 1) < 0))
		pw_log_warn("%p: write failed %m", impl);

	return SPA_STATUS_OK;
}

static struct pw_node *
client_node_get_node(void *data,
		   uint32_t version,
		   size_t user_data_size)
{
	struct impl *impl = data;
	uint32_t new_id = user_data_size;

	pw_log_debug("%p: bind %u/%u", impl, new_id, version);

	impl->bind_node_version = version;
	impl->bind_node_id = new_id;
	pw_map_insert_at(&impl->client->objects, new_id, NULL);

	return NULL;
}

static int
client_node_update(void *data,
		   uint32_t change_mask,
		   uint32_t n_params,
		   const struct spa_pod **params,
		   const struct spa_node_info *info)
{
	struct impl *impl = data;

	if (change_mask & PW_CLIENT_NODE_UPDATE_PARAMS) {
		pw_log_debug("%p: update %d params", impl, n_params);
		update_params(&impl->params, n_params, params);
	}
	if (change_mask & PW_CLIENT_NODE_UPDATE_INFO) {
		spa_node_emit_info(&impl->hooks, info);
	}
	pw_log_debug("%p: got node update", impl);
	return 0;
}

static int
client_node_port_update(void *data,
			enum spa_direction direction,
			uint32_t port_id,
			uint32_t change_mask,
			uint32_t n_params,
			const struct spa_pod **params,
			const struct spa_port_info *info)
{
	struct impl *impl = data;
	struct port *port;
	bool remove;

	spa_log_debug(impl->log, "%p: got port update change:%08x params:%d",
			impl, change_mask, n_params);

	remove = (change_mask == 0);

	port = GET_PORT(impl, direction, port_id);

	if (remove) {
		if (port == NULL)
			return 0;
		port->destroyed = true;
		clear_port(impl, port);
	} else {
		struct port *target;

		if (port == NULL) {
			if (!CHECK_FREE_PORT(impl, direction, port_id))
				return -EINVAL;

			target = &impl->dummy;
			spa_zero(impl->dummy);
			target->direction = direction;
			target->id = port_id;
		} else
			target = port;

		do_update_port(impl,
			       target,
			       change_mask,
			       n_params, params,
			       info);
	}
	return 0;
}

static int client_node_set_active(void *data, bool active)
{
	struct impl *impl = data;
	spa_log_debug(impl->log, "%p: active:%d", impl, active);
	return pw_impl_node_set_active(impl->this.node, active);
}

static int client_node_event(void *data, const struct spa_event *event)
{
	struct impl *impl = data;
	spa_node_emit_event(&impl->hooks, event);
	return 0;
}

static int client_node_port_buffers(void *data,
			enum spa_direction direction,
			uint32_t port_id,
			uint32_t mix_id,
			uint32_t n_buffers,
			struct spa_buffer **buffers)
{
	struct impl *impl = data;
	struct port *p;
	struct mix *mix;
	uint32_t i, j;

	spa_log_debug(impl->log, "%p: %s port %d.%d buffers %p %u", impl,
			direction == SPA_DIRECTION_INPUT ? "input" : "output",
			port_id, mix_id, buffers, n_buffers);

	p = GET_PORT(impl, direction, port_id);
	spa_return_val_if_fail(p != NULL, -EINVAL);

	if (direction == SPA_DIRECTION_OUTPUT)
		mix_id = SPA_ID_INVALID;

	if ((mix = find_mix(p, mix_id)) == NULL)
		goto invalid;

	if (mix->n_buffers != n_buffers)
		goto invalid;

	for (i = 0; i < n_buffers; i++) {
		if (mix->buffers[i].outbuf->n_datas != buffers[i]->n_datas)
			goto invalid;
	}

	for (i = 0; i < n_buffers; i++) {
		struct spa_buffer *oldbuf, *newbuf;
		struct buffer *b = &mix->buffers[i];

		oldbuf = b->outbuf;
		newbuf = buffers[i];

		spa_log_debug(impl->log, "buffer %d n_datas:%d", i, newbuf->n_datas);

		for (j = 0; j < b->buffer.n_datas; j++) {
			struct spa_chunk *oldchunk = oldbuf->datas[j].chunk;
			struct spa_data *d = &newbuf->datas[j];
			uint32_t flags = d->flags;

			if (d->type == SPA_DATA_MemFd &&
			    !SPA_FLAG_IS_SET(flags, SPA_DATA_FLAG_MAPPABLE)) {
				spa_log_debug(impl->log, "buffer:%d data:%d has non mappable MemFd, "
						"fixing to ensure backwards compatibility.",
						i, j);
				flags |= SPA_DATA_FLAG_MAPPABLE;
			}

			/* overwrite everything except the chunk */
			oldbuf->datas[j] = *d;
			oldbuf->datas[j].flags = flags;
			oldbuf->datas[j].chunk = oldchunk;

			b->datas[j].type = d->type;
			b->datas[j].flags = flags;
			b->datas[j].fd = d->fd;

			spa_log_debug(impl->log, " data %d type:%d fl:%08x fd:%d, offs:%d max:%d",
					j, d->type, flags, (int) d->fd, d->mapoffset,
					d->maxsize);
		}
	}
	return 0;
invalid:
	for (i = 0; i < n_buffers; i++)
		clear_buffer(impl, buffers[i]);
	return -EINVAL;
}

static const struct pw_client_node_methods client_node_methods = {
	PW_VERSION_CLIENT_NODE_METHODS,
	.get_node = client_node_get_node,
	.update = client_node_update,
	.port_update = client_node_port_update,
	.set_active = client_node_set_active,
	.event = client_node_event,
	.port_buffers = client_node_port_buffers,
};

static void node_on_data_fd_events(struct spa_source *source)
{
	struct impl *impl = source->data;

	if (SPA_UNLIKELY(source->rmask & (SPA_IO_ERR | SPA_IO_HUP))) {
		spa_log_warn(impl->log, "%p: got error", impl);
		return;
	}
	if (SPA_LIKELY(source->rmask & SPA_IO_IN)) {
		uint64_t cmd;
		struct pw_impl_node *node = impl->this.node;

		if (SPA_UNLIKELY(spa_system_eventfd_read(impl->data_system,
					impl->data_source.fd, &cmd) < 0))
			pw_log_warn("%p: read failed %m", impl);
		else if (SPA_UNLIKELY(cmd > 1))
			pw_log_info("(%s-%u) client missed %"PRIu64" wakeups",
				node->name, node->info.id, cmd - 1);

		if (impl->resource && impl->resource->version < 5) {
			struct pw_node_activation *a = node->rt.target.activation;
			int status = a->state[0].status;
			spa_log_trace_fp(impl->log, "%p: got ready %d", impl, status);
			spa_node_call_ready(&impl->callbacks, status);
		} else {
			spa_log_trace_fp(impl->log, "%p: got complete", impl);
			pw_impl_node_rt_emit_complete(node);
		}
	}
}

static const struct spa_node_methods impl_node = {
	SPA_VERSION_NODE_METHODS,
	.add_listener = impl_node_add_listener,
	.set_callbacks = impl_node_set_callbacks,
	.sync = impl_node_sync,
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

static int
impl_init(struct impl *impl,
	  struct spa_dict *info)
{
	impl->node.iface = SPA_INTERFACE_INIT(
			SPA_TYPE_INTERFACE_Node,
			SPA_VERSION_NODE,
			&impl_node, impl);
	spa_hook_list_init(&impl->hooks);

	impl->data_source.func = node_on_data_fd_events;
	impl->data_source.data = impl;
	impl->data_source.fd = -1;
	impl->data_source.mask = SPA_IO_IN | SPA_IO_ERR | SPA_IO_HUP;
	impl->data_source.rmask = 0;

	return 0;
}

static int impl_clear(struct impl *impl)
{
	update_params(&impl->params, 0, NULL);
	return 0;
}

static int do_remove_source(struct spa_loop *loop,
			    bool async,
			    uint32_t seq,
			    const void *data,
			    size_t size,
			    void *user_data)
{
	struct spa_source *source = user_data;
	spa_loop_remove_source(loop, source);
	return 0;
}

static void client_node_resource_destroy(void *data)
{
	struct impl *impl = data;
	struct pw_impl_client_node *this = &impl->this;

	pw_log_debug("%p: destroy", impl);

	impl->resource = this->resource = NULL;
	spa_hook_remove(&impl->resource_listener);
	spa_hook_remove(&impl->object_listener);

	if (impl->data_source.fd != -1) {
		spa_loop_invoke(impl->data_loop,
				do_remove_source,
				SPA_ID_INVALID,
				NULL,
				0,
				true,
				&impl->data_source);
	}
	if (this->node)
		pw_impl_node_destroy(this->node);
}

static void client_node_resource_error(void *data, int seq, int res, const char *message)
{
	struct impl *impl = data;
	struct spa_result_node_error result;

	pw_log_error("%p: error seq:%d %d (%s)", impl, seq, res, message);
	result.message = message;
	spa_node_emit_result(&impl->hooks, seq, res, SPA_RESULT_TYPE_NODE_ERROR, &result);
}

static void client_node_resource_pong(void *data, int seq)
{
	struct impl *impl = data;
	pw_log_debug("%p: got pong, emit result %d", impl, seq);
	spa_node_emit_result(&impl->hooks, seq, 0, 0, NULL);
}

static void node_peer_added(void *data, struct pw_impl_node *peer)
{
	struct impl *impl = data;
	struct pw_memblock *m;

	m = pw_mempool_import_block(impl->client_pool, peer->activation);
	if (m == NULL) {
		pw_log_warn("%p: can't ensure mem: %m", impl);
		return;
	}

	pw_log_debug("%p: peer %p/%p id:%u added mem_id:%u %p %d", impl, peer,
			impl->this.node, peer->info.id, m->id, m, m->ref);

	if (impl->resource == NULL)
		return;

	pw_client_node_resource_set_activation(impl->resource,
					  peer->info.id,
					  peer->source.fd,
					  m->id,
					  0,
					  sizeof(struct pw_node_activation));
}

static void node_peer_removed(void *data, struct pw_impl_node *peer)
{
	struct impl *impl = data;
	struct pw_memblock *m;

	m = pw_mempool_find_fd(impl->client_pool, peer->activation->fd);
	if (m == NULL) {
		pw_log_warn("%p: unknown peer %p fd:%d", impl, peer,
			peer->source.fd);
		return;
	}

	pw_log_debug("%p: peer %p/%p id:%u removed mem_id:%u", impl, peer,
			impl->this.node, peer->info.id, m->id);

	if (impl->resource != NULL) {
		pw_client_node_resource_set_activation(impl->resource,
					  peer->info.id,
					  -1,
					  SPA_ID_INVALID,
					  0,
					  0);
	}
	pw_memblock_unref(m);
}

void pw_impl_client_node_registered(struct pw_impl_client_node *this, struct pw_global *global)
{
	struct impl *impl = SPA_CONTAINER_OF(this, struct impl, this);
	struct pw_impl_node *node = this->node;
	struct pw_impl_client *client = impl->client;
	uint32_t node_id = global->id;

	pw_log_debug("%p: %d", &impl->node, node_id);

	impl->activation = pw_mempool_import_block(impl->client_pool, node->activation);
	if (impl->activation == NULL) {
		pw_log_debug("%p: can't import block: %m", &impl->node);
		return;
	}
	impl->node_id = node_id;

	if (impl->resource == NULL)
		return;

	pw_resource_set_bound_id(impl->resource, node_id);

	pw_client_node_resource_transport(impl->resource,
					  this->node->source.fd,
					  impl->data_source.fd,
					  impl->activation->id,
					  0,
					  sizeof(struct pw_node_activation));

	if (impl->bind_node_id) {
		pw_global_bind(global, client, PW_PERM_ALL,
				impl->bind_node_version, impl->bind_node_id);
	}
}

static int add_area(struct impl *impl)
{
	size_t size;
	struct pw_memblock *area;

	size = AREA_SLOT * AREA_SIZE;

	area = pw_mempool_alloc(impl->context_pool,
			PW_MEMBLOCK_FLAG_READWRITE |
			PW_MEMBLOCK_FLAG_SEAL |
			PW_MEMBLOCK_FLAG_MAP,
			SPA_DATA_MemFd, size);
	if (area == NULL)
                return -errno;

	pw_log_debug("%p: io area %u %p", impl,
			(unsigned)pw_array_get_len(&impl->io_areas, struct pw_memblock*),
			area->map->ptr);

	pw_array_add_ptr(&impl->io_areas, area);
	return 0;
}

static void node_initialized(void *data)
{
	struct impl *impl = data;
	struct pw_impl_client_node *this = &impl->this;
	struct pw_global *global;
	struct spa_system *data_system = impl->data_system;

	impl->data_source.fd = spa_system_eventfd_create(data_system,
			SPA_FD_CLOEXEC | SPA_FD_NONBLOCK);

	spa_loop_add_source(impl->data_loop, &impl->data_source);
	pw_log_debug("%p: transport read-fd:%d write-fd:%d", impl,
			impl->data_source.fd, this->node->source.fd);

	if (add_area(impl) < 0)
		return;

	if ((global = pw_impl_node_get_global(this->node)) != NULL)
		pw_impl_client_node_registered(this, global);
}

static void node_free(void *data)
{
	struct impl *impl = data;
	struct pw_impl_client_node *this = &impl->this;
	struct spa_system *data_system = impl->data_system;
	uint32_t tag[5] = { impl->node_id, };
	struct pw_memmap *mm;
	struct pw_memblock **area;

	this->node = NULL;

	pw_log_debug("%p: free", impl);
	impl_clear(impl);

	spa_hook_remove(&impl->node_listener);

	while ((mm = pw_mempool_find_tag(impl->client_pool, tag, sizeof(uint32_t))) != NULL)
		pw_memmap_free(mm);

	if (impl->activation)
		pw_memblock_free(impl->activation);

	pw_array_for_each(area, &impl->io_areas) {
		if (*area)
			pw_memblock_unref(*area);
	}
	pw_array_clear(&impl->io_areas);

	if (impl->resource)
		pw_resource_destroy(impl->resource);

	pw_map_clear(&impl->ports[0]);
	pw_map_clear(&impl->ports[1]);
	pw_map_clear(&impl->io_map);

	if (impl->data_source.fd != -1)
		spa_system_close(data_system, impl->data_source.fd);
	free(impl);
}

static int port_init_mix(void *data, struct pw_impl_port_mix *mix)
{
	struct port *port = data;
	struct impl *impl = port->impl;
	struct mix *m;
	uint32_t idx, pos, len;
	struct pw_memblock *area;
	struct spa_io_async_buffers *ab;

	if ((m = create_mix(port, mix->port.port_id)) == NULL)
		return -ENOMEM;

	mix->id = pw_map_insert_new(&impl->io_map, NULL);
	if (mix->id == SPA_ID_INVALID) {
		free_mix(port, m);
		return -errno;
	}

	idx = mix->id / AREA_SIZE;
	pos = mix->id % AREA_SIZE;

	len = pw_array_get_len(&impl->io_areas, struct pw_memblock *);
	if (idx > len)
		goto no_mem;
	if (idx == len) {
		pw_log_debug("%p: extend area idx:%u pos:%u", impl, idx, pos);
		if (add_area(impl) < 0)
			goto no_mem;
	}
	area = *pw_array_get_unchecked(&impl->io_areas, idx, struct pw_memblock*);

	ab = SPA_PTROFF(area->map->ptr, pos * AREA_SLOT, void);
	mix->io_data = ab;
	mix->io[0] = &ab->buffers[0];
	mix->io[1] = &ab->buffers[1];
	*mix->io[0] = SPA_IO_BUFFERS_INIT;
	*mix->io[1] = SPA_IO_BUFFERS_INIT;

	m->peer_id = mix->peer_id;
	m->impl_mix_id = mix->id;

	if (impl->resource && impl->resource->version >= 4)
		pw_client_node_resource_port_set_mix_info(impl->resource,
					 mix->port.direction, mix->p->port_id,
					 mix->port.port_id, mix->peer_id, NULL);

	pw_log_debug("%p: init mix id:%d io:%p/%p base:%p", impl,
			mix->id, mix->io[0], mix->io[1], area->map->ptr);

	return 0;
no_mem:
	pw_map_remove(&impl->io_map, mix->id);
	free_mix(port, m);
	return -ENOMEM;
}

static int port_release_mix(void *data, struct pw_impl_port_mix *mix)
{
	struct port *port = data;
	struct impl *impl = port->impl;
	struct mix *m;

	pw_log_debug("%p: remove mix id:%d io:%p",
			impl, mix->id, mix->io);

	if (!pw_map_has_item(&impl->io_map, mix->id))
		return -EINVAL;

	if (impl->resource && impl->resource->version >= 4)
		pw_client_node_resource_port_set_mix_info(impl->resource,
					 mix->port.direction, mix->p->port_id,
					 mix->port.port_id, SPA_ID_INVALID, NULL);

	pw_map_remove(&impl->io_map, mix->id);

	m = find_mix(port, mix->port.port_id);
	if (m && m->impl_mix_id == mix->id)
		free_mix(port, m);
	else
		pw_log_debug("%p: already cleared mix id:%d port-id:%d",
				impl, mix->id, mix->port.port_id);

	return 0;
}

static const struct pw_impl_port_implementation port_impl = {
	PW_VERSION_PORT_IMPLEMENTATION,
	.init_mix = port_init_mix,
	.release_mix = port_release_mix,
};

static int
impl_mix_add_listener(void *object, struct spa_hook *listener,
		const struct spa_node_events *events, void *data)
{
	struct port *port = object;
	spa_hook_list_append(&port->mix_hooks, listener, events, data);
	return 0;
}

static int
impl_mix_port_enum_params(void *object, int seq,
		enum spa_direction direction, uint32_t port_id,
		uint32_t id, uint32_t start, uint32_t num,
		const struct spa_pod *filter)
{
	struct port *port = object;

	if (port->direction != direction)
		return -ENOTSUP;

	return node_port_enum_params(port->impl, seq, direction, port->id,
			id, start, num, filter, &port->mix_hooks);
}

static int
impl_mix_port_set_param(void *object,
			enum spa_direction direction, uint32_t port_id,
			uint32_t id, uint32_t flags,
			const struct spa_pod *param)
{
	return -ENOTSUP;
}

static int
impl_mix_add_port(void *object, enum spa_direction direction, uint32_t mix_id,
		const struct spa_dict *props)
{
	struct port *port = object;
	pw_log_debug("%p: add port %d:%d.%d", object, direction, port->id, mix_id);
	return 0;
}

static int
impl_mix_remove_port(void *object, enum spa_direction direction, uint32_t mix_id)
{
	struct port *port = object;
	pw_log_debug("%p: remove port %d:%d.%d", object, direction, port->id, mix_id);
	return 0;
}

static int
impl_mix_port_use_buffers(void *object,
			   enum spa_direction direction,
			   uint32_t mix_id,
			   uint32_t flags,
			   struct spa_buffer **buffers,
			   uint32_t n_buffers)
{
	struct port *port = object;
	struct impl *impl = port->impl;

	return do_port_use_buffers(impl, direction, port->id, mix_id, flags, buffers, n_buffers);
}

static int impl_mix_port_set_io(void *object,
			   enum spa_direction direction, uint32_t mix_id,
			   uint32_t id, void *data, size_t size)
{
	struct port *p = object;
	struct pw_impl_port *port = p->port;
	struct impl *impl = port->owner_data;
	struct pw_impl_port_mix *mix;

	mix = pw_map_lookup(&port->mix_port_map, mix_id);
	if (mix == NULL)
		return -EINVAL;

	switch (id) {
	case SPA_IO_Buffers:
		if (data && size >= sizeof(struct spa_io_buffers))
			mix->io[0] = mix->io[1] = data;
		else
			mix->io[0] = mix->io[1] = NULL;
		break;
	case SPA_IO_AsyncBuffers:
		if (data && size >= sizeof(struct spa_io_async_buffers)) {
			struct spa_io_async_buffers *ab = data;
			mix->io[0] = &ab->buffers[0];
			mix->io[1] = &ab->buffers[1];
		}
		else
			mix->io[0] = mix->io[1] = NULL;
		break;
	default:
		break;
	}
	return do_port_set_io(impl,
			      direction, port->port_id, mix->port.port_id,
			      id, data, size);
}

static int
impl_mix_port_reuse_buffer(void *object, uint32_t port_id, uint32_t buffer_id)
{
	struct port *p = object;
	return impl_node_port_reuse_buffer(p->impl, p->id, buffer_id);
}

static int impl_mix_process(void *object)
{
	return SPA_STATUS_HAVE_DATA;
}

static const struct spa_node_methods impl_port_mix = {
	SPA_VERSION_NODE_METHODS,
	.add_listener = impl_mix_add_listener,
	.port_enum_params = impl_mix_port_enum_params,
	.port_set_param = impl_mix_port_set_param,
	.add_port = impl_mix_add_port,
	.remove_port = impl_mix_remove_port,
	.port_use_buffers = impl_mix_port_use_buffers,
	.port_set_io = impl_mix_port_set_io,
	.port_reuse_buffer = impl_mix_port_reuse_buffer,
	.process = impl_mix_process,
};

static void node_port_init(void *data, struct pw_impl_port *port)
{
	struct impl *impl = data;
	struct port *p = pw_impl_port_get_user_data(port);

	pw_log_debug("%p: port %p init", impl, port);

	*p = impl->dummy;
	p->port = port;
	p->impl = impl;
	p->direction = port->direction;
	p->id = port->port_id;
	p->impl = impl;
	pw_map_init(&p->mix, 2, 2);
	spa_hook_list_init(&p->mix_hooks);
	p->mix_node.iface = SPA_INTERFACE_INIT(
			SPA_TYPE_INTERFACE_Node,
			SPA_VERSION_NODE,
			&impl_port_mix, p);
	create_mix(p, SPA_ID_INVALID);

	pw_map_insert_at(&impl->ports[p->direction], p->id, p);
	return;
}

static void node_port_added(void *data, struct pw_impl_port *port)
{
	struct impl *impl = data;
	struct port *p = pw_impl_port_get_user_data(port);

	port->flags |= PW_IMPL_PORT_FLAG_NO_MIXER;

	port->impl = SPA_CALLBACKS_INIT(&port_impl, p);
	port->owner_data = impl;

	pw_impl_port_set_mix(port, &p->mix_node,
			PW_IMPL_PORT_MIX_FLAG_MULTI |
			PW_IMPL_PORT_MIX_FLAG_MIX_ONLY);
}

static void node_port_removed(void *data, struct pw_impl_port *port)
{
	struct impl *impl = data;
	struct port *p = pw_impl_port_get_user_data(port);

	pw_log_debug("%p: port %p remove", impl, port);

	p->removed = true;
	clear_port(impl, p);
}

static const struct pw_impl_node_events node_events = {
	PW_VERSION_IMPL_NODE_EVENTS,
	.free = node_free,
	.initialized = node_initialized,
	.port_init = node_port_init,
	.port_added = node_port_added,
	.port_removed = node_port_removed,
	.peer_added = node_peer_added,
	.peer_removed = node_peer_removed,
};

static const struct pw_resource_events resource_events = {
	PW_VERSION_RESOURCE_EVENTS,
	.destroy = client_node_resource_destroy,
	.error = client_node_resource_error,
	.pong = client_node_resource_pong,
};

/** Create a new client node
 * \param client an owner \ref pw_client
 * \param id an id
 * \param name a name
 * \param properties extra properties
 * \return a newly allocated client node
 *
 * Create a new \ref pw_impl_node.
 *
 * \memberof pw_impl_client_node
 */
struct pw_impl_client_node *pw_impl_client_node_new(struct pw_resource *resource,
					  struct pw_properties *properties,
					  bool do_register)
{
	struct impl *impl;
	struct pw_impl_client_node *this;
	struct pw_impl_client *client = pw_resource_get_client(resource);
	struct pw_context *context = pw_impl_client_get_context(client);
	int res;

	impl = calloc(1, sizeof(struct impl));
	if (impl == NULL) {
		res = -errno;
		goto error_exit_cleanup;
	}

	if (properties == NULL)
		properties = pw_properties_new(NULL, NULL);
	if (properties == NULL) {
		res = -errno;
		goto error_exit_free;
	}

	pw_properties_setf(properties, PW_KEY_CLIENT_ID, "%d", client->global->id);

	this = &impl->this;

	impl->context = context;
	impl->context_pool = pw_context_get_mempool(context);
	impl->data_source.fd = -1;
	pw_log_debug("%p: new", &impl->node);

	impl_init(impl, NULL);
	impl->log = pw_log_get();
	impl->resource = resource;
	impl->client = client;
	impl->client_pool = pw_impl_client_get_mempool(client);
	this->flags = do_register ? 0 : 1;

	pw_map_init(&impl->ports[0], 64, 64);
	pw_map_init(&impl->ports[1], 64, 64);
	pw_map_init(&impl->io_map, 64, 64);
	pw_array_init(&impl->io_areas, 64 * sizeof(struct pw_memblock*));

	this->resource = resource;
	this->node = pw_spa_node_new(context,
				     PW_SPA_NODE_FLAG_ASYNC |
				     (do_register ? 0 : PW_SPA_NODE_FLAG_NO_REGISTER),
				     (struct spa_node *)&impl->node,
				     NULL,
				     properties, 0);

	if (this->node == NULL)
		goto error_no_node;

	if (this->node->data_loop == NULL) {
		errno = EIO;
		goto error_no_node;
	}

	impl->data_loop = this->node->data_loop->loop;
	impl->data_system = this->node->data_loop->system;

	this->node->remote = true;
	this->flags = 0;

	pw_resource_add_listener(this->resource,
				&impl->resource_listener,
				&resource_events,
				impl);
	pw_resource_add_object_listener(this->resource,
				&impl->object_listener,
				&client_node_methods,
				impl);

	this->node->port_user_data_size = sizeof(struct port);

	pw_impl_node_add_listener(this->node, &impl->node_listener, &node_events, impl);

	return this;

error_no_node:
	res = -errno;
	impl_clear(impl);
	properties = NULL;
	goto error_exit_free;

error_exit_free:
	free(impl);
error_exit_cleanup:
	if (resource)
		pw_resource_destroy(resource);
	pw_properties_free(properties);
	errno = -res;
	return NULL;
}

/** Destroy a client node
 * \param node the client node to destroy
 * \memberof pw_impl_client_node
 */
void pw_impl_client_node_destroy(struct pw_impl_client_node *node)
{
	pw_resource_destroy(node->resource);
}
