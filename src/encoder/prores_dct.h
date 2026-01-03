/*
 * ProRes DCT (Discrete Cosine Transform) Implementation
 * Integer-only implementation suitable for WebAssembly
 */

#ifndef PRORES_DCT_H
#define PRORES_DCT_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Forward DCT on 8x8 block
 * Uses scaled integer arithmetic (AAN algorithm)
 *
 * @param block  Input: pixel values (10-bit), Output: DCT coefficients
 */
void prores_fdct_8x8(int16_t* block);

/*
 * Forward DCT on 8x8 block with row stride
 *
 * @param src     Source pixel data
 * @param dst     Destination coefficient data
 * @param stride  Row stride in pixels
 */
void prores_fdct_8x8_stride(const int16_t* src, int16_t* dst, int stride);

#ifdef __cplusplus
}
#endif

#endif /* PRORES_DCT_H */
