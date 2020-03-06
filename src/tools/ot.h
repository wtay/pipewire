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
	OT_FLOAT,
	OT_DOUBLE,
	OT_STRING,
	OT_ARRAY,
	OT_OBJECT,
};

union ot_val {
	bool b;
	int32_t i;
	int64_t l;
	float f;
	double d;
	const char *s;
	const void *cp;
	void *p;
};

struct ot_node {
	enum ot_type type;

#define NODE_FLAG_FLAT (1<<0)
#define NODE_FLAG_EXPENSIVE (1<<1)
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

#define OT_INIT_NULL(_k)	(struct ot_node){ .type = OT_NULL, .k = _k }
#define OT_INIT_BOOL(_k,_v)	(struct ot_node){ .type = OT_BOOL, .k = _k, .v.b = _v }
#define OT_INIT_INT(_k,_v)	(struct ot_node){ .type = OT_INT, .k = _k, .v.i = _v }
#define OT_INIT_LONG(_k,_v)	(struct ot_node){ .type = OT_LONG, .k = _k, .v.l = _v }
#define OT_INIT_FLOAT(_k,_v)	(struct ot_node){ .type = OT_FLOAT, .k = _k, .v.f = _v }
#define OT_INIT_DOUBLE(_k,_v)	(struct ot_node){ .type = OT_DOUBLE, .k = _k, .v.d = _v }
#define OT_INIT_STRING(_k,_v)	(struct ot_node){ .type = _v ? OT_STRING : OT_NULL, .k = _k, .v.s = _v }
#define OT_INIT_ARRAY(_k,_i)	(struct ot_node){ .type = OT_ARRAY, .k = _k, .iterate = _i }
#define OT_INIT_OBJECT(_k,_i)	(struct ot_node){ .type = OT_OBJECT, .k = _k, .iterate = _i }

static inline void ot_set_null(struct ot_node *node, const char *k)
{
	*node = OT_INIT_NULL(k);
}

static inline void ot_set_bool(struct ot_node *node, const char *k, bool val)
{
	*node = OT_INIT_BOOL(k, val);
}

static inline void ot_set_int(struct ot_node *node, const char *k, int32_t val)
{
	*node = OT_INIT_INT(k, val);
}

static inline void ot_set_long(struct ot_node *node, const char *k, int64_t val)
{
	*node = OT_INIT_LONG(k, val);
}

static inline void ot_set_float(struct ot_node *node, const char *k, double val)
{
	*node = OT_INIT_FLOAT(k, val);
}

static inline void ot_set_double(struct ot_node *node, const char *k, double val)
{
	*node = OT_INIT_DOUBLE(k, val);
}

static inline void ot_set_string(struct ot_node *node, const char *k, const char *val)
{
	*node = OT_INIT_STRING(k, val);
}

static inline void ot_set_array(struct ot_node *node, const char *k,
		int (*iterate) (struct ot_node *node, struct ot_node *sub))
{
	*node = OT_INIT_ARRAY(k, iterate);
}

static inline void ot_set_object(struct ot_node *node, const char *k,
		int (*iterate) (struct ot_node *node, struct ot_node *sub))
{
	*node = OT_INIT_OBJECT(k, iterate);
}

#ifdef __cplusplus
}
#endif

#endif /* PIPEWIRE_OT_H */
