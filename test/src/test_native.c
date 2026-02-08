/*
 * Native test for ProRes encoder (without WASM)
 * Compile with: gcc -o test_native test_native.c ../src/encoder/*.c ../src/muxer/*.c -I../src -lm
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "encoder/prores_encoder.h"
#include "muxer/mov_muxer.h"

#define WIDTH 640
#define HEIGHT 480
#define FPS 30
#define NUM_FRAMES 90

/* Generate a test frame with animated gradient */
static void generate_test_frame(uint8_t* rgba, int width, int height, int frame, int total_frames)
{
    float t = (float)frame / total_frames;

    for (int y = 0; y < height; y++) {
        for (int x = 0; x < width; x++) {
            int idx = (y * width + x) * 4;

            /* Animated gradient */
            float fx = (float)x / width;
            float fy = (float)y / height;

            /* RGB values based on position and time */
            float r = 0.5f + 0.5f * sinf(fx * 6.28f + t * 6.28f);
            float g = 0.5f + 0.5f * sinf(fy * 6.28f + t * 6.28f * 2);
            float b = 0.5f + 0.5f * sinf((fx + fy) * 3.14f + t * 6.28f * 3);

            rgba[idx + 0] = (uint8_t)(r * 255);
            rgba[idx + 1] = (uint8_t)(g * 255);
            rgba[idx + 2] = (uint8_t)(b * 255);
            rgba[idx + 3] = 255;  /* Alpha */
        }
    }
}

int main(int argc, char* argv[])
{
    printf("ProRes Encoder Native Test\n");
    printf("==========================\n\n");

    const char* output_file = "test_output.mov";
    if (argc > 1) {
        output_file = argv[1];
    }

    /* Create encoder */
    printf("Creating encoder: %dx%d @ %d fps, ProRes HQ\n", WIDTH, HEIGHT, FPS);

    ProResEncoderConfig enc_config = {
        .width = WIDTH,
        .height = HEIGHT,
        .fps_num = FPS,
        .fps_den = 1,
        .profile = PRORES_PROFILE_HQ,
        .colorspace = PRORES_CS_BT709,
        .frame_type = PRORES_FRAME_PROGRESSIVE,
        .quality = 85,
        .range = PRORES_RANGE_FULL
    };

    ProResEncoderContext* encoder = prores_encoder_create(&enc_config);
    if (!encoder) {
        fprintf(stderr, "Failed to create encoder\n");
        return 1;
    }

    /* Create muxer */
    MovMuxerConfig mux_config = {
        .width = WIDTH,
        .height = HEIGHT,
        .fps_num = FPS,
        .fps_den = 1,
        .fourcc = prores_encoder_get_fourcc(encoder),
        .bit_depth = 10,
        .has_alpha = 0,
        .full_range = 1,
        .color = { .primaries = 1, .transfer = 1, .matrix = 1 }
    };

    MovMuxerContext* muxer = mov_muxer_create(&mux_config);
    if (!muxer) {
        fprintf(stderr, "Failed to create muxer\n");
        prores_encoder_destroy(encoder);
        return 1;
    }

    /* Allocate buffers */
    uint8_t* rgba_buffer = (uint8_t*)malloc(WIDTH * HEIGHT * 4);
    uint16_t* yuv_buffer = (uint16_t*)malloc(WIDTH * HEIGHT * 2 * sizeof(uint16_t));

    if (!rgba_buffer || !yuv_buffer) {
        fprintf(stderr, "Failed to allocate buffers\n");
        mov_muxer_destroy(muxer);
        prores_encoder_destroy(encoder);
        return 1;
    }

    /* Encode frames */
    printf("Encoding %d frames...\n", NUM_FRAMES);

    for (int frame = 0; frame < NUM_FRAMES; frame++) {
        /* Generate test frame */
        generate_test_frame(rgba_buffer, WIDTH, HEIGHT, frame, NUM_FRAMES);

        /* Convert to YUV */
        rgba_to_yuv422p10(rgba_buffer, yuv_buffer, WIDTH, HEIGHT, PRORES_RANGE_FULL);

        /* Encode frame */
        uint8_t* frame_data = NULL;
        int frame_size = 0;

        int ret = prores_encoder_encode_frame(encoder, yuv_buffer, &frame_data, &frame_size);
        if (ret < 0) {
            fprintf(stderr, "Failed to encode frame %d\n", frame);
            break;
        }

        /* Write to muxer */
        ret = mov_muxer_write_frame(muxer, frame_data, frame_size);
        free(frame_data);

        if (ret < 0) {
            fprintf(stderr, "Failed to mux frame %d\n", frame);
            break;
        }

        /* Progress */
        if ((frame + 1) % 10 == 0 || frame == NUM_FRAMES - 1) {
            printf("  Encoded frame %d/%d\n", frame + 1, NUM_FRAMES);
        }
    }

    /* Finalize */
    printf("Finalizing MOV file...\n");

    uint8_t* mov_data = NULL;
    size_t mov_size = 0;

    int ret = mov_muxer_finalize(muxer, &mov_data, &mov_size);
    if (ret < 0) {
        fprintf(stderr, "Failed to finalize MOV\n");
    } else {
        /* Write to file */
        FILE* fp = fopen(output_file, "wb");
        if (fp) {
            fwrite(mov_data, 1, mov_size, fp);
            fclose(fp);
            printf("Written: %s (%zu bytes)\n", output_file, mov_size);
        } else {
            fprintf(stderr, "Failed to write output file\n");
        }
        free(mov_data);
    }

    /* Cleanup */
    free(rgba_buffer);
    free(yuv_buffer);
    mov_muxer_destroy(muxer);
    prores_encoder_destroy(encoder);

    printf("\nDone!\n");
    return 0;
}
