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

#include <stdio.h>
#include <signal.h>
#include <float.h>

#include <spa/utils/result.h>
#include <spa/debug/pod.h>
#include <spa/debug/format.h>
#include <spa/debug/types.h>

#include <pipewire/pipewire.h>

#include "ot.h"
#include "json-dump.h"
#include "json-parse.h"
#include "tree.h"
#include "path.h"

#define NAME "dump"

struct data {
	struct pw_main_loop *loop;
	struct pw_context *context;

	struct pw_core *core;
	struct spa_hook core_listener;

	struct pw_tree *tree;
	struct spa_hook tree_listener;

	int pending_seq;
};

static void core_sync(struct data *d)
{
	d->pending_seq = pw_core_sync(d->core, PW_ID_CORE, d->pending_seq);
}
static void core_roundtrip(struct data *d)
{
	core_sync(d);
	pw_main_loop_run(d->loop);
}

static void on_core_done(void *data, uint32_t id, int seq)
{
       struct data *d = data;

       if (id == PW_ID_CORE && d->pending_seq == seq)
               pw_main_loop_quit(d->loop);
}

static void on_core_error(void *_data, uint32_t id, int seq, int res, const char *message)
{
	struct data *data = _data;

	pw_log_error("error id:%u seq:%d res:%d (%s): %s",
			id, seq, res, spa_strerror(res), message);

	if (id == PW_ID_CORE)
		pw_main_loop_quit(data->loop);
}

static const struct pw_core_events core_events = {
	PW_VERSION_CORE_EVENTS,
	.done = on_core_done,
	.error = on_core_error,
};

static void do_quit(void *data, int signal_number)
{
	struct data *d = data;
	pw_main_loop_quit(d->loop);
}

static void tree_added(void *data, const char *path)
{
}

static void tree_updated(void *data, const char *path)
{
}

static void tree_removed(void *data, const char *path)
{
}

static const struct pw_tree_events tree_events = {
	PW_VERSION_TREE_EVENTS,
	.added = tree_added,
	.updated = tree_updated,
	.removed = tree_removed,
};

static int filter_port(struct ot_step *step)
{
	struct ot_step steps[3];
	struct ot_node result, val;
	struct ot_key key = { 0, };
	int res = 0;

	steps[0] = OT_INIT_MATCH_KEY("type");
	ot_path_begin(&step->current, steps, 1, &result);

	if (ot_node_iterate(&result, &key, &val)) {
		if (val.type == OT_STRING &&
		    strncmp(val.v.s.val, "PipeWire:Interface:Port", val.v.s.len) == 0)
			res = 1;
	}
	ot_path_end(&result);
	if (!res)
		return res;

	res = 0;
	steps[0] = OT_INIT_MATCH_KEY("properties");
	steps[1] = OT_INIT_MATCH_KEY("node.id");
	ot_path_begin(&step->current, steps, 2, &result);

	if (ot_node_iterate(&result, &key, &val)) {
		if (val.type == OT_INT && val.v.i == 40)
			res = 1;
	}
	ot_path_end(&result);

	return res;
}

int main(int argc, char *argv[])
{
	struct data data = { 0 };
	struct pw_loop *l;
	struct pw_properties *props = NULL;
	struct ot_node root, result;
	struct ot_step steps[5];

	pw_init(&argc, &argv);

	data.loop = pw_main_loop_new(NULL);
	if (data.loop == NULL)
		return -1;

	l = pw_main_loop_get_loop(data.loop);
	pw_loop_add_signal(l, SIGINT, do_quit, &data);
	pw_loop_add_signal(l, SIGTERM, do_quit, &data);

	data.context = pw_context_new(l, NULL, 0);
	if (data.context == NULL)
		return -1;

	if (argc > 1)
		props = pw_properties_new(PW_KEY_REMOTE_NAME, argv[1], NULL);

	data.core = pw_context_connect(data.context, props, 0);
	if (data.core == NULL)
		return -1;

	pw_core_add_listener(data.core,
			&data.core_listener,
			&core_events, &data);

	data.tree = pw_tree_new(data.core);
	pw_tree_add_listener(data.tree,
			&data.tree_listener,
			&tree_events, &data);

	core_roundtrip(&data);

	pw_tree_get_root(data.tree, &root);

	steps[0] = OT_INIT_MATCH_SLICE(0,-1,1);
	steps[0].filter = filter_port;
	steps[1] = OT_INIT_MATCH_KEY("info");
	steps[2] = OT_INIT_MATCH_ALL();
	steps[3] = OT_INIT_MATCH_KEY("object.id");

	ot_path_begin(&root, steps, 4, &result);
	ot_json_dump(stdout, &result, 2);
	ot_path_end(&result);

	printf("\n");

	const char *json =
		"{"
		"  \"id\": 45,"
		"  \"name\": \"foo.bar\","
		"  \"info\": {"
		"     \"key\": \"foo.bar\","
		"     \"object.id\": 45,"
		"     \"object.f\": 0.67,"
		"     \"object.bt\": true,"
		"     \"object.bf\": false,"
		"     \"object.n\": null,"
		"     \"object.a\": [ \"1\", 2, null, true, false, { uu: 9999999999999999999 } ],"
		"  }"
		"}";

	json_parse_begin(json, strlen(json), &result);
	ot_json_dump(stdout, &result, 2);
	json_parse_end(&result);

	pw_context_destroy(data.context);
	pw_main_loop_destroy(data.loop);

	return 0;
}
