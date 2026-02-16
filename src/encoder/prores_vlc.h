/*
 * ProRes VLC (Variable Length Coding) Implementation
 * Bitstream writing and ProRes-specific entropy coding
 */

#ifndef PRORES_VLC_H
#define PRORES_VLC_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Bitstream writer context */
typedef struct {
    uint8_t* buf;
    uint8_t* buf_ptr;
    uint8_t* buf_end;
    uint32_t bit_buf;
    int bit_left;
    size_t size_in_bits;
} PutBitContext;

/*
 * Initialize bitstream writer
 */
void init_put_bits(PutBitContext* pb, uint8_t* buf, size_t buf_size);

/*
 * Write n bits (max 25)
 */
void put_bits(PutBitContext* pb, int n, uint32_t value);

/*
 * Write 32 bits
 */
void put_bits32(PutBitContext* pb, uint32_t value);

/*
 * Flush remaining bits to buffer
 */
void flush_put_bits(PutBitContext* pb);

/*
 * Get number of bytes written
 */
size_t put_bytes_count(const PutBitContext* pb);

/*
 * Get number of bits written
 */
size_t put_bits_count(const PutBitContext* pb);

/*
 * ProRes-specific: Estimate bits for a VLC codeword (without writing)
 * Mirrors encode_vlc_codeword but returns bit count
 *
 * @param codebook Codebook byte
 * @param val      Unsigned value to estimate
 * @return         Number of bits required
 */
int prores_estimate_vlc_codeword(unsigned int codebook, int val);

/* Exposed codebook tables for bit estimation */
extern const uint8_t prores_run_to_cb[16];
extern const uint8_t prores_lev_to_cb[10];
extern const uint8_t prores_dc_codebook[7];
#define PRORES_FIRST_DC_CB 0xB8

/*
 * ProRes-specific: Encode a single coefficient using Rice coding
 *
 * @param pb       Bitstream context
 * @param codebook Codebook index (0-7)
 * @param value    Value to encode (can be negative)
 */
void prores_encode_vlc(PutBitContext* pb, int codebook, int value);

/*
 * ProRes-specific: Encode DC coefficient with differential coding
 *
 * @param pb        Bitstream context
 * @param codebook  DC codebook index
 * @param diff      DC difference from previous block
 */
void prores_encode_dc(PutBitContext* pb, int codebook, int diff);

/*
 * ProRes-specific: Encode AC coefficients for a block
 *
 * @param pb        Bitstream context
 * @param codebook  AC codebook index
 * @param coeffs    Quantized DCT coefficients (64 values, zigzag order)
 */
void prores_encode_ac(PutBitContext* pb, int codebook, const int16_t* coeffs);

/*
 * ProRes-specific: Encode a run value
 *
 * @param pb        Bitstream context
 * @param run_cb    Run codebook index (0-15)
 * @param run       Run length to encode
 */
void prores_encode_run(PutBitContext* pb, int run_cb, int run);

/*
 * ProRes-specific: Encode a level value
 *
 * @param pb        Bitstream context
 * @param lev_cb    Level codebook index (0-9)
 * @param level     Absolute level value to encode (must be >= 1)
 */
void prores_encode_level(PutBitContext* pb, int lev_cb, int level);

#ifdef __cplusplus
}
#endif

#endif /* PRORES_VLC_H */
