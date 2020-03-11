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

#include "path.h"

#define NODE_CONTEXT(n)		((n)->extra[0].p)

struct context {
	struct ot_step *steps;
	uint32_t n_steps;
	int32_t depth;
	struct ot_node *root;
};

static int iterate_one(struct ot_node *node, struct ot_node *sub)
{
	struct context *ctx = NODE_CONTEXT(node);
	if (node->index < -1 || node->index > 0)
		return 0;
	*sub = *ctx->root;
	sub->k.val = NULL;
	return 1;
}

static void enter_step(struct context *ctx, struct ot_node *node, int32_t depth)
{
	struct ot_step *next;
	ctx->depth = depth;
	next = &ctx->steps[depth];
	next->parent = depth > 0 ? &ctx->steps[depth-1] : NULL;
	next->node = node;
	next->node->index = next->type == OT_MATCH_SLICE ? next->match.slice.start : 0;
}

static int iterate_steps(struct ot_node *n, struct ot_node *sub)
{
	struct context *ctx = NODE_CONTEXT(n);
	uint32_t depth;
	struct ot_step *step;
	struct ot_node *node;
	union ot_match *match, mtch;
	int32_t index;
	int res;
again:
	depth = ctx->depth;
	step = &ctx->steps[depth];
	node = step->node;
	match = &step->match;

	switch (step->type) {
	case OT_MATCH_DEEP:
		break;
	case OT_MATCH_SLICE:
		index = node->index;
		if ((match->slice.end >= 0 && index >= match->slice.end) ||
		    (match->slice.end < 0 && index <= match->slice.end))
			goto back;
		if ((res = ot_node_iterate(node, &step->current)) <= 0)
			goto back;
		node->index += match->slice.step;
		break;
	case OT_MATCH_INDEXES:
		do {
			index = node->index;
			if (index >= match->indexes.n_indexes)
				goto back;
			node->index = match->indexes.indexes[index];
			res = ot_node_iterate(node, &step->current);
			node->index = index + 1;
		} while (res <= 0);
		break;
	case OT_MATCH_KEY:
		mtch.keys.n_keys = 1;
		mtch.keys.keys = &match->key;
		match = &mtch;
		/* fallthrough */
	case OT_MATCH_KEYS:
		if (node->type != OT_OBJECT)
			goto back;
		do {
			index = node->index;
			if (index >= match->keys.n_keys)
				goto back;
			node->index = 0;
			do {
				if ((res = ot_node_iterate(node, &step->current)) <= 0)
					break;
				node->index++;
			} while (strncmp(step->current.k.val,
						match->keys.keys[index],
						step->current.k.len) != 0);
			node->index = index + 1;
		} while (res <= 0);
		break;
	default:
		goto back;
	}
	if (step->filter != NULL &&
	    !step->filter(step))
		goto again;

	if (++depth < ctx->n_steps) {
		enter_step(ctx, &step->current, depth);
		goto again;
	} else {
		*sub = step->current;
		sub->k.val = NULL;
		return 1;
	}
back:
	if (ctx->depth == 0)
		return 0;
	ctx->depth--;
	goto again;
}

int ot_path_begin(struct ot_node *root, struct ot_step *steps, uint32_t n_steps, struct ot_node *result)
{
	struct context *ctx;

	if ((ctx = calloc(1, sizeof(*ctx))) == NULL)
		return -errno;

	ctx->steps = steps;
	ctx->n_steps = n_steps;
	ctx->root = root;

	*result = OT_INIT_ARRAY(NULL, n_steps > 0 ? iterate_steps : iterate_one);
	NODE_CONTEXT(result) = ctx;

	if (n_steps > 0)
		enter_step(ctx, root, 0);

	return 0;
}

void ot_path_end(struct ot_node *result)
{
	free(NODE_CONTEXT(result));;
}
