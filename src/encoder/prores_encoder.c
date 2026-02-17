/*
 * ProRes Encoder Implementation
 * Based on Apple ProRes specification
 */

#include "prores_encoder.h"
#include "prores_dct.h"
#include "prores_vlc.h"
#include <stdlib.h>
#include <string.h>
#include <limits.h>

/* ProRes frame header magic */
#define PRORES_FRAME_MAGIC  0x69637066  /* 'icpf' */

/* Maximum slices per picture */
#define MAX_SLICES_PER_LINE  256
#define MAX_SLICES          8192

/* Macroblock size */
#define MB_SIZE 16
/* Maximum size for a single plane's encoded data in a slice */
#define MAX_PLANE_DATA_SIZE 65536

/* Maximum blocks per slice: 8 MBs x 4 luma blocks = 32 blocks per component */
#define MAX_BLOCKS_PER_SLICE 32

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

/* Profile info table: quant ranges and bitrate targets per resolution tier
 * From FFmpeg's proresenc_kostya.c (prores_profile_info) */
static const struct {
    int min_quant;
    int max_quant;
    int br_tab[4]; /* bits per MB by resolution tier */
} prores_profile_info[6] = {
    { 4, 8, {  300,  242,  220,  194 } },  /* Proxy */
    { 1, 9, {  720,  560,  490,  440 } },  /* LT */
    { 1, 6, { 1050,  808,  710,  632 } },  /* Standard */
    { 1, 6, { 1566, 1216, 1070,  950 } },  /* HQ */
    { 1, 6, { 2350, 1828, 1600, 1425 } },  /* 4444 */
    { 1, 6, { 3525, 2742, 2400, 2137 } },  /* 4444 XQ */
};

/* Trellis search parameters for adaptive quantization */
#define TRELLIS_WIDTH  16
#define MAX_STORED_Q   16
#define SCORE_LIMIT    (INT_MAX / 2)

typedef struct {
    int prev_node;
    int quant;
    int bits;
    int score;
} TrellisNode;

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
    int bit_depth;
    int sample_center;

    /* Adaptive quantization */
    int min_quant;
    int max_quant;
    int bits_per_mb;                            /* target bits per macroblock */
    int16_t quants[MAX_STORED_Q][64];           /* pre-computed luma qmat * q */
    int16_t quants_chroma[MAX_STORED_Q][64];    /* pre-computed chroma qmat * q */
    int* slice_q;                               /* per-slice quant decisions */

    /* Per-row DCT block storage (for trellis estimation then encoding) */
    int16_t* row_luma_blocks;       /* [slices_per_row * MAX_BLOCKS_PER_SLICE * 64] */
    int16_t* row_u_blocks;
    int16_t* row_v_blocks;
    int* row_luma_block_counts;     /* [slices_per_row] */
    int* row_chroma_block_counts;   /* [slices_per_row] */

    /* Trellis nodes */
    TrellisNode* trellis;           /* [(slices_per_row + 1) * TRELLIS_WIDTH] */

    /* Working buffers */
    int16_t* y_plane;        /* Luma buffer */
    int16_t* u_plane;        /* Cb buffer */
    int16_t* v_plane;        /* Cr buffer */
    int16_t* a_plane;        /* Alpha buffer (4444 only) */

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

    /* Always use 10-bit internally, matching FFmpeg (avctx->bits_per_raw_sample = 10).
     * This ensures DCT coefficients fit in int16_t (max DC = 32 * 1023 = 32736). */
    ctx->bit_depth = 10;
    ctx->sample_center = 1 << (ctx->bit_depth - 1);

    /* Copy quantization matrices */
    memcpy(ctx->quant_matrix, get_quant_matrix(config->profile), 64);
    memcpy(ctx->chroma_quant_matrix, get_chroma_quant_matrix(config->profile), 64);

    /* Adaptive quantization setup */
    ctx->min_quant = prores_profile_info[config->profile].min_quant;
    ctx->max_quant = prores_profile_info[config->profile].max_quant;

    /* Resolution tier selection (by total MB count) */
    {
        int total_mbs = ctx->mb_width * ctx->mb_height;
        int tier;
        if (total_mbs <= 1620)      tier = 0;  /* up to 720x576 */
        else if (total_mbs <= 2700) tier = 1;  /* up to 960x720 */
        else if (total_mbs <= 6075) tier = 2;  /* up to 1440x1080 */
        else                        tier = 3;
        ctx->bits_per_mb = prores_profile_info[config->profile].br_tab[tier];
        /* FFmpeg multiplies bits_per_mb by 20 when alpha is present.
         * The alpha plane is run-coded and can be large for varying alpha,
         * so the inflated budget ensures YUV quality isn't constrained.
         * We always encode alpha for 4444 profiles. */
        if (is_444_profile(config->profile))
            ctx->bits_per_mb *= 20;
    }

    /* Pre-compute quantization matrices for each possible q_scale */
    {
        int num_q = ctx->max_quant - ctx->min_quant + 1;
        for (int qi = 0; qi < num_q && qi < MAX_STORED_Q; qi++) {
            int q = ctx->min_quant + qi;
            for (int i = 0; i < 64; i++) {
                ctx->quants[qi][i] = (int16_t)(ctx->quant_matrix[i] * q);
                ctx->quants_chroma[qi][i] = (int16_t)(ctx->chroma_quant_matrix[i] * q);
            }
        }
    }

    /* Allocate per-slice quant decisions */
    ctx->slice_q = (int*)calloc(ctx->num_slices, sizeof(int));

    /* Allocate per-row DCT block storage */
    {
        int row_blocks = ctx->slices_per_row * MAX_BLOCKS_PER_SLICE * 64;
        ctx->row_luma_blocks = (int16_t*)malloc(row_blocks * sizeof(int16_t));
        ctx->row_u_blocks = (int16_t*)malloc(row_blocks * sizeof(int16_t));
        ctx->row_v_blocks = (int16_t*)malloc(row_blocks * sizeof(int16_t));
        ctx->row_luma_block_counts = (int*)malloc(ctx->slices_per_row * sizeof(int));
        ctx->row_chroma_block_counts = (int*)malloc(ctx->slices_per_row * sizeof(int));
    }

    /* Allocate trellis nodes */
    ctx->trellis = (TrellisNode*)malloc((ctx->slices_per_row + 1) * TRELLIS_WIDTH * sizeof(TrellisNode));

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

    if (!ctx->y_plane || !ctx->u_plane || !ctx->v_plane ||
        !ctx->output_buf || !ctx->slice_luma_buf || !ctx->slice_u_buf || !ctx->slice_v_buf ||
        !ctx->slice_q || !ctx->row_luma_blocks || !ctx->row_u_blocks || !ctx->row_v_blocks ||
        !ctx->row_luma_block_counts || !ctx->row_chroma_block_counts || !ctx->trellis) {
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
 * Bit estimation functions for adaptive quantization
 * These mirror the encoding functions but return bit counts + accumulate
 * quantization error (|coeff| % q) for trellis optimization.
 */

/* Estimate DC encoding bits + accumulate quantization error */
static int estimate_dcs(int *error, int16_t blocks[][64],
                        int blocks_per_slice, int scale)
{
    int bits = 0;
    int16_t *flat = &blocks[0][0];

    if (scale < 1) scale = 1;

    /* First DC */
    int dc_raw = flat[0] - 0x4000;
    int prev_dc = dc_raw / scale;
    int abs_dc_raw = (dc_raw < 0) ? -dc_raw : dc_raw;
    *error += abs_dc_raw % scale;

    unsigned int uval = (prev_dc < 0) ? (unsigned int)(-prev_dc * 2 - 1) : (unsigned int)(prev_dc * 2);
    bits += prores_estimate_vlc_codeword(PRORES_FIRST_DC_CB, uval);

    int sign = 0;
    int codebook_idx = 5;
    flat += 64;

    for (int b = 1; b < blocks_per_slice; b++, flat += 64) {
        dc_raw = flat[0] - 0x4000;
        int dc = dc_raw / scale;
        abs_dc_raw = (dc_raw < 0) ? -dc_raw : dc_raw;
        *error += abs_dc_raw % scale;

        int delta = dc - prev_dc;
        int new_sign = (delta < 0) ? -1 : 0;
        delta = (delta ^ sign) - sign;

        unsigned int code = (delta < 0) ? (unsigned int)(-delta * 2 - 1) : (unsigned int)(delta * 2);
        uint8_t cb = prores_dc_codebook[codebook_idx < 7 ? codebook_idx : 6];
        bits += prores_estimate_vlc_codeword(cb, code);

        codebook_idx = (code < 7) ? (int)code : 6;
        sign = new_sign;
        prev_dc = dc;
    }

    return bits;
}

/* Estimate AC encoding bits + accumulate quantization error */
static int estimate_acs(int *error, int16_t blocks[][64],
                        int blocks_per_slice, const uint8_t *scan,
                        const int16_t *qmat)
{
    int prev_run = 4;
    int prev_level = 2;
    int run = 0;
    int bits = 0;
    int max_coeffs = blocks_per_slice << 6;
    int16_t *flat = &blocks[0][0];

    for (int i = 1; i < 64; i++) {
        int q = qmat[scan[i]];
        if (q < 1) q = 1;
        for (int idx = scan[i]; idx < max_coeffs; idx += 64) {
            int coeff = flat[idx];
            int level = coeff / q;
            int abs_coeff = (coeff < 0) ? -coeff : coeff;
            *error += abs_coeff % q;
            if (level) {
                int abs_level = (level < 0) ? -level : level;
                int rcb = (prev_run < 16) ? prev_run : 15;
                int lcb = (prev_level < 10) ? prev_level : 9;
                bits += prores_estimate_vlc_codeword(prores_run_to_cb[rcb], run);
                bits += prores_estimate_vlc_codeword(prores_lev_to_cb[lcb], abs_level - 1);
                bits += 1;  /* sign bit */
                prev_run = (run < 16) ? run : 15;
                prev_level = (abs_level < 10) ? abs_level : 9;
                run = 0;
            } else {
                run++;
            }
        }
    }

    return bits;
}

/* Estimate total bits for one plane of one slice at a given quant.
 * Returns bits aligned to byte boundary (matching FFmpeg's FFALIGN(bits, 8)). */
static int estimate_slice_plane(int *error, int16_t blocks[][64],
                                int blocks_per_slice,
                                const int16_t *qmat)
{
    int bits = 0;
    int scale = qmat[0];
    bits += estimate_dcs(error, blocks, blocks_per_slice, scale);
    bits += estimate_acs(error, blocks, blocks_per_slice, prores_scan, qmat);
    return (bits + 7) & ~7;  /* byte-align like FFmpeg */
}

/* DCT all blocks for one slice, storing results in output arrays.
 * Separates DCT from encoding so blocks can be reused for estimation + encoding. */
static void dct_slice_blocks(ProResEncoderContext* ctx,
                              int slice_mb_x, int mb_y, int slice_width,
                              int16_t luma_out[][64], int *luma_count,
                              int16_t u_out[][64], int *u_count,
                              int16_t v_out[][64], int *v_count)
{
    int is_444 = is_444_profile(ctx->config.profile);
    int mb_x, block_x, block_y;

    *luma_count = 0;
    *u_count = 0;
    *v_count = 0;

    /* Luma blocks (row-major: TL, TR, BL, BR) */
    for (mb_x = 0; mb_x < slice_width; mb_x++) {
        int pixel_x = (slice_mb_x + mb_x) * MB_SIZE;
        int pixel_y = mb_y * MB_SIZE;
        for (block_y = 0; block_y < 2; block_y++) {
            for (block_x = 0; block_x < 2; block_x++) {
                int bx = pixel_x + block_x * 8;
                int by = pixel_y + block_y * 8;
                dct_block(ctx->y_plane + by * ctx->padded_width + bx,
                          ctx->padded_width,
                          luma_out[(*luma_count)++]);
            }
        }
    }

    /* U blocks */
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
                              u_out[(*u_count)++]);
                }
            }
        } else {
            int chroma_width = ctx->padded_width / 2;
            for (block_y = 0; block_y < 2; block_y++) {
                int bx = pixel_x / 2;
                int by = pixel_y + block_y * 8;
                dct_block(ctx->u_plane + by * chroma_width + bx,
                          chroma_width,
                          u_out[(*u_count)++]);
            }
        }
    }

    /* V blocks (same structure as U) */
    for (mb_x = 0; mb_x < slice_width; mb_x++) {
        int pixel_x = (slice_mb_x + mb_x) * MB_SIZE;
        int pixel_y = mb_y * MB_SIZE;
        if (is_444) {
            for (block_x = 0; block_x < 2; block_x++) {
                for (block_y = 0; block_y < 2; block_y++) {
                    int bx = pixel_x + block_x * 8;
                    int by = pixel_y + block_y * 8;
                    dct_block(ctx->v_plane + by * ctx->padded_width + bx,
                              ctx->padded_width,
                              v_out[(*v_count)++]);
                }
            }
        } else {
            int chroma_width = ctx->padded_width / 2;
            for (block_y = 0; block_y < 2; block_y++) {
                int bx = pixel_x / 2;
                int by = pixel_y + block_y * 8;
                dct_block(ctx->v_plane + by * chroma_width + bx,
                          chroma_width,
                          v_out[(*v_count)++]);
            }
        }
    }
}

/* Viterbi trellis search across all slices in a MB row.
 * Matches FFmpeg's find_slice_quant algorithm:
 * - Tries q values from min_quant to max_quant
 * - If max_quant doesn't fit, searches up to q=128 ("overquant")
 * - Uses cumulative bit tracking across slices
 * - Nodes indexed by q value (min_quant..max_quant+1, where +1 = overquant) */
static void find_slice_quants(ProResEncoderContext *ctx, int mb_row)
{
    int slices = ctx->slices_per_row;
    int is_444 = is_444_profile(ctx->config.profile);
    int base_slice_idx = mb_row * slices;
    int min_q = ctx->min_quant;
    int max_q = ctx->max_quant;
    int num_q = max_q - min_q + 2;  /* +1 for the overquant slot */

    /* Access trellis nodes as [position][qi] where qi = q - min_q
     * qi ranges from 0 to num_q-1, with num_q-1 being the overquant slot */
    TrellisNode *nodes = ctx->trellis;
    #define TNODE(pos, qi) nodes[(pos) * TRELLIS_WIDTH + (qi)]

    /* Initialize position 0 (before first slice) */
    for (int qi = 0; qi < num_q; qi++) {
        TNODE(0, qi).score = 0;
        TNODE(0, qi).bits = 0;
        TNODE(0, qi).quant = min_q + qi;
        TNODE(0, qi).prev_node = -1;
    }

    int mbs_so_far = 0;

    /* Forward pass: for each slice */
    for (int s = 0; s < slices; s++) {
        int slice_mb_x = s * ctx->slice_mb_width;
        int slice_width = ctx->slice_mb_width;
        if (slice_mb_x + slice_width > ctx->mb_width)
            slice_width = ctx->mb_width - slice_mb_x;
        mbs_so_far += slice_width;

        /* Cumulative bit budget (total bits allowed up through this slice) */
        int bits_limit = ctx->bits_per_mb * mbs_so_far;

        /* Get pointers to this slice's pre-computed DCT blocks */
        int16_t (*luma)[64] = (int16_t (*)[64])(ctx->row_luma_blocks + s * MAX_BLOCKS_PER_SLICE * 64);
        int luma_count = ctx->row_luma_block_counts[s];
        int16_t (*u)[64] = (int16_t (*)[64])(ctx->row_u_blocks + s * MAX_BLOCKS_PER_SLICE * 64);
        int16_t (*v)[64] = (int16_t (*)[64])(ctx->row_v_blocks + s * MAX_BLOCKS_PER_SLICE * 64);
        int chroma_count = ctx->row_chroma_block_counts[s];

        int hdr_bits = is_444 ? 64 : 48;

        /* Per-q estimation arrays */
        int slice_bits[TRELLIS_WIDTH];
        int slice_score[TRELLIS_WIDTH];

        /* Estimate bits+error for q in [min_q, max_q] */
        for (int qi = 0; qi <= max_q - min_q; qi++) {
            int error = 0;
            int bits = 0;
            bits += estimate_slice_plane(&error, luma, luma_count, ctx->quants[qi]);
            bits += estimate_slice_plane(&error, u, chroma_count, ctx->quants_chroma[qi]);
            bits += estimate_slice_plane(&error, v, chroma_count, ctx->quants_chroma[qi]);
            bits += hdr_bits;
            if (bits > 65000 * 8)
                error = SCORE_LIMIT;
            slice_bits[qi] = bits;
            slice_score[qi] = error;
        }

        /* Overquant: if max_quant doesn't fit the per-slice budget, search higher */
        int oq_idx = num_q - 1;  /* overquant slot index */
        int per_slice_budget = ctx->bits_per_mb * slice_width;
        int overquant;

        if (slice_bits[max_q - min_q] <= per_slice_budget) {
            /* Max quant fits — overquant = max_quant with slightly worse score */
            slice_bits[oq_idx] = slice_bits[max_q - min_q];
            slice_score[oq_idx] = slice_score[max_q - min_q] + 1;
            overquant = max_q;
        } else {
            /* Search beyond max_quant up to q=128 */
            int q, bits = 0, error = 0;
            for (q = max_q + 1; q < 128; q++) {
                error = 0;
                bits = 0;
                /* Compute qmat on the fly for q beyond pre-computed range */
                int16_t qmat_luma[64], qmat_chroma[64];
                for (int i = 0; i < 64; i++) {
                    qmat_luma[i] = (int16_t)(ctx->quant_matrix[i] * q);
                    qmat_chroma[i] = (int16_t)(ctx->chroma_quant_matrix[i] * q);
                }
                bits += estimate_slice_plane(&error, luma, luma_count, qmat_luma);
                bits += estimate_slice_plane(&error, u, chroma_count, qmat_chroma);
                bits += estimate_slice_plane(&error, v, chroma_count, qmat_chroma);
                bits += hdr_bits;
                if (bits <= per_slice_budget)
                    break;
            }
            slice_bits[oq_idx] = bits;
            slice_score[oq_idx] = error;
            overquant = q;
        }

        /* Initialize next position nodes */
        for (int qi = 0; qi < num_q; qi++) {
            TNODE(s + 1, qi).prev_node = -1;
            TNODE(s + 1, qi).quant = (qi < oq_idx) ? (min_q + qi) : overquant;
            TNODE(s + 1, qi).score = SCORE_LIMIT;
            TNODE(s + 1, qi).bits = 0;
        }

        /* Trellis forward pass: try all (prev_q, cur_q) pairs */
        for (int pqi = 0; pqi < num_q; pqi++) {
            for (int qi = 0; qi < num_q; qi++) {
                int bits = TNODE(s, pqi).bits + slice_bits[qi];
                int error = slice_score[qi];

                if (bits > bits_limit)
                    error = SCORE_LIMIT;

                int new_score;
                if (TNODE(s, pqi).score < SCORE_LIMIT && error < SCORE_LIMIT)
                    new_score = TNODE(s, pqi).score + error;
                else
                    new_score = SCORE_LIMIT;

                if (TNODE(s + 1, qi).prev_node == -1 ||
                    TNODE(s + 1, qi).score >= new_score) {
                    TNODE(s + 1, qi).bits = bits;
                    TNODE(s + 1, qi).score = new_score;
                    TNODE(s + 1, qi).prev_node = pqi;
                }
            }
        }
    }

    /* Find best terminal node */
    int best_qi = 0;
    for (int qi = 1; qi < num_q; qi++) {
        if (TNODE(slices, qi).score <= TNODE(slices, best_qi).score)
            best_qi = qi;
    }

    /* Backtrack to populate slice_q[] */
    int qi = best_qi;
    for (int s = slices - 1; s >= 0; s--) {
        ctx->slice_q[base_slice_idx + s] = TNODE(s + 1, qi).quant;
        qi = TNODE(s + 1, qi).prev_node;
    }

    #undef TNODE
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

/* Encode a single slice from pre-computed DCT blocks with a given quant.
 * ProRes slice structure:
 *   422:  6-byte header (hdr_size, scale, luma_size, u_size) + Y + U + V
 *   4444: 8-byte header (hdr_size, scale, luma_size, u_size, v_size) + Y + U + V + Alpha
 */
static int encode_slice(ProResEncoderContext* ctx, PutBitContext* pb,
                       int slice_mb_x, int slice_mb_y, int slice_width,
                       int quant,
                       int16_t luma_blocks[][64], int luma_block_count,
                       int16_t u_blocks[][64], int chroma_block_count,
                       int16_t v_blocks[][64], int v_block_count)
{
    int is_444 = is_444_profile(ctx->config.profile);

    /* Temporary buffers for encoded plane data (heap-backed to avoid WASM stack overflow) */
    uint8_t* luma_data = ctx->slice_luma_buf;
    uint8_t* u_data = ctx->slice_u_buf;
    uint8_t* v_data = ctx->slice_v_buf;

    /* Encode luma to temp buffer */
    PutBitContext luma_pb;
    init_put_bits(&luma_pb, luma_data, ctx->slice_buf_capacity);
    encode_dc_coeffs(&luma_pb, luma_blocks, luma_block_count, ctx->quant_matrix, quant);
    encode_ac_coeffs_all(&luma_pb, luma_blocks, luma_block_count,
                         prores_scan, ctx->quant_matrix, quant);

    int luma_bits = put_bits_count(&luma_pb);
    if (luma_bits % 8) put_bits(&luma_pb, 8 - (luma_bits % 8), 0);
    flush_put_bits(&luma_pb);
    int luma_size = (put_bits_count(&luma_pb) + 7) / 8;
    if ((size_t)luma_size > ctx->slice_buf_capacity) return -1;

    /* Encode chroma U to temp buffer */
    PutBitContext u_pb;
    init_put_bits(&u_pb, u_data, ctx->slice_buf_capacity);
    encode_dc_coeffs(&u_pb, u_blocks, chroma_block_count, ctx->chroma_quant_matrix, quant);
    encode_ac_coeffs_all(&u_pb, u_blocks, chroma_block_count,
                         prores_scan, ctx->chroma_quant_matrix, quant);

    int u_bits = put_bits_count(&u_pb);
    if (u_bits % 8) put_bits(&u_pb, 8 - (u_bits % 8), 0);
    flush_put_bits(&u_pb);
    int u_size = (put_bits_count(&u_pb) + 7) / 8;
    if ((size_t)u_size > ctx->slice_buf_capacity) return -1;

    /* Encode chroma V to temp buffer */
    PutBitContext v_pb;
    init_put_bits(&v_pb, v_data, ctx->slice_buf_capacity);
    encode_dc_coeffs(&v_pb, v_blocks, v_block_count, ctx->chroma_quant_matrix, quant);
    encode_ac_coeffs_all(&v_pb, v_blocks, v_block_count,
                         prores_scan, ctx->chroma_quant_matrix, quant);

    int v_bits = put_bits_count(&v_pb);
    if (v_bits % 8) put_bits(&v_pb, 8 - (v_bits % 8), 0);
    flush_put_bits(&v_pb);
    int v_size = (put_bits_count(&v_pb) + 7) / 8;
    if ((size_t)v_size > ctx->slice_buf_capacity) return -1;

    /* Encode alpha for 4444 profiles (pixel-level, not DCT — doesn't depend on quant) */
    int alpha_size = 0;
    if (is_444 && ctx->a_plane) {
        int slice_pixel_w = slice_width * MB_SIZE;
        int pixel_x = slice_mb_x * MB_SIZE;
        int pixel_y = slice_mb_y * MB_SIZE;

        int num_alpha_pixels = get_alpha_data(ctx->a_plane, ctx->padded_width,
                                              pixel_x, pixel_y,
                                              slice_pixel_w, ctx->padded_height,
                                              ctx->alpha_pixel_buf);

        PutBitContext alpha_pb;
        init_put_bits(&alpha_pb, ctx->slice_alpha_buf, MAX_PLANE_DATA_SIZE);
        encode_alpha_plane(&alpha_pb, ctx->alpha_pixel_buf, num_alpha_pixels, 16);

        int alpha_bits = put_bits_count(&alpha_pb);
        if (alpha_bits % 8) put_bits(&alpha_pb, 8 - (alpha_bits % 8), 0);
        flush_put_bits(&alpha_pb);
        alpha_size = (put_bits_count(&alpha_pb) + 7) / 8;
    }

    /* Write slice header with known sizes */
    if (is_444 && ctx->a_plane) {
        put_bits(pb, 8, 64);       /* slice_hdr_size = 64 bits = 8 bytes */
        put_bits(pb, 8, quant);    /* scale_factor */
        put_bits(pb, 16, luma_size);
        put_bits(pb, 16, u_size);
        put_bits(pb, 16, v_size);
    } else {
        put_bits(pb, 8, 48);       /* slice_hdr_size = 48 bits = 6 bytes */
        put_bits(pb, 8, quant);    /* scale_factor */
        put_bits(pb, 16, luma_size);
        put_bits(pb, 16, u_size);
    }

    /* Write plane data byte by byte */
    for (int i = 0; i < luma_size; i++) put_bits(pb, 8, luma_data[i]);
    for (int i = 0; i < u_size; i++) put_bits(pb, 8, u_data[i]);
    for (int i = 0; i < v_size; i++) put_bits(pb, 8, v_data[i]);

    if (is_444 && ctx->a_plane && alpha_size > 0) {
        for (int i = 0; i < alpha_size; i++) put_bits(pb, 8, ctx->slice_alpha_buf[i]);
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

    /* Encode slices with adaptive quantization: two-pass per MB row.
     * Pass 1: DCT all blocks for every slice in this row
     * Pass 2: Trellis search to find optimal per-slice quant
     * Pass 3: Encode each slice using stored DCT blocks + chosen quant */
    int* slice_sizes = (int*)malloc(ctx->num_slices * sizeof(int));
    int slice_idx = 0;

    for (slice_y = 0; slice_y < ctx->mb_height; slice_y++) {
        /* Pass 1: DCT all blocks for this MB row */
        for (int s = 0; s < ctx->slices_per_row; s++) {
            int slice_mb_x = s * ctx->slice_mb_width;
            int slice_width = ctx->slice_mb_width;
            if (slice_mb_x + slice_width > ctx->mb_width)
                slice_width = ctx->mb_width - slice_mb_x;

            int16_t (*luma)[64] = (int16_t (*)[64])(ctx->row_luma_blocks + s * MAX_BLOCKS_PER_SLICE * 64);
            int16_t (*u)[64] = (int16_t (*)[64])(ctx->row_u_blocks + s * MAX_BLOCKS_PER_SLICE * 64);
            int16_t (*v)[64] = (int16_t (*)[64])(ctx->row_v_blocks + s * MAX_BLOCKS_PER_SLICE * 64);

            /* U and V always have the same block count; use a local for V
             * to avoid sharing the same pointer (which would corrupt the count) */
            int v_count_tmp;
            dct_slice_blocks(ctx, slice_mb_x, slice_y, slice_width,
                            luma, &ctx->row_luma_block_counts[s],
                            u, &ctx->row_chroma_block_counts[s],
                            v, &v_count_tmp);
        }

        /* Pass 2: Trellis search for optimal per-slice quant values */
        find_slice_quants(ctx, slice_y);

        /* Pass 3: Encode each slice using stored blocks + chosen quant */
        for (int s = 0; s < ctx->slices_per_row; s++) {
            int slice_mb_x = s * ctx->slice_mb_width;
            int slice_width = ctx->slice_mb_width;
            if (slice_mb_x + slice_width > ctx->mb_width)
                slice_width = ctx->mb_width - slice_mb_x;

            int16_t (*luma)[64] = (int16_t (*)[64])(ctx->row_luma_blocks + s * MAX_BLOCKS_PER_SLICE * 64);
            int16_t (*u)[64] = (int16_t (*)[64])(ctx->row_u_blocks + s * MAX_BLOCKS_PER_SLICE * 64);
            int16_t (*v)[64] = (int16_t (*)[64])(ctx->row_v_blocks + s * MAX_BLOCKS_PER_SLICE * 64);

            int start_offset = put_bits_count(&pb) / 8;
            encode_slice(ctx, &pb, slice_mb_x, slice_y, slice_width,
                        ctx->slice_q[slice_idx],
                        luma, ctx->row_luma_block_counts[s],
                        u, ctx->row_chroma_block_counts[s],
                        v, ctx->row_chroma_block_counts[s]);
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
    free(ctx->output_buf);
    free(ctx->slice_luma_buf);
    free(ctx->slice_u_buf);
    free(ctx->slice_v_buf);
    free(ctx->slice_alpha_buf);
    free(ctx->alpha_pixel_buf);
    free(ctx->slice_q);
    free(ctx->row_luma_blocks);
    free(ctx->row_u_blocks);
    free(ctx->row_v_blocks);
    free(ctx->row_luma_block_counts);
    free(ctx->row_chroma_block_counts);
    free(ctx->trellis);
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

