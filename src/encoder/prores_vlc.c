/*
 * ProRes VLC (Variable Length Coding) Implementation
 * Rice/Golomb entropy coding as used in Apple ProRes
 *
 * SPDX-License-Identifier: LGPL-2.1-or-later
 *
 * Derived from FFmpeg's libavcodec/proresenc_kostya.c:
 *   Copyright (c) 2011 Anatoliy Wasserman
 *   Copyright (c) 2012 Konstantin Shishkov
 * Modifications for WebAssembly:
 *   Copyright (c) 2026 Olivier Estévez
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU Lesser General Public License as published by
 * the Free Software Foundation; either version 2.1 of the License, or (at
 * your option) any later version. See the LICENSE file for details.
 */

#include "prores_vlc.h"

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