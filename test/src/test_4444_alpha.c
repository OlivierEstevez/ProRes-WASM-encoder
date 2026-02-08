/*
 * Test for ProRes 4444 alpha encoding
 * Compile: gcc -o test/binaries/test_4444_alpha test/src/test_4444_alpha.c \
 *          src/encoder/*.c src/muxer/*.c -I src -lm
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
#define NUM_FRAMES 30

/* Generate a test frame with varying alpha */
static void generate_test_frame(uint8_t* rgba, int width, int height, int frame, int total_frames)
{
    float t = (float)frame / total_frames;

    for (int y = 0; y < height; y++) {
        for (int x = 0; x < width; x++) {
            int idx = (y * width + x) * 4;

            float fx = (float)x / width;
            float fy = (float)y / height;

            float r = 0.5f + 0.5f * sinf(fx * 6.28f + t * 6.28f);
            float g = 0.5f + 0.5f * sinf(fy * 6.28f + t * 6.28f * 2);
            float b = 0.5f + 0.5f * sinf((fx + fy) * 3.14f + t * 6.28f * 3);

            /* Alpha: gradient from left (transparent) to right (opaque) */
            float a = fx;

            rgba[idx + 0] = (uint8_t)(r * 255);
            rgba[idx + 1] = (uint8_t)(g * 255);
            rgba[idx + 2] = (uint8_t)(b * 255);
            rgba[idx + 3] = (uint8_t)(a * 255);
        }
    }
}

int main(int argc, char* argv[])
{
    printf("ProRes 4444 Alpha Encoding Test\n");
    printf("================================\n\n");

    const char* output_file = "test/outputs/test_4444_alpha.mov";
    if (argc > 1) {
        output_file = argv[1];
    }

    printf("Creating encoder: %dx%d @ %d fps, ProRes 4444\n", WIDTH, HEIGHT, FPS);

    ProResEncoderConfig enc_config = {
        .width = WIDTH,
        .height = HEIGHT,
        .fps_num = FPS,
        .fps_den = 1,
        .profile = PRORES_PROFILE_4444,
        .colorspace = PRORES_CS_BT709,
        .frame_type = PRORES_FRAME_PROGRESSIVE,
        .quality = 100,
        .range = PRORES_RANGE_FULL
    };

    ProResEncoderContext* encoder = prores_encoder_create(&enc_config);
    if (!encoder) {
        fprintf(stderr, "Failed to create encoder\n");
        return 1;
    }

    MovMuxerConfig mux_config = {
        .width = WIDTH,
        .height = HEIGHT,
        .fps_num = FPS,
        .fps_den = 1,
        .fourcc = prores_encoder_get_fourcc(encoder),
        .bit_depth = 12,
        .has_alpha = 1,
        .full_range = 1,
        .color = { .primaries = 1, .transfer = 1, .matrix = 1 }
    };

    MovMuxerContext* muxer = mov_muxer_create(&mux_config);
    if (!muxer) {
        fprintf(stderr, "Failed to create muxer\n");
        prores_encoder_destroy(encoder);
        return 1;
    }

    /* YUVA: 4 planes, each width*height uint16_t */
    uint8_t* rgba_buffer = (uint8_t*)malloc(WIDTH * HEIGHT * 4);
    uint16_t* yuva_buffer = (uint16_t*)malloc(WIDTH * HEIGHT * 4 * sizeof(uint16_t));

    if (!rgba_buffer || !yuva_buffer) {
        fprintf(stderr, "Failed to allocate buffers\n");
        mov_muxer_destroy(muxer);
        prores_encoder_destroy(encoder);
        return 1;
    }

    printf("Encoding %d frames...\n", NUM_FRAMES);

    for (int frame = 0; frame < NUM_FRAMES; frame++) {
        generate_test_frame(rgba_buffer, WIDTH, HEIGHT, frame, NUM_FRAMES);

        /* Convert to YUVA444P10 */
        rgba_to_yuva444p10(rgba_buffer, yuva_buffer, WIDTH, HEIGHT, PRORES_RANGE_FULL);

        uint8_t* frame_data = NULL;
        int frame_size = 0;

        int ret = prores_encoder_encode_frame(encoder, yuva_buffer, &frame_data, &frame_size);
        if (ret < 0) {
            fprintf(stderr, "Failed to encode frame %d\n", frame);
            break;
        }

        ret = mov_muxer_write_frame(muxer, frame_data, frame_size);
        free(frame_data);

        if (ret < 0) {
            fprintf(stderr, "Failed to mux frame %d\n", frame);
            break;
        }

        if ((frame + 1) % 10 == 0 || frame == NUM_FRAMES - 1) {
            printf("  Encoded frame %d/%d\n", frame + 1, NUM_FRAMES);
        }
    }

    printf("Finalizing MOV file...\n");

    uint8_t* mov_data = NULL;
    size_t mov_size = 0;

    int ret = mov_muxer_finalize(muxer, &mov_data, &mov_size);
    if (ret < 0) {
        fprintf(stderr, "Failed to finalize MOV\n");
    } else {
        FILE* fp = fopen(output_file, "wb");
        if (fp) {
            fwrite(mov_data, 1, mov_size, fp);
            fclose(fp);
            printf("Written: %s (%zu bytes, %.2f MB)\n", output_file, mov_size, mov_size / (1024.0 * 1024.0));
        } else {
            fprintf(stderr, "Failed to write output file\n");
        }
        free(mov_data);
    }

    free(rgba_buffer);
    free(yuva_buffer);
    mov_muxer_destroy(muxer);
    prores_encoder_destroy(encoder);

    printf("\nDone!\n");
    return 0;
}
