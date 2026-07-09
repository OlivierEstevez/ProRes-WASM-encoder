/**
 * Benchmark our library: single-thread and multi-thread (worker pool).
 *
 *  - Single-thread: one `createProResEncoder`, timed over the encode loop.
 *  - Multi-thread:  N `worker_threads`, each with its own encoder, pulling
 *    frames off a shared atomic counter (see ours-worker.mjs). This is the
 *    parallel model the shipped `createProResEncoderPool` uses.
 *
 * The timed metric is pure ENCODE throughput via `encodePacketRgba` (the same
 * per-frame kernel the muxer path calls); muxing/finalize is measured
 * separately for the single-thread end-to-end number.
 */
import { Worker } from 'node:worker_threads';
import { writeFileSync } from 'node:fs';
import { join } from 'node:path';
import {
  PROFILES, loadFramesBuffer, loadFramesShared, timeReps, makeRow, RESULTS_DIR,
} from './bench-common.mjs';

/** Single-thread: pure encode throughput + a full end-to-end .mov for size. */
export async function runSingle(meta, profileKey, { reps = 5, save = false } = {}) {
  const { createProResEncoder } = await import('../dist/prores-encoder.esm.js');
  const prof = PROFILES[profileKey];
  const frames = loadFramesBuffer(meta);
  const bpf = meta.bytesPerFrame;

  const enc = await createProResEncoder();
  enc.initialize({ width: meta.width, height: meta.height, frameRate: meta.fps, profile: prof.ourProfile });

  const encodeAll = () => {
    for (let i = 0; i < meta.frames; i++) {
      enc.encodePacketRgba(frames.subarray(i * bpf, (i + 1) * bpf));
    }
  };
  const samples = await timeReps(encodeAll, { reps, warmup: 1 });
  enc.destroy();

  // Separate full pass through the muxer to get a real .mov (size / bitrate).
  const enc2 = await createProResEncoder();
  enc2.initialize({ width: meta.width, height: meta.height, frameRate: meta.fps, profile: prof.ourProfile });
  for (let i = 0; i < meta.frames; i++) enc2.addFrameRgba(frames.subarray(i * bpf, (i + 1) * bpf));
  const mov = enc2.finalize();
  enc2.destroy();
  if (save) writeFileSync(join(RESULTS_DIR, `ours_1t_${profileKey}.mov`), mov);

  return makeRow({
    contender: 'prores-wasm (ours)', environment: 'node', threads: '1t',
    profileKey, samples, outBytes: mov.length, meta,
  });
}

/** Multi-thread: N workers pulling frames off a shared counter. */
export async function runPool(meta, profileKey, { workers = 8, reps = 5 } = {}) {
  const prof = PROFILES[profileKey];
  const framesSAB = loadFramesShared(meta);
  const counterSAB = new SharedArrayBuffer(8);
  const counter = new Int32Array(counterSAB);

  const ws = [];
  const ready = [];
  for (let w = 0; w < workers; w++) {
    const worker = new Worker(new URL('./ours-worker.mjs', import.meta.url), {
      workerData: { framesSAB, counterSAB, meta, ourProfile: prof.ourProfile },
      execArgv: ['--no-warnings'],
    });
    let resolveReady;
    ready.push(new Promise((r) => { resolveReady = r; }));
    worker._doneResolve = null;
    worker.on('message', (m) => {
      if (m.type === 'ready') resolveReady();
      else if (m.type === 'done' && worker._doneResolve) worker._doneResolve(m);
      else if (m.type === 'stopped' && worker._stopResolve) worker._stopResolve();
    });
    worker.on('error', (e) => { throw e; });
    ws.push(worker);
  }
  await Promise.all(ready);

  const runOnce = () => {
    Atomics.store(counter, 0, 0);
    const dones = ws.map((worker) => new Promise((res) => { worker._doneResolve = res; }));
    const t0 = performance.now();
    for (const worker of ws) worker.postMessage({ type: 'go' });
    return Promise.all(dones).then(() => (performance.now() - t0) / 1000);
  };

  const samples = [];
  // 1 warmup rep + `reps` timed.
  for (let i = 0; i < reps + 1; i++) {
    const sec = await runOnce();
    if (i >= 1) samples.push(sec);
  }

  await Promise.all(ws.map((worker) => new Promise((res) => {
    worker._stopResolve = res;
    worker.postMessage({ type: 'stop' });
    setTimeout(res, 200);
  })));
  await Promise.all(ws.map((w) => w.terminate()));

  return makeRow({
    contender: 'prores-wasm (ours)', environment: 'node', threads: `${workers}t`,
    profileKey, samples, outBytes: null, meta,
    note: `${workers} worker_threads`,
  });
}
