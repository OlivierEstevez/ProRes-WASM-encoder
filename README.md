# ProRes WASM Encoder

A WebAssembly-based Apple ProRes encoder that encodes canvas frames to `.mov` files directly in the browser. No server required.

## Features

- **All ProRes Profiles**: Proxy, LT, Standard, HQ, 4444, and 4444 XQ
- **Alpha Channel**: Full transparency support with 4444 profiles
- **QuickTime .mov Output**: With any framerate or resolution
- **Canvas Integration**: Encode directly from `HTMLCanvasElement` or `OffscreenCanvas`
- **Professional Color**: Internal 10-bit YUV encoding with BT.709 color matrix
- **Pure WASM**: Runs entirely in the browser, no server-side processing

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
});

// Encode frames from canvas
const canvas = document.getElementById('myCanvas');
for (let i = 0; i < 100; i++) {
  drawFrame(canvas, i);  // Your rendering code
  encoder.addFrameFromCanvas(canvas);
}

// Download the .mov file
const movData = encoder.finalize();
downloadMov(movData, 'my-video.mov');

// Clean up
encoder.destroy();
```

### Recording a Canvas Animation

```javascript
import { createProResEncoder, ProResProfile, downloadMov } from 'prores-wasm';

const canvas = document.getElementById('canvas');
const encoder = await createProResEncoder();

encoder.initialize({
  width: canvas.width,
  height: canvas.height,
  frameRateNum: 30,
  frameRateDen: 1,
  profile: ProResProfile.HQ,
});

// Capture each frame of your animation loop
function animate(frame) {
  renderScene(canvas, frame);       // Your render logic
  encoder.addFrameFromCanvas(canvas);

  if (frame < totalFrames) {
    requestAnimationFrame(() => animate(frame + 1));
  } else {
    const movData = encoder.finalize();
    downloadMov(movData, 'animation.mov');
    encoder.destroy();
  }
}

animate(0);
```

## API Reference

### `createProResEncoder(): Promise<ProResEncoder>`

Creates and returns a new encoder instance. The WASM module is loaded automatically.

### `ProResEncoder.initialize(options)`

Initialize the encoder with the specified options:

| Option | Type | Default | Description |
|--------|------|---------|-------------|
| `width` | number | required | Frame width in pixels |
| `height` | number | required | Frame height in pixels |
| `frameRateNum` | number | `30` | Frame rate numerator |
| `frameRateDen` | number | `1` | Frame rate denominator |
| `profile` | ProResProfile | `HQ` | ProRes profile to use |
| `range` | string | `"limited"` | Color range: `"limited"` (TV/studio) or `"full"` |

### `ProResEncoder.addFrameRgba(rgbaData)`

Add a frame from raw RGBA pixel data (`Uint8Array` or `Uint8ClampedArray`, must be `width * height * 4` bytes).

### `ProResEncoder.addFrameFromCanvas(canvas)`

Add a frame directly from an `HTMLCanvasElement` or `OffscreenCanvas`.

### `ProResEncoder.addFrameFromImageData(imageData)`

Add a frame from an `ImageData` object (e.g., from `ctx.getImageData()`).

### `ProResEncoder.finalize(): Uint8Array`

Finalize encoding and return the `.mov` file as a `Uint8Array`.

### `ProResEncoder.destroy()`

Free all WASM memory and resources. Always call this when done encoding.

### Properties

| Property | Type | Description |
|----------|------|-------------|
| `frameCount` | number | Number of frames encoded so far |
| `width` | number | Encoder width |
| `height` | number | Encoder height |
| `initialized` | boolean | Whether the encoder is initialized |

### Helper Functions

#### `downloadMov(movData, filename?)`

Triggers a browser download of the `.mov` file. Default filename is `"output.mov"`.

#### `movToBlob(movData): Blob`

Converts the `Uint8Array` to a `Blob` with MIME type `video/quicktime`.

#### `movToObjectUrl(movData): string`

Converts the `Uint8Array` to an Object URL. Remember to call `URL.revokeObjectURL()` when done.

### Constants

#### `ProResProfile`

Profile enum with values: `PROXY` (0), `LT` (1), `STANDARD` (2), `HQ` (3), `P4444` (4), `P4444XQ` (5).

#### `ProfileNames`

Maps profile values to display names (e.g., `ProfileNames[ProResProfile.HQ]` returns `"ProRes 422 HQ"`). Useful for building profile selector UIs.

## ProRes Profiles

| Profile | Constant | Approx. Bitrate (1080p30) | Best For |
|---------|----------|---------------------------|----------|
| 422 Proxy | `ProResProfile.PROXY` | ~45 Mbps | Lightweight proxies for offline editing |
| 422 LT | `ProResProfile.LT` | ~102 Mbps | Editing with limited storage |
| 422 Standard | `ProResProfile.STANDARD` | ~147 Mbps | General-purpose editing |
| 422 HQ | `ProResProfile.HQ` | ~220 Mbps | High quality mastering (recommended) |
| 4444 | `ProResProfile.P4444` | ~330 Mbps | Content with alpha transparency |
| 4444 XQ | `ProResProfile.P4444XQ` | ~500 Mbps | Maximum quality with alpha |

> Bitrates are approximate and vary with content complexity.

## Frame Rates

Use numerator/denominator pairs for precise frame rates:

| Frame Rate | Numerator | Denominator |
|------------|-----------|-------------|
| 23.976 fps | 24000 | 1001 |
| 24 fps | 24 | 1 |
| 25 fps | 25 | 1 |
| 29.97 fps | 30000 | 1001 |
| 30 fps | 30 | 1 |
| 59.94 fps | 60000 | 1001 |
| 60 fps | 60 | 1 |

## Limits

- **WASM memory ceiling**: 2 GB — the maximum number of frames depends on resolution and profile

## Browser Support

- Chrome 89+
- Firefox 89+
- Safari 15+
- Edge 89+

Requires WebAssembly support.

## Performance

Approximate encoding speeds on modern hardware:

| Resolution | Profile | Speed |
|------------|---------|-------|
| 720p | HQ | ~15-20 fps |
| 1080p | HQ | ~8-12 fps |
| 4K | HQ | ~2-4 fps |

> Speeds vary with CPU, profile, and content complexity.

## License

MIT License

## Acknowledgments

- Based on ProRes codec reverse engineering by the FFmpeg project
- MOV container format based on Apple QuickTime File Format specification
- Inspired by [h264-mp4-encoder](https://github.com/TrevorSundberg/h264-mp4-encoder) architecture and [mp4-wasm](https://github.com/mattdesl/mp4-wasm/?tab=readme-ov-file)

## Related Projects

- [ffmpeg.wasm](https://github.com/ffmpegwasm/ffmpeg.wasm) - Full FFmpeg in WebAssembly
- [webm-wasm](https://github.com/GoogleChromeLabs/webm-wasm) - WebM encoder in WASM
