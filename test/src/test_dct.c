/*
 * DCT Diagnostic Test
 * Tests DCT output for simple patterns to verify correctness
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "encoder/prores_dct.h"

/* Print 8x8 block */
static void print_block(const char* name, int16_t* block)
{
    printf("%s:\n", name);
    for (int y = 0; y < 8; y++) {
        for (int x = 0; x < 8; x++) {
            printf("%6d ", block[y * 8 + x]);
        }
        printf("\n");
    }
    printf("\n");
}

/* Test 1: Solid color block - should have only DC, all AC = 0 */
static void test_solid_block(void)
{
    int16_t block[64];
    int16_t centered[64];
    int sample_center = 512;  /* 10-bit center */

    printf("=== Test 1: Solid Color Block ===\n");

    /* Fill with constant value 512 (center) - after centering should be all 0 */
    for (int i = 0; i < 64; i++) {
        block[i] = 512;
        centered[i] = block[i] - sample_center;  /* Should be 0 */
    }

    print_block("Input (centered)", centered);

    prores_fdct_8x8(centered);

    print_block("DCT Output (expected: DC=0, all AC=0)", centered);

    /* Check: all should be 0 */
    int errors = 0;
    for (int i = 0; i < 64; i++) {
        if (centered[i] != 0) errors++;
    }
    printf("Result: %s (%d non-zero coefficients)\n\n", errors == 0 ? "PASS" : "FAIL", errors);
}

/* Test 2: Block with offset - should have only DC */
static void test_offset_block(void)
{
    int16_t block[64];
    int sample_center = 512;

    printf("=== Test 2: Offset Block (all 600) ===\n");

    /* Fill with constant value 600 - after centering should be all 88 */
    for (int i = 0; i < 64; i++) {
        block[i] = 600 - sample_center;  /* = 88 */
    }

    print_block("Input (centered, all 88)", block);

    prores_fdct_8x8(block);

    print_block("DCT Output (expected: DC=88*8=704, all AC=0)", block);

    /* DC should be input_value * 8 due to DCT scaling */
    /* AC should all be 0 */
    int ac_errors = 0;
    for (int i = 1; i < 64; i++) {
        if (block[i] != 0) ac_errors++;
    }
    printf("DC coefficient: %d (expected ~88 after scaling)\n", block[0]);
    printf("AC errors: %d non-zero AC coefficients\n", ac_errors);
    printf("Result: %s\n\n", ac_errors == 0 ? "PASS" : "FAIL");
}

/* Test 3: Horizontal gradient - should have DC and horizontal frequency components */
static void test_horizontal_gradient(void)
{
    int16_t block[64];
    int sample_center = 512;

    printf("=== Test 3: Horizontal Gradient ===\n");

    /* Horizontal gradient: 0 to 70 across each row */
    for (int y = 0; y < 8; y++) {
        for (int x = 0; x < 8; x++) {
            block[y * 8 + x] = x * 10 - 35;  /* -35 to +35, centered */
        }
    }

    print_block("Input (horizontal gradient)", block);

    prores_fdct_8x8(block);

    print_block("DCT Output", block);

    /* Check: vertical frequencies (column 0 except DC) should be 0 */
    printf("Vertical frequency components (should be ~0):\n");
    for (int y = 1; y < 8; y++) {
        printf("  [%d,0] = %d\n", y, block[y * 8]);
    }
    printf("\n");
}

/* Test 4: Checkerboard - high frequency pattern */
static void test_checkerboard(void)
{
    int16_t block[64];

    printf("=== Test 4: Checkerboard Pattern ===\n");

    /* Checkerboard: +100 and -100 */
    for (int y = 0; y < 8; y++) {
        for (int x = 0; x < 8; x++) {
            block[y * 8 + x] = ((x + y) & 1) ? 100 : -100;
        }
    }

    print_block("Input (checkerboard)", block);

    prores_fdct_8x8(block);

    print_block("DCT Output (should have energy at [7,7])", block);

    printf("DC[0,0] = %d (should be ~0)\n", block[0]);
    printf("High freq[7,7] = %d (should be large)\n", block[63]);
    printf("\n");
}

/* Test 5: Edge - sharp transition */
static void test_edge(void)
{
    int16_t block[64];

    printf("=== Test 5: Vertical Edge ===\n");

    /* Left half = -200, right half = +200 */
    for (int y = 0; y < 8; y++) {
        for (int x = 0; x < 8; x++) {
            block[y * 8 + x] = (x < 4) ? -200 : 200;
        }
    }

    print_block("Input (vertical edge)", block);

    prores_fdct_8x8(block);

    print_block("DCT Output", block);

    printf("DC[0,0] = %d (should be 0 - balanced)\n", block[0]);
    printf("\n");
}

int main(void)
{
    printf("ProRes DCT Diagnostic Tests\n");
    printf("============================\n\n");

    test_solid_block();
    test_offset_block();
    test_horizontal_gradient();
    test_checkerboard();
    test_edge();

    printf("Tests complete.\n");
    return 0;
}
