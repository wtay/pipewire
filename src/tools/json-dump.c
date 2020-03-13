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

#include <unistd.h>
#include <stdio.h>

#include "json-dump.h"

struct ot_json_ctx {
	FILE *out;
	int l0, l1;
	int expensive;
	int cutoff;
	unsigned int colors:1;
};

#define NORMAL	(ctx->colors?"\x1B[0m":"")
#define NUL	(ctx->colors?"\x1B[95m":"")
#define BOOL	(ctx->colors?"\x1B[95m":"")
#define NUMBER	(ctx->colors?"\x1B[96m":"")
#define STRING	(ctx->colors?"\x1B[92m":"")
#define KEY	(ctx->colors?"\x1B[94m":"")

static inline int ot_json_dump2(struct ot_node *node, struct ot_json_ctx *ctx)
{
	int l0 = ctx->l0, l1 = ctx->l1;
#define OUT(fmt,...)		fprintf(ctx->out, fmt, ##__VA_ARGS__)
#define IND(fmt,level,...)	fprintf(ctx->out, "%*s"fmt, (level)*2, "", ##__VA_ARGS__)
	if (node->k.val) {
		IND("%s\"%.*s\"%s: ", l0, KEY, node->k.len, node->k.val, NORMAL);
		l0 = 0;
	}
	switch (node->type) {
	case OT_NULL:
		IND("%snull%s", l0, NUL, NORMAL);
		break;
	case OT_BOOL:
		IND("%s%s%s", l0, BOOL, node->v.b ? "true" : "false", NORMAL);
		break;
	case OT_INT:
		IND("%s%d%s", l0, NUMBER, node->v.i, NORMAL);
		break;
	case OT_LONG:
		IND("%s%"PRIi64"%s", l0, NUMBER, node->v.l, NORMAL);
		break;
	case OT_FLOAT:
		IND("%s%f%s", l0, NUMBER, node->v.f, NORMAL);
		break;
	case OT_DOUBLE:
		IND("%s%f%s", l0, NUMBER, node->v.d, NORMAL);
		break;
	case OT_STRING:
		IND("%s\"%.*s\"%s", l0, STRING, node->v.s.len, node->v.s.val, NORMAL);
		break;
	case OT_ARRAY:
	case OT_OBJECT:
	{
		uint32_t i = 0;
		const char open_sep = node->type == OT_ARRAY ? '[' : '{';
		const char close_sep = node->type == OT_ARRAY ? ']' : '}';
		struct ot_node sub = { 0, };
		struct ot_key key = { 0, };

		IND("%c", l0, open_sep);

		if (node->flags & NODE_FLAG_EXPENSIVE)
			ctx->expensive++;

		if (ctx->expensive <= ctx->cutoff) {
			ctx->l1++;
			l0 = ctx->l0;
			ctx->l0 = (node->flags & NODE_FLAG_FLAT) ? 0 : ctx->l1;

			key.index = 0;
			while (ot_node_iterate(node, &key, &sub)) {
				OUT("%s %s", i++ > 0 ? "," : "", ctx->l0 ? "\n" : "");
				ot_json_dump2(&sub, ctx);
				key.index++;
			}
			ctx->l1--;
			ctx->l0 = l0;
		}

		if (node->flags & NODE_FLAG_EXPENSIVE)
			ctx->expensive--;

		if (!(node->flags & NODE_FLAG_FLAT) && i > 0) {
			OUT("\n");
			IND("%c", l1, close_sep);
		} else {
			OUT("%s%c", i > 0 ? " " : "", close_sep);
		}
		break;
	}
	default:
		break;
	}
	return 0;
#undef IND
#undef OUT
}

int ot_json_dump(FILE *out, struct ot_node *node, int cutoff)
{
	struct ot_json_ctx ctx = { .out = out, 0, 0, 0, cutoff };
	if (isatty(fileno(out)))
		ctx.colors = true;
	return ot_json_dump2(node, &ctx);
}
