/*
 * ProRes DCT Implementation
 * Integer-only implementation for WebAssembly compatibility
 *
 * SPDX-License-Identifier: LGPL-2.1-or-later
 *
 * The 8x8 integer forward DCT is derived from libjpeg's jfdctint.c
 * (jpeg_fdct_islow), which implements the Loeffler-Ligtenberg-Moschytz
 * algorithm and is distributed under the permissive IJG License.
 *   Modifications for ProRes / WebAssembly:
 *   Copyright (c) 2026 Olivier Estévez
 *
 * This is a slow-but-accurate integer DCT. For better accuracy at edges,
 * we use the proven libjpeg algorithm rather than faster approximations.
 * See the LICENSE file for details.
 *
 * Two implementations of the same arithmetic:
 *  - a scalar reference (prores_fdct_8x8_scalar), and
 *  - a vectorized version using GCC/Clang portable vector extensions,
 *    which Emscripten lowers to WASM SIMD128 (with -msimd128) and native
 *    Clang/GCC lower to NEON/SSE. All operations are integer adds,
 *    multiplies and shifts on 32-bit lanes, so both paths are bit-exact
 *    with each other. Define PRORES_NO_SIMD to force the scalar path.
 */

#include "prores_dct.h"

/* DCT constants scaled by 2^13 (from libjpeg jfdctint.c) */
#define FIX_0_298631336  2446
#define FIX_0_390180644  3196
#define FIX_0_541196100  4433
#define FIX_0_765366865  6270
#define FIX_0_899976223  7373
#define FIX_1_175875602  9633
#define FIX_1_501321110  12299
#define FIX_1_847759065  15137
#define FIX_1_961570560  16069
#define FIX_2_053119869  16819
#define FIX_2_562915447  20995
#define FIX_3_072711026  25172

#define CONST_BITS  13
#define PASS1_BITS  1
#define OUT_SHIFT   (PASS1_BITS + 1)

#define MULTIPLY(a, b)  ((int32_t)(a) * (int32_t)(b))
#define DESCALE(x, n)  (((x) + (1 << ((n) - 1))) >> (n))

/*
 * Scalar reference: forward DCT on one 8x8 block, in place.
 * Based on libjpeg's jpeg_fdct_islow (jfdctint.c).
 */
void prores_fdct_8x8_scalar(int16_t* block)
{
    int32_t tmp0, tmp1, tmp2, tmp3, tmp4, tmp5, tmp6, tmp7;
    int32_t tmp10, tmp11, tmp12, tmp13;
    int32_t z1, z2, z3, z4, z5;
    int16_t* dataptr;
    int32_t workspace[64];
    int32_t* wsptr;
    int ctr;

    /* Pass 1: process rows */
    dataptr = block;
    wsptr = workspace;
    for (ctr = 0; ctr < 8; ctr++) {
        tmp0 = dataptr[0] + dataptr[7];
        tmp7 = dataptr[0] - dataptr[7];
        tmp1 = dataptr[1] + dataptr[6];
        tmp6 = dataptr[1] - dataptr[6];
        tmp2 = dataptr[2] + dataptr[5];
        tmp5 = dataptr[2] - dataptr[5];
        tmp3 = dataptr[3] + dataptr[4];
        tmp4 = dataptr[3] - dataptr[4];

        /* Even part per LL&M figure 1 */
        tmp10 = tmp0 + tmp3;
        tmp13 = tmp0 - tmp3;
        tmp11 = tmp1 + tmp2;
        tmp12 = tmp1 - tmp2;

        wsptr[0] = (tmp10 + tmp11) << PASS1_BITS;
        wsptr[4] = (tmp10 - tmp11) << PASS1_BITS;

        z1 = MULTIPLY(tmp12 + tmp13, FIX_0_541196100);
        wsptr[2] = DESCALE(z1 + MULTIPLY(tmp13, FIX_0_765366865), CONST_BITS - PASS1_BITS);
        wsptr[6] = DESCALE(z1 - MULTIPLY(tmp12, FIX_1_847759065), CONST_BITS - PASS1_BITS);

        /* Odd part per figure 8 */
        z1 = tmp4 + tmp7;
        z2 = tmp5 + tmp6;
        z3 = tmp4 + tmp6;
        z4 = tmp5 + tmp7;
        z5 = MULTIPLY(z3 + z4, FIX_1_175875602);

        tmp4 = MULTIPLY(tmp4, FIX_0_298631336);
        tmp5 = MULTIPLY(tmp5, FIX_2_053119869);
        tmp6 = MULTIPLY(tmp6, FIX_3_072711026);
        tmp7 = MULTIPLY(tmp7, FIX_1_501321110);
        z1 = MULTIPLY(z1, -FIX_0_899976223);
        z2 = MULTIPLY(z2, -FIX_2_562915447);
        z3 = MULTIPLY(z3, -FIX_1_961570560);
        z4 = MULTIPLY(z4, -FIX_0_390180644);

        z3 += z5;
        z4 += z5;

        wsptr[7] = DESCALE(tmp4 + z1 + z3, CONST_BITS - PASS1_BITS);
        wsptr[5] = DESCALE(tmp5 + z2 + z4, CONST_BITS - PASS1_BITS);
        wsptr[3] = DESCALE(tmp6 + z2 + z3, CONST_BITS - PASS1_BITS);
        wsptr[1] = DESCALE(tmp7 + z1 + z4, CONST_BITS - PASS1_BITS);

        dataptr += 8;
        wsptr += 8;
    }

    /* Pass 2: process columns */
    wsptr = workspace;
    dataptr = block;
    for (ctr = 0; ctr < 8; ctr++) {
        tmp0 = wsptr[0*8] + wsptr[7*8];
        tmp7 = wsptr[0*8] - wsptr[7*8];
        tmp1 = wsptr[1*8] + wsptr[6*8];
        tmp6 = wsptr[1*8] - wsptr[6*8];
        tmp2 = wsptr[2*8] + wsptr[5*8];
        tmp5 = wsptr[2*8] - wsptr[5*8];
        tmp3 = wsptr[3*8] + wsptr[4*8];
        tmp4 = wsptr[3*8] - wsptr[4*8];

        /* Even part */
        tmp10 = tmp0 + tmp3;
        tmp13 = tmp0 - tmp3;
        tmp11 = tmp1 + tmp2;
        tmp12 = tmp1 - tmp2;

        dataptr[0*8] = (int16_t)DESCALE(tmp10 + tmp11, OUT_SHIFT);
        dataptr[4*8] = (int16_t)DESCALE(tmp10 - tmp11, OUT_SHIFT);

        z1 = MULTIPLY(tmp12 + tmp13, FIX_0_541196100);
        dataptr[2*8] = (int16_t)DESCALE(z1 + MULTIPLY(tmp13, FIX_0_765366865), CONST_BITS + OUT_SHIFT);
        dataptr[6*8] = (int16_t)DESCALE(z1 - MULTIPLY(tmp12, FIX_1_847759065), CONST_BITS + OUT_SHIFT);

        /* Odd part */
        z1 = tmp4 + tmp7;
        z2 = tmp5 + tmp6;
        z3 = tmp4 + tmp6;
        z4 = tmp5 + tmp7;
        z5 = MULTIPLY(z3 + z4, FIX_1_175875602);

        tmp4 = MULTIPLY(tmp4, FIX_0_298631336);
        tmp5 = MULTIPLY(tmp5, FIX_2_053119869);
        tmp6 = MULTIPLY(tmp6, FIX_3_072711026);
        tmp7 = MULTIPLY(tmp7, FIX_1_501321110);
        z1 = MULTIPLY(z1, -FIX_0_899976223);
        z2 = MULTIPLY(z2, -FIX_2_562915447);
        z3 = MULTIPLY(z3, -FIX_1_961570560);
        z4 = MULTIPLY(z4, -FIX_0_390180644);

        z3 += z5;
        z4 += z5;

        dataptr[7*8] = (int16_t)DESCALE(tmp4 + z1 + z3, CONST_BITS + OUT_SHIFT);
        dataptr[5*8] = (int16_t)DESCALE(tmp5 + z2 + z4, CONST_BITS + OUT_SHIFT);
        dataptr[3*8] = (int16_t)DESCALE(tmp6 + z2 + z3, CONST_BITS + OUT_SHIFT);
        dataptr[1*8] = (int16_t)DESCALE(tmp7 + z1 + z4, CONST_BITS + OUT_SHIFT);

        wsptr++;
        dataptr++;
    }
}

#if !defined(PRORES_NO_SIMD) && (defined(__clang__) || defined(__GNUC__))
#define PRORES_DCT_VECTOR 1

typedef int16_t v8i16 __attribute__((vector_size(16)));
typedef int32_t v8i32 __attribute__((vector_size(32)));
/* Unaligned-load variant: plane rows are only 2-byte aligned in general */
typedef int16_t v8i16_u __attribute__((vector_size(16), aligned(2)));

#define VSHUF(a, b, ...) __builtin_shufflevector(a, b, __VA_ARGS__)

/* Transpose 8 vectors of 8 int16 lanes (rows in, columns out) */
static inline void transpose8x8_i16(v8i16 r[8])
{
    v8i16 p0 = VSHUF(r[0], r[1], 0, 8, 1, 9, 2, 10, 3, 11);
    v8i16 p1 = VSHUF(r[0], r[1], 4, 12, 5, 13, 6, 14, 7, 15);
    v8i16 p2 = VSHUF(r[2], r[3], 0, 8, 1, 9, 2, 10, 3, 11);
    v8i16 p3 = VSHUF(r[2], r[3], 4, 12, 5, 13, 6, 14, 7, 15);
    v8i16 p4 = VSHUF(r[4], r[5], 0, 8, 1, 9, 2, 10, 3, 11);
    v8i16 p5 = VSHUF(r[4], r[5], 4, 12, 5, 13, 6, 14, 7, 15);
    v8i16 p6 = VSHUF(r[6], r[7], 0, 8, 1, 9, 2, 10, 3, 11);
    v8i16 p7 = VSHUF(r[6], r[7], 4, 12, 5, 13, 6, 14, 7, 15);

    v8i16 q0 = VSHUF(p0, p2, 0, 1, 8, 9, 2, 3, 10, 11);
    v8i16 q1 = VSHUF(p0, p2, 4, 5, 12, 13, 6, 7, 14, 15);
    v8i16 q2 = VSHUF(p1, p3, 0, 1, 8, 9, 2, 3, 10, 11);
    v8i16 q3 = VSHUF(p1, p3, 4, 5, 12, 13, 6, 7, 14, 15);
    v8i16 q4 = VSHUF(p4, p6, 0, 1, 8, 9, 2, 3, 10, 11);
    v8i16 q5 = VSHUF(p4, p6, 4, 5, 12, 13, 6, 7, 14, 15);
    v8i16 q6 = VSHUF(p5, p7, 0, 1, 8, 9, 2, 3, 10, 11);
    v8i16 q7 = VSHUF(p5, p7, 4, 5, 12, 13, 6, 7, 14, 15);

    r[0] = VSHUF(q0, q4, 0, 1, 2, 3, 8, 9, 10, 11);
    r[1] = VSHUF(q0, q4, 4, 5, 6, 7, 12, 13, 14, 15);
    r[2] = VSHUF(q1, q5, 0, 1, 2, 3, 8, 9, 10, 11);
    r[3] = VSHUF(q1, q5, 4, 5, 6, 7, 12, 13, 14, 15);
    r[4] = VSHUF(q2, q6, 0, 1, 2, 3, 8, 9, 10, 11);
    r[5] = VSHUF(q2, q6, 4, 5, 6, 7, 12, 13, 14, 15);
    r[6] = VSHUF(q3, q7, 0, 1, 2, 3, 8, 9, 10, 11);
    r[7] = VSHUF(q3, q7, 4, 5, 6, 7, 12, 13, 14, 15);
}

/* Same butterfly pattern on 8 vectors of 8 int32 lanes */
static inline void transpose8x8_i32(v8i32 r[8])
{
    v8i32 p0 = VSHUF(r[0], r[1], 0, 8, 1, 9, 2, 10, 3, 11);
    v8i32 p1 = VSHUF(r[0], r[1], 4, 12, 5, 13, 6, 14, 7, 15);
    v8i32 p2 = VSHUF(r[2], r[3], 0, 8, 1, 9, 2, 10, 3, 11);
    v8i32 p3 = VSHUF(r[2], r[3], 4, 12, 5, 13, 6, 14, 7, 15);
    v8i32 p4 = VSHUF(r[4], r[5], 0, 8, 1, 9, 2, 10, 3, 11);
    v8i32 p5 = VSHUF(r[4], r[5], 4, 12, 5, 13, 6, 14, 7, 15);
    v8i32 p6 = VSHUF(r[6], r[7], 0, 8, 1, 9, 2, 10, 3, 11);
    v8i32 p7 = VSHUF(r[6], r[7], 4, 12, 5, 13, 6, 14, 7, 15);

    v8i32 q0 = VSHUF(p0, p2, 0, 1, 8, 9, 2, 3, 10, 11);
    v8i32 q1 = VSHUF(p0, p2, 4, 5, 12, 13, 6, 7, 14, 15);
    v8i32 q2 = VSHUF(p1, p3, 0, 1, 8, 9, 2, 3, 10, 11);
    v8i32 q3 = VSHUF(p1, p3, 4, 5, 12, 13, 6, 7, 14, 15);
    v8i32 q4 = VSHUF(p4, p6, 0, 1, 8, 9, 2, 3, 10, 11);
    v8i32 q5 = VSHUF(p4, p6, 4, 5, 12, 13, 6, 7, 14, 15);
    v8i32 q6 = VSHUF(p5, p7, 0, 1, 8, 9, 2, 3, 10, 11);
    v8i32 q7 = VSHUF(p5, p7, 4, 5, 12, 13, 6, 7, 14, 15);

    r[0] = VSHUF(q0, q4, 0, 1, 2, 3, 8, 9, 10, 11);
    r[1] = VSHUF(q0, q4, 4, 5, 6, 7, 12, 13, 14, 15);
    r[2] = VSHUF(q1, q5, 0, 1, 2, 3, 8, 9, 10, 11);
    r[3] = VSHUF(q1, q5, 4, 5, 6, 7, 12, 13, 14, 15);
    r[4] = VSHUF(q2, q6, 0, 1, 2, 3, 8, 9, 10, 11);
    r[5] = VSHUF(q2, q6, 4, 5, 6, 7, 12, 13, 14, 15);
    r[6] = VSHUF(q3, q7, 0, 1, 2, 3, 8, 9, 10, 11);
    r[7] = VSHUF(q3, q7, 4, 5, 6, 7, 12, 13, 14, 15);
}

#define DESCALE_V(x, n)  (((x) + (1 << ((n) - 1))) >> (n))

/* One butterfly pass over 8 lanes. in[0..7] are the 8 values of each
 * row (pass 1) or column (pass 2), one row/column per lane. shift_lo is
 * how outputs 0/4 are scaled, descale_* the DESCALE amounts — matching
 * the scalar code's two passes exactly. */
static inline void fdct_pass_v(const v8i32 in[8], v8i32 out[8],
                               int pass1)
{
    v8i32 tmp0 = in[0] + in[7];
    v8i32 tmp7 = in[0] - in[7];
    v8i32 tmp1 = in[1] + in[6];
    v8i32 tmp6 = in[1] - in[6];
    v8i32 tmp2 = in[2] + in[5];
    v8i32 tmp5 = in[2] - in[5];
    v8i32 tmp3 = in[3] + in[4];
    v8i32 tmp4 = in[3] - in[4];

    /* Even part per LL&M figure 1 */
    v8i32 tmp10 = tmp0 + tmp3;
    v8i32 tmp13 = tmp0 - tmp3;
    v8i32 tmp11 = tmp1 + tmp2;
    v8i32 tmp12 = tmp1 - tmp2;

    if (pass1) {
        out[0] = (tmp10 + tmp11) << PASS1_BITS;
        out[4] = (tmp10 - tmp11) << PASS1_BITS;
    } else {
        out[0] = DESCALE_V(tmp10 + tmp11, OUT_SHIFT);
        out[4] = DESCALE_V(tmp10 - tmp11, OUT_SHIFT);
    }

    int ds = pass1 ? (CONST_BITS - PASS1_BITS) : (CONST_BITS + OUT_SHIFT);

    v8i32 z1 = (tmp12 + tmp13) * FIX_0_541196100;
    out[2] = DESCALE_V(z1 + tmp13 * FIX_0_765366865, ds);
    out[6] = DESCALE_V(z1 - tmp12 * FIX_1_847759065, ds);

    /* Odd part per figure 8 */
    z1 = tmp4 + tmp7;
    v8i32 z2 = tmp5 + tmp6;
    v8i32 z3 = tmp4 + tmp6;
    v8i32 z4 = tmp5 + tmp7;
    v8i32 z5 = (z3 + z4) * FIX_1_175875602;

    tmp4 = tmp4 * FIX_0_298631336;
    tmp5 = tmp5 * FIX_2_053119869;
    tmp6 = tmp6 * FIX_3_072711026;
    tmp7 = tmp7 * FIX_1_501321110;
    z1 = z1 * -FIX_0_899976223;
    z2 = z2 * -FIX_2_562915447;
    z3 = z3 * -FIX_1_961570560;
    z4 = z4 * -FIX_0_390180644;

    z3 += z5;
    z4 += z5;

    out[7] = DESCALE_V(tmp4 + z1 + z3, ds);
    out[5] = DESCALE_V(tmp5 + z2 + z4, ds);
    out[3] = DESCALE_V(tmp6 + z2 + z3, ds);
    out[1] = DESCALE_V(tmp7 + z1 + z4, ds);
}

/* Vectorized FDCT: src is an 8x8 block at the given stride (in int16
 * elements), dst is a contiguous 64-coefficient block. Bit-exact with
 * prores_fdct_8x8_scalar. */
static void fdct_8x8_vec(const int16_t* src, int stride, int16_t* dst)
{
    v8i16 rows[8];
    for (int i = 0; i < 8; i++)
        rows[i] = *(const v8i16_u*)(src + i * stride);

    /* Lane j = row j; vector i = element i of every row */
    transpose8x8_i16(rows);

    v8i32 e[8], w[8];
    for (int i = 0; i < 8; i++)
        e[i] = __builtin_convertvector(rows[i], v8i32);

    /* Pass 1 (rows), all 8 rows in lanes */
    fdct_pass_v(e, w, 1);

    /* w[k] lane j = workspace[j*8+k]; pass 2 needs lanes = columns */
    transpose8x8_i32(w);

    v8i32 d[8];
    fdct_pass_v(w, d, 0);

    for (int k = 0; k < 8; k++)
        *(v8i16_u*)(dst + k * 8) = __builtin_convertvector(d[k], v8i16);
}
#endif /* vector extensions */

void prores_fdct_8x8(int16_t* block)
{
#ifdef PRORES_DCT_VECTOR
    fdct_8x8_vec(block, 8, block);
#else
    prores_fdct_8x8_scalar(block);
#endif
}

void prores_fdct_8x8_stride(const int16_t* src, int16_t* dst, int stride)
{
#ifdef PRORES_DCT_VECTOR
    fdct_8x8_vec(src, stride, dst);
#else
    int i, j;
    for (i = 0; i < 8; i++) {
        for (j = 0; j < 8; j++) {
            dst[i * 8 + j] = src[i * stride + j];
        }
    }
    prores_fdct_8x8_scalar(dst);
#endif
}
