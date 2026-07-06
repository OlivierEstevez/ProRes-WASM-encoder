/*
 * Debug test: encode 1 frame of a complex PNG and report slice buffer usage.
 * Usage: test_debug_slice <png_path>
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"

#include "encoder/prores_encoder.h"
#include "muxer/mov_muxer.h"

int main(int argc, char* argv[])
{
    if (argc < 2) {
        fprintf(stderr, "Usage: %s <png_path>\n", argv[0]);
        return 1;
    }

    int w, h, channels;
    uint8_t* rgba = stbi_load(argv[1], &w, &h, &channels, 4);
    if (!rgba) {
        fprintf(stderr, "Failed to load %s: %s\n", argv[1], stbi_failure_reason());
        return 1;
    }
    printf("Loaded: %dx%d (%d channels)\n", w, h, channels);

    /* Test each profile */
    const char* profile_names[] = {"Proxy", "LT", "Standard", "HQ", "4444", "4444XQ"};
    for (int p = 0; p <= 5; p++) {
        printf("\n=== Profile %d (%s) ===\n", p, profile_names[p]);

        ProResEncoderConfig enc_config = {
            .width = w, .height = h,
            .fps_num = 30, .fps_den = 1,
            .profile = (ProResProfile)p,
            .colorspace = PRORES_CS_BT709,
            .frame_type = PRORES_FRAME_PROGRESSIVE,
            .range = PRORES_RANGE_LIMITED
        };

        ProResEncoderContext* encoder = prores_encoder_create(&enc_config);
        if (!encoder) {
            printf("  Failed to create encoder\n");
            continue;
        }

        int is_444 = (p >= 4);
        size_t yuv_size;
        if (is_444) {
            yuv_size = (size_t)w * h * 4 * sizeof(uint16_t);
        } else {
            size_t cw = ((size_t)w + 1) / 2;
            yuv_size = ((size_t)w * h + cw * h * 2) * sizeof(uint16_t);
        }
        uint16_t* yuv = (uint16_t*)malloc(yuv_size);

        if (is_444) {
            rgba_to_yuva444p10(rgba, yuv, w, h, PRORES_RANGE_LIMITED);
        } else {
            rgba_to_yuv422p10(rgba, yuv, w, h, PRORES_RANGE_LIMITED);
        }

        uint8_t* frame_data = NULL;
        int frame_size = 0;
        int ret = prores_encoder_encode_frame(encoder, yuv, &frame_data, &frame_size);
        printf("  encode_frame returned: %d, frame_size: %d (%.2f KB)\n",
               ret, frame_size, frame_size / 1024.0);

        if (ret == 0 && frame_data) {
            /* Try muxing too */
            MovMuxerConfig mux_config = {
                .width = w, .height = h,
                .fps_num = 30, .fps_den = 1,
                .fourcc = prores_encoder_get_fourcc(encoder),
                .bit_depth = is_444 ? 12 : 10,
                .has_alpha = is_444 ? 1 : 0,
                .full_range = 0,
                .color = { .primaries = 1, .transfer = 1, .matrix = 1 }
            };
            MovMuxerContext* muxer = mov_muxer_create(&mux_config);
            if (muxer) {
                int mux_ret = mov_muxer_write_frame(muxer, frame_data, frame_size);
                printf("  mux_write_frame returned: %d\n", mux_ret);
                mov_muxer_destroy(muxer);
            }
            /* frame_data is owned by the encoder, no free */
        }

        free(yuv);
        prores_encoder_destroy(encoder);
    }

    stbi_image_free(rgba);
    return 0;
}
