/* PipeWire */
/* SPDX-FileCopyrightText: Copyright © 2018 Wim Taymans */
/* SPDX-License-Identifier: MIT */

#include <errno.h>
#include <string.h>
#include <stdio.h>
#include <time.h>

#include <spa/node/utils.h>
#include <spa/pod/parser.h>
#include <spa/pod/compare.h>
#include <spa/param/param.h>
#include <spa/debug/types.h>

#include "pipewire/impl-link.h"
#include "pipewire/private.h"

PW_LOG_TOPIC_EXTERN(log_link);
#define PW_LOG_TOPIC_DEFAULT log_link

#define MAX_HOPS	32

#define pw_link_resource_info(r,...)      pw_resource_call(r,struct pw_link_events,info,0,__VA_ARGS__)

/** \cond */
struct impl {
	struct pw_impl_link this;

	unsigned int activated:1;

	struct pw_work_queue *work;

	uint32_t output_busy_id;
	uint32_t input_busy_id;

	struct spa_pod *format_filter;
	struct pw_properties *properties;

	struct spa_hook input_port_listener;
	struct spa_hook input_node_listener;
	struct spa_hook input_global_listener;
	struct spa_hook output_port_listener;
	struct spa_hook output_node_listener;
	struct spa_hook output_global_listener;

	struct spa_io_buffers io[2];

	struct pw_impl_node *inode, *onode;
	bool async;
};

/** \endcond */

static struct pw_node_peer *pw_node_peer_ref(struct pw_impl_node *onode, struct pw_impl_node *inode)
{
	struct pw_node_peer *peer;

	spa_list_for_each(peer, &onode->peer_list, link) {
		if (peer->target.id == inode->info.id) {
			pw_log_debug("exiting peer %p from %p to %p", peer, onode, inode);
			peer->ref++;
			return peer;
		}
	}
	peer = calloc(1, sizeof(*peer));
	if (peer == NULL)
		return NULL;

	peer->ref = 1;
	peer->output = onode;
	peer->active_count = 0;
	copy_target(&peer->target, &inode->rt.target);
	peer->target.flags = PW_NODE_TARGET_PEER;

	spa_list_append(&onode->peer_list, &peer->link);
	pw_log_debug("new peer %p from %p to %p", peer, onode, inode);
	pw_impl_node_add_target(onode, &peer->target);

	return peer;
}

static void pw_node_peer_unref(struct pw_node_peer *peer)
{
	if (--peer->ref > 0)
		return;
	spa_list_remove(&peer->link);
	pw_log_debug("remove peer %p from %p to %p", peer, peer->output, peer->target.node);
	pw_impl_node_remove_target(peer->output, &peer->target);
	free(peer);
}

static void pw_node_peer_activate(struct pw_node_peer *peer)
{
	struct pw_node_activation_state *state;

	state = &peer->target.activation->state[0];

	if (peer->active_count++ == 0) {
		spa_list_append(&peer->output->rt.target_list, &peer->target.link);

		if (!peer->target.active && peer->output->rt.driver_target.node != NULL) {
//			if (!peer->output->async)
//				SPA_ATOMIC_INC(state->required);
			peer->target.active = true;
		}
	}
	pw_log_trace("%p: node:%s state:%p pending:%d/%d", peer->output,
			peer->target.name, state, state->pending, state->required);
}

static void pw_node_peer_deactivate(struct pw_node_peer *peer)
{
	struct pw_node_activation_state *state;
	state = &peer->target.activation->state[0];
	if (--peer->active_count == 0) {
		spa_list_remove(&peer->target.link);

		if (peer->target.active) {
			if (!peer->output->async) {
//				SPA_ATOMIC_DEC(state->required);
//				trigger_target(&peer->target, get_time_ns(peer->target.system));
			}
			peer->target.active = false;
		}
	}
	pw_log_trace("%p: node:%s state:%p pending:%d/%d", peer->output,
			peer->target.name, state, state->pending, state->required);
}

static void info_changed(struct pw_impl_link *link)
{
	struct pw_resource *resource;

	if (link->info.change_mask == 0)
		return;

	pw_impl_link_emit_info_changed(link, &link->info);

	if (link->global)
		spa_list_for_each(resource, &link->global->resource_list, link)
			pw_link_resource_info(resource, &link->info);

	link->info.change_mask = 0;
}

static inline void input_set_busy_id(struct pw_impl_link *link, uint32_t id)
{
	struct impl *impl = SPA_CONTAINER_OF(link, struct impl, this);
	if (impl->input_busy_id != SPA_ID_INVALID)
		link->input->busy_count--;
	if (id != SPA_ID_INVALID)
		link->input->busy_count++;
	impl->input_busy_id = id;
	if (link->input->busy_count < 0)
		pw_log_error("%s: invalid busy count:%d", link->name, link->input->busy_count);
}

static inline void output_set_busy_id(struct pw_impl_link *link, uint32_t id)
{
	struct impl *impl = SPA_CONTAINER_OF(link, struct impl, this);
	if (impl->output_busy_id != SPA_ID_INVALID)
		link->output->busy_count--;
	if (id != SPA_ID_INVALID)
		link->output->busy_count++;
	impl->output_busy_id = id;
	if (link->output->busy_count < 0)
		pw_log_error("%s: invalid busy count:%d", link->name, link->output->busy_count);
}

static void link_update_state(struct pw_impl_link *link, enum pw_link_state state, int res, char *error)
{
	struct impl *impl = SPA_CONTAINER_OF(link, struct impl, this);
	enum pw_link_state old = link->info.state;

	link->info.state = state;
	free((char*)link->info.error);
	link->info.error = error;

	if (state == old)
		return;

	pw_log_debug("%p: %s -> %s (%s)", link,
		     pw_link_state_as_string(old),
		     pw_link_state_as_string(state), error);

	if (state == PW_LINK_STATE_ERROR) {
		pw_log_error("(%s) %s -> error (%s) (%s-%s)", link->name,
				pw_link_state_as_string(old), error,
				pw_impl_port_state_as_string(link->output->state),
				pw_impl_port_state_as_string(link->input->state));
	} else {
		pw_log_info("(%s) %s -> %s (%s-%s)", link->name,
				pw_link_state_as_string(old),
				pw_link_state_as_string(state),
				pw_impl_port_state_as_string(link->output->state),
				pw_impl_port_state_as_string(link->input->state));
	}

	pw_impl_link_emit_state_changed(link, old, state, error);

	link->info.change_mask |= PW_LINK_CHANGE_MASK_STATE;
	if (state == PW_LINK_STATE_ERROR ||
	    state == PW_LINK_STATE_PAUSED ||
	    state == PW_LINK_STATE_ACTIVE)
		info_changed(link);

	if (state == PW_LINK_STATE_ERROR && link->global) {
		struct pw_resource *resource;
		spa_list_for_each(resource, &link->global->resource_list, link)
			pw_resource_error(resource, res, error);
	}

	if (old < PW_LINK_STATE_PAUSED && state == PW_LINK_STATE_PAUSED) {
		link->prepared = true;
		link->preparing = false;
		pw_context_recalc_graph(link->context, "link prepared");
	} else if (old == PW_LINK_STATE_PAUSED && state < PW_LINK_STATE_PAUSED) {
		link->prepared = false;
		link->preparing = false;
		pw_context_recalc_graph(link->context, "link unprepared");
	} else if (state == PW_LINK_STATE_INIT) {
		link->prepared = false;
		link->preparing = false;

		output_set_busy_id(link, SPA_ID_INVALID);
		pw_work_queue_cancel(impl->work, &link->output_link, SPA_ID_INVALID);

		input_set_busy_id(link, SPA_ID_INVALID);
		pw_work_queue_cancel(impl->work, &link->input_link, SPA_ID_INVALID);
	}
}

static void complete_ready(void *obj, void *data, int res, uint32_t id)
{
	struct pw_impl_port *port;
	struct pw_impl_link *this = data;
	struct impl *impl = SPA_CONTAINER_OF(this, struct impl, this);

	if (obj == &this->input_link)
		port = this->input;
	else
		port = this->output;

	if (id != SPA_ID_INVALID) {
		if (id == impl->input_busy_id) {
			input_set_busy_id(this, SPA_ID_INVALID);
		} else if (id == impl->output_busy_id) {
			output_set_busy_id(this, SPA_ID_INVALID);
		} else {
			return;
		}
	}

	pw_log_debug("%p: obj:%p port %p complete state:%d: %s", this, obj, port,
			port->state, spa_strerror(res));

	if (SPA_RESULT_IS_OK(res)) {
		if (port->state < PW_IMPL_PORT_STATE_READY)
			pw_impl_port_update_state(port, PW_IMPL_PORT_STATE_READY,
					0, NULL);
	} else {
		pw_impl_port_update_state(port, PW_IMPL_PORT_STATE_ERROR,
				res, spa_aprintf("port error going to READY: %s", spa_strerror(res)));
	}
	if (this->input->state >= PW_IMPL_PORT_STATE_READY &&
	    this->output->state >= PW_IMPL_PORT_STATE_READY)
		link_update_state(this, PW_LINK_STATE_ALLOCATING, 0, NULL);
}

static void complete_paused(void *obj, void *data, int res, uint32_t id)
{
	struct pw_impl_port *port;
	struct pw_impl_link *this = data;
	struct impl *impl = SPA_CONTAINER_OF(this, struct impl, this);
	struct pw_impl_port_mix *mix;

	if (obj == &this->input_link) {
		port = this->input;
		mix = &this->rt.in_mix;
	} else {
		port = this->output;
		mix = &this->rt.out_mix;
	}

	if (id != SPA_ID_INVALID) {
		if (id == impl->input_busy_id) {
			input_set_busy_id(this, SPA_ID_INVALID);
		} else if (id == impl->output_busy_id) {
			output_set_busy_id(this, SPA_ID_INVALID);
		} else {
			return;
		}
	}

	pw_log_debug("%p: obj:%p port %p complete state:%d: %s", this, obj, port,
			port->state, spa_strerror(res));

	if (SPA_RESULT_IS_OK(res)) {
		if (port->state < PW_IMPL_PORT_STATE_PAUSED)
			pw_impl_port_update_state(port, PW_IMPL_PORT_STATE_PAUSED,
					0, NULL);
		mix->have_buffers = true;
	} else {
		pw_impl_port_update_state(port, PW_IMPL_PORT_STATE_ERROR,
				res, spa_aprintf("port error going to PAUSED: %s", spa_strerror(res)));
		mix->have_buffers = false;
	}
	if (this->rt.in_mix.have_buffers && this->rt.out_mix.have_buffers)
		link_update_state(this, PW_LINK_STATE_PAUSED, 0, NULL);
}

static void complete_sync(void *obj, void *data, int res, uint32_t id)
{
	struct pw_impl_port *port;
	struct pw_impl_link *this = data;

	if (obj == &this->input_link)
		port = this->input;
	else
		port = this->output;

	pw_log_debug("%p: obj:%p port %p complete state:%d: %s", this, obj, port,
			port->state, spa_strerror(res));
}

static int do_negotiate(struct pw_impl_link *this)
{
	struct pw_context *context = this->context;
	struct impl *impl = SPA_CONTAINER_OF(this, struct impl, this);
	int res = -EIO, res2;
	struct spa_pod *format = NULL, *current;
	char *error = NULL;
	bool changed = true;
	struct pw_impl_port *input, *output;
	uint8_t buffer[4096];
	struct spa_pod_builder b = SPA_POD_BUILDER_INIT(buffer, sizeof(buffer));
	uint32_t index, busy_id;
	uint32_t in_state, out_state;
	struct spa_node *in_node, *out_node;
	uint32_t in_port, out_port;

	if (this->info.state >= PW_LINK_STATE_NEGOTIATING)
		return 0;

	input = this->input;
	output = this->output;

	in_state = input->state;
	out_state = output->state;

	pw_log_debug("%p: in_state:%d out_state:%d", this, in_state, out_state);

	if (in_state != PW_IMPL_PORT_STATE_CONFIGURE && out_state != PW_IMPL_PORT_STATE_CONFIGURE)
		return 0;

	link_update_state(this, PW_LINK_STATE_NEGOTIATING, 0, NULL);

	input = this->input;
	output = this->output;
#if 0
	in_node = input->mix;
	in_port = this->rt.in_mix.port.port_id;
	out_node = output->mix;
	out_port = this->rt.out_mix.port.port_id;
#else
	in_node = input->node->node;
	in_port = input->port_id;
	out_node = output->node->node;
	out_port = output->port_id;
#endif

	/* find a common format for the ports */
	if ((res = pw_context_find_format(context,
					output, SPA_ID_INVALID,
					input, SPA_ID_INVALID,
					NULL, 0, NULL,
					&format, &b, &error)) < 0) {
		format = NULL;
		goto error;
	}

	format = spa_pod_copy(format);
	spa_pod_fixate(format);

	spa_pod_builder_init(&b, buffer, sizeof(buffer));

	/* if output port had format and is idle, check if it changed. If so, renegotiate */
	if (out_state > PW_IMPL_PORT_STATE_CONFIGURE && output->node->info.state == PW_NODE_STATE_IDLE) {
		index = 0;
		res = spa_node_port_enum_params_sync(out_node,
				output->direction, out_port,
				SPA_PARAM_Format, &index,
				NULL, &current, &b);
		switch (res) {
		case -EIO:
			current = NULL;
			res = 0;
			SPA_FALLTHROUGH
		case 1:
			break;
		case 0:
			res = -EBADF;
			SPA_FALLTHROUGH
		default:
			error = spa_aprintf("error get output format: %s", spa_strerror(res));
			goto error;
		}
		if (current == NULL || spa_pod_compare(current, format) != 0) {
			pw_log_debug("%p: output format change, renegotiate", this);
			if (current)
				pw_log_pod(SPA_LOG_LEVEL_DEBUG, current);
			pw_log_pod(SPA_LOG_LEVEL_DEBUG, format);
			pw_impl_node_set_state(output->node, PW_NODE_STATE_SUSPENDED);
			out_state = PW_IMPL_PORT_STATE_CONFIGURE;
		}
		else {
			pw_log_debug("%p: format was already set", this);
			changed = false;
		}
	}
	/* if input port had format and is idle, check if it changed. If so, renegotiate */
	if (in_state > PW_IMPL_PORT_STATE_CONFIGURE && input->node->info.state == PW_NODE_STATE_IDLE) {
		index = 0;
		res = spa_node_port_enum_params_sync(in_node,
				input->direction, in_port,
				SPA_PARAM_Format, &index,
				NULL, &current, &b);
		switch (res) {
		case -EIO:
			current = NULL;
			res = 0;
			SPA_FALLTHROUGH
		case 1:
			break;
		case 0:
			res = -EBADF;
			SPA_FALLTHROUGH
		default:
			error = spa_aprintf("error get input format: %s", spa_strerror(res));
			goto error;
		}
		if (current == NULL || spa_pod_compare(current, format) != 0) {
			pw_log_debug("%p: input format change, renegotiate", this);
			if (current)
				pw_log_pod(SPA_LOG_LEVEL_DEBUG, current);
			pw_log_pod(SPA_LOG_LEVEL_DEBUG, format);
			pw_impl_node_set_state(input->node, PW_NODE_STATE_SUSPENDED);
			in_state = PW_IMPL_PORT_STATE_CONFIGURE;
		}
		else {
			pw_log_debug("%p: format was already set", this);
			changed = false;
		}
	}

	pw_log_pod(SPA_LOG_LEVEL_DEBUG, format);

	SPA_POD_OBJECT_ID(format) = SPA_PARAM_Format;
	pw_log_debug("%p: doing set format %p fixated:%d", this,
			format, spa_pod_is_fixated(format));

	if (out_state == PW_IMPL_PORT_STATE_CONFIGURE) {
		pw_log_debug("%p: doing set format on output", this);
		if ((res = pw_impl_port_set_param(output,
						SPA_PARAM_Format, 0,
						format)) < 0) {
			error = spa_aprintf("error set output format: %d (%s)", res, spa_strerror(res));
			pw_log_error("tried to set output format:");
			pw_log_pod(SPA_LOG_LEVEL_ERROR, format);
			goto error;
		}
		if (SPA_RESULT_IS_ASYNC(res)) {
			res = spa_node_sync(output->node->node, res);
			busy_id = pw_work_queue_add(impl->work, &this->output_link, res,
					complete_ready, this);
			output_set_busy_id(this, busy_id);
		} else {
			complete_ready(&this->output_link, this, res, SPA_ID_INVALID);
		}
	}
	if (in_state == PW_IMPL_PORT_STATE_CONFIGURE) {
		pw_log_debug("%p: doing set format on input", this);
		if ((res2 = pw_impl_port_set_param(input,
						SPA_PARAM_Format, 0,
						format)) < 0) {
			error = spa_aprintf("error set input format: %d (%s)", res2, spa_strerror(res2));
			pw_log_error("tried to set input format:");
			pw_log_pod(SPA_LOG_LEVEL_ERROR, format);
			goto error;
		}
		if (SPA_RESULT_IS_ASYNC(res2)) {
			res2 = spa_node_sync(input->node->node, res2);
			busy_id = pw_work_queue_add(impl->work, &this->input_link, res2,
					complete_ready, this);
			input_set_busy_id(this, busy_id);
			if (res == 0)
				res = res2;
		} else {
			complete_ready(&this->input_link, this, res2, SPA_ID_INVALID);
		}
	}

	free(this->info.format);
	this->info.format = format;

	if (changed)
		this->info.change_mask |= PW_LINK_CHANGE_MASK_FORMAT;

	pw_log_debug("%p: result %d", this, res);
	return res;

error:
	pw_context_debug_port_params(context, in_node,
			input->direction, in_port,
			SPA_PARAM_EnumFormat, res, "input format (%s)", error);
	pw_context_debug_port_params(context, out_node,
			output->direction, out_port,
			SPA_PARAM_EnumFormat, res, "output format (%s)", error);
	link_update_state(this, PW_LINK_STATE_ERROR, res, error);
	free(format);
	return res;
}

static int port_set_io(struct pw_impl_link *this, struct pw_impl_port *port, uint32_t id,
		void *data, size_t size, struct pw_impl_port_mix *mix)
{
	int res = 0;

	pw_log_debug("%p: %s port %p %d.%d set io: %d %p %zd", this,
			pw_direction_as_string(port->direction),
			port, port->port_id, mix->port.port_id, id, data, size);

	if ((res = spa_node_port_set_io(port->mix,
			     mix->port.direction,
			     mix->port.port_id,
			     id, data, size)) < 0) {
		if (res == -ENOTSUP)
			res = 0;
		else
			pw_log_warn("%p: port %p can't set io:%d (%s): %s",
					this, port, id,
					spa_debug_type_find_name(spa_type_io, id),
					spa_strerror(res));
	}
	return res;
}

static void select_io(struct pw_impl_link *this)
{
	struct impl *impl = SPA_CONTAINER_OF(this, struct impl, this);
	struct spa_io_buffers *io;

	io = this->rt.in_mix.io_data;
	if (io == NULL)
		io = this->rt.out_mix.io_data;
	if (io == NULL)
		io = impl->io;

	this->io = io;
	this->io[0] = SPA_IO_BUFFERS_INIT;
	this->io[1] = SPA_IO_BUFFERS_INIT;
}

static int do_allocation(struct pw_impl_link *this)
{
	struct impl *impl = SPA_CONTAINER_OF(this, struct impl, this);
	int res;
	uint32_t in_flags, out_flags, busy_id;
	char *error = NULL;
	struct pw_impl_port *input, *output;

	if (this->info.state > PW_LINK_STATE_ALLOCATING)
		return 0;

	output = this->output;
	input = this->input;

	pw_log_debug("%p: out-state:%d in-state:%d", this, output->state, input->state);

	if (input->state < PW_IMPL_PORT_STATE_READY || output->state < PW_IMPL_PORT_STATE_READY)
		return 0;

	link_update_state(this, PW_LINK_STATE_ALLOCATING, 0, NULL);

	out_flags = output->spa_flags;
	in_flags = input->spa_flags;

	pw_log_debug("%p: out-node:%p in-node:%p: out-flags:%08x in-flags:%08x",
			this, output->node, input->node, out_flags, in_flags);

	this->rt.in_mix.have_buffers = false;
	this->rt.out_mix.have_buffers = false;

	if (out_flags & SPA_PORT_FLAG_LIVE) {
		pw_log_debug("%p: setting link as live", this);
		output->node->live = true;
		input->node->live = true;
	}

	if (output->buffers.n_buffers) {
		pw_log_debug("%p: reusing %d output buffers %p", this,
				output->buffers.n_buffers, output->buffers.buffers);
		this->rt.out_mix.have_buffers = true;
	} else {
		uint32_t flags, alloc_flags;
		struct spa_node *in_node, *out_node;
		uint32_t in_port, out_port;

		flags = 0;
		/* always shared buffers for the link */
		alloc_flags = PW_BUFFERS_FLAG_SHARED;
		/* always enable async mode */
		alloc_flags |= PW_BUFFERS_FLAG_ASYNC;

		if (output->node->remote || input->node->remote)
			alloc_flags |= PW_BUFFERS_FLAG_SHARED_MEM;

		if (output->node->driver)
			alloc_flags |= PW_BUFFERS_FLAG_IN_PRIORITY;

		/* if output port can alloc buffers, alloc skeleton buffers */
		if (SPA_FLAG_IS_SET(out_flags, SPA_PORT_FLAG_CAN_ALLOC_BUFFERS)) {
			SPA_FLAG_SET(alloc_flags, PW_BUFFERS_FLAG_NO_MEM);
			flags |= SPA_NODE_BUFFERS_FLAG_ALLOC;
		}
#if 0
		in_node = input->mix;
		in_port = this->rt.in_mix.port.port_id;
		out_node = output->mix;
		out_port = this->rt.out_mix.port.port_id;
#else
		in_node = input->node->node;
		in_port = input->port_id;
		out_node = output->node->node;
		out_port = output->port_id;
#endif

		if ((res = pw_buffers_negotiate(this->context, alloc_flags,
						out_node, out_port,
						in_node, in_port,
						&output->buffers)) < 0) {
			error = spa_aprintf("error alloc buffers: %s", spa_strerror(res));
			goto error;
		}

		pw_log_debug("%p: allocating %d buffers %p", this,
			     output->buffers.n_buffers, output->buffers.buffers);

		if ((res = pw_impl_port_use_buffers(output, &this->rt.out_mix, flags,
						output->buffers.buffers,
						output->buffers.n_buffers)) < 0) {
			error = spa_aprintf("error use output buffers: %d (%s)", res,
					spa_strerror(res));
			goto error_clear;
		}
		if (SPA_RESULT_IS_ASYNC(res)) {
			res = spa_node_sync(output->node->node, res);
			busy_id = pw_work_queue_add(impl->work, &this->output_link, res,
					complete_paused, this);
			output_set_busy_id(this, busy_id);
			if (flags & SPA_NODE_BUFFERS_FLAG_ALLOC)
				return 0;
		} else {
			complete_paused(&this->output_link, this, res, SPA_ID_INVALID);
		}
	}

	pw_log_debug("%p: using %d buffers %p on input port", this,
		     output->buffers.n_buffers, output->buffers.buffers);

	if ((res = pw_impl_port_use_buffers(input, &this->rt.in_mix, 0,
				output->buffers.buffers,
				output->buffers.n_buffers)) < 0) {
		error = spa_aprintf("error use input buffers: %d (%s)", res,
				spa_strerror(res));
		goto error;
	}

	if (SPA_RESULT_IS_ASYNC(res)) {
		res = spa_node_sync(input->node->node, res);
		busy_id = pw_work_queue_add(impl->work, &this->input_link, res,
				complete_paused, this);
		input_set_busy_id(this, busy_id);
	} else {
		complete_paused(&this->input_link, this, res, SPA_ID_INVALID);
	}
	return 0;

error_clear:
	pw_buffers_clear(&output->buffers);
error:
	link_update_state(this, PW_LINK_STATE_ERROR, res, error);
	return res;
}

static int
do_activate_link(struct spa_loop *loop,
		 bool async, uint32_t seq, const void *data, size_t size, void *user_data)
{
	struct pw_impl_link *this = user_data;
	pw_log_trace("%p: activate", this);
	if (this->peer)
		pw_node_peer_activate(this->peer);
	return 0;
}

int pw_impl_link_activate(struct pw_impl_link *this)
{
	struct impl *impl = SPA_CONTAINER_OF(this, struct impl, this);
	int res;
	uint32_t io_type, io_size;

	pw_log_debug("%p: activate activated:%d state:%s", this, impl->activated,
			pw_link_state_as_string(this->info.state));

	if (this->destroyed || impl->activated || !this->prepared ||
		!impl->inode->runnable || !impl->onode->runnable)
		return 0;

	if (impl->async) {
		io_type = SPA_IO_AsyncBuffers;
		io_size = sizeof(struct spa_io_async_buffers);
	} else {
		io_type = SPA_IO_Buffers;
		io_size = sizeof(struct spa_io_buffers);
	}

	if ((res = port_set_io(this, this->input, io_type, this->io,
					io_size, &this->rt.in_mix)) < 0)
		goto error;
	if ((res = port_set_io(this, this->output, io_type, this->io,
					io_size, &this->rt.out_mix)) < 0)
		goto error_clean;

	pw_loop_invoke(this->output->node->data_loop,
	       do_activate_link, SPA_ID_INVALID, NULL, 0, false, this);

	impl->activated = true;
	pw_log_info("(%s) activated", this->name);
	link_update_state(this, PW_LINK_STATE_ACTIVE, 0, NULL);

	return 0;

error_clean:
	port_set_io(this, this->input, io_type, NULL, 0, &this->rt.in_mix);
error:
	pw_log_error("%p: can't activate link: %s", this, spa_strerror(res));
	return res;
}

static void check_states(void *obj, void *user_data, int res, uint32_t id)
{
	struct pw_impl_link *this = obj;
	struct impl *impl = SPA_CONTAINER_OF(this, struct impl, this);
	int in_state, out_state;
	struct pw_impl_port *input, *output;

	if (this->info.state == PW_LINK_STATE_ERROR)
		return;

	if (this->info.state >= PW_LINK_STATE_PAUSED)
		return;

	output = this->output;
	input = this->input;

	if (output == NULL || input == NULL) {
		link_update_state(this, PW_LINK_STATE_ERROR, -EIO,
				strdup("link without input or output port"));
		return;
	}

	if (output->node->info.state == PW_NODE_STATE_ERROR ||
	    input->node->info.state == PW_NODE_STATE_ERROR) {
		pw_log_warn("%p: one of the nodes is in error out:%s in:%s", this,
				pw_node_state_as_string(output->node->info.state),
				pw_node_state_as_string(input->node->info.state));
		return;
	}

	out_state = output->state;
	in_state = input->state;

	pw_log_debug("%p: output state %d, input state %d", this, out_state, in_state);

	if (out_state == PW_IMPL_PORT_STATE_ERROR || in_state == PW_IMPL_PORT_STATE_ERROR) {
		link_update_state(this, PW_LINK_STATE_ERROR, -EIO, strdup("ports are in error"));
		return;
	}

	if (PW_IMPL_PORT_IS_CONTROL(output) && PW_IMPL_PORT_IS_CONTROL(input)) {
		pw_impl_port_update_state(output, PW_IMPL_PORT_STATE_PAUSED, 0, NULL);
		pw_impl_port_update_state(input, PW_IMPL_PORT_STATE_PAUSED, 0, NULL);
		link_update_state(this, PW_LINK_STATE_PAUSED, 0, NULL);
	}

	if (output->busy_count > 0) {
		pw_log_debug("%p: output port %p was busy %d", this, output, output->busy_count);
		res = spa_node_sync(output->node->node, 0);
		pw_work_queue_add(impl->work, &this->output_link, res, complete_sync, this);
		goto exit;
	}
	else if (input->busy_count > 0) {
		pw_log_debug("%p: input port %p was busy %d", this, input, input->busy_count);
		res = spa_node_sync(input->node->node, 0);
		pw_work_queue_add(impl->work, &this->input_link, res, complete_sync, this);
		goto exit;
	}

	if ((res = do_negotiate(this)) != 0)
		goto exit;

	if ((res = do_allocation(this)) != 0)
		goto exit;

exit:
	if (SPA_RESULT_IS_ERROR(res)) {
		pw_log_debug("%p: got error result %d (%s)", this, res, spa_strerror(res));
		return;
	}

	pw_work_queue_add(impl->work,
			  this, -EBUSY, (pw_work_func_t) check_states, this);
}

static void input_remove(struct pw_impl_link *this, struct pw_impl_port *port)
{
	struct impl *impl = (struct impl *) this;
	struct pw_impl_port_mix *mix = &this->rt.in_mix;
	int res;

	pw_log_debug("%p: remove input port %p", this, port);

	input_set_busy_id(this, SPA_ID_INVALID);

	spa_hook_remove(&impl->input_port_listener);
	spa_hook_remove(&impl->input_node_listener);
	spa_hook_remove(&impl->input_global_listener);

	spa_list_remove(&this->input_link);
	pw_impl_port_emit_link_removed(this->input, this);

	pw_impl_port_recalc_latency(this->input);
	pw_impl_port_recalc_tag(this->input);

	if ((res = pw_impl_port_use_buffers(port, mix, 0, NULL, 0)) < 0) {
		pw_log_warn("%p: port %p clear error %s", this, port, spa_strerror(res));
	}
	pw_impl_port_release_mix(port, mix);

	pw_work_queue_cancel(impl->work, &this->input_link, SPA_ID_INVALID);
	this->input = NULL;
}

static void output_remove(struct pw_impl_link *this, struct pw_impl_port *port)
{
	struct impl *impl = (struct impl *) this;
	struct pw_impl_port_mix *mix = &this->rt.out_mix;

	pw_log_debug("%p: remove output port %p", this, port);

	output_set_busy_id(this, SPA_ID_INVALID);

	spa_hook_remove(&impl->output_port_listener);
	spa_hook_remove(&impl->output_node_listener);
	spa_hook_remove(&impl->output_global_listener);

	spa_list_remove(&this->output_link);
	pw_impl_port_emit_link_removed(this->output, this);

	pw_impl_port_recalc_latency(this->output);
	pw_impl_port_recalc_tag(this->output);

	/* we don't clear output buffers when the link goes away. They will get
	 * cleared when the node goes to suspend */
	pw_impl_port_release_mix(port, mix);

	pw_work_queue_cancel(impl->work, &this->output_link, SPA_ID_INVALID);
	this->output = NULL;
}

int pw_impl_link_prepare(struct pw_impl_link *this)
{
	struct impl *impl = SPA_CONTAINER_OF(this, struct impl, this);

	pw_log_debug("%p: prepared:%d preparing:%d in_active:%d out_active:%d passive:%u",
			this, this->prepared, this->preparing,
			impl->inode->active, impl->onode->active, this->passive);

	if (!impl->inode->active || !impl->onode->active)
		return 0;

	if (this->destroyed || this->preparing || this->prepared)
		return 0;

	this->preparing = true;

	pw_work_queue_add(impl->work,
			  this, -EBUSY, (pw_work_func_t) check_states, this);

	return 0;
}

static int
do_deactivate_link(struct spa_loop *loop,
		   bool async, uint32_t seq, const void *data, size_t size, void *user_data)
{
        struct pw_impl_link *this = user_data;
	pw_log_trace("%p: disable out %p", this, &this->rt.out_mix);
	if (this->peer)
		pw_node_peer_deactivate(this->peer);
	return 0;
}

int pw_impl_link_deactivate(struct pw_impl_link *this)
{
	struct impl *impl = SPA_CONTAINER_OF(this, struct impl, this);

	pw_log_debug("%p: deactivate activated:%d", this, impl->activated);

	if (!impl->activated)
		return 0;

	pw_loop_invoke(this->output->node->data_loop,
		       do_deactivate_link, SPA_ID_INVALID, NULL, 0, true, this);

	port_set_io(this, this->output, SPA_IO_Buffers, NULL, 0,
			&this->rt.out_mix);
	port_set_io(this, this->input, SPA_IO_Buffers, NULL, 0,
			&this->rt.in_mix);

	impl->activated = false;
	pw_log_info("(%s) deactivated", this->name);

	if (this->info.state < PW_LINK_STATE_PAUSED || this->destroyed)
		link_update_state(this, PW_LINK_STATE_INIT, 0, NULL);
	else
		link_update_state(this, PW_LINK_STATE_PAUSED, 0, NULL);
	return 0;
}

static int
global_bind(void *object, struct pw_impl_client *client, uint32_t permissions,
	       uint32_t version, uint32_t id)
{
	struct pw_impl_link *this = object;
	struct pw_global *global = this->global;
	struct pw_resource *resource;

	resource = pw_resource_new(client, id, permissions, global->type, version, 0);
	if (resource == NULL)
		goto error_resource;

	pw_log_debug("%p: bound to %d", this, resource->id);
	pw_global_add_resource(global, resource);

	this->info.change_mask = PW_LINK_CHANGE_MASK_ALL;
	pw_link_resource_info(resource, &this->info);
	this->info.change_mask = 0;

	return 0;

error_resource:
	pw_log_error("%p: can't create link resource: %m", this);
	return -errno;
}

static void port_state_changed(struct pw_impl_link *this, struct pw_impl_port *port,
		struct pw_impl_port *other, enum pw_impl_port_state old,
		enum pw_impl_port_state state, const char *error)
{
	pw_log_debug("%p: port %p old:%d -> state:%d prepared:%d preparing:%d",
			this, port, old, state, this->prepared, this->preparing);

	switch (state) {
	case PW_IMPL_PORT_STATE_ERROR:
		link_update_state(this, PW_LINK_STATE_ERROR, -EIO, error ? strdup(error) : NULL);
		break;
	case PW_IMPL_PORT_STATE_INIT:
	case PW_IMPL_PORT_STATE_CONFIGURE:
		if (this->prepared || state < old) {
			this->prepared = false;
			link_update_state(this, PW_LINK_STATE_INIT, 0, NULL);
		}
		break;
	case PW_IMPL_PORT_STATE_READY:
		if (this->prepared || state < old) {
			this->prepared = false;
			link_update_state(this, PW_LINK_STATE_NEGOTIATING, 0, NULL);
		}
		break;
	case PW_IMPL_PORT_STATE_PAUSED:
		break;
	}
}

static void port_param_changed(struct pw_impl_link *this, uint32_t id,
		struct pw_impl_port *outport, struct pw_impl_port *inport)
{
	enum pw_impl_port_state target;

	pw_log_debug("%p: outport %p input %p param %d (%s)", this,
		outport, inport, id, spa_debug_type_find_name(spa_type_param, id));

	switch (id) {
	case SPA_PARAM_EnumFormat:
		target = PW_IMPL_PORT_STATE_CONFIGURE;
		break;
	default:
		return;
	}
	if (outport)
		pw_impl_port_update_state(outport, target, 0, NULL);
	if (inport)
		pw_impl_port_update_state(inport, target, 0, NULL);

	this->preparing = this->prepared = false;
	link_update_state(this, PW_LINK_STATE_INIT, 0, NULL);
	pw_impl_link_prepare(this);
}

static void input_port_param_changed(void *data, uint32_t id)
{
	struct impl *impl = data;
	struct pw_impl_link *this = &impl->this;
	port_param_changed(this, id, this->output, this->input);
}

static void input_port_state_changed(void *data, enum pw_impl_port_state old,
			enum pw_impl_port_state state, const char *error)
{
	struct impl *impl = data;
	struct pw_impl_link *this = &impl->this;
	port_state_changed(this, this->input, this->output, old, state, error);
}

static void output_port_param_changed(void *data, uint32_t id)
{
	struct impl *impl = data;
	struct pw_impl_link *this = &impl->this;
	port_param_changed(this, id, this->output, this->input);
}

static void output_port_state_changed(void *data, enum pw_impl_port_state old,
			enum pw_impl_port_state state, const char *error)
{
	struct impl *impl = data;
	struct pw_impl_link *this = &impl->this;
	port_state_changed(this, this->output, this->input, old, state, error);
}

static void input_port_latency_changed(void *data)
{
	struct impl *impl = data;
	struct pw_impl_link *this = &impl->this;
	if (!this->feedback)
		pw_impl_port_recalc_latency(this->output);
}

static void input_port_tag_changed(void *data)
{
	struct impl *impl = data;
	struct pw_impl_link *this = &impl->this;
	if (!this->feedback)
		pw_impl_port_recalc_tag(this->output);
}

static void output_port_latency_changed(void *data)
{
	struct impl *impl = data;
	struct pw_impl_link *this = &impl->this;
	if (!this->feedback)
		pw_impl_port_recalc_latency(this->input);
}

static void output_port_tag_changed(void *data)
{
	struct impl *impl = data;
	struct pw_impl_link *this = &impl->this;
	if (!this->feedback)
		pw_impl_port_recalc_tag(this->input);
}

static const struct pw_impl_port_events input_port_events = {
	PW_VERSION_IMPL_PORT_EVENTS,
	.param_changed = input_port_param_changed,
	.state_changed = input_port_state_changed,
	.latency_changed = input_port_latency_changed,
	.tag_changed = input_port_tag_changed,
};

static const struct pw_impl_port_events output_port_events = {
	PW_VERSION_IMPL_PORT_EVENTS,
	.param_changed = output_port_param_changed,
	.state_changed = output_port_state_changed,
	.latency_changed = output_port_latency_changed,
	.tag_changed = output_port_tag_changed,
};

static void node_result(struct impl *impl, void *obj,
		int seq, int res, uint32_t type, const void *result)
{
	if (SPA_RESULT_IS_ASYNC(seq))
		pw_work_queue_complete(impl->work, obj, SPA_RESULT_ASYNC_SEQ(seq), res);
}

static void input_node_result(void *data, int seq, int res, uint32_t type, const void *result)
{
	struct impl *impl = data;
	struct pw_impl_port *port = impl->this.input;
	pw_log_trace("%p: input port %p result seq:%d res:%d type:%u",
			impl, port, seq, res, type);
	node_result(impl, &impl->this.input_link, seq, res, type, result);
}

static void output_node_result(void *data, int seq, int res, uint32_t type, const void *result)
{
	struct impl *impl = data;
	struct pw_impl_port *port = impl->this.output;
	pw_log_trace("%p: output port %p result seq:%d res:%d type:%u",
			impl, port, seq, res, type);
	node_result(impl, &impl->this.output_link, seq, res, type, result);
}

static void node_active_changed(void *data, bool active)
{
	struct impl *impl = data;
	pw_impl_link_prepare(&impl->this);
}

static const struct pw_impl_node_events input_node_events = {
	PW_VERSION_IMPL_NODE_EVENTS,
	.result = input_node_result,
	.active_changed = node_active_changed,
};

static const struct pw_impl_node_events output_node_events = {
	PW_VERSION_IMPL_NODE_EVENTS,
	.result = output_node_result,
	.active_changed = node_active_changed,
};

static bool pw_impl_node_can_reach(struct pw_impl_node *output, struct pw_impl_node *input, int hop)
{
	struct pw_impl_port *p;
	struct pw_impl_link *l;

	output->loopchecked = true;

	if (output == input)
		return true;

	if (hop == MAX_HOPS) {
		pw_log_warn("exceeded hops (%d) %s -> %s", hop, output->name, input->name);
		return false;
	}

	spa_list_for_each(p, &output->output_ports, link) {
		spa_list_for_each(l, &p->links, output_link)
			l->input->node->loopchecked = l->feedback;
	}
	spa_list_for_each(p, &output->output_ports, link) {
		spa_list_for_each(l, &p->links, output_link) {
			if (l->input->node->loopchecked)
				continue;
			if (pw_impl_node_can_reach(l->input->node, input, hop+1))
				return true;
		}
	}
	return false;
}

static void try_link_controls(struct impl *impl, struct pw_impl_port *output, struct pw_impl_port *input)
{
	struct pw_control *cin, *cout;
	struct pw_impl_link *this = &impl->this;
	uint32_t omix, imix;
	int res;

	imix = this->rt.in_mix.port.port_id;
	omix = this->rt.out_mix.port.port_id;

	pw_log_debug("%p: trying controls", impl);
	spa_list_for_each(cout, &output->control_list[SPA_DIRECTION_OUTPUT], port_link) {
		spa_list_for_each(cin, &input->control_list[SPA_DIRECTION_INPUT], port_link) {
			if ((res = pw_control_add_link(cout, omix, cin, imix, &this->control)) < 0)
				pw_log_error("%p: failed to link controls: %s",
						this, spa_strerror(res));
			break;
		}
	}
	spa_list_for_each(cin, &output->control_list[SPA_DIRECTION_INPUT], port_link) {
		spa_list_for_each(cout, &input->control_list[SPA_DIRECTION_OUTPUT], port_link) {
			if ((res = pw_control_add_link(cout, imix, cin, omix, &this->notify)) < 0)
				pw_log_error("%p: failed to link controls: %s",
						this, spa_strerror(res));
			break;
		}
	}
}

static void try_unlink_controls(struct impl *impl, struct pw_impl_port *output, struct pw_impl_port *input)
{
	struct pw_impl_link *this = &impl->this;
	int res;

	pw_log_debug("%p: unlinking controls", impl);
	if (this->control.valid) {
		if ((res = pw_control_remove_link(&this->control)) < 0)
			pw_log_error("%p: failed to unlink controls: %s",
					this, spa_strerror(res));
	}
	if (this->notify.valid) {
		if ((res = pw_control_remove_link(&this->notify)) < 0)
			pw_log_error("%p: failed to unlink controls: %s",
					this, spa_strerror(res));
	}
}

static int check_owner_permissions(struct pw_context *context,
		struct pw_impl_node *node, struct pw_global *other, uint32_t permissions)
{
	const char *str;
	struct pw_impl_client *client;
	struct pw_global *global;
	uint32_t perms;
	uint32_t client_id;

	str = pw_properties_get(node->properties, PW_KEY_CLIENT_ID);
	if (str == NULL)
		/* node not owned by client */
		return 0;

	if (!spa_atou32(str, &client_id, 0))
		/* invalid client_id, something is wrong */
		return -EIO;
	if ((global = pw_context_find_global(context, client_id)) == NULL)
		/* current client can't see the owner client */
		return -errno;
	if (!pw_global_is_type(global, PW_TYPE_INTERFACE_Client) ||
	    (client = global->object) == NULL)
		/* not the right object, something wrong */
		return -EIO;

	perms = pw_global_get_permissions(other, client);
	if ((perms & permissions) != permissions)
		/* owner client can't see other node */
		return -EPERM;

	return 0;
}

static int
check_permission(struct pw_context *context,
		 struct pw_impl_port *output,
		 struct pw_impl_port *input,
		 struct pw_properties *properties)
{
	int res;
	uint32_t in_perms, out_perms;
	struct pw_global *in_global, *out_global;

	if ((in_global = input->node->global) == NULL)
		return -ENOENT;
	if ((out_global = output->node->global) == NULL)
		return -ENOENT;

	in_perms = out_perms = PW_PERM_R | PW_PERM_L;
	if (context->current_client != NULL) {
		in_perms = pw_global_get_permissions(in_global, context->current_client);
		out_perms = pw_global_get_permissions(out_global, context->current_client);
	}
	/* current client can't see input node or output node */
	if (!PW_PERM_IS_R(in_perms) || !PW_PERM_IS_R(out_perms))
		return -ENOENT;

	if ((res = check_owner_permissions(context, output->node,
					in_global, PW_PERM_R)) < 0) {
		/* output node owner can't see input node, check if the current
		 * client has universal link permissions for the output node */
		if (!PW_PERM_IS_L(out_perms))
			return res;
	}
	if ((res = check_owner_permissions(context, input->node,
					out_global, PW_PERM_R)) < 0) {
		/* input node owner can't see output node, check if the current
		 * client has universal link permissions for the input node */
		if (!PW_PERM_IS_L(in_perms))
			return res;
	}
	return 0;
}

static void permissions_changed(struct pw_impl_link *this, struct pw_impl_port *other,
		struct pw_impl_client *client, uint32_t old, uint32_t new)
{
	int res;
	uint32_t perm;

	perm = pw_global_get_permissions(other->global, client);
	old &= perm;
	new &= perm;
	pw_log_debug("%p: permissions changed %08x -> %08x", this, old, new);

	if ((res = check_permission(this->context, this->output, this->input, this->properties)) < 0) {
		pw_log_info("%p: link permissions removed: %s", this, spa_strerror(res));
		pw_impl_link_destroy(this);
	} else if (this->global != NULL) {
		pw_global_update_permissions(this->global, client, old, new);
	}
}

static bool is_port_owner(struct pw_impl_client *client, struct pw_impl_port *port)
{
	const char *str;
	uint32_t client_id;

	str = pw_properties_get(port->node->properties, PW_KEY_CLIENT_ID);
	if (str == NULL)
		return false;

	if (!spa_atou32(str, &client_id, 0))
		return false;

	return client_id == client->info.id;
}

static void output_permissions_changed(void *data,
		struct pw_impl_client *client, uint32_t old, uint32_t new)
{
	struct pw_impl_link *this = data;
	if (!is_port_owner(client, this->output) &&
	    !is_port_owner(client, this->input))
		return;
	permissions_changed(this, this->input, client, old, new);
}

static const struct pw_global_events output_global_events = {
	PW_VERSION_GLOBAL_EVENTS,
	.permissions_changed = output_permissions_changed,
};

static void input_permissions_changed(void *data,
		struct pw_impl_client *client, uint32_t old, uint32_t new)
{
	struct pw_impl_link *this = data;
	if (!is_port_owner(client, this->output) &&
	    !is_port_owner(client, this->input))
		return;
	permissions_changed(this, this->output, client, old, new);
}

static const struct pw_global_events input_global_events = {
	PW_VERSION_GLOBAL_EVENTS,
	.permissions_changed = input_permissions_changed,
};

SPA_EXPORT
struct pw_impl_link *pw_context_create_link(struct pw_context *context,
			    struct pw_impl_port *output,
			    struct pw_impl_port *input,
			    struct spa_pod *format_filter,
			    struct pw_properties *properties,
			    size_t user_data_size)
{
	struct impl *impl;
	struct pw_impl_link *this;
	struct pw_impl_node *input_node, *output_node;
	const char *str;
	int res;

	if (output == input)
		goto error_same_ports;

	if (output->direction != PW_DIRECTION_OUTPUT ||
	    input->direction != PW_DIRECTION_INPUT)
		goto error_wrong_direction;

	if (pw_impl_link_find(output, input))
		goto error_link_exists;

	if (check_permission(context, output, input, properties) < 0)
		goto error_link_not_allowed;

	output_node = output->node;
	input_node = input->node;

	if (properties == NULL)
		properties = pw_properties_new(NULL, NULL);
	if (properties == NULL)
		goto error_no_mem;

	impl = calloc(1, sizeof(struct impl) + user_data_size);
	if (impl == NULL)
		goto error_no_mem;

	impl->input_busy_id = SPA_ID_INVALID;
	impl->output_busy_id = SPA_ID_INVALID;

	this = &impl->this;
	this->feedback = pw_impl_node_can_reach(input_node, output_node, 0);
	pw_properties_set(properties, PW_KEY_LINK_FEEDBACK, this->feedback ? "true" : NULL);

	pw_log_debug("%p: new out-port:%p -> in-port:%p", this, output, input);

	if (user_data_size > 0)
                this->user_data = SPA_PTROFF(impl, sizeof(struct impl), void);

	impl->work = pw_context_get_work_queue(context);

	this->context = context;
	this->properties = properties;
	this->info.state = PW_LINK_STATE_INIT;

	this->output = output;
	this->input = input;

	/* passive means that this link does not make the nodes active */
	str = pw_properties_get(properties, PW_KEY_LINK_PASSIVE);
	this->passive = str ? spa_atob(str) :
		(output->passive && input_node->can_suspend) ||
		(input->passive && output_node->can_suspend) ||
		(input->passive && output->passive);
	if (this->passive && str == NULL)
		 pw_properties_set(properties, PW_KEY_LINK_PASSIVE, "true");

	impl->async = (output_node->async || input_node->async) &&
		SPA_FLAG_IS_SET(output->flags, PW_IMPL_PORT_FLAG_ASYNC) &&
		SPA_FLAG_IS_SET(input->flags, PW_IMPL_PORT_FLAG_ASYNC);

	if (impl->async)
		 pw_properties_set(properties, PW_KEY_LINK_ASYNC, "true");

	spa_hook_list_init(&this->listener_list);

	impl->format_filter = format_filter;
	this->info.format = NULL;
	this->info.props = &this->properties->dict;

	this->rt.out_mix.peer_id = input->global->id;
	this->rt.in_mix.peer_id = output->global->id;

	if ((res = pw_impl_port_init_mix(output, &this->rt.out_mix)) < 0)
		goto error_output_mix;
	if ((res = pw_impl_port_init_mix(input, &this->rt.in_mix)) < 0)
		goto error_input_mix;

	pw_impl_port_add_listener(input, &impl->input_port_listener, &input_port_events, impl);
	pw_impl_node_add_listener(input_node, &impl->input_node_listener, &input_node_events, impl);
	pw_global_add_listener(input->global, &impl->input_global_listener, &input_global_events, impl);
	pw_impl_port_add_listener(output, &impl->output_port_listener, &output_port_events, impl);
	pw_impl_node_add_listener(output_node, &impl->output_node_listener, &output_node_events, impl);
	pw_global_add_listener(output->global, &impl->output_global_listener, &output_global_events, impl);

	input_node->live = output_node->live;

	pw_log_debug("%p: output node %p live %d, feedback %d",
			this, output_node, output_node->live, this->feedback);

	spa_list_append(&output->links, &this->output_link);
	spa_list_append(&input->links, &this->input_link);

	select_io(this);

	if (this->feedback) {
		impl->inode = output_node;
		impl->onode = input_node;
	}
	else {
		impl->onode = output_node;
		impl->inode = input_node;
	}

	pw_log_debug("%p: constructed out:%p:%d.%d -> in:%p:%d.%d", impl,
		     output_node, output->port_id, this->rt.out_mix.port.port_id,
		     input_node, input->port_id, this->rt.in_mix.port.port_id);

	this->name = spa_aprintf("%d.%d.%d -> %d.%d.%d",
			output_node->info.id, output->port_id, this->rt.out_mix.port.port_id,
			input_node->info.id, input->port_id, this->rt.in_mix.port.port_id);
	pw_log_info("(%s) (%s) -> (%s) async:%04x:%04x:%d", this->name, output_node->name,
			input_node->name, output->flags, input->flags, impl->async);

	pw_impl_port_emit_link_added(output, this);
	pw_impl_port_emit_link_added(input, this);

	try_link_controls(impl, output, input);

	pw_impl_port_recalc_latency(output);
	pw_impl_port_recalc_latency(input);
	pw_impl_port_recalc_tag(output);
	pw_impl_port_recalc_tag(input);

	if (impl->onode != impl->inode)
		this->peer = pw_node_peer_ref(impl->onode, impl->inode);

	return this;

error_same_ports:
	res = -EINVAL;
	pw_log_debug("can't link the same ports");
	goto error_exit;
error_wrong_direction:
	res = -EINVAL;
	pw_log_debug("ports have wrong direction");
	goto error_exit;
error_link_exists:
	res = -EEXIST;
	pw_log_debug("link already exists");
	goto error_exit;
error_link_not_allowed:
	res = -EPERM;
	pw_log_debug("link not allowed");
	goto error_exit;
error_no_mem:
	res = -errno;
	pw_log_debug("alloc failed: %m");
	goto error_exit;
error_output_mix:
	pw_log_error("%p: can't get output mix %d (%s)", this, res, spa_strerror(res));
	goto error_free;
error_input_mix:
	pw_log_error("%p: can't get input mix %d (%s)", this, res, spa_strerror(res));
	pw_impl_port_release_mix(output, &this->rt.out_mix);
	goto error_free;
error_free:
	free(impl);
error_exit:
	pw_properties_free(properties);
	errno = -res;
	return NULL;
}

static void global_destroy(void *data)
{
	struct pw_impl_link *link = data;
	spa_hook_remove(&link->global_listener);
	link->global = NULL;
	pw_impl_link_destroy(link);
}

static const struct pw_global_events global_events = {
	PW_VERSION_GLOBAL_EVENTS,
	.destroy = global_destroy,
};

SPA_EXPORT
int pw_impl_link_register(struct pw_impl_link *link,
		     struct pw_properties *properties)
{
	static const char * const keys[] = {
		PW_KEY_OBJECT_PATH,
		PW_KEY_MODULE_ID,
		PW_KEY_FACTORY_ID,
		PW_KEY_CLIENT_ID,
		PW_KEY_LINK_OUTPUT_PORT,
		PW_KEY_LINK_INPUT_PORT,
		PW_KEY_LINK_OUTPUT_NODE,
		PW_KEY_LINK_INPUT_NODE,
		NULL
	};

	struct pw_context *context = link->context;
	struct pw_impl_node *output_node, *input_node;

	if (link->registered)
		goto error_existed;

	output_node = link->output->node;
	input_node = link->input->node;

	link->info.output_node_id = output_node->global->id;
	link->info.output_port_id = link->output->global->id;
	link->info.input_node_id = input_node->global->id;
	link->info.input_port_id = link->input->global->id;

	link->global = pw_global_new(context,
				     PW_TYPE_INTERFACE_Link,
				     PW_VERSION_LINK,
				     PW_LINK_PERM_MASK,
				     properties,
				     global_bind,
				     link);
	if (link->global == NULL)
		return -errno;

	spa_list_append(&context->link_list, &link->link);
	link->registered = true;

	link->info.id = link->global->id;
	pw_properties_setf(link->properties, PW_KEY_OBJECT_ID, "%d", link->info.id);
	pw_properties_setf(link->properties, PW_KEY_OBJECT_SERIAL, "%"PRIu64,
			pw_global_get_serial(link->global));
	pw_properties_setf(link->properties, PW_KEY_LINK_OUTPUT_NODE, "%u", link->info.output_node_id);
	pw_properties_setf(link->properties, PW_KEY_LINK_OUTPUT_PORT, "%u", link->info.output_port_id);
	pw_properties_setf(link->properties, PW_KEY_LINK_INPUT_NODE, "%u", link->info.input_node_id);
	pw_properties_setf(link->properties, PW_KEY_LINK_INPUT_PORT, "%u", link->info.input_port_id);
	link->info.props = &link->properties->dict;

	pw_global_update_keys(link->global, link->info.props, keys);

	pw_impl_link_emit_initialized(link);

	pw_global_add_listener(link->global, &link->global_listener, &global_events, link);
	pw_global_register(link->global);

	pw_impl_link_prepare(link);

	return 0;

error_existed:
	pw_properties_free(properties);
	return -EEXIST;
}

SPA_EXPORT
void pw_impl_link_destroy(struct pw_impl_link *link)
{
	struct impl *impl = SPA_CONTAINER_OF(link, struct impl, this);
	bool was_prepared = link->prepared;

	pw_log_debug("%p: destroy", impl);
	pw_log_info("(%s) destroy", link->name);

	link->destroyed = true;
	pw_impl_link_emit_destroy(link);

	pw_impl_link_deactivate(link);

	if (link->registered)
		spa_list_remove(&link->link);

	if (link->peer)
		pw_node_peer_unref(link->peer);

	try_unlink_controls(impl, link->output, link->input);

	output_remove(link, link->output);
	input_remove(link, link->input);

	if (link->global) {
		spa_hook_remove(&link->global_listener);
		pw_global_destroy(link->global);
	}

	if (was_prepared)
		pw_context_recalc_graph(link->context, "link destroy");

	pw_log_debug("%p: free", impl);
	pw_impl_link_emit_free(link);

	pw_work_queue_cancel(impl->work, link, SPA_ID_INVALID);

	spa_hook_list_clean(&link->listener_list);

	pw_properties_free(link->properties);

	free(link->name);
	free(link->info.format);
	free(impl);
}

SPA_EXPORT
void pw_impl_link_add_listener(struct pw_impl_link *link,
			  struct spa_hook *listener,
			  const struct pw_impl_link_events *events,
			  void *data)
{
	pw_log_debug("%p: add listener %p", link, listener);
	spa_hook_list_append(&link->listener_list, listener, events, data);
}

struct pw_impl_link *pw_impl_link_find(struct pw_impl_port *output_port, struct pw_impl_port *input_port)
{
	struct pw_impl_link *pl;

	spa_list_for_each(pl, &output_port->links, output_link) {
		if (pl->input == input_port)
			return pl;
	}
	return NULL;
}

SPA_EXPORT
struct pw_context *pw_impl_link_get_context(struct pw_impl_link *link)
{
	return link->context;
}

SPA_EXPORT
void *pw_impl_link_get_user_data(struct pw_impl_link *link)
{
	return link->user_data;
}

SPA_EXPORT
const struct pw_link_info *pw_impl_link_get_info(struct pw_impl_link *link)
{
	return &link->info;
}

SPA_EXPORT
struct pw_global *pw_impl_link_get_global(struct pw_impl_link *link)
{
	return link->global;
}

SPA_EXPORT
struct pw_impl_port *pw_impl_link_get_output(struct pw_impl_link *link)
{
	return link->output;
}

SPA_EXPORT
struct pw_impl_port *pw_impl_link_get_input(struct pw_impl_link *link)
{
	return link->input;
}
