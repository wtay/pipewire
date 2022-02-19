/* Spa
 *
 * Copyright © 2018 Wim Taymans
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice (including the next
 * paragraph) shall be included in all copies or substantial portions of the
 * Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
 * DEALINGS IN THE SOFTWARE.
 */

#include <unistd.h>
#include <errno.h>
#include <sys/types.h>
#include <signal.h>
#include <stdlib.h>
#include <stdio.h>
#include <pthread.h>

#include <spa/support/loop.h>
#include <spa/support/system.h>
#include <spa/support/log.h>
#include <spa/support/plugin.h>
#include <spa/utils/list.h>
#include <spa/utils/names.h>
#include <spa/utils/result.h>
#include <spa/utils/type.h>
#include <spa/utils/ringbuffer.h>
#include <spa/utils/string.h>

static struct spa_log_topic log_topic = SPA_LOG_TOPIC(0, "spa.loop");
#undef SPA_LOG_TOPIC_DEFAULT
#define SPA_LOG_TOPIC_DEFAULT &log_topic

#define MAX_ALIGN	8
#define ITEM_ALIGN	8
#define DATAS_SIZE	(4096*8)
#define MAX_EP		32

/** \cond */

struct invoke_item {
	size_t item_size;
	spa_invoke_func_t func;
	uint32_t seq;
	void *data;
	size_t size;
	bool block;
	void *user_data;
	int res;
};

static int loop_signal_event(void *object, struct spa_source *source);

struct impl {
	struct spa_handle handle;
	struct spa_loop loop;
	struct spa_loop_control control;
	struct spa_loop_utils utils;

        struct spa_log *log;
        struct spa_system *system;

	struct spa_list source_list;
	struct spa_list destroy_list;
	struct spa_hook_list hooks_list;

	int poll_fd;
	pthread_t thread;
	int enter_count;

	struct spa_source *wakeup;
	int ack_fd;

	struct spa_ringbuffer buffer;
	uint8_t *buffer_data;
	uint8_t buffer_mem[DATAS_SIZE + MAX_ALIGN];

	unsigned int flushing:1;
};

struct source_impl {
	struct spa_source source;

	struct impl *impl;
	struct spa_list link;

	union {
		spa_source_io_func_t io;
		spa_source_idle_func_t idle;
		spa_source_event_func_t event;
		spa_source_timer_func_t timer;
		spa_source_signal_func_t signal;
	} func;

	struct spa_source *fallback;

	bool close;
	bool enabled;
};
/** \endcond */

static int loop_add_source(void *object, struct spa_source *source)
{
	struct impl *impl = object;
	source->loop = &impl->loop;
	source->priv = NULL;
	source->rmask = 0;
	return spa_system_pollfd_add(impl->system, impl->poll_fd, source->fd, source->mask, source);
}

static int loop_update_source(void *object, struct spa_source *source)
{
	struct impl *impl = object;

	spa_assert(source->loop == &impl->loop);

	return spa_system_pollfd_mod(impl->system, impl->poll_fd, source->fd, source->mask, source);
}

static int loop_remove_source(void *object, struct spa_source *source)
{
	struct impl *impl = object;
	struct spa_poll_event *e;

	spa_assert(source->loop == &impl->loop);

	if ((e = source->priv)) {
		/* active in an iteration of the loop, remove it from there */
		e->data = NULL;
		source->priv = NULL;
	}
	source->loop = NULL;
	source->rmask = 0;
	return spa_system_pollfd_del(impl->system, impl->poll_fd, source->fd);
}

static void flush_items(struct impl *impl)
{
	uint32_t index;
	int res;

	impl->flushing = true;
	while (spa_ringbuffer_get_read_index(&impl->buffer, &index) > 0) {
		struct invoke_item *item;
		bool block;

		item = SPA_PTROFF(impl->buffer_data, index & (DATAS_SIZE - 1), struct invoke_item);
		block = item->block;

		spa_log_trace(impl->log, "%p: flush item %p", impl, item);
		item->res = item->func ? item->func(&impl->loop,
				true, item->seq, item->data, item->size,
			   item->user_data) : 0;

		spa_ringbuffer_read_update(&impl->buffer, index + item->item_size);

		if (block) {
			if ((res = spa_system_eventfd_write(impl->system, impl->ack_fd, 1)) < 0)
				spa_log_warn(impl->log, "%p: failed to write event fd: %s",
						impl, spa_strerror(res));
		}
	}
	impl->flushing = false;
}

static int
loop_invoke_inthread(struct impl *impl,
	    spa_invoke_func_t func,
	    uint32_t seq,
	    const void *data,
	    size_t size,
	    bool block,
	    void *user_data)
{
	if (!impl->flushing)
		flush_items(impl);
	return func ? func(&impl->loop, true, seq, data, size, user_data) : 0;
}

static int
loop_invoke(void *object,
	    spa_invoke_func_t func,
	    uint32_t seq,
	    const void *data,
	    size_t size,
	    bool block,
	    void *user_data)
{
	struct impl *impl = object;
	struct invoke_item *item;
	int res;
	int32_t filled;
	uint32_t avail, idx, offset, l0;

	if (impl->thread == 0 || pthread_equal(impl->thread, pthread_self()))
		return loop_invoke_inthread(impl, func, seq, data, size, block, user_data);

	filled = spa_ringbuffer_get_write_index(&impl->buffer, &idx);
	if (filled < 0 || filled > DATAS_SIZE) {
		spa_log_warn(impl->log, "%p: queue xrun %d", impl, filled);
		return -EPIPE;
	}
	avail = DATAS_SIZE - filled;
	if (avail < sizeof(struct invoke_item)) {
		spa_log_warn(impl->log, "%p: queue full %d", impl, avail);
		return -EPIPE;
	}
	offset = idx & (DATAS_SIZE - 1);

	/* l0 is remaining size in ringbuffer, this should always be larger than
	 * invoke_item, see below */
	l0 = DATAS_SIZE - offset;

	item = SPA_PTROFF(impl->buffer_data, offset, struct invoke_item);
	item->func = func;
	item->seq = seq;
	item->size = size;
	item->block = block;
	item->user_data = user_data;
	item->item_size = SPA_ROUND_UP_N(sizeof(struct invoke_item) + size, ITEM_ALIGN);

	spa_log_trace(impl->log, "%p: add item %p filled:%d", impl, item, filled);

	if (l0 >= item->item_size) {
		/* item + size fit in current ringbuffer idx */
		item->data = SPA_PTROFF(item, sizeof(struct invoke_item), void);
		if (l0 < sizeof(struct invoke_item) + item->item_size) {
			/* not enough space for next invoke_item, fill up till the end
			 * so that the next item will be at the start */
			item->item_size = l0;
		}
	} else {
		/* item does not fit, place the invoke_item at idx and start the
		 * data at the start of the ringbuffer */
		item->data = impl->buffer_data;
		item->item_size = SPA_ROUND_UP_N(l0 + size, ITEM_ALIGN);
	}
	if (avail < item->item_size) {
		spa_log_warn(impl->log, "%p: queue full %d, need %zd", impl, avail,
				item->item_size);
		return -EPIPE;
	}
	if (data && size > 0)
		memcpy(item->data, data, size);

	spa_ringbuffer_write_update(&impl->buffer, idx + item->item_size);

	loop_signal_event(impl, impl->wakeup);

	if (block) {
		uint64_t count = 1;

		spa_loop_control_hook_before(&impl->hooks_list);

		if ((res = spa_system_eventfd_read(impl->system, impl->ack_fd, &count)) < 0)
			spa_log_warn(impl->log, "%p: failed to read event fd: %s",
					impl, spa_strerror(res));

		spa_loop_control_hook_after(&impl->hooks_list);

		res = item->res;
	}
	else {
		if (seq != SPA_ID_INVALID)
			res = SPA_RESULT_RETURN_ASYNC(seq);
		else
			res = 0;
	}
	return res;
}

static void wakeup_func(void *data, uint64_t count)
{
	struct impl *impl = data;
	flush_items(impl);
}

static int loop_get_fd(void *object)
{
	struct impl *impl = object;
	return impl->poll_fd;
}

static void
loop_add_hook(void *object,
	      struct spa_hook *hook,
	      const struct spa_loop_control_hooks *hooks,
	      void *data)
{
	struct impl *impl = object;
	spa_hook_list_append(&impl->hooks_list, hook, hooks, data);
}

static void loop_enter(void *object)
{
	struct impl *impl = object;
	pthread_t thread_id = pthread_self();

	if (impl->enter_count == 0) {
		spa_return_if_fail(impl->thread == 0);
		impl->thread = thread_id;
		impl->enter_count = 1;
	} else {
		spa_return_if_fail(impl->enter_count > 0);
		spa_return_if_fail(impl->thread == thread_id);
		impl->enter_count++;
	}
	spa_log_trace(impl->log, "%p: enter %lu", impl, impl->thread);
}

static void loop_leave(void *object)
{
	struct impl *impl = object;
	pthread_t thread_id = pthread_self();

	spa_return_if_fail(impl->enter_count > 0);
	spa_return_if_fail(impl->thread == thread_id);

	spa_log_trace(impl->log, "%p: leave %lu", impl, impl->thread);

	if (--impl->enter_count == 0)
		impl->thread = 0;
}

static inline void process_destroy(struct impl *impl)
{
	struct source_impl *source, *tmp;
	spa_list_for_each_safe(source, tmp, &impl->destroy_list, link)
		free(source);
	spa_list_init(&impl->destroy_list);
}

static int loop_iterate(void *object, int timeout)
{
	struct impl *impl = object;
	struct spa_poll_event ep[MAX_EP], *e;
	int i, nfds;

	spa_loop_control_hook_before(&impl->hooks_list);

	nfds = spa_system_pollfd_wait(impl->system, impl->poll_fd, ep, SPA_N_ELEMENTS(ep), timeout);

	spa_loop_control_hook_after(&impl->hooks_list);

	/* first we set all the rmasks, then call the callbacks. The reason is that
	 * some callback might also want to look at other sources it manages and
	 * can then reset the rmask to suppress the callback */
	for (i = 0; i < nfds; i++) {
		struct spa_source *s = ep[i].data;
		s->rmask = ep[i].events;
		/* already active in another iteration of the loop,
		 * remove it from that iteration */
		if (SPA_UNLIKELY(e = s->priv))
			e->data = NULL;
		s->priv = &ep[i];
	}
	for (i = 0; i < nfds; i++) {
		struct spa_source *s = ep[i].data;
		if (SPA_LIKELY(s && s->rmask && s->loop))
			s->func(s);
	}
	for (i = 0; i < nfds; i++) {
		struct spa_source *s = ep[i].data;
		if (SPA_LIKELY(s)) {
			s->rmask = 0;
			s->priv = NULL;
		}
	}
	if (SPA_UNLIKELY(!spa_list_is_empty(&impl->destroy_list)))
		process_destroy(impl);

	return nfds;
}

static void source_io_func(struct spa_source *source)
{
	struct source_impl *s = SPA_CONTAINER_OF(source, struct source_impl, source);
	spa_log_trace_fp(s->impl->log, "%p: io %08x", s, source->rmask);
	s->func.io(source->data, source->fd, source->rmask);
}

static struct spa_source *loop_add_io(void *object,
				      int fd,
				      uint32_t mask,
				      bool close, spa_source_io_func_t func, void *data)
{
	struct impl *impl = object;
	struct source_impl *source;
	int res;

	source = calloc(1, sizeof(struct source_impl));
	if (source == NULL)
		goto error_exit;

	source->source.loop = &impl->loop;
	source->source.func = source_io_func;
	source->source.data = data;
	source->source.fd = fd;
	source->source.mask = mask;
	source->impl = impl;
	source->close = close;
	source->func.io = func;

	if ((res = loop_add_source(impl, &source->source)) < 0) {
		if (res != -EPERM)
			goto error_exit_free;

		/* file fds (stdin/stdout/...) give EPERM in epoll. Those fds always
		 * return from epoll with the mask set, so we can handle this with
		 * an idle source */
		source->source.rmask = mask;
		source->fallback = spa_loop_utils_add_idle(&impl->utils,
				mask & (SPA_IO_IN | SPA_IO_OUT) ? true : false,
				(spa_source_idle_func_t) source_io_func, source);
		spa_log_trace(impl->log, "%p: adding fallback %p", impl,
				source->fallback);
	}

	spa_list_insert(&impl->source_list, &source->link);

	return &source->source;

error_exit_free:
	free(source);
	errno = -res;
error_exit:
	return NULL;
}

static int loop_update_io(void *object, struct spa_source *source, uint32_t mask)
{
	struct impl *impl = object;
	struct source_impl *s = SPA_CONTAINER_OF(source, struct source_impl, source);
	int res;

	spa_assert(s->impl == object);
	spa_assert(source->func == source_io_func);

	spa_log_trace(impl->log, "%p: update %08x -> %08x", s, source->mask, mask);
	source->mask = mask;

	if (s->fallback)
		res = spa_loop_utils_enable_idle(&impl->utils, s->fallback,
				mask & (SPA_IO_IN | SPA_IO_OUT) ? true : false);
	else
		res = loop_update_source(object, source);
	return res;
}

static void source_idle_func(struct spa_source *source)
{
	struct source_impl *s = SPA_CONTAINER_OF(source, struct source_impl, source);
	s->func.idle(source->data);
}

static int loop_enable_idle(void *object, struct spa_source *source, bool enabled)
{
	struct source_impl *s = SPA_CONTAINER_OF(source, struct source_impl, source);
	int res = 0;

	spa_assert(s->impl == object);
	spa_assert(source->func == source_idle_func);

	if (enabled && !s->enabled) {
		if ((res = spa_system_eventfd_write(s->impl->system, source->fd, 1)) < 0)
			spa_log_warn(s->impl->log, "%p: failed to write idle fd %d: %s",
					source, source->fd, spa_strerror(res));
	} else if (!enabled && s->enabled) {
		uint64_t count;
		if ((res = spa_system_eventfd_read(s->impl->system, source->fd, &count)) < 0)
			spa_log_warn(s->impl->log, "%p: failed to read idle fd %d: %s",
					source, source->fd, spa_strerror(res));
	}
	s->enabled = enabled;
	return res;
}

static struct spa_source *loop_add_idle(void *object,
					bool enabled, spa_source_idle_func_t func, void *data)
{
	struct impl *impl = object;
	struct source_impl *source;
	int res;

	source = calloc(1, sizeof(struct source_impl));
	if (source == NULL)
		goto error_exit;

	if ((res = spa_system_eventfd_create(impl->system, SPA_FD_CLOEXEC | SPA_FD_NONBLOCK)) < 0)
		goto error_exit_free;

	source->source.loop = &impl->loop;
	source->source.func = source_idle_func;
	source->source.data = data;
	source->source.fd = res;
	source->impl = impl;
	source->close = true;
	source->source.mask = SPA_IO_IN;
	source->func.idle = func;

	if ((res = loop_add_source(impl, &source->source)) < 0)
		goto error_exit_close;

	spa_list_insert(&impl->source_list, &source->link);

	if (enabled)
		loop_enable_idle(impl, &source->source, true);

	return &source->source;

error_exit_close:
	spa_system_close(impl->system, source->source.fd);
error_exit_free:
	free(source);
	errno = -res;
error_exit:
	return NULL;
}

static void source_event_func(struct spa_source *source)
{
	struct source_impl *s = SPA_CONTAINER_OF(source, struct source_impl, source);
	uint64_t count = 0;
	int res;

	if ((res = spa_system_eventfd_read(s->impl->system, source->fd, &count)) < 0)
		spa_log_warn(s->impl->log, "%p: failed to read event fd %d: %s",
				source, source->fd, spa_strerror(res));

	s->func.event(source->data, count);
}

static struct spa_source *loop_add_event(void *object,
					 spa_source_event_func_t func, void *data)
{
	struct impl *impl = object;
	struct source_impl *source;
	int res;

	source = calloc(1, sizeof(struct source_impl));
	if (source == NULL)
		goto error_exit;

	if ((res = spa_system_eventfd_create(impl->system, SPA_FD_CLOEXEC | SPA_FD_NONBLOCK)) < 0)
		goto error_exit_free;

	source->source.loop = &impl->loop;
	source->source.func = source_event_func;
	source->source.data = data;
	source->source.fd = res;
	source->source.mask = SPA_IO_IN;
	source->impl = impl;
	source->close = true;
	source->func.event = func;

	if ((res = loop_add_source(impl, &source->source)) < 0)
		goto error_exit_close;

	spa_list_insert(&impl->source_list, &source->link);

	return &source->source;

error_exit_close:
	spa_system_close(impl->system, source->source.fd);
error_exit_free:
	free(source);
	errno = -res;
error_exit:
	return NULL;
}

static int loop_signal_event(void *object, struct spa_source *source)
{
	struct source_impl *s = SPA_CONTAINER_OF(source, struct source_impl, source);
	int res;

	spa_assert(s->impl == object);
	spa_assert(source->func == source_event_func);

	if (SPA_UNLIKELY((res = spa_system_eventfd_write(s->impl->system, source->fd, 1)) < 0))
		spa_log_warn(s->impl->log, "%p: failed to write event fd %d: %s",
				source, source->fd, spa_strerror(res));
	return res;
}

static void source_timer_func(struct spa_source *source)
{
	struct source_impl *s = SPA_CONTAINER_OF(source, struct source_impl, source);
	uint64_t expirations = 0;
	int res;

	if (SPA_UNLIKELY((res = spa_system_timerfd_read(s->impl->system,
				source->fd, &expirations)) < 0))
		spa_log_warn(s->impl->log, "%p: failed to read timer fd %d: %s",
				source, source->fd, spa_strerror(res));

	s->func.timer(source->data, expirations);
}

static struct spa_source *loop_add_timer(void *object,
					 spa_source_timer_func_t func, void *data)
{
	struct impl *impl = object;
	struct source_impl *source;
	int res;

	source = calloc(1, sizeof(struct source_impl));
	if (source == NULL)
		goto error_exit;

	if ((res = spa_system_timerfd_create(impl->system, CLOCK_MONOTONIC,
			SPA_FD_CLOEXEC | SPA_FD_NONBLOCK)) < 0)
		goto error_exit_free;

	source->source.loop = &impl->loop;
	source->source.func = source_timer_func;
	source->source.data = data;
	source->source.fd = res;
	source->source.mask = SPA_IO_IN;
	source->impl = impl;
	source->close = true;
	source->func.timer = func;

	if ((res = loop_add_source(impl, &source->source)) < 0)
		goto error_exit_close;

	spa_list_insert(&impl->source_list, &source->link);

	return &source->source;

error_exit_close:
	spa_system_close(impl->system, source->source.fd);
error_exit_free:
	free(source);
	errno = -res;
error_exit:
	return NULL;
}

static int
loop_update_timer(void *object, struct spa_source *source,
		  struct timespec *value, struct timespec *interval, bool absolute)
{
	struct source_impl *s = SPA_CONTAINER_OF(source, struct source_impl, source);
	struct itimerspec its;
	int flags = 0, res;

	spa_assert(s->impl == object);
	spa_assert(source->func == source_timer_func);

	spa_zero(its);
	if (SPA_LIKELY(value)) {
		its.it_value = *value;
	} else if (interval) {
		its.it_value = *interval;
		absolute = true;
	}
	if (SPA_UNLIKELY(interval))
		its.it_interval = *interval;
	if (SPA_LIKELY(absolute))
		flags |= SPA_FD_TIMER_ABSTIME;

	if (SPA_UNLIKELY((res = spa_system_timerfd_settime(s->impl->system, source->fd, flags, &its, NULL)) < 0))
		return res;

	return 0;
}

static void source_signal_func(struct spa_source *source)
{
	struct source_impl *s = SPA_CONTAINER_OF(source, struct source_impl, source);
	int res, signal_number = 0;

	if ((res = spa_system_signalfd_read(s->impl->system, source->fd, &signal_number)) < 0)
		spa_log_warn(s->impl->log, "%p: failed to read signal fd %d: %s",
				source, source->fd, spa_strerror(res));

	s->func.signal(source->data, signal_number);
}

static struct spa_source *loop_add_signal(void *object,
					  int signal_number,
					  spa_source_signal_func_t func, void *data)
{
	struct impl *impl = object;
	struct source_impl *source;
	int res;

	source = calloc(1, sizeof(struct source_impl));
	if (source == NULL)
		goto error_exit;

	if ((res = spa_system_signalfd_create(impl->system,
			signal_number, SPA_FD_CLOEXEC | SPA_FD_NONBLOCK)) < 0)
		goto error_exit_free;

	source->source.loop = &impl->loop;
	source->source.func = source_signal_func;
	source->source.data = data;
	source->source.fd = res;
	source->source.mask = SPA_IO_IN;
	source->impl = impl;
	source->close = true;
	source->func.signal = func;

	if ((res = loop_add_source(impl, &source->source)) < 0)
		goto error_exit_close;

	spa_list_insert(&impl->source_list, &source->link);

	return &source->source;

error_exit_close:
	spa_system_close(impl->system, source->source.fd);
error_exit_free:
	free(source);
	errno = -res;
error_exit:
	return NULL;
}

static void loop_destroy_source(void *object, struct spa_source *source)
{
	struct source_impl *s = SPA_CONTAINER_OF(source, struct source_impl, source);

	spa_assert(s->impl == object);

	spa_log_trace(s->impl->log, "%p ", s);

	spa_list_remove(&s->link);

	if (s->fallback)
		loop_destroy_source(s->impl, s->fallback);
	else if (source->loop)
		loop_remove_source(s->impl, source);

	if (source->fd != -1 && s->close) {
		spa_system_close(s->impl->system, source->fd);
		source->fd = -1;
	}
	spa_list_insert(&s->impl->destroy_list, &s->link);
}

static const struct spa_loop_methods impl_loop = {
	SPA_VERSION_LOOP_METHODS,
	.add_source = loop_add_source,
	.update_source = loop_update_source,
	.remove_source = loop_remove_source,
	.invoke = loop_invoke,
};

static const struct spa_loop_control_methods impl_loop_control = {
	SPA_VERSION_LOOP_CONTROL_METHODS,
	.get_fd = loop_get_fd,
	.add_hook = loop_add_hook,
	.enter = loop_enter,
	.leave = loop_leave,
	.iterate = loop_iterate,
};

static const struct spa_loop_utils_methods impl_loop_utils = {
	SPA_VERSION_LOOP_UTILS_METHODS,
	.add_io = loop_add_io,
	.update_io = loop_update_io,
	.add_idle = loop_add_idle,
	.enable_idle = loop_enable_idle,
	.add_event = loop_add_event,
	.signal_event = loop_signal_event,
	.add_timer = loop_add_timer,
	.update_timer = loop_update_timer,
	.add_signal = loop_add_signal,
	.destroy_source = loop_destroy_source,
};

static int impl_get_interface(struct spa_handle *handle, const char *type, void **interface)
{
	struct impl *impl;

	spa_return_val_if_fail(handle != NULL, -EINVAL);
	spa_return_val_if_fail(interface != NULL, -EINVAL);

	impl = (struct impl *) handle;

	if (spa_streq(type, SPA_TYPE_INTERFACE_Loop))
		*interface = &impl->loop;
	else if (spa_streq(type, SPA_TYPE_INTERFACE_LoopControl))
		*interface = &impl->control;
	else if (spa_streq(type, SPA_TYPE_INTERFACE_LoopUtils))
		*interface = &impl->utils;
	else
		return -ENOENT;

	return 0;
}

static int impl_clear(struct spa_handle *handle)
{
	struct impl *impl;
	struct source_impl *source;

	spa_return_val_if_fail(handle != NULL, -EINVAL);

	impl = (struct impl *) handle;

	if (impl->enter_count != 0)
		spa_log_warn(impl->log, "%p: loop is entered %d times",
				impl, impl->enter_count);

	spa_list_consume(source, &impl->source_list, link)
		loop_destroy_source(impl, &source->source);

	process_destroy(impl);

	spa_system_close(impl->system, impl->ack_fd);
	spa_system_close(impl->system, impl->poll_fd);

	return 0;
}

static size_t
impl_get_size(const struct spa_handle_factory *factory,
	      const struct spa_dict *params)
{
	return sizeof(struct impl);
}

static int
impl_init(const struct spa_handle_factory *factory,
	  struct spa_handle *handle,
	  const struct spa_dict *info,
	  const struct spa_support *support,
	  uint32_t n_support)
{
	struct impl *impl;
	int res;

	spa_return_val_if_fail(factory != NULL, -EINVAL);
	spa_return_val_if_fail(handle != NULL, -EINVAL);

	handle->get_interface = impl_get_interface;
	handle->clear = impl_clear;

	impl = (struct impl *) handle;
	impl->loop.iface = SPA_INTERFACE_INIT(
			SPA_TYPE_INTERFACE_Loop,
			SPA_VERSION_LOOP,
			&impl_loop, impl);
	impl->control.iface = SPA_INTERFACE_INIT(
			SPA_TYPE_INTERFACE_LoopControl,
			SPA_VERSION_LOOP_CONTROL,
			&impl_loop_control, impl);
	impl->utils.iface = SPA_INTERFACE_INIT(
			SPA_TYPE_INTERFACE_LoopUtils,
			SPA_VERSION_LOOP_UTILS,
			&impl_loop_utils, impl);

	impl->log = spa_support_find(support, n_support, SPA_TYPE_INTERFACE_Log);
	spa_log_topic_init(impl->log, &log_topic);
	impl->system = spa_support_find(support, n_support, SPA_TYPE_INTERFACE_System);

	if (impl->system == NULL) {
		spa_log_error(impl->log, "%p: a System is needed", impl);
		res = -EINVAL;
		goto error_exit;
	}

	if ((res = spa_system_pollfd_create(impl->system, SPA_FD_CLOEXEC)) < 0) {
		spa_log_error(impl->log, "%p: can't create pollfd: %s",
				impl, spa_strerror(res));
		goto error_exit;
	}
	impl->poll_fd = res;

	spa_list_init(&impl->source_list);
	spa_list_init(&impl->destroy_list);
	spa_hook_list_init(&impl->hooks_list);

	impl->buffer_data = SPA_PTR_ALIGN(impl->buffer_mem, MAX_ALIGN, uint8_t);
	spa_ringbuffer_init(&impl->buffer);

	impl->wakeup = loop_add_event(impl, wakeup_func, impl);
	if (impl->wakeup == NULL) {
		res = -errno;
		spa_log_error(impl->log, "%p: can't create wakeup event: %m", impl);
		goto error_exit_free_poll;
	}
	if ((res = spa_system_eventfd_create(impl->system,
			SPA_FD_EVENT_SEMAPHORE | SPA_FD_CLOEXEC)) < 0) {
		spa_log_error(impl->log, "%p: can't create ack event: %s",
				impl, spa_strerror(res));
		goto error_exit_free_wakeup;
	}
	impl->ack_fd = res;

	spa_log_debug(impl->log, "%p: initialized", impl);

	return 0;

error_exit_free_wakeup:
	loop_destroy_source(impl, impl->wakeup);
error_exit_free_poll:
	spa_system_close(impl->system, impl->poll_fd);
error_exit:
	return res;
}

static const struct spa_interface_info impl_interfaces[] = {
	{SPA_TYPE_INTERFACE_Loop,},
	{SPA_TYPE_INTERFACE_LoopControl,},
	{SPA_TYPE_INTERFACE_LoopUtils,},
};

static int
impl_enum_interface_info(const struct spa_handle_factory *factory,
			 const struct spa_interface_info **info,
			 uint32_t *index)
{
	spa_return_val_if_fail(factory != NULL, -EINVAL);
	spa_return_val_if_fail(info != NULL, -EINVAL);
	spa_return_val_if_fail(index != NULL, -EINVAL);

	if (*index >= SPA_N_ELEMENTS(impl_interfaces))
		return 0;

	*info = &impl_interfaces[(*index)++];
	return 1;
}

const struct spa_handle_factory spa_support_loop_factory = {
	SPA_VERSION_HANDLE_FACTORY,
	SPA_NAME_SUPPORT_LOOP,
	NULL,
	impl_get_size,
	impl_init,
	impl_enum_interface_info
};
