/* Simple Plugin API */
/* SPDX-FileCopyrightText: Copyright © 2018 Wim Taymans */
/* SPDX-License-Identifier: MIT */

#ifndef SPA_PARAM_FORMAT_UTILS_H
#define SPA_PARAM_FORMAT_UTILS_H

#include <spa/pod/parser.h>
#include <spa/param/format.h>

#ifdef __cplusplus
extern "C" {
#endif


/**
 * \addtogroup spa_param
 * \{
 */

#ifndef SPA_API_FORMAT_UTILS
 #ifdef SPA_API_IMPL
  #define SPA_API_FORMAT_UTILS SPA_API_IMPL
 #else
  #define SPA_API_FORMAT_UTILS static inline
 #endif
#endif

SPA_API_FORMAT_UTILS int
spa_format_parse(const struct spa_pod *format, uint32_t *media_type, uint32_t *media_subtype)
{
	return spa_pod_parse_object(format,
		SPA_TYPE_OBJECT_Format, NULL,
		SPA_FORMAT_mediaType,    SPA_POD_Id(media_type),
		SPA_FORMAT_mediaSubtype, SPA_POD_Id(media_subtype));
}

/**
 * \}
 */

#ifdef __cplusplus
}  /* extern "C" */
#endif

#endif /* SPA_PARAM_FORMAT_UTILS_H */
