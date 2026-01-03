# ProRes WASM Encoder

A WebAssembly-based Apple ProRes encoder for encoding canvas frames to ProRes MOV files directly in the browser.

## Features

- **All ProRes Profiles**: Proxy, LT, Standard, HQ, 4444, 4444 XQ
- **Alpha Channel Support**: Full alpha channel encoding with 4444 profiles
- **Arbitrary Frame Rates**: Support for any frame rate (24, 29.97, 30, 60, etc.)
- **Canvas Integration**: Direct encoding from HTML canvas elements
- **10-bit Color**: Professional-grade 10-bit YUV encoding
- **Pure WASM**: No server-side processing required

## Installation

```bash
npm install prores-wasm
```

## Quick Start

```javascript
import { createProResEncoder, ProResProfile, downloadMov } from 'prores-wasm';

// Create encoder
const encoder = await createProResEncoder();

// Initialize with settings
encoder.initialize({
  width: 1920,
  height: 1080,
  frameRateNum: 24000,
  frameRateDen: 1001,  // 23.976 fps
  profile: ProResProfile.HQ,
  quality: 85
});

// Encode frames from canvas
const canvas = document.getElementById('myCanvas');
for (let i = 0; i < 100; i++) {
  drawFrame(canvas, i);  // Your rendering code
  encoder.addFrameFromCanvas(canvas);
}

// Get MOV file and download
const movData = encoder.finalize();
downloadMov(movData, 'my-video.mov');

// Clean up
encoder.destroy();
```

## API Reference

### `createProResEncoder(): Promise<ProResEncoder>`

Creates a new ProRes encoder instance.

### `ProResEncoder.initialize(options)`

Initialize the encoder with the specified options:

| Option | Type | Default | Description |
|--------|------|---------|-------------|
| `width` | number | required | Frame width (must be multiple of 16) |
| `height` | number | required | Frame height (must be multiple of 16) |
| `frameRateNum` | number | 30 | Frame rate numerator |
| `frameRateDen` | number | 1 | Frame rate denominator |
| `profile` | ProResProfile | HQ | ProRes profile |
| `quality` | number | 85 | Quality (0-100) |

### `ProResEncoder.addFrameRgba(rgbaData: Uint8Array)`

Add a frame from raw RGBA data (width × height × 4 bytes).

### `ProResEncoder.addFrameFromCanvas(canvas: HTMLCanvasElement)`

Add a frame directly from a canvas element.

### `ProResEncoder.addFrameFromImageData(imageData: ImageData)`

Add a frame from an ImageData object.

### `ProResEncoder.finalize(): Uint8Array`

Finalize encoding and return the MOV file data.

### `ProResEncoder.destroy()`

Free all resources.

## ProRes Profiles

| Profile | Constant | Bitrate (1080p30) | Use Case |
|---------|----------|-------------------|----------|
| Proxy | `ProResProfile.PROXY` | ~45 Mbps | Offline editing |
| LT | `ProResProfile.LT` | ~102 Mbps | Light editing |
| Standard | `ProResProfile.STANDARD` | ~147 Mbps | Standard workflows |
| HQ | `ProResProfile.HQ` | ~220 Mbps | High quality (recommended) |
| 4444 | `ProResProfile.P4444` | ~330 Mbps | With alpha channel |
| 4444 XQ | `ProResProfile.P4444XQ` | ~500 Mbps | Maximum quality |

## Frame Rates

Use numerator/denominator for precise frame rates:

| Frame Rate | Numerator | Denominator |
|------------|-----------|-------------|
| 23.976 fps | 24000 | 1001 |
| 24 fps | 24 | 1 |
| 25 fps | 25 | 1 |
| 29.97 fps | 30000 | 1001 |
| 30 fps | 30 | 1 |
| 59.94 fps | 60000 | 1001 |
| 60 fps | 60 | 1 |

## Building from Source

### Prerequisites

- Docker (for Emscripten build)
- Node.js 18+
- npm

### Build Steps

```bash
# Clone repository
git clone https://github.com/your-repo/prores-wasm
cd prores-wasm

# Install dependencies
npm install

# Build WASM module (requires Docker)
npm run build:wasm

# Build JavaScript wrapper
npm run build:js

# Or build everything
npm run build
```

### Native Testing

You can test the encoder natively without WASM:

```bash
# Compile native test
cd test
gcc -o test_native test_native.c \
    ../src/encoder/*.c \
    ../src/muxer/*.c \
    -I../src -lm

# Run test
./test_native output.mov
```

## Architecture

```
┌─────────────────────────────────────────────────────────────────┐
│  JavaScript API                                                  │
│  ┌──────────────────────────────────────────────────────────┐   │
│  │ ProResEncoder class                                       │   │
│  │ - initialize(), addFrameRgba(), finalize(), destroy()    │   │
│  └──────────────────────────────────────────────────────────┘   │
└─────────────────────────────────────────────────────────────────┘
                              │
                    ┌─────────▼─────────┐
                    │ Emscripten WASM   │
                    │ Bindings          │
                    └─────────┬─────────┘
                              │
┌─────────────────────────────▼───────────────────────────────────┐
│  WASM Module (C)                                                │
│  ┌────────────────────┐    ┌────────────────────────────────┐  │
│  │ ProRes Encoder     │    │ MOV Muxer                      │  │
│  │ - DCT transform    │    │ - QuickTime container format   │  │
│  │ - Quantization     │    │ - Sample tables                │  │
│  │ - VLC coding       │    │ - Color metadata               │  │
│  └────────────────────┘    └────────────────────────────────┘  │
└─────────────────────────────────────────────────────────────────┘
```

## Browser Support

- Chrome 89+
- Firefox 89+
- Safari 15+
- Edge 89+

Requires WebAssembly support.

## Performance

Typical encoding speeds on modern hardware:

| Resolution | Profile | Speed |
|------------|---------|-------|
| 720p | HQ | ~15-20 fps |
| 1080p | HQ | ~8-12 fps |
| 4K | HQ | ~2-4 fps |

Performance depends on CPU, profile, and quality settings.

## License

MIT License

## Acknowledgments

- Based on ProRes codec reverse engineering by FFmpeg project
- MOV container format based on Apple QuickTime File Format specification
- Inspired by [h264-mp4-encoder](https://github.com/ArnoldoRios/h264-mp4-encoder) architecture

## Related Projects

- [ffmpeg.wasm](https://github.com/ffmpegwasm/ffmpeg.wasm) - Full FFmpeg in WebAssembly
- [mp4-wasm](https://github.com/ArnoldoRios/mp4-wasm) - H.264 MP4 muxer in WASM
- [webm-wasm](https://github.com/nickswalker/webm-wasm) - WebM encoder in WASM
