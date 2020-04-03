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
#include <math.h>

#include <spa/utils/defs.h>

#include "path.h"
#include "json-dump.h"

#define NODE_STEPS(n)		((n)->extra[0].p)
#define NODE_GET_STEP(n,d)	(&((struct ot_step*)NODE_STEPS(n))[d])
#define NODE_N_STEPS(n)		((n)->extra[1].i)
#define NODE_DEPTH(n)		((n)->extra[2].i)
#define NODE_ROOT(n)		((n)->extra[3].p)
#define NODE_GET_ROOT(n)	((struct ot_node*)NODE_ROOT(n))
#define NODE_DEEP(n)		((n)->extra[4].p)
#define DEEP_PARENT(d)		((d)->extra[0].p)

#define NODE_IS_NULL(n)		((n)->type == OT_NULL)
#define NODE_IS_BOOL(n)		((n)->type == OT_BOOL)
#define NODE_IS_STRING(n)	((n)->type == OT_STRING)
#define NODE_IS_NUMBER(n)	((n)->type == OT_NUMBER)

static int ot_string_cmp(struct ot_string *s1, struct ot_string *s2)
{
	int res;
	if (s1->len == -1)
		s1->len = strlen(s1->val);
	if (s2->len == -1)
		s2->len = strlen(s2->val);
	if ((res = strncmp(s1->val, s2->val, SPA_MIN(s1->len, s2->len))) != 0)
		return res;
	return s1->len - s2->len;
}

static double node_number(struct ot_node *node)
{
	char *end;
	double res;
	switch (node->type) {
	case OT_NULL:
		return 0.0;
	case OT_BOOL:
		return node->v.b ? 1.0 : 0.0;
	case OT_NUMBER:
		return node->v.d;
	case OT_STRING:
		if (node->v.s.val == NULL || node->v.s.len == 0 ||
		    ((res = strtod(node->v.s.val, &end)) == 0.0 && node->v.s.val == end))
			return NAN;
		return res;
	case OT_ARRAY:
	case OT_OBJECT:
		return NAN;
	}
	return NAN;
}

static int node_boolean(struct ot_node *node)
{
	switch (node->type) {
	case OT_NULL:
		return 0;
	case OT_BOOL:
		return node->v.b;
	case OT_NUMBER:
		return node->v.d != 0.0 && !isnan(node->v.d);
	case OT_STRING:
		return node->v.s.val != NULL && node->v.s.len != 0;
	case OT_ARRAY:
		if (node->flags & NODE_FLAG_MULTI) {
			struct ot_key key = { 0, };
			struct ot_node child;
			if (ot_node_iterate(node, &key, &child) <= 0)
				return 0;
		}
		/* fallthrough */
	case OT_OBJECT:
		return 1;
	}
	return 0;
}

static int node_compare(struct ot_node *child, int n_child, enum ot_expr_type type)
{
#define CMP_EQ_U	2
#define CMP_NEQ_U	-2
#define CMP_EQ		0
#define CMP_LT		-1
#define CMP_GT		1
	int cmp;

	if (n_child < 2)
		return 0;

	if (NODE_IS_NULL(&child[0]) || NODE_IS_NULL(&child[1])) {
		cmp = NODE_IS_NULL(&child[0]) && NODE_IS_NULL(&child[1]) ? CMP_EQ_U : CMP_NEQ_U;
	} else if (NODE_IS_STRING(&child[0]) && NODE_IS_STRING(&child[1])) {
		cmp = ot_string_cmp(&child[0].v.s, &child[1].v.s);
		cmp = cmp == 0 ? CMP_EQ : cmp < 0 ? CMP_LT : CMP_GT;
	} else if (NODE_IS_NUMBER(&child[0]) || NODE_IS_NUMBER(&child[1])) {
		double l = node_number(&child[0]), r = node_number(&child[1]);
		if (isnan(l) || isnan(r))
			cmp = CMP_NEQ_U;
		else
			cmp = l == r ? CMP_EQ : l < r ? CMP_LT : CMP_GT;
	} else {
		int l = node_boolean(&child[0]), r = node_boolean(&child[1]);
		cmp = l == r ? CMP_EQ_U : CMP_NEQ_U;
	}

	switch (type) {
	case OT_EXPR_EQ:
		return cmp == CMP_EQ || cmp == CMP_EQ_U;
	case OT_EXPR_NEQ:
		return cmp != CMP_EQ && cmp != CMP_EQ_U;
	case OT_EXPR_LT:
		return cmp == CMP_LT;
	case OT_EXPR_LTE:
		return cmp == CMP_LT || cmp == CMP_EQ;
	case OT_EXPR_GT:
		return cmp == CMP_GT;
	case OT_EXPR_GTE:
		return cmp == CMP_GT || cmp == CMP_EQ;
	default:
		break;
	}
	return 0;
}

static int node_logic(struct ot_node *child, int n_child, enum ot_expr_type type)
{
	int i, b[n_child];

	for (i = 0; i < n_child; i++)
		b[i] = node_boolean(&child[i]);

	switch (type) {
	case OT_EXPR_AND:
		return n_child >= 2 && b[0] && b[1];
	case OT_EXPR_OR:
		return n_child >= 2 && (b[0] || b[1]);
	case OT_EXPR_NOT:
		return n_child >= 1 && !b[0];
	default:
		break;
	}
	return 0;
}
static int node_regex(struct ot_node *child, int n_child, enum ot_expr_type type)
{
	if (n_child == 2 && NODE_IS_STRING(&child[0]) && NODE_IS_STRING(&child[1])) {
		regex_t *regex = child[1].extra[0].p;
		if (regex) {
			int len = child[0].v.s.len;
			char val[len+1];
			const char *v = child[0].v.s.val;

			if (len != -1) {
				memcpy(val, v, len);
				val[len] = '\0';
				v = val;
			}
                        if (regexec(regex, v, 0, NULL, 0) == 0)
				return 1;
		}
	}
	return 0;
}

static int eval_expr(struct ot_node *root, struct ot_step *step, struct ot_expr *expr, struct ot_node *result)
{
	int i, res, n_child, multi;
	struct ot_node *child, *ichild;
	struct ot_key *keys;

	if (expr == NULL)
		goto null_result;

	switch(expr->type) {
	case OT_EXPR_PATH:
		return ot_path_iterate(&step->current, expr->path.steps, expr->path.n_steps, result);
	case OT_EXPR_NODE:
		*result = expr->node;
		return 0;
	case OT_EXPR_FUNC:
		if (!expr->func)
			goto null_result;
		return expr->func(step, result);
	default:
		break;
	}

	n_child = expr->n_child;
	child = alloca(n_child * sizeof(*child));
	ichild = alloca(n_child * sizeof(*ichild));
	keys = alloca(n_child * sizeof(*keys));
	memset(keys, 0, n_child * sizeof(*keys));

	for (i = 0; i < n_child; i++) {
		if ((res = eval_expr(root, step, expr->child[i], &child[i])) < 0)
			return res;
	}

again:
	multi = false;
	for (i = 0; i < n_child; i++) {
		if (child[i].flags & NODE_FLAG_MULTI) {
			if ((res = ot_node_iterate(&child[i], &keys[i], &ichild[i])) <= 0) {
				if (keys[i].index > 0)
					goto bool_result;
				ichild[i] = OT_INIT_NULL(0, NULL);
			} else {
				keys[i].index++;
				multi = true;
			}
		} else {
			ichild[i] = child[i];
		}
	}
	switch(expr->type) {
	case OT_EXPR_EQ ... OT_EXPR_GTE:
		res = node_compare(ichild, n_child, expr->type);
		break;
	case OT_EXPR_REGEX:
		res = node_regex(ichild, n_child, expr->type);
		break;
	case OT_EXPR_AND ... OT_EXPR_NOT:
		res = node_logic(ichild, n_child, expr->type);
		break;
	default:
		return -EINVAL;
	}
	if (res == 0 && multi)
		goto again;

bool_result:
	*result = OT_INIT_BOOL(0, NULL, res);
	return 0;

null_result:
	*result = OT_INIT_NULL(0, NULL);
	return 0;

}

static int iterate_one(struct ot_node *node, const struct ot_key *key, struct ot_node *sub)
{
	if (key->index < -1 || key->index > 0)
		return 0;
	*sub = *NODE_GET_ROOT(node);
	sub->key.index = key->index;
	sub->flags |= NODE_FLAG_NO_KEY;
	return 1;
}

static void enter_step(struct ot_node *root, struct ot_node *node, int32_t depth)
{
	struct ot_step *next;
	NODE_DEPTH(root) = depth;
	next = NODE_GET_STEP(root, depth);
	next->node = node;
	next->key.index = next->type == OT_MATCH_SLICE ? next->match.slice.start : 0;
	root->parent = node;
}

static int iterate_steps(struct ot_node *n, const struct ot_key *key, struct ot_node *sub)
{
	int32_t depth, iter = 0;
	struct ot_step *step, *deep, *parent;
	struct ot_node *node;
	union ot_match *match, mtch;
	int32_t index;
	int res;
again:
	depth = NODE_DEPTH(n);
	step = NODE_GET_STEP(n, depth);
	node = step->node;
	match = &step->match;
	iter++;

	switch (step->type) {
	case OT_MATCH_DEEP:
		deep = NODE_DEEP(step);
		if (deep == NULL) {
			DEEP_PARENT(step) = NULL;
			NODE_DEEP(step) = step;
			step->current = *step->node;
			node = depth > 0 ? NODE_GET_STEP(n, depth-1)->node : NULL;
			break;
		}
		while (true) {
			index = deep->key.index;
			node = deep->node;
			if ((res = ot_node_iterate(node, &deep->key, &deep->current)) > 0)
				break;
			parent = DEEP_PARENT(deep);
			NODE_DEEP(step) = parent;
			if (parent == NULL)
				goto back;
			free(deep);
			deep = parent;
		}
		deep->key.index = index + 1;
		if (deep->current.type == OT_ARRAY || deep->current.type == OT_OBJECT) {
			parent = calloc(1, sizeof(*deep));
			DEEP_PARENT(parent) = deep;
			NODE_DEEP(step) = parent;
			parent->node = &deep->current;
		}
		step = deep;
		break;
	case OT_MATCH_SLICE:
		index = step->key.index;
		if ((match->slice.end >= 0 && index >= match->slice.end) ||
		    (match->slice.end < 0 && index <= match->slice.end))
			goto back;
		if ((res = ot_node_iterate(node, &step->key, &step->current)) <= 0)
			goto back;
		step->key.index = match->slice.step ? index + match->slice.step : match->slice.end;
		break;
	case OT_MATCH_INDEX:
		mtch.indexes.n_indexes = 1;
		mtch.indexes.indexes = &match->index;
		match = &mtch;
		/* fallthrough */
	case OT_MATCH_INDEXES:
		do {
			index = step->key.index;
			if (index >= match->indexes.n_indexes)
				goto back;
			step->key.index = match->indexes.indexes[index];
			res = ot_node_iterate(node, &step->key, &step->current);
			step->key.index = index + 1;
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
			index = step->key.index;
			if (index >= match->keys.n_keys)
				goto back;

			step->key = OT_INIT_KEYN(0, match->keys.keys[index].val, match->keys.keys[index].len);
			do {
				if ((res = ot_node_iterate(node, &step->key, &step->current)) <= 0)
					break;
				step->key.index++;
			} while (ot_string_cmp(&match->keys.keys[index], &step->current.key.k) != 0);
			step->key.index = index + 1;
		} while (res <= 0);
		break;
	default:
		goto back;
	}
	step->current.parent = node;
	if (step->expr != NULL) {
		struct ot_node result;
		if (eval_expr(n, step, step->expr, &result) < 0 ||
		    !node_boolean(&result))
			goto again;
	}
	if (++depth < NODE_N_STEPS(n)) {
		enter_step(n, &step->current, depth);
		goto again;
	} else {
		*sub = step->current;
		sub->flags |= NODE_FLAG_NO_KEY;
		return 1;
	}
back:
	if (NODE_DEPTH(n) == 0)
		return 0;
	NODE_DEPTH(n)--;
	goto again;
}

int ot_path_iterate(struct ot_node *root, struct ot_step *steps, uint32_t n_steps, struct ot_node *result)
{
	*result = OT_INIT_ARRAY(0, NULL, n_steps > 0 ? iterate_steps : iterate_one);
	result->flags |= NODE_FLAG_MULTI;

	NODE_STEPS(result) = steps;
	NODE_N_STEPS(result) = n_steps;
	NODE_DEPTH(result) = 0;
	NODE_ROOT(result) = root;

	if (n_steps > 0)
		enter_step(result, root, 0);

	return 0;
}
