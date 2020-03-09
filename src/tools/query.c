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

#include "query.h"

struct context {
	struct ot_node *root;
	char *query;
};

static int result_iterate(struct ot_node *node, struct ot_node *sub)
{
	struct context *ctx = node->extra[0].p;
	int index;

	if ((index = node->index) < 0)
		index += 1;
	if (index < 0 || index > 0)
		return 0;

	*sub = *ctx->root;
	return 1;
}

int ot_query_begin(struct ot_node *root, const char *query, struct ot_node *result)
{
	struct context *ctx;

	if ((ctx = calloc(1, sizeof(*ctx))) == NULL)
		return -errno;

	ctx->root = root;
	ctx->query = strdup(query);

	*result = OT_INIT_ARRAY(NULL, result_iterate);
	result->extra[0].p = ctx;

	return 0;
}

void ot_query_end(struct ot_node *result)
{
	free(result->extra[0].p);
}
