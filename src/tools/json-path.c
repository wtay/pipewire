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
#include <stdio.h>
#include <string.h>
#include <ctype.h>
#include <math.h>
#include <errno.h>

#include <spa/utils/defs.h>

#include "json-path.h"

#define SKIP_SPACE(p) while(isspace(*p) || *p == '\\') p++
#define ADVANCE(p) if (*p) p++
#define CONSUME(p,c) if (*p == c) p++

static struct ot_expr *parse_condition(const char **path);

//
// term =
//       "!" term
//     | ("$"|"@") path
//     | [0..9]number
//     | "'" string "'"
//     | "true"
//     | "false"
//     | "null"
//     | "(" condition ")" .
//
//
static struct ot_expr *parse_term(const char **path)
{
	struct ot_expr *expr = NULL, tmp = { 0, };
	const char *p = *path, *s;
	char *e;

	SKIP_SPACE(p);

	switch (*p) {
	case '!':
		ADVANCE(p);
		tmp.type = OT_EXPR_NOT;
		tmp.child[0] = parse_term(&p);
		tmp.n_child = 1;
		break;
	case '$': case '@':
		ADVANCE(p);
		tmp.type = OT_EXPR_PATH;
		tmp.path.n_steps = json_path_parse(&p, tmp.path.steps, SPA_N_ELEMENTS(tmp.path.steps));
		break;
	case '-': case '+':
	case '0' ... '9':
	{
		double val = strtod(p, &e);
		tmp.type = OT_EXPR_NODE;
		if (val == 0.0 && p == e)
			goto done;
		tmp.node = OT_INIT_NUMBER(0, NULL, val);
		p = e;
		break;
	}
	case '\'':
		s = ++p;
		while (*p && *p != '\'') {
			CONSUME(p, '\\');
			ADVANCE(p);
		}
		tmp.type = OT_EXPR_NODE;
		tmp.node = OT_INIT_STRINGN(0, NULL, 0, s, p - s);
		ADVANCE(p);
		break;
	case '(':
		++p;
		expr = parse_condition(&p);
		SKIP_SPACE(p);
		CONSUME(p, ')');
		goto done;
	case 't': case 'f': case 'n':
		tmp.type = OT_EXPR_NODE;
		if (strncmp(p, "true", 4) == 0) {
			tmp.node = OT_INIT_BOOL(0, NULL, true);
			p+=4;
		} else if (strncmp(p, "false", 5) == 0) {
			tmp.node = OT_INIT_BOOL(0, NULL, false);
			p+=5;
		} else if (strncmp(p, "null", 4) == 0) {
			tmp.node = OT_INIT_NULL(0, NULL);
			p+=4;
		} else
			goto done;
		break;
	default:
		goto done;
	}
	expr = calloc(1, sizeof(struct ot_expr));
	*expr = tmp;
done:
	*path = p;
	return expr;
}
// test = term [("=="|"!="|"<"|"<="|">"|">="|"~=") term] .
//
static struct ot_expr *parse_test(const char **path)
{
	struct ot_expr *expr = NULL, *tmp, test = { 0, };
	const char *p = *path;

	tmp = parse_term(&p);
	if (tmp == NULL)
		return NULL;

	SKIP_SPACE(p);

	if (strncmp(p, "==", 2) == 0) {
		test.type = OT_EXPR_EQ;
		p+=2;
	} else if (strncmp(p, "!=", 2) == 0) {
		test.type = OT_EXPR_NEQ;
		p+=2;
	} else if (strncmp(p, "<=", 2) == 0) {
		test.type = OT_EXPR_LTE;
		p+=2;
	} else if (strncmp(p, "<", 1) == 0) {
		test.type = OT_EXPR_LT;
		p+=1;
	} else if (strncmp(p, ">=", 2) == 0) {
		test.type = OT_EXPR_GTE;
		p+=2;
	} else if (strncmp(p, ">", 1) == 0) {
		test.type = OT_EXPR_GT;
		p+=1;
	} else if (strncmp(p, "~=", 2) == 0) {
		test.type = OT_EXPR_REGEX;
		p+=2;
	} else {
		expr = tmp;
		goto done;
	}

	expr = calloc(1, sizeof(struct ot_expr));
	*expr = test;
	if ((expr->child[expr->n_child] = tmp) != NULL)
		expr->n_child++;
	if ((expr->child[expr->n_child] = parse_term(&p)) != NULL)
		expr->n_child++;

	if (expr->type == OT_EXPR_REGEX && expr->n_child == 2 &&
	    expr->child[1]->type == OT_EXPR_NODE &&
	    expr->child[1]->node.type == OT_STRING) {
		int len = expr->child[1]->node.v.s.len;
		char val[len+1];
		const char *v = expr->child[1]->node.v.s.val;

		if (len != -1) {
			strncpy(val, v, len);
			val[len] = '\0';
			v = val;
		}
		if (regcomp(&expr->regex, v, REG_EXTENDED | REG_NOSUB) == 0)
			expr->child[1]->node.extra[0].p = &expr->regex;
	}
done:
	*path = p;
	return expr;
}

// condition =
//    test {("&&"|"||") test}
//
static struct ot_expr *parse_condition(const char **path)
{
	struct ot_expr *expr = NULL, *tmp, test = { 0, };
	const char *p = *path;

	tmp = parse_test(&p);

	SKIP_SPACE(p);

	if (strncmp(p, "&&", 2) == 0) {
		test.type = OT_EXPR_AND;
		p += 2;
	} else if (strncmp(p, "||", 2) == 0) {
		test.type = OT_EXPR_OR;
		p += 2;
	} else {
		expr = tmp;
		goto done;
	}
	expr = calloc(1, sizeof(struct ot_expr));
	*expr = test;
	expr->child[0] = tmp;
	expr->child[1] = parse_test(&p);
	expr->n_child = 2;
done:
	*path = p;
	return expr;
}


// path
//   ("$"|"@") { step }
//
// step
//   step-expr [ "[?" condition "]" ]
//
// step-expr
//      ".."
//    | "//"
//    | sep "*"
//    | sep "**"
//    | sep array-index
//    | sep simple-key
//    | sep esc-key
//    | "[(" script ")]
//    | "[" step-expr2 "]"
//
// sep
//    "."|"/"
//
// simple-char
//    "a"..."z"|"A"..."Z"|"_"
//
// simple-key
//   simple-char { simple-char }
//
// esc-key
//    "'" char "'"
//
// array-index
//   [("-"|"+"))]"0".."9"{"0".."9"}
//
// step-expr2
//      "*"
//    | "**"
//    | array-index { "," array-index }
//    | array-index [ ":" [ array-index] ":" [ array-index ] ]
//    | esc-key { "," esc-key }
//

int json_path_parse(const char **path, struct ot_step *step, int max_step)
{
	const char *p, *s;
	char *e;
	int n_step = 0;

	p = *path;

	while (*p) {
		switch (*p) {
		case '$': case '@': case ':': case '[': case ']': case '\\':
			break;
		case '.': case '/':
			if (strncmp(p, "..", 2) == 0  || strncmp(p, "//", 2) == 0) {
				if (n_step < max_step)
					step[n_step++] = OT_INIT_MATCH_DEEP();
				ADVANCE(p);
			}
			break;
		case '\'':
		{
			int n_keys = 0, len;
			struct ot_string *keys = NULL;
			do {
				s = ++p;
				while (*p && *p != '\'') {
					CONSUME(p, '\\');
					ADVANCE(p);
				}
				len = p - s;
				ADVANCE(p);
				if (*p == ',' || n_keys > 0) {
					keys = realloc(keys, sizeof(struct ot_string) * (n_keys+1));
					keys[n_keys].val = s;
					keys[n_keys++].len = len;
					CONSUME(p, ',');
				}
			} while (*p == '\'');

			if (n_step < max_step) {
				if (n_keys == 0)
					step[n_step++] = OT_INIT_MATCH_KEYN(s, len);
				else
					step[n_step++] = OT_INIT_MATCH_KEYS(keys, n_keys);
			}
			continue;
		}
		case '?':
		{
			struct ot_expr *expr;
			++p;
			expr = parse_condition(&p);
			if (n_step <= max_step)
				step[n_step-1].expr = expr;
			break;
		}
		case '*':
			if (n_step < max_step)
				step[n_step++] = OT_INIT_MATCH_ALL();
			break;
		case '-': case '+': case '0' ... '9':
		{
			int n_idx = 0, n_slice = 0;
			int32_t *idx = NULL, val = 0, slice[3];
			do {
				val = strtoll(p, &e, 10);
				if (val == 0 && p == e)
					e++;
				p = e;
				if (*p == ',' || n_idx > 0) {
					idx = realloc(idx, sizeof(int32_t) * (n_idx+1));
					idx[n_idx++] = val;
					CONSUME(p, ',');
				} else if (*p == ':' || n_slice > 0) {
					if (n_slice < 3)
						slice[n_slice++] = val;
					CONSUME(p, ':');
				}
			} while (isdigit(*p) || *p == '-' || *p == '+');

			if (n_step < max_step) {
				if (n_slice == 3)
					step[n_step++] = OT_INIT_MATCH_SLICE(slice[0], slice[1], slice[2]);
				else if (n_idx == 0)
					step[n_step++] = OT_INIT_MATCH_INDEX(val);
				else
					step[n_step++] = OT_INIT_MATCH_INDEXES(idx, n_idx);
			}
			continue;
		}
		case 'a' ... 'z': case 'A' ... 'Z': case '_':
			s = p;
			while (*p && (isalpha(*p) || *p == '\\' || *p == '_')) {
				CONSUME(p, '\\');
				ADVANCE(p);
			}
			if (n_step < max_step)
				step[n_step++] = OT_INIT_MATCH_KEYN(s, p - s);
			continue;
		default:
			goto done;
		}
		ADVANCE(p);
	}
done:
	*path = p;
	return n_step;
}

void json_path_cleanup(struct ot_step *step, int n_step)
{
}
