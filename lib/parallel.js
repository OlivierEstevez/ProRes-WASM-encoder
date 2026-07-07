/**
 * prores-wasm-encoder/parallel — frame-parallel encoding entry point.
 *
 * Kept as a separate entry so the base library stays small: importing only
 * 'prores-wasm-encoder' never pulls in the worker machinery. This entry
 * embeds the worker source (WITHOUT a second copy of the WASM binary) and
 * shares one compiled WebAssembly.Module with every worker.
 */
import { compileWasmModule, createModuleFrom } from './wasm-loader.js';
import { ProResEncoderPool } from './pool.js';
// Worker source (Emscripten glue + encode loop, no wasm payload), injected
// at build time by the rollup inline-worker plugin. Spawned from a Blob
// URL — no bundler or server configuration needed.
import proresWorkerCode from 'prores:worker';

export { ProResEncoderPool } from './pool.js';
export { ProResProfile, ProfileNames } from './index.js';

/**
 * Create a frame-parallel ProRes encoder pool.
 *
 * ProRes is intra-only, so frames are independent and encode in parallel
 * across Web Workers with byte-identical output to the single-thread
 * encoder. The WASM binary is compiled once and the compiled module is
 * shared with every worker (structured clone), so workers add no extra
 * compiles or copies.
 *
 * Requires Web Workers with module support (Chrome 80+, Safari 15+,
 * Firefox 114+). For environments without workers, use the base entry's
 * createProResEncoder().
 *
 * @param {Object} options
 * @param {number} options.width - Frame width in pixels
 * @param {number} options.height - Frame height in pixels
 * @param {number} [options.frameRate=30]
 * @param {number} [options.frameRateNum]
 * @param {number} [options.frameRateDen]
 * @param {number} [options.profile=3] - ProRes profile (see ProResProfile)
 * @param {string} [options.range="limited"]
 * @param {number} [options.workers] - Worker count (default: min(hardwareConcurrency, 8))
 * @param {function} [options.onFrameData] - Streaming mode (see ProResEncoder)
 * @returns {Promise<ProResEncoderPool>}
 */
export async function createProResEncoderPool(options = {}) {
  if (typeof Worker === 'undefined') {
    throw new Error(
      'Web Workers are not available in this environment; ' +
      'use createProResEncoder() from "prores-wasm-encoder" instead'
    );
  }

  // Compile the WASM once; every worker instantiates from this module.
  const wasmModule = await compileWasmModule();

  // Main-thread module instance, used only for muxing.
  const module = await createModuleFrom(wasmModule);

  let spawnWorker = options.spawnWorker;
  let blobUrl = null;
  if (!spawnWorker) {
    const blob = new Blob([proresWorkerCode], { type: 'text/javascript' });
    blobUrl = URL.createObjectURL(blob);
    spawnWorker = () => new Worker(blobUrl, { type: 'module' });
  }

  try {
    const pool = await ProResEncoderPool.create({ ...options, module, wasmModule, spawnWorker });
    return pool;
  } finally {
    // Every worker has been constructed and reported ready by now, so the
    // Blob URL has been fetched and is safe to release.
    if (blobUrl) URL.revokeObjectURL(blobUrl);
  }
}

export default createProResEncoderPool;
