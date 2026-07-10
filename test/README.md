# Test Directory

This directory contains tests, test outputs, and debugging tools for the ProRes encoder.

## Directory Structure

```
test/
├── index.html                  # Browser-based demo page
├── test_wasm_sequences.html    # WASM PNG sequence test page
├── src/                        # Test source code (.c files)
│   ├── test_native.c           # Basic native encoding test
│   ├── test_profiles.c         # 422 profile quantization matrix test
│   ├── test_4444_alpha.c       # 4444 alpha encoding test
│   ├── test_png_sequences.c    # PNG sequence encoding test (all profiles)
│   ├── test_dct.c / test_dct_ref.c
│   └── test_vlc.c
├── scripts/                    # Test automation scripts
│   ├── sequences.conf          # Sequence metadata (dir, resolution, fps, etc.)
│   ├── encode_native.sh        # Build + run native encoder for all sequences
│   ├── encode_ffmpeg.sh        # FFmpeg reference encoding
│   ├── analyze_results.sh      # PSNR/SSIM computation + diff images
│   ├── generate_reports.sh     # Markdown report generation
│   └── run_test_suite.sh       # Master orchestrator
├── outputs/                    # Encoded videos (.mov) and decoded frames (.png)
├── diffs/                      # Difference comparison images
├── binaries/                   # Compiled test executables
├── reference/                  # PNG reference sequences (from After Effects)
│   ├── TEST-01/                # 1080x1080 30fps 150 frames
│   ├── TEST-02-30fps/          # 1080x1080 30fps 150 frames
│   ├── TEST-02-60fps/          # 1080x1080 60fps 300 frames
│   ├── TEST-03/                # 1080x1080 30fps 150 frames
│   ├── TEST-04/                # 1920x1080 60fps 300 frames
│   └── TEST-05/                # 1080x1080 30fps 150 frames (RGBA alpha)
└── suite-results/              # Test suite output (gitignored)
    ├── summary.md
    └── <SEQUENCE>/
        ├── native/             # Native encoder MOVs
        ├── ffmpeg/             # FFmpeg reference MOVs
        ├── wasm/               # WASM encoder MOVs (from browser test)
        ├── diffs/              # Diff images (encoder-vs-ref/profile/)
        ├── metrics.json        # PSNR/SSIM/size data
        └── report.md           # Per-sequence comparison report
```

## PNG Sequence Test Suite

Compares our encoder against FFmpeg's `prores_ks` across all ProRes profiles using real PNG sequences rendered from After Effects.

> Note: this testing suite only encodes ffmpeg and native since native and WASM are identical.

### PNG Sequences

To configure the set of PNG sequences to encode, use `sequences.conf` and follow its format.

### Running the Test Suite

```bash
# Run all sequences (native + FFmpeg + analysis + reports)
./test/scripts/run_test_suite.sh

# Run a single sequence
./test/scripts/run_test_suite.sh TEST-01

# Skip FFmpeg if you only want native results
./test/scripts/run_test_suite.sh --skip-ffmpeg

# Skip analysis/reports (encoding only)
./test/scripts/run_test_suite.sh --skip-analyze

# Run individual steps
./test/scripts/encode_native.sh TEST-01
./test/scripts/encode_ffmpeg.sh TEST-01
./test/scripts/analyze_results.sh TEST-01
./test/scripts/generate_reports.sh TEST-01
```

### Building the Native Test Binary

```bash
gcc -O2 -o test/binaries/test_png_sequences test/src/test_png_sequences.c \
    src/encoder/*.c src/muxer/*.c -I src -I third_party -lm
```

### WASM Browser Test

1. Build the WASM module: `npm run build`
2. Start the dev server: `npm run demo`
3. Open `http://localhost:3000/test/test_wasm_sequences.html`
4. Select a PNG folder, configure FPS, click "Encode All Profiles"
5. Download results as ZIP

### Output: Metrics & Reports

After running the suite, results are in `test/suite-results/`:

- **metrics.json** - Structured PSNR/SSIM/file size data per encoder and profile
- **report.md** - Per-sequence markdown report with comparison tables
- **summary.md** - Cross-sequence overview
- **diffs/** - Amplified difference images (first, middle, last frames)

### Profiles Tested

Non-alpha sequences encode all 6 profiles: Proxy, LT, Standard, HQ, 4444, 4444XQ.
Alpha sequences (TEST-05) encode only 4444 and 4444XQ.

---

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

# PNG sequence test (requires third_party/stb_image.h)
gcc -O2 -o test/binaries/test_png_sequences test/src/test_png_sequences.c \
    src/encoder/*.c src/muxer/*.c \
    -I src -I third_party -lm

# Profile quantization matrix test
gcc -o test/binaries/test_profiles test/src/test_profiles.c \
    src/encoder/*.c src/muxer/*.c \
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
