# Development Guide

Instructions for building, testing, and contributing to the ProRes WASM encoder.

## Prerequisites

- **Docker** — Required for the Emscripten WASM build
- **Node.js 18+** and **npm**
- **gcc** — For native builds (optional, useful for debugging)
- **FFmpeg** — For reference encoding and quality comparison (optional)

## Build Commands

### WASM Build (requires Docker)

```bash
npm run build:wasm    # Build WASM module using Docker + Emscripten (3.1.50)
npm run build:js      # Build JS wrapper with Rollup (ESM + CJS output)
npm run build         # Build both WASM and JS
```

The WASM build uses `build/Dockerfile` with CMake + Ninja. Output goes to `dist/` as a single-file module (WASM embedded in JS via `SINGLE_FILE=1`). Memory: 64MB initial, 2GB max.

### Native Build (for debugging)

You can compile and test the encoder natively without WASM:

```bash
# Compile a specific native test (e.g., test_native)
gcc -o test/binaries/test_native test/src/test_native.c \
    src/encoder/*.c src/muxer/*.c \
    -I src -lm

# Run it
./test/binaries/test_native test/outputs/output.mov
```

Other test sources in `test/src/`:
- `test_profiles.c` — All 422 profiles
- `test_4444_alpha.c` — Alpha encoding
- `test_dct.c` / `test_vlc.c` — Unit tests
- `test_png_sequences.c` — Batch encoder (uses `third_party/stb_image.h`)

## Testing

### Node.js Tests

```bash
npm test    # Runs test/test.js
```

### Full Test Suite

```bash
bash test/scripts/run_test_suite.sh
```

The test suite:
1. Builds the encoder
2. Encodes PNG sequences from `test/reference/` (TEST-01 through TEST-05)
3. Compares output against FFmpeg reference encodes
4. Generates PSNR/SSIM quality metrics

Results go to `test/suite-results/` with per-sequence `metrics.json` and `report.md`. Configuration lives in `test/scripts/sequences.conf`.

### Testing Against FFmpeg

```bash
# Encode reference with FFmpeg
ffmpeg -i input.png -vcodec prores_ks -profile:v 4444 -pix_fmt yuva444p10le ref.mov

# Decode our output and compare
ffmpeg -i test/outputs/output.mov -vframes 1 -pix_fmt rgba decoded.png
compare original.png decoded.png diff.png
```

## Development Server

```bash
npm run demo    # Serves test/ on localhost:3000
```

Three HTML demos:
- `test/index.html` — Interactive canvas recording with P5.js
- `test/test_wasm_sequences.html` — Batch profile comparison
- `test/mediabunny.html` — Recording via the MediaBunny custom-encoder
  integration (`prores-wasm-encoder/mediabunny`); requires `npm install`
  first so `mediabunny` is present as a devDependency

## Architecture

Three layers:

```
JS API (lib/index.js)  →  WASM bindings (src/wasm/bindings.c)  →  C encoder + muxer
```

```
┌─────────────────────────────────────────────────────────────────┐
│  JavaScript API (lib/index.js)                                  │
│  ┌──────────────────────────────────────────────────────────┐   │
│  │ ProResEncoder class                                       │   │
│  │ - initialize(), addFrameRgba(), finalize(), destroy()    │   │
│  └──────────────────────────────────────────────────────────┘   │
└─────────────────────────────────────────────────────────────────┘
                              │
                    ┌─────────▼─────────┐
                    │ Emscripten WASM   │
                    │ Bindings          │
                    │ (src/wasm/        │
                    │  bindings.c)      │
                    └─────────┬─────────┘
                              │
┌─────────────────────────────▼───────────────────────────────────┐
│  WASM Module (C)                                                │
│  ┌────────────────────┐    ┌────────────────────────────────┐  │
│  │ ProRes Encoder      │    │ MOV Muxer                      │  │
│  │ (src/encoder/)      │    │ (src/muxer/)                   │  │
│  │ - DCT transform     │    │ - QuickTime container format   │  │
│  │ - Quantization      │    │ - Sample tables                │  │
│  │ - VLC coding        │    │ - Color metadata atoms         │  │
│  └────────────────────┘    └────────────────────────────────┘  │
└─────────────────────────────────────────────────────────────────┘
```

**JS Wrapper (`lib/index.js`, `lib/index.d.ts`)** — `ProResEncoder` class manages WASM memory lifecycle: allocates an RGBA buffer once via `_prores_wasm_alloc`, copies frame data into WASM heap each frame, frees on destroy. Also exports helper functions (`downloadMov`, `movToBlob`, `movToObjectUrl`). Rollup bundles to ESM + CJS in `dist/`.

**WASM Bindings (`src/wasm/bindings.c`)** — `ProResWasmContext` wraps both encoder and muxer into a single opaque handle. Handles RGBA to YUV conversion internally (dispatches to `rgba_to_yuv422p10` or `rgba_to_yuva444p10` based on profile). Error codes: -1 (invalid args), -2 (encode failed), -3 (mux/OOM).

**C Encoder (`src/encoder/`)** — Based on FFmpeg's `proresenc_kostya.c`:
- `prores_encoder.c` — Frame/picture headers, slice encoding, RGBA to YUV conversion, quantization matrices per profile
- `prores_dct.c` — Integer Loeffler 8x8 FDCT (PASS1_BITS=1 for 32x DC gain)
- `prores_vlc.c` — Rice/Golomb entropy coding, position-major AC encoding, custom ProRes scan order

**MOV Muxer (`src/muxer/mov_muxer.c`)** — QuickTime container writer (sample tables, color metadata atoms). Sample array grows dynamically.

## Encoding Pipeline

1. RGBA (8-bit) is converted to YUV (BT.709 coefficients, 10-bit output for all profiles)
2. Frame is divided into 16x16 macroblocks, grouped into slices (8 MBs wide)
3. Each 8x8 block: FDCT → quantize (truncation, not rounding) → VLC
4. AC coefficients are encoded position-major (all blocks' coeff[1], then coeff[2], etc.)
5. Slice data + headers (6 bytes for 422, 8 bytes for 4444) are assembled into the MOV container

## ProRes Profile Internals

| Profile | Chroma | Alpha | Slice Header |
|---------|--------|-------|-------------|
| 422 Proxy/LT/Standard/HQ | 4:2:2 subsampled | No | 6 bytes |
| 4444 / 4444 XQ | 4:4:4 full chroma | Yes | 8 bytes |

Key implementation details:
- All profiles use 10-bit internally (`bits_per_raw_sample=10`), even 4444. Container metadata may declare 12-bit for 4444 profiles.
- 4444 chroma blocks use column-major order (TL, BL, TR, BR); luma uses row-major (TL, TR, BL, BR).
- Alpha uses per-pixel differential + Rice/Golomb run-length coding (not DCT+VLC like luma/chroma).
- DCT must produce 32x DC gain. The FDCT uses 10-bit parameters (PASS1_BITS=1).
- Quantization uses integer truncation (C division), not round-to-nearest.
- DC offset: no pixel centering before DCT; subtract `0x4000` from raw DC in `encode_dcs`.

## Project Structure

```
├── build/              # Docker + CMake build configuration
├── dist/               # Built output (WASM + JS bundles)
├── lib/                # JavaScript wrapper + TypeScript declarations
│   ├── index.js
│   └── index.d.ts
├── src/
│   ├── encoder/        # ProRes encoder (DCT, VLC, quantization)
│   ├── muxer/          # MOV container muxer
│   └── wasm/           # Emscripten bindings
├── test/
│   ├── src/            # Native C test sources
│   ├── scripts/        # Test suite scripts
│   ├── reference/      # PNG test sequences
│   └── suite-results/  # Quality metric reports
└── third_party/        # stb_image.h
```
