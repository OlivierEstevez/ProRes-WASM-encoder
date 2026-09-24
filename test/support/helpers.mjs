/**
 * Shared helpers for the Node test suites (test/*.test.mjs).
 */
import { execFileSync } from 'node:child_process';
import { existsSync, mkdtempSync, rmSync, writeFileSync } from 'node:fs';
import { tmpdir } from 'node:os';
import { join } from 'node:path';

/** Exit with an actionable message when the built dist is missing. */
export function requireDist(file, suite) {
  if (!existsSync(new URL(`../../dist/${file}`, import.meta.url))) {
    console.error(
      `\n[${suite}] Built dist not found. Run \`npm run build\` first ` +
      '(or `npm run build:js` if dist/prores-encoder.core.wasm already exists).\n'
    );
    process.exit(1);
  }
}

function mulberry32(seed) {
  let a = seed >>> 0;
  return () => {
    a |= 0; a = (a + 0x6d2b79f5) | 0;
    let t = Math.imul(a ^ (a >>> 15), 1 | a);
    t = (t + Math.imul(t ^ (t >>> 7), 61 | t)) ^ t;
    return ((t ^ (t >>> 14)) >>> 0) / 4294967296;
  };
}

/** Deterministic, noisy RGBA frame. Alpha varies when withAlpha is set. */
export function makeFrame(width, height, frameIndex, withAlpha = false) {
  const rnd = mulberry32(0x1234 + frameIndex * 977);
  const buf = new Uint8Array(width * height * 4);
  for (let y = 0; y < height; y++) {
    for (let x = 0; x < width; x++) {
      const i = (y * width + x) * 4;
      buf[i]     = (x * 2 + frameIndex * 10 + (rnd() * 40)) & 0xff;
      buf[i + 1] = (y * 3 + frameIndex * 5 + (rnd() * 40)) & 0xff;
      buf[i + 2] = ((x + y) + frameIndex * 7 + (rnd() * 40)) & 0xff;
      buf[i + 3] = withAlpha ? ((x * 2 + y) & 0xff) : 255;
    }
  }
  return buf;
}

/** Concatenate byte arrays. */
export function concat(parts) {
  let total = 0;
  for (const p of parts) total += p.length;
  const out = new Uint8Array(total);
  let off = 0;
  for (const p of parts) { out.set(p, off); off += p.length; }
  return out;
}

/** Number of differing bytes between two equal-length arrays. */
export function countDiffs(a, b) {
  if (a.length !== b.length) return Infinity;
  let n = 0;
  for (let i = 0; i < a.length; i++) if (a[i] !== b[i]) n++;
  return n;
}

/**
 * Two encodes of the same frames are identical except for the wall-clock
 * creation/modification timestamps the muxer writes (mvhd/tkhd/mdhd):
 * at most 24 bytes. More than that means the bitstream differs.
 */
export const TIMESTAMP_BYTES = 24;

let ffprobeAvailable;
/** True when ffprobe is on PATH (optional, used for decode checks). */
export function hasFfprobe() {
  if (ffprobeAvailable === undefined) {
    try {
      execFileSync('ffprobe', ['-version'], { stdio: 'ignore' });
      ffprobeAvailable = true;
    } catch {
      ffprobeAvailable = false;
    }
  }
  return ffprobeAvailable;
}

/** Decode a .mov with ffprobe and return its video stream info. */
export function probeMov(bytes) {
  const dir = mkdtempSync(join(tmpdir(), 'prores-test-'));
  const file = join(dir, 'out.mov');
  try {
    writeFileSync(file, bytes);
    const json = execFileSync('ffprobe', [
      '-v', 'error', '-count_frames', '-select_streams', 'v:0',
      '-show_entries', 'stream=codec_name,profile,width,height,pix_fmt,nb_read_frames,r_frame_rate',
      '-of', 'json', file,
    ], { encoding: 'utf8' });
    return JSON.parse(json).streams[0];
  } finally {
    rmSync(dir, { recursive: true, force: true });
  }
}

/**
 * spawnWorker factory for createProResEncoderPool that runs each worker on
 * a real Node thread, exposing the Web Worker shape the pool expects.
 */
export async function nodeWorkerSpawner() {
  const { Worker } = await import('node:worker_threads');
  const url = new URL('./node-pool-worker.mjs', import.meta.url);
  return () => {
    const w = new Worker(url);
    const handle = {
      onmessage: null,
      onerror: null,
      postMessage: (m, t) => w.postMessage(m, t),
      terminate: () => w.terminate(),
    };
    w.on('message', (data) => handle.onmessage && handle.onmessage({ data }));
    w.on('error', (e) => handle.onerror && handle.onerror(e));
    return handle;
  };
}
