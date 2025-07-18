/* PipeWire */
/* SPDX-FileCopyrightText: Copyright © 2018 Wim Taymans */
/* SPDX-License-Identifier: MIT */

#include <stdio.h>

#include <spa/support/loop.h>
#include <spa/utils/names.h>
#include <spa/utils/result.h>

#define PW_API_LOOP_IMPL	SPA_EXPORT
#include <pipewire/pipewire.h>
#include <pipewire/private.h>
#include <pipewire/loop.h>
#include <pipewire/log.h>
#include <pipewire/type.h>

PW_LOG_TOPIC_EXTERN(log_loop);
#define PW_LOG_TOPIC_DEFAULT log_loop

/** \cond */

struct impl {
	struct pw_loop this;

	char name[16];

	struct spa_handle *system_handle;
	struct spa_handle *loop_handle;

	void *user_data;
	const struct pw_loop_callbacks *cb;
};
/** \endcond */

/** Create a new loop
 * \returns a newly allocated loop
 */
SPA_EXPORT
struct pw_loop *pw_loop_new(const struct spa_dict *props)
{
	int res;
	struct impl *impl;
	struct pw_loop *this;
	void *iface;
	struct spa_support support[32];
	uint32_t n_support;
	const char *lib, *str, *name = NULL;

	n_support = pw_get_support(support, 32);

	impl = calloc(1, sizeof(struct impl));
	if (impl == NULL) {
		res = -errno;
		goto error_cleanup;
	}

	this = &impl->this;

	if (props)
		lib = spa_dict_lookup(props, PW_KEY_LIBRARY_NAME_SYSTEM);
	else
		lib = NULL;

	impl->system_handle = pw_load_spa_handle(lib,
			SPA_NAME_SUPPORT_SYSTEM,
			props, n_support, support);
	if (impl->system_handle == NULL) {
		res = -errno;
		pw_log_error("%p: can't make "SPA_NAME_SUPPORT_SYSTEM" handle: %m", this);
		goto error_free;
	}

        if ((res = spa_handle_get_interface(impl->system_handle,
					    SPA_TYPE_INTERFACE_System,
					    &iface)) < 0) {
                pw_log_error("%p: can't get System interface: %s", this, spa_strerror(res));
                goto error_unload_system;
	}
	this->system = iface;

	support[n_support++] = SPA_SUPPORT_INIT(SPA_TYPE_INTERFACE_System, iface);

	if (props)
		lib = spa_dict_lookup(props, PW_KEY_LIBRARY_NAME_LOOP);
	else
		lib = NULL;

	impl->loop_handle = pw_load_spa_handle(lib,
			SPA_NAME_SUPPORT_LOOP, props,
			n_support, support);
	if (impl->loop_handle == NULL) {
		res = -errno;
		pw_log_error("%p: can't make "SPA_NAME_SUPPORT_LOOP" handle: %m", this);
		goto error_unload_system;
	}

        if ((res = spa_handle_get_interface(impl->loop_handle,
					    SPA_TYPE_INTERFACE_Loop,
					    &iface)) < 0) {
		pw_log_error("%p: can't get Loop interface: %s",
				this, spa_strerror(res));
                goto error_unload_loop;
        }
	this->loop = iface;

        if ((res = spa_handle_get_interface(impl->loop_handle,
					    SPA_TYPE_INTERFACE_LoopControl,
					    &iface)) < 0) {
		pw_log_error("%p: can't get LoopControl interface: %s",
				this, spa_strerror(res));
                goto error_unload_loop;
        }
	this->control = iface;
	if (!spa_interface_callback_check(&this->control->iface,
			struct spa_loop_control_methods, iterate, 0)) {
		res = -EINVAL;
		pw_log_error("%p: loop does not support iterate", this);
		goto error_unload_loop;
	}

        if ((res = spa_handle_get_interface(impl->loop_handle,
					    SPA_TYPE_INTERFACE_LoopUtils,
					    &iface)) < 0) {
		pw_log_error("%p: can't get LoopUtils interface: %s",
				this, spa_strerror(res));
                goto error_unload_loop;
        }
	this->utils = iface;

	if (props != NULL) {
		if ((str = spa_dict_lookup(props, PW_KEY_LOOP_NAME)) != NULL)
			name = str;
	}
	if (name)
		snprintf(impl->name, sizeof(impl->name), "%s", name);
	this->name = impl->name;

	return this;

error_unload_loop:
	pw_unload_spa_handle(impl->loop_handle);
error_unload_system:
	pw_unload_spa_handle(impl->system_handle);
error_free:
	free(impl);
error_cleanup:
	errno = -res;
	return NULL;
}

/** Destroy a loop
 * \param loop a loop to destroy
 */
SPA_EXPORT
void pw_loop_destroy(struct pw_loop *loop)
{
	struct impl *impl = SPA_CONTAINER_OF(loop, struct impl, this);

	pw_unload_spa_handle(impl->loop_handle);
	pw_unload_spa_handle(impl->system_handle);
	free(impl);
}

void
pw_loop_set_callbacks(struct pw_loop *loop, const struct pw_loop_callbacks *cb, void *data)
{
	struct impl *impl = SPA_CONTAINER_OF(loop, struct impl, this);

	impl->user_data = data;
	impl->cb = cb;
}

SPA_EXPORT
int pw_loop_set_name(struct pw_loop *loop, const char *name)
{
	struct impl *impl = SPA_CONTAINER_OF(loop, struct impl, this);
	snprintf(impl->name, sizeof(impl->name), "%s", name);
	return 0;
}
