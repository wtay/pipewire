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
#include <getopt.h>

#include <spa/utils/result.h>
#include <spa/debug/pod.h>
#include <spa/debug/format.h>
#include <spa/debug/types.h>

#include <pipewire/pipewire.h>

#include "ot.h"
#include "json-dump.h"
#include "json-parse.h"
#include "json-path.h"
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

static void show_help(struct data *data, const char *name)
{
        fprintf(stdout, "%s [options] [path]\n"
             "  -h, --help                            Show this help\n"
             "      --version                         Show version\n"
             "  -r, --remote                          Remote daemon name\n"
             "  -p, --path                            Dump matching paths only\n"
             "  -v, --verbose                         Show input and parsed path\n"
             "  -m, --monitor                         Wait for show changes\n",
	     name);
}

int main(int argc, char *argv[])
{
	struct data data = { 0 };
	struct pw_loop *l;
	struct ot_node root, result;
	const char *p, *opt_remote = NULL;
	bool opt_path = false, opt_monitor = false;
	struct ot_step steps[10];
	int n_steps, c, opt_verbose = 0;
	static const struct option long_options[] = {
		{ "help",	0, NULL, 'h' },
		{ "version",	0, NULL, 'V' },
		{ "remote",	1, NULL, 'r' },
		{ "path",	0, NULL, 'p' },
		{ "monitor",	0, NULL, 'm' },
		{ "verbose",	0, NULL, 'v' },
		{ NULL, 0, NULL, 0 }
	};

	pw_init(&argc, &argv);

	while ((c = getopt_long(argc, argv, "hVvr:pm", long_options, NULL)) != -1) {
		switch (c) {
		case 'h':
			show_help(&data, argv[0]);
			return 0;
		case 'V':
			fprintf(stdout, "%s\n"
				"Compiled with libpipewire %s\n"
				"Linked with libpipewire %s\n",
				argv[0],
				pw_get_headers_version(),
				pw_get_library_version());
			return 0;
		case 'r':
			opt_remote = optarg;
			break;
		case 'p':
			opt_path = true;
			break;
		case 'v':
			opt_verbose++;
			break;
		case 'm':
			opt_monitor = true;
			break;
		default:
			return -1;
		}
	}
	data.loop = pw_main_loop_new(NULL);
	if (data.loop == NULL)
		return -1;

	l = pw_main_loop_get_loop(data.loop);
	pw_loop_add_signal(l, SIGINT, do_quit, &data);
	pw_loop_add_signal(l, SIGTERM, do_quit, &data);

	data.context = pw_context_new(l, NULL, 0);
	if (data.context == NULL)
		return -1;

	data.core = pw_context_connect(data.context,
			pw_properties_new(
				PW_KEY_REMOTE_NAME, opt_remote,
				NULL),
			0);
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

	p = argc > optind ? argv[optind] : ".";

	if (opt_verbose)
		fprintf(stdout, "parsing \"%s\"\n", p);

	n_steps = json_path_parse(&p, steps, SPA_N_ELEMENTS(steps));

	if (opt_verbose) {
		fprintf(stdout, "parsed: \"$");
		ot_json_dump_steps(stdout, steps, n_steps);
		fprintf(stdout, "\n");
	}

	pw_tree_get_root(data.tree, &root);
	ot_path_iterate(&root, steps, n_steps, &result);

	if (opt_path) {
		struct ot_key key = { 0, };
		struct ot_node val;
		while (ot_node_iterate(&result, &key, &val) == 1) {
			fprintf(stdout, "$");
			ot_json_dump_path(stdout, &val);
			fprintf(stdout, "\n");
			key.index++;
		}
	} else {
		ot_json_dump(stdout, &result, 2);
		fprintf(stdout, "\n");
	}
	json_path_cleanup(steps, n_steps);

	if (opt_monitor)
		pw_main_loop_run(data.loop);

	pw_context_destroy(data.context);
	pw_main_loop_destroy(data.loop);

	return 0;
}
