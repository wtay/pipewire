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

#ifndef PIPEWIRE_PATH_H
#define PIPEWIRE_PATH_H

#ifdef __cplusplus
extern "C" {
#endif

#include "ot.h"

enum ot_match_type {
	OT_MATCH_DEEP,
	OT_MATCH_SLICE,
	OT_MATCH_INDEXES,
	OT_MATCH_KEY,
	OT_MATCH_KEYS,
};

union ot_match {
	struct {
		int32_t start;
		int32_t end;
		int32_t step;
	} slice;
	const char *key;
	struct {
		int32_t *indexes;
		int32_t n_indexes;
	} indexes;
	struct {
		const char **keys;
		int32_t n_keys;
	} keys;
};

struct ot_step {
	enum ot_match_type type;
	union ot_match match;
	struct ot_step *parent;
	struct ot_node *node;
	struct ot_key key;
	struct ot_node current;
	int (*filter) (struct ot_step *path);
	void *data;
};

#define OT_INIT_MATCH_ALL() (struct ot_step){ .type = OT_MATCH_SLICE, .match.slice = { 0, -1, 1 } }
#define OT_INIT_MATCH_INDEX(i) (struct ot_step){ .type = OT_MATCH_SLICE, .match.slice = { (i), (i)+1, 1 } }
#define OT_INIT_MATCH_INDEXES(idx,n_idx) (struct ot_step){ .type = OT_MATCH_INDEXES, .match.indexes = { idx, n_idx } }
#define OT_INIT_MATCH_SLICE(s,e,st) (struct ot_step){ .type = OT_MATCH_SLICE, .match.slice = { s, e, st } }
#define OT_INIT_MATCH_KEY(k) (struct ot_step){ .type = OT_MATCH_KEY, .match.key = k }
#define OT_INIT_MATCH_KEYS(keys,n_keys) (struct ot_step){ .type = OT_MATCH_KEYS, .match.keys = { keys, n_keys } }

int ot_path_begin(struct ot_node *root, struct ot_step *steps, uint32_t n_steps, struct ot_node *result);

void ot_path_end(struct ot_node *result);

#ifdef __cplusplus
}
#endif

#endif /* PIPEWIRE_PATH_H */
