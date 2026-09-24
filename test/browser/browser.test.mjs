/**
 * End-to-end browser test: installs the PACKED tarball into a scratch Vite
 * app (test/browser/app), serves it with both `vite build` + preview and
 * the Vite dev server, and runs the fixture in headless Chromium through
 * Playwright. Encoded files come back to Node and are checked with ffprobe.
 *
 * Run with `npm run test:browser` after `npm run build`. Uses Playwright's
 * Chromium; set PLAYWRIGHT_CHANNEL=chrome to use an installed Chrome.
 */
import { describe, it, before, after } from 'node:test';
import assert from 'node:assert';
import { execFileSync } from 'node:child_process';
import { cpSync, mkdtempSync, mkdirSync, rmSync, writeFileSync } from 'node:fs';
import { tmpdir } from 'node:os';
import { join } from 'node:path';
import { fileURLToPath } from 'node:url';
import { build, preview, createServer } from 'vite';
import { chromium } from 'playwright';
import { requireDist, hasFfprobe, probeMov } from '../support/helpers.mjs';

requireDist('prores-encoder.mjs', 'browser.test');

const ROOT = fileURLToPath(new URL('../..', import.meta.url));
const FIXTURE = fileURLToPath(new URL('./app', import.meta.url));
const FRAMES = 12;

let work;
let app;
let browser;

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

  browser = await chromium.launch({ channel: process.env.PLAYWRIGHT_CHANNEL || undefined });
});

after(async () => {
  if (browser) await browser.close();
  if (work) rmSync(work, { recursive: true, force: true });
});

async function runFixture(url) {
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

  for (const [name, b64] of Object.entries(run.files)) {
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

describe('browser (Chromium + Vite)', () => {
  it('works from a production build (vite build + preview)', async () => {
    await build({ root: app, logLevel: 'silent' });
    const server = await preview({ root: app, logLevel: 'silent', preview: { port: 0, host: '127.0.0.1' } });
    try {
      checkRun(await runFixture(server.resolvedUrls.local[0]));
    } finally {
      await new Promise((r) => server.httpServer.close(r));
    }
  });

  it('works from the dev server with pre-bundled dependencies', async () => {
    const server = await createServer({ root: app, logLevel: 'silent', server: { port: 0, host: '127.0.0.1' } });
    await server.listen();
    try {
      checkRun(await runFixture(server.resolvedUrls.local[0]));
    } finally {
      await server.close();
    }
  });
});
