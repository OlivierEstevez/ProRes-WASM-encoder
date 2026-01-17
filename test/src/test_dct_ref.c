/*
 * Reference DCT Test
 * Compares our DCT with a reference floating-point implementation
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "encoder/prores_dct.h"

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

/* Reference floating-point DCT-II */
static void reference_fdct_8x8(double* block)
{
    double temp[64];
    double cu, cv;

    for (int u = 0; u < 8; u++) {
        for (int v = 0; v < 8; v++) {
            double sum = 0.0;
            for (int x = 0; x < 8; x++) {
                for (int y = 0; y < 8; y++) {
                    sum += block[y * 8 + x] *
                           cos((2*x + 1) * u * M_PI / 16.0) *
                           cos((2*y + 1) * v * M_PI / 16.0);
                }
            }
            cu = (u == 0) ? 1.0/sqrt(2.0) : 1.0;
            cv = (v == 0) ? 1.0/sqrt(2.0) : 1.0;
            temp[v * 8 + u] = 0.25 * cu * cv * sum;
        }
    }

    memcpy(block, temp, 64 * sizeof(double));
}

/* Print comparison */
static void compare_dct(const char* name, int16_t* input)
{
    double ref_block[64];
    int16_t our_block[64];

    /* Copy input to both */
    for (int i = 0; i < 64; i++) {
        ref_block[i] = (double)input[i];
        our_block[i] = input[i];
    }

    /* Run both DCTs */
    reference_fdct_8x8(ref_block);
    prores_fdct_8x8(our_block);

    printf("\n=== %s ===\n", name);
    printf("Position  Reference    Ours     Diff\n");
    printf("----------------------------------------\n");

    int significant_diffs = 0;
    for (int y = 0; y < 8; y++) {
        for (int x = 0; x < 8; x++) {
            int idx = y * 8 + x;
            double ref = ref_block[idx];
            int16_t ours = our_block[idx];
            double diff = fabs(ref - (double)ours);

            /* Only print significant coefficients */
            if (fabs(ref) > 1.0 || abs(ours) > 1) {
                printf("[%d,%d]    %8.1f  %8d  %8.1f", y, x, ref, ours, diff);
                if (diff > 10.0 && (fabs(ref) > 10.0 || abs(ours) > 10)) {
                    printf(" <-- SIGNIFICANT DIFF");
                    significant_diffs++;
                }
                printf("\n");
            }
        }
    }

    printf("\nSignificant differences: %d\n", significant_diffs);
}

int main(void)
{
    printf("DCT Reference Comparison\n");
    printf("========================\n");

    /* Test 1: Checkerboard */
    {
        int16_t block[64];
        for (int y = 0; y < 8; y++) {
            for (int x = 0; x < 8; x++) {
                block[y * 8 + x] = ((x + y) & 1) ? 100 : -100;
            }
        }
        compare_dct("Checkerboard (±100)", block);
    }

    /* Test 2: Vertical edge */
    {
        int16_t block[64];
        for (int y = 0; y < 8; y++) {
            for (int x = 0; x < 8; x++) {
                block[y * 8 + x] = (x < 4) ? -200 : 200;
            }
        }
        compare_dct("Vertical Edge", block);
    }

    /* Test 3: Horizontal gradient */
    {
        int16_t block[64];
        for (int y = 0; y < 8; y++) {
            for (int x = 0; x < 8; x++) {
                block[y * 8 + x] = x * 10 - 35;
            }
        }
        compare_dct("Horizontal Gradient", block);
    }

    /* Test 4: Diagonal edge */
    {
        int16_t block[64];
        for (int y = 0; y < 8; y++) {
            for (int x = 0; x < 8; x++) {
                block[y * 8 + x] = (x + y < 8) ? -150 : 150;
            }
        }
        compare_dct("Diagonal Edge", block);
    }

    return 0;
}
