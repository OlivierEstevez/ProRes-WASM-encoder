/**
 * Tests for the single-thread encoder (`prores-wasm-encoder`), run against
 * the BUILT dist (what users import). Needs `npm run build:js`.
 *
 * Covers the three output paths (finalize, finalizeToBlob, streaming via
 * onFrameData + finalizeHeaders), every profile, awkward frame sizes and
 * rates, option validation, lifecycle errors, and canvas readback
 * (2D, WebGL-style and size mismatches). When ffprobe is on PATH, outputs
 * are also decoded to check frame count, size and pixel format.
 */
import { describe, it, afterEach } from 'node:test';
import assert from 'node:assert';
import {
  requireDist, makeFrame, concat, countDiffs, TIMESTAMP_BYTES, hasFfprobe, probeMov,
} from './support/helpers.mjs';

requireDist('prores-encoder.mjs', 'encoder.test');
const {
  createProResEncoder, ProResProfile, ProfileNames, movToBlob,
} = await import('../dist/prores-encoder.mjs');

const W = 64;
const H = 48;

async function encode(options, frames) {
  const enc = await createProResEncoder();
  enc.initialize(options);
  for (const f of frames) enc.addFrameRgba(f);
  const mov = enc.finalize();
  enc.destroy();
  return mov;
}

describe('output paths', () => {
  const frames = [0, 1, 2, 3].map((i) => makeFrame(W, H, i));

  it('finalize, finalizeToBlob and streaming produce the same file', async () => {
    const opts = { width: W, height: H, frameRate: 30, profile: ProResProfile.HQ };
    const a = await encode(opts, frames);

    const encB = await createProResEncoder();
    encB.initialize(opts);
    for (const f of frames) encB.addFrameRgba(f);
    const blob = encB.finalizeToBlob();
    assert.strictEqual(blob.type, 'video/quicktime');
    const b = new Uint8Array(await blob.arrayBuffer());
    encB.destroy();

    const chunks = [];
    const encC = await createProResEncoder();
    encC.initialize({ ...opts, onFrameData: (c) => chunks.push(c) });
    for (const f of frames) encC.addFrameRgba(f);
    const { header, moov } = encC.finalizeHeaders();
    const c = concat([header, ...chunks, moov]);
    encC.destroy();

    assert.strictEqual(chunks.length, frames.length);
    assert.ok(countDiffs(a, b) <= TIMESTAMP_BYTES, 'finalizeToBlob differs from finalize');
    assert.ok(countDiffs(a, c) <= TIMESTAMP_BYTES, 'streaming differs from finalize');
  });

  it('returns bytes backed by a plain ArrayBuffer', async () => {
    const mov = await encode({ width: W, height: H }, frames.slice(0, 1));
    assert.ok(mov instanceof Uint8Array);
    assert.ok(mov.buffer instanceof ArrayBuffer);
    assert.strictEqual(movToBlob(mov).size, mov.length);
  });

  it('accepts ImageData-shaped input', async () => {
    const enc = await createProResEncoder();
    enc.initialize({ width: W, height: H });
    enc.addFrameFromImageData({ width: W, height: H, data: new Uint8ClampedArray(frames[0]) });
    assert.strictEqual(enc.frameCount, 1);
    assert.throws(
      () => enc.addFrameFromImageData({ width: W + 1, height: H, data: frames[0] }),
      /don't match encoder/
    );
    enc.destroy();
  });
});

describe('profiles, sizes and frame rates', () => {
  for (const [key, profile] of Object.entries(ProResProfile)) {
    it(`encodes ${ProfileNames[profile]} (${key})`, async (t) => {
      const alpha = profile >= ProResProfile.P4444;
      const mov = await encode(
        { width: W, height: H, profile },
        [0, 1, 2].map((i) => makeFrame(W, H, i, alpha))
      );
      assert.ok(mov.length > 1000);
      if (!hasFfprobe()) return t.skip('ffprobe not on PATH');
      const s = probeMov(mov);
      assert.strictEqual(s.codec_name, 'prores');
      assert.strictEqual(Number(s.nb_read_frames), 3);
      assert.match(s.pix_fmt, alpha ? /^yuva444p/ : /^yuv422p10/);
    });
  }

  // Widths whose MB count isn't a multiple of 8 need power-of-two remainder
  // slices (48px = 3 MBs = 2 + 1, 720px = 45 MBs = 5x8 + 4 + 1).
  for (const [w, h] of [
    [1, 1], [2, 2], [17, 9], [33, 65], [48, 16], [130, 34], [200, 48], [720, 64], [1080, 1080],
  ]) {
    it(`encodes ${w}x${h}`, async (t) => {
      const mov = await encode({ width: w, height: h }, [makeFrame(w, h, 0), makeFrame(w, h, 1)]);
      if (!hasFfprobe()) return t.skip('ffprobe not on PATH');
      const s = probeMov(mov);
      assert.strictEqual(s.width, w);
      assert.strictEqual(s.height, h);
      assert.strictEqual(Number(s.nb_read_frames), 2);
    });
  }

  for (const [opts, expected] of [
    [{ frameRate: 23.976 }, '24000/1001'],
    [{ frameRate: 29.97 }, '30000/1001'],
    [{ frameRate: 60 }, '60/1'],
    [{ frameRateNum: 25, frameRateDen: 1 }, '25/1'],
  ]) {
    it(`writes frame rate ${expected}`, async (t) => {
      const mov = await encode({ width: W, height: H, ...opts }, [makeFrame(W, H, 0)]);
      if (!hasFfprobe()) return t.skip('ffprobe not on PATH');
      assert.strictEqual(probeMov(mov).r_frame_rate, expected);
    });
  }
});

describe('option validation', () => {
  const bad = [
    [{}, /width and height must be positive integers/],
    [{ width: NaN, height: 10 }, /positive integers/],
    [{ width: 10.5, height: 10 }, /positive integers/],
    [{ width: -2, height: 10 }, /positive integers/],
    [{ width: '64', height: 48 }, /positive integers/],
    [{ width: 20000, height: 10 }, /too large/],
    [{ width: 10, height: 10, profile: 9 }, /unknown profile/],
    [{ width: 10, height: 10, profile: 'HQ' }, /unknown profile/],
    [{ width: 10, height: 10, range: 'tv' }, /range must be/],
    [{ width: 10, height: 10, frameRate: 0 }, /frameRate must be a positive number/],
    [{ width: 10, height: 10, frameRate: Infinity }, /frameRate must be a positive number/],
    [{ width: 10, height: 10, frameRateNum: 24 }, /must be passed together/],
    [{ width: 10, height: 10, frameRateNum: 24, frameRateDen: 0 }, /positive integers/],
    [{ width: 10, height: 10, onFrameData: 'yes' }, /onFrameData must be a function/],
  ];
  for (const [opts, re] of bad) {
    it(`rejects ${JSON.stringify(opts)}`, async () => {
      const enc = await createProResEncoder();
      assert.throws(() => enc.initialize(opts), re);
      assert.strictEqual(enc.initialized, false);
    });
  }

  it('rejects a missing options object', async () => {
    const enc = await createProResEncoder();
    assert.throws(() => enc.initialize(), /options object/);
  });
});

describe('lifecycle', () => {
  const frame = makeFrame(W, H, 0);

  it('rejects frames before initialize', async () => {
    const enc = await createProResEncoder();
    assert.throws(() => enc.addFrameRgba(frame), /not initialized/);
  });

  it('rejects a wrong-sized frame', async () => {
    const enc = await createProResEncoder();
    enc.initialize({ width: W, height: H });
    assert.throws(() => enc.addFrameRgba(new Uint8Array(10)), /Invalid RGBA data size/);
    enc.destroy();
  });

  it('rejects finalize with no frames', async () => {
    const enc = await createProResEncoder();
    enc.initialize({ width: W, height: H });
    assert.throws(() => enc.finalize(), /No frames encoded/);
    enc.destroy();
  });

  it('rejects frames and a second finalize after finalize', async () => {
    const enc = await createProResEncoder();
    enc.initialize({ width: W, height: H });
    enc.addFrameRgba(frame);
    enc.finalize();
    assert.throws(() => enc.addFrameRgba(frame), /after finalize/);
    assert.throws(() => enc.finalize(), /already finalized/);
    assert.throws(() => enc.finalizeToBlob(), /already finalized/);
    enc.destroy();
  });

  it('rejects any use after destroy', async () => {
    const enc = await createProResEncoder();
    enc.initialize({ width: W, height: H });
    enc.addFrameRgba(frame);
    enc.destroy();
    enc.destroy(); // idempotent
    assert.throws(() => enc.addFrameRgba(frame), /destroyed/);
    assert.throws(() => enc.finalize(), /destroyed/);
    assert.throws(() => enc.initialize({ width: W, height: H }), /destroyed/);
  });

  it('rejects finalize()/finalizeToBlob() in streaming mode', async () => {
    const enc = await createProResEncoder();
    enc.initialize({ width: W, height: H, onFrameData: () => {} });
    enc.addFrameRgba(frame);
    assert.throws(() => enc.finalize(), /streaming mode/);
    assert.throws(() => enc.finalizeToBlob(), /streaming mode/);
    enc.destroy();
  });
});

describe('addFrameFromCanvas', () => {
  const pixels = makeFrame(W, H, 7);
  const originalOffscreen = globalThis.OffscreenCanvas;
  afterEach(() => { globalThis.OffscreenCanvas = originalOffscreen; });

  function canvas2d(width = W, height = H) {
    return {
      width, height,
      getContext: (type) => (type === '2d'
        ? { getImageData: () => ({ data: new Uint8ClampedArray(pixels) }) }
        : null),
    };
  }

  /** A canvas holding a WebGL context: getContext('2d') returns null. */
  function canvasWebgl() {
    return { width: W, height: H, getContext: () => null, _pixels: pixels };
  }

  /** Minimal OffscreenCanvas that "draws" by copying the source's pixels. */
  class FakeOffscreenCanvas {
    constructor(width, height) {
      this.width = width;
      this.height = height;
      FakeOffscreenCanvas.created++;
      let drawn = null;
      this._ctx = {
        clearRect() { drawn = null; },
        drawImage(src) { drawn = src._pixels; },
        getImageData: () => ({ data: new Uint8ClampedArray(drawn) }),
      };
    }
    getContext(type, opts) {
      assert.strictEqual(type, '2d');
      assert.deepStrictEqual(opts, { willReadFrequently: true });
      return this._ctx;
    }
  }
  FakeOffscreenCanvas.created = 0;

  it('reads a 2D canvas', async () => {
    const enc = await createProResEncoder();
    enc.initialize({ width: W, height: H });
    enc.addFrameFromCanvas(canvas2d());
    const viaCanvas = enc.finalize();
    enc.destroy();
    const direct = await encode({ width: W, height: H }, [pixels]);
    assert.ok(countDiffs(viaCanvas, direct) <= TIMESTAMP_BYTES);
  });

  it('reads a WebGL canvas through one reused scratch 2D canvas', async () => {
    globalThis.OffscreenCanvas = FakeOffscreenCanvas;
    FakeOffscreenCanvas.created = 0;
    const enc = await createProResEncoder();
    enc.initialize({ width: W, height: H });
    enc.addFrameFromCanvas(canvasWebgl());
    enc.addFrameFromCanvas(canvasWebgl());
    const viaCanvas = enc.finalize();
    enc.destroy();
    assert.strictEqual(FakeOffscreenCanvas.created, 1);
    const direct = await encode({ width: W, height: H }, [pixels, pixels]);
    assert.ok(countDiffs(viaCanvas, direct) <= TIMESTAMP_BYTES);
  });

  it('throws a clear error for a WebGL canvas without OffscreenCanvas or document', async () => {
    globalThis.OffscreenCanvas = undefined;
    const enc = await createProResEncoder();
    enc.initialize({ width: W, height: H });
    assert.throws(() => enc.addFrameFromCanvas(canvasWebgl()), /non-2D canvas/);
    enc.destroy();
  });

  it('rejects a canvas whose size differs from the encoder', async () => {
    const enc = await createProResEncoder();
    enc.initialize({ width: W, height: H });
    assert.throws(() => enc.addFrameFromCanvas(canvas2d(W * 2, H * 2)), /backing store/);
    enc.destroy();
  });

  it('rejects something that is not a canvas', async () => {
    const enc = await createProResEncoder();
    enc.initialize({ width: W, height: H });
    assert.throws(() => enc.addFrameFromCanvas({}), /expected an HTMLCanvasElement/);
    enc.destroy();
  });
});
