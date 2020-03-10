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

#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <spa/utils/defs.h>

#include "query.h"

struct context {
	struct ot_path *path;
	uint32_t n_path;
	int32_t depth;
	struct ot_node *root;
};

static int iterate_one(struct ot_node *node, struct ot_node *sub)
{
	struct context *ctx = node->extra[0].p;
	if (node->index < -1 || node->index > 0)
		return 0;
	*sub = *ctx->root;
	sub->k = NULL;
	return 1;
}

static void enter_path(struct context *ctx, struct ot_node *root, int32_t depth)
{
	struct ot_path *next;
	ctx->depth = depth;
	next = &ctx->path[depth];
	next->root = root;
	next->root->index = next->slice.start;
}

static int iterate_path(struct ot_node *node, struct ot_node *sub)
{
	struct context *ctx = node->extra[0].p;
	uint32_t depth;
	struct ot_path *path;
	struct ot_node *root;
	int32_t index;
	int res;
again:
	depth = ctx->depth;
	path = &ctx->path[depth];
	root = path->root;

	switch (path->type) {
	case OT_MATCH_DEEP:
		break;
	case OT_MATCH_SLICE:
		index = root->index;
		if ((path->slice.end >= 0 && index >= path->slice.end) ||
		    (path->slice.end < 0 && index <= path->slice.end))
			goto back;
		if ((res = ot_node_iterate(root, &path->current)) <= 0)
			goto back;
		root->index += path->slice.step;
		break;
	case OT_MATCH_KEY:
		if (root->type != OT_OBJECT)
			goto back;
		while (true) {
			if ((res = ot_node_iterate(root, &path->current)) <= 0)
				goto back;
			root->index++;
			if (strcmp(path->current.k, path->key) == 0)
				break;
		}
		break;
	default:
		goto back;
	}
	if (path->check != NULL &&
	    !path->check(path))
		goto again;

	if (++depth < ctx->n_path) {
		enter_path(ctx, &path->current, depth);
		goto again;
	} else {
		*sub = path->current;
		sub->k = NULL;
		return 1;
	}
back:
	if (ctx->depth == 0)
		return 0;
	ctx->depth--;
	goto again;
}

int ot_query_begin(struct ot_node *root, struct ot_path *path, uint32_t n_path, struct ot_node *result)
{
	struct context *ctx;

	if ((ctx = calloc(1, sizeof(*ctx))) == NULL)
		return -errno;

	ctx->path = path;
	ctx->n_path = n_path;
	ctx->root = root;

	*result = OT_INIT_ARRAY(NULL, n_path > 0 ? iterate_path : iterate_one);
	result->extra[0].p = ctx;

	if (n_path > 0)
		enter_path(ctx, root, 0);

	return 0;
}

void ot_query_end(struct ot_node *result)
{
	free(result->extra[0].p);
}
