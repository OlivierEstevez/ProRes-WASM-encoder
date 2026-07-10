/**
 * Dedicated worker that drives ffmpeg.wasm (single-thread @ffmpeg/core or
 * multi-thread @ffmpeg/core-mt) directly — no @ffmpeg/ffmpeg wrapper.
 *
 * Running the core inside a worker (rather than the page) is what makes the
 * multi-thread core usable: its pthreads call Atomics.wait, which is forbidden
 * on the main thread but fine here. The worker fetches its own copy of the raw
 * frames (via a Range request) so nothing has to be transferred/detached.
 */
// Absolute (origin-qualified) URLs: a bare "/path" is not a resolvable module
// specifier for dynamic import() inside a worker.
const ORIGIN = self.location.origin;
const FOOTAGE = `${ORIGIN}/benchmark/footage`;

async function getCore(mt) {
  const dir = `${ORIGIN}/benchmark/node_modules/@ffmpeg/${mt ? 'core-mt' : 'core'}/dist/esm`;
  const createCore = (await import(`${dir}/ffmpeg-core.js`)).default;
  // In a worker, self/location exist and import.meta.url resolves the wasm and
  // pthread worker alongside the core — no extra config needed.
  return createCore();
}

self.onmessage = async (e) => {
  const { width, height, fps, frames, bpf, reps, mt, threads, ffProfile, ffPixFmt } = e.data;
  try {
    const resp = await fetch(`${FOOTAGE}/frames_1080p.rgba`, { headers: { Range: `bytes=0-${frames * bpf - 1}` } });
    const input = new Uint8Array(await resp.arrayBuffer());

    // One core instance, reused across reps (fresh instances don't coexist
    // reliably in one worker). reset() clears arg state between runs; the wasm
    // stays hot after the warmup rep.
    const core = await getCore(mt);
    core.setLogger(() => {});
    core.setTimeout(-1);
    core.FS.writeFile('in.rgba', input);
    const args = [
      '-f', 'rawvideo', '-pix_fmt', 'rgba', '-s', `${width}x${height}`, '-r', String(fps),
      '-i', 'in.rgba', '-frames:v', String(frames),
    ];
    if (mt) args.push('-threads', String(threads));
    args.push('-c:v', 'prores_ks', '-profile:v', ffProfile, '-pix_fmt', ffPixFmt, '-vendor', 'apl0', 'out.mov');

    const samples = [];
    let outBytes = null;
    for (let r = 0; r < reps + 1; r++) { // +1 warmup
      const t0 = performance.now();
      core.exec(...args);
      const ms = performance.now() - t0;
      if (core.ret !== 0) throw new Error(`ffmpeg.wasm exec ret ${core.ret}`);
      outBytes = core.FS.readFile('out.mov').length;
      if (r > 0) samples.push(ms);
      core.reset();
    }
    self.postMessage({ ok: true, samples, outBytes });
  } catch (err) {
    self.postMessage({ ok: false, error: String(err && err.message || err) });
  }
};
