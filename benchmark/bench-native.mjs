/**
 * Benchmark native FFmpeg (the CLI baseline): prores_ks software at 1 thread
 * and all threads, plus the Apple VideoToolbox hardware ProRes encoder.
 *
 * Every encoder reads the identical raw RGBA frames and encodes to /dev/null
 * (`-f null`), so we time encode + colorspace conversion only, no container
 * write. Input is read from the OS page cache (warm after the first run).
 */
import { spawn, spawnSync } from 'node:child_process';
import { statSync } from 'node:fs';
import { join } from 'node:path';
import { PROFILES, RAW_FILE, timeReps, makeRow, RESULTS_DIR } from './bench-common.mjs';

function inputArgs(meta) {
  return [
    '-f', 'rawvideo', '-pix_fmt', 'rgba',
    '-s', `${meta.width}x${meta.height}`, '-r', String(meta.fps),
    '-i', RAW_FILE(meta), '-frames:v', String(meta.frames),
  ];
}

/** Encoder args for a (mode, profile) pair. mode: 'sw1' | 'swAuto' | 'hw'. */
function encoderArgs(mode, prof, meta) {
  if (mode === 'hw') {
    const a = ['-c:v', 'prores_videotoolbox', '-profile:v', prof.vtProfile];
    if (prof.vtPixFmt) a.push('-pix_fmt', prof.vtPixFmt);
    return a;
  }
  const threads = mode === 'sw1' ? '1' : '0';
  return [
    '-threads', threads,
    '-c:v', 'prores_ks', '-profile:v', prof.ffProfile, '-pix_fmt', prof.ffPixFmt,
  ];
}

function runFFmpeg(args) {
  return new Promise((resolve, reject) => {
    const p = spawn('ffmpeg', ['-hide_banner', '-loglevel', 'error', '-y', ...args], { stdio: ['ignore', 'ignore', 'pipe'] });
    let err = '';
    p.stderr.on('data', (d) => { err += d; });
    p.on('close', (code) => code === 0 ? resolve() : reject(new Error(`ffmpeg exited ${code}: ${err.slice(0, 400)}`)));
    p.on('error', reject);
  });
}

const MODES = {
  sw1: { threads: 'sw-1core', label: 'software, 1 core' },
  swAuto: { threads: 'sw-allcores', label: 'software, all cores' },
  hw: { threads: 'hw-vt', label: 'hardware (VideoToolbox)' },
};

/** Measure encode output size once (writes a real .mov). */
function measureSize(mode, prof, meta) {
  const out = join(RESULTS_DIR, `native_${mode}_${prof.ffProfile}.mov`);
  const r = spawnSync('ffmpeg', [
    '-hide_banner', '-loglevel', 'error', '-y', ...inputArgs(meta), ...encoderArgs(mode, prof, meta), out,
  ]);
  if (r.status !== 0) return null;
  try { return statSync(out).size; } catch { return null; }
}

export async function runNative(meta, profileKey, mode, { reps = 5 } = {}) {
  const prof = PROFILES[profileKey];
  const args = [...inputArgs(meta), ...encoderArgs(mode, prof, meta), '-f', 'null', '-'];
  const fn = () => runFFmpeg(args);

  // Probe once so a hardware/pixfmt failure surfaces clearly before timing.
  try { await fn(); } catch (e) {
    return { error: e.message, contender: `FFmpeg (native, ${MODES[mode].label})`, mode, profile: profileKey };
  }

  const samples = await timeReps(fn, { reps, warmup: 0 });
  const outBytes = measureSize(mode, prof, meta);

  return makeRow({
    contender: 'FFmpeg (native)',
    environment: 'native', threads: MODES[mode].threads,
    profileKey, samples, outBytes, meta,
  });
}

export const NATIVE_MODES = Object.keys(MODES);
