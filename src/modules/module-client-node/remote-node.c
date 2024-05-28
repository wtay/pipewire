/* PipeWire */
/* SPDX-FileCopyrightText: Copyright © 2018 Wim Taymans */
/* SPDX-License-Identifier: MIT */

#include <stdio.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <errno.h>
#include <time.h>
#include <sys/mman.h>

#include <spa/pod/parser.h>
#include <spa/pod/dynamic.h>
#include <spa/node/utils.h>
#include <spa/utils/result.h>
#include <spa/debug/types.h>

#include "pipewire/pipewire.h"
#include "pipewire/private.h"

#include "pipewire/extensions/protocol-native.h"
#include "pipewire/extensions/client-node.h"

#define MAX_BUFFERS	64

PW_LOG_TOPIC_EXTERN(mod_topic);
#define PW_LOG_TOPIC_DEFAULT mod_topic

/** \cond */
static bool mlock_warned = false;

struct buffer {
	uint32_t id;
	struct spa_buffer *buf;
	struct pw_memmap *mem;
};

struct mix {
	struct spa_list link;
	struct pw_impl_port *port;
	struct pw_impl_port_mix mix;
	struct pw_array buffers;
};

struct node_data {
	struct pw_context *context;

	struct pw_loop *data_loop;
	struct spa_system *data_system;

	struct pw_mempool *pool;

	uint32_t remote_id;
	int rtwritefd;
	struct pw_memmap *activation;

	struct spa_list mix[2];
	struct spa_list free_mix;

	struct pw_impl_node *node;
	struct spa_hook node_listener;
	struct spa_hook node_rt_listener;
	unsigned int do_free:1;
	unsigned int have_transport:1;
	unsigned int allow_mlock:1;
	unsigned int warn_mlock:1;

	struct pw_client_node *client_node;
	struct spa_hook client_node_listener;
	struct spa_hook proxy_client_node_listener;

	struct spa_list links;
};

struct link {
	struct spa_list link;
	struct node_data *data;
	struct pw_memmap *map;
	struct pw_node_target target;
	uint32_t node_id;
};

/** \endcond */

static struct link *find_activation(struct spa_list *links, uint32_t node_id)
{
	struct link *l;

	spa_list_for_each(l, links, link) {
		if (l->target.id == node_id)
			return l;
	}
	return NULL;
}

static void clear_link(struct node_data *data, struct link *link)
{
	pw_log_debug("link %p", link);
	pw_impl_node_remove_target(data->node, &link->target);
	pw_memmap_free(link->map);
	spa_system_close(link->target.system, link->target.fd);
	spa_list_remove(&link->link);
	free(link);
}

static void clean_transport(struct node_data *data)
{
	struct link *l;
	uint32_t tag[5] = { data->remote_id, };
	struct pw_impl_node *node = data->node;
	struct pw_memmap *mm;

	if (!data->have_transport)
		return;

	spa_list_consume(l, &data->links, link)
		clear_link(data, l);

	while ((mm = pw_mempool_find_tag(data->pool, tag, sizeof(uint32_t))) != NULL) {
		if (mm->tag[1] == SPA_ID_INVALID)
			spa_node_set_io(node->node, mm->tag[2], NULL, 0);

		pw_memmap_free(mm);
	}

	pw_memmap_free(data->activation);
	node->rt.target.activation = node->activation->map->ptr;

	spa_system_close(data->data_system, data->rtwritefd);
	data->have_transport = false;
}

static void mix_init(struct mix *mix, struct pw_impl_port *port,
		uint32_t mix_id, uint32_t peer_id)
{
	pw_log_debug("port %p: mix init %d.%d", port, port->port_id, mix_id);
	mix->port = port;
	mix->mix.id = mix_id;
	mix->mix.peer_id = peer_id;
	if (mix_id != SPA_ID_INVALID)
		pw_impl_port_init_mix(port, &mix->mix);
	pw_array_init(&mix->buffers, 32);
	pw_array_ensure_size(&mix->buffers, sizeof(struct buffer) * 64);
}

static struct mix *find_mix(struct node_data *data,
		enum spa_direction direction, uint32_t port_id, uint32_t mix_id)
{
	struct mix *mix;

	spa_list_for_each(mix, &data->mix[direction], link) {
		if (mix->port->port_id == port_id &&
		    mix->mix.id == mix_id) {
			pw_log_debug("port %p: found mix %d:%d.%d", mix->port,
					direction, port_id, mix_id);
			return mix;
		}
	}
	return NULL;
}

static struct mix *create_mix(struct node_data *data, struct pw_impl_port *port,
		uint32_t mix_id, uint32_t peer_id)
{
	struct mix *mix;

	if (spa_list_is_empty(&data->free_mix)) {
		if ((mix = calloc(1, sizeof(*mix))) == NULL)
			return NULL;
	} else {
		mix = spa_list_first(&data->free_mix, struct mix, link);
		spa_list_remove(&mix->link);
	}
	mix_init(mix, port, mix_id, peer_id);
	spa_list_append(&data->mix[port->direction], &mix->link);

	return mix;
}

static int client_node_transport(void *_data,
			int readfd, int writefd, uint32_t mem_id, uint32_t offset, uint32_t size)
{
	struct node_data *data = _data;
	struct pw_impl_node *node = data->node;
	struct pw_proxy *proxy = (struct pw_proxy*)data->client_node;

	clean_transport(data);

	data->activation = pw_mempool_map_id(data->pool, mem_id,
				PW_MEMMAP_FLAG_READWRITE, offset, size, NULL);
	if (data->activation == NULL) {
		pw_log_warn("remote-node %p: can't map activation: %m", proxy);
		return -errno;
	}

	node->rt.target.activation = data->activation->ptr;

	pw_impl_node_set_io(node, SPA_IO_Clock,
			&node->rt.target.activation->position.clock,
			sizeof(struct spa_io_clock));
	pw_impl_node_set_io(node, SPA_IO_Position,
			&node->rt.target.activation->position,
			sizeof(struct spa_io_position));

	pw_log_debug("remote-node %p: fds:%d %d node:%u activation:%p",
		proxy, readfd, writefd, data->remote_id, data->activation->ptr);

	data->rtwritefd = writefd;
	spa_system_close(node->rt.target.system, node->source.fd);
	node->rt.target.fd = node->source.fd = readfd;

	data->have_transport = true;

	if (node->active)
		pw_client_node_set_active(data->client_node, true);

	return 0;
}

static int add_node_update(struct node_data *data, uint32_t change_mask, uint32_t info_mask)
{
	struct pw_impl_node *node = data->node;
	struct spa_node_info ni = SPA_NODE_INFO_INIT();
	uint32_t n_params = 0;
	struct spa_pod **params = NULL;
	int res;

	if (change_mask & PW_CLIENT_NODE_UPDATE_PARAMS) {
		uint32_t i, idx, id;
		uint8_t buf[4096];
		struct spa_pod_dynamic_builder b;

		for (i = 0; i < node->info.n_params; i++) {
			struct spa_pod *param;

			id = node->info.params[i].id;
			if (id == SPA_PARAM_Invalid)
				continue;

			for (idx = 0;;) {
				spa_pod_dynamic_builder_init(&b, buf, sizeof(buf), 4096);

	                        res = spa_node_enum_params_sync(node->node,
							id, &idx, NULL, &param, &b.b);
				if (res == 1) {
					void *p;
					p = pw_reallocarray(params, n_params + 1, sizeof(struct spa_pod *));
					if (p == NULL) {
						res = -errno;
						pw_log_error("realloc failed: %m");
					} else {
						params = p;
						params[n_params++] = spa_pod_copy(param);
					}
				}
				spa_pod_dynamic_builder_clean(&b);
				if (res != 1)
	                                break;
			}
                }
	}
	if (change_mask & PW_CLIENT_NODE_UPDATE_INFO) {
		ni.max_input_ports = node->info.max_input_ports;
		ni.max_output_ports = node->info.max_output_ports;
		ni.change_mask = info_mask;
		ni.flags = node->spa_flags;
		ni.props = node->info.props;
		ni.params = node->info.params;
		ni.n_params = node->info.n_params;
	}

        res = pw_client_node_update(data->client_node,
				change_mask,
				n_params,
				(const struct spa_pod **)params,
				&ni);

	if (params) {
		while (n_params > 0)
			free(params[--n_params]);
		free(params);
	}
	return res;
}

static int add_port_update(struct node_data *data, struct pw_impl_port *port, uint32_t change_mask)
{
	struct spa_port_info pi = SPA_PORT_INFO_INIT();
	uint32_t n_params = 0;
	struct spa_pod **params = NULL;
	int res;

	if (change_mask & PW_CLIENT_NODE_PORT_UPDATE_PARAMS) {
		uint32_t i, idx, id;
		uint8_t buf[4096];
		struct spa_pod_dynamic_builder b;

		for (i = 0; i < port->info.n_params; i++) {
			struct spa_pod *param;

			id = port->info.params[i].id;
			if (id == SPA_PARAM_Invalid)
				continue;

			for (idx = 0;;) {
				struct spa_node *qnode;
				uint32_t qport;

				spa_pod_dynamic_builder_init(&b, buf, sizeof(buf), 4096);

				switch (id) {
				case SPA_PARAM_IO:
					qnode = port->mix;
					qport = SPA_ID_INVALID;
					break;
				default:
					qnode = port->node->node;
					qport = port->port_id;
					break;
				}
				res = spa_node_port_enum_params_sync(qnode,
						port->direction, qport,
						id, &idx, NULL, &param, &b.b);

				if (res == 1) {
					void *p;
					p = pw_reallocarray(params, n_params + 1, sizeof(struct spa_pod*));
					if (p == NULL) {
						res = -errno;
						pw_log_error("realloc failed: %m");
					} else {
						params = p;
						params[n_params++] = spa_pod_copy(param);
					}
				}
				spa_pod_dynamic_builder_clean(&b);

				if (res != 1)
	                                break;

			}
                }
	}
	if (change_mask & PW_CLIENT_NODE_PORT_UPDATE_INFO) {
		pi.change_mask = SPA_PORT_CHANGE_MASK_FLAGS |
			SPA_PORT_CHANGE_MASK_RATE |
			SPA_PORT_CHANGE_MASK_PROPS |
			SPA_PORT_CHANGE_MASK_PARAMS;
		pi.flags = port->spa_flags;
		pi.rate = SPA_FRACTION(0, 1);
		pi.props = &port->properties->dict;
		SPA_FLAG_CLEAR(pi.flags, SPA_PORT_FLAG_DYNAMIC_DATA);
		pi.n_params = port->info.n_params;
		pi.params = port->info.params;
	}

	res = pw_client_node_port_update(data->client_node,
                                         port->direction,
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
	return res;
}

static int
client_node_set_param(void *_data, uint32_t id, uint32_t flags,
		      const struct spa_pod *param)
{
	struct node_data *data = _data;
	struct pw_proxy *proxy = (struct pw_proxy*)data->client_node;
	int res;

	pw_log_debug("node %p: set_param %s:", proxy,
			spa_debug_type_find_name(spa_type_param, id));

	res = spa_node_set_param(data->node->node, id, flags, param);

	if (res < 0) {
		pw_log_error("node %p: set_param %s (%d) %p: %s", proxy,
				spa_debug_type_find_name(spa_type_param, id),
				id, param, spa_strerror(res));
		pw_proxy_errorf(proxy, res, "node_set_param(%s) failed: %s",
				spa_debug_type_find_name(spa_type_param, id),
				spa_strerror(res));
	}
	return res;
}

static int
client_node_set_io(void *_data,
		   uint32_t id,
		   uint32_t memid,
		   uint32_t offset,
		   uint32_t size)
{
	struct node_data *data = _data;
	struct pw_impl_node *node = data->node;
	struct pw_proxy *proxy = (struct pw_proxy*)data->client_node;
	struct pw_memmap *old, *mm;
	void *ptr;
	uint32_t tag[5] = { data->remote_id, SPA_ID_INVALID, id, };
	int res;

	old = pw_mempool_find_tag(data->pool, tag, sizeof(tag));

	if (memid == SPA_ID_INVALID) {
		mm = ptr = NULL;
		size = 0;
	} else {
		mm = pw_mempool_map_id(data->pool, memid,
				PW_MEMMAP_FLAG_READWRITE, offset, size, tag);
		if (mm == NULL) {
			pw_log_warn("can't map memory id %u: %m", memid);
			res = -errno;
			goto exit;
		}
		ptr = mm->ptr;
	}

	pw_log_debug("node %p: set io %s %p", proxy,
			spa_debug_type_find_name(spa_type_io, id), ptr);

	res =  pw_impl_node_set_io(node, id, ptr, size);

	pw_memmap_free(old);
exit:
	if (res < 0) {
		pw_log_error("node %p: set_io: %s", proxy, spa_strerror(res));
		pw_proxy_errorf(proxy, res, "node_set_io failed: %s", spa_strerror(res));
	}
	return res;
}

static int client_node_event(void *data, const struct spa_event *event)
{
	uint32_t id = SPA_NODE_EVENT_ID(event);
	pw_log_warn("unhandled node event %d (%s)", id,
		    spa_debug_type_find_name(spa_type_node_event_id, id));
	return -ENOTSUP;
}

static int client_node_command(void *_data, const struct spa_command *command)
{
	struct node_data *data = _data;
	struct pw_impl_node *node = data->node;
	struct pw_proxy *proxy = (struct pw_proxy*)data->client_node;
	int res;
	uint32_t id = SPA_NODE_COMMAND_ID(command);

	pw_log_debug("%p: got command %d (%s)", proxy, id,
		    spa_debug_type_find_name(spa_type_node_command_id, id));

	switch (id) {
	case SPA_NODE_COMMAND_Pause:
		if ((res = pw_impl_node_set_state(node, PW_NODE_STATE_IDLE)) < 0) {
			pw_log_warn("node %p: pause failed", proxy);
			pw_proxy_error(proxy, res, "pause failed");
		}

		break;
	case SPA_NODE_COMMAND_Start:
		if ((res = pw_impl_node_set_state(node, PW_NODE_STATE_RUNNING)) < 0) {
			pw_log_warn("node %p: start failed", proxy);
			pw_proxy_error(proxy, res, "start failed");
		}
		break;

	case SPA_NODE_COMMAND_Suspend:
		if ((res = pw_impl_node_set_state(node, PW_NODE_STATE_SUSPENDED)) < 0) {
			pw_log_warn("node %p: suspend failed", proxy);
			pw_proxy_error(proxy, res, "suspend failed");
		}
		break;
	case SPA_NODE_COMMAND_RequestProcess:
		res = pw_impl_node_send_command(node, command);
		break;
	default:
		pw_log_warn("unhandled node command %d (%s)", id,
				spa_debug_type_find_name(spa_type_node_command_id, id));
		res = -ENOTSUP;
		pw_proxy_errorf(proxy, res, "command %d (%s) not supported", id,
				spa_debug_type_find_name(spa_type_node_command_id, id));
	}
	return res;
}

static int
client_node_add_port(void *_data, enum spa_direction direction, uint32_t port_id,
		const struct spa_dict *props)
{
	struct node_data *data = _data;
	struct pw_proxy *proxy = (struct pw_proxy*)data->client_node;
	pw_log_warn("add port not supported");
	pw_proxy_error(proxy, -ENOTSUP, "add port not supported");
	return -ENOTSUP;
}

static int
client_node_remove_port(void *_data, enum spa_direction direction, uint32_t port_id)
{
	struct node_data *data = _data;
	struct pw_proxy *proxy = (struct pw_proxy*)data->client_node;
	pw_log_warn("remove port not supported");
	pw_proxy_error(proxy, -ENOTSUP, "remove port not supported");
	return -ENOTSUP;
}

static int clear_buffers(struct node_data *data, struct mix *mix)
{
	struct pw_impl_port *port = mix->port;
        struct buffer *b;
	int res;

        pw_log_debug("port %p: clear %zd buffers mix:%d", port,
			pw_array_get_len(&mix->buffers, struct buffer),
			mix->mix.id);

	if ((res = pw_impl_port_use_buffers(port, &mix->mix, 0, NULL, 0)) < 0) {
		pw_log_error("port %p: error clear buffers %s", port, spa_strerror(res));
		return res;
	}

        pw_array_for_each(b, &mix->buffers) {
		pw_log_debug("port %p: clear buffer %d map %p %p",
			port, b->id, b->mem, b->buf);
		pw_memmap_free(b->mem);
		free(b->buf);
        }
	mix->buffers.size = 0;
	return 0;
}

static int
client_node_port_set_param(void *_data,
			   enum spa_direction direction, uint32_t port_id,
			   uint32_t id, uint32_t flags,
			   const struct spa_pod *param)
{
	struct node_data *data = _data;
	struct pw_proxy *proxy = (struct pw_proxy*)data->client_node;
	struct pw_impl_port *port;
	int res;

	port = pw_impl_node_find_port(data->node, direction, port_id);
	if (port == NULL) {
		res = -EINVAL;
		goto error_exit;
	}

	pw_log_debug("port %p: set_param %s %p", port,
			spa_debug_type_find_name(spa_type_param, id), param);

	res = pw_impl_port_set_param(port, id, flags, param);
	if (res < 0)
		goto error_exit;

	if (id == SPA_PARAM_Format) {
		struct mix *mix;
		spa_list_for_each(mix, &data->mix[direction], link) {
			if (mix->port->port_id == port_id)
				clear_buffers(data, mix);
		}
	}
	return res;

error_exit:
	pw_log_error("port %p: set_param %d %p: %s", port, id, param, spa_strerror(res));
	pw_proxy_errorf(proxy, res, "port_set_param(%s) failed: %s",
				spa_debug_type_find_name(spa_type_param, id),
				spa_strerror(res));
	return res;
}

static int
client_node_port_use_buffers(void *_data,
			     enum spa_direction direction, uint32_t port_id, uint32_t mix_id,
			     uint32_t flags,
			     uint32_t n_buffers, struct pw_client_node_buffer *buffers)
{
	struct node_data *data = _data;
	struct pw_proxy *proxy = (struct pw_proxy*)data->client_node;
	struct buffer *bid;
	uint32_t i, j;
	struct spa_buffer *b, **bufs;
	struct mix *mix;
	int res, prot;

	mix = find_mix(data, direction, port_id, mix_id);
	if (mix == NULL) {
		res = -ENOENT;
		goto error_exit;
	}

	if (n_buffers > MAX_BUFFERS)
		return -ENOSPC;

	prot = PW_MEMMAP_FLAG_READWRITE;

	/* clear previous buffers */
	clear_buffers(data, mix);

	bufs = alloca(n_buffers * sizeof(struct spa_buffer *));

	for (i = 0; i < n_buffers; i++) {
		size_t size;
		off_t offset;
		struct pw_memmap *mm;

		mm = pw_mempool_map_id(data->pool, buffers[i].mem_id,
				prot, buffers[i].offset, buffers[i].size, NULL);
		if (mm == NULL) {
			res = -errno;
			goto error_exit_cleanup;
		}

		bid = pw_array_add(&mix->buffers, sizeof(struct buffer));
		if (bid == NULL) {
			res = -errno;
			goto error_exit_cleanup;
		}
		bid->id = i;
		bid->mem = mm;

		if (data->allow_mlock && mlock(mm->ptr, mm->size) < 0)
			if (errno != ENOMEM || !mlock_warned) {
				pw_log(data->warn_mlock ? SPA_LOG_LEVEL_WARN : SPA_LOG_LEVEL_DEBUG,
						"Failed to mlock memory %p %u: %s",
						mm->ptr, mm->size,
						errno == ENOMEM ?
						"This is not a problem but for best performance, "
						"consider increasing RLIMIT_MEMLOCK" : strerror(errno));
				mlock_warned |= errno == ENOMEM;
			}

		size = sizeof(struct spa_buffer);
		for (j = 0; j < buffers[i].buffer->n_metas; j++)
			size += sizeof(struct spa_meta);
		for (j = 0; j < buffers[i].buffer->n_datas; j++)
			size += sizeof(struct spa_data);

		b = bid->buf = malloc(size);
		if (b == NULL) {
			res = -errno;
			goto error_exit_cleanup;
		}
		memcpy(b, buffers[i].buffer, sizeof(struct spa_buffer));

		b->metas = SPA_PTROFF(b, sizeof(struct spa_buffer), struct spa_meta);
		b->datas = SPA_PTROFF(b->metas, sizeof(struct spa_meta) * b->n_metas,
				       struct spa_data);

		pw_log_debug("add buffer mem:%d id:%d offset:%u size:%u %p", mm->block->id,
				bid->id, buffers[i].offset, buffers[i].size, bid->buf);

		offset = 0;
		for (j = 0; j < b->n_metas; j++) {
			struct spa_meta *m = &b->metas[j];
			memcpy(m, &buffers[i].buffer->metas[j], sizeof(struct spa_meta));
			m->data = SPA_PTROFF(mm->ptr, offset, void);
			offset += SPA_ROUND_UP_N(m->size, 8);
		}

		for (j = 0; j < b->n_datas; j++) {
			struct spa_data *d = &b->datas[j];

			memcpy(d, &buffers[i].buffer->datas[j], sizeof(struct spa_data));
			d->chunk =
			    SPA_PTROFF(mm->ptr, offset + sizeof(struct spa_chunk) * j,
				       struct spa_chunk);

			if (flags & SPA_NODE_BUFFERS_FLAG_ALLOC)
				continue;

			if (d->type == SPA_DATA_MemId) {
				uint32_t mem_id = SPA_PTR_TO_UINT32(d->data);
				struct pw_memblock *bm;

				bm = pw_mempool_find_id(data->pool, mem_id);
				if (bm == NULL) {
					pw_log_error("unknown buffer mem %u", mem_id);
					res = -ENODEV;
					goto error_exit_cleanup;
				}

				d->fd = bm->fd;
				d->type = bm->type;
				d->data = NULL;

				pw_log_debug(" data %d %u -> fd %d maxsize %d flags:%08x",
						j, bm->id, bm->fd, d->maxsize, d->flags);
			} else if (d->type == SPA_DATA_MemPtr) {
				int offs = SPA_PTR_TO_INT(d->data);
				d->data = SPA_PTROFF(mm->ptr, offs, void);
				d->fd = -1;
				pw_log_debug(" data %d id:%u -> mem:%p offs:%d maxsize:%d flags:%08x",
						j, bid->id, d->data, offs, d->maxsize, d->flags);
			} else {
				pw_log_warn("unknown buffer data type %d", d->type);
			}
		}
		bufs[i] = b;
	}

	if ((res = pw_impl_port_use_buffers(mix->port, &mix->mix, flags, bufs, n_buffers)) < 0)
		goto error_exit_cleanup;

	if (flags & SPA_NODE_BUFFERS_FLAG_ALLOC) {
		pw_client_node_port_buffers(data->client_node,
				direction, port_id, mix_id,
				n_buffers,
				bufs);
	}
	return res;

error_exit_cleanup:
	clear_buffers(data, mix);
error_exit:
	pw_log_error("port %p: use_buffers(%u:%u:%d): %d %s", mix,
			direction, port_id, mix_id, res, spa_strerror(res));
	pw_proxy_errorf(proxy, res, "port_use_buffers(%u:%u:%d) error: %s",
			direction, port_id, mix_id, spa_strerror(res));
	return res;
}

static int
client_node_port_set_io(void *_data,
                        uint32_t direction,
                        uint32_t port_id,
                        uint32_t mix_id,
                        uint32_t id,
                        uint32_t memid,
                        uint32_t offset,
                        uint32_t size)
{
	struct node_data *data = _data;
	struct pw_proxy *proxy = (struct pw_proxy*)data->client_node;
	struct mix *mix;
	struct pw_memmap *mm, *old;
	void *ptr;
	int res = 0;
	uint32_t tag[5] = { data->remote_id, direction, port_id, mix_id, id };

	mix = find_mix(data, direction, port_id, mix_id);
	if (mix == NULL) {
		res = -ENOENT;
		goto exit;
	}

	old = pw_mempool_find_tag(data->pool, tag, sizeof(tag));

	if (memid == SPA_ID_INVALID) {
		mm = ptr = NULL;
		size = 0;
	}
	else {
		mm = pw_mempool_map_id(data->pool, memid,
				PW_MEMMAP_FLAG_READWRITE, offset, size, tag);
		if (mm == NULL) {
			pw_log_warn("can't map memory id %u: %m", memid);
			res = -errno;
			goto exit;
		}
		ptr = mm->ptr;
	}

	pw_log_debug("port %p: set io:%s new:%p old:%p", mix->port,
			spa_debug_type_find_name(spa_type_io, id), ptr, mix->mix.io);

	if ((res = spa_node_port_set_io(mix->port->mix,
			     direction, mix->mix.port.port_id, id, ptr, size)) < 0) {
		if (res == -ENOTSUP)
			res = 0;
		else
			goto exit_free;
	}
exit_free:
	pw_memmap_free(old);
exit:
	if (res < 0) {
		pw_log_error("port %p: set_io: %s", mix, spa_strerror(res));
		pw_proxy_errorf(proxy, res, "port_set_io failed: %s", spa_strerror(res));
	}
	return res;
}

static int
client_node_set_activation(void *_data,
                        uint32_t node_id,
                        int signalfd,
                        uint32_t memid,
                        uint32_t offset,
                        uint32_t size)
{
	struct node_data *data = _data;
	struct pw_proxy *proxy = (struct pw_proxy*)data->client_node;
	struct pw_impl_node *node = data->node;
	struct pw_memmap *mm;
	void *ptr;
	struct link *link;
	int res = 0;

	if (memid == SPA_ID_INVALID) {
		mm = ptr = NULL;
		size = 0;
	} else {
		mm = pw_mempool_map_id(data->pool, memid,
				PW_MEMMAP_FLAG_READWRITE, offset, size, NULL);
		if (mm == NULL) {
			res = -errno;
			goto error_exit;
		}
		ptr = mm->ptr;
	}
	if (data->remote_id == node_id) {
		pw_log_debug("node %p: our activation %u: %u %p %u %u", node, node_id,
				memid, ptr, offset, size);
	} else {
		pw_log_debug("node %p: set activation %u: %u %p %u %u", node, node_id,
				memid, ptr, offset, size);
	}

	if (ptr) {
		link = calloc(1, sizeof(struct link));
		if (link == NULL) {
			res = -errno;
			goto error_exit;
		}
		link->data = data;
		link->map = mm;
		link->target.id = node_id;
		link->target.activation = ptr;
		link->target.system = data->data_system;
		link->target.fd = signalfd;
		spa_list_append(&data->links, &link->link);

		pw_impl_node_add_target(node, &link->target);

		pw_log_info("node %p: add link %p: memid:%u fd:%d id:%u state:%p pending:%d/%d",
				node, link, memid, signalfd, node_id,
				&link->target.activation->state[0],
				link->target.activation->state[0].pending,
				link->target.activation->state[0].required);
	} else {
		link = find_activation(&data->links, node_id);
		if (link == NULL) {
			res = -ENOENT;
			goto error_exit;
		}
		pw_log_info("node %p: remove link %p: id:%u state:%p pending:%d/%d",
				node, link, node_id,
				&link->target.activation->state[0],
				link->target.activation->state[0].pending,
				link->target.activation->state[0].required);
		clear_link(data, link);
	}
	return res;

error_exit:
	pw_log_error("node %p: set activation %d: %s", node, node_id, spa_strerror(res));
	pw_proxy_errorf(proxy, res, "set_activation: %s", spa_strerror(res));
	return res;
}

static void clear_mix(struct node_data *data, struct mix *mix)
{
	pw_log_debug("port %p: mix clear %d.%d", mix->port, mix->port->port_id, mix->mix.id);

	if (mix->mix.id != SPA_ID_INVALID)
		spa_node_port_set_io(mix->port->mix, mix->mix.port.direction,
				mix->mix.port.port_id, SPA_IO_Buffers, NULL, 0);

	spa_list_remove(&mix->link);

	clear_buffers(data, mix);
	pw_array_clear(&mix->buffers);

	spa_list_append(&data->free_mix, &mix->link);
	if (mix->mix.id != SPA_ID_INVALID)
		pw_impl_port_release_mix(mix->port, &mix->mix);
}

static int client_node_port_set_mix_info(void *_data,
		enum spa_direction direction, uint32_t port_id,
		uint32_t mix_id, uint32_t peer_id, const struct spa_dict *props)
{
	struct node_data *data = _data;
	struct mix *mix;

	pw_log_debug("%p: %d:%d:%d peer:%d", data, direction, port_id, mix_id, peer_id);

	mix = find_mix(data, direction, port_id, mix_id);

	if (peer_id == SPA_ID_INVALID) {
		if (mix == NULL)
			return -EINVAL;
		clear_mix(data, mix);
	} else {
		struct pw_impl_port *port;
		if (mix != NULL)
			return -EEXIST;
		port = pw_impl_node_find_port(data->node, direction, port_id);
		if (port == NULL)
			return -ENOENT;
		mix = create_mix(data, port, mix_id, peer_id);
		if (mix == NULL)
			return -errno;
	}
	return 0;
}

static const struct pw_client_node_events client_node_events = {
	PW_VERSION_CLIENT_NODE_EVENTS,
	.transport = client_node_transport,
	.set_param = client_node_set_param,
	.set_io = client_node_set_io,
	.event = client_node_event,
	.command = client_node_command,
	.add_port = client_node_add_port,
	.remove_port = client_node_remove_port,
	.port_set_param = client_node_port_set_param,
	.port_use_buffers = client_node_port_use_buffers,
	.port_set_io = client_node_port_set_io,
	.set_activation = client_node_set_activation,
	.port_set_mix_info = client_node_port_set_mix_info,
};

static void do_node_init(struct node_data *data)
{
	struct pw_impl_node *node = data->node;
	struct pw_impl_port *port;
	struct mix *mix;

	pw_log_debug("%p: node %p init", data, node);
	add_node_update(data, PW_CLIENT_NODE_UPDATE_PARAMS |
				PW_CLIENT_NODE_UPDATE_INFO,
				SPA_NODE_CHANGE_MASK_FLAGS |
				SPA_NODE_CHANGE_MASK_PROPS |
				SPA_NODE_CHANGE_MASK_PARAMS);

	spa_list_for_each(port, &node->input_ports, link) {
		mix = create_mix(data, port, SPA_ID_INVALID, SPA_ID_INVALID);
		if (mix == NULL)
			pw_log_error("%p: failed to create port mix: %m", node);
		add_port_update(data, port,
				PW_CLIENT_NODE_PORT_UPDATE_PARAMS |
				PW_CLIENT_NODE_PORT_UPDATE_INFO);
	}
	spa_list_for_each(port, &node->output_ports, link) {
		mix = create_mix(data, port, SPA_ID_INVALID, SPA_ID_INVALID);
		if (mix == NULL)
			pw_log_error("%p: failed to create port mix: %m", node);
		add_port_update(data, port,
				PW_CLIENT_NODE_PORT_UPDATE_PARAMS |
				PW_CLIENT_NODE_PORT_UPDATE_INFO);
	}
}

static void clean_node(struct node_data *d)
{
	struct mix *mix;

	if (d->have_transport) {
		spa_list_consume(mix, &d->mix[SPA_DIRECTION_INPUT], link)
			clear_mix(d, mix);
		spa_list_consume(mix, &d->mix[SPA_DIRECTION_OUTPUT], link)
			clear_mix(d, mix);
	}
	spa_list_consume(mix, &d->free_mix, link) {
		spa_list_remove(&mix->link);
		free(mix);
	}
	clean_transport(d);
}

static void node_destroy(void *data)
{
	struct node_data *d = data;

	pw_log_debug("%p: destroy", d);

	clean_node(d);
}

static void node_free(void *data)
{
	struct node_data *d = data;
	pw_log_debug("%p: free", d);
	d->node = NULL;
}

static void node_info_changed(void *data, const struct pw_node_info *info)
{
	struct node_data *d = data;
	uint32_t change_mask, info_mask;

	pw_log_debug("info changed %p", d);

	if (d->client_node == NULL)
		return;

	change_mask = PW_CLIENT_NODE_UPDATE_INFO;
	info_mask = SPA_NODE_CHANGE_MASK_FLAGS;
	if (info->change_mask & PW_NODE_CHANGE_MASK_PROPS) {
		info_mask |= SPA_NODE_CHANGE_MASK_PROPS;
	}
	if (info->change_mask & PW_NODE_CHANGE_MASK_PARAMS) {
		change_mask |= PW_CLIENT_NODE_UPDATE_PARAMS;
		info_mask |= SPA_NODE_CHANGE_MASK_PARAMS;
	}
	add_node_update(d, change_mask, info_mask);
}

static void node_port_info_changed(void *data, struct pw_impl_port *port,
		const struct pw_port_info *info)
{
	struct node_data *d = data;
	uint32_t change_mask = 0;

	pw_log_debug("info changed %p", d);

	if (d->client_node == NULL)
		return;

	if (info->change_mask & PW_PORT_CHANGE_MASK_PROPS)
		change_mask |= PW_CLIENT_NODE_PORT_UPDATE_INFO;
	if (info->change_mask & PW_PORT_CHANGE_MASK_PARAMS) {
		change_mask |= PW_CLIENT_NODE_PORT_UPDATE_PARAMS;
		change_mask |= PW_CLIENT_NODE_PORT_UPDATE_INFO;
	}
	add_port_update(d, port, change_mask);
}

static void node_port_added(void *data, struct pw_impl_port *port)
{
	struct node_data *d = data;
	struct mix *mix;

	pw_log_debug("added %p", d);

	if (d->client_node == NULL)
		return;

	mix = create_mix(d, port, SPA_ID_INVALID, SPA_ID_INVALID);
	if (mix == NULL)
		pw_log_error("%p: failed to create port mix: %m", d->node);
}

static void node_port_removed(void *data, struct pw_impl_port *port)
{
	struct node_data *d = data;
	struct mix *mix, *tmp;

	pw_log_debug("removed %p", d);

	if (d->client_node == NULL)
		return;

	pw_client_node_port_update(d->client_node,
			port->direction,
			port->port_id,
			0, 0, NULL, NULL);

	spa_list_for_each_safe(mix, tmp, &d->mix[port->direction], link) {
		if (mix->port == port)
			clear_mix(d, mix);
	}
}

static void node_active_changed(void *data, bool active)
{
	struct node_data *d = data;
	pw_log_debug("active %d", active);

	if (d->client_node == NULL)
		return;

	pw_client_node_set_active(d->client_node, active);
}

static void node_event(void *data, const struct spa_event *event)
{
	struct node_data *d = data;
	pw_log_debug("%p", d);

	if (d->client_node == NULL)
		return;
	pw_client_node_event(d->client_node, event);
}

static const struct pw_impl_node_events node_events = {
	PW_VERSION_IMPL_NODE_EVENTS,
	.destroy = node_destroy,
	.free = node_free,
	.info_changed = node_info_changed,
	.port_info_changed = node_port_info_changed,
	.port_added = node_port_added,
	.port_removed = node_port_removed,
	.active_changed = node_active_changed,
	.event = node_event,
};

static void client_node_removed(void *_data)
{
	struct node_data *data = _data;
	struct pw_impl_node *node = data->node;
	pw_log_debug("%p: removed", data);

	spa_hook_remove(&data->proxy_client_node_listener);
	spa_hook_remove(&data->client_node_listener);

	if (node) {
		spa_hook_remove(&data->node_listener);
		pw_impl_node_remove_rt_listener(node,
				&data->node_rt_listener);
		pw_impl_node_set_state(node, PW_NODE_STATE_SUSPENDED);

		clean_node(data);

		if (data->do_free)
			pw_impl_node_destroy(node);
	}
	data->client_node = NULL;
}

static void client_node_destroy(void *_data)
{
	struct node_data *data = _data;

	pw_log_debug("%p: destroy", data);
	client_node_removed(_data);
}

static void client_node_bound_props(void *_data, uint32_t global_id, const struct spa_dict *props)
{
	struct node_data *data = _data;
	pw_log_debug("%p: bound %u", data, global_id);
	data->remote_id = global_id;
	if (props)
		pw_properties_update(data->node->properties, props);
}

static const struct pw_proxy_events proxy_client_node_events = {
	PW_VERSION_PROXY_EVENTS,
	.removed = client_node_removed,
	.destroy = client_node_destroy,
	.bound_props = client_node_bound_props,
};

static void node_rt_complete(void *data)
{
	struct node_data *d = data;
	struct pw_impl_node *node = d->node;
	struct spa_system *data_system = d->data_system;

	if (!node->driving ||
	    !SPA_FLAG_IS_SET(node->rt.target.activation->flags, PW_NODE_ACTIVATION_FLAG_PROFILER))
		return;

	if (SPA_UNLIKELY(spa_system_eventfd_write(data_system, d->rtwritefd, 1) < 0))
		pw_log_warn("node %p: write failed %m", node);
}

static const struct pw_impl_node_rt_events node_rt_events = {
	PW_VERSION_IMPL_NODE_RT_EVENTS,
	.complete = node_rt_complete,
};

static struct pw_proxy *node_export(struct pw_core *core, void *object, bool do_free,
		size_t user_data_size)
{
	struct pw_impl_node *node = object;
	struct pw_proxy *client_node;
	struct node_data *data;

	if (node->data_loop == NULL)
		goto error;

	user_data_size = SPA_ROUND_UP_N(user_data_size, __alignof__(struct node_data));

	client_node = pw_core_create_object(core,
			"client-node",
			PW_TYPE_INTERFACE_ClientNode,
			PW_VERSION_CLIENT_NODE,
			&node->properties->dict,
			user_data_size + sizeof(struct node_data));
	if (client_node == NULL)
		goto error;

	data = pw_proxy_get_user_data(client_node);
	data = SPA_PTROFF(data, user_data_size, struct node_data);
	data->pool = pw_core_get_mempool(core);
	data->node = node;
	data->do_free = do_free;
	data->context = pw_impl_node_get_context(node);
	data->data_loop = node->data_loop;
	data->data_system = data->data_loop->system;
	data->client_node = (struct pw_client_node *)client_node;
	data->remote_id = SPA_ID_INVALID;


	data->allow_mlock = pw_properties_get_bool(node->properties, "mem.allow-mlock",
						   data->context->settings.mem_allow_mlock);

	data->warn_mlock = pw_properties_get_bool(node->properties, "mem.warn-mlock",
						  data->context->settings.mem_warn_mlock);

	node->exported = true;

	spa_list_init(&data->free_mix);
	spa_list_init(&data->mix[0]);
	spa_list_init(&data->mix[1]);

	spa_list_init(&data->links);

	pw_proxy_add_listener(client_node,
			&data->proxy_client_node_listener,
			&proxy_client_node_events, data);

	pw_impl_node_add_listener(node, &data->node_listener, &node_events, data);
	pw_impl_node_add_rt_listener(node, &data->node_rt_listener,
			&node_rt_events, data);

	pw_client_node_add_listener(data->client_node,
					  &data->client_node_listener,
					  &client_node_events,
					  data);

	do_node_init(data);

	return client_node;
error:
	if (do_free)
		pw_impl_node_destroy(node);
	return NULL;

}

struct pw_proxy *pw_core_node_export(struct pw_core *core,
		const char *type, const struct spa_dict *props, void *object,
		size_t user_data_size)
{
	struct pw_impl_node *node = object;

	if (props)
		pw_impl_node_update_properties(node, props);
	return node_export(core, object, false, user_data_size);
}

struct pw_proxy *pw_core_spa_node_export(struct pw_core *core,
		const char *type, const struct spa_dict *props, void *object,
		size_t user_data_size)
{
	struct pw_impl_node *node;
	struct pw_proxy *proxy;
	const char *str;
	bool do_register;

	str = props ? spa_dict_lookup(props, PW_KEY_OBJECT_REGISTER) : NULL;
	do_register = str ? pw_properties_parse_bool(str) : true;

	node = pw_context_create_node(pw_core_get_context(core),
			props ? pw_properties_new_dict(props) : NULL, 0);
	if (node == NULL)
		return NULL;

	pw_impl_node_set_implementation(node, (struct spa_node*)object);

	if (do_register)
		pw_impl_node_register(node, NULL);

	proxy = node_export(core, node, true, user_data_size);
	if (proxy)
		pw_impl_node_set_active(node, true);

	return proxy;
}
