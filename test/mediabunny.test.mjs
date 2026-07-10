/**
 * Regression tests for the MediaBunny integration (`prores-wasm-encoder/mediabunny`).
 * Uses Node's built-in test runner: `node --test test/*.test.mjs` (or `npm test`).
 *
 * Runs against the BUILT dist artifacts (what users import), so it needs
 * `npm run build:js` (and `npm install` for the mediabunny devDependency).
 * Node has no Web Worker global, so the adapter uses the single-thread path;
 * we also force workers:0 for determinism.
 *
 * Part 1 — Bit-identical muxing. The core claim: a ProRes .mov produced by
 * MediaBunny (muxing through our registered custom encoder) carries frame
 * samples byte-identical to the ones our own muxer writes for the same RGBA
 * input. Containers differ (muxers, timestamps), so we compare at the packet
 * level: demux both files and diff the packets.
 *
 * Part 2 — Alpha extraction. The adapter pulls RGBA out of a VideoSample via
 * copyTo (lossless) with a 2D-canvas fallback. 2D canvases are premultiplied,
 * so the fallback loses alpha precision on the 4444 profiles. These tests pin
 * down that the fast path is byte-exact for semi-transparent alpha, and that
 * the fallback is no longer silent — it warns on alpha profiles and throws a
 * clear error when no canvas exists.
 */
import { describe, it, before } from 'node:test';
import assert from 'node:assert';
import { existsSync } from 'node:fs';
import {
  Output, BufferTarget, MovOutputFormat, VideoSampleSource, VideoSample,
  Input, ALL_FORMATS, BufferSource, EncodedPacketSink,
} from 'mediabunny';

// This suite exercises the built artifacts. Fail loudly with an actionable
// message instead of a confusing module-resolution error when they're absent.
if (!existsSync(new URL('../dist/prores-encoder-mediabunny.esm.js', import.meta.url))) {
  console.error(
    '\n[mediabunny.test] Built dist not found. Run `npm run build` first ' +
    '(or `npm run build:js` if dist/prores-encoder.core.wasm already exists).\n'
  );
  process.exit(1);
}
const { createProResEncoder } = await import('../dist/prores-encoder.esm.js');
const { registerProResEncoder, ProResVideoEncoder } =
  await import('../dist/prores-encoder-mediabunny.esm.js');

const WIDTH = 128;
const HEIGHT = 80;
const FRAMES = 6;
const FPS = 30;

const PROFILES = [
  { fourcc: 'apco', name: 'ProRes 422 Proxy', alpha: false, enum: 0 },
  { fourcc: 'apcs', name: 'ProRes 422 LT', alpha: false, enum: 1 },
  { fourcc: 'apcn', name: 'ProRes 422', alpha: false, enum: 2 },
  { fourcc: 'apch', name: 'ProRes 422 HQ', alpha: false, enum: 3 },
  { fourcc: 'ap4h', name: 'ProRes 4444', alpha: true, enum: 4 },
  { fourcc: 'ap4x', name: 'ProRes 4444 XQ', alpha: true, enum: 5 },
];

function mulberry32(seed) {
  let a = seed >>> 0;
  return () => {
    a |= 0; a = (a + 0x6d2b79f5) | 0;
    let t = Math.imul(a ^ (a >>> 15), 1 | a);
    t = (t + Math.imul(t ^ (t >>> 7), 61 | t)) ^ t;
    return ((t ^ (t >>> 14)) >>> 0) / 4294967296;
  };
}

/** Deterministic RGBA frames. Alpha profiles get varying (incl. very low)
 *  alpha so the alpha path is genuinely exercised. */
function makeFrame(frameIndex, withAlpha) {
  const rnd = mulberry32(0x1234 + frameIndex * 977);
  const buf = new Uint8Array(WIDTH * HEIGHT * 4);
  for (let y = 0; y < HEIGHT; y++) {
    for (let x = 0; x < WIDTH; x++) {
      const i = (y * WIDTH + x) * 4;
      buf[i]     = (x * 2 + frameIndex * 10 + (rnd() * 40)) & 0xff;
      buf[i + 1] = (y * 3 + frameIndex * 5 + (rnd() * 40)) & 0xff;
      buf[i + 2] = ((x + y) + frameIndex * 7 + (rnd() * 40)) & 0xff;
      buf[i + 3] = withAlpha ? ((x * 2 + y) & 0xff) : 255; // straight alpha
    }
  }
  return buf;
}

/** Path A: our own encoder + our own muxer → .mov bytes. */
async function encodeWithOwnMuxer(profileEnum, frames) {
  const enc = await createProResEncoder();
  enc.initialize({ width: WIDTH, height: HEIGHT, frameRate: FPS, profile: profileEnum });
  for (const f of frames) enc.addFrameRgba(f);
  const mov = enc.finalize();
  enc.destroy();
  return mov;
}

/** Path B: the full MediaBunny pipeline → .mov bytes. */
async function encodeWithMediaBunny(fourcc, frames) {
  const output = new Output({ format: new MovOutputFormat(), target: new BufferTarget() });
  const source = new VideoSampleSource({ codec: 'prores', fullCodecString: fourcc, bitrate: 100_000_000 });
  output.addVideoTrack(source, { frameRate: FPS });
  await output.start();
  for (let i = 0; i < frames.length; i++) {
    const vs = new VideoSample(frames[i], {
      format: 'RGBA', codedWidth: WIDTH, codedHeight: HEIGHT,
      timestamp: i / FPS, duration: 1 / FPS,
    });
    await source.add(vs);
    vs.close();
  }
  await output.finalize();
  return new Uint8Array(output.target.buffer);
}

/** Demux a .mov with MediaBunny → array of encoded packet byte arrays. */
async function extractPackets(movBytes) {
  const input = new Input({ formats: ALL_FORMATS, source: new BufferSource(movBytes) });
  const track = await input.getPrimaryVideoTrack();
  assert(track, 'no video track found in muxed output');
  const sink = new EncodedPacketSink(track);
  const packets = [];
  for await (const p of sink.packets()) packets.push(p.data);
  return packets;
}

function firstDiff(a, b) {
  if (a.length !== b.length) return `length ${a.length} vs ${b.length}`;
  for (let i = 0; i < a.length; i++) if (a[i] !== b[i]) return `byte ${i}: ${a[i]} vs ${b[i]}`;
  return null;
}

/** Bare adapter instance with only the fields _extractRgba reads (mirrors
 *  what init() sets), for unit-testing extraction in isolation. */
function makeAdapter(fourcc) {
  const enc = new ProResVideoEncoder();
  enc.config = { codec: fourcc, width: WIDTH, height: HEIGHT };
  enc._hasAlpha = fourcc === 'ap4h' || fourcc === 'ap4x';
  enc._warnedAlphaFallback = false;
  enc._pixelBuf = null;
  enc._canvasCtx = null;
  return enc;
}

describe('MediaBunny integration — bit-identical muxing', () => {
  before(() => registerProResEncoder({ workers: 0 })); // deterministic single-thread

  for (const prof of PROFILES) {
    const label = `${prof.fourcc} (${prof.name})${prof.alpha ? ' [alpha]' : ''}` +
      ' is byte-identical to our own muxer';
    it(label, async () => {
      const frames = Array.from({ length: FRAMES }, (_, i) => makeFrame(i, prof.alpha));
      const movOwn = await encodeWithOwnMuxer(prof.enum, frames);
      const movMB = await encodeWithMediaBunny(prof.fourcc, frames);
      const pktsOwn = await extractPackets(movOwn);
      const pktsMB = await extractPackets(movMB);

      assert.strictEqual(pktsOwn.length, FRAMES, 'own-muxer packet count');
      assert.strictEqual(pktsMB.length, FRAMES, 'mediabunny packet count');
      for (let i = 0; i < FRAMES; i++) {
        assert.strictEqual(firstDiff(pktsOwn[i], pktsMB[i]), null, `frame ${i} differs`);
      }
    });
  }
});

describe('MediaBunny integration — alpha extraction', () => {
  it('copyTo fast path is byte-exact for semi-transparent alpha', async () => {
    const enc = makeAdapter('ap4h');
    const src = new Uint8Array(WIDTH * HEIGHT * 4);
    for (let i = 0; i < WIDTH * HEIGHT; i++) {
      src[i * 4] = 200; src[i * 4 + 1] = 100; src[i * 4 + 2] = 50;
      src[i * 4 + 3] = (i % 4 === 0) ? 8 : (i % 3 === 0) ? 128 : 255; // incl. very low alpha
    }
    const vs = new VideoSample(src, { format: 'RGBA', codedWidth: WIDTH, codedHeight: HEIGHT, timestamp: 0 });
    const out = await enc._extractRgba(vs);
    vs.close();
    assert.strictEqual(firstDiff(src, out), null);
  });

  it('fallback warns exactly once on an alpha profile and still returns data', async () => {
    const enc = makeAdapter('ap4h');
    const marker = new Uint8ClampedArray(WIDTH * HEIGHT * 4).fill(123);
    const fakeSample = {
      copyTo() { throw new Error('unsupported sample'); },
      draw(ctx) { ctx._data.set(marker); },
    };
    class FakeCtx { constructor(w, h) { this._data = new Uint8ClampedArray(w * h * 4); } clearRect() {} getImageData() { return { data: this._data }; } }
    class FakeCanvas { constructor(w, h) { this._ctx = new FakeCtx(w, h); } getContext() { return this._ctx; } }

    globalThis.OffscreenCanvas = FakeCanvas;
    const warnings = [];
    const origWarn = console.warn;
    console.warn = (m) => warnings.push(String(m));
    try {
      const out1 = await enc._extractRgba(fakeSample);
      const out2 = await enc._extractRgba(fakeSample); // second call must not warn again
      assert.strictEqual(warnings.length, 1, 'should warn exactly once');
      assert.match(warnings[0], /premultiplied|precision/i, 'warning explains the alpha risk');
      assert.strictEqual(firstDiff(marker, out1), null, 'first call returns canvas data');
      assert.strictEqual(firstDiff(marker, out2), null, 'second call returns canvas data');
    } finally {
      console.warn = origWarn;
      delete globalThis.OffscreenCanvas;
    }
  });

  it('no-canvas fallback throws a clear, actionable error', async () => {
    const enc = makeAdapter('ap4h');
    const fakeSample = { copyTo() { throw new Error('unsupported'); }, draw() {} };
    assert.ok(typeof OffscreenCanvas === 'undefined' && typeof document === 'undefined');

    const origWarn = console.warn;
    console.warn = () => {}; // the alpha-fallback warn fires first; not under test here
    await assert.rejects(
      () => enc._extractRgba(fakeSample),
      (e) => /cannot read RGBA/i.test(e.message) && !/is not defined/.test(e.message),
    );
    console.warn = origWarn;
  });
});
