/* PipeWire
 *
 * Copyright © 2018 Wim Taymans
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

#ifndef PIPEWIRE_OT_H
#define PIPEWIRE_OT_H

#ifdef __cplusplus
extern "C" {
#endif

enum ot_type {
	OT_NULL,
	OT_BOOL,
	OT_INT,
	OT_LONG,
	OT_DOUBLE,
	OT_STRING,
	OT_ARRAY,
	OT_OBJECT,
};

union ot_val {
	bool b;
	int32_t i;
	int64_t l;
	double d;
	const char *s;
	const char *k;
	const void *cp;
	void *p;
};

struct ot_node {
	enum ot_type type;

#define NODE_FLAG_FLAT (1<<0)
	uint32_t flags;

	const char *k;
	union ot_val v;

	int (*iterate) (struct ot_node *node, struct ot_node *sub);

	union ot_val extra[8];
	char buffer[64];
};

#define ot_node_iterate(node,sub)					\
({									\
	int res = 0;							\
	if ((node)->iterate)						\
		res = (node)->iterate(node, sub);			\
	res;								\
})

static inline void ot_set_null(struct ot_node *node, const char *k)
{
	node->type = OT_NULL;
	node->k = k;
}

static inline void ot_set_bool(struct ot_node *node, const char *k, bool val)
{
	node->type = OT_BOOL;
	node->k = k;
	node->v.b = val;
}

static inline void ot_set_int(struct ot_node *node, const char *k, int32_t val)
{
	node->type = OT_INT;
	node->k = k;
	node->v.i = val;
}

static inline void ot_set_long(struct ot_node *node, const char *k, int64_t val)
{
	node->type = OT_LONG;
	node->k = k;
	node->v.l = val;
}

static inline void ot_set_double(struct ot_node *node, const char *k, double val)
{
	node->type = OT_DOUBLE;
	node->k = k;
	node->v.d = val;
}

static inline void ot_set_string(struct ot_node *node, const char *k, const char *str)
{
	node->type = str ? OT_STRING : OT_NULL;
	node->v.s = str;
	node->k = k;
}

static inline void ot_set_array(struct ot_node *node, const char *k,
		int (*iterate) (struct ot_node *node, struct ot_node *sub))
{
	node->type = OT_ARRAY;
	node->k = k;
	node->iterate = iterate;
}

static inline void ot_set_object(struct ot_node *node, const char *k,
		int (*iterate) (struct ot_node *node, struct ot_node *sub))
{
	node->type = OT_OBJECT;
	node->flags = 0;
	node->k = k;
	node->iterate = iterate;
}

#ifdef __cplusplus
}
#endif

#endif /* PIPEWIRE_OT_H */
