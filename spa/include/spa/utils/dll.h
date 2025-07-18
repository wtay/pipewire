/* Simple DLL */
/* SPDX-FileCopyrightText: Copyright © 2019 Wim Taymans */
/* SPDX-License-Identifier: MIT */

#ifndef SPA_DLL_H
#define SPA_DLL_H

#include <stddef.h>
#include <math.h>

#include <spa/utils/defs.h>

#ifdef __cplusplus
extern "C" {
#endif

#ifndef SPA_API_DLL
 #ifdef SPA_API_IMPL
  #define SPA_API_DLL SPA_API_IMPL
 #else
  #define SPA_API_DLL static inline
 #endif
#endif

#define SPA_DLL_BW_MAX		0.128
#define SPA_DLL_BW_MIN		0.016

struct spa_dll {
	double bw;
	double z1, z2, z3;
	double w0, w1, w2;
};

SPA_API_DLL void spa_dll_init(struct spa_dll *dll)
{
	dll->bw = 0.0;
	dll->z1 = dll->z2 = dll->z3 = 0.0;
}

SPA_API_DLL void spa_dll_set_bw(struct spa_dll *dll, double bw, unsigned period, unsigned rate)
{
	double w = 2 * M_PI * bw * period / rate;
	dll->w0 = 1.0 - exp (-20.0 * w);
	dll->w1 = w * 1.5 / period;
	dll->w2 = w / 1.5;
	dll->bw = bw;
}

SPA_API_DLL double spa_dll_update(struct spa_dll *dll, double err)
{
	dll->z1 += dll->w0 * (dll->w1 * err - dll->z1);
	dll->z2 += dll->w0 * (dll->z1 - dll->z2);
	dll->z3 += dll->w2 * dll->z2;
	return 1.0 - (dll->z2 + dll->z3);
}

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* SPA_DLL_H */
