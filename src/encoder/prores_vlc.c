/*
 * ProRes VLC (Variable Length Coding) Implementation
 * Rice/Golomb entropy coding as used in Apple ProRes
 * Based on FFmpeg's proresenc_kostya.c implementation
 */

#include "prores_vlc.h"
#include <string.h>

/* Codebook tables from FFmpeg proresenc_kostya.c
 * Format: (rice_order << 5) | (exp_order << 2) | (switch_bits - 1)
 * Non-static so they can be accessed by the encoder for bit estimation.
 */
const uint8_t prores_run_to_cb[16] = {
    0x06, 0x06, 0x05, 0x05, 0x04, 0x29, 0x29, 0x29,
    0x29, 0x28, 0x28, 0x28, 0x28, 0x28, 0x28, 0x4C
};

const uint8_t prores_lev_to_cb[10] = {
    0x04, 0x0A, 0x05, 0x06, 0x04, 0x28, 0x28, 0x28, 0x28, 0x4C
};

/* DC codebook table from FFmpeg's proresdata.c
 * Index by MIN(previous_code, 6) to select codebook for current DC */
const uint8_t prores_dc_codebook[7] = { 0x04, 0x28, 0x28, 0x4D, 0x4D, 0x70, 0x70 };
#define FIRST_DC_CB 0xB8

/* Simple log2 for small integers */
static int simple_log2(unsigned int v)
{
    int n = 0;
    if (v >= 65536) { v >>= 16; n += 16; }
    if (v >= 256)   { v >>= 8;  n += 8; }
    if (v >= 16)    { v >>= 4;  n += 4; }
    if (v >= 4)     { v >>= 2;  n += 2; }
    if (v >= 2)     { n += 1; }
    return n;
}

void init_put_bits(PutBitContext* pb, uint8_t* buf, size_t buf_size)
{
    pb->buf = buf;
    pb->buf_ptr = buf;
    pb->buf_end = buf + buf_size;
    pb->bit_buf = 0;
    pb->bit_left = 32;
    pb->size_in_bits = 0;
}

void put_bits(PutBitContext* pb, int n, uint32_t value)
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

void put_bits32(PutBitContext* pb, uint32_t value)
{
    put_bits(pb, 16, value >> 16);
    put_bits(pb, 16, value & 0xFFFF);
}

void flush_put_bits(PutBitContext* pb)
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

size_t put_bytes_count(const PutBitContext* pb)
{
    return (pb->size_in_bits + 7) / 8;
}

size_t put_bits_count(const PutBitContext* pb)
{
    return pb->size_in_bits;
}

/*
 * Encode value using ProRes VLC
 * Based on FFmpeg's encode_vlc_codeword
 * Codebook format: (rice_order << 5) | (exp_order << 2) | (switch_bits - 1)
 */
static void encode_vlc_codeword(PutBitContext* pb, unsigned int codebook, int val)
{
    unsigned int rice_order, exp_order, switch_bits, switch_val;
    int exponent;

    /* Extract parameters from codebook */
    switch_bits = (codebook & 3) + 1;
    rice_order  = codebook >> 5;
    exp_order   = (codebook >> 2) & 7;

    switch_val  = switch_bits << rice_order;

    if ((unsigned int)val >= switch_val) {
        /* Use exp-golomb coding for larger values */
        val -= switch_val - (1 << exp_order);
        exponent = simple_log2(val);

        put_bits(pb, exponent - exp_order + switch_bits, 0);
        put_bits(pb, exponent + 1, val);
    } else {
        /* Use Rice coding for smaller values */
        exponent = val >> rice_order;

        if (exponent)
            put_bits(pb, exponent, 0);
        put_bits(pb, 1, 1);
        if (rice_order)
            put_bits(pb, rice_order, val & ((1 << rice_order) - 1));
    }
}

/*
 * Estimate bits for a VLC codeword without writing
 * Mirrors encode_vlc_codeword but returns bit count
 */
int prores_estimate_vlc_codeword(unsigned int codebook, int val)
{
    unsigned int rice_order, exp_order, switch_bits, switch_val;
    int exponent;

    switch_bits = (codebook & 3) + 1;
    rice_order  = codebook >> 5;
    exp_order   = (codebook >> 2) & 7;
    switch_val  = switch_bits << rice_order;

    if ((unsigned int)val >= switch_val) {
        val -= switch_val - (1 << exp_order);
        exponent = simple_log2(val);
        return (exponent - exp_order + switch_bits) + (exponent + 1);
    } else {
        exponent = val >> rice_order;
        return exponent + 1 + rice_order;
    }
}

void prores_encode_vlc(PutBitContext* pb, int codebook, int value)
{
    /* Convert signed to unsigned using zigzag: negative -> odd, positive -> even */
    unsigned int uval = (value < 0) ? (unsigned int)(-value * 2 - 1) : (unsigned int)(value * 2);
    encode_vlc_codeword(pb, codebook, uval);
}

void prores_encode_dc(PutBitContext* pb, int codebook_idx, int diff)
{
    /* DC uses signed-to-unsigned conversion (zigzag) */
    unsigned int uval = (diff < 0) ? (unsigned int)(-diff * 2 - 1) : (unsigned int)(diff * 2);

    /* Select codebook based on index (0 = first DC, otherwise based on prev code) */
    uint8_t cb;
    if (codebook_idx < 0) {
        cb = FIRST_DC_CB;
    } else {
        cb = prores_dc_codebook[codebook_idx < 7 ? codebook_idx : 6];
    }

    encode_vlc_codeword(pb, cb, uval);
}

void prores_encode_run(PutBitContext* pb, int run_cb, int run)
{
    if (run_cb < 0) run_cb = 0;
    if (run_cb > 15) run_cb = 15;
    encode_vlc_codeword(pb, prores_run_to_cb[run_cb], run);
}

void prores_encode_level(PutBitContext* pb, int lev_cb, int level)
{
    if (lev_cb < 0) lev_cb = 0;
    if (lev_cb > 9) lev_cb = 9;
    /* Level is encoded as (abs_level - 1) */
    encode_vlc_codeword(pb, prores_lev_to_cb[lev_cb], level - 1);
}

void prores_encode_ac(PutBitContext* pb, int codebook, const int16_t* coeffs)
{
    (void)codebook;
    int prev_run = 4;
    int prev_level = 2;
    int run = 0;
    int i;

    /* Encode run-level pairs for AC coefficients (skip DC at index 0) */
    for (i = 1; i < 64; i++) {
        int level = coeffs[i];

        if (level == 0) {
            run++;
        } else {
            /* Encode run using adaptive codebook selection */
            prores_encode_run(pb, prev_run, run);

            /* Encode level using adaptive codebook selection */
            int abs_level = (level < 0) ? -level : level;
            prores_encode_level(pb, prev_level, abs_level);

            /* Encode sign */
            put_bits(pb, 1, (level < 0) ? 1 : 0);

            /* Update adaptive state */
            prev_run = (run < 16) ? run : 15;
            prev_level = (abs_level < 10) ? abs_level : 9;
            run = 0;
        }
    }

    /* End of block: if remaining zeros, encode final run */
    if (run > 0 || prev_run == 4) {
        prores_encode_run(pb, prev_run, run);
    }
}
