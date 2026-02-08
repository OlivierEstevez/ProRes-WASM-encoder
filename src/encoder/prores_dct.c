/*
 * ProRes DCT Implementation
 * Based on Loeffler algorithm from libjpeg (jfdctint.c)
 * Integer-only implementation for WebAssembly compatibility
 *
 * This is a slow-but-accurate integer DCT. For better accuracy at edges,
 * we use the proven libjpeg algorithm rather than faster approximations.
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
 * Perform the forward DCT on one 8x8 block of samples.
 * Based on libjpeg's jpeg_fdct_islow (jfdctint.c)
 */
void prores_fdct_8x8(int16_t* block)
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

void prores_fdct_8x8_stride(const int16_t* src, int16_t* dst, int stride)
{
    int16_t block[64];
    int i, j;

    /* Copy with stride to contiguous block */
    for (i = 0; i < 8; i++) {
        for (j = 0; j < 8; j++) {
            block[i * 8 + j] = src[i * stride + j];
        }
    }

    /* Perform DCT */
    prores_fdct_8x8(block);

    /* Copy back */
    for (i = 0; i < 64; i++) {
        dst[i] = block[i];
    }
}
