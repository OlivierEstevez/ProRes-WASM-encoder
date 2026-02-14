/*
 * Test all 422 profile quantization matrices
 * Encodes a single frame with each profile and writes MOV files
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "encoder/prores_encoder.h"
#include "muxer/mov_muxer.h"

#define WIDTH 320
#define HEIGHT 240

static void generate_test_frame(uint8_t* rgba, int width, int height)
{
    for (int y = 0; y < height; y++) {
        for (int x = 0; x < width; x++) {
            int idx = (y * width + x) * 4;
            float fx = (float)x / width;
            float fy = (float)y / height;
            rgba[idx + 0] = (uint8_t)(fx * 255);
            rgba[idx + 1] = (uint8_t)(fy * 255);
            rgba[idx + 2] = (uint8_t)((1.0f - fx) * 255);
            rgba[idx + 3] = 255;
        }
    }
}

static int test_profile(ProResProfile profile, const char* name, const char* output_path)
{
    printf("Testing %s profile...\n", name);

    ProResEncoderConfig enc_config = {
        .width = WIDTH,
        .height = HEIGHT,
        .fps_num = 30,
        .fps_den = 1,
        .profile = profile,
        .colorspace = PRORES_CS_BT709,
        .frame_type = PRORES_FRAME_PROGRESSIVE,
        .quality = 85,
        .range = PRORES_RANGE_FULL
    };

    ProResEncoderContext* encoder = prores_encoder_create(&enc_config);
    if (!encoder) {
        fprintf(stderr, "  FAIL: Could not create encoder\n");
        return 1;
    }

    MovMuxerConfig mux_config = {
        .width = WIDTH,
        .height = HEIGHT,
        .fps_num = 30,
        .fps_den = 1,
        .fourcc = prores_encoder_get_fourcc(encoder),
        .bit_depth = 10,
        .has_alpha = 0,
        .full_range = 1,
        .color = { .primaries = 1, .transfer = 1, .matrix = 1 }
    };

    MovMuxerContext* muxer = mov_muxer_create(&mux_config);
    if (!muxer) {
        fprintf(stderr, "  FAIL: Could not create muxer\n");
        prores_encoder_destroy(encoder);
        return 1;
    }

    uint8_t* rgba = (uint8_t*)malloc(WIDTH * HEIGHT * 4);
    uint16_t* yuv = (uint16_t*)malloc(WIDTH * HEIGHT * 2 * sizeof(uint16_t));

    generate_test_frame(rgba, WIDTH, HEIGHT);
    rgba_to_yuv422p10(rgba, yuv, WIDTH, HEIGHT, PRORES_RANGE_FULL);

    uint8_t* frame_data = NULL;
    int frame_size = 0;
    int ret = prores_encoder_encode_frame(encoder, yuv, &frame_data, &frame_size);
    if (ret < 0) {
        fprintf(stderr, "  FAIL: Encode failed\n");
        free(rgba); free(yuv);
        mov_muxer_destroy(muxer);
        prores_encoder_destroy(encoder);
        return 1;
    }

    mov_muxer_write_frame(muxer, frame_data, frame_size);
    free(frame_data);

    uint8_t* mov_data = NULL;
    size_t mov_size = 0;
    ret = mov_muxer_finalize(muxer, &mov_data, &mov_size);
    if (ret < 0) {
        fprintf(stderr, "  FAIL: Finalize failed\n");
        free(rgba); free(yuv);
        mov_muxer_destroy(muxer);
        prores_encoder_destroy(encoder);
        return 1;
    }

    FILE* fp = fopen(output_path, "wb");
    if (fp) {
        fwrite(mov_data, 1, mov_size, fp);
        fclose(fp);
        printf("  OK: Written %s (%zu bytes)\n", output_path, mov_size);
    }

    free(mov_data);
    free(rgba);
    free(yuv);
    mov_muxer_destroy(muxer);
    prores_encoder_destroy(encoder);
    return 0;
}

int main(void)
{
    printf("ProRes Profile Quantization Matrix Test\n");
    printf("=======================================\n\n");

    int failures = 0;
    failures += test_profile(PRORES_PROFILE_PROXY,    "Proxy",    "test/outputs/test_proxy.mov");
    failures += test_profile(PRORES_PROFILE_LT,       "LT",       "test/outputs/test_lt.mov");
    failures += test_profile(PRORES_PROFILE_STANDARD,  "Standard", "test/outputs/test_standard.mov");
    failures += test_profile(PRORES_PROFILE_HQ,        "HQ",       "test/outputs/test_hq.mov");

    printf("\n%s: %d/%d profiles passed\n",
           failures ? "FAIL" : "ALL PASS", 4 - failures, 4);
    return failures;
}
