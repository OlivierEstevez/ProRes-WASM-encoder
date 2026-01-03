/*
 * MOV Muxer for ProRes
 * Minimal QuickTime/MOV container implementation
 */

#ifndef MOV_MUXER_H
#define MOV_MUXER_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Maximum number of samples (frames) */
#define MOV_MAX_SAMPLES 100000

/* MOV Muxer context (opaque) */
typedef struct MovMuxerContext MovMuxerContext;

/* Color information for colr atom */
typedef struct {
    uint16_t primaries;      /* Color primaries (1=BT.709, 9=BT.2020) */
    uint16_t transfer;       /* Transfer characteristics */
    uint16_t matrix;         /* Matrix coefficients */
} MovColorInfo;

/* Configuration for MOV muxer */
typedef struct {
    int width;
    int height;
    int fps_num;            /* Frame rate numerator */
    int fps_den;            /* Frame rate denominator */
    uint32_t fourcc;        /* ProRes FourCC (e.g., 'apch') */
    int bit_depth;          /* 10 or 12 */
    int has_alpha;          /* 1 if alpha channel present */
    MovColorInfo color;     /* Color metadata */
} MovMuxerConfig;

/*
 * Create a new MOV muxer instance
 *
 * @param config  Muxer configuration
 * @return        Muxer context, or NULL on failure
 */
MovMuxerContext* mov_muxer_create(const MovMuxerConfig* config);

/*
 * Write a single encoded frame to the MOV
 *
 * @param ctx         Muxer context
 * @param frame_data  Encoded ProRes frame data
 * @param frame_size  Size of frame data in bytes
 * @return            0 on success, negative on error
 */
int mov_muxer_write_frame(MovMuxerContext* ctx, const uint8_t* frame_data, int frame_size);

/*
 * Finalize the MOV file and get output data
 *
 * @param ctx       Muxer context
 * @param out_data  Output buffer pointer (allocated by muxer, caller must free)
 * @param out_size  Output size in bytes
 * @return          0 on success, negative on error
 */
int mov_muxer_finalize(MovMuxerContext* ctx, uint8_t** out_data, size_t* out_size);

/*
 * Destroy muxer and free resources
 *
 * @param ctx  Muxer context
 */
void mov_muxer_destroy(MovMuxerContext* ctx);

#ifdef __cplusplus
}
#endif

#endif /* MOV_MUXER_H */
