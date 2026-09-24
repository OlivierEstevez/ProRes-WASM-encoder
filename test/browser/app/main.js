/**
 * Browser fixture for test/browser/browser.test.mjs. Built and served by
 * Vite from the installed package tarball, like a user's app. Exposes
 * window.runSuite(), which returns results plus the encoded files (base64)
 * so the Node side can probe them with ffprobe.
 *
 * Entries are imported lazily (the pattern that tripped Vite's dependency
 * discovery in the field report).
 */

// 720px = 45 MBs: five full slices plus 4 + 1 remainder slices.
const W = 720;
const H = 404;
const FRAMES = 12;

function makeCanvas(w = W, h = H) {
  const c = document.createElement('canvas');
  c.width = w;
  c.height = h;
  return c;
}

function draw2d(ctx, i) {
  const g = ctx.createLinearGradient(0, 0, W, H);
  g.addColorStop(0, `hsl(${i * 30} 70% 40%)`);
  g.addColorStop(1, `hsl(${i * 30 + 120} 70% 60%)`);
  ctx.fillStyle = g;
  ctx.fillRect(0, 0, W, H);
  ctx.fillStyle = '#fff';
  ctx.fillRect(40 + i * 50, 100, 80, 80);
  ctx.font = '48px sans-serif';
  ctx.fillText(`frame ${i}`, 40, 320);
}

/** Opaque solid colors, identical whether drawn by WebGL or 2D. */
const SOLIDS = [[255, 0, 0], [0, 255, 0], [0, 0, 255], [17, 136, 204], [250, 250, 5]];

function equalPackets(a, b) {
  if (a.length !== b.length) return false;
  for (let i = 0; i < a.length; i++) {
    if (a[i].length !== b[i].length) return false;
    for (let j = 0; j < a[i].length; j++) if (a[i][j] !== b[i][j]) return false;
  }
  return true;
}

async function toBase64(blobOrBytes) {
  const blob = blobOrBytes instanceof Blob ? blobOrBytes : new Blob([blobOrBytes]);
  const url = await new Promise((resolve) => {
    const r = new FileReader();
    r.onload = () => resolve(r.result);
    r.readAsDataURL(blob);
  });
  return url.slice(url.indexOf(',') + 1);
}

async function runSuite() {
  const results = {};
  const files = {};
  const { createProResEncoder, ProResProfile } = await import('prores-wasm-encoder');
  const { createProResEncoderPool } = await import('prores-wasm-encoder/parallel');

  // 1. Pool output matches single-thread, from a 2D canvas.
  const c2d = makeCanvas();
  const ctx = c2d.getContext('2d', { willReadFrequently: true });
  const single = [];
  const enc = await createProResEncoder();
  enc.initialize({ width: W, height: H, profile: ProResProfile.HQ, onFrameData: (c) => single.push(c) });
  for (let i = 0; i < FRAMES; i++) { draw2d(ctx, i); enc.addFrameFromCanvas(c2d); }
  enc.destroy();

  const pooled = [];
  const pool = await createProResEncoderPool({
    width: W, height: H, profile: ProResProfile.HQ, workers: 4, onFrameData: (c) => pooled.push(c),
  });
  results.poolWorkers = pool.workerCount;
  for (let i = 0; i < FRAMES; i++) { draw2d(ctx, i); await pool.addFrameFromCanvas(c2d); }
  await pool.flush();
  await pool.destroy();
  results.poolMatchesSingle = equalPackets(single, pooled);
  results.packetCount = pooled.length;

  // 2. WebGL canvas (getContext('2d') is null) encodes like a 2D canvas.
  const glCanvas = makeCanvas();
  const gl = glCanvas.getContext('webgl');
  const flat = makeCanvas();
  const flatCtx = flat.getContext('2d', { willReadFrequently: true });
  const fromGl = [];
  const fromGlPool = [];
  const from2d = [];
  const encGl = await createProResEncoder();
  encGl.initialize({ width: W, height: H, onFrameData: (c) => fromGl.push(c) });
  const poolGl = await createProResEncoderPool({ width: W, height: H, workers: 2, onFrameData: (c) => fromGlPool.push(c) });
  const enc2d = await createProResEncoder();
  enc2d.initialize({ width: W, height: H, onFrameData: (c) => from2d.push(c) });
  for (const [r, g, b] of SOLIDS) {
    gl.clearColor(r / 255, g / 255, b / 255, 1);
    gl.clear(gl.COLOR_BUFFER_BIT);
    encGl.addFrameFromCanvas(glCanvas); // same task as the draw
    await poolGl.addFrameFromCanvas(glCanvas); // pixels read before the await
    gl.clearColor(0, 0, 0, 1);
    gl.clear(gl.COLOR_BUFFER_BIT);
    flatCtx.fillStyle = `rgb(${r} ${g} ${b})`;
    flatCtx.fillRect(0, 0, W, H);
    enc2d.addFrameFromCanvas(flat);
  }
  await poolGl.flush();
  await poolGl.destroy();
  encGl.destroy();
  enc2d.destroy();
  results.webglMatches2d = equalPackets(fromGl, from2d);
  results.webglPoolMatches2d = equalPackets(fromGlPool, from2d);

  // 3. A canvas sized by CSS only is rejected with a clear message.
  try {
    const e = await createProResEncoder();
    e.initialize({ width: W, height: H });
    e.addFrameFromCanvas(makeCanvas(300, 150));
    results.sizeMismatch = 'no error';
  } catch (err) {
    results.sizeMismatch = String(err.message);
  }

  // 4. Complete files: pool 4444 with alpha, single-thread HQ.
  const alpha = makeCanvas();
  const actx = alpha.getContext('2d', { willReadFrequently: true });
  const pool4444 = await createProResEncoderPool({ width: W, height: H, profile: ProResProfile.P4444, frameRate: 24 });
  for (let i = 0; i < FRAMES; i++) {
    actx.clearRect(0, 0, W, H);
    actx.fillStyle = `hsl(${i * 30} 80% 50% / 0.5)`;
    actx.fillRect(50 + i * 40, 50, 200, 200);
    await pool4444.addFrameFromCanvas(alpha);
  }
  files.pool4444 = await toBase64(await pool4444.finalizeToBlob());
  await pool4444.destroy();

  const encHq = await createProResEncoder();
  encHq.initialize({ width: W, height: H, profile: ProResProfile.HQ, frameRate: 30 });
  for (let i = 0; i < FRAMES; i++) { draw2d(ctx, i); encHq.addFrameFromCanvas(c2d); }
  files.singleHq = await toBase64(encHq.finalize());
  encHq.destroy();

  // 5. MediaBunny integration (multi-threaded by default).
  const { Output, BufferTarget, MovOutputFormat, CanvasSource } = await import('mediabunny');
  const { registerProResEncoder } = await import('prores-wasm-encoder/mediabunny');
  registerProResEncoder();
  const output = new Output({ format: new MovOutputFormat(), target: new BufferTarget() });
  const source = new CanvasSource(c2d, { codec: 'prores', fullCodecString: 'apcn', bitrate: 100_000_000 });
  output.addVideoTrack(source, { frameRate: 30 });
  await output.start();
  for (let i = 0; i < FRAMES; i++) { draw2d(ctx, i); await source.add(i / 30, 1 / 30); }
  await output.finalize();
  files.mediabunny = await toBase64(new Uint8Array(output.target.buffer));

  return { results, files };
}

window.runSuite = async () => {
  document.getElementById('status').textContent = 'running';
  try {
    const out = await runSuite();
    document.getElementById('status').textContent = 'done';
    return out;
  } catch (err) {
    document.getElementById('status').textContent = 'error';
    return { error: String(err && err.stack || err) };
  }
};
window.fixtureReady = true;
