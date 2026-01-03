/* Test VLC encoding against expected bit patterns */
#include <stdio.h>
#include <string.h>
#include "encoder/prores_vlc.h"

void print_bits(uint8_t* buf, int bits) {
    for (int i = 0; i < bits; i++) {
        int byte_idx = i / 8;
        int bit_idx = 7 - (i % 8);
        printf("%d", (buf[byte_idx] >> bit_idx) & 1);
    }
    printf(" (%d bits)\n", bits);
}

int main() {
    uint8_t buf[64];
    PutBitContext pb;
    
    printf("Testing VLC encoding\n");
    printf("====================\n\n");
    
    /* Test DC encoding with codebook 0x04 (FIRST_DC_CB) */
    printf("DC encoding (codebook -1 = FIRST_DC_CB = 0x04):\n");
    for (int val = -3; val <= 3; val++) {
        memset(buf, 0, 64);
        init_put_bits(&pb, buf, 64);
        prores_encode_dc(&pb, -1, val);
        flush_put_bits(&pb);
        printf("  val=%2d: ", val);
        print_bits(buf, put_bits_count(&pb));
    }
    
    printf("\nRun encoding (run_to_cb[4] = 0x04):\n");
    for (int run = 0; run <= 5; run++) {
        memset(buf, 0, 64);
        init_put_bits(&pb, buf, 64);
        prores_encode_run(&pb, 4, run);
        flush_put_bits(&pb);
        printf("  run=%d: ", run);
        print_bits(buf, put_bits_count(&pb));
    }
    
    printf("\nLevel encoding (lev_to_cb[2] = 0x05):\n");
    for (int lev = 1; lev <= 5; lev++) {
        memset(buf, 0, 64);
        init_put_bits(&pb, buf, 64);
        prores_encode_level(&pb, 2, lev);
        flush_put_bits(&pb);
        printf("  level=%d: ", lev);
        print_bits(buf, put_bits_count(&pb));
    }
    
    return 0;
}
