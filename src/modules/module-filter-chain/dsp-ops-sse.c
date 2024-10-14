/* Spa */
/* SPDX-FileCopyrightText: Copyright © 2022 Wim Taymans */
/* SPDX-License-Identifier: MIT */

#include <string.h>
#include <stdio.h>
#include <math.h>
#include <float.h>

#include <spa/utils/defs.h>

#include "dsp-ops.h"

#include <xmmintrin.h>

void dsp_mix_gain_sse(struct dsp_ops *ops,
		void * SPA_RESTRICT dst,
		const void * SPA_RESTRICT src[],
		float gain[], uint32_t n_src, uint32_t n_samples)
{
	if (n_src == 0) {
		memset(dst, 0, n_samples * sizeof(float));
	} else if (n_src == 1 && gain[0] == 1.0f) {
		if (dst != src[0])
			spa_memcpy(dst, src[0], n_samples * sizeof(float));
	} else {
		uint32_t n, i, unrolled;
		__m128 in[4], g;
		const float **s = (const float **)src;
		float *d = dst;

		if (SPA_LIKELY(SPA_IS_ALIGNED(dst, 16))) {
			unrolled = n_samples & ~15;
			for (i = 0; i < n_src; i++) {
				if (SPA_UNLIKELY(!SPA_IS_ALIGNED(src[i], 16))) {
					unrolled = 0;
					break;
				}
			}
		} else
			unrolled = 0;

		for (n = 0; n < unrolled; n += 16) {
			g = _mm_set1_ps(gain[0]);
			in[0] = _mm_mul_ps(g, _mm_load_ps(&s[0][n+ 0]));
			in[1] = _mm_mul_ps(g, _mm_load_ps(&s[0][n+ 4]));
			in[2] = _mm_mul_ps(g, _mm_load_ps(&s[0][n+ 8]));
			in[3] = _mm_mul_ps(g, _mm_load_ps(&s[0][n+12]));

			for (i = 1; i < n_src; i++) {
				g = _mm_set1_ps(gain[i]);
				in[0] = _mm_add_ps(in[0], _mm_mul_ps(g, _mm_load_ps(&s[i][n+ 0])));
				in[1] = _mm_add_ps(in[1], _mm_mul_ps(g, _mm_load_ps(&s[i][n+ 4])));
				in[2] = _mm_add_ps(in[2], _mm_mul_ps(g, _mm_load_ps(&s[i][n+ 8])));
				in[3] = _mm_add_ps(in[3], _mm_mul_ps(g, _mm_load_ps(&s[i][n+12])));
			}
			_mm_store_ps(&d[n+ 0], in[0]);
			_mm_store_ps(&d[n+ 4], in[1]);
			_mm_store_ps(&d[n+ 8], in[2]);
			_mm_store_ps(&d[n+12], in[3]);
		}
		for (; n < n_samples; n++) {
			g = _mm_set_ss(gain[0]);
			in[0] = _mm_mul_ss(g, _mm_load_ss(&s[0][n]));
			for (i = 1; i < n_src; i++) {
				g = _mm_set_ss(gain[i]);
				in[0] = _mm_add_ss(in[0], _mm_mul_ss(g, _mm_load_ss(&s[i][n])));
			}
			_mm_store_ss(&d[n], in[0]);
		}
	}
}

void dsp_sum_sse(struct dsp_ops *ops, float *r, const float *a, const float *b, uint32_t n_samples)
{
	uint32_t n, unrolled;
	__m128 in[4];

	unrolled = n_samples & ~15;

	if (SPA_LIKELY(SPA_IS_ALIGNED(r, 16)) &&
	    SPA_LIKELY(SPA_IS_ALIGNED(a, 16)) &&
	    SPA_LIKELY(SPA_IS_ALIGNED(b, 16))) {
		for (n = 0; n < unrolled; n += 16) {
			in[0] = _mm_load_ps(&a[n+ 0]);
			in[1] = _mm_load_ps(&a[n+ 4]);
			in[2] = _mm_load_ps(&a[n+ 8]);
			in[3] = _mm_load_ps(&a[n+12]);

			in[0] = _mm_add_ps(in[0], _mm_load_ps(&b[n+ 0]));
			in[1] = _mm_add_ps(in[1], _mm_load_ps(&b[n+ 4]));
			in[2] = _mm_add_ps(in[2], _mm_load_ps(&b[n+ 8]));
			in[3] = _mm_add_ps(in[3], _mm_load_ps(&b[n+12]));

			_mm_store_ps(&r[n+ 0], in[0]);
			_mm_store_ps(&r[n+ 4], in[1]);
			_mm_store_ps(&r[n+ 8], in[2]);
			_mm_store_ps(&r[n+12], in[3]);
		}
	} else {
		for (n = 0; n < unrolled; n += 16) {
			in[0] = _mm_loadu_ps(&a[n+ 0]);
			in[1] = _mm_loadu_ps(&a[n+ 4]);
			in[2] = _mm_loadu_ps(&a[n+ 8]);
			in[3] = _mm_loadu_ps(&a[n+12]);

			in[0] = _mm_add_ps(in[0], _mm_loadu_ps(&b[n+ 0]));
			in[1] = _mm_add_ps(in[1], _mm_loadu_ps(&b[n+ 4]));
			in[2] = _mm_add_ps(in[2], _mm_loadu_ps(&b[n+ 8]));
			in[3] = _mm_add_ps(in[3], _mm_loadu_ps(&b[n+12]));

			_mm_storeu_ps(&r[n+ 0], in[0]);
			_mm_storeu_ps(&r[n+ 4], in[1]);
			_mm_storeu_ps(&r[n+ 8], in[2]);
			_mm_storeu_ps(&r[n+12], in[3]);
		}
	}
	for (; n < n_samples; n++) {
		in[0] = _mm_load_ss(&a[n]);
		in[0] = _mm_add_ss(in[0], _mm_load_ss(&b[n]));
		_mm_store_ss(&r[n], in[0]);
	}
}

void dsp_biquad_run_sse(struct dsp_ops *ops, struct biquad *bq,
		float *out, const float *in, uint32_t n_samples)
{
	__m128 x, y, z;
	__m128 b012;
	__m128 a12;
	__m128 x12;
	uint32_t i;

	b012 = _mm_setr_ps(bq->b0, bq->b1, bq->b2, 0.0f);  /* b0  b1  b2  0 */
	a12 = _mm_setr_ps(0.0f, bq->a1, bq->a2, 0.0f);	  /* 0   a1  a2  0 */
	x12 = _mm_setr_ps(bq->x1, bq->x2, 0.0f, 0.0f);	  /* x1  x2  0   0 */

	for (i = 0; i < n_samples; i++) {
		x = _mm_load1_ps(&in[i]);		/*  x         x         x      x */
		z = _mm_mul_ps(x, b012);		/*  b0*x      b1*x      b2*x   0 */
		z = _mm_add_ps(z, x12); 		/*  b0*x+x1   b1*x+x2   b2*x   0 */
		_mm_store_ss(&out[i], z);		/*  out[i] = b0*x+x1 */
		y = _mm_shuffle_ps(z, z, _MM_SHUFFLE(0,0,0,0));	/*  b0*x+x1  b0*x+x1  b0*x+x1  b0*x+x1 = y*/
		y = _mm_mul_ps(y, a12);		        /*  0        a1*y     a2*y     0 */
		y = _mm_sub_ps(z, y);	 		/*  y        x1       x2       0 */
		x12 = _mm_shuffle_ps(y, y, _MM_SHUFFLE(3,3,2,1));    /*  x1  x2  0  0*/
	}
#define F(x) (-FLT_MIN < (x) && (x) < FLT_MIN ? 0.0f : (x))
	bq->x1 = F(x12[0]);
	bq->x2 = F(x12[1]);
#undef F
}

static void dsp_biquad2_run_sse(struct dsp_ops *ops, struct biquad *bq0, struct biquad *bq1,
		float *out, const float *in, uint32_t n_samples)
{
	__m128 x, y, z;
	__m128 b0, b1;
	__m128 a0, a1;
	__m128 x0, x1;
	uint32_t i;

	b0 = _mm_setr_ps(bq0->b0, bq0->b1, bq0->b2, 0.0f);  /* b0  b1  b2  0 */
	a0 = _mm_setr_ps(0.0f, bq0->a1, bq0->a2, 0.0f);	    /* 0   a1  a2  0 */
	x0 = _mm_setr_ps(bq0->x1, bq0->x2, 0.0f, 0.0f);	    /* x1  x2  0   0 */

	b1 = _mm_setr_ps(bq1->b0, bq1->b1, bq1->b2, 0.0f);  /* b0  b1  b2  0 */
	a1 = _mm_setr_ps(0.0f, bq1->a1, bq1->a2, 0.0f);	    /* 0   a1  a2  0 */
	x1 = _mm_setr_ps(bq1->x1, bq1->x2, 0.0f, 0.0f);	    /* x1  x2  0   0 */

	for (i = 0; i < n_samples; i++) {
		x = _mm_load1_ps(&in[i]);			/*  x         x         x      x */

		z = _mm_mul_ps(x, b0);				/*  b0*x      b1*x      b2*x   0 */
		z = _mm_add_ps(z, x0); 				/*  b0*x+x1   b1*x+x2   b2*x   0 */
		y = _mm_shuffle_ps(z, z, _MM_SHUFFLE(0,0,0,0));	/*  b0*x+x1  b0*x+x1  b0*x+x1  b0*x+x1 = y*/
		x = _mm_mul_ps(y, a0);			        /*  0        a1*y     a2*y     0 */
		x = _mm_sub_ps(z, x);	 			/*  y        x1       x2       0 */
		x0 = _mm_shuffle_ps(x, x, _MM_SHUFFLE(3,3,2,1));    /*  x1  x2  0  0*/

		z = _mm_mul_ps(y, b1);				/*  b0*x      b1*x      b2*x   0 */
		z = _mm_add_ps(z, x1); 				/*  b0*x+x1   b1*x+x2   b2*x   0 */
		x = _mm_shuffle_ps(z, z, _MM_SHUFFLE(0,0,0,0));	/*  b0*x+x1  b0*x+x1  b0*x+x1  b0*x+x1 = y*/
		y = _mm_mul_ps(x, a1);			        /*  0        a1*y     a2*y     0 */
		y = _mm_sub_ps(z, y);	 			/*  y        x1       x2       0 */
		x1 = _mm_shuffle_ps(y, y, _MM_SHUFFLE(3,3,2,1));    /*  x1  x2  0  0*/

		_mm_store_ss(&out[i], x);			/*  out[i] = b0*x+x1 */
	}
#define F(x) (-FLT_MIN < (x) && (x) < FLT_MIN ? 0.0f : (x))
	bq0->x1 = F(x0[0]);
	bq0->x2 = F(x0[1]);
	bq1->x1 = F(x1[0]);
	bq1->x2 = F(x1[1]);
#undef F
}

static void dsp_biquad_run2_sse(struct dsp_ops *ops, struct biquad *bqL, struct biquad *bqR,
		float *outL, float *outR, const float *inL, const float *inR, uint32_t n_samples)
{
	__m128 x, y, z;
	__m128 b0, b1, b2;
	__m128 a1, a2;
	__m128 x1, x2;
	uint32_t i;

	b0 = _mm_setr_ps(bqL->b0, bqR->b0, 0.0f, 0.0f);  /* b00  b10  0  0 */
	b1 = _mm_setr_ps(bqL->b1, bqR->b1, 0.0f, 0.0f);  /* b01  b11  0  0 */
	b2 = _mm_setr_ps(bqL->b2, bqR->b2, 0.0f, 0.0f);  /* b02  b12  0  0 */
	a1 = _mm_setr_ps(bqL->a1, bqR->a1, 0.0f, 0.0f);  /* b00  b10  0  0 */
	a2 = _mm_setr_ps(bqL->a2, bqR->a2, 0.0f, 0.0f);  /* b01  b11  0  0 */
	x1 = _mm_setr_ps(bqL->x1, bqR->x1, 0.0f, 0.0f);  /* b00  b10  0  0 */
	x2 = _mm_setr_ps(bqL->x2, bqR->x2, 0.0f, 0.0f);  /* b01  b11  0  0 */

	for (i = 0; i < n_samples; i++) {
		x = _mm_setr_ps(inL[i], inR[i], 0.0f, 0.0f);

		y = _mm_mul_ps(x, b0);		/* y = x * b0 */
		y = _mm_add_ps(y, x1);		/* y = x * b0 + x1*/
		z = _mm_mul_ps(y, a1);		/* z = a1 * y */
		x1 = _mm_mul_ps(x, b1);		/* x1 = x * b1 */
		x1 = _mm_add_ps(x1, x2);	/* x1 = x * b1 + x2*/
		x1 = _mm_sub_ps(x1, z);		/* x1 = x * b1 + x2 - a1 * y*/
		z = _mm_mul_ps(y, a2);		/* z = a2 * y */
		x2 = _mm_mul_ps(x, b2);		/* x2 = x * b2 */
		x2 = _mm_sub_ps(x2, z);		/* x2 = x * b2 - a2 * y*/

		outL[i] = y[0];
		outR[i] = y[1];
	}
#define F(x) (-FLT_MIN < (x) && (x) < FLT_MIN ? 0.0f : (x))
	bqL->x1 = F(x1[0]);
	bqL->x2 = F(x2[0]);
	bqR->x1 = F(x1[1]);
	bqR->x2 = F(x2[1]);
#undef F
}


static void dsp_biquad2_run2_sse(struct dsp_ops *ops, struct biquad *bqL0, struct biquad *bqL1,
		struct biquad *bqR0, struct biquad *bqR1,
		float *outL, float *outR, const float *inL, const float *inR, uint32_t n_samples)
{
	__m128 x, y, z;
	__m128 b00, b01, b02, b10, b11, b12;
	__m128 a01, a02, a11, a12;
	__m128 x01, x02, x11, x12;
	uint32_t i;

	b00 = _mm_setr_ps(bqL0->b0, bqR0->b0, 0.0f, 0.0f);  /* b00  b10  0  0 */
	b01 = _mm_setr_ps(bqL0->b1, bqR0->b1, 0.0f, 0.0f);  /* b01  b11  0  0 */
	b02 = _mm_setr_ps(bqL0->b2, bqR0->b2, 0.0f, 0.0f);  /* b02  b12  0  0 */
	a01 = _mm_setr_ps(bqL0->a1, bqR0->a1, 0.0f, 0.0f);  /* b00  b10  0  0 */
	a02 = _mm_setr_ps(bqL0->a2, bqR0->a2, 0.0f, 0.0f);  /* b01  b11  0  0 */
	x01 = _mm_setr_ps(bqL0->x1, bqR0->x1, 0.0f, 0.0f);  /* b00  b10  0  0 */
	x02 = _mm_setr_ps(bqL0->x2, bqR0->x2, 0.0f, 0.0f);  /* b01  b11  0  0 */

	b10 = _mm_setr_ps(bqL1->b0, bqR1->b0, 0.0f, 0.0f);  /* b00  b10  0  0 */
	b11 = _mm_setr_ps(bqL1->b1, bqR1->b1, 0.0f, 0.0f);  /* b01  b11  0  0 */
	b12 = _mm_setr_ps(bqL1->b2, bqR1->b2, 0.0f, 0.0f);  /* b02  b12  0  0 */
	a11 = _mm_setr_ps(bqL1->a1, bqR1->a1, 0.0f, 0.0f);  /* b00  b10  0  0 */
	a12 = _mm_setr_ps(bqL1->a2, bqR1->a2, 0.0f, 0.0f);  /* b01  b11  0  0 */
	x11 = _mm_setr_ps(bqL1->x1, bqR1->x1, 0.0f, 0.0f);  /* b00  b10  0  0 */
	x12 = _mm_setr_ps(bqL1->x2, bqR1->x2, 0.0f, 0.0f);  /* b01  b11  0  0 */

	for (i = 0; i < n_samples; i++) {
		x = _mm_setr_ps(inL[i], inR[i], 0.0f, 0.0f);

		y = _mm_mul_ps(x, b00);		/* y = x * b0 */
		y = _mm_add_ps(y, x01);		/* y = x * b0 + x1*/
		z = _mm_mul_ps(y, a01);		/* z = a1 * y */
		x01 = _mm_mul_ps(x, b01);	/* x1 = x * b1 */
		x01 = _mm_add_ps(x01, x02);	/* x1 = x * b1 + x2*/
		x01 = _mm_sub_ps(x01, z);	/* x1 = x * b1 + x2 - a1 * y*/
		z = _mm_mul_ps(y, a02);		/* z = a2 * y */
		x02 = _mm_mul_ps(x, b02);	/* x2 = x * b2 */
		x02 = _mm_sub_ps(x02, z);	/* x2 = x * b2 - a2 * y*/

		x = y;

		y = _mm_mul_ps(x, b10);		/* y = x * b0 */
		y = _mm_add_ps(y, x11);		/* y = x * b0 + x1*/
		z = _mm_mul_ps(y, a11);		/* z = a1 * y */
		x11 = _mm_mul_ps(x, b11);	/* x1 = x * b1 */
		x11 = _mm_add_ps(x11, x12);	/* x1 = x * b1 + x2*/
		x11 = _mm_sub_ps(x11, z);	/* x1 = x * b1 + x2 - a1 * y*/
		z = _mm_mul_ps(y, a12);		/* z = a2 * y*/
		x12 = _mm_mul_ps(x, b12);	/* x2 = x * b2 */
		x12 = _mm_sub_ps(x12, z);	/* x2 = x * b2 - a2 * y*/

		outL[i] = y[0];
		outR[i] = y[1];
	}
#define F(x) (-FLT_MIN < (x) && (x) < FLT_MIN ? 0.0f : (x))
	bqL0->x1 = F(x01[0]);
	bqL0->x2 = F(x02[0]);
	bqR0->x1 = F(x01[1]);
	bqR0->x2 = F(x02[1]);
	bqL1->x1 = F(x11[0]);
	bqL1->x2 = F(x12[0]);
	bqR1->x1 = F(x11[1]);
	bqR1->x2 = F(x12[1]);
#undef F
}

void dsp_biquadn_run_sse(struct dsp_ops *ops, struct biquad *bq, uint32_t n_bq, uint32_t bq_stride,
		float * SPA_RESTRICT out[], const float * SPA_RESTRICT in[],
		uint32_t n_src, uint32_t n_samples)
{
	uint32_t i, j;
	uint32_t iunrolled = n_src & ~1;
	uint32_t junrolled = n_bq & ~1;

	for (i = 0; i < iunrolled; i+=2, bq+=bq_stride) {
		const float *s0 = in[i], *s1 = in[i+1];
		float *d0 = out[i], *d1 = out[i+1];
		if (s0 == NULL || s1 == NULL || d0 == NULL || d1 == NULL)
			break;

		for (j = 0; j < junrolled; j+=2) {
			dsp_biquad2_run2_sse(ops, &bq[j], &bq[j+1], &bq[j+bq_stride], &bq[j+bq_stride+1],
					d0, d1, s0, s1, n_samples);
			s0 = d0;
			s1 = d1;
		}
		for (; j < n_bq; j++) {
			dsp_biquad_run2_sse(ops, &bq[j], &bq[j+bq_stride], d0, d1, s0, s1, n_samples);
			s0 = d0;
			s1 = d1;
		}
	}
	for (; i < n_src; i++, bq+=bq_stride) {
		const float *s = in[i];
		float *d = out[i];
		if (s == NULL || d == NULL)
			continue;

		for (j = 0; j < junrolled; j+=2) {
			dsp_biquad2_run_sse(ops, &bq[j], &bq[j+1], d, s, n_samples);
			s = d;
		}
		for (; j < n_bq; j++) {
			dsp_biquad_run_sse(ops, &bq[j], d, s, n_samples);
			s = d;
		}
	}
}
