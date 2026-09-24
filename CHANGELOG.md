# Changelog

All notable changes to this project are documented here. The project
follows [Semantic Versioning](https://semver.org): from 1.0.0 on, the
public API is what the TypeScript declarations (`.d.ts`) describe, and
breaking changes to it only ship in a new major version.

## [1.0.0] - 2026-09-24

### Fixed

- `addFrameFromCanvas()` now works with WebGL, WebGL2 and WebGPU canvases.
  It used to throw a `TypeError`, because `getContext('2d')` returns `null`
  on those.
- The pool's backpressure now counts encoded frames waiting to be written
  in order, so a slow frame can no longer grow memory without limit.
- The pool rejects frames added after `finalize()`, and a second
  `finalize()`.
- `destroy()` on a pool that is still encoding makes pending
  `addFrame…()`, `flush()` and `finalize…()` calls reject with "encoder
  destroyed". Before, they never settled.
- If a worker fails to start, `createProResEncoderPool()` now terminates
  the workers that did start, and the error mentions the likely cause
  (a CSP that blocks `blob:` workers).
- `finalize()` and the other methods that return bytes are typed
  `Uint8Array<ArrayBuffer>`, so `new Blob([mov])` type-checks with
  TypeScript 5.7+.
- Types resolve correctly in every TypeScript module mode, including
  `node16`/`nodenext` and legacy `node10` (checked with arethetypeswrong).
- The MediaBunny entry's types import `CustomVideoEncoder` as a value, as
  `extends` needs.

### Changed

- ESM builds are now `.mjs` files (`dist/prores-encoder.mjs`,
  `dist/prores-encoder-parallel.mjs`, `dist/prores-encoder-mediabunny.mjs`)
  with `.d.mts` types, so Node loads them as ES modules without syntax
  detection. Package imports (`prores-wasm-encoder`, `/parallel`,
  `/mediabunny`) are unchanged; only deep imports of `dist/*.esm.js` need
  updating. The UMD files keep their names.
- Options are validated up front, with clear errors: `width` and `height`
  must be positive integers (max 16384 per side and 8192 × 8192 pixels in
  total), `profile` must be a `ProResProfile` value, frame rates must be
  positive, and `workers` must be a positive integer.
- `addFrameFromCanvas()` throws if the canvas backing store size differs
  from the encoder size, instead of reading a cropped or padded frame.
- The single-thread encoder throws on frames after `finalize()`, a second
  `finalize()`, and any use after `destroy()`.
- `range: 'full'` logs a warning: output has always been limited range.
- The pool's `flush()` is now part of the typed API.

### Added

- Test suites for the single-thread encoder, the pool (on real threads),
  the published package contents and types, and a Vite + Chromium browser
  test. CI runs them on every push; tagged releases publish from CI.
- README sections on choosing an encoder, canvas sources and bundlers.

## [0.3.1] - 2026-09-19

The first npm release of the 0.3 line (0.3.0 was tagged but never
published).

### Fixed

- **Frames with some widths were undecodable.** When the width in 16-px
  macroblocks wasn't a multiple of 8, and the remainder wasn't 1, 2 or 4
  (for example 48, 200 or 720 px wide), the encoder wrote one odd-width
  slice at the end of each row. Decoders expect power-of-two slices there,
  so FFmpeg failed with "slice out of bounds". Rows now split like FFmpeg's
  (720 px = 45 MBs = 5 × 8 + 4 + 1). Output for other widths is unchanged.

## [0.3.0] - 2026-07-10

Tagged, but not published to npm; 0.3.1 ships its features.

### Added

- Multi-threaded encoding: `createProResEncoderPool()` from the new
  `prores-wasm-encoder/parallel` entry. Output is byte-identical to the
  single-thread encoder; no COOP/COEP headers needed.
- Streaming: `onFrameData`, `finalizeHeaders()` and `finalizeToBlob()`, so
  memory stays constant for long recordings. Files over 4 GB use 64-bit
  offsets.
- MediaBunny integration: `registerProResEncoder()` from the new
  `prores-wasm-encoder/mediabunny` entry.

### Changed

- About 3 to 5 times faster: WASM SIMD DCT, faster quantization search and
  bitstream writing.
- The WASM binary ships once and is shared by all entry points.
- License clarified as LGPL-2.1-or-later.

## [0.2.3] - 2026-06-12

- Published under the `prores-wasm-encoder` name on npm.

## [0.2.2] - 2026-02-18

- Fixed 4444 alpha quality.

[1.0.0]: https://github.com/OlivierEstevez/ProRes-WASM-encoder/compare/v0.3.1...v1.0.0
[0.3.1]: https://github.com/OlivierEstevez/ProRes-WASM-encoder/compare/v0.3.0...v0.3.1
[0.3.0]: https://github.com/OlivierEstevez/ProRes-WASM-encoder/compare/v0.2.3...v0.3.0
[0.2.3]: https://github.com/OlivierEstevez/ProRes-WASM-encoder/compare/v0.2.2...v0.2.3
[0.2.2]: https://github.com/OlivierEstevez/ProRes-WASM-encoder/compare/v0.2.1...v0.2.2
