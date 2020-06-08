/* PipeWire
 * Copyright (C) 2015 Wim Taymans <wim.taymans@gmail.com>
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

#include <stdio.h>

#include "pipewire/pipewire.h"
#include "pipewire/properties.h"

/** \cond */
struct properties {
	struct pw_properties this;

	struct pw_array items;
};
/** \endcond */

static int add_func(struct pw_properties *this, char *key, char *value)
{
	struct spa_dict_item *item;
	struct properties *impl = SPA_CONTAINER_OF(this, struct properties, this);

	item = pw_array_add(&impl->items, sizeof(struct spa_dict_item));
	item->key = key;
	item->value = value;

	this->dict.items = impl->items.data;
	this->dict.n_items = pw_array_get_len(&impl->items, struct spa_dict_item);
	return 0;
}

static void clear_item(struct spa_dict_item *item)
{
	free((char *) item->key);
	free((char *) item->value);
}

static int find_index(const struct pw_properties *this, const char *key)
{
	struct properties *impl = SPA_CONTAINER_OF(this, struct properties, this);
	int i, len = pw_array_get_len(&impl->items, struct spa_dict_item);

	for (i = 0; i < len; i++) {
		struct spa_dict_item *item =
		    pw_array_get_unchecked(&impl->items, i, struct spa_dict_item);
		if (strcmp(item->key, key) == 0)
			return i;
	}
	return -1;
}

static struct properties *properties_new(int prealloc)
{
	struct properties *impl;

	impl = calloc(1, sizeof(struct properties));
	if (impl == NULL)
		return NULL;

	pw_array_init(&impl->items, prealloc);

	return impl;
}

/** Make a new properties object
 *
 * \param key a first key
 * \param ... value and more keys NULL terminated
 * \return a newly allocated properties object
 *
 * \memberof pw_properties
 */
SPA_EXPORT
struct pw_properties *pw_properties_new(const char *key, ...)
{
	struct properties *impl;
	va_list varargs;
	const char *value;

	impl = properties_new(16);
	if (impl == NULL)
		return NULL;

	va_start(varargs, key);
	while (key != NULL) {
		value = va_arg(varargs, char *);
		if (value)
			add_func(&impl->this, strdup(key), strdup(value));
		key = va_arg(varargs, char *);
	}
	va_end(varargs);

	return &impl->this;
}

/** Make a new properties object from the given dictionary
 *
 * \param dict a dictionary. keys and values are copied
 * \return a new properties object
 *
 * \memberof pw_properties
 */
SPA_EXPORT
struct pw_properties *pw_properties_new_dict(const struct spa_dict *dict)
{
	uint32_t i;
	struct properties *impl;

	impl = properties_new(16);
	if (impl == NULL)
		return NULL;

	for (i = 0; i < dict->n_items; i++) {
		if (dict->items[i].key != NULL && dict->items[i].value != NULL)
			add_func(&impl->this, strdup(dict->items[i].key),
				 strdup(dict->items[i].value));
	}

	return &impl->this;
}

/** Make a new properties object from the given str
 *
 * \a str should be a whitespace separated list of key=value
 * strings.
 *
 * \param args a property description
 * \return a new properties object
 *
 * \memberof pw_properties
 */
SPA_EXPORT
struct pw_properties *
pw_properties_new_string(const char *str)
{

	struct properties *impl;
        const char *state = NULL, *s = NULL;
	size_t len;

	impl = properties_new(16);
	if (impl == NULL)
		return NULL;

	s = pw_split_walk(str, " \t\n\r", &len, &state);
	while (s) {
		char *val, *eq;

		val = strndup(s, len);
		eq = strchr(val, '=');
		if (eq) {
			*eq = '\0';
			add_func(&impl->this, val, strdup(eq+1));
		} else {
			free(val);
		}
		s = pw_split_walk(str, " \t\n\r", &len, &state);
	}
	return &impl->this;
}

/** Copy a properties object
 *
 * \param properties properties to copy
 * \return a new properties object
 *
 * \memberof pw_properties
 */
SPA_EXPORT
struct pw_properties *pw_properties_copy(const struct pw_properties *properties)
{
	struct properties *impl = SPA_CONTAINER_OF(properties, struct properties, this);
	struct pw_properties *copy;
	struct spa_dict_item *item;

	copy = pw_properties_new(NULL, NULL);
	if (copy == NULL)
		return NULL;

	pw_array_for_each(item, &impl->items)
		add_func(copy, strdup(item->key), strdup(item->value));

	return copy;
}

/** Merge properties into one
 *
 * \param oldprops properties to merge into
 * \param newprops properties to merge
 * \return a newly allocated \ref pw_properties
 *
 * A new \ref pw_properties is allocated and the properties of
 * \a oldprops and \a newprops are copied into it in that order.
 *
 * \memberof pw_properties
 */
SPA_EXPORT
struct pw_properties *pw_properties_merge(const struct pw_properties *oldprops,
					  struct pw_properties *newprops)
{
	struct pw_properties *res = NULL;

	if (oldprops == NULL) {
		if (newprops == NULL)
			res = NULL;
		else
			res = pw_properties_copy(newprops);
	} else if (newprops == NULL) {
		res = pw_properties_copy(oldprops);
	} else {
		const char *key;
		void *state = NULL;

		res = pw_properties_copy(oldprops);
		if (res == NULL)
			return NULL;

		while ((key = pw_properties_iterate(newprops, &state))) {
			pw_properties_set(res, key, pw_properties_get(newprops, key));
		}
	}
	return res;
}

/** Free a properties object
 *
 * \param properties the properties to free
 *
 * \memberof pw_properties
 */
SPA_EXPORT
void pw_properties_free(struct pw_properties *properties)
{
	struct properties *impl = SPA_CONTAINER_OF(properties, struct properties, this);
	struct spa_dict_item *item;

	pw_array_for_each(item, &impl->items)
		clear_item(item);

	pw_array_clear(&impl->items);
	free(impl);
}

static int do_replace(struct pw_properties *properties, const char *key, char *value, bool copy)
{
	struct properties *impl = SPA_CONTAINER_OF(properties, struct properties, this);
	int index = find_index(properties, key);

	if (index == -1) {
		if (value == NULL)
			return 0;
		add_func(properties, strdup(key), copy ? strdup(value) : value);
	} else {
		struct spa_dict_item *item =
		    pw_array_get_unchecked(&impl->items, index, struct spa_dict_item);

		if (value && strcmp(item->value, value) == 0) {
			if (!copy)
				free(value);
			return 0;
		}

		if (value == NULL) {
			struct spa_dict_item *other = pw_array_get_unchecked(&impl->items,
						     pw_array_get_len(&impl->items, struct spa_dict_item) - 1,
						     struct spa_dict_item);
			clear_item(item);
			item->key = other->key;
			item->value = other->value;
			impl->items.size -= sizeof(struct spa_dict_item);
		} else {
			free((char *) item->value);
			item->value = copy ? strdup(value) : value;
		}
	}
	return 1;
}

/** Set a property value
 *
 * \param properties the properties to change
 * \param key a key
 * \param value a value or NULL to remove the key
 * \return 1 if the properties were changed. 0 if nothing was changed because
 *  the property already existed with the same value or because the key to remove
 *  did not exist.
 *
 * Set the property in \a properties with \a key to \a value. Any previous value
 * of \a key will be overwritten. When \a value is NULL, the key will be
 * removed.
 *
 * \memberof pw_properties
 */
SPA_EXPORT
int pw_properties_set(struct pw_properties *properties, const char *key, const char *value)
{
	return do_replace(properties, key, (char*)value, true);
}

SPA_EXPORT
int
pw_properties_setva(struct pw_properties *properties,
		const char *key, const char *format, va_list args)
{
	char *value;
	vasprintf(&value, format, args);
	return do_replace(properties, key, value, false);
}

/** Set a property value by format
 *
 * \param properties a \ref pw_properties
 * \param key a key
 * \param format a value
 * \param ... extra arguments
 * \return 1 if the properties were changed. 0 if nothing was changed because
 *  the property already existed with the same value.
 *
 * Set the property in \a properties with \a key to the value in printf style \a format
 * Any previous value of \a key will be overwritten.
 *
 * \memberof pw_properties
 */
SPA_EXPORT
int pw_properties_setf(struct pw_properties *properties, const char *key, const char *format, ...)
{
	int res;
	va_list varargs;

	va_start(varargs, format);
	res = pw_properties_setva(properties, key, format, varargs);
	va_end(varargs);

	return res;
}

/** Get a property
 *
 * \param properties a \ref pw_properties
 * \param key a key
 * \return the property for \a key or NULL when the key was not found
 *
 * Get the property in \a properties with \a key.
 *
 * \memberof pw_properties
 */
SPA_EXPORT
const char *pw_properties_get(const struct pw_properties *properties, const char *key)
{
	struct properties *impl = SPA_CONTAINER_OF(properties, struct properties, this);
	int index = find_index(properties, key);

	if (index == -1)
		return NULL;

	return pw_array_get_unchecked(&impl->items, index, struct spa_dict_item)->value;
}

/** Iterate property values
 *
 * \param properties a \ref pw_properties
 * \param state state
 * \return The next key or NULL when there are no more keys to iterate.
 *
 * Iterate over \a properties, returning each key in turn. \a state should point
 * to a pointer holding NULL to get the first element and will be updated
 * after each iteration. When NULL is returned, all elements have been
 * iterated.
 *
 * \memberof pw_properties
 */
SPA_EXPORT
const char *pw_properties_iterate(const struct pw_properties *properties, void **state)
{
	struct properties *impl = SPA_CONTAINER_OF(properties, struct properties, this);
	uint32_t index;

	if (*state == NULL)
		index = 0;
	else
		index = SPA_PTR_TO_INT(*state);

	if (!pw_array_check_index(&impl->items, index, struct spa_dict_item))
		 return NULL;

	*state = SPA_INT_TO_PTR(index + 1);

	return pw_array_get_unchecked(&impl->items, index, struct spa_dict_item)->key;
}
