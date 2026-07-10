/**
 * Orchestrate the full benchmark and write results/{results.json,results.csv}.
 *
 * Contenders (all encode the identical 150-frame 1080p RGBA clip):
 *   - prores-wasm (ours)     single-thread + worker sweep {2,4,8}
 *   - ffmpeg.wasm            single-thread (@ffmpeg/core, Node)
 *   - ffmpeg native          prores_ks 1-thread, prores_ks all-threads,
 *                            prores_videotoolbox (Apple HW)
 *
 * Run OUTSIDE the sandbox: prores_videotoolbox needs the macOS VideoToolbox
 * service, which the command sandbox blocks. ffmpeg.wasm multi-thread is not
 * here — pthreads need Web Workers; see the browser harness (bench-browser/).
 *
 * Usage:  node --no-warnings run-all.mjs [--quick]
 */
import { execFileSync } from 'node:child_process';
import { writeFileSync, mkdirSync } from 'node:fs';
import { join } from 'node:path';
import { loadMeta, printRow, RESULTS_DIR } from './bench-common.mjs';
import { runSingle, runPool } from './bench-ours.mjs';
import { runWasmSingle } from './bench-ffmpegwasm.mjs';
import { runNative } from './bench-native.mjs';

const QUICK = process.argv.includes('--quick');
const WORKER_SWEEP = QUICK ? [8] : [2, 4, 8];
const REPS = QUICK
  ? { slow: 1, fast: 1, wasm: 1 }
  : { slow: 2, fast: 4, wasm: 2 };

mkdirSync(RESULTS_DIR, { recursive: true });
const meta = loadMeta();

function hostInfo() {
  const get = (cmd, args) => { try { return execFileSync(cmd, args, { encoding: 'utf8' }).trim(); } catch { return null; } };
  return {
    date: new Date().toISOString(),
    node: process.version,
    cpu: get('sysctl', ['-n', 'machdep.cpu.brand_string']),
    cores: get('getconf', ['_NPROCESSORS_ONLN']),
    ffmpeg: (get('ffmpeg', ['-version']) || '').split('\n')[0],
    os: get('sw_vers', ['-productVersion']),
  };
}

const rows = [];
function record(row) {
  if (row.error) { console.log(`  ! ${row.contender} [${row.profile}] FAILED: ${row.error}`); return; }
  rows.push(row);
  printRow(row);
}

console.log(`\nProRes encoder benchmark — Big Buck Bunny ${meta.width}x${meta.height}, ${meta.frames} frames @ ${meta.fps}fps (${meta.seconds}s)`);
console.log(`Mode: ${QUICK ? 'QUICK (smoke)' : 'FULL'}  |  worker sweep: [${WORKER_SWEEP}]  |  reps: ${JSON.stringify(REPS)}\n`);

for (const profileKey of ['422hq', '4444']) {
  console.log(`\n######## ${meta ? '' : ''}Profile: ${profileKey} ########`);

  // Native FFmpeg (child processes; VideoToolbox needs sandbox off).
  console.log('\n-- native ffmpeg --');
  record(await runNative(meta, profileKey, 'sw1', { reps: REPS.slow }));
  record(await runNative(meta, profileKey, 'swAuto', { reps: REPS.fast }));
  record(await runNative(meta, profileKey, 'hw', { reps: REPS.fast }));

  // Our library.
  console.log('\n-- prores-wasm (ours) --');
  record(await runSingle(meta, profileKey, { reps: REPS.slow, save: true }));
  for (const w of WORKER_SWEEP) {
    record(await runPool(meta, profileKey, { workers: w, reps: REPS.fast }));
  }

  // ffmpeg.wasm single-thread.
  console.log('\n-- ffmpeg.wasm --');
  record(await runWasmSingle(meta, profileKey, { reps: REPS.wasm }));
}

// ---- write outputs ----
const out = { meta, host: hostInfo(), results: rows };
writeFileSync(join(RESULTS_DIR, 'results.json'), JSON.stringify(out, null, 2));

const cols = ['contender', 'environment', 'threads', 'profile', 'profileLabel', 'frames', 'runs',
  'fps', 'fpsBest', 'realtimeX', 'encodeSecMedian', 'encodeSecBest', 'encodeSecStdev', 'outMB', 'mbps', 'note'];
const csv = [cols.join(',')]
  .concat(rows.map((r) => cols.map((c) => {
    const v = r[c] ?? '';
    return typeof v === 'string' && v.includes(',') ? `"${v}"` : v;
  }).join(',')))
  .join('\n');
writeFileSync(join(RESULTS_DIR, 'results.csv'), csv + '\n');

console.log(`\n\nWrote ${rows.length} rows to:`);
console.log(`  ${join(RESULTS_DIR, 'results.csv')}`);
console.log(`  ${join(RESULTS_DIR, 'results.json')}`);
process.exit(0);
