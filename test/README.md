# Test Directory

This directory contains tests, test outputs, and debugging tools for the ProRes encoder.

## Directory Structure

```
test/
├── index.html      # Browser-based demo page
├── src/            # Test source code (.c files)
├── outputs/        # Encoded videos (.mov) and decoded frames (.png)
├── diffs/          # Difference comparison images
├── binaries/       # Compiled test executables
└── reference/      # Reference frames for comparison
    ├── amesa/
    └── blob/
```

## Test Categories

### Solid Color Tests
- `test_solid.*` - Validates basic encoding with uniform colors

### Gradient Tests
- `test_gradient.*` - Tests DCT on smooth transitions
- `test_large_gradient.*` - Large gradient images

### Edge Tests
- `test_edge_*` - Tests block boundary handling
- `test_h_edge_*` - Horizontal edge tests
- `test_block_edge.*` - Block edge artifacts

### Circle Tests
- `test_circle_*` - Tests curved shapes
- `test_color_circles.*` - Colored circle patterns

### Quality Tests
- `test_q1.*` - Quality setting 1 (lowest)
- `test_q80.*` - Quality setting 80

### Comparison Tests
- `*_ffmpeg.*` - Files encoded with FFmpeg (reference)
- `*_ours.*` - Files encoded with this encoder

### Debug Traces
- `trace_ac_encoding` - Debug AC coefficient encoding
- `trace_block` - Debug block processing
- `trace_h_v_edge` - Debug horizontal/vertical edge handling
- `trace_vlc` - Debug variable-length coding

## Building Tests

### Native Test Executables

Compile from `test/src/`:

```bash
# Main native test
gcc -o test/binaries/test_native test/src/test_native.c \
    src/encoder/*.c src/muxer/*.c \
    -I src -lm

# DCT test
gcc -o test/binaries/test_dct test/src/test_dct.c \
    src/encoder/prores_dct.c \
    -I src -lm

# VLC test
gcc -o test/binaries/test_vlc test/src/test_vlc.c \
    src/encoder/prores_vlc.c \
    -I src -lm
```

## Running the Demo

```bash
npm run demo
```

Then open http://localhost:3000/test/ in your browser.

## Comparing with FFmpeg Reference

```bash
# Encode with FFmpeg (reference)
ffmpeg -i input.png -vcodec prores_ks -profile:v 4444 -pix_fmt yuva444p10le test/outputs/ref.mov

# Decode our output
ffmpeg -i test/outputs/our_output.mov -vframes 1 -pix_fmt rgba test/outputs/decoded.png

# Generate diff image
compare original.png test/outputs/decoded.png test/diffs/diff.png
```
