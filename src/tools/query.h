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

#ifndef PIPEWIRE_QUERY_H
#define PIPEWIRE_QUERY_H

#ifdef __cplusplus
extern "C" {
#endif

#include "ot.h"

enum ot_match {
	OT_MATCH_DEEP,
	OT_MATCH_SLICE,
	OT_MATCH_KEY,
};

struct ot_path {
	enum ot_match type;
	struct {
		int32_t start;
		int32_t end;
		int32_t step;
	} slice;
	const char *key;
	struct ot_node *root;
	struct ot_node current;
	int (*check) (struct ot_path *path);
	void *data;
};

#define OT_INIT_MATCH_ALL() (struct ot_path){ .type = OT_MATCH_SLICE, .slice = { 0, -1, 1 } }
#define OT_INIT_MATCH_INDEX(i) (struct ot_path){ .type = OT_MATCH_SLICE, .slice = { i, i, 1 } }
#define OT_INIT_MATCH_SLICE(s,e,st) (struct ot_path){ .type = OT_MATCH_SLICE, .slice = { s, e, st } }
#define OT_INIT_MATCH_KEY(k) (struct ot_path){ .type = OT_MATCH_KEY, .slice = { 0, -1, 1 }, .key = k }

int ot_query_begin(struct ot_node *root, struct ot_path *path, uint32_t n_path, struct ot_node *result);

void ot_query_end(struct ot_node *result);

#ifdef __cplusplus
}
#endif

#endif /* PIPEWIRE_QUERY_H */
