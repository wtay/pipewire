/* PipeWire
 * Copyright (C) 2016 Wim Taymans <wim.taymans@gmail.com>
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

#include <unistd.h>
#include <errno.h>
#include <sys/mman.h>

#include <spa/utils/ringbuffer.h>
#include <spa/node/io.h>
#include <pipewire/log.h>
#include <extensions/client-node.h>

#include "transport.h"

/** \cond */

#define INPUT_BUFFER_SIZE       (1<<12)
#define OUTPUT_BUFFER_SIZE      (1<<12)

struct transport {
	struct pw_client_node_transport trans;

	struct pw_memblock *mem;
	size_t offset;

	struct pw_client_node_message current;
	uint32_t current_index;
};
/** \endcond */

static size_t area_get_size(struct pw_client_node_area *area)
{
	size_t size;
	size = sizeof(struct pw_client_node_area);
	size += area->max_input_ports * sizeof(struct spa_io_buffers);
	size += area->max_output_ports * sizeof(struct spa_io_buffers);
	size += sizeof(struct spa_ringbuffer);
	size += INPUT_BUFFER_SIZE;
	size += sizeof(struct spa_ringbuffer);
	size += OUTPUT_BUFFER_SIZE;
	return size;
}

static void transport_setup_area(void *p, struct pw_client_node_transport *trans)
{
	struct pw_client_node_area *a;

	trans->area = a = p;
	p = SPA_MEMBER(p, sizeof(struct pw_client_node_area), struct spa_io_buffers);

	trans->inputs = p;
	p = SPA_MEMBER(p, a->max_input_ports * sizeof(struct spa_io_buffers), void);

	trans->outputs = p;
	p = SPA_MEMBER(p, a->max_output_ports * sizeof(struct spa_io_buffers), void);

	trans->input_buffer = p;
	p = SPA_MEMBER(p, sizeof(struct spa_ringbuffer), void);

	trans->input_data = p;
	p = SPA_MEMBER(p, INPUT_BUFFER_SIZE, void);

	trans->output_buffer = p;
	p = SPA_MEMBER(p, sizeof(struct spa_ringbuffer), void);

	trans->output_data = p;
	p = SPA_MEMBER(p, OUTPUT_BUFFER_SIZE, void);
}

static void transport_reset_area(struct pw_client_node_transport *trans)
{
	int i;
	struct pw_client_node_area *a = trans->area;

	for (i = 0; i < a->max_input_ports; i++) {
		trans->inputs[i].status = SPA_STATUS_OK;
		trans->inputs[i].buffer_id = SPA_ID_INVALID;
	}
	for (i = 0; i < a->max_output_ports; i++) {
		trans->outputs[i].status = SPA_STATUS_OK;
		trans->outputs[i].buffer_id = SPA_ID_INVALID;
	}
	spa_ringbuffer_init(trans->input_buffer);
	spa_ringbuffer_init(trans->output_buffer);
}

static void destroy(struct pw_client_node_transport *trans)
{
	struct transport *impl = (struct transport *) trans;

	pw_log_debug("transport %p: destroy", trans);

	pw_memblock_free(impl->mem);
	free(impl);
}

static int add_message(struct pw_client_node_transport *trans, struct pw_client_node_message *message)
{
	struct transport *impl = (struct transport *) trans;
	int32_t filled, avail;
	uint32_t size, index;

	if (impl == NULL || message == NULL)
		return -EINVAL;

	filled = spa_ringbuffer_get_write_index(trans->output_buffer, &index);
	avail = OUTPUT_BUFFER_SIZE - filled;
	size = SPA_POD_SIZE(message);
	if (avail < size)
		return -ENOSPC;

	spa_ringbuffer_write_data(trans->output_buffer,
				  trans->output_data, OUTPUT_BUFFER_SIZE,
				  index & (OUTPUT_BUFFER_SIZE - 1), message, size);
	spa_ringbuffer_write_update(trans->output_buffer, index + size);

	return 0;
}

static int next_message(struct pw_client_node_transport *trans, struct pw_client_node_message *message)
{
	struct transport *impl = (struct transport *) trans;
	int32_t avail;

	if (impl == NULL || message == NULL)
		return -EINVAL;

	avail = spa_ringbuffer_get_read_index(trans->input_buffer, &impl->current_index);
	if (avail < sizeof(struct pw_client_node_message))
		return 0;

	spa_ringbuffer_read_data(trans->input_buffer,
				 trans->input_data, INPUT_BUFFER_SIZE,
				 impl->current_index & (INPUT_BUFFER_SIZE - 1),
				 &impl->current, sizeof(struct pw_client_node_message));

	if (avail < SPA_POD_SIZE(&impl->current))
		return 0;

	*message = impl->current;

	return 1;
}

static int parse_message(struct pw_client_node_transport *trans, void *message)
{
	struct transport *impl = (struct transport *) trans;
	uint32_t size;

	if (impl == NULL || message == NULL)
		return -EINVAL;

	size = SPA_POD_SIZE(&impl->current);

	spa_ringbuffer_read_data(trans->input_buffer,
				 trans->input_data, INPUT_BUFFER_SIZE,
				 impl->current_index & (INPUT_BUFFER_SIZE - 1), message, size);
	spa_ringbuffer_read_update(trans->input_buffer, impl->current_index + size);

	return 0;
}

/** Create a new transport
 * \param max_input_ports maximum number of input_ports
 * \param max_output_ports maximum number of output_ports
 * \return a newly allocated \ref pw_client_node_transport
 * \memberof pw_client_node_transport
 */
struct pw_client_node_transport *
pw_client_node_transport_new(uint32_t max_input_ports, uint32_t max_output_ports)
{
	struct transport *impl;
	struct pw_client_node_transport *trans;
	struct pw_client_node_area area = { 0 };

	area.max_input_ports = max_input_ports;
	area.n_input_ports = 0;
	area.max_output_ports = max_output_ports;
	area.n_output_ports = 0;

	impl = calloc(1, sizeof(struct transport));
	if (impl == NULL)
		return NULL;

	pw_log_debug("transport %p: new %d %d", impl, max_input_ports, max_output_ports);

	trans = &impl->trans;
	impl->offset = 0;

	if (pw_memblock_alloc(PW_MEMBLOCK_FLAG_WITH_FD |
			  PW_MEMBLOCK_FLAG_MAP_READWRITE |
			  PW_MEMBLOCK_FLAG_SEAL,
			  area_get_size(&area),
			  &impl->mem) < 0) {
		free(impl);
		return NULL;
	}

	memcpy(impl->mem->ptr, &area, sizeof(struct pw_client_node_area));
	transport_setup_area(impl->mem->ptr, trans);
	transport_reset_area(trans);

	trans->destroy = destroy;
	trans->add_message = add_message;
	trans->next_message = next_message;
	trans->parse_message = parse_message;

	return trans;
}

struct pw_client_node_transport *
pw_client_node_transport_new_from_info(struct pw_client_node_transport_info *info)
{
	struct transport *impl;
	struct pw_client_node_transport *trans;
	void *tmp;
	int res;

	impl = calloc(1, sizeof(struct transport));
	if (impl == NULL)
		return NULL;

	trans = &impl->trans;
	pw_log_debug("transport %p: new from info", impl);

	if ((res = pw_memblock_import(PW_MEMBLOCK_FLAG_MAP_READWRITE |
				      PW_MEMBLOCK_FLAG_WITH_FD,
				      info->memfd,
				      info->offset,
				      info->size, &impl->mem)) < 0) {
		pw_log_warn("transport %p: failed to map fd %d: %s", impl, info->memfd,
			    spa_strerror(res));
		goto mmap_failed;
	}

	impl->offset = info->offset;

	transport_setup_area(impl->mem->ptr, trans);

	tmp = trans->output_buffer;
	trans->output_buffer = trans->input_buffer;
	trans->input_buffer = tmp;

	tmp = trans->output_data;
	trans->output_data = trans->input_data;
	trans->input_data = tmp;

	trans->destroy = destroy;
	trans->add_message = add_message;
	trans->next_message = next_message;
	trans->parse_message = parse_message;

	return trans;

      mmap_failed:
	free(impl);
	errno = -res;
	return NULL;
}

/** Get transport info
 * \param trans the transport to get info of
 * \param[out] info transport info
 * \return 0 on success
 *
 * Fill \a info with the transport info of \a trans. This information can be
 * passed to the client to set up the shared transport.
 *
 * \memberof pw_client_node_transport
 */
int pw_client_node_transport_get_info(struct pw_client_node_transport *trans,
				      struct pw_client_node_transport_info *info)
{
	struct transport *impl = (struct transport *) trans;

	info->memfd = impl->mem->fd;
	info->offset = impl->offset;
	info->size = impl->mem->size;

	return 0;
}
