/* PipeWire
 *
 * Copyright © 2020 Wim Taymans
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

#ifndef PIPEWIRE_TREE_H
#define PIPEWIRE_TREE_H

#ifdef __cplusplus
extern "C" {
#endif

#include "ot.h"

struct pw_tree;

struct pw_tree_events {
#define PW_VERSION_TREE_EVENTS		0
	uint32_t version;

	void (*added) (void *data, const char *path);

	void (*updated) (void *data, const char *path);

	void (*removed) (void *data, const char *path);
};

struct pw_tree * pw_tree_new(struct pw_core *core);

void pw_tree_add_listener(struct pw_tree *tree,
		struct spa_hook *listener,
		const struct pw_tree_events *events, void *data);

int pw_tree_get_root(struct pw_tree *tree, struct ot_node *node);

void pw_tree_destroy(struct pw_tree *tree);

#ifdef __cplusplus
}
#endif

#endif /* PIPEWIRE_TREE_H */
