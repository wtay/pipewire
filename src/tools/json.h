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

#ifndef PIPEWIRE_JSON_H
#define PIPEWIRE_JSON_H

#ifdef __cplusplus
extern "C" {
#endif

#include "ot.h"

static inline int ot_json_dump2(struct ot_node *node, int l0, int l1)
{
#define OUT(fmt,...)		printf(fmt, ##__VA_ARGS__)
#define IND(fmt,level,...)	printf("%*s"fmt, (level)*2, "", ##__VA_ARGS__)
	if (node->k) {
		IND("\"%s\": ", l0, node->k);
		l0 = 0;
	}
	switch (node->type) {
	case OT_NULL:
		IND("null", l0);
		break;
	case OT_BOOL:
		IND("%s", l0, node->v.b ? "true" : "false");
		break;
	case OT_INT:
		IND("%d", l0, node->v.i);
		break;
	case OT_LONG:
		IND("%"PRIi64, l0, node->v.l);
		break;
	case OT_DOUBLE:
		IND("%f", l0, node->v.d);
		break;
	case OT_STRING:
		IND("\"%s\"", l0, node->v.s);
		break;
	case OT_ARRAY:
	case OT_OBJECT:
	{
		uint32_t i = 0;
		const char open_sep = node->type == OT_ARRAY ? '[' : '{';
		const char close_sep = node->type == OT_ARRAY ? ']' : '}';
		struct ot_node sub = { 0, };

		IND("%c", l0, open_sep);

		l0 = (node->flags & NODE_FLAG_FLAT) ? 0 : l1 + 1;

		while (ot_node_iterate(node, &sub)) {
			OUT("%s %s", i++ > 0 ? "," : "", l0 ? "\n" : "");
			ot_json_dump2(&sub, l0, l1+1);
		}
		if (l0 && i > 0) {
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

static inline int ot_json_dump(struct ot_node *node, int level)
{
	return ot_json_dump2(node, level, level);
}

#ifdef __cplusplus
}
#endif

#endif /* PIPEWIRE_JSON_H */
