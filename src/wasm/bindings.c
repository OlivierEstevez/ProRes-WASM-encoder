/*
 * Emscripten Bindings for ProRes WASM Encoder
 * Provides JavaScript-callable interface to the encoder
 */

#include <emscripten.h>
#include <stdlib.h>
#include <string.h>
#include "../encoder/prores_encoder.h"
#include "../muxer/mov_muxer.h"

/* Combined encoder + muxer context for simplified API */
typedef struct {
    ProResEncoderContext* encoder;
    MovMuxerContext* muxer;
    int width;
    int height;
    int profile;
    ProResColorRange range;
    uint16_t* yuv_buffer;
    size_t yuv_buffer_size;
} ProResWasmContext;

/*
 * Create a new ProRes encoder instance
 *
 * @param width       Frame width (must be multiple of 16)
 * @param height      Frame height (must be multiple of 16)
 * @param fps_num     Frame rate numerator
 * @param fps_den     Frame rate denominator
 * @param profile     Profile (0=proxy, 1=lt, 2=standard, 3=hq, 4=4444, 5=4444xq)
 * @param quality     Quality 0-100
 * @param range       Color range (0=limited, 1=full)
 * @return            Context pointer or 0 on failure
 */
EMSCRIPTEN_KEEPALIVE
void* prores_wasm_create(
    int width, int height,
    int fps_num, int fps_den,
    int profile, int quality,
    int range)
{
    ProResWasmContext* ctx = (ProResWasmContext*)calloc(1, sizeof(ProResWasmContext));
    if (!ctx) return NULL;

    ctx->width = width;
    ctx->height = height;
    ctx->profile = profile;
    ctx->range = (range != 0) ? PRORES_RANGE_FULL : PRORES_RANGE_LIMITED;

    /* Create encoder */
    ProResEncoderConfig enc_config = {
        .width = width,
        .height = height,
        .fps_num = fps_num,
        .fps_den = fps_den,
        .profile = (ProResProfile)profile,
        .colorspace = PRORES_CS_BT709,
        .frame_type = PRORES_FRAME_PROGRESSIVE,
        .quality = quality,
        .range = ctx->range
    };

    ctx->encoder = prores_encoder_create(&enc_config);
    if (!ctx->encoder) {
        free(ctx);
        return NULL;
    }

    /* Create muxer */
    int mux_bit_depth = (profile >= 4) ? 12 : 10;
    MovMuxerConfig mux_config = {
        .width = width,
        .height = height,
        .fps_num = fps_num,
        .fps_den = fps_den,
        .fourcc = prores_encoder_get_fourcc(ctx->encoder),
        .bit_depth = mux_bit_depth,
        .has_alpha = (profile >= 4),
        .color = {
            .primaries = 1,  /* BT.709 */
            .transfer = 1,
            .matrix = 1
        },
        .full_range = (ctx->range == PRORES_RANGE_FULL)
    };

    ctx->muxer = mov_muxer_create(&mux_config);
    if (!ctx->muxer) {
        prores_encoder_destroy(ctx->encoder);
        free(ctx);
        return NULL;
    }

    /* Allocate YUV conversion buffer */
    int is_444 = (profile >= 4);
    if (is_444) {
        /* 4:4:4 or 4:4:4:4 */
        ctx->yuv_buffer_size = width * height * (profile >= 4 ? 4 : 3) * sizeof(uint16_t);
    } else {
        /* 4:2:2 */
        size_t chroma_width = (size_t)(width + 1) / 2;
        size_t y_size = (size_t)width * height;
        size_t c_size = chroma_width * height;
        ctx->yuv_buffer_size = (y_size + c_size * 2) * sizeof(uint16_t);
    }
    ctx->yuv_buffer = (uint16_t*)malloc(ctx->yuv_buffer_size);
    if (!ctx->yuv_buffer) {
        mov_muxer_destroy(ctx->muxer);
        prores_encoder_destroy(ctx->encoder);
        free(ctx);
        return NULL;
    }

    return ctx;
}

/*
 * Add an RGBA frame (8-bit per channel)
 * Automatically converts to appropriate YUV format
 *
 * @param ctx_ptr    Context from prores_wasm_create
 * @param rgba_ptr   Pointer to RGBA data (width * height * 4 bytes)
 * @return           0 on success, negative on error
 */
EMSCRIPTEN_KEEPALIVE
int prores_wasm_add_frame_rgba(void* ctx_ptr, const uint8_t* rgba_ptr)
{
    ProResWasmContext* ctx = (ProResWasmContext*)ctx_ptr;
    if (!ctx || !rgba_ptr) return -1;

    /* Convert RGBA to YUV based on profile */
    int bit_depth = (ctx->profile >= 4) ? 12 : 10;
    if (ctx->profile >= 4) {
        /* 4:4:4 profiles */
        if (ctx->profile == 4 || ctx->profile == 5) {
            rgba_to_yuva444p10(rgba_ptr, ctx->yuv_buffer, ctx->width, ctx->height, bit_depth, ctx->range);
        } else {
            rgba_to_yuv444p10(rgba_ptr, ctx->yuv_buffer, ctx->width, ctx->height, bit_depth, ctx->range);
        }
    } else {
        /* 4:2:2 profiles */
        rgba_to_yuv422p10(rgba_ptr, ctx->yuv_buffer, ctx->width, ctx->height, bit_depth, ctx->range);
    }

    /* Encode frame */
    uint8_t* frame_data = NULL;
    int frame_size = 0;

    int ret = prores_encoder_encode_frame(ctx->encoder, ctx->yuv_buffer, &frame_data, &frame_size);
    if (ret < 0) {
        return ret;
    }

    /* Write to muxer */
    ret = mov_muxer_write_frame(ctx->muxer, frame_data, frame_size);
    free(frame_data);

    return ret;
}

/*
 * Add a YUV frame directly (10-bit planar)
 *
 * @param ctx_ptr    Context from prores_wasm_create
 * @param yuv_ptr    Pointer to YUV data (format depends on profile)
 * @return           0 on success, negative on error
 */
EMSCRIPTEN_KEEPALIVE
int prores_wasm_add_frame_yuv(void* ctx_ptr, const uint16_t* yuv_ptr)
{
    ProResWasmContext* ctx = (ProResWasmContext*)ctx_ptr;
    if (!ctx || !yuv_ptr) return -1;

    /* Encode frame directly */
    uint8_t* frame_data = NULL;
    int frame_size = 0;

    int ret = prores_encoder_encode_frame(ctx->encoder, yuv_ptr, &frame_data, &frame_size);
    if (ret < 0) {
        return ret;
    }

    /* Write to muxer */
    ret = mov_muxer_write_frame(ctx->muxer, frame_data, frame_size);
    free(frame_data);

    return ret;
}

/*
 * Finalize encoding and get MOV data
 *
 * @param ctx_ptr       Context from prores_wasm_create
 * @param out_size_ptr  Pointer to receive output size
 * @return              Pointer to MOV data (caller must call prores_wasm_free_buffer)
 */
EMSCRIPTEN_KEEPALIVE
uint8_t* prores_wasm_finalize(void* ctx_ptr, size_t* out_size_ptr)
{
    ProResWasmContext* ctx = (ProResWasmContext*)ctx_ptr;
    if (!ctx || !out_size_ptr) return NULL;

    uint8_t* mov_data = NULL;
    size_t mov_size = 0;

    int ret = mov_muxer_finalize(ctx->muxer, &mov_data, &mov_size);
    if (ret < 0) {
        *out_size_ptr = 0;
        return NULL;
    }

    *out_size_ptr = mov_size;
    return mov_data;
}

/*
 * Free a buffer allocated by the encoder
 */
EMSCRIPTEN_KEEPALIVE
void prores_wasm_free_buffer(void* ptr)
{
    free(ptr);
}

/*
 * Destroy encoder and free all resources
 */
EMSCRIPTEN_KEEPALIVE
void prores_wasm_destroy(void* ctx_ptr)
{
    ProResWasmContext* ctx = (ProResWasmContext*)ctx_ptr;
    if (!ctx) return;

    prores_encoder_destroy(ctx->encoder);
    mov_muxer_destroy(ctx->muxer);
    free(ctx->yuv_buffer);
    free(ctx);
}

/*
 * Get required RGBA buffer size for a frame
 */
EMSCRIPTEN_KEEPALIVE
size_t prores_wasm_get_rgba_buffer_size(int width, int height)
{
    return (size_t)width * height * 4;
}

/*
 * Get required YUV buffer size for a frame
 */
EMSCRIPTEN_KEEPALIVE
size_t prores_wasm_get_yuv_buffer_size(int width, int height, int profile)
{
    if (profile >= 4) {
        /* 4:4:4:4 with alpha */
        return (size_t)width * height * 4 * sizeof(uint16_t);
    } else {
        /* 4:2:2 */
        size_t chroma_width = (size_t)(width + 1) / 2;
        size_t y_size = (size_t)width * height;
        size_t c_size = chroma_width * height;
        return (y_size + c_size * 2) * sizeof(uint16_t);
    }
}

/*
 * Allocate a buffer in WASM memory
 */
EMSCRIPTEN_KEEPALIVE
void* prores_wasm_alloc(size_t size)
{
    return malloc(size);
}

/*
 * Get profile FourCC string
 */
EMSCRIPTEN_KEEPALIVE
uint32_t prores_wasm_get_fourcc(int profile)
{
    static const uint32_t fourccs[6] = {
        0x6170636F,  /* 'apco' - Proxy */
        0x61706373,  /* 'apcs' - LT */
        0x6170636E,  /* 'apcn' - Standard */
        0x61706368,  /* 'apch' - HQ */
        0x61703468,  /* 'ap4h' - 4444 */
        0x61703478,  /* 'ap4x' - 4444 XQ */
    };
    if (profile < 0 || profile > 5) return 0;
    return fourccs[profile];
}
