/**
 * Tests for the frame-parallel pool (`prores-wasm-encoder/parallel`), run
 * against the BUILT dist. Workers run on real Node threads (worker_threads)
 * through the pool's spawnWorker option; browsers use the Blob-URL Worker
 * the entry spawns itself. Needs `npm run build:js`.
 *
 * Core claim: pool output is packet-identical to the single-thread encoder.
 * Also covers backpressure, error propagation, cleanup when startup fails,
 * option validation and lifecycle errors.
 */
import { describe, it, after } from 'node:test';
import assert from 'node:assert';
import {
  requireDist, makeFrame, countDiffs, TIMESTAMP_BYTES, nodeWorkerSpawner,
} from './support/helpers.mjs';

requireDist('prores-encoder-parallel.mjs', 'pool.test');
requireDist('prores-worker.mjs', 'pool.test');
const { createProResEncoder, ProResProfile } = await import('../dist/prores-encoder.mjs');
const { createProResEncoderPool } = await import('../dist/prores-encoder-parallel.mjs');

const W = 96;   // 6 MBs: exercises the 4 + 2 remainder slices too
const H = 48;
const WORKERS = 3;

function singleThreadPackets(options, frames) {
  return createProResEncoder().then((enc) => {
    const packets = [];
    enc.initialize({ ...options, onFrameData: (c) => packets.push(c) });
    for (const f of frames) enc.addFrameRgba(f);
    enc.destroy();
    return packets;
  });
}

// Destroyed at the end even when a test fails, so live worker threads
// can't keep the test process running.
const pools = [];
after(() => Promise.all(pools.map((p) => p.destroy())));

async function createPool(options) {
  const pool = await createProResEncoderPool({
    workers: WORKERS,
    spawnWorker: await nodeWorkerSpawner(),
    ...options,
  });
  pools.push(pool);
  return pool;
}

/**
 * Fake worker transport for failure tests. `behaviour(msg, reply, handle)`
 * decides how the "worker" answers each message.
 */
function fakeSpawner(behaviour) {
  const spawned = [];
  const spawn = () => {
    const handle = {
      onmessage: null,
      onerror: null,
      terminated: false,
      postMessage(msg) {
        const reply = (data) => setTimeout(() => handle.onmessage && handle.onmessage({ data }), 0);
        behaviour(msg, reply, handle);
      },
      terminate() { handle.terminated = true; },
    };
    spawned.push(handle);
    return handle;
  };
  spawn.spawned = spawned;
  return spawn;
}

describe('bit-identical to the single-thread encoder', () => {
  for (const profile of [ProResProfile.PROXY, ProResProfile.HQ, ProResProfile.P4444]) {
    const alpha = profile >= ProResProfile.P4444;
    const frames = Array.from({ length: 10 }, (_, i) => makeFrame(W, H, i, alpha));
    const opts = { width: W, height: H, frameRate: 25, profile };

    it(`streams identical packets in order (profile ${profile})`, async () => {
      const expected = await singleThreadPackets(opts, frames);
      const got = [];
      const pool = await createPool({ ...opts, onFrameData: (c) => got.push(c) });
      for (const f of frames) await pool.addFrameRgba(f);
      const { header, moov } = await pool.finalizeStreaming();
      await pool.destroy();

      assert.strictEqual(got.length, expected.length);
      for (let i = 0; i < got.length; i++) {
        assert.deepStrictEqual(got[i], expected[i], `packet ${i} differs`);
      }
      assert.ok(header.length > 0 && moov.length > 0);
    });

    it(`finalize and finalizeToBlob match the single-thread file (profile ${profile})`, async () => {
      const enc = await createProResEncoder();
      enc.initialize(opts);
      for (const f of frames) enc.addFrameRgba(f);
      const expected = enc.finalize();
      enc.destroy();

      const poolA = await createPool(opts);
      for (const f of frames) await poolA.addFrameRgba(f);
      const a = await poolA.finalize();
      assert.strictEqual(poolA.frameCount, frames.length);
      await poolA.destroy();

      const poolB = await createPool(opts);
      for (const f of frames) await poolB.addFrameRgba(f);
      const b = new Uint8Array(await (await poolB.finalizeToBlob()).arrayBuffer());
      await poolB.destroy();

      assert.ok(a.buffer instanceof ArrayBuffer);
      assert.ok(countDiffs(a, expected) <= TIMESTAMP_BYTES, 'pool finalize() differs');
      assert.ok(countDiffs(b, expected) <= TIMESTAMP_BYTES, 'pool finalizeToBlob() differs');
    });
  }
});

describe('backpressure and flush', () => {
  it('keeps at most 2x workers frames in flight', async () => {
    const frames = Array.from({ length: 24 }, (_, i) => makeFrame(W, H, i));
    let delivered = 0;
    let submitted = 0;
    let maxInFlight = 0;
    const pool = await createPool({ width: W, height: H, onFrameData: () => { delivered++; } });
    for (const f of frames) {
      await pool.addFrameRgba(f);
      submitted++;
      maxInFlight = Math.max(maxInFlight, submitted - delivered);
    }
    await pool.flush();
    assert.strictEqual(delivered, frames.length);
    assert.ok(maxInFlight <= WORKERS * 2, `in flight reached ${maxInFlight}`);

    // flush() doesn't finalize: more frames can follow.
    await pool.addFrameRgba(frames[0]);
    await pool.flush();
    assert.strictEqual(delivered, frames.length + 1);
    await pool.destroy();
  });

  it('reads canvases through the shared canvas helper', async () => {
    const pixels = makeFrame(W, H, 3);
    const canvas = {
      width: W, height: H,
      getContext: () => ({ getImageData: () => ({ data: new Uint8ClampedArray(pixels) }) }),
    };
    const pool = await createPool({ width: W, height: H });
    await pool.addFrameFromCanvas(canvas);
    await pool.addFrameFromImageData({ width: W, height: H, data: pixels });
    await assert.rejects(pool.addFrameFromCanvas({ ...canvas, width: W + 16 }), /backing store/);
    await assert.rejects(pool.addFrameFromImageData(null), /ImageData null/);
    const mov = await pool.finalize();
    assert.strictEqual(pool.frameCount, 2);
    assert.ok(mov.length > 0);
    await pool.destroy();
  });
});

describe('errors and cleanup', () => {
  it('surfaces a worker encode error on the next call', async () => {
    const spawnWorker = fakeSpawner((msg, reply) => {
      if (msg.type === 'init') reply({ type: 'ready' });
      else if (msg.type === 'encode') reply({ type: 'error', frameIndex: msg.frameIndex, error: 'boom' });
      else if (msg.type === 'destroy') reply({ type: 'destroyed' });
    });
    const pool = await createProResEncoderPool({ width: W, height: H, workers: 2, spawnWorker });
    await pool.addFrameRgba(makeFrame(W, H, 0));
    await assert.rejects(pool.finalize(), /boom/);
    await assert.rejects(pool.addFrameRgba(makeFrame(W, H, 1)), /boom|finalize/);
    await pool.destroy();
    assert.ok(spawnWorker.spawned.every((w) => w.terminated));
  });

  it('terminates started workers when startup fails, with a CSP hint', async () => {
    let n = 0;
    const spawnWorker = fakeSpawner((msg, reply, handle) => {
      if (msg.type === 'init') {
        // First worker starts fine, the second fails to load (e.g. CSP).
        if (n++ === 0) reply({ type: 'ready' });
        else setTimeout(() => handle.onerror({ message: 'blocked', preventDefault() {} }), 0);
      } else if (msg.type === 'destroy') {
        reply({ type: 'destroyed' });
      }
    });
    await assert.rejects(
      createProResEncoderPool({ width: W, height: H, workers: 2, spawnWorker }),
      /worker error: blocked.*worker-src blob:/
    );
    assert.strictEqual(spawnWorker.spawned.length, 2);
    assert.ok(spawnWorker.spawned.every((w) => w.terminated), 'a worker leaked');
  });

  it('rejects pending calls instead of hanging when destroyed mid-flight', async () => {
    // Workers that start but never finish a frame.
    const spawnWorker = fakeSpawner((msg, reply) => {
      if (msg.type === 'init') reply({ type: 'ready' });
      else if (msg.type === 'destroy') reply({ type: 'destroyed' });
    });
    const pool = await createProResEncoderPool({ width: W, height: H, workers: 1, spawnWorker });
    await pool.addFrameRgba(makeFrame(W, H, 0));
    await pool.addFrameRgba(makeFrame(W, H, 1));
    // Attach the assertions first: the calls reject while destroy() runs.
    const blocked = assert.rejects(pool.addFrameRgba(makeFrame(W, H, 2)), /destroyed/); // over the limit
    const flushing = assert.rejects(pool.flush(), /destroyed/);
    await pool.destroy();
    await blocked;
    await flushing;
  });

  it('rejects when spawning a worker throws', async () => {
    const spawnWorker = () => { throw new Error('SecurityError'); };
    await assert.rejects(
      createProResEncoderPool({ width: W, height: H, workers: 1, spawnWorker }),
      /SecurityError/
    );
  });
});

describe('validation and lifecycle', () => {
  for (const [opts, re] of [
    [{ width: W, height: H, workers: 0 }, /workers must be a positive integer.*createProResEncoder/],
    [{ width: W, height: H, workers: 2.5 }, /workers must be a positive integer/],
    [{ width: NaN, height: H }, /positive integers/],
    [{ width: W, height: H, profile: 7 }, /unknown profile/],
  ]) {
    it(`rejects ${JSON.stringify(opts)} before spawning workers`, async () => {
      const spawnWorker = fakeSpawner(() => {});
      await assert.rejects(createProResEncoderPool({ ...opts, spawnWorker }), re);
      assert.strictEqual(spawnWorker.spawned.length, 0);
    });
  }

  it('rejects frames and a second finalize after finalize', async () => {
    const pool = await createPool({ width: W, height: H });
    await pool.addFrameRgba(makeFrame(W, H, 0));
    await pool.finalize();
    await assert.rejects(pool.addFrameRgba(makeFrame(W, H, 1)), /after finalize/);
    await assert.rejects(pool.finalize(), /already finalized/);
    await assert.rejects(pool.finalizeToBlob(), /already finalized/);
    await pool.destroy();
  });

  it('rejects any use after destroy', async () => {
    const pool = await createPool({ width: W, height: H });
    await pool.addFrameRgba(makeFrame(W, H, 0));
    await pool.destroy();
    await pool.destroy(); // idempotent
    await assert.rejects(pool.addFrameRgba(makeFrame(W, H, 1)), /destroyed/);
    await assert.rejects(pool.finalize(), /destroyed/);
    assert.throws(() => pool.finalizeHeaders(), /destroyed/);
  });

  it('rejects finalize()/finalizeToBlob() in streaming mode', async () => {
    const pool = await createPool({ width: W, height: H, onFrameData: () => {} });
    await pool.addFrameRgba(makeFrame(W, H, 0));
    await assert.rejects(pool.finalize(), /streaming mode/);
    await assert.rejects(pool.finalizeToBlob(), /streaming mode/);
    const { header, moov } = await pool.finalizeStreaming();
    assert.ok(header.length > 0 && moov.length > 0);
    await pool.destroy();
  });
});
