/*
 * ProRes VLC (Variable Length Coding) Implementation
 * Bitstream writing and ProRes-specific entropy coding
 */

#ifndef PRORES_VLC_H
#define PRORES_VLC_H

#include <stdint.h>
#include <stddef.h>
#include <string.h>

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
static inline void init_put_bits(PutBitContext* pb, uint8_t* buf, size_t buf_size)
{
    pb->buf = buf;
    pb->buf_ptr = buf;
    pb->buf_end = buf + buf_size;
    pb->bit_buf = 0;
    pb->bit_left = 32;
    pb->size_in_bits = 0;
}

/*
 * Write n bits (max 25)
 */
static inline void put_bits(PutBitContext* pb, int n, uint32_t value)
{
    if (n <= 0) return;

    if (n < pb->bit_left) {
        pb->bit_buf = (pb->bit_buf << n) | (value & ((1u << n) - 1));
        pb->bit_left -= n;
    } else {
        /* Flush current word and start new one */
        uint32_t word = (pb->bit_buf << pb->bit_left) | ((value >> (n - pb->bit_left)) & ((1u << pb->bit_left) - 1));
        if (pb->buf_ptr + 4 <= pb->buf_end) {
            pb->buf_ptr[0] = word >> 24;
            pb->buf_ptr[1] = word >> 16;
            pb->buf_ptr[2] = word >> 8;
            pb->buf_ptr[3] = word;
            pb->buf_ptr += 4;
        }
        int remaining = n - pb->bit_left;
        pb->bit_buf = value & ((1u << remaining) - 1);
        pb->bit_left = 32 - remaining;
    }
    pb->size_in_bits += n;
}

/*
 * Write 32 bits
 */
static inline void put_bits32(PutBitContext* pb, uint32_t value)
{
    put_bits(pb, 16, value >> 16);
    put_bits(pb, 16, value & 0xFFFF);
}

/*
 * Flush remaining bits to buffer
 */
static inline void flush_put_bits(PutBitContext* pb)
{
    if (pb->bit_left < 32) {
        uint32_t word = pb->bit_buf << pb->bit_left;
        int bytes = (32 - pb->bit_left + 7) / 8;
        while (bytes > 0 && pb->buf_ptr < pb->buf_end) {
            *pb->buf_ptr++ = word >> 24;
            word <<= 8;
            bytes--;
        }
        pb->bit_left = 32;
        pb->bit_buf = 0;
    }
}

/*
 * Copy whole bytes into the bitstream.
 * The writer must be byte-aligned and flushed (bit_left == 32); call
 * flush_put_bits() at a byte boundary first. Like put_bits, silently
 * truncates at buf_end while still accounting all bits in size_in_bits.
 */
static inline void put_bytes(PutBitContext* pb, const uint8_t* src, size_t count)
{
    size_t avail = (size_t)(pb->buf_end - pb->buf_ptr);
    size_t n = (count < avail) ? count : avail;
    memcpy(pb->buf_ptr, src, n);
    pb->buf_ptr += n;
    pb->size_in_bits += count * 8;
}

/*
 * Get number of bytes written
 */
static inline size_t put_bytes_count(const PutBitContext* pb)
{
    return (pb->size_in_bits + 7) / 8;
}

/*
 * Get number of bits written
 */
static inline size_t put_bits_count(const PutBitContext* pb)
{
    return pb->size_in_bits;
}

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
