/* Simple Plugin API */
/* SPDX-FileCopyrightText: Copyright © 2018 Wim Taymans */
/* SPDX-License-Identifier: MIT */

#ifndef SPA_DEBUG_TYPES_H
#define SPA_DEBUG_TYPES_H

#include <spa/utils/type-info.h>

#include <string.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * \addtogroup spa_debug
 * \{
 */

#ifndef SPA_API_DEBUG_TYPES
 #ifdef SPA_API_IMPL
  #define SPA_API_DEBUG_TYPES SPA_API_IMPL
 #else
  #define SPA_API_DEBUG_TYPES static inline
 #endif
#endif


SPA_API_DEBUG_TYPES const struct spa_type_info *spa_debug_type_find(const struct spa_type_info *info, uint32_t type)
{
	const struct spa_type_info *res;

	if (info == NULL)
		info = SPA_TYPE_ROOT;

	while (info && info->name) {
		if (info->type == SPA_ID_INVALID) {
			if (info->values && (res = spa_debug_type_find(info->values, type)))
				return res;
		}
		else if (info->type == type)
			return info;
		info++;
	}
	return NULL;
}

SPA_API_DEBUG_TYPES const char *spa_debug_type_short_name(const char *name)
{
	return spa_type_short_name(name);
}

SPA_API_DEBUG_TYPES const char *spa_debug_type_find_name(const struct spa_type_info *info, uint32_t type)
{
	if ((info = spa_debug_type_find(info, type)) == NULL)
		return NULL;
	return info->name;
}

SPA_API_DEBUG_TYPES const char *spa_debug_type_find_short_name(const struct spa_type_info *info, uint32_t type)
{
	const char *str;
	if ((str = spa_debug_type_find_name(info, type)) == NULL)
		return NULL;
	return spa_debug_type_short_name(str);
}

SPA_API_DEBUG_TYPES uint32_t spa_debug_type_find_type(const struct spa_type_info *info, const char *name)
{
	if (info == NULL)
		info = SPA_TYPE_ROOT;

	while (info && info->name) {
		uint32_t res;
		if (strcmp(info->name, name) == 0)
			return info->type;
		if (info->values && (res = spa_debug_type_find_type(info->values, name)) != SPA_ID_INVALID)
			return res;
		info++;
	}
	return SPA_ID_INVALID;
}

SPA_API_DEBUG_TYPES const struct spa_type_info *spa_debug_type_find_short(const struct spa_type_info *info, const char *name)
{
	while (info && info->name) {
		if (strcmp(spa_debug_type_short_name(info->name), name) == 0)
			return info;
		if (strcmp(info->name, name) == 0)
			return info;
		if (info->type != 0 && info->type == (uint32_t)atoi(name))
			return info;
		info++;
	}
	return NULL;
}

SPA_API_DEBUG_TYPES uint32_t spa_debug_type_find_type_short(const struct spa_type_info *info, const char *name)
{
	if ((info = spa_debug_type_find_short(info, name)) == NULL)
		return SPA_ID_INVALID;
	return info->type;
}
/**
 * \}
 */

#ifdef __cplusplus
}  /* extern "C" */
#endif

#endif /* SPA_DEBUG_NODE_H */
