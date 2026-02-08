/*
 * ProRes Encoder for WebAssembly
 * Based on Apple ProRes specification and FFmpeg's proresenc_kostya.c
 */

#ifndef PRORES_ENCODER_H
#define PRORES_ENCODER_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ProRes Profile Constants */
typedef enum {
    PRORES_PROFILE_PROXY   = 0,  /* 'apco' - ProRes 422 Proxy */
    PRORES_PROFILE_LT      = 1,  /* 'apcs' - ProRes 422 LT */
    PRORES_PROFILE_STANDARD= 2,  /* 'apcn' - ProRes 422 Standard */
    PRORES_PROFILE_HQ      = 3,  /* 'apch' - ProRes 422 HQ */
    PRORES_PROFILE_4444    = 4,  /* 'ap4h' - ProRes 4444 */
    PRORES_PROFILE_4444XQ  = 5,  /* 'ap4x' - ProRes 4444 XQ */
} ProResProfile;

/* Color Space */
typedef enum {
    PRORES_CS_BT709  = 0,
    PRORES_CS_BT2020 = 1,
} ProResColorSpace;

/* Color Range */
typedef enum {
    PRORES_RANGE_LIMITED = 0,
    PRORES_RANGE_FULL = 1,
} ProResColorRange;

/* Pixel Format */
typedef enum {
    PRORES_PIX_FMT_YUV422P10 = 0,  /* 4:2:2 10-bit planar */
    PRORES_PIX_FMT_YUV444P10 = 1,  /* 4:4:4 10-bit planar */
    PRORES_PIX_FMT_YUVA444P10= 2,  /* 4:4:4:4 10-bit planar with alpha */
} ProResPixelFormat;

/* Frame Type */
typedef enum {
    PRORES_FRAME_PROGRESSIVE = 0,
    PRORES_FRAME_INTERLACED_TFF = 1,  /* Top field first */
    PRORES_FRAME_INTERLACED_BFF = 2,  /* Bottom field first */
} ProResFrameType;

/* Encoder Configuration */
typedef struct {
    int width;
    int height;
    int fps_num;           /* Frame rate numerator */
    int fps_den;           /* Frame rate denominator */
    ProResProfile profile;
    ProResColorSpace colorspace;
    ProResFrameType frame_type;
    int quality;           /* 0-100, higher is better */
    ProResColorRange range;
} ProResEncoderConfig;

/* Opaque encoder context */
typedef struct ProResEncoderContext ProResEncoderContext;

/*
 * Create a new ProRes encoder instance
 *
 * @param config  Encoder configuration
 * @return        Encoder context, or NULL on failure
 */
ProResEncoderContext* prores_encoder_create(const ProResEncoderConfig* config);

/*
 * Encode a single frame
 *
 * @param ctx       Encoder context
 * @param yuv_data  Input YUV data (planar format matching pixel format)
 *                  For YUV422P10: Y plane (w*h*2), U plane (w/2*h*2), V plane (w/2*h*2)
 *                  For YUV444P10: Y plane (w*h*2), U plane (w*h*2), V plane (w*h*2)
 *                  For YUVA444P10: + A plane (w*h*2)
 * @param out_data  Output buffer pointer (allocated by encoder, caller must free)
 * @param out_size  Output size in bytes
 * @return          0 on success, negative on error
 */
int prores_encoder_encode_frame(
    ProResEncoderContext* ctx,
    const uint16_t* yuv_data,
    uint8_t** out_data,
    int* out_size
);

/*
 * Get the FourCC code for the configured profile
 *
 * @param ctx  Encoder context
 * @return     FourCC as uint32_t (e.g., 'apch' for HQ)
 */
uint32_t prores_encoder_get_fourcc(const ProResEncoderContext* ctx);

/*
 * Get the pixel format for the configured profile
 *
 * @param ctx  Encoder context
 * @return     Pixel format enum
 */
ProResPixelFormat prores_encoder_get_pixel_format(const ProResEncoderContext* ctx);

/*
 * Destroy encoder and free resources
 *
 * @param ctx  Encoder context
 */
void prores_encoder_destroy(ProResEncoderContext* ctx);

/*
 * Convert RGBA (8-bit) to YUV422P10 (10-bit) for 4:2:2 profiles
 * Uses 12-bit fixed-point BT.709 coefficients for direct 10-bit output
 *
 * @param rgba    Input RGBA data (width * height * 4 bytes)
 * @param yuv     Output YUV422P10 data (must be pre-allocated)
 * @param width   Frame width
 * @param height  Frame height
 * @param range   Color range (full or limited/video)
 */
void rgba_to_yuv422p10(
    const uint8_t* rgba,
    uint16_t* yuv,
    int width,
    int height,
    ProResColorRange range
);

/*
 * Convert RGBA (8-bit) to YUV444P10 (10-bit) for 4:4:4 profiles
 * Uses 12-bit fixed-point BT.709 coefficients for direct 10-bit output
 *
 * @param rgba    Input RGBA data (width * height * 4 bytes)
 * @param yuv     Output YUV444P10 data (must be pre-allocated)
 * @param width   Frame width
 * @param height  Frame height
 * @param range   Color range (full or limited/video)
 */
void rgba_to_yuv444p10(
    const uint8_t* rgba,
    uint16_t* yuv,
    int width,
    int height,
    ProResColorRange range
);

/*
 * Convert RGBA (8-bit) to YUVA444P10 (10-bit) with alpha for 4444 profiles
 * Uses 12-bit fixed-point BT.709 coefficients for direct 10-bit output
 *
 * @param rgba    Input RGBA data (width * height * 4 bytes)
 * @param yuva    Output YUVA444P10 data (must be pre-allocated)
 * @param width   Frame width
 * @param height  Frame height
 * @param range   Color range (full or limited/video)
 */
void rgba_to_yuva444p10(
    const uint8_t* rgba,
    uint16_t* yuva,
    int width,
    int height,
    ProResColorRange range
);

#ifdef __cplusplus
}
#endif

#endif /* PRORES_ENCODER_H */
