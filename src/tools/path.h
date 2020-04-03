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

#include <regex.h>

#include "ot.h"

struct ot_expr;

enum ot_match_type {
	OT_MATCH_DEEP,
	OT_MATCH_SLICE,
	OT_MATCH_INDEX,
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
	int32_t index;
	struct {
		int32_t *indexes;
		int32_t n_indexes;
	} indexes;
	struct ot_string key;
	struct {
		struct ot_string *keys;
		int32_t n_keys;
	} keys;
};

struct ot_step {
	enum ot_match_type type;
	uint32_t flags;
	union ot_match match;
	struct ot_node *node;
	struct ot_key key;
	struct ot_node current;
	struct ot_expr *expr;
	void *data;
	union ot_val extra[8];
};

#define OT_INIT_MATCH_DEEP() (struct ot_step){ .type = OT_MATCH_DEEP, }
#define OT_INIT_MATCH_ALL() (struct ot_step){ .type = OT_MATCH_SLICE, .match.slice = { 0, -1, 1 } }
#define OT_INIT_MATCH_INDEX(i) (struct ot_step){ .type = OT_MATCH_INDEX, .match.index = i }
#define OT_INIT_MATCH_INDEXES(idx,n_idx) (struct ot_step){ .type = OT_MATCH_INDEXES, .match.indexes = { idx, n_idx } }
#define OT_INIT_MATCH_SLICE(s,e,st) (struct ot_step){ .type = OT_MATCH_SLICE, .match.slice = { s, e, st } }
#define OT_INIT_MATCH_KEYN(k,n) (struct ot_step){ .type = OT_MATCH_KEY, .match.key.val = k, .match.key.len = n }
#define OT_INIT_MATCH_KEY(k) OT_INIT_MATCH_KEYN(k,-1)
#define OT_INIT_MATCH_KEYS(keys,n_keys) (struct ot_step){ .type = OT_MATCH_KEYS, .match.keys = { keys, n_keys } }

enum ot_expr_type {
	OT_EXPR_EQ,
	OT_EXPR_NEQ,
	OT_EXPR_LT,
	OT_EXPR_LTE,
	OT_EXPR_GT,
	OT_EXPR_GTE,

	OT_EXPR_EXP,
	OT_EXPR_MULT,
	OT_EXPR_DIV,
	OT_EXPR_MOD,
	OT_EXPR_ADD,
	OT_EXPR_SUB,

	OT_EXPR_AND,
	OT_EXPR_OR,
	OT_EXPR_NOT,

	OT_EXPR_NODE,
	OT_EXPR_PATH,
	OT_EXPR_FUNC,
	OT_EXPR_REGEX,
};

struct ot_expr {
	enum ot_expr_type type;

	struct ot_expr *child[4];
	int n_child;

	union {
		struct ot_node node;
		struct {
			struct ot_step steps[16];
			uint32_t n_steps;
		} path;
		int (*func) (struct ot_step *path, struct ot_node *result);
		regex_t regex;
	};
	union ot_val extra[8];
};

int ot_path_iterate(struct ot_node *root, struct ot_step *steps, uint32_t n_steps, struct ot_node *result);

#ifdef __cplusplus
}
#endif

#endif /* PIPEWIRE_PATH_H */
