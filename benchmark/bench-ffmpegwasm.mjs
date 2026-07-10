/**
 * Benchmark ffmpeg.wasm (single-thread @ffmpeg/core) under Node.
 *
 * The core is a web/worker Emscripten build; we alias the few browser globals
 * it reaches for (`self`, `location`) and hand it the wasm bytes directly so
 * it runs headless. Raw frames are written into MEMFS once per run and we time
 * only `exec()` — the encode — not the MEMFS write.
 *
 * The multi-thread core (@ffmpeg/core-mt) uses pthreads via Web Workers and
 * cannot run under Node; it is benchmarked in the browser harness instead.
 */
import { createRequire } from 'node:module';
import { readFileSync } from 'node:fs';
import { join } from 'node:path';
import { PROFILES, loadFramesBuffer, makeRow, __dirname } from './bench-common.mjs';

if (typeof globalThis.self === 'undefined') globalThis.self = globalThis;
const require = createRequire(import.meta.url);

const CORE_BASE = join(__dirname, 'node_modules', '@ffmpeg/core', 'dist', 'umd');

async function freshCore() {
  globalThis.location = { href: `file://${CORE_BASE}/ffmpeg-core.js` };
  const createFFmpegCore = require(join(CORE_BASE, 'ffmpeg-core.js'));
  const wasmBinary = readFileSync(join(CORE_BASE, 'ffmpeg-core.wasm'));
  const core = await createFFmpegCore({ wasmBinary, locateFile: (p) => join(CORE_BASE, p) });
  core.setLogger(() => {});
  core.setTimeout(-1);
  return core;
}

function encodeArgs(meta, prof) {
  return [
    '-f', 'rawvideo', '-pix_fmt', 'rgba', '-s', `${meta.width}x${meta.height}`, '-r', String(meta.fps),
    '-i', 'in.rgba', '-frames:v', String(meta.frames),
    '-c:v', 'prores_ks', '-profile:v', prof.ffProfile, '-pix_fmt', prof.ffPixFmt, '-vendor', 'apl0', 'out.mov',
  ];
}

export async function runWasmSingle(meta, profileKey, { reps = 3 } = {}) {
  const prof = PROFILES[profileKey];
  const frames = loadFramesBuffer(meta);
  const input = new Uint8Array(frames.buffer, frames.byteOffset, frames.byteLength);
  const args = encodeArgs(meta, prof);

  const samples = [];
  let outBytes = null;
  // Fresh core per rep keeps MEMFS memory from accumulating across runs.
  for (let i = 0; i < reps + 1; i++) { // +1 warmup
    const core = await freshCore();
    core.FS.writeFile('in.rgba', input);
    const t0 = performance.now();
    core.exec(...args);
    const sec = (performance.now() - t0) / 1000;
    if (core.ret !== 0) throw new Error(`ffmpeg.wasm exec returned ${core.ret} for ${profileKey}`);
    outBytes = core.FS.readFile('out.mov').length;
    if (i > 0) samples.push(sec);
    core.FS.unlink('in.rgba');
    core.FS.unlink('out.mov');
  }

  return makeRow({
    contender: 'ffmpeg.wasm', environment: 'wasm', threads: '1t',
    profileKey, samples, outBytes, meta, note: '@ffmpeg/core single-thread',
  });
}
