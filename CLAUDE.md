# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Build Commands

### WASM Build (requires Docker)
```bash
npm run build:wasm    # Build WASM module using Docker + Emscripten
npm run build:js      # Build JS wrapper with Rollup
npm run build         # Build both WASM and JS
```

### Native Build (for testing/debugging)
```bash
# Compile native test
gcc -o test/binaries/test_native test/src/test_native.c \
    src/encoder/*.c src/muxer/*.c \
    -I src -lm

# Run test
./test/binaries/test_native test/outputs/output.mov
```

### Development Server
```bash
npm run demo          # Serves test/ on localhost:3000
```

## Architecture

This is a WebAssembly-based Apple ProRes video encoder that runs entirely in the browser.

### Core Components

**C Encoder (`src/encoder/`)**
- `prores_encoder.c` - Main encoder: frame header, picture header, slice encoding, YUV conversion
- `prores_dct.c` - 8x8 DCT using Loeffler algorithm (matches libjpeg)
- `prores_vlc.c` - Variable-length coding with Rice/Golomb codes for DC/AC coefficients

**MOV Muxer (`src/muxer/`)**
- `mov_muxer.c` - QuickTime container writer (sample tables, color metadata)

**WASM Bindings (`src/wasm/`)**
- `bindings.c` - Emscripten interface exposing encoder+muxer as single API

### Encoding Pipeline

1. RGBA input → YUV conversion (BT.709, 10/12-bit)
2. Frame divided into 16x16 macroblocks, grouped into slices (8 MBs wide)
3. Each 8x8 block: DCT → Quantization → VLC encoding
4. ProRes-specific: position-major AC encoding (all blocks' coeff[1], then coeff[2], etc.)
5. Slice data assembled with headers → MOV container

### ProRes Profiles
- 422 profiles: YUV422P10 (4:2:2, 10-bit)
- 4444 profiles: YUVA444P10 (4:4:4:4, 12-bit with alpha)

### Key Implementation Details
- Slice header: 6 bytes for 422, 8 bytes for 4444 (includes V size field)
- DC coefficients: differential coding with sign prediction
- Scan order: custom ProRes progressive scan (not standard zigzag)
- Quantization matrices stored in scan order

## Testing

Test files are organized in `test/`:
- `test/src/` - Test source code (.c files)
- `test/outputs/` - Encoded videos and decoded frames
- `test/diffs/` - Difference comparison images
- `test/binaries/` - Compiled test executables

Compare encoder output against FFmpeg reference:
```bash
# Encode with FFmpeg (reference)
ffmpeg -i input.png -vcodec prores_ks -profile:v 4444 -pix_fmt yuva444p10le test/outputs/ref.mov

# Decode and compare
ffmpeg -i test/outputs/output.mov -vframes 1 -pix_fmt rgba test/outputs/decoded.png
compare original.png test/outputs/decoded.png test/diffs/diff.png
```

## Known Issues

The encoder currently has visual artifacts on complex images (color gradients, edges). Simple patterns (solid colors, uniform gradients) encode correctly. Root cause is being investigated in AC coefficient encoding structure.
