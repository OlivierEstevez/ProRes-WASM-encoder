/*
 * PNG Sequence Encoder Test
 *
 * Loads a PNG image sequence via stb_image and encodes it through
 * all applicable ProRes profiles, writing individual MOV files.
 *
 * Usage:
 *   test_png_sequences <input_dir> <output_dir> <width> <height> \
 *                      <fps_num> <fps_den> <num_frames> <has_alpha> <png_pattern>
 *
 * Build:
 *   gcc -O2 -o test/binaries/test_png_sequences test/src/test_png_sequences.c \
 *       src/encoder/*.c src/muxer/*.c -I src -I third_party -lm
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"

#include "encoder/prores_encoder.h"
#include "muxer/mov_muxer.h"

/* Profile metadata */
typedef struct {
    ProResProfile profile;
    const char*   name;
    const char*   filename;
    int           is_444;
} ProfileInfo;

static const ProfileInfo ALL_PROFILES[] = {
    { PRORES_PROFILE_PROXY,    "Proxy",    "proxy.mov",    0 },
    { PRORES_PROFILE_LT,       "LT",       "lt.mov",       0 },
    { PRORES_PROFILE_STANDARD, "Standard", "standard.mov", 0 },
    { PRORES_PROFILE_HQ,       "HQ",       "hq.mov",       0 },
    { PRORES_PROFILE_4444,     "4444",     "4444.mov",     1 },
    { PRORES_PROFILE_4444XQ,   "4444XQ",   "4444xq.mov",  1 },
};
#define NUM_ALL_PROFILES 6

/* Alpha-only profiles (for sequences with alpha) */
static const int ALPHA_PROFILE_INDICES[] = { 4, 5 };  /* 4444, 4444XQ */
#define NUM_ALPHA_PROFILES 2

static double get_time_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000.0 + ts.tv_nsec / 1000000.0;
}

/*
 * Encode a full PNG sequence with one profile.
 * Returns 0 on success, -1 on error.
 * Writes timing_ms and file_size_bytes to output pointers.
 */
static int encode_sequence(
    const char* input_dir,
    const char* output_path,
    const ProfileInfo* prof,
    int width, int height,
    int fps_num, int fps_den,
    int num_frames,
    int has_alpha,
    const char* png_pattern,
    double* timing_ms,
    size_t* file_size_bytes)
{
    *timing_ms = 0;
    *file_size_bytes = 0;

    /* Create encoder */
    ProResEncoderConfig enc_config = {
        .width = width,
        .height = height,
        .fps_num = fps_num,
        .fps_den = fps_den,
        .profile = prof->profile,
        .colorspace = PRORES_CS_BT709,
        .frame_type = PRORES_FRAME_PROGRESSIVE,
        .range = PRORES_RANGE_LIMITED
    };

    ProResEncoderContext* encoder = prores_encoder_create(&enc_config);
    if (!encoder) {
        fprintf(stderr, "  ERROR: Failed to create encoder for %s\n", prof->name);
        return -1;
    }

    /* Create muxer */
    int mux_bit_depth = prof->is_444 ? 12 : 10;
    int mux_has_alpha = (prof->is_444 && has_alpha) ? 1 : 0;

    MovMuxerConfig mux_config = {
        .width = width,
        .height = height,
        .fps_num = fps_num,
        .fps_den = fps_den,
        .fourcc = prores_encoder_get_fourcc(encoder),
        .bit_depth = mux_bit_depth,
        .has_alpha = mux_has_alpha,
        .full_range = 0,
        .color = { .primaries = 1, .transfer = 1, .matrix = 1 }
    };

    MovMuxerContext* muxer = mov_muxer_create(&mux_config);
    if (!muxer) {
        fprintf(stderr, "  ERROR: Failed to create muxer for %s\n", prof->name);
        prores_encoder_destroy(encoder);
        return -1;
    }

    /* Allocate YUV buffer */
    size_t yuv_size;
    if (prof->is_444) {
        /* YUVA: 4 planes of w*h uint16_t */
        yuv_size = (size_t)width * height * 4 * sizeof(uint16_t);
    } else {
        /* YUV422: Y + Cb + Cr */
        size_t chroma_width = ((size_t)width + 1) / 2;
        yuv_size = ((size_t)width * height + chroma_width * height * 2) * sizeof(uint16_t);
    }

    uint16_t* yuv_buffer = (uint16_t*)malloc(yuv_size);
    if (!yuv_buffer) {
        fprintf(stderr, "  ERROR: Failed to allocate YUV buffer (%zu bytes)\n", yuv_size);
        mov_muxer_destroy(muxer);
        prores_encoder_destroy(encoder);
        return -1;
    }

    /* Encode frames */
    double t_start = get_time_ms();
    int success = 1;

    for (int frame = 0; frame < num_frames; frame++) {
        /* Build PNG path */
        char frame_name[512];
        snprintf(frame_name, sizeof(frame_name), png_pattern, frame);

        char png_path[1024];
        snprintf(png_path, sizeof(png_path), "%s/%s", input_dir, frame_name);

        /* Load PNG */
        int img_w, img_h, img_channels;
        uint8_t* rgba = stbi_load(png_path, &img_w, &img_h, &img_channels, 4);
        if (!rgba) {
            fprintf(stderr, "  ERROR: Failed to load %s: %s\n", png_path, stbi_failure_reason());
            success = 0;
            break;
        }

        if (img_w != width || img_h != height) {
            fprintf(stderr, "  ERROR: Frame %d size mismatch: expected %dx%d, got %dx%d\n",
                    frame, width, height, img_w, img_h);
            stbi_image_free(rgba);
            success = 0;
            break;
        }

        /* Convert to YUV */
        if (prof->is_444) {
            if (has_alpha) {
                rgba_to_yuva444p10(rgba, yuv_buffer, width, height, PRORES_RANGE_LIMITED);
            } else {
                rgba_to_yuva444p10(rgba, yuv_buffer, width, height, PRORES_RANGE_LIMITED);
            }
        } else {
            rgba_to_yuv422p10(rgba, yuv_buffer, width, height, PRORES_RANGE_LIMITED);
        }

        stbi_image_free(rgba);

        /* Encode */
        uint8_t* frame_data = NULL;
        int frame_size = 0;
        int ret = prores_encoder_encode_frame(encoder, yuv_buffer, &frame_data, &frame_size);
        if (ret < 0) {
            fprintf(stderr, "  ERROR: Failed to encode frame %d\n", frame);
            success = 0;
            break;
        }

        /* Mux */
        ret = mov_muxer_write_frame(muxer, frame_data, frame_size);
        free(frame_data);
        if (ret < 0) {
            fprintf(stderr, "  ERROR: Failed to mux frame %d\n", frame);
            success = 0;
            break;
        }

        /* Progress every 30 frames */
        if ((frame + 1) % 30 == 0 || frame == num_frames - 1) {
            printf("    [%s] Frame %d/%d\n", prof->name, frame + 1, num_frames);
        }
    }

    double t_encode = get_time_ms() - t_start;

    if (!success) {
        free(yuv_buffer);
        mov_muxer_destroy(muxer);
        prores_encoder_destroy(encoder);
        return -1;
    }

    /* Finalize */
    uint8_t* mov_data = NULL;
    size_t mov_size = 0;
    int ret = mov_muxer_finalize(muxer, &mov_data, &mov_size);
    if (ret < 0) {
        fprintf(stderr, "  ERROR: Failed to finalize MOV for %s\n", prof->name);
        free(yuv_buffer);
        mov_muxer_destroy(muxer);
        prores_encoder_destroy(encoder);
        return -1;
    }

    /* Write MOV file */
    FILE* fp = fopen(output_path, "wb");
    if (!fp) {
        fprintf(stderr, "  ERROR: Failed to open %s for writing\n", output_path);
        free(mov_data);
        free(yuv_buffer);
        mov_muxer_destroy(muxer);
        prores_encoder_destroy(encoder);
        return -1;
    }
    fwrite(mov_data, 1, mov_size, fp);
    fclose(fp);

    *timing_ms = t_encode;
    *file_size_bytes = mov_size;

    printf("    [%s] Done: %s (%.2f MB, %.0f ms)\n",
           prof->name, output_path,
           mov_size / (1024.0 * 1024.0), t_encode);

    free(mov_data);
    free(yuv_buffer);
    mov_muxer_destroy(muxer);
    prores_encoder_destroy(encoder);
    return 0;
}

static void print_usage(const char* progname)
{
    fprintf(stderr,
        "Usage: %s <input_dir> <output_dir> <width> <height> "
        "<fps_num> <fps_den> <num_frames> <has_alpha> <png_pattern>\n\n"
        "Example:\n"
        "  %s test/reference/TEST-01 test/suite-results/TEST-01/native \\\n"
        "     1080 1080 30 1 150 0 TEST_01_BASIC_30fps_%%04d.png\n",
        progname, progname);
}

int main(int argc, char* argv[])
{
    if (argc != 10) {
        print_usage(argv[0]);
        return 1;
    }

    const char* input_dir   = argv[1];
    const char* output_dir  = argv[2];
    int width               = atoi(argv[3]);
    int height              = atoi(argv[4]);
    int fps_num             = atoi(argv[5]);
    int fps_den             = atoi(argv[6]);
    int num_frames          = atoi(argv[7]);
    int has_alpha           = atoi(argv[8]);
    const char* png_pattern = argv[9];

    printf("PNG Sequence Encoder Test\n");
    printf("=========================\n");
    printf("  Input:      %s/%s\n", input_dir, png_pattern);
    printf("  Output:     %s/\n", output_dir);
    printf("  Resolution: %dx%d\n", width, height);
    printf("  FPS:        %d/%d\n", fps_num, fps_den);
    printf("  Frames:     %d\n", num_frames);
    printf("  Alpha:      %s\n", has_alpha ? "yes" : "no");
    printf("\n");

    /* Determine which profiles to encode */
    int profile_count;
    const int* profile_indices;
    int all_indices[] = { 0, 1, 2, 3, 4, 5 };

    if (has_alpha) {
        /* Alpha sequences: only 4444 profiles */
        profile_indices = ALPHA_PROFILE_INDICES;
        profile_count = NUM_ALPHA_PROFILES;
    } else {
        /* Non-alpha: all profiles */
        profile_indices = all_indices;
        profile_count = NUM_ALL_PROFILES;
    }

    int failures = 0;

    /* Collect timing data for JSON */
    double timings[NUM_ALL_PROFILES];
    size_t sizes[NUM_ALL_PROFILES];
    memset(timings, 0, sizeof(timings));
    memset(sizes, 0, sizeof(sizes));

    for (int i = 0; i < profile_count; i++) {
        const ProfileInfo* prof = &ALL_PROFILES[profile_indices[i]];

        char output_path[1024];
        snprintf(output_path, sizeof(output_path), "%s/%s", output_dir, prof->filename);

        printf("  Encoding %s profile...\n", prof->name);

        double timing_ms = 0;
        size_t file_size = 0;
        int ret = encode_sequence(
            input_dir, output_path, prof,
            width, height, fps_num, fps_den,
            num_frames, has_alpha, png_pattern,
            &timing_ms, &file_size);

        if (ret < 0) {
            fprintf(stderr, "  FAILED: %s\n", prof->name);
            failures++;
        }

        timings[i] = timing_ms;
        sizes[i] = file_size;

        printf("\n");
    }

    /* Write JSON timing data to stderr */
    fprintf(stderr, "{\n  \"profiles\": [\n");
    for (int i = 0; i < profile_count; i++) {
        const ProfileInfo* prof = &ALL_PROFILES[profile_indices[i]];
        fprintf(stderr, "%s    {\"profile\":\"%s\",\"file\":\"%s\",\"size_bytes\":%zu,\"encode_ms\":%.1f}",
                i > 0 ? ",\n" : "",
                prof->name, prof->filename, sizes[i], timings[i]);
    }
    fprintf(stderr, "\n  ]\n}\n");

    printf("=========================\n");
    printf("Results: %d/%d profiles encoded successfully\n",
           profile_count - failures, profile_count);

    return failures > 0 ? 1 : 0;
}
