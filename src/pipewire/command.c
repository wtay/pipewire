/* PipeWire
 * Copyright (C) 2016 Axis Communications <dev-gstreamer@axis.com>
 * @author Linus Svensson <linus.svensson@axis.com>
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Library General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Library General Public License for more details.
 *
 * You should have received a copy of the GNU Library General Public
 * License along with this library; if not, write to the
 * Free Software Foundation, Inc., 51 Franklin St, Fifth Floor,
 * Boston, MA 02110-1301, USA.
 */

#include <errno.h>
#include <string.h>
#include <stdio.h>

#include <pipewire/pipewire.h>
#include <pipewire/utils.h>
#include <pipewire/module.h>

#include "command.h"
#include "private.h"

/** \cond */

static struct pw_command *parse_command_help(const char *line, char **err);
static struct pw_command *parse_command_module_load(const char *line, char **err);

struct impl {
	struct pw_command this;
};

typedef struct pw_command *(*pw_command_parse_func_t) (const char *line, char **err);

struct command_parse {
	const char *name;
	const char *description;
	pw_command_parse_func_t func;
};

static const struct command_parse parsers[] = {
	{"help", "Show this help", parse_command_help},
	{"load-module", "Load a module", parse_command_module_load},
	{NULL, NULL, NULL }
};

static const char whitespace[] = " \t";
/** \endcond */

static int
execute_command_help(struct pw_command *command, struct pw_core *core, char **err)
{
	int i;

	fputs("Available commands:\n", stdout);
	for (i = 0; parsers[i].name; i++)
		fprintf(stdout, "    %20.20s\t%s\n", parsers[i].name, parsers[i].description);

	return 0;
}

static struct pw_command *parse_command_help(const char *line, char **err)
{
	struct impl *impl;
	struct pw_command *this;

	impl = calloc(1, sizeof(struct impl));
	if (impl == NULL)
		goto no_mem;

	this = &impl->this;
	this->func = execute_command_help;
	this->args = pw_split_strv(line, whitespace, 1, &this->n_args);

	return this;

      no_mem:
	asprintf(err, "no memory");
	return NULL;
}

static int
execute_command_module_load(struct pw_command *command, struct pw_core *core, char **err)
{
	struct pw_module *module;

	module = pw_module_load(core, command->args[1], command->args[2], NULL, NULL, NULL);
	if (module == NULL) {
		asprintf(err, "could not load module \"%s\"", command->args[1]);
		return -ENOMEM;
	}
	return 0;
}

static struct pw_command *parse_command_module_load(const char *line, char **err)
{
	struct impl *impl;
	struct pw_command *this;

	impl = calloc(1, sizeof(struct impl));
	if (impl == NULL)
		goto no_mem;

	this = &impl->this;
	this->func = execute_command_module_load;
	this->args = pw_split_strv(line, whitespace, 3, &this->n_args);

	if (this->n_args < 2)
		goto no_module;

	return this;

      no_module:
	asprintf(err, "%s requires a module name", this->args[0]);
	pw_free_strv(this->args);
	free(impl);
	return NULL;
      no_mem:
	asprintf(err, "no memory");
	return NULL;
}

/** Free command
 *
 * \param command a command to free
 *
 * Free all resources assicated with \a command.
 *
 * \memberof pw_command
 */
SPA_EXPORT
void pw_command_free(struct pw_command *command)
{
	struct impl *impl = SPA_CONTAINER_OF(command, struct impl, this);

	spa_list_remove(&command->link);
	pw_free_strv(command->args);
	free(impl);
}

/** Parses a command line
 * \param line command line to parse
 * \param[out] err Return location for an error
 * \return The command or NULL when \a err is set.
 *
 * Parses a command line, \a line, and return the parsed command.
 * A command can later be executed with \ref pw_command_run()
 *
 * \memberof pw_command
 */
SPA_EXPORT
struct pw_command *pw_command_parse(const char *line, char **err)
{
	struct pw_command *command = NULL;
	const struct command_parse *parse;
	char *name;
	size_t len;

	len = strcspn(line, whitespace);

	name = strndup(line, len);

	for (parse = parsers; parse->name != NULL; parse++) {
		if (strcmp(name, parse->name) == 0) {
			command = parse->func(line, err);
			goto out;
		}
	}

	asprintf(err, "Command \"%s\" does not exist", name);
      out:
	free(name);
	return command;
}

/** Run a command
 *
 * \param command A \ref pw_command
 * \param core A \ref pw_core
 * \param err Return location for an error string, or NULL
 * \return 0 on success, < 0 on error
 *
 * \memberof pw_command
 */
SPA_EXPORT
int pw_command_run(struct pw_command *command, struct pw_core *core, char **err)
{
	return command->func(command, core, err);
}
