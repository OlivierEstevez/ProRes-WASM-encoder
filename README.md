# ProRes WASM Encoder

A WebAssembly-based Apple ProRes encoder that encodes canvas frames to `.mov` files directly in the browser. No server required.

## Features

- **All ProRes Profiles**: Proxy, LT, Standard, HQ, 4444, and 4444 XQ
- **Alpha Channel**: Full transparency support with 4444 profiles
- **QuickTime .mov Output**: With any framerate or resolution
- **Canvas Integration**: Encode directly from `HTMLCanvasElement` or `OffscreenCanvas`
- **Professional Color**: Internal 10-bit YUV encoding with BT.709 color matrix
- **Pure WASM**: Runs entirely in the browser, no server-side processing
- **Multi-threaded**: Optional frame-parallel encoding across Web Workers, no COOP/COEP headers needed
- **MediaBunny Compatible**: Drop-in custom encoder for [MediaBunny](https://mediabunny.dev)'s conversion and muxing pipelines

## Installation

```bash
npm install prores-wasm-encoder
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
  frameRate: 30,
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

## Multi-threaded Encoding

ProRes is intra-only — every frame is independent — so frames encode in
parallel across Web Workers (real threads) with **byte-identical output**
to the single-thread encoder. **No SharedArrayBuffer or COOP/COEP headers
required** — each worker holds its own WASM instance, and the main thread
muxes the results in frame order. Scaling is near-linear with core count
(measured ~6.4x on 8 workers for complex 1080p HQ).

Multi-threaded encoding lives in its own entry point, so apps that don't
use it never pay for it:

```javascript
import { createProResEncoderPool } from 'prores-wasm-encoder/parallel';
import { ProResProfile } from 'prores-wasm-encoder';

const pool = await createProResEncoderPool({
  width: 1920,
  height: 1080,
  frameRate: 30,
  profile: ProResProfile.HQ,
  workers: 4,           // default: min(hardwareConcurrency, 8)
});

// Same frame methods as the single-thread encoder, but async
for (let i = 0; i < frames.length; i++) {
  await pool.addFrameRgba(frames[i]);   // backpressured
}

const mov = await pool.finalize();       // or finalizeToBlob()
await pool.destroy();
```

Workers are spawned from an inlined Blob URL, so **no bundler configuration
is required** — it works in Vite, webpack, esbuild, plain `<script>`, and
CDN builds alike. The WASM binary ships and compiles exactly **once**: the
compiled module is shared with every worker, so adding workers adds no
download or compile cost. Requires Web Workers with module support
(Chrome 80+, Safari 15+, Firefox 114+); where those are unavailable, use
the single-thread `createProResEncoder()`.

Streaming (`onFrameData`) and `finalizeToBlob()` work on the pool exactly as
on the single-thread encoder, so long parallel recordings also stay within
constant memory.

### Bundle size

| Import | Size (min+gzip) |
|---|---|
| `prores-wasm-encoder` | ~36 KB |
| `prores-wasm-encoder/parallel` | ~41 KB |

With a bundler, apps importing both entries share the common chunk (encoder
core + WASM), so the combined cost is the parallel size, not the sum.

## MediaBunny Integration

This library can plug into [MediaBunny](https://mediabunny.dev)'s custom-encoder
API, so any MediaBunny `Output` or `Conversion` targeting the `'prores'`
codec encodes through it.

```javascript
import { Output, BufferTarget, MovOutputFormat, CanvasSource } from 'mediabunny';
import { registerProResEncoder } from 'prores-wasm-encoder/mediabunny';

registerProResEncoder(); // multi-threaded when Web Workers are available

const output = new Output({
  format: new MovOutputFormat(),
  target: new BufferTarget(),
});
const source = new CanvasSource(canvas, {
  codec: 'prores',
  fullCodecString: 'apch',   // pick the variant by fourcc: apco, apcs, apcn, apch, ap4h, ap4x
  bitrate: 100_000_000,
});
output.addVideoTrack(source, { frameRate: 30 });
await output.start();

for (let f = 0; f < totalFrames; f++) {
  drawFrame(f);                      // your rendering code
  await source.add(f / 30, 1 / 30);  // capture + encode
}

await output.finalize();
// output.target.buffer is the finished .mov
```

`mediabunny` is an optional peer dependency (`npm install mediabunny`), and
this entry point is ESM-only. Select the ProRes variant with
`fullCodecString` (a ProRes fourcc), or omit it and MediaBunny infers one
from `bitrate` and alpha settings. Encoded frames are identical to the ones
this library's own muxer writes.

## API Reference

### `createProResEncoder(): Promise<ProResEncoder>`

Creates and returns a new encoder instance. The WASM module is loaded automatically.

### `createProResEncoderPool(options): Promise<ProResEncoderPool>`

Imported from **`prores-wasm-encoder/parallel`**. Creates a multi-threaded,
frame-parallel encoder pool (see [Multi-threaded Encoding](#multi-threaded-encoding)).
Accepts the same options as `initialize()` plus `workers` (worker count).
Frame-adding methods (`addFrameRgba`, `addFrameFromCanvas`,
`addFrameFromImageData`) and `finalize()` / `finalizeToBlob()` are **async**.

### `ProResEncoder.initialize(options)`

Initialize the encoder with the specified options:

| Option | Type | Default | Description |
|--------|------|---------|-------------|
| `width` | number | required | Frame width in pixels |
| `height` | number | required | Frame height in pixels |
| `frameRate` | number | `30` | Frame rate (e.g. `23.976`, `30`, `60`). Standard rates are mapped to exact num/den pairs |
| `frameRateNum` | number | | Advanced: explicit numerator (overrides `frameRate` when both num and den are set) |
| `frameRateDen` | number | | Advanced: explicit denominator (overrides `frameRate` when both num and den are set) |
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

Just pass the frame rate as a number:

```javascript
encoder.initialize({
  width: 1920,
  height: 1080,
  framerate: 23.976,
  profile: ProResProfile.HQ,
})
```

For fractional framerates, you can also use `frameRateNum`/`frameRateDen`:

```javascript
encoder.initialize({
  width: 1920,
  height: 1080,
  frameRateNum: 24000,
  frameRateDen: 1001,
  profile: ProResProfile.HQ,
});
```

## Long Recordings

Frame data never accumulates in WASM memory: each encoded frame is handed
to JavaScript immediately, so encoder memory stays constant (~64 MB)
regardless of recording length.

- `finalize()` returns the file as one `Uint8Array` (simple, fine for
  short recordings)
- `finalizeToBlob()` returns a `Blob` — preferred for long recordings,
  since Blob parts are browser-managed and need no single contiguous
  allocation
- For unbounded recordings, pass `onFrameData` to `initialize()` and
  stream each chunk to disk (e.g. OPFS), then assemble
  `[header, ...chunks, moov]` from `finalizeHeaders()`

Files over 4 GB are written with 64-bit offsets (`co64` + large `mdat`)
automatically.

## Browser Support

- Chrome 91+
- Firefox 89+
- Safari 16.4+
- Edge 91+

Requires WebAssembly with SIMD128 (universal in browsers since early 2023).

## Performance

Approximate encoding speeds on modern hardware:

| Resolution | Profile | Speed |
|------------|---------|-------|
| 720p | HQ | ~15-20 fps |
| 1080p | HQ | ~8-12 fps |
| 4K | HQ | ~2-4 fps |

> Speeds vary with CPU, profile, and content complexity.

## Motivation

I'm a motion designer and creative developer working in branding, and I kept
running into the same wall: there was no good way to export transparent, high
quality video directly from the web. Everything I built needed a server or a
round-trip through desktop software just to get an alpha-capable master out of
a canvas.

So I built this for my own custom tools. It's a port of FFmpeg's `prores_ks`
to C and WebAssembly. I'm not a C developer, and this started as an
experiment to see how far I could push a browser-native ProRes encoder.
Despite being built almost entirely with AI-assisted tools, it ended up
working surprisingly well, matching FFmpeg's output closely enough that I
decided to polish it into a proper library.

It's still primarily a tool I use in my own work, but if it's useful to you,
that makes me happy :)

## License

**[GNU Lesser General Public License v2.1 or later](LICENSE).**

The encoder core is derived from FFmpeg's `proresenc_kostya.c` (© Anatoliy
Wasserman, Konstantin Shishkov), which is LGPL-2.1+. As a derivative work,
this project is distributed under the same license. The integer DCT is
derived from libjpeg's `jfdctint.c` (IJG License).

## Trademark

"Apple ProRes" and "ProRes" are trademarks of Apple Inc. This project is an
independent, unofficial implementation and is **not affiliated with, endorsed
by, or certified by Apple Inc.** The codec bitstream is described in the
public SMPTE RDD 36 specification. This software license grants no patent
rights; see the SMPTE RDD 36 IP declarations.

## Acknowledgments

- Encoder derived from the FFmpeg project's ProRes encoder (`proresenc_kostya.c`)
- Integer DCT derived from libjpeg's `jfdctint.c` (IJG License)
- MOV container format based on Apple QuickTime File Format specification
- Inspired by [h264-mp4-encoder](https://github.com/TrevorSundberg/h264-mp4-encoder) architecture and [mp4-wasm](https://github.com/mattdesl/mp4-wasm/?tab=readme-ov-file)
