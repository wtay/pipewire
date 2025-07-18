/* Simple Plugin API */
/* SPDX-FileCopyrightText: Copyright © 2018 Wim Taymans */
/* SPDX-License-Identifier: MIT */

#ifndef SPA_POD_DYNAMIC_H
#define SPA_POD_DYNAMIC_H

#include <spa/pod/builder.h>
#include <spa/utils/cleanup.h>

#ifdef __cplusplus
extern "C" {
#endif

#ifndef SPA_API_POD_DYNAMIC
 #ifdef SPA_API_IMPL
  #define SPA_API_POD_DYNAMIC SPA_API_IMPL
 #else
  #define SPA_API_POD_DYNAMIC static inline
 #endif
#endif

struct spa_pod_dynamic_builder {
	struct spa_pod_builder b;
	void *data;
	uint32_t extend;
	uint32_t _padding;
};

static int spa_pod_dynamic_builder_overflow(void *data, uint32_t size)
{
	struct spa_pod_dynamic_builder *d = (struct spa_pod_dynamic_builder*)data;
	int32_t old_size = d->b.size;
	int32_t new_size = SPA_ROUND_UP_N(size, d->extend);
	void *old_data = d->b.data, *new_data;

	if (old_data == d->data)
		d->b.data = NULL;
	if ((new_data = realloc(d->b.data, new_size)) == NULL)
		return -errno;
	if (old_data == d->data && new_data != old_data && old_size > 0)
		memcpy(new_data, old_data, old_size);
	d->b.data = new_data;
	d->b.size = new_size;
        return 0;
}

SPA_API_POD_DYNAMIC void spa_pod_dynamic_builder_init(struct spa_pod_dynamic_builder *builder,
		void *data, uint32_t size, uint32_t extend)
{
	static const struct spa_pod_builder_callbacks spa_pod_dynamic_builder_callbacks = {
		.version = SPA_VERSION_POD_BUILDER_CALLBACKS,
		.overflow = spa_pod_dynamic_builder_overflow
	};
	builder->b = SPA_POD_BUILDER_INIT(data, size);
	if (extend > 0)
		spa_pod_builder_set_callbacks(&builder->b, &spa_pod_dynamic_builder_callbacks, builder);
	builder->extend = extend;
	builder->data = data;
}

SPA_API_POD_DYNAMIC void spa_pod_dynamic_builder_continue(struct spa_pod_dynamic_builder *builder,
		struct spa_pod_builder *b)
{
	uint32_t remain = b->state.offset >= b->size ? 0 : b->size - b->state.offset;
	spa_pod_dynamic_builder_init(builder,
			remain ? SPA_PTROFF(b->data, b->state.offset, void) : NULL,
			remain, b->callbacks.funcs == NULL ? 0 : 4096);
}

SPA_API_POD_DYNAMIC void spa_pod_dynamic_builder_clean(struct spa_pod_dynamic_builder *builder)
{
	if (builder->data != builder->b.data)
		free(builder->b.data);
}

SPA_DEFINE_AUTO_CLEANUP(spa_pod_dynamic_builder, struct spa_pod_dynamic_builder, {
	spa_pod_dynamic_builder_clean(thing);
})

#ifdef __cplusplus
}  /* extern "C" */
#endif

#endif /* SPA_POD_DYNAMIC_H */
