# WebGPU acceleration — investigation & work handoff

Status: **not started** — this is a scoped brief for a future agent/session.
Two independent pieces of work: (A) fix the WebGPU capture path in the demo so
the renderer comparison is fair, and (B) investigate a GPU-side ProRes encode
path. Do (A) first — it's small, high-signal, and de-risks (B).

## Background: what we found

While reframing the benchmark (we're ~size-competitive, not speed-competitive,
vs ffmpeg.wasm — see the benchmark work on the `benchmark` branch), the user
noted that **WebGPU was the *slowest* renderer to capture** in their comparison
demo (Canvas2D / WebGL / p5.js / Three.js / WebGPU), which is counter-intuitive.

Root cause: **it is entirely a GPU→CPU readback cost, not a rendering cost.**
Key facts to carry forward:

- **All of these renderers run on the GPU** (WebGL, Three.js = WebGL, p5 in
  WEBGL mode, and Canvas2D is GPU-accelerated too). None render on the CPU. The
  differences are 100% in how pixels are read *back* to CPU memory to feed the
  WASM/CPU encoder.
- **WebGL readback is one cheap synchronous call**: `gl.readPixels(...RGBA...)`
  (see `test/demos/index.html` lines ~679, ~769, ~883).
- **WebGPU readback is architecturally heavier** and, in the current demo,
  compounded by avoidable per-frame work. See `WebGPURenderer.captureFrame`
  (`test/demos/index.html`, ~line 1100):
  - `getCurrentTexture()` (1104)
  - `device.createBuffer(...)` **allocated every frame** (1113) — no reuse.
  - `copyTextureToBuffer` with **256-byte row padding** (1110, 1119).
  - `queue.submit(...)` then **`await readBuffer.mapAsync(READ)`** (1126) — an
    async round-trip that resolves only after the GPU queue drains; awaiting it
    per frame inserts a hard sync point and destroys CPU/GPU overlap.
  - a **JS row loop to strip the alignment padding** (~1132).
  - a **per-pixel B↔R swap in a nested JS loop** (~1135) because the WebGPU
    canvas is `bgra8unorm` — ~2M scalar ops per 1080p frame.

So WebGPU pays: per-frame staging alloc + padded GPU copy + async map (serializes
the pipeline) + JS un-pad + JS channel swap. WebGL pays one `readPixels`. That is
why the fastest-to-render tech is the slowest-to-capture.

This readback tax is also the core motivation for (B): a GPU-side encoder never
reads back the raw 8.3 MB/frame RGBA at all — it would read back only the
~0.9 MB compressed ProRes frame, after the math instead of before.

---

## Task A — Make the WebGPU capture fair (small, do first)

**File:** `test/demos/index.html`, `WebGPURenderer.captureFrame` (~line 1100).

**Goal:** bring WebGPU capture in line with WebGL so the demo's renderer
comparison measures rendering + a reasonable readback, not avoidable JS overhead.

**Fixes (roughly in impact order):**
1. **Reuse staging buffers** — allocate a small ring (2–3) of `MAP_READ`
   buffers once (sized to `paddedBytesPerRow * h`), rotate per frame, instead of
   `createBuffer` every frame.
2. **Stop serializing on `mapAsync`** — pipeline it: submit the copy for frame N,
   but map/read a buffer from frame N-1 or N-2, so the GPU isn't stalled waiting
   for the CPU each frame. (For a strictly-ordered encoder feed, at minimum avoid
   `await`-ing the map inside the render loop's critical path.)
3. **Kill the JS channel swap** — either configure the canvas context with
   `format: 'rgba8unorm'` if the adapter allows, or do the BGRA→RGBA swap on the
   GPU in the copy/blit, not in a JS per-pixel loop.
4. **Avoid the JS un-pad copy** when possible — if `w*4` is already a multiple of
   256 (true for 1920: 7680/256 = 30) there is no padding to strip; guard the
   un-pad loop behind `paddedBytesPerRow !== unpaddedBytesPerRow`.

**Acceptance:** WebGPU capture time drops to roughly WebGL's ballpark; the demo's
per-renderer capture timings become a fair comparison. Note in the demo UI (or a
comment) that all renderers pay a GPU→CPU readback and that WebGPU's is inherently
async.

**Watch out for:** the WebGPU canvas texture is only valid for the current frame
(must copy before the next `getCurrentTexture`); the context must be configured
with `usage` including `COPY_SRC`; `mapAsync` cannot be called while the buffer is
already mapped (hence the ring).

---

## Task B — GPU-side ProRes encode (investigation → prototype)

**Do NOT start B before profiling** the current CPU encoder (see Prerequisite).
The payoff of GPU work depends entirely on where single-thread time actually
goes, and that number does not exist yet.

### Pipeline, stage by stage (per frame)

Current CPU path lives in `src/encoder/` (`prores_encoder.c` = headers, slice
encode, RGBA→YUV, quant matrices; `prores_dct.c` = integer FDCT; `prores_vlc.c` =
entropy) and is dispatched from `src/wasm/bindings.c` (RGBA→YUV) / `lib/index.js`
(`encodePacketRgba`).

| Stage | GPU-friendly? | Notes |
|---|---|---|
| RGBA→YUV (BT.709, 10-bit) | ✅ ideal | per-pixel; also where the readback would be eliminated |
| 8×8 FDCT | ✅ ideal | per-block, ~independent; classic compute-shader workload |
| Quantization (truncation) | ✅ ideal | per-coefficient |
| Rate control (search q per slice) | ⚠️ maps to GPU as a cost matrix | evaluate bits(q) for all slices × all candidate q in one dispatch, then pick — potentially a *good* GPU fit, but needs a GPU bit-size estimator |
| Entropy coding (DC pred + AC run/level + Rice/Golomb, bit-packing) | ❌ hostile | serial, variable-length, data-dependent; the hard part |

### Two realistic shapes

1. **Hybrid (recommended first target):** GPU does color-convert + FDCT + quant
   (+ maybe bit-size estimation for rate control); CPU does the final entropy
   write. Biggest structural win is **eliminating the raw-frame readback** for
   canvas/WebGL/WebGPU sources — read the texture in-place, read back only the
   compressed result. Amdahl caveat: the CPU entropy stage becomes the floor, so
   the speedup is bounded by (time not in entropy). **Profile first to size this.**
2. **Full GPU (research, "v2"):** entropy-code slices in parallel in compute
   shaders (ProRes slices are independent → slice-level parallelism), then a
   prefix-sum/compaction pass to concatenate variable-length outputs and fix
   slice offsets. Doable but months of work; bit-packing in WGSL is painful and
   bit-exactness vs real ProRes decoders is a real risk.

### Hard constraints / gotchas

- **Bit-exactness matters.** Output must decode correctly in FFmpeg / QuickTime /
  Resolve. The existing CPU encoder is validated bit-identical to FFmpeg's
  `prores_ks` (see `test/mediabunny.test.mjs` and the encoder gotchas in
  `CLAUDE.md`). Any GPU path must reproduce: 32× DC gain FDCT, 0x4000 DC offset,
  integer-truncation quant, 10-bit internal depth, column-major 4444 chroma
  order, and the differential/RLE alpha path. Re-read `CLAUDE.md` §"ProRes
  Implementation Gotchas" before touching the math.
- **Alpha (4444)** uses per-pixel differential + Rice/Golomb RLE, *not* DCT — a
  separate GPU consideration from luma/chroma.
- **WebGPU availability & bundle size.** WebGPU ships in Chrome/Edge 113+,
  Safari 17+, Firefox (recent). A GPU path must fall back to the current WASM
  encoder where WebGPU is absent, and should not blow the library's ~41 KB
  gzipped size advantage (WGSL shaders are small; keep them out of the base
  bundle / lazy-load).
- **WebCodecs is not an option** — `VideoEncoder` doesn't support ProRes, and
  there's no in-browser hardware ProRes path. That absence is precisely why this
  library exists.

### Prerequisite: profile the CPU encoder first

Before committing to any GPU work, measure the single-thread time split across:
color-convert, FDCT, quant, rate-control q-search, entropy write. Cheapest way:
instrument `src/encoder/*.c` (or a native build under `test/`) with timers around
each stage, or run a sampling profiler on a native `test_native`-style harness.

Deliverable: a table of "% of frame time per stage." That decides:
- whether the hybrid GPU path is worth it (how much time is *not* entropy), and
- whether a cheaper CPU win exists first — e.g. a **fixed-q "fast mode"** that
  skips the rate-control search entirely (big potential single-thread win for
  real-time canvas capture where exact bitrate targeting isn't needed).

### Suggested order

1. Profile CPU encoder → stage breakdown. *(gates everything)*
2. If entropy is small: prototype GPU color+DCT+quant, keep CPU entropy, and — for
   canvas sources — wire it to read the GPU texture directly (no raw readback).
3. Measure end-to-end incl. eliminated readback vs today.
4. Only then consider full-GPU entropy coding.

---

## References

- `test/demos/index.html` — renderer comparison demo; WebGL readback ~679/769/883,
  WebGPU `captureFrame` ~1100–1145.
- `src/encoder/prores_encoder.c`, `prores_dct.c`, `prores_vlc.c` — the encode stages.
- `src/wasm/bindings.c`, `lib/index.js` (`encodePacketRgba`) — the entry points.
- `CLAUDE.md` — "ProRes Implementation Gotchas" (bit-exactness constraints).
- Benchmark suite (`benchmark/` on the `benchmark` branch) — current speed numbers
  and the size-vs-speed framing that motivated this.
