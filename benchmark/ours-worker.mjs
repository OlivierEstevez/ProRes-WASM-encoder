/**
 * Worker for the multi-threaded throughput benchmark of our library.
 *
 * Each worker owns one single-thread ProRes WASM encoder (the exact same
 * kernel the shipped `createProResEncoderPool` runs inside each Web Worker).
 * ProRes is intra-only, so frames are independent: workers pull frame indices
 * off a shared atomic counter and encode them in parallel. Frame pixels live
 * in a SharedArrayBuffer, so there is zero per-frame data transfer — this
 * measures the encoder's parallel ceiling, not messaging overhead.
 */
import { parentPort, workerData } from 'node:worker_threads';
import { createProResEncoder } from '../dist/prores-encoder.esm.js';

const { framesSAB, counterSAB, meta, ourProfile } = workerData;
const bpf = meta.bytesPerFrame;
const counter = new Int32Array(counterSAB); // [0] = next frame index to claim

const enc = await createProResEncoder();
enc.initialize({
  width: meta.width, height: meta.height,
  frameRate: meta.fps, profile: ourProfile,
});

const frameView = (i) => new Uint8Array(framesSAB, i * bpf, bpf);

// Warm the JIT / wasm tiering up before any timed run.
for (let i = 0; i < 3; i++) enc.encodePacketRgba(frameView(0));

parentPort.postMessage({ type: 'ready' });

parentPort.on('message', (msg) => {
  if (msg.type === 'go') {
    let n = 0;
    for (;;) {
      const i = Atomics.add(counter, 0, 1);
      if (i >= meta.frames) break;
      enc.encodePacketRgba(frameView(i));
      n++;
    }
    parentPort.postMessage({ type: 'done', count: n });
  } else if (msg.type === 'stop') {
    enc.destroy();
    parentPort.postMessage({ type: 'stopped' });
  }
});
