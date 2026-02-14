/*
 * ProRes Encoder Implementation
 * Based on Apple ProRes specification
 */

#include "prores_encoder.h"
#include "prores_dct.h"
#include "prores_vlc.h"
#include <stdlib.h>
#include <string.h>

/* ProRes frame header magic */
#define PRORES_FRAME_MAGIC  0x69637066  /* 'icpf' */

/* Maximum slices per picture */
#define MAX_SLICES_PER_LINE  256
#define MAX_SLICES          8192

/* Macroblock size */
#define MB_SIZE 16
/* Maximum size for a single plane's encoded data in a slice */
#define MAX_PLANE_DATA_SIZE 65536

/* ProRes profile FourCC codes (big endian) */
static const uint32_t profile_fourcc[6] = {
    0x6170636F,  /* 'apco' - Proxy */
    0x61706373,  /* 'apcs' - LT */
    0x6170636E,  /* 'apcn' - Standard */
    0x61706368,  /* 'apch' - HQ */
    0x61703468,  /* 'ap4h' - 4444 */
    0x61703478,  /* 'ap4x' - 4444 XQ */
};

/* Profile bitrates (bits per pixel) */
static const int profile_bpp[6] = {
    10,   /* Proxy - ~45 Mbps @ 1080p30 */
    22,   /* LT - ~102 Mbps @ 1080p30 */
    32,   /* Standard - ~147 Mbps @ 1080p30 */
    48,   /* HQ - ~220 Mbps @ 1080p30 */
    66,   /* 4444 - ~330 Mbps @ 1080p30 */
    110,  /* 4444 XQ - ~500 Mbps @ 1080p30 */
};

/* Quantization matrices for each profile (from FFmpeg proresenc_kostya.c, raster order) */
static const uint8_t quant_matrix_proxy[64] = {
     4,  7,  9, 11, 13, 14, 15, 63,
     7,  7, 11, 12, 14, 15, 63, 63,
     9, 11, 13, 14, 15, 63, 63, 63,
    11, 11, 13, 14, 63, 63, 63, 63,
    11, 13, 14, 63, 63, 63, 63, 63,
    13, 14, 63, 63, 63, 63, 63, 63,
    13, 63, 63, 63, 63, 63, 63, 63,
    63, 63, 63, 63, 63, 63, 63, 63,
};

/* Proxy uses a separate chroma matrix (from FFmpeg proresenc_kostya.c) */
static const uint8_t quant_matrix_proxy_chroma[64] = {
     4,  7,  9, 11, 13, 14, 63, 63,
     7,  7, 11, 12, 14, 63, 63, 63,
     9, 11, 13, 14, 63, 63, 63, 63,
    11, 11, 13, 14, 63, 63, 63, 63,
    11, 13, 14, 63, 63, 63, 63, 63,
    13, 14, 63, 63, 63, 63, 63, 63,
    13, 63, 63, 63, 63, 63, 63, 63,
    63, 63, 63, 63, 63, 63, 63, 63,
};

static const uint8_t quant_matrix_lt[64] = {
     4,  5,  6,  7,  9, 11, 13, 15,
     5,  5,  7,  8, 11, 13, 15, 17,
     6,  7,  9, 11, 13, 15, 15, 17,
     7,  7,  9, 11, 13, 15, 17, 19,
     7,  9, 11, 13, 14, 16, 19, 23,
     9, 11, 13, 14, 16, 19, 23, 29,
     9, 11, 13, 15, 17, 21, 28, 35,
    11, 13, 16, 17, 21, 28, 35, 41,
};

static const uint8_t quant_matrix_std[64] = {
     4,  4,  5,  5,  6,  7,  7,  9,
     4,  4,  5,  6,  7,  7,  9,  9,
     5,  5,  6,  7,  7,  9,  9, 10,
     5,  5,  6,  7,  7,  9,  9, 10,
     5,  6,  7,  7,  8,  9, 10, 12,
     6,  7,  7,  8,  9, 10, 12, 15,
     6,  7,  7,  9, 10, 11, 14, 17,
     7,  7,  9, 10, 11, 14, 17, 21,
};

static const uint8_t quant_matrix_hq[64] = {
    4, 4, 4, 4, 4, 4, 4, 4,
    4, 4, 4, 4, 4, 4, 4, 4,
    4, 4, 4, 4, 4, 4, 4, 4,
    4, 4, 4, 4, 4, 4, 4, 5,
    4, 4, 4, 4, 4, 4, 5, 5,
    4, 4, 4, 4, 4, 5, 5, 6,
    4, 4, 4, 4, 5, 5, 6, 7,
    4, 4, 4, 4, 5, 6, 7, 7,
};

/* ProRes progressive scan order (FFmpeg compatible) */
static const uint8_t prores_scan[64] = {
     0,  1,  8,  9,  2,  3, 10, 11,
    16, 17, 24, 25, 18, 19, 26, 27,
     4,  5, 12, 20, 13,  6,  7, 14,
    21, 28, 29, 22, 15, 23, 30, 31,
    32, 33, 40, 48, 41, 34, 35, 42,
    49, 56, 57, 50, 43, 36, 37, 44,
    51, 58, 59, 52, 45, 38, 39, 46,
    53, 60, 61, 54, 47, 55, 62, 63
};

/* Encoder context structure */
struct ProResEncoderContext {
    ProResEncoderConfig config;

    /* Derived values */
    int padded_width;        /* Width padded to macroblock */
    int padded_height;       /* Height padded to macroblock */
    int mb_width;            /* Width in macroblocks */
    int mb_height;           /* Height in macroblocks */
    int num_slices;          /* Total number of slices */
    int slice_mb_count;      /* Macroblocks per slice */
    int slices_per_row;      /* Number of slices per macroblock row */
    int slice_mb_width;      /* MBs per slice width */
    int slice_mb_height;     /* MBs per slice height */
    int log2_slice_mb_width; /* Log2 of slice_mb_width */
    int log2_slice_mb_height;/* Log2 of slice_mb_height */

    /* Quantization */
    uint8_t quant_matrix[64];        /* Luma quantization matrix */
    uint8_t chroma_quant_matrix[64]; /* Chroma quantization matrix */
    int q_scale;             /* Quantization scale factor */
    int bit_depth;
    int sample_center;

    /* Working buffers */
    int16_t* y_plane;        /* Luma buffer */
    int16_t* u_plane;        /* Cb buffer */
    int16_t* v_plane;        /* Cr buffer */
    int16_t* a_plane;        /* Alpha buffer (4444 only) */
    int16_t* dct_block;      /* DCT coefficient buffer */

    /* Output buffer */
    uint8_t* output_buf;
    size_t output_size;
    size_t output_capacity;

    /* Slice temp buffers */
    uint8_t* slice_luma_buf;
    uint8_t* slice_u_buf;
    uint8_t* slice_v_buf;
    uint8_t* slice_alpha_buf;    /* Encoded alpha temp buffer */
    uint16_t* alpha_pixel_buf;   /* Raw alpha pixels for one slice */
    size_t slice_buf_capacity;

    /* State */
    int frame_count;
    int last_dc[4];          /* Last DC values for differential coding */
};

/* Helper: Get luma quantization matrix for profile */
static const uint8_t* get_quant_matrix(ProResProfile profile)
{
    switch (profile) {
        case PRORES_PROFILE_PROXY:   return quant_matrix_proxy;
        case PRORES_PROFILE_LT:      return quant_matrix_lt;
        case PRORES_PROFILE_STANDARD:return quant_matrix_std;
        default:                     return quant_matrix_hq;
    }
}

/* Helper: Get chroma quantization matrix for profile
 * Only Proxy uses a separate chroma matrix; all others use the same as luma */
static const uint8_t* get_chroma_quant_matrix(ProResProfile profile)
{
    if (profile == PRORES_PROFILE_PROXY)
        return quant_matrix_proxy_chroma;
    return get_quant_matrix(profile);
}

/* Helper: Check if profile is 4:4:4 */
static int is_444_profile(ProResProfile profile)
{
    return profile == PRORES_PROFILE_4444 || profile == PRORES_PROFILE_4444XQ;
}

ProResEncoderContext* prores_encoder_create(const ProResEncoderConfig* config)
{
    ProResEncoderContext* ctx;
    int plane_size;

    if (!config || config->width <= 0 || config->height <= 0) {
        return NULL;
    }

    ctx = (ProResEncoderContext*)calloc(1, sizeof(ProResEncoderContext));
    if (!ctx) return NULL;

    /* Copy configuration */
    ctx->config = *config;

    /* Calculate padded dimensions for macroblock alignment */
    ctx->padded_width = (config->width + MB_SIZE - 1) & ~(MB_SIZE - 1);
    ctx->padded_height = (config->height + MB_SIZE - 1) & ~(MB_SIZE - 1);

    /* Calculate dimensions */
    ctx->mb_width = ctx->padded_width / MB_SIZE;
    ctx->mb_height = ctx->padded_height / MB_SIZE;

    /* Slice dimensions: use 8 MBs per slice width (log2 = 3), 1 MB per slice height (log2 = 0)
     * This matches FFmpeg's default and avoids "unsupported slice resolution" errors */
    ctx->log2_slice_mb_width = 3;  /* 2^3 = 8 MBs per slice width */
    ctx->log2_slice_mb_height = 0; /* 2^0 = 1 MB per slice height */
    ctx->slice_mb_width = 1 << ctx->log2_slice_mb_width;  /* 8 MBs */
    ctx->slice_mb_height = 1 << ctx->log2_slice_mb_height; /* 1 MB */

    /* Calculate slices per row and total slices */
    ctx->slices_per_row = (ctx->mb_width + ctx->slice_mb_width - 1) / ctx->slice_mb_width;
    ctx->num_slices = ctx->slices_per_row * ctx->mb_height;
    ctx->slice_mb_count = ctx->slice_mb_width;  /* MBs per slice (may be less for last slice in row) */

    /* Quality affects quantization scale (1..16, lower is higher quality) */
    ctx->q_scale = 1 + (100 - config->quality) * 15 / 100;
    if (ctx->q_scale < 1) ctx->q_scale = 1;
    if (ctx->q_scale > 16) ctx->q_scale = 16;

    /* Always use 10-bit internally, matching FFmpeg (avctx->bits_per_raw_sample = 10).
     * This ensures DCT coefficients fit in int16_t (max DC = 32 * 1023 = 32736). */
    ctx->bit_depth = 10;
    ctx->sample_center = 1 << (ctx->bit_depth - 1);

    /* Copy quantization matrices */
    memcpy(ctx->quant_matrix, get_quant_matrix(config->profile), 64);
    memcpy(ctx->chroma_quant_matrix, get_chroma_quant_matrix(config->profile), 64);

    /* Allocate plane buffers (16-bit for 10-bit data), using padded sizes */
    plane_size = ctx->padded_width * ctx->padded_height * sizeof(int16_t);

    ctx->y_plane = (int16_t*)malloc(plane_size);
    if (is_444_profile(config->profile)) {
        ctx->u_plane = (int16_t*)malloc(plane_size);
        ctx->v_plane = (int16_t*)malloc(plane_size);
        ctx->a_plane = (int16_t*)malloc(plane_size);
    } else {
        int chroma_width = ctx->padded_width / 2;
        int chroma_size = chroma_width * ctx->padded_height * sizeof(int16_t);
        ctx->u_plane = (int16_t*)malloc(chroma_size);
        ctx->v_plane = (int16_t*)malloc(chroma_size);
    }

    ctx->dct_block = (int16_t*)malloc(64 * 4 * sizeof(int16_t));  /* 4 blocks per macroblock minimum */

    /* Output buffer - estimate based on bitrate */
    ctx->output_capacity = (size_t)(ctx->padded_width * ctx->padded_height * profile_bpp[config->profile] / 8);
    ctx->output_capacity += 1024;  /* Header overhead */
    ctx->output_buf = (uint8_t*)malloc(ctx->output_capacity);

    ctx->slice_buf_capacity = MAX_PLANE_DATA_SIZE;
    ctx->slice_luma_buf = (uint8_t*)malloc(ctx->slice_buf_capacity);
    ctx->slice_u_buf = (uint8_t*)malloc(ctx->slice_buf_capacity);
    ctx->slice_v_buf = (uint8_t*)malloc(ctx->slice_buf_capacity);

    if (is_444_profile(config->profile)) {
        ctx->slice_alpha_buf = (uint8_t*)malloc(MAX_PLANE_DATA_SIZE);
        /* Max alpha pixels per slice: slice_mb_width * 16 * 16 (16 rows x 16*N cols) */
        ctx->alpha_pixel_buf = (uint16_t*)malloc(ctx->slice_mb_width * 256 * sizeof(uint16_t));
    }

    if (!ctx->y_plane || !ctx->u_plane || !ctx->v_plane || !ctx->dct_block ||
        !ctx->output_buf || !ctx->slice_luma_buf || !ctx->slice_u_buf || !ctx->slice_v_buf) {
        prores_encoder_destroy(ctx);
        return NULL;
    }

    if (is_444_profile(config->profile) && (!ctx->a_plane || !ctx->slice_alpha_buf || !ctx->alpha_pixel_buf)) {
        prores_encoder_destroy(ctx);
        return NULL;
    }

    return ctx;
}

/* Frame header data size (matching FFmpeg's proresenc_kostya format) */
#define FRAME_HEADER_SIZE 148

/* Write ProRes frame header (follows Apple spec / FFmpeg format)
 * Returns the offset where picture header should start */
static int write_frame_header(ProResEncoderContext* ctx, uint8_t* buf)
{
    int is_444 = is_444_profile(ctx->config.profile);

    /* hdr_size value: FFmpeg uses 148, representing header size from this field onwards
     * Picture data starts at: frame_size(4) + icpf(4) + hdr_size = 8 + hdr_size
     * FFmpeg decoder does: buf += hdr_size + 8 to reach picture data */
    int hdr_size = FRAME_HEADER_SIZE;  /* 148 - matches FFmpeg */
    int total_frame_header = 4 + 4 + hdr_size;  /* frame_size + icpf + header = 156 */

    memset(buf, 0, total_frame_header);  /* Clear header area */
    uint8_t* p = buf;

    /* Frame size placeholder (4 bytes) - filled later */
    p += 4;

    /* Frame identifier 'icpf' */
    *p++ = 'i'; *p++ = 'c'; *p++ = 'p'; *p++ = 'f';

    /* Frame header size (2 bytes BE) - matches FFmpeg's 148 */
    *p++ = (hdr_size >> 8) & 0xFF;
    *p++ = hdr_size & 0xFF;

    /* Version (2 bytes): FFmpeg sets byte 3 to 1 for 4444 profiles */
    *p++ = 0;
    *p++ = is_444 ? 1 : 0;

    /* Encoder identifier 'wasm' (4 bytes) */
    *p++ = 'w'; *p++ = 'a'; *p++ = 's'; *p++ = 'm';

    /* Width (2 bytes BE) */
    *p++ = (ctx->config.width >> 8) & 0xFF;
    *p++ = ctx->config.width & 0xFF;

    /* Height (2 bytes BE) */
    *p++ = (ctx->config.height >> 8) & 0xFF;
    *p++ = ctx->config.height & 0xFF;

    /* Flags byte: (chroma << 6) | (reserved << 4) | (interlace << 2) | alpha_info
     * Matches FFmpeg's proresenc_kostya.c / proresdec2.c format:
     * - bits 7-6: chroma_format (2=4:2:2, 3=4:4:4)
     * - bits 5-4: reserved (0)
     * - bits 3-2: interlace_mode (0=progressive, 1=TFF, 2=BFF)
     * - bits 1-0: alpha_info (0=none, 1=8-bit, 2=16-bit) */
    int chroma = is_444 ? 3 : 2;  /* 2 = 4:2:2, 3 = 4:4:4 */
    int alpha_info = 0;  /* FFmpeg leaves this 0 even with alpha; alpha_config byte is authoritative */
    int interlace = 0;  /* 0 = progressive */
    if (ctx->config.frame_type == PRORES_FRAME_INTERLACED_TFF) interlace = 1;
    else if (ctx->config.frame_type == PRORES_FRAME_INTERLACED_BFF) interlace = 2;
    *p++ = (chroma << 6) | (interlace << 2) | alpha_info;

    /* Reserved */
    *p++ = 0;

    /* Color primaries (1=BT.709, 9=BT.2020) */
    *p++ = (ctx->config.colorspace == PRORES_CS_BT2020) ? 9 : 1;

    /* Transfer function (1=BT.709, 14=BT.2020) */
    *p++ = (ctx->config.colorspace == PRORES_CS_BT2020) ? 14 : 1;

    /* Matrix coefficients (1=BT.709, 9=BT.2020) */
    *p++ = (ctx->config.colorspace == PRORES_CS_BT2020) ? 9 : 1;

    /* Alpha configuration byte (matches FFmpeg: 0x00=none, 0x02=16-bit alpha) */
    *p++ = is_444 ? 0x02 : 0x00;

    /* Reserved byte */
    *p++ = 0x00;

    /* Quantization matrix flags: 0x03 = custom luma and chroma matrices */
    *p++ = 0x03;

    /* Write luma quantization matrix (64 bytes) */
    for (int i = 0; i < 64; i++) {
        *p++ = ctx->quant_matrix[i];
    }

    /* Write chroma quantization matrix (64 bytes) */
    for (int i = 0; i < 64; i++) {
        *p++ = ctx->chroma_quant_matrix[i];
    }

    /* Note: total frame header = 4 (size) + 4 (icpf) + 148 (header) = 156 bytes
     * Header content: 2 (hdr_size) + 2 (reserved) + 4 (vendor) + 2 (width) +
     *                 2 (height) + 1 (flags) + 1 (reserved) + 1 (primaries) +
     *                 1 (transfer) + 1 (matrix) + 1 (alpha_config) + 1 (reserved) +
     *                 1 (qmat_flags) + 64 (luma_qmat) + 64 (chroma_qmat) = 148 */

    return total_frame_header;  /* Return offset to picture header (156) */
}

/* DCT into raster order (ProRes quantization happens during VLC)
 * Input pixels are unsigned 10-bit values (0-1023), NOT centered.
 * Matches FFmpeg: raw pixel values go directly into DCT,
 * DC offset (0x4000) is subtracted during encoding. */
static void dct_block(const int16_t* src, int stride, int16_t* dst)
{
    int16_t block[64];
    int i, j;

    for (i = 0; i < 8; i++) {
        for (j = 0; j < 8; j++) {
            block[i * 8 + j] = src[i * stride + j];
        }
    }

    prores_fdct_8x8(block);

    for (i = 0; i < 64; i++) {
        dst[i] = block[i];
    }
}

/* Maximum blocks per slice: 8 MBs x 4 luma blocks = 32 blocks per component */
#define MAX_BLOCKS_PER_SLICE 32

/*
 * Encode DC coefficients for all blocks in a plane
 * ProRes encodes all DCs first, before any AC coefficients
 * Uses adaptive codebook selection based on previous code value
 */
static void encode_dc_coeffs(PutBitContext* pb, int16_t blocks[][64], int num_blocks,
                             const uint8_t* qmat, int q_scale)
{
    int prev_dc = 0;
    int codebook_idx = 5;
    int sign = 0;
    int scale = qmat[0] * q_scale;
    int16_t* flat = &blocks[0][0];

    if (scale < 1) scale = 1;

    /* First DC uses the fixed codebook.
     * Subtract 0x4000 DC offset (matches FFmpeg's encode_dcs).
     * Uses truncation (plain integer division), matching FFmpeg. */
    int dc0 = flat[0] - 0x4000;
    prev_dc = dc0 / scale;
    prores_encode_dc(pb, -1, prev_dc);
    flat += 64;

    for (int b = 1; b < num_blocks; b++, flat += 64) {
        int dc_raw = flat[0] - 0x4000;
        int dc = dc_raw / scale;
        int delta = dc - prev_dc;
        int new_sign = (delta < 0) ? -1 : 0;
        delta = (delta ^ sign) - sign;
        prores_encode_dc(pb, codebook_idx, delta);
        unsigned int code = (delta < 0) ? (unsigned int)(-delta * 2 - 1) : (unsigned int)(delta * 2);
        codebook_idx = (code < 7) ? code : 6;
        sign = new_sign;
        prev_dc = dc;
    }
}

/*
 * Encode AC coefficients for all blocks in a plane together
 * FFmpeg/ProRes expects position-major order: all blocks' coeff[1], then all coeff[2], etc.
 */
static void encode_ac_coeffs_all(PutBitContext* pb, int16_t blocks[][64], int num_blocks,
                                 const uint8_t* scan, const uint8_t* qmat, int q_scale)
{
    int prev_run = 4;
    int prev_level = 2;
    int run = 0;
    int max_coeffs = num_blocks << 6;
    int16_t* flat = &blocks[0][0];

    for (int i = 1; i < 64; i++) {
        int q = qmat[scan[i]] * q_scale;
        if (q < 1) q = 1;
        for (int idx = scan[i]; idx < max_coeffs; idx += 64) {
            /* Truncation (plain integer division), matching FFmpeg's encode_acs */
            int level = flat[idx] / q;
            if (level) {
                int abs_level = (level < 0) ? -level : level;
                prores_encode_run(pb, prev_run, run);
                prores_encode_level(pb, prev_level, abs_level);
                put_bits(pb, 1, (level < 0) ? 1 : 0);

                prev_run = (run < 16) ? run : 15;
                prev_level = (abs_level < 10) ? abs_level : 9;
                run = 0;
            } else {
                run++;
            }
        }
    }

    (void)run;
}

/*
 * Alpha encoding helpers for ProRes 4444
 * ProRes alpha uses per-pixel differential coding with run-length encoding,
 * NOT DCT+VLC like the YUV planes.
 */

/* Encode one alpha pixel difference */
static void put_alpha_diff(PutBitContext* pb, int cur, int prev, int abits)
{
    int dbits = (abits == 16) ? 7 : 4;
    int dsize = 1 << (dbits - 1);  /* 64 for 16-bit */
    int mask = (1 << abits) - 1;

    /* Compute unsigned wrapped difference */
    int diff = (cur - prev) & mask;

    /* Convert to signed range */
    if (diff >= (1 << abits) - dsize)
        diff -= (1 << abits);

    if (diff < -dsize || diff > dsize || diff == 0) {
        /* Full path: 1 flag bit + abits of unsigned wrapped value */
        put_bits(pb, 1, 1);
        if (abits > 25) {
            put_bits(pb, abits - 25, (unsigned)((cur - prev) & mask) >> 25);
            put_bits(pb, 25, (unsigned)((cur - prev) & mask) & ((1 << 25) - 1));
        } else {
            put_bits(pb, abits, (unsigned)((cur - prev) & mask));
        }
    } else {
        /* Short path: 0 flag + (dbits-1) magnitude bits + 1 sign bit = dbits+1 bits total */
        int abs_diff = (diff < 0) ? -diff : diff;
        int sign = (diff < 0) ? 1 : 0;
        put_bits(pb, 1, 0);
        put_bits(pb, dbits - 1, abs_diff - 1);
        put_bits(pb, 1, sign);
    }
}

/* Encode a run of identical alpha pixels */
static void put_alpha_run(PutBitContext* pb, int run)
{
    if (run == 0) {
        put_bits(pb, 1, 1);  /* No run */
    } else if (run < 16) {
        put_bits(pb, 1, 0);
        put_bits(pb, 4, run);
    } else {
        put_bits(pb, 1, 0);
        put_bits(pb, 15, run);
    }
}

/* Encode all alpha pixels for one slice */
static void encode_alpha_plane(PutBitContext* pb, const uint16_t* alpha_pixels,
                               int num_pixels, int abits)
{
    int mask = (1 << abits) - 1;
    int prev = mask;  /* Initial previous value = all-ones (0xFFFF for 16-bit) */
    int run = 0;
    int i;

    if (num_pixels <= 0) return;

    /* First pixel: always encode diff, no run */
    put_alpha_diff(pb, alpha_pixels[0], prev, abits);
    prev = alpha_pixels[0];

    for (i = 1; i < num_pixels; i++) {
        if (alpha_pixels[i] == prev) {
            run++;
        } else {
            /* Flush accumulated run, then encode the new diff */
            put_alpha_run(pb, run);
            put_alpha_diff(pb, alpha_pixels[i], prev, abits);
            prev = alpha_pixels[i];
            run = 0;
        }
    }

    /* Flush final run */
    put_alpha_run(pb, run);
}

/* Extract alpha pixels from a_plane in raster order for one slice.
 * Scales 10-bit (0-1023) to 16-bit (0-65535). */
static int get_alpha_data(const int16_t* a_plane, int stride,
                          int pixel_x, int pixel_y,
                          int slice_pixel_w, int padded_height,
                          uint16_t* out_buf)
{
    int count = 0;
    for (int y = 0; y < 16; y++) {
        int src_y = pixel_y + y;
        if (src_y >= padded_height) src_y = padded_height - 1;
        for (int x = 0; x < slice_pixel_w; x++) {
            int src_x = pixel_x + x;
            int val = a_plane[src_y * stride + src_x];
            if (val < 0) val = 0;
            if (val > 1023) val = 1023;
            /* Scale 10-bit to 16-bit: (val << 6) | (val >> 4) */
            out_buf[count++] = (uint16_t)((val << 6) | (val >> 4));
        }
    }
    return count;
}

/* Encode a single slice with proper slice header
 * ProRes slice structure:
 *   422:  6-byte header (hdr_size, scale, luma_size, u_size) + Y + U + V
 *   4444: 8-byte header (hdr_size, scale, luma_size, u_size, v_size) + Y + U + V + Alpha
 *
 * We encode each plane to a temp buffer first to get sizes, then write header + data
 */
static int encode_slice(ProResEncoderContext* ctx, PutBitContext* pb,
                       int slice_mb_x, int slice_mb_y, int slice_width)
{
    int mb_x;
    int block_x, block_y;
    int is_444 = is_444_profile(ctx->config.profile);
    int mb_y = slice_mb_y;

    /* Storage for all block coefficients */
    int16_t luma_blocks[MAX_BLOCKS_PER_SLICE][64];
    int16_t u_blocks[MAX_BLOCKS_PER_SLICE][64];
    int16_t v_blocks[MAX_BLOCKS_PER_SLICE][64];

    /* Temporary buffers for encoded plane data (heap-backed to avoid WASM stack overflow) */
    uint8_t* luma_data = ctx->slice_luma_buf;
    uint8_t* u_data = ctx->slice_u_buf;
    uint8_t* v_data = ctx->slice_v_buf;

    int luma_block_count = 0;
    int chroma_block_count = 0;

    /* First pass: DCT and quantize all luma blocks
     * Block order: row-major (TL, TR, BL, BR) */
    for (mb_x = 0; mb_x < slice_width; mb_x++) {
        int pixel_x = (slice_mb_x + mb_x) * MB_SIZE;
        int pixel_y = mb_y * MB_SIZE;

        for (block_y = 0; block_y < 2; block_y++) {
            for (block_x = 0; block_x < 2; block_x++) {
                int bx = pixel_x + block_x * 8;
                int by = pixel_y + block_y * 8;

                dct_block(ctx->y_plane + by * ctx->padded_width + bx,
                          ctx->padded_width,
                          luma_blocks[luma_block_count]);
                luma_block_count++;
            }
        }
    }

    /* Encode luma to temp buffer */
    PutBitContext luma_pb;
    init_put_bits(&luma_pb, luma_data, ctx->slice_buf_capacity);
    encode_dc_coeffs(&luma_pb, luma_blocks, luma_block_count, ctx->quant_matrix, ctx->q_scale);
    encode_ac_coeffs_all(&luma_pb, luma_blocks, luma_block_count,
                         prores_scan, ctx->quant_matrix, ctx->q_scale);

    /* Byte-align luma */
    int luma_bits = put_bits_count(&luma_pb);
    if (luma_bits % 8) put_bits(&luma_pb, 8 - (luma_bits % 8), 0);
    flush_put_bits(&luma_pb);
    int luma_size = (put_bits_count(&luma_pb) + 7) / 8;
    if ((size_t)luma_size > ctx->slice_buf_capacity) return -1;

    /* DCT and quantize chroma U blocks */
    for (mb_x = 0; mb_x < slice_width; mb_x++) {
        int pixel_x = (slice_mb_x + mb_x) * MB_SIZE;
        int pixel_y = mb_y * MB_SIZE;

        if (is_444) {
            /* 4444: column-major block order (TL, BL, TR, BR) */
            for (block_x = 0; block_x < 2; block_x++) {
                for (block_y = 0; block_y < 2; block_y++) {
                    int bx = pixel_x + block_x * 8;
                    int by = pixel_y + block_y * 8;

                    dct_block(ctx->u_plane + by * ctx->padded_width + bx,
                              ctx->padded_width,
                              u_blocks[chroma_block_count]);
                    chroma_block_count++;
                }
            }
        } else {
            int chroma_width = ctx->padded_width / 2;
            for (block_y = 0; block_y < 2; block_y++) {
                int bx = pixel_x / 2;
                int by = pixel_y + block_y * 8;

                dct_block(ctx->u_plane + by * chroma_width + bx,
                          chroma_width,
                          u_blocks[chroma_block_count]);
                chroma_block_count++;
            }
        }
    }

    /* Encode chroma U to temp buffer */
    PutBitContext u_pb;
    init_put_bits(&u_pb, u_data, ctx->slice_buf_capacity);
    encode_dc_coeffs(&u_pb, u_blocks, chroma_block_count, ctx->chroma_quant_matrix, ctx->q_scale);
    encode_ac_coeffs_all(&u_pb, u_blocks, chroma_block_count,
                         prores_scan, ctx->chroma_quant_matrix, ctx->q_scale);

    /* Byte-align chroma U */
    int u_bits = put_bits_count(&u_pb);
    if (u_bits % 8) put_bits(&u_pb, 8 - (u_bits % 8), 0);
    flush_put_bits(&u_pb);
    int u_size = (put_bits_count(&u_pb) + 7) / 8;
    if ((size_t)u_size > ctx->slice_buf_capacity) return -1;

    /* DCT and quantize chroma V blocks */
    int v_block_count = 0;
    for (mb_x = 0; mb_x < slice_width; mb_x++) {
        int pixel_x = (slice_mb_x + mb_x) * MB_SIZE;
        int pixel_y = mb_y * MB_SIZE;

        if (is_444) {
            /* 4444: column-major block order (TL, BL, TR, BR) */
            for (block_x = 0; block_x < 2; block_x++) {
                for (block_y = 0; block_y < 2; block_y++) {
                    int bx = pixel_x + block_x * 8;
                    int by = pixel_y + block_y * 8;

                    dct_block(ctx->v_plane + by * ctx->padded_width + bx,
                              ctx->padded_width,
                              v_blocks[v_block_count]);
                    v_block_count++;
                }
            }
        } else {
            int chroma_width = ctx->padded_width / 2;
            for (block_y = 0; block_y < 2; block_y++) {
                int bx = pixel_x / 2;
                int by = pixel_y + block_y * 8;

                dct_block(ctx->v_plane + by * chroma_width + bx,
                          chroma_width,
                          v_blocks[v_block_count]);
                v_block_count++;
            }
        }
    }

    /* Encode chroma V to temp buffer */
    PutBitContext v_pb;
    init_put_bits(&v_pb, v_data, ctx->slice_buf_capacity);
    encode_dc_coeffs(&v_pb, v_blocks, v_block_count, ctx->chroma_quant_matrix, ctx->q_scale);
    encode_ac_coeffs_all(&v_pb, v_blocks, v_block_count,
                         prores_scan, ctx->chroma_quant_matrix, ctx->q_scale);

    /* Byte-align chroma V */
    int v_bits = put_bits_count(&v_pb);
    if (v_bits % 8) put_bits(&v_pb, 8 - (v_bits % 8), 0);
    flush_put_bits(&v_pb);
    int v_size = (put_bits_count(&v_pb) + 7) / 8;
    if ((size_t)v_size > ctx->slice_buf_capacity) return -1;

    /* Encode alpha for 4444 profiles */
    int alpha_size = 0;
    if (is_444 && ctx->a_plane) {
        int slice_pixel_w = slice_width * MB_SIZE;
        int pixel_x = slice_mb_x * MB_SIZE;
        int pixel_y = mb_y * MB_SIZE;

        int num_alpha_pixels = get_alpha_data(ctx->a_plane, ctx->padded_width,
                                              pixel_x, pixel_y,
                                              slice_pixel_w, ctx->padded_height,
                                              ctx->alpha_pixel_buf);

        PutBitContext alpha_pb;
        init_put_bits(&alpha_pb, ctx->slice_alpha_buf, MAX_PLANE_DATA_SIZE);
        encode_alpha_plane(&alpha_pb, ctx->alpha_pixel_buf, num_alpha_pixels, 16);

        /* Byte-align alpha */
        int alpha_bits = put_bits_count(&alpha_pb);
        if (alpha_bits % 8) put_bits(&alpha_pb, 8 - (alpha_bits % 8), 0);
        flush_put_bits(&alpha_pb);
        alpha_size = (put_bits_count(&alpha_pb) + 7) / 8;
    }

    /* Write slice header with known sizes */
    if (is_444 && ctx->a_plane) {
        /* 8-byte header for 4444 with alpha: adds v_data_size so decoder can find alpha */
        put_bits(pb, 8, 64);  /* slice_hdr_size = 64 bits = 8 bytes */
        put_bits(pb, 8, ctx->q_scale);  /* scale_factor */
        put_bits(pb, 16, luma_size);    /* luma_data_size */
        put_bits(pb, 16, u_size);       /* u_data_size */
        put_bits(pb, 16, v_size);       /* v_data_size */
    } else {
        /* 6-byte header for 422 profiles */
        put_bits(pb, 8, 48);  /* slice_hdr_size = 48 bits = 6 bytes */
        put_bits(pb, 8, ctx->q_scale);  /* scale_factor */
        put_bits(pb, 16, luma_size);    /* luma_data_size */
        put_bits(pb, 16, u_size);       /* u_data_size */
    }

    /* Write plane data byte by byte */
    for (int i = 0; i < luma_size; i++) {
        put_bits(pb, 8, luma_data[i]);
    }
    for (int i = 0; i < u_size; i++) {
        put_bits(pb, 8, u_data[i]);
    }
    for (int i = 0; i < v_size; i++) {
        put_bits(pb, 8, v_data[i]);
    }

    /* Write alpha data for 4444 */
    if (is_444 && ctx->a_plane && alpha_size > 0) {
        for (int i = 0; i < alpha_size; i++) {
            put_bits(pb, 8, ctx->slice_alpha_buf[i]);
        }
    }

    return 0;
}

/* Write picture header (follows Apple ProRes spec / MultimediaWiki format)
 * Structure:
 *   Byte 0: pic_hdr_size in bits (8 bytes = 64 bits for fixed header)
 *   Bytes 1-4: pic_data_size in bytes (32-bit big-endian, filled in later)
 *   Bytes 5-6: total_slices count (16-bit big-endian)
 *   Byte 7: (log2_slice_mb_width << 4) | log2_slice_mb_height
 *   Bytes 8+: slice size table (2 bytes per slice)
 */
static int write_picture_header(ProResEncoderContext* ctx, uint8_t* buf)
{
    int i;
    uint8_t* p = buf;

    /* Calculate picture header size: 8 bytes fixed + 2 bytes per slice */
    int pic_hdr_size = 8 + ctx->num_slices * 2;

    /* Byte 0: picture header size in bits (fixed header size) */
    *p++ = 64;

    /* Bytes 1-4: picture data size placeholder (32-bit BE, filled in later) */
    *p++ = 0;
    *p++ = 0;
    *p++ = 0;
    *p++ = 0;

    /* Bytes 5-6: total number of slices (16-bit big-endian) */
    *p++ = (ctx->num_slices >> 8) & 0xFF;
    *p++ = ctx->num_slices & 0xFF;

    /* Byte 7: (log2_slice_mb_width << 4) | log2_slice_mb_height */
    *p++ = (ctx->log2_slice_mb_width << 4) | ctx->log2_slice_mb_height;

    /* Slice size table (2 bytes per slice, big-endian) - filled in later */
    for (i = 0; i < ctx->num_slices; i++) {
        *p++ = 0;
        *p++ = 0;
    }

    return pic_hdr_size;
}

static void copy_pad_plane(const uint16_t* src, int src_w, int src_h,
                           int dst_w, int dst_h, int16_t* dst)
{
    for (int y = 0; y < dst_h; y++) {
        int src_y = (y < src_h) ? y : (src_h - 1);
        const uint16_t* src_row = src + src_y * src_w;
        int16_t* dst_row = dst + y * dst_w;
        for (int x = 0; x < src_w; x++) {
            dst_row[x] = (int16_t)src_row[x];
        }
        uint16_t pad = src_row[src_w - 1];
        for (int x = src_w; x < dst_w; x++) {
            dst_row[x] = (int16_t)pad;
        }
    }
}

int prores_encoder_encode_frame(
    ProResEncoderContext* ctx,
    const uint16_t* yuv_data,
    uint8_t** out_data,
    int* out_size)
{
    PutBitContext pb;
    int i;
    int slice_y;

    if (!ctx || !yuv_data || !out_data || !out_size) {
        return -1;
    }

    /* Copy input data to internal buffers with padding */
    int plane_size = ctx->config.width * ctx->config.height;
    int is_444 = is_444_profile(ctx->config.profile);

    /* Y plane */
    copy_pad_plane(yuv_data, ctx->config.width, ctx->config.height,
                   ctx->padded_width, ctx->padded_height, ctx->y_plane);
    yuv_data += plane_size;

    if (is_444) {
        /* U plane (full resolution) */
        copy_pad_plane(yuv_data, ctx->config.width, ctx->config.height,
                       ctx->padded_width, ctx->padded_height, ctx->u_plane);
        yuv_data += plane_size;

        /* V plane (full resolution) */
        copy_pad_plane(yuv_data, ctx->config.width, ctx->config.height,
                       ctx->padded_width, ctx->padded_height, ctx->v_plane);
        yuv_data += plane_size;

        /* Alpha plane if present */
        if (ctx->a_plane && ctx->config.profile >= PRORES_PROFILE_4444) {
            copy_pad_plane(yuv_data, ctx->config.width, ctx->config.height,
                           ctx->padded_width, ctx->padded_height, ctx->a_plane);
        }
    } else {
        /* U plane (half horizontal resolution) */
        int src_chroma_width = (ctx->config.width + 1) / 2;
        int dst_chroma_width = ctx->padded_width / 2;
        int chroma_size = src_chroma_width * ctx->config.height;
        copy_pad_plane(yuv_data, src_chroma_width, ctx->config.height,
                       dst_chroma_width, ctx->padded_height, ctx->u_plane);
        yuv_data += chroma_size;

        /* V plane (half horizontal resolution) */
        copy_pad_plane(yuv_data, src_chroma_width, ctx->config.height,
                       dst_chroma_width, ctx->padded_height, ctx->v_plane);
    }

    /* Write frame header directly to buffer */
    int frame_hdr_size = write_frame_header(ctx, ctx->output_buf);

    /* Write picture header directly to buffer */
    int picture_header_pos = frame_hdr_size;
    int pic_hdr_size = write_picture_header(ctx, ctx->output_buf + frame_hdr_size);

    /* Initialize bitstream writer after both headers */
    int data_start = frame_hdr_size + pic_hdr_size;
    init_put_bits(&pb, ctx->output_buf + data_start, ctx->output_capacity - data_start);

    /* Encode slices and track their sizes
     * Slices are organized as: for each MB row, encode slices_per_row slices */
    int* slice_sizes = (int*)malloc(ctx->num_slices * sizeof(int));
    int slice_idx = 0;

    for (slice_y = 0; slice_y < ctx->mb_height; slice_y++) {
        for (int slice_in_row = 0; slice_in_row < ctx->slices_per_row; slice_in_row++) {
            int slice_mb_x = slice_in_row * ctx->slice_mb_width;
            int slice_width = ctx->slice_mb_width;

            /* Handle last slice in row which may be narrower */
            if (slice_mb_x + slice_width > ctx->mb_width) {
                slice_width = ctx->mb_width - slice_mb_x;
            }

            int start_offset = put_bits_count(&pb) / 8;
            encode_slice(ctx, &pb, slice_mb_x, slice_y, slice_width);
            int end_offset = put_bits_count(&pb) / 8;
            slice_sizes[slice_idx++] = end_offset - start_offset;
        }
    }

    flush_put_bits(&pb);

    /* Fill in slice size table (each entry is 16-bit slice length in bytes)
     * Per ProRes spec: "Slice index table consists of 16bit entries -
     * one for each slice - giving the length of the data for each slice" */
    for (i = 0; i < ctx->num_slices; i++) {
        int size_in_bytes = slice_sizes[i];
        ctx->output_buf[picture_header_pos + 8 + i * 2] = (size_in_bytes >> 8) & 0xFF;
        ctx->output_buf[picture_header_pos + 8 + i * 2 + 1] = size_in_bytes & 0xFF;
    }

    free(slice_sizes);

    /* Fill in picture data size (header + slices) at picture header bytes 1-4 (32-bit BE) */
    int slice_data_size = put_bytes_count(&pb);
    int picture_data_size = pic_hdr_size + slice_data_size;
    ctx->output_buf[picture_header_pos + 1] = (picture_data_size >> 24) & 0xFF;
    ctx->output_buf[picture_header_pos + 2] = (picture_data_size >> 16) & 0xFF;
    ctx->output_buf[picture_header_pos + 3] = (picture_data_size >> 8) & 0xFF;
    ctx->output_buf[picture_header_pos + 4] = picture_data_size & 0xFF;

    /* Fill in frame size */
    int frame_size = data_start + slice_data_size;
    ctx->output_buf[0] = (frame_size >> 24) & 0xFF;
    ctx->output_buf[1] = (frame_size >> 16) & 0xFF;
    ctx->output_buf[2] = (frame_size >> 8) & 0xFF;
    ctx->output_buf[3] = frame_size & 0xFF;

    /* Output */
    *out_data = (uint8_t*)malloc(frame_size);
    if (!*out_data) {
        return -1;
    }
    memcpy(*out_data, ctx->output_buf, frame_size);
    *out_size = frame_size;

    ctx->frame_count++;
    return 0;
}

uint32_t prores_encoder_get_fourcc(const ProResEncoderContext* ctx)
{
    if (!ctx) return 0;
    return profile_fourcc[ctx->config.profile];
}

ProResPixelFormat prores_encoder_get_pixel_format(const ProResEncoderContext* ctx)
{
    if (!ctx) return PRORES_PIX_FMT_YUV422P10;

    if (ctx->config.profile == PRORES_PROFILE_4444 ||
        ctx->config.profile == PRORES_PROFILE_4444XQ) {
        return ctx->a_plane ? PRORES_PIX_FMT_YUVA444P10 : PRORES_PIX_FMT_YUV444P10;
    }
    return PRORES_PIX_FMT_YUV422P10;
}

void prores_encoder_destroy(ProResEncoderContext* ctx)
{
    if (!ctx) return;

    free(ctx->y_plane);
    free(ctx->u_plane);
    free(ctx->v_plane);
    free(ctx->a_plane);
    free(ctx->dct_block);
    free(ctx->output_buf);
    free(ctx->slice_luma_buf);
    free(ctx->slice_u_buf);
    free(ctx->slice_v_buf);
    free(ctx->slice_alpha_buf);
    free(ctx->alpha_pixel_buf);
    free(ctx);
}

/* Color conversion functions */

void rgba_to_yuv422p10(const uint8_t* rgba, uint16_t* yuv, int width, int height, ProResColorRange range)
{
    int x, y;
    int plane_size = width * height;
    int chroma_width = (width + 1) / 2;

    uint16_t* y_plane = yuv;
    uint16_t* u_plane = yuv + plane_size;
    uint16_t* v_plane = yuv + plane_size + chroma_width * height;

    /* BT.709 12-bit fixed-point coefficients (Kr=0.2126, Kg=0.7152, Kb=0.0722)
     * Computed directly to 10-bit output from 8-bit RGB input */
    int RY, GY, BY, y_offset;
    int RCb, GCb, BCb, RCr, GCr, BCr, c_offset;
    int y_min, y_max, c_min, c_max;

    if (range == PRORES_RANGE_FULL) {
        RY = 3493;  GY = 11751; BY = 1186;  y_offset = 0;
        RCb = 1883; GCb = 6336; BCb = 8217; c_offset = 512;
        RCr = 8217; GCr = 7465; BCr = 754;
        y_min = 0;   y_max = 1023;
        c_min = 0;   c_max = 1023;
    } else {
        RY = 2992;  GY = 10066; BY = 1016;  y_offset = 64;
        RCb = 1649; GCb = 5548; BCb = 7193; c_offset = 512;
        RCr = 7193; GCr = 6534; BCr = 660;
        y_min = 64;  y_max = 940;
        c_min = 64;  c_max = 960;
    }

    for (y = 0; y < height; y++) {
        for (x = 0; x < width; x++) {
            int idx = (y * width + x) * 4;
            int r = rgba[idx + 0];
            int g = rgba[idx + 1];
            int b = rgba[idx + 2];

            int y_val = ((RY * r + GY * g + BY * b + 2048) >> 12) + y_offset;
            if (y_val < y_min) y_val = y_min;
            if (y_val > y_max) y_val = y_max;
            y_plane[y * width + x] = y_val;

            /* Subsample chroma (average of 2 horizontal pixels) */
            if ((x & 1) == 0) {
                int r2 = r, g2 = g, b2 = b;
                if (x + 1 < width) {
                    r2 = rgba[idx + 4 + 0];
                    g2 = rgba[idx + 4 + 1];
                    b2 = rgba[idx + 4 + 2];
                }

                int ravg = (r + r2) / 2;
                int gavg = (g + g2) / 2;
                int bavg = (b + b2) / 2;

                int cb = ((-RCb * ravg - GCb * gavg + BCb * bavg + 2048) >> 12) + c_offset;
                int cr = ((RCr * ravg - GCr * gavg - BCr * bavg + 2048) >> 12) + c_offset;

                if (cb < c_min) cb = c_min;
                if (cb > c_max) cb = c_max;
                if (cr < c_min) cr = c_min;
                if (cr > c_max) cr = c_max;

                u_plane[y * chroma_width + x / 2] = cb;
                v_plane[y * chroma_width + x / 2] = cr;
            }
        }
    }
}

void rgba_to_yuv444p10(const uint8_t* rgba, uint16_t* yuv, int width, int height, ProResColorRange range)
{
    int x, y;
    int plane_size = width * height;

    uint16_t* y_plane = yuv;
    uint16_t* u_plane = yuv + plane_size;
    uint16_t* v_plane = yuv + plane_size * 2;

    int RY, GY, BY, y_offset;
    int RCb, GCb, BCb, RCr, GCr, BCr, c_offset;
    int y_min, y_max, c_min, c_max;

    if (range == PRORES_RANGE_FULL) {
        RY = 3493;  GY = 11751; BY = 1186;  y_offset = 0;
        RCb = 1883; GCb = 6336; BCb = 8217; c_offset = 512;
        RCr = 8217; GCr = 7465; BCr = 754;
        y_min = 0;   y_max = 1023;
        c_min = 0;   c_max = 1023;
    } else {
        RY = 2992;  GY = 10066; BY = 1016;  y_offset = 64;
        RCb = 1649; GCb = 5548; BCb = 7193; c_offset = 512;
        RCr = 7193; GCr = 6534; BCr = 660;
        y_min = 64;  y_max = 940;
        c_min = 64;  c_max = 960;
    }

    for (y = 0; y < height; y++) {
        for (x = 0; x < width; x++) {
            int idx = (y * width + x) * 4;
            int r = rgba[idx + 0];
            int g = rgba[idx + 1];
            int b = rgba[idx + 2];

            int y_val = ((RY * r + GY * g + BY * b + 2048) >> 12) + y_offset;
            int cb = ((-RCb * r - GCb * g + BCb * b + 2048) >> 12) + c_offset;
            int cr = ((RCr * r - GCr * g - BCr * b + 2048) >> 12) + c_offset;

            if (y_val < y_min) y_val = y_min;
            if (y_val > y_max) y_val = y_max;
            if (cb < c_min) cb = c_min;
            if (cb > c_max) cb = c_max;
            if (cr < c_min) cr = c_min;
            if (cr > c_max) cr = c_max;

            y_plane[y * width + x] = y_val;
            u_plane[y * width + x] = cb;
            v_plane[y * width + x] = cr;
        }
    }
}

void rgba_to_yuva444p10(const uint8_t* rgba, uint16_t* yuva, int width, int height, ProResColorRange range)
{
    int x, y;
    int plane_size = width * height;

    uint16_t* y_plane = yuva;
    uint16_t* u_plane = yuva + plane_size;
    uint16_t* v_plane = yuva + plane_size * 2;
    uint16_t* a_plane = yuva + plane_size * 3;

    int RY, GY, BY, y_offset;
    int RCb, GCb, BCb, RCr, GCr, BCr, c_offset;
    int y_min, y_max, c_min, c_max;

    if (range == PRORES_RANGE_FULL) {
        RY = 3493;  GY = 11751; BY = 1186;  y_offset = 0;
        RCb = 1883; GCb = 6336; BCb = 8217; c_offset = 512;
        RCr = 8217; GCr = 7465; BCr = 754;
        y_min = 0;   y_max = 1023;
        c_min = 0;   c_max = 1023;
    } else {
        RY = 2992;  GY = 10066; BY = 1016;  y_offset = 64;
        RCb = 1649; GCb = 5548; BCb = 7193; c_offset = 512;
        RCr = 7193; GCr = 6534; BCr = 660;
        y_min = 64;  y_max = 940;
        c_min = 64;  c_max = 960;
    }

    /* Alpha: scale 8-bit (0-255) to 10-bit (0-1023) directly
     * 1023/255 ≈ 4.012, use fixed-point: (a * 16430 + 2048) >> 12 */
    for (y = 0; y < height; y++) {
        for (x = 0; x < width; x++) {
            int idx = (y * width + x) * 4;
            int r = rgba[idx + 0];
            int g = rgba[idx + 1];
            int b = rgba[idx + 2];
            int a = rgba[idx + 3];

            int y_val = ((RY * r + GY * g + BY * b + 2048) >> 12) + y_offset;
            int cb = ((-RCb * r - GCb * g + BCb * b + 2048) >> 12) + c_offset;
            int cr = ((RCr * r - GCr * g - BCr * b + 2048) >> 12) + c_offset;
            int a_val = (a * 16430 + 2048) >> 12;

            if (y_val < y_min) y_val = y_min;
            if (y_val > y_max) y_val = y_max;
            if (cb < c_min) cb = c_min;
            if (cb > c_max) cb = c_max;
            if (cr < c_min) cr = c_min;
            if (cr > c_max) cr = c_max;
            if (a_val > 1023) a_val = 1023;

            y_plane[y * width + x] = y_val;
            u_plane[y * width + x] = cb;
            v_plane[y * width + x] = cr;
            a_plane[y * width + x] = a_val;
        }
    }
}
