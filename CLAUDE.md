# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Build Commands

### WASM Build (requires Docker)
```bash
npm run build:wasm    # Build WASM module using Docker + Emscripten (3.1.50)
npm run build:js      # Build JS wrapper with Rollup (ESM + CJS output)
npm run build         # Build both WASM and JS
```
The WASM build uses `build/Dockerfile` with CMake + Ninja. Output goes to `dist/` as a single-file module (WASM embedded in JS via `SINGLE_FILE=1`). Memory: 64MB initial, 2GB max.

### Native Build (for testing/debugging)
```bash
# Compile a specific native test (e.g., test_native)
gcc -o test/binaries/test_native test/src/test_native.c \
    src/encoder/*.c src/muxer/*.c \
    -I src -lm

# Run it
./test/binaries/test_native test/outputs/output.mov
```
Other test sources in `test/src/`: `test_profiles.c` (all 422 profiles), `test_4444_alpha.c` (alpha encoding), `test_dct.c`/`test_vlc.c` (unit tests), `test_png_sequences.c` (batch encoder, uses `third_party/stb_image.h`).

### Test Suite
```bash
npm test                              # Run test/*.test.mjs (Node.js test runner)
bash test/scripts/run_test_suite.sh   # Full suite: build, encode, FFmpeg compare, PSNR/SSIM reports
```
The test suite uses PNG sequences in `test/reference/` (TEST-01 through TEST-05). Results go to `test/suite-results/` with per-sequence `metrics.json` and `report.md`. Configuration lives in `test/scripts/sequences.conf`.

### Development Server
```bash
npm run demo    # Serves the repo root on localhost:3000 (demos live in test/)
```
Three HTML demos: `test/index.html` (interactive canvas recording with P5.js), `test/test_wasm_sequences.html` (batch profile comparison), and `test/mediabunny.html` (MediaBunny custom-encoder integration).

## Architecture

WebAssembly-based Apple ProRes encoder. Three layers:

```
JS API (lib/index.js)  →  WASM bindings (src/wasm/bindings.c)  →  C encoder + muxer
```

**JS Wrapper (`lib/index.js`, `lib/index.d.ts`)** — `ProResEncoder` class manages WASM memory lifecycle: allocates an RGBA buffer once via `_prores_wasm_alloc`, copies frame data into WASM heap each frame, frees on destroy. Also exports `downloadMov()`, `movToBlob()`, `movToObjectUrl()` helpers. Rollup bundles to ESM + CJS (`dist/`).

**WASM Bindings (`src/wasm/bindings.c`)** — `ProResWasmContext` wraps both encoder and muxer into a single opaque handle. Handles RGBA→YUV conversion internally (dispatches to `rgba_to_yuv422p10` or `rgba_to_yuva444p10` based on profile). Error codes: -1 (invalid args), -2 (encode failed), -3 (mux/OOM).

**C Encoder (`src/encoder/`)** — Based on FFmpeg's `proresenc_kostya.c`:
- `prores_encoder.c` — Frame/picture headers, slice encoding, RGBA→YUV conversion, quantization matrices per profile
- `prores_dct.c` — Integer Loeffler 8x8 FDCT (PASS1_BITS=1 for 32x DC gain)
- `prores_vlc.c` — Rice/Golomb entropy coding, position-major AC encoding, custom ProRes scan order

**MOV Muxer (`src/muxer/mov_muxer.c`)** — QuickTime container writer (sample tables, color metadata atoms). Max 100,000 frames.

### Encoding Pipeline

1. RGBA (8-bit) → YUV conversion (BT.709 coefficients, 10-bit output for all profiles)
2. Frame divided into 16x16 macroblocks, grouped into slices (8 MBs wide)
3. Each 8x8 block: FDCT → quantize (truncation, not rounding) → VLC
4. AC coefficients encoded position-major (all blocks' coeff[1], then coeff[2], etc.)
5. Slice data + headers (6 bytes for 422, 8 bytes for 4444) → MOV container

### ProRes Profiles
- 422 profiles (0-3): YUV422P10, 4:2:2 chroma subsampling, 10-bit
- 4444 profiles (4-5): YUVA444P10, 4:4:4 full chroma + alpha, 10-bit internally (container declares 12-bit)

## ProRes Implementation Gotchas

These are hard-won lessons — violating any of these causes subtle visual corruption:

- **DCT scaling**: Must produce 32x DC gain (10-bit FDCT params). Using 8-bit libjpeg params gives only 8x → washed-out/blocky output.
- **DC offset**: Do NOT subtract sample center before DCT. Instead subtract `0x4000` from raw DC in `encode_dcs` (FFmpeg convention).
- **Quantization**: Use integer truncation (C division), not round-to-nearest. Rounding preserves too many small coefficients → 2x file size bloat.
- **Bit depth**: Always 10-bit internally (`bits_per_raw_sample=10`), even for 4444. 12-bit would overflow int16_t after DCT (32×4095 > 32767).
- **4444 chroma block order**: Column-major (TL, BL, TR, BR), unlike luma which is row-major (TL, TR, BL, BR).
- **Alpha encoding**: Uses per-pixel differential + Rice/Golomb run-length coding, NOT DCT+VLC like luma/chroma.
- **Color metadata**: FFmpeg writes primaries=2, transfer=2, matrix=2 ("unspecified") by default.

## Testing Against FFmpeg

```bash
# Encode reference with FFmpeg
ffmpeg -i input.png -vcodec prores_ks -profile:v 4444 -pix_fmt yuva444p10le ref.mov

# Decode our output and compare
ffmpeg -i test/outputs/output.mov -vframes 1 -pix_fmt rgba decoded.png
compare original.png decoded.png diff.png
```
