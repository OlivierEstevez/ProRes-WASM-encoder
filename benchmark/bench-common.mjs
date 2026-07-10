/**
 * Shared benchmark helpers: footage loading, timing stats, result rows.
 */
import { readFileSync, openSync, readSync, closeSync } from 'node:fs';
import { dirname, join } from 'node:path';
import { fileURLToPath } from 'node:url';

export const __dirname = dirname(fileURLToPath(import.meta.url));
export const FOOTAGE_DIR = join(__dirname, 'footage');
export const RESULTS_DIR = join(__dirname, 'results');

/** The two profiles under test. `ourProfile` is the ProResProfile enum; the
 *  ffmpeg fields drive both native ffmpeg and ffmpeg.wasm. */
export const PROFILES = {
  '422hq': {
    label: 'ProRes 422 HQ',
    ourProfile: 3,
    ffProfile: '3',
    ffPixFmt: 'yuv422p10le',
    vtProfile: 'hq',
    vtPixFmt: null,        // rgba fed directly; VideoToolbox picks its format
    alpha: false,
  },
  '4444': {
    label: 'ProRes 4444 (alpha)',
    ourProfile: 4,
    ffProfile: '4',
    ffPixFmt: 'yuva444p10le',
    vtProfile: '4444',
    vtPixFmt: null,        // rgba (with alpha) fed directly
    alpha: true,
  },
};

export function loadMeta() {
  return JSON.parse(readFileSync(join(FOOTAGE_DIR, 'meta.json'), 'utf8'));
}

/** Load all raw frames into one Buffer (host memory). */
export function loadFramesBuffer(meta) {
  return readFileSync(join(FOOTAGE_DIR, meta.rawFile));
}

/** Load all raw frames into a SharedArrayBuffer for zero-copy worker access. */
export function loadFramesShared(meta) {
  const total = meta.frames * meta.bytesPerFrame;
  const sab = new SharedArrayBuffer(total);
  const view = new Uint8Array(sab);
  const fd = openSync(join(FOOTAGE_DIR, meta.rawFile), 'r');
  try {
    let off = 0;
    const CHUNK = 64 * 1024 * 1024;
    const tmp = Buffer.allocUnsafe(CHUNK);
    while (off < total) {
      const n = readSync(fd, tmp, 0, Math.min(CHUNK, total - off), off);
      if (n <= 0) break;
      view.set(tmp.subarray(0, n), off);
      off += n;
    }
  } finally {
    closeSync(fd);
  }
  return sab;
}

export const RAW_FILE = (meta) => join(FOOTAGE_DIR, meta.rawFile);

// ---- stats ----
export function median(xs) {
  const s = [...xs].sort((a, b) => a - b);
  const m = s.length >> 1;
  return s.length % 2 ? s[m] : (s[m - 1] + s[m]) / 2;
}
export function mean(xs) { return xs.reduce((a, b) => a + b, 0) / xs.length; }
export function stdev(xs) {
  if (xs.length < 2) return 0;
  const m = mean(xs);
  return Math.sqrt(xs.reduce((a, b) => a + (b - m) ** 2, 0) / (xs.length - 1));
}
export function minv(xs) { return Math.min(...xs); }

/** Build a normalized result row from a list of encode-seconds samples. */
export function makeRow({ contender, environment, threads, profileKey, samples, outBytes, meta, note }) {
  const prof = PROFILES[profileKey];
  const secMedian = median(samples);
  const secBest = minv(samples);
  const fps = meta.frames / secMedian;
  const fpsBest = meta.frames / secBest;
  return {
    contender,
    environment,
    threads,
    profile: profileKey,
    profileLabel: prof.label,
    frames: meta.frames,
    runs: samples.length,
    encodeSecMedian: +secMedian.toFixed(4),
    encodeSecBest: +secBest.toFixed(4),
    encodeSecStdev: +stdev(samples).toFixed(4),
    fps: +fps.toFixed(2),
    fpsBest: +fpsBest.toFixed(2),
    realtimeX: +(fps / meta.fps).toFixed(2),
    outMB: outBytes != null ? +(outBytes / 1e6).toFixed(1) : null,
    mbps: outBytes != null ? +((outBytes * 8 / 1e6) / meta.seconds).toFixed(0) : null,
    note: note || '',
  };
}

/** Run fn() `reps` times after `warmup` untimed runs; returns seconds samples. */
export async function timeReps(fn, { reps, warmup = 1 } = {}) {
  for (let i = 0; i < warmup; i++) await fn();
  const samples = [];
  for (let i = 0; i < reps; i++) {
    const t0 = performance.now();
    await fn();
    samples.push((performance.now() - t0) / 1000);
  }
  return samples;
}

export function printRow(r) {
  const env = `${r.environment}/${r.threads}`.padEnd(16);
  console.log(
    `  ${r.contender.padEnd(20)} ${env} ${String(r.fps).padStart(8)} fps  ` +
    `${String(r.realtimeX).padStart(6)}x RT  ${r.encodeSecMedian.toFixed(2).padStart(7)}s` +
    (r.outMB != null ? `  ${String(r.outMB).padStart(6)}MB` : '') +
    (r.note ? `  (${r.note})` : '')
  );
}
