/**
 * End-to-end browser test: installs the PACKED tarball into a scratch Vite
 * app (test/browser/app), serves it with both `vite build` + preview and
 * the Vite dev server, and runs the fixture in headless Chromium through
 * Playwright. Encoded files come back to Node and are checked with ffprobe.
 *
 * Run with `npm run test:browser` after `npm run build`. Runs in Playwright's
 * Chromium, Firefox and WebKit; set BROWSERS=chromium (comma-separated) to
 * pick engines, and PLAYWRIGHT_CHANNEL=chrome to use an installed Chrome.
 */
import { describe, it, before, after } from 'node:test';
import assert from 'node:assert';
import { execFileSync } from 'node:child_process';
import { cpSync, mkdtempSync, mkdirSync, rmSync, writeFileSync } from 'node:fs';
import { tmpdir } from 'node:os';
import { join } from 'node:path';
import { fileURLToPath } from 'node:url';
import { build, preview, createServer } from 'vite';
import { chromium, firefox, webkit } from 'playwright';
import { requireDist, hasFfprobe, probeMov, decodeFirstFrameRgba } from '../support/helpers.mjs';

requireDist('prores-encoder.mjs', 'browser.test');

const ROOT = fileURLToPath(new URL('../..', import.meta.url));
const FIXTURE = fileURLToPath(new URL('./app', import.meta.url));
const FRAMES = 12;
const W = 720;
const H = 404;
const ENGINES = { chromium, firefox, webkit };
const BROWSERS = (process.env.BROWSERS || 'chromium,firefox,webkit').split(',');

let work;
let app;
const browsers = {};

before(async () => {
  work = mkdtempSync(join(tmpdir(), 'prores-browser-'));
  const env = { ...process.env, npm_config_cache: join(work, 'npm-cache') };
  const [info] = JSON.parse(execFileSync(
    'npm', ['pack', '--json', '--pack-destination', work], { cwd: ROOT, env, encoding: 'utf8' }
  ));

  app = join(work, 'app');
  mkdirSync(app);
  cpSync(FIXTURE, app, { recursive: true });
  writeFileSync(join(app, 'package.json'), JSON.stringify({ name: 'fixture', private: true, type: 'module' }));
  execFileSync('npm', ['install', '--offline', '--no-audit', '--no-fund', join(work, info.filename)], { cwd: app, env });
  // A real copy (not a symlink), so the dev server serves it from inside the app.
  cpSync(join(ROOT, 'node_modules', 'mediabunny'), join(app, 'node_modules', 'mediabunny'), { recursive: true });

  for (const name of BROWSERS) {
    browsers[name] = await ENGINES[name].launch(
      name === 'chromium' ? { channel: process.env.PLAYWRIGHT_CHANNEL || undefined } : {}
    );
  }
});

after(async () => {
  for (const b of Object.values(browsers)) await b.close();
  if (work) rmSync(work, { recursive: true, force: true });
});

async function runFixture(browser, url) {
  const page = await browser.newPage();
  const errors = [];
  let navigations = 0;
  page.on('pageerror', (err) => errors.push(String(err)));
  page.on('framenavigated', (frame) => { if (frame === page.mainFrame()) navigations++; });
  try {
    await page.goto(url);
    await page.waitForFunction(() => window.fixtureReady === true, null, { timeout: 30_000 });
    const out = await page.evaluate(() => window.runSuite());
    return { ...out, errors, navigations };
  } finally {
    await page.close();
  }
}

function checkRun(run) {
  assert.ok(!run.error, run.error);
  assert.deepStrictEqual(run.errors, [], 'page errors');
  assert.strictEqual(run.navigations, 1, 'the page reloaded during the run');

  const r = run.results;
  assert.strictEqual(r.poolWorkers, 4);
  assert.strictEqual(r.packetCount, FRAMES);
  assert.ok(r.poolMatchesSingle, 'pool packets differ from single-thread');
  assert.ok(r.webglMatches2d, 'WebGL canvas encoded differently from 2D (single-thread)');
  assert.ok(r.webglPoolMatches2d, 'WebGL canvas encoded differently from 2D (pool)');
  assert.match(r.sizeMismatch, /canvas is 300x150 but the encoder is 720x404/);

  checkColors(run);

  for (const [name, b64] of Object.entries(run.files)) {
    if (name.startsWith('colors')) continue; // one-frame files, checked above
    const bytes = Buffer.from(b64, 'base64');
    assert.ok(bytes.length > 10_000, `${name} is too small`);
    if (!hasFfprobe()) continue;
    const s = probeMov(bytes);
    assert.strictEqual(s.codec_name, 'prores', name);
    assert.strictEqual(s.width, 720, name);
    assert.strictEqual(s.height, 404, name);
    assert.strictEqual(Number(s.nb_read_frames), FRAMES, name);
    if (name === 'pool4444') {
      assert.match(s.pix_fmt, /^yuva444p/);
      assert.strictEqual(s.r_frame_rate, '24/1');
    }
  }
}

/** Mean RGBA over a rectangle of a decoded frame. */
function meanRgba(rgba, x0, x1) {
  const sum = [0, 0, 0, 0];
  let n = 0;
  for (let y = 8; y < H - 8; y++) {
    for (let x = x0; x < x1; x++) {
      const i = (y * W + x) * 4;
      for (let c = 0; c < 4; c++) sum[c] += rgba[i + c];
      n++;
    }
  }
  return sum.map((v) => v / n);
}

function assertClose(actual, expected, tol, what) {
  const ok = actual.every((v, i) => Math.abs(v - expected[i]) <= tol);
  assert.ok(ok, `${what}: got [${actual.map((v) => v.toFixed(1))}], expected [${expected.map((v) => v.toFixed(1))}]`);
}

/**
 * The MediaBunny path must decode to the same colors as the standalone
 * encoder fed from the same canvas: opaque colors always (catches R/B
 * swaps), semi-transparent ones whenever the browser's VideoFrame itself
 * is correct (catches premultiplied readback).
 */
function checkColors(run) {
  if (!hasFfprobe()) return;
  const ref = decodeFirstFrameRgba(Buffer.from(run.files.colorsReference, 'base64'));
  const mb = decodeFirstFrameRgba(Buffer.from(run.files.colorsMediabunny, 'base64'));
  const left = [16, W / 2 - 16];
  const right = [W / 2 + 16, W - 16];

  assertClose(meanRgba(ref, ...left), [230, 40, 20, 255], 2, 'reference opaque');
  assertClose(meanRgba(ref, ...right), [200, 100, 50, 128], 2, 'reference semi-transparent');
  assertClose(meanRgba(mb, ...left), meanRgba(ref, ...left), 2, 'MediaBunny opaque');

  const frameOk = Math.abs(run.results.browserFrameHalf[0] - 200) <= 3;
  if (frameOk) {
    assertClose(meanRgba(mb, ...right), meanRgba(ref, ...right), 3, 'MediaBunny semi-transparent');
  }
}

for (const name of BROWSERS) {
  describe(`browser (${name} + Vite)`, () => {
    it('works from a production build (vite build + preview)', async () => {
      await build({ root: app, logLevel: 'silent' });
      const server = await preview({ root: app, logLevel: 'silent', preview: { port: 0, host: '127.0.0.1' } });
      try {
        checkRun(await runFixture(browsers[name], server.resolvedUrls.local[0]));
      } finally {
        await new Promise((r) => server.httpServer.close(r));
      }
    });

    it('works from the dev server with pre-bundled dependencies', async () => {
      const server = await createServer({ root: app, logLevel: 'silent', server: { port: 0, host: '127.0.0.1' } });
      await server.listen();
      try {
        checkRun(await runFixture(browsers[name], server.resolvedUrls.local[0]));
      } finally {
        await server.close();
      }
    });
  });
}
