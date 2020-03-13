#include <stddef.h>
#include <errno.h>
#include <limits.h>

#include <spa/utils/defs.h>

#include "json-parse.h"

#define NONE      0
#define STRUCT    1
#define BARE      2
#define STRING    3
#define UTF8      4
#define ESC       5

#define NODE_CUR(n)		((n)->extra[0].p)
#define NODE_CUR_INC(n,i)	NODE_CUR(n) = SPA_MEMBER(NODE_CUR(n),(i),void)
#define NODE_END(n)		((n)->extra[1].p)
#define NODE_STATE(n)		((n)->extra[2].i)
#define NODE_PARENT(n)		((n)->extra[3].p)
#define NODE_PARENT_CUR(n)	NODE_CUR((struct ot_node*)NODE_PARENT(n))
#define NODE_DEPTH(n)		((n)->extra[4].i)

static int node_iterate(struct ot_node *node, struct ot_key *key, struct ot_node *sub);

static void parse_begin(char *data, char *end, struct ot_node *parent, struct ot_node *sub)
{
	NODE_CUR(sub) = data;
	NODE_END(sub) = end;
	NODE_STATE(sub) = NONE;
	NODE_PARENT(sub) = parent;
	sub->iterate = node_iterate;
}

static void bare_type(const char *start, int len, int32_t x, struct ot_string *key, struct ot_node *node)
{
	if (strncmp(start, "null", len) == 0)
		*node = OT_INIT_NULLN(x, key->val, key->len);
	else if (strncmp(start, "true", len) == 0)
		*node = OT_INIT_BOOLN(x, key->val, key->len, true);
	else if (strncmp(start, "false", len) == 0)
		*node = OT_INIT_BOOLN(x, key->val, key->len, false);
	else  {
		char tmp[len+1], *end;
		long long ll;
		double d;

		memcpy(tmp, start, len);
		tmp[len] = '\0';

		errno = 0;
		ll = strtoll(tmp, &end, 10);
		if (*end == '\0' && errno == 0) {
			if (ll < INT32_MIN || ll > INT32_MAX)
				*node = OT_INIT_LONGN(x, key->val, key->len, ll);
			else
				*node = OT_INIT_INTN(x, key->val, key->len, ll);
		} else {
			errno = 0;
			d = strtod(tmp, &end);
			if (*end == '\0' && errno == 0)
				*node = OT_INIT_DOUBLEN(x, key->val, key->len, d);
			else
				*node = OT_INIT_STRINGN(x, key->val, key->len, start, len);
		}
	}
}
static int node_iterate(struct ot_node *node, struct ot_key *k, struct ot_node *sub)
{
	int utf8_remain = 0, len;
	struct ot_string key = { 0, };
	char *start = NULL;

	for (; NODE_CUR(node) < NODE_END(node); NODE_CUR_INC(node, 1)) {
		unsigned char cur = *(unsigned char*)NODE_CUR(node);
	      again:
		switch (NODE_STATE(node)) {
		case NONE:
			NODE_STATE(node) = STRUCT;
			NODE_DEPTH(node) = 0;
			goto again;
		case STRUCT:
			switch (cur) {
			case '\t': case ' ': case '\r': case '\n': case ':': case ',':
				continue;
			case '"':
				start = NODE_CUR(node);
				start++;
				NODE_STATE(node) = STRING;
				continue;
			case '[': case '{':
				start = NODE_CUR(node);
				if (++NODE_DEPTH(node) > 1)
					continue;
				NODE_CUR_INC(node, 1);
				if (cur == '[')
					*sub = OT_INIT_ARRAYN(0, key.val, key.len, node_iterate);
				else
					*sub = OT_INIT_OBJECTN(0, key.val, key.len, node_iterate);
				key.val = NULL;
				parse_begin(NODE_CUR(node), NODE_END(node), node, sub);
				return 1;
			case '}': case ']':
				if (NODE_DEPTH(node) == 0) {
					if (NODE_PARENT(node))
						NODE_PARENT_CUR(node) = NODE_CUR(node);
					return 0;
				}
				--NODE_DEPTH(node);
				continue;
			case '-': case 'a' ... 'z': case 'A' ... 'Z': case '0' ... '9':
				start = NODE_CUR(node);
				NODE_STATE(node) = BARE;
				continue;
			}
			return -EINVAL;
		case BARE:
			switch (cur) {
			case '\t': case ' ': case '\r': case '\n': case ':': case ',':
			case ']': case '}':
				NODE_STATE(node) = STRUCT;
				if (NODE_DEPTH(node) > 0)
					goto again;
				len = SPA_PTRDIFF(NODE_CUR(node), start);
				if (node->type == OT_OBJECT && key.val == NULL) {
					key.val = start;
					key.len = len;
					continue;
				}
				bare_type(start, len, 0, &key, sub);
				key.val = NULL;
				parse_begin(NODE_CUR(node), NODE_END(node), node, sub);
				return 1;
			default:
				if (cur >= 32 && cur <= 126)
					continue;
			}
			return -EINVAL;
		case STRING:
			switch (cur) {
			case '\\':
				NODE_STATE(node) = ESC;
				continue;
			case '"':
				NODE_STATE(node) = STRUCT;
				if (NODE_DEPTH(node) > 0)
					continue;
				len = SPA_PTRDIFF(NODE_CUR(node), start);
				NODE_CUR_INC(node, 1);
				if (node->type == OT_OBJECT && key.val == NULL) {
					key.val = start;
					key.len = len;
					continue;
				}
				*sub = OT_INIT_STRINGN(0, key.val, key.len, start, len);
				key.val = NULL;
				parse_begin(NODE_CUR(node), NODE_END(node), node, sub);
				return 1;
			case 240 ... 247:
				utf8_remain++;
				/* fallthrough */
			case 224 ... 239:
				utf8_remain++;
				/* fallthrough */
			case 192 ... 223:
				utf8_remain++;
				NODE_STATE(node) = UTF8;
				continue;
			default:
				if (cur >= 32 && cur <= 126)
					continue;
			}
			return -EINVAL;
		case UTF8:
			switch (cur) {
			case 128 ... 191:
				if (--utf8_remain == 0)
					NODE_STATE(node) = STRING;
				continue;
			}
			return -EINVAL;
		case ESC:
			switch (cur) {
			case '"': case '\\': case '/': case 'b': case 'f': case 'n':
			case 'r': case 't': case 'u':
				NODE_STATE(node) = STRING;
				continue;
			}
			return -EINVAL;
		}
	}
	return NODE_DEPTH(node) == 0 ? 0 : -EINVAL;
}

int json_parse_begin(const char *data, int32_t size, struct ot_node *result)
{
	struct ot_node root;
	struct ot_key key = { 0, };
	parse_begin((char*)data, (char*)(data + size), NULL, &root);
	return ot_node_iterate(&root, &key, result);
}

void json_parse_end(struct ot_node *result)
{
}
