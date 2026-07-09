/**
 * In-browser WASM benchmark: our library (single + worker pool) vs ffmpeg.wasm
 * (single-thread @ffmpeg/core and multi-thread @ffmpeg/core-mt). This is the
 * apples-to-apples WASM comparison — every contender runs on the same V8, in
 * the same page, over the same frames. Native ffmpeg is measured separately
 * (Node harness) as the CLI reference.
 *
 * ffmpeg.wasm runs through the official @ffmpeg/ffmpeg FFmpeg class, which
 * hosts the core in a Web Worker — required for the multi-thread core, whose
 * pthreads call Atomics.wait (forbidden on the page's main thread).
 *
 * Our encoder is timed on the pure encode kernel (encodePacketRgba / pool
 * add+flush) with a warmup pass so the wasm is tiered up (hot), matching the
 * Node harness. fps = frames / median encode seconds.
 */
const params = new URLSearchParams(location.search);
const N_FRAMES = Number(params.get('frames')) || 150;
const REPS = Number(params.get('reps')) || 2;
const POOL_WORKERS = Number(params.get('workers')) || 8;
const MT_THREADS = Number(params.get('threads')) || 8;

const REPO = '/';
const FOOTAGE = '/benchmark/footage';
const FF_PKG = '/benchmark/node_modules/@ffmpeg';

const PROFILE_ORDER = ['422hq', '4444'];
const PROFILES = {
  '422hq': { label: 'ProRes 422 HQ', ourProfile: 3, ffProfile: '3', ffPixFmt: 'yuv422p10le' },
  '4444':  { label: 'ProRes 4444 (alpha)', ourProfile: 4, ffProfile: '4', ffPixFmt: 'yuva444p10le' },
};

window.__results = [];
window.__status = 'idle';
window.__done = false;

const logEl = () => document.getElementById('log');
const tableEl = () => document.getElementById('rows');
function log(msg) {
  console.log(msg);
  const el = logEl();
  if (el) el.textContent += msg + '\n';
  window.__status = msg;
}

let meta, framesBuf, bpf;

async function loadFrames() {
  meta = await (await fetch(`${FOOTAGE}/meta.json`)).json();
  bpf = meta.bytesPerFrame;
  const n = Math.min(N_FRAMES, meta.frames);
  const bytes = n * bpf;
  log(`Fetching ${n} frames (${(bytes / 1e6).toFixed(0)} MB) ...`);
  const resp = await fetch(`${FOOTAGE}/${meta.rawFile}`, { headers: { Range: `bytes=0-${bytes - 1}` } });
  framesBuf = new Uint8Array(await resp.arrayBuffer());
  meta = { ...meta, frames: n };
  log(`Loaded ${n} frames. ${meta.width}x${meta.height} @ ${meta.fps}fps`);
}

const frameView = (i) => framesBuf.subarray(i * bpf, (i + 1) * bpf);
const median = (xs) => { const s = [...xs].sort((a, b) => a - b); const m = s.length >> 1; return s.length % 2 ? s[m] : (s[m - 1] + s[m]) / 2; };

function pushRow(contender, threads, profileKey, samples, outBytes, note) {
  const secMed = median(samples) / 1000;
  const row = {
    contender, environment: 'browser', threads, profile: profileKey,
    profileLabel: PROFILES[profileKey].label, frames: meta.frames, runs: samples.length,
    fps: +(meta.frames / secMed).toFixed(2), realtimeX: +((meta.frames / secMed) / meta.fps).toFixed(2),
    encodeSecMedian: +secMed.toFixed(4), outMB: outBytes != null ? +(outBytes / 1e6).toFixed(1) : null, note: note || '',
  };
  window.__results.push(row);
  const tr = document.createElement('tr');
  tr.innerHTML = `<td>${contender}</td><td>${threads}</td><td>${PROFILES[profileKey].label}</td>` +
    `<td><b>${row.fps}</b></td><td>${row.realtimeX}x</td><td>${row.encodeSecMedian.toFixed(2)}s</td><td>${row.outMB ?? ''}</td>`;
  tableEl().appendChild(tr);
  log(`  ${contender} ${threads} ${profileKey}: ${row.fps} fps (${row.realtimeX}x RT)`);
  return row;
}

// ---- our library (pure encode, hot) ----
async function benchOursSingle(profileKey) {
  const { createProResEncoder } = await import(`${REPO}dist/prores-encoder.esm.js`);
  const prof = PROFILES[profileKey];
  const enc = await createProResEncoder();
  enc.initialize({ width: meta.width, height: meta.height, frameRate: meta.fps, profile: prof.ourProfile });
  const encodeAll = () => { for (let i = 0; i < meta.frames; i++) enc.encodePacketRgba(frameView(i)); };
  encodeAll();                 // warmup (tier up wasm)
  const samples = [];
  for (let r = 0; r < REPS; r++) { const t0 = performance.now(); encodeAll(); samples.push(performance.now() - t0); }
  enc.destroy();

  // Output size from a real muxed file (separate instance).
  const enc2 = await createProResEncoder();
  enc2.initialize({ width: meta.width, height: meta.height, frameRate: meta.fps, profile: prof.ourProfile });
  for (let i = 0; i < meta.frames; i++) enc2.addFrameRgba(frameView(i));
  const outBytes = enc2.finalize().length;
  enc2.destroy();
  pushRow('prores-wasm (ours)', '1t', profileKey, samples, outBytes);
}

async function benchOursPool(profileKey) {
  const { createProResEncoderPool } = await import(`${REPO}dist/prores-encoder-parallel.esm.js`);
  const prof = PROFILES[profileKey];
  const pool = await createProResEncoderPool({
    width: meta.width, height: meta.height, frameRate: meta.fps, profile: prof.ourProfile, workers: POOL_WORKERS,
  });
  const addAll = async () => { for (let i = 0; i < meta.frames; i++) await pool.addFrameRgba(frameView(i)); await pool.flush(); };
  await addAll();              // warmup
  const samples = [];
  for (let r = 0; r < REPS; r++) { const t0 = performance.now(); await addAll(); samples.push(performance.now() - t0); }
  await pool.destroy();
  pushRow('prores-wasm (ours)', `${POOL_WORKERS}t`, profileKey, samples, null, `${POOL_WORKERS} workers`);
}

// ---- ffmpeg.wasm in a dedicated worker (single-thread core & mt core) ----
function benchFFmpegWasm(profileKey, mt) {
  const prof = PROFILES[profileKey];
  return new Promise((resolve, reject) => {
    const worker = new Worker('/benchmark/browser/ffworker.js', { type: 'module' });
    worker.onmessage = (e) => {
      worker.terminate();
      if (!e.data.ok) return reject(new Error(e.data.error));
      pushRow('ffmpeg.wasm', mt ? `${MT_THREADS}t` : '1t', profileKey, e.data.samples, e.data.outBytes,
        mt ? '@ffmpeg/core-mt' : '@ffmpeg/core');
      resolve();
    };
    worker.onerror = (e) => { worker.terminate(); reject(new Error(e.message || 'worker error')); };
    worker.postMessage({
      width: meta.width, height: meta.height, fps: meta.fps, frames: meta.frames, bpf,
      reps: REPS, mt, threads: MT_THREADS, ffProfile: prof.ffProfile, ffPixFmt: prof.ffPixFmt,
    });
  });
}

async function runAll() {
  window.__done = false;
  window.__results = [];
  tableEl().innerHTML = '';
  await loadFrames();
  for (const profileKey of PROFILE_ORDER) {
    log(`\n== ${PROFILES[profileKey].label} ==`);
    try { await benchFFmpegWasm(profileKey, false); } catch (e) { log(`  ffmpeg.wasm 1t FAILED: ${e.message}`); }
    try { await benchFFmpegWasm(profileKey, true); } catch (e) { log(`  ffmpeg.wasm mt FAILED: ${e.message}`); }
    try { await benchOursSingle(profileKey); } catch (e) { log(`  ours 1t FAILED: ${e.message}`); }
    try { await benchOursPool(profileKey); } catch (e) { log(`  ours pool FAILED: ${e.message}`); }
  }
  window.__done = true;
  log('\nDONE');
}

window.__runAll = runAll;
window.__downloadCSV = function () {
  const cols = ['contender', 'environment', 'threads', 'profile', 'profileLabel', 'frames', 'runs', 'fps', 'realtimeX', 'encodeSecMedian', 'outMB', 'note'];
  const csv = [cols.join(',')].concat(window.__results.map((r) => cols.map((c) => {
    const v = r[c] ?? ''; return typeof v === 'string' && v.includes(',') ? `"${v}"` : v;
  }).join(','))).join('\n');
  const a = document.createElement('a');
  a.href = URL.createObjectURL(new Blob([csv], { type: 'text/csv' }));
  a.download = 'results_browser.csv';
  a.click();
};

document.getElementById('run').addEventListener('click', () => { runAll(); });
document.getElementById('csv').addEventListener('click', () => window.__downloadCSV());
log(`Ready. frames=${N_FRAMES} reps=${REPS} pool=${POOL_WORKERS} mt-threads=${MT_THREADS}. crossOriginIsolated=${self.crossOriginIsolated}`);
if (params.get('auto') === '1') runAll();
