/*
 * MOV Muxer Implementation for ProRes
 * Based on QuickTime File Format specification
 */

#include "mov_muxer.h"
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* Helper macros */
#define WRITE_BE32(p, v) do { \
    (p)[0] = ((v) >> 24) & 0xFF; \
    (p)[1] = ((v) >> 16) & 0xFF; \
    (p)[2] = ((v) >> 8) & 0xFF;  \
    (p)[3] = (v) & 0xFF;         \
} while(0)

#define WRITE_BE16(p, v) do { \
    (p)[0] = ((v) >> 8) & 0xFF;  \
    (p)[1] = (v) & 0xFF;         \
} while(0)

#define WRITE_BE64(p, v) do { \
    WRITE_BE32(p, (uint32_t)((v) >> 32)); \
    WRITE_BE32((p) + 4, (uint32_t)(v));   \
} while(0)

/* FourCC macro */
#define FOURCC(a,b,c,d) (((uint32_t)(a)<<24)|((uint32_t)(b)<<16)|((uint32_t)(c)<<8)|(d))

/* MOV atom types */
#define MOV_FTYP  FOURCC('f','t','y','p')
#define MOV_QT    FOURCC('q','t',' ',' ')
#define MOV_MDAT  FOURCC('m','d','a','t')
#define MOV_MOOV  FOURCC('m','o','o','v')
#define MOV_MVHD  FOURCC('m','v','h','d')
#define MOV_TRAK  FOURCC('t','r','a','k')
#define MOV_TKHD  FOURCC('t','k','h','d')
#define MOV_MDIA  FOURCC('m','d','i','a')
#define MOV_MDHD  FOURCC('m','d','h','d')
#define MOV_HDLR  FOURCC('h','d','l','r')
#define MOV_MINF  FOURCC('m','i','n','f')
#define MOV_VMHD  FOURCC('v','m','h','d')
#define MOV_DINF  FOURCC('d','i','n','f')
#define MOV_DREF  FOURCC('d','r','e','f')
#define MOV_URL   FOURCC('u','r','l',' ')
#define MOV_STBL  FOURCC('s','t','b','l')
#define MOV_STSD  FOURCC('s','t','s','d')
#define MOV_STTS  FOURCC('s','t','t','s')
#define MOV_STSS  FOURCC('s','t','s','s')
#define MOV_STSC  FOURCC('s','t','s','c')
#define MOV_STSZ  FOURCC('s','t','s','z')
#define MOV_STCO  FOURCC('s','t','c','o')
#define MOV_CO64  FOURCC('c','o','6','4')
#define MOV_COLR  FOURCC('c','o','l','r')
#define MOV_VIDE  FOURCC('v','i','d','e')
#define MOV_NCLC  FOURCC('n','c','l','c')
#define MOV_NCLX  FOURCC('n','c','l','x')

/* Sample entry */
typedef struct {
    uint32_t size;
    uint64_t offset;
} SampleEntry;

/* Muxer context structure */
struct MovMuxerContext {
    MovMuxerConfig config;

    /* Sample tracking */
    SampleEntry* samples;
    int num_samples;
    int max_samples;

    /* Media data buffer */
    uint8_t* mdat_buf;
    size_t mdat_size;
    size_t mdat_capacity;

    /* Timing */
    uint32_t timescale;
    uint64_t duration;
};

/* Dynamic buffer writing */
static int ensure_mdat_capacity(MovMuxerContext* ctx, size_t needed)
{
    if (ctx->mdat_size + needed > ctx->mdat_capacity) {
        size_t new_capacity = ctx->mdat_capacity * 2;
        if (new_capacity < ctx->mdat_size + needed) {
            new_capacity = ctx->mdat_size + needed + 1024 * 1024;
        }
        uint8_t* new_buf = (uint8_t*)realloc(ctx->mdat_buf, new_capacity);
        if (!new_buf) return -1;
        ctx->mdat_buf = new_buf;
        ctx->mdat_capacity = new_capacity;
    }
    return 0;
}

MovMuxerContext* mov_muxer_create(const MovMuxerConfig* config)
{
    if (!config || config->width <= 0 || config->height <= 0) {
        return NULL;
    }

    MovMuxerContext* ctx = (MovMuxerContext*)calloc(1, sizeof(MovMuxerContext));
    if (!ctx) return NULL;

    ctx->config = *config;

    /* Calculate timescale from frame rate */
    ctx->timescale = config->fps_num;

    /* Allocate sample array */
    ctx->max_samples = 1024;
    ctx->samples = (SampleEntry*)malloc(ctx->max_samples * sizeof(SampleEntry));
    if (!ctx->samples) {
        free(ctx);
        return NULL;
    }

    /* Initial mdat buffer (will grow as needed) */
    ctx->mdat_capacity = 1024 * 1024;  /* 1MB initial */
    ctx->mdat_buf = (uint8_t*)malloc(ctx->mdat_capacity);
    if (!ctx->mdat_buf) {
        free(ctx->samples);
        free(ctx);
        return NULL;
    }

    return ctx;
}

int mov_muxer_write_frame(MovMuxerContext* ctx, const uint8_t* frame_data, int frame_size)
{
    if (!ctx || !frame_data || frame_size <= 0) {
        return -1;
    }

    /* Expand sample array if needed */
    if (ctx->num_samples >= ctx->max_samples) {
        int new_max = ctx->max_samples * 2;
        SampleEntry* new_samples = (SampleEntry*)realloc(ctx->samples, new_max * sizeof(SampleEntry));
        if (!new_samples) return -1;
        ctx->samples = new_samples;
        ctx->max_samples = new_max;
    }

    /* Ensure mdat buffer capacity */
    if (ensure_mdat_capacity(ctx, frame_size) < 0) {
        return -1;
    }

    /* Record sample info */
    ctx->samples[ctx->num_samples].size = frame_size;
    ctx->samples[ctx->num_samples].offset = ctx->mdat_size;
    ctx->num_samples++;

    /* Copy frame data */
    memcpy(ctx->mdat_buf + ctx->mdat_size, frame_data, frame_size);
    ctx->mdat_size += frame_size;

    /* Update duration */
    ctx->duration += ctx->config.fps_den;

    return 0;
}

/* Write a generic box header */
static uint8_t* write_box_header(uint8_t* p, uint32_t type, uint32_t size)
{
    WRITE_BE32(p, size);
    WRITE_BE32(p + 4, type);
    return p + 8;
}

/* Write ftyp box */
static uint8_t* write_ftyp(uint8_t* p)
{
    uint8_t* start = p;
    p += 8;  /* Skip size/type for now */

    /* Major brand: qt */
    WRITE_BE32(p, MOV_QT); p += 4;

    /* Minor version */
    WRITE_BE32(p, 0x00000200); p += 4;

    /* Compatible brands */
    WRITE_BE32(p, MOV_QT); p += 4;

    /* Write header */
    write_box_header(start, MOV_FTYP, p - start);

    return p;
}

/* Write mvhd (movie header) box */
static uint8_t* write_mvhd(uint8_t* p, MovMuxerContext* ctx)
{
    uint8_t* start = p;
    p += 8;  /* Skip size/type */

    /* Version and flags */
    WRITE_BE32(p, 0); p += 4;

    /* Creation time (seconds since 1904) */
    time_t now = time(NULL);
    uint32_t creation_time = (uint32_t)(now + 2082844800UL);  /* Mac epoch offset */
    WRITE_BE32(p, creation_time); p += 4;

    /* Modification time */
    WRITE_BE32(p, creation_time); p += 4;

    /* Timescale */
    WRITE_BE32(p, ctx->timescale); p += 4;

    /* Duration */
    WRITE_BE32(p, (uint32_t)ctx->duration); p += 4;

    /* Preferred rate (1.0 = 0x00010000) */
    WRITE_BE32(p, 0x00010000); p += 4;

    /* Preferred volume (1.0 = 0x0100) */
    WRITE_BE16(p, 0x0100); p += 2;

    /* Reserved */
    memset(p, 0, 10); p += 10;

    /* Matrix (identity) */
    WRITE_BE32(p, 0x00010000); p += 4;  /* a */
    WRITE_BE32(p, 0x00000000); p += 4;  /* b */
    WRITE_BE32(p, 0x00000000); p += 4;  /* u */
    WRITE_BE32(p, 0x00000000); p += 4;  /* c */
    WRITE_BE32(p, 0x00010000); p += 4;  /* d */
    WRITE_BE32(p, 0x00000000); p += 4;  /* v */
    WRITE_BE32(p, 0x00000000); p += 4;  /* x */
    WRITE_BE32(p, 0x00000000); p += 4;  /* y */
    WRITE_BE32(p, 0x40000000); p += 4;  /* w */

    /* Preview time, duration, poster time, selection time/duration, current time */
    memset(p, 0, 24); p += 24;

    /* Next track ID */
    WRITE_BE32(p, 2); p += 4;

    write_box_header(start, MOV_MVHD, p - start);
    return p;
}

/* Write tkhd (track header) box */
static uint8_t* write_tkhd(uint8_t* p, MovMuxerContext* ctx)
{
    uint8_t* start = p;
    p += 8;

    /* Version and flags (track enabled, in movie, in preview) */
    WRITE_BE32(p, 0x00000003); p += 4;

    /* Creation/modification time */
    time_t now = time(NULL);
    uint32_t creation_time = (uint32_t)(now + 2082844800UL);
    WRITE_BE32(p, creation_time); p += 4;
    WRITE_BE32(p, creation_time); p += 4;

    /* Track ID */
    WRITE_BE32(p, 1); p += 4;

    /* Reserved */
    WRITE_BE32(p, 0); p += 4;

    /* Duration */
    WRITE_BE32(p, (uint32_t)ctx->duration); p += 4;

    /* Reserved */
    WRITE_BE32(p, 0); p += 4;
    WRITE_BE32(p, 0); p += 4;

    /* Layer */
    WRITE_BE16(p, 0); p += 2;

    /* Alternate group */
    WRITE_BE16(p, 0); p += 2;

    /* Volume (0 for video) */
    WRITE_BE16(p, 0); p += 2;

    /* Reserved */
    WRITE_BE16(p, 0); p += 2;

    /* Matrix (identity) */
    WRITE_BE32(p, 0x00010000); p += 4;
    WRITE_BE32(p, 0x00000000); p += 4;
    WRITE_BE32(p, 0x00000000); p += 4;
    WRITE_BE32(p, 0x00000000); p += 4;
    WRITE_BE32(p, 0x00010000); p += 4;
    WRITE_BE32(p, 0x00000000); p += 4;
    WRITE_BE32(p, 0x00000000); p += 4;
    WRITE_BE32(p, 0x00000000); p += 4;
    WRITE_BE32(p, 0x40000000); p += 4;

    /* Width (16.16 fixed point) */
    WRITE_BE32(p, ctx->config.width << 16); p += 4;

    /* Height (16.16 fixed point) */
    WRITE_BE32(p, ctx->config.height << 16); p += 4;

    write_box_header(start, MOV_TKHD, p - start);
    return p;
}

/* Write mdhd (media header) box */
static uint8_t* write_mdhd(uint8_t* p, MovMuxerContext* ctx)
{
    uint8_t* start = p;
    p += 8;

    /* Version and flags */
    WRITE_BE32(p, 0); p += 4;

    /* Creation/modification time */
    time_t now = time(NULL);
    uint32_t creation_time = (uint32_t)(now + 2082844800UL);
    WRITE_BE32(p, creation_time); p += 4;
    WRITE_BE32(p, creation_time); p += 4;

    /* Timescale */
    WRITE_BE32(p, ctx->timescale); p += 4;

    /* Duration */
    WRITE_BE32(p, (uint32_t)ctx->duration); p += 4;

    /* Language (undetermined) */
    WRITE_BE16(p, 0x55C4); p += 2;

    /* Quality */
    WRITE_BE16(p, 0); p += 2;

    write_box_header(start, MOV_MDHD, p - start);
    return p;
}

/* Write hdlr (handler) box */
static uint8_t* write_hdlr(uint8_t* p)
{
    uint8_t* start = p;
    p += 8;

    /* Version and flags */
    WRITE_BE32(p, 0); p += 4;

    /* Component type */
    WRITE_BE32(p, 0); p += 4;

    /* Component subtype (vide) */
    WRITE_BE32(p, MOV_VIDE); p += 4;

    /* Component manufacturer */
    WRITE_BE32(p, 0); p += 4;

    /* Component flags */
    WRITE_BE32(p, 0); p += 4;

    /* Component flags mask */
    WRITE_BE32(p, 0); p += 4;

    /* Component name (Pascal string) */
    *p++ = 12;  /* Length */
    memcpy(p, "VideoHandler", 12);
    p += 12;

    write_box_header(start, MOV_HDLR, p - start);
    return p;
}

/* Write vmhd (video media header) box */
static uint8_t* write_vmhd(uint8_t* p)
{
    uint8_t* start = p;
    p += 8;

    /* Version and flags */
    WRITE_BE32(p, 0x00000001); p += 4;

    /* Graphics mode */
    WRITE_BE16(p, 0); p += 2;

    /* Opcolor (RGB) */
    WRITE_BE16(p, 0); p += 2;
    WRITE_BE16(p, 0); p += 2;
    WRITE_BE16(p, 0); p += 2;

    write_box_header(start, MOV_VMHD, p - start);
    return p;
}

/* Write dinf/dref boxes */
static uint8_t* write_dinf(uint8_t* p)
{
    uint8_t* dinf_start = p;
    p += 8;

    /* dref box */
    uint8_t* dref_start = p;
    p += 8;

    /* Version and flags */
    WRITE_BE32(p, 0); p += 4;

    /* Entry count */
    WRITE_BE32(p, 1); p += 4;

    /* url entry (self-contained) */
    uint8_t* url_start = p;
    p += 8;
    WRITE_BE32(p, 0x00000001); p += 4;  /* Flags: self-contained */
    write_box_header(url_start, MOV_URL, p - url_start);

    write_box_header(dref_start, MOV_DREF, p - dref_start);
    write_box_header(dinf_start, MOV_DINF, p - dinf_start);

    return p;
}

/* Write stsd (sample description) box with ProRes codec */
static uint8_t* write_stsd(uint8_t* p, MovMuxerContext* ctx)
{
    uint8_t* start = p;
    p += 8;

    /* Version and flags */
    WRITE_BE32(p, 0); p += 4;

    /* Entry count */
    WRITE_BE32(p, 1); p += 4;

    /* Video sample entry */
    uint8_t* entry_start = p;
    p += 8;  /* Size and type (fourcc) filled later */

    /* Reserved */
    memset(p, 0, 6); p += 6;

    /* Data reference index */
    WRITE_BE16(p, 1); p += 2;

    /* Version */
    WRITE_BE16(p, 0); p += 2;

    /* Revision level */
    WRITE_BE16(p, 0); p += 2;

    /* Vendor */
    WRITE_BE32(p, FOURCC('a','p','p','l')); p += 4;

    /* Temporal quality */
    WRITE_BE32(p, 0); p += 4;

    /* Spatial quality - value affects alpha_info parsing in some decoders
     * Use 512 (0x200) to match FFmpeg's proresenc output */
    WRITE_BE32(p, 512); p += 4;

    /* Width */
    WRITE_BE16(p, ctx->config.width); p += 2;

    /* Height */
    WRITE_BE16(p, ctx->config.height); p += 2;

    /* Horizontal resolution (72 dpi = 0x00480000) */
    WRITE_BE32(p, 0x00480000); p += 4;

    /* Vertical resolution */
    WRITE_BE32(p, 0x00480000); p += 4;

    /* Data size */
    WRITE_BE32(p, 0); p += 4;

    /* Frame count */
    WRITE_BE16(p, 1); p += 2;

    /* Compressor name (32 bytes, Pascal string) */
    const char* compressor = "Apple ProRes 422";
    uint8_t compressor_len = 17;
    if (ctx->config.fourcc == FOURCC('a','p','4','h') ||
        ctx->config.fourcc == FOURCC('a','p','4','x')) {
        compressor = "Apple ProRes 4444";
        compressor_len = 17;
    }
    memset(p, 0, 32);
    p[0] = compressor_len;
    memcpy(p + 1, compressor, compressor_len);
    p += 32;

    /* Depth */
    WRITE_BE16(p, 24); p += 2;

    /* Color table ID */
    WRITE_BE16(p, 0xFFFF); p += 2;

    /* fiel atom - field/interlace information (required by some decoders) */
    uint8_t* fiel_start = p;
    p += 8;
    *p++ = 1;  /* fields: 1 = progressive, 2 = interlaced */
    *p++ = 0;  /* detail: 0 = unknown, 1 = TFF, 2 = BFF */
    write_box_header(fiel_start, FOURCC('f','i','e','l'), p - fiel_start);

    /* colr atom */
    uint8_t* colr_start = p;
    p += 8;
    WRITE_BE32(p, ctx->config.full_range ? MOV_NCLX : MOV_NCLC); p += 4;  /* Color parameter type */
    WRITE_BE16(p, ctx->config.color.primaries); p += 2;
    WRITE_BE16(p, ctx->config.color.transfer); p += 2;
    WRITE_BE16(p, ctx->config.color.matrix); p += 2;
    if (ctx->config.full_range) {
        *p++ = 0x80;  /* Full range flag */
    }
    write_box_header(colr_start, MOV_COLR, p - colr_start);

    /* Write entry header with ProRes FourCC */
    WRITE_BE32(entry_start, p - entry_start);
    WRITE_BE32(entry_start + 4, ctx->config.fourcc);

    write_box_header(start, MOV_STSD, p - start);
    return p;
}

/* Write stts (time-to-sample) box */
static uint8_t* write_stts(uint8_t* p, MovMuxerContext* ctx)
{
    uint8_t* start = p;
    p += 8;

    /* Version and flags */
    WRITE_BE32(p, 0); p += 4;

    /* Entry count (all frames have same duration) */
    WRITE_BE32(p, 1); p += 4;

    /* Sample count */
    WRITE_BE32(p, ctx->num_samples); p += 4;

    /* Sample duration */
    WRITE_BE32(p, ctx->config.fps_den); p += 4;

    write_box_header(start, MOV_STTS, p - start);
    return p;
}

/* Write stss (sync sample) box - all frames are keyframes for ProRes */
static uint8_t* write_stss(uint8_t* p, MovMuxerContext* ctx)
{
    uint8_t* start = p;
    p += 8;

    /* Version and flags */
    WRITE_BE32(p, 0); p += 4;

    /* Entry count */
    WRITE_BE32(p, ctx->num_samples); p += 4;

    /* All samples are sync samples (ProRes is intra-only) */
    for (int i = 0; i < ctx->num_samples; i++) {
        WRITE_BE32(p, i + 1); p += 4;
    }

    write_box_header(start, MOV_STSS, p - start);
    return p;
}

/* Write stsc (sample-to-chunk) box */
static uint8_t* write_stsc(uint8_t* p, MovMuxerContext* ctx)
{
    uint8_t* start = p;
    p += 8;

    /* Version and flags */
    WRITE_BE32(p, 0); p += 4;

    /* Entry count (one chunk per sample for simplicity) */
    WRITE_BE32(p, 1); p += 4;

    /* First chunk */
    WRITE_BE32(p, 1); p += 4;

    /* Samples per chunk */
    WRITE_BE32(p, 1); p += 4;

    /* Sample description index */
    WRITE_BE32(p, 1); p += 4;

    write_box_header(start, MOV_STSC, p - start);
    return p;
}

/* Write stsz (sample size) box */
static uint8_t* write_stsz(uint8_t* p, MovMuxerContext* ctx)
{
    uint8_t* start = p;
    p += 8;

    /* Version and flags */
    WRITE_BE32(p, 0); p += 4;

    /* Sample size (0 = variable) */
    WRITE_BE32(p, 0); p += 4;

    /* Sample count */
    WRITE_BE32(p, ctx->num_samples); p += 4;

    /* Individual sample sizes */
    for (int i = 0; i < ctx->num_samples; i++) {
        WRITE_BE32(p, ctx->samples[i].size); p += 4;
    }

    write_box_header(start, MOV_STSZ, p - start);
    return p;
}

/* Write stco (chunk offset) box */
static uint8_t* write_stco(uint8_t* p, MovMuxerContext* ctx, uint64_t mdat_offset)
{
    uint8_t* start = p;
    p += 8;

    /* Version and flags */
    WRITE_BE32(p, 0); p += 4;

    /* Entry count */
    WRITE_BE32(p, ctx->num_samples); p += 4;

    /* Chunk offsets (relative to file start) */
    for (int i = 0; i < ctx->num_samples; i++) {
        uint32_t offset = (uint32_t)(mdat_offset + 8 + ctx->samples[i].offset);
        WRITE_BE32(p, offset); p += 4;
    }

    write_box_header(start, MOV_STCO, p - start);
    return p;
}

/* Write stbl (sample table) box */
static uint8_t* write_stbl(uint8_t* p, MovMuxerContext* ctx, uint64_t mdat_offset)
{
    uint8_t* start = p;
    p += 8;

    p = write_stsd(p, ctx);
    p = write_stts(p, ctx);
    p = write_stss(p, ctx);
    p = write_stsc(p, ctx);
    p = write_stsz(p, ctx);
    p = write_stco(p, ctx, mdat_offset);

    write_box_header(start, MOV_STBL, p - start);
    return p;
}

/* Write minf (media information) box */
static uint8_t* write_minf(uint8_t* p, MovMuxerContext* ctx, uint64_t mdat_offset)
{
    uint8_t* start = p;
    p += 8;

    p = write_vmhd(p);
    p = write_dinf(p);
    p = write_stbl(p, ctx, mdat_offset);

    write_box_header(start, MOV_MINF, p - start);
    return p;
}

/* Write mdia (media) box */
static uint8_t* write_mdia(uint8_t* p, MovMuxerContext* ctx, uint64_t mdat_offset)
{
    uint8_t* start = p;
    p += 8;

    p = write_mdhd(p, ctx);
    p = write_hdlr(p);
    p = write_minf(p, ctx, mdat_offset);

    write_box_header(start, MOV_MDIA, p - start);
    return p;
}

/* Write trak (track) box */
static uint8_t* write_trak(uint8_t* p, MovMuxerContext* ctx, uint64_t mdat_offset)
{
    uint8_t* start = p;
    p += 8;

    p = write_tkhd(p, ctx);
    p = write_mdia(p, ctx, mdat_offset);

    write_box_header(start, MOV_TRAK, p - start);
    return p;
}

/* Write moov (movie) box */
static uint8_t* write_moov(uint8_t* p, MovMuxerContext* ctx, uint64_t mdat_offset)
{
    uint8_t* start = p;
    p += 8;

    p = write_mvhd(p, ctx);
    p = write_trak(p, ctx, mdat_offset);

    write_box_header(start, MOV_MOOV, p - start);
    return p;
}

int mov_muxer_finalize(MovMuxerContext* ctx, uint8_t** out_data, size_t* out_size)
{
    if (!ctx || !out_data || !out_size) {
        return -1;
    }

    if (ctx->num_samples == 0) {
        return -1;
    }

    /* Calculate sizes */
    size_t ftyp_size = 20;  /* ftyp box */
    size_t mdat_size = 8 + ctx->mdat_size;  /* mdat header + data */

    /* Estimate moov size (will be exact after first pass) */
    size_t moov_estimate = 1024 + ctx->num_samples * 16;

    /* Allocate output buffer */
    size_t total_size = ftyp_size + mdat_size + moov_estimate;
    uint8_t* buf = (uint8_t*)malloc(total_size);
    if (!buf) return -1;

    uint8_t* p = buf;

    /* Write ftyp */
    p = write_ftyp(p);

    /* Write mdat header */
    uint64_t mdat_offset = p - buf;
    WRITE_BE32(p, 8 + ctx->mdat_size);
    WRITE_BE32(p + 4, MOV_MDAT);
    p += 8;

    /* Copy mdat content */
    memcpy(p, ctx->mdat_buf, ctx->mdat_size);
    p += ctx->mdat_size;

    /* Write moov */
    p = write_moov(p, ctx, mdat_offset);

    /* Final size */
    *out_size = p - buf;
    *out_data = (uint8_t*)realloc(buf, *out_size);
    if (!*out_data) {
        *out_data = buf;  /* Keep original if realloc fails */
    }

    return 0;
}

void mov_muxer_destroy(MovMuxerContext* ctx)
{
    if (!ctx) return;

    free(ctx->samples);
    free(ctx->mdat_buf);
    free(ctx);
}
