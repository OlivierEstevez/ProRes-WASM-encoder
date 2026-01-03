/*
 * ProRes DCT Implementation
 * Based on AAN (Arai-Agui-Nakajima) fast DCT algorithm
 * Integer-only implementation for WebAssembly compatibility
 */

#include "prores_dct.h"

/* DCT constants scaled by 2^14 */
#define FIX_0_382683433  6270   /* cos(3*pi/8) * 2^14 */
#define FIX_0_541196100  8867   /* cos(3*pi/8) + cos(pi/8) / 2 */
#define FIX_0_707106781  11585  /* 1/sqrt(2) * 2^14 */
#define FIX_1_306562965  21407  /* cos(pi/8) * 2^14 */

#define CONST_BITS  14
#define PASS1_BITS  2

#define DESCALE(x, n)  (((x) + (1 << ((n) - 1))) >> (n))

/*
 * 1D DCT on 8 elements (row or column)
 */
static void fdct_1d(int32_t* data)
{
    int32_t tmp0, tmp1, tmp2, tmp3, tmp4, tmp5, tmp6, tmp7;
    int32_t tmp10, tmp11, tmp12, tmp13;
    int32_t z1, z2, z3, z4, z5;

    /* Even part */
    tmp0 = data[0] + data[7];
    tmp7 = data[0] - data[7];
    tmp1 = data[1] + data[6];
    tmp6 = data[1] - data[6];
    tmp2 = data[2] + data[5];
    tmp5 = data[2] - data[5];
    tmp3 = data[3] + data[4];
    tmp4 = data[3] - data[4];

    tmp10 = tmp0 + tmp3;
    tmp13 = tmp0 - tmp3;
    tmp11 = tmp1 + tmp2;
    tmp12 = tmp1 - tmp2;

    data[0] = tmp10 + tmp11;
    data[4] = tmp10 - tmp11;

    z1 = (tmp12 + tmp13) * FIX_0_707106781;
    data[2] = tmp13 + DESCALE(z1, CONST_BITS);
    data[6] = tmp13 - DESCALE(z1, CONST_BITS);

    /* Odd part */
    tmp10 = tmp4 + tmp5;
    tmp11 = tmp5 + tmp6;
    tmp12 = tmp6 + tmp7;

    z5 = (tmp10 - tmp12) * FIX_0_382683433;
    z2 = tmp10 * FIX_0_541196100 + z5;
    z4 = tmp12 * FIX_1_306562965 + z5;
    z3 = tmp11 * FIX_0_707106781;

    z1 = tmp7 + DESCALE(z3, CONST_BITS);
    z2 = DESCALE(z2, CONST_BITS);
    z3 = DESCALE(z3, CONST_BITS);
    z4 = DESCALE(z4, CONST_BITS);

    data[7] = tmp7 - z3 - z4;
    data[5] = tmp7 - z3 + z2;
    data[3] = z1 + z4;
    data[1] = z1 + z2;
}

void prores_fdct_8x8(int16_t* block)
{
    int32_t workspace[64];
    int32_t* wsptr = workspace;
    int16_t* blkptr = block;
    int ctr;

    /* Pass 1: process rows */
    for (ctr = 0; ctr < 8; ctr++) {
        int32_t row[8];
        row[0] = blkptr[0];
        row[1] = blkptr[1];
        row[2] = blkptr[2];
        row[3] = blkptr[3];
        row[4] = blkptr[4];
        row[5] = blkptr[5];
        row[6] = blkptr[6];
        row[7] = blkptr[7];

        fdct_1d(row);

        wsptr[0] = row[0] << PASS1_BITS;
        wsptr[1] = row[1] << PASS1_BITS;
        wsptr[2] = row[2] << PASS1_BITS;
        wsptr[3] = row[3] << PASS1_BITS;
        wsptr[4] = row[4] << PASS1_BITS;
        wsptr[5] = row[5] << PASS1_BITS;
        wsptr[6] = row[6] << PASS1_BITS;
        wsptr[7] = row[7] << PASS1_BITS;

        blkptr += 8;
        wsptr += 8;
    }

    /* Pass 2: process columns */
    wsptr = workspace;
    blkptr = block;
    for (ctr = 0; ctr < 8; ctr++) {
        int32_t col[8];
        col[0] = wsptr[0*8];
        col[1] = wsptr[1*8];
        col[2] = wsptr[2*8];
        col[3] = wsptr[3*8];
        col[4] = wsptr[4*8];
        col[5] = wsptr[5*8];
        col[6] = wsptr[6*8];
        col[7] = wsptr[7*8];

        fdct_1d(col);

        blkptr[0*8] = (int16_t)DESCALE(col[0], PASS1_BITS + 3);
        blkptr[1*8] = (int16_t)DESCALE(col[1], PASS1_BITS + 3);
        blkptr[2*8] = (int16_t)DESCALE(col[2], PASS1_BITS + 3);
        blkptr[3*8] = (int16_t)DESCALE(col[3], PASS1_BITS + 3);
        blkptr[4*8] = (int16_t)DESCALE(col[4], PASS1_BITS + 3);
        blkptr[5*8] = (int16_t)DESCALE(col[5], PASS1_BITS + 3);
        blkptr[6*8] = (int16_t)DESCALE(col[6], PASS1_BITS + 3);
        blkptr[7*8] = (int16_t)DESCALE(col[7], PASS1_BITS + 3);

        wsptr++;
        blkptr++;
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
