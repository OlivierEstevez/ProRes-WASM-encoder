/**
 * Package contract test: packs the library exactly as `npm publish` would,
 * installs the tarball into a scratch project, and checks that every
 * documented entry point and export resolves (ESM and CommonJS) and that
 * the type declarations resolve under every TypeScript module mode.
 * Needs `npm run build` (or `npm run build:js`) first.
 */
import { describe, it, before, after } from 'node:test';
import assert from 'node:assert';
import { execFileSync } from 'node:child_process';
import { copyFileSync, mkdtempSync, mkdirSync, rmSync, writeFileSync } from 'node:fs';
import { tmpdir } from 'node:os';
import { join } from 'node:path';
import { fileURLToPath } from 'node:url';
import { requireDist } from './support/helpers.mjs';
import { API_SURFACE, METHODS } from './support/api-surface.mjs';

requireDist('prores-encoder.mjs', 'package.test');

const ROOT = fileURLToPath(new URL('..', import.meta.url));
const MEDIABUNNY = join(ROOT, 'node_modules', 'mediabunny');

let work;
let tarball;
let packedFiles;

// Private npm cache: packing must not depend on (or touch) the user's cache.
const npmEnv = () => ({ ...process.env, npm_config_cache: join(work, 'npm-cache') });

function npm(args, cwd) {
  return execFileSync('npm', args, { cwd, env: npmEnv(), encoding: 'utf8' });
}

before(() => {
  work = mkdtempSync(join(tmpdir(), 'prores-package-'));
  const [info] = JSON.parse(npm(['pack', '--json', '--pack-destination', work], ROOT));
  tarball = join(work, info.filename);
  packedFiles = info.files.map((f) => f.path);

  const app = join(work, 'app');
  mkdirSync(app);
  writeFileSync(join(app, 'package.json'), JSON.stringify({ name: 'app', private: true }));
  npm(['install', '--offline', '--no-audit', '--no-fund', tarball, MEDIABUNNY], app);
});

after(() => {
  if (work) rmSync(work, { recursive: true, force: true });
});

function runInApp(file, code) {
  const path = join(work, 'app', file);
  writeFileSync(path, code);
  return JSON.parse(execFileSync(process.execPath, [path], { cwd: join(work, 'app'), encoding: 'utf8' }));
}

/** Script body that reports the shape of each entry's expected exports. */
function describeExports(specs, load) {
  return `
    const surface = ${JSON.stringify(API_SURFACE)};
    const methods = ${JSON.stringify(METHODS)};
    const out = {};
    for (const spec of ${JSON.stringify(specs)}) {
      const want = surface[spec];
      const mod = ${load};
      const r = out[spec] = {};
      for (const n of [...want.functions, ...want.classes]) r[n] = typeof mod[n];
      for (const n of want.objects) r[n] = mod[n] && typeof mod[n];
      for (const c of want.classes) {
        r[c + '#methods'] = (methods[c] || []).filter((m) => typeof mod[c]?.prototype?.[m] !== 'function');
      }
    }
    console.log(JSON.stringify(out));
  `;
}

function checkExports(result, specs) {
  for (const spec of specs) {
    const want = API_SURFACE[spec];
    const got = result[spec];
    assert.ok(got, `${spec} did not load`);
    for (const n of [...want.functions, ...want.classes]) {
      assert.strictEqual(got[n], 'function', `${spec}: ${n} missing`);
    }
    for (const n of want.objects) assert.strictEqual(got[n], 'object', `${spec}: ${n} missing`);
    for (const c of want.classes) {
      assert.deepStrictEqual(got[c + '#methods'], [], `${spec}: ${c} lacks methods`);
    }
  }
}

describe('packed tarball', () => {
  it('ships every entry point and no build intermediates', () => {
    for (const f of [
      'dist/prores-encoder.mjs', 'dist/prores-encoder.js', 'dist/prores-encoder.d.ts', 'dist/prores-encoder.d.mts',
      'dist/prores-encoder-parallel.mjs', 'dist/prores-encoder-parallel.js',
      'dist/prores-encoder-parallel.d.ts', 'dist/prores-encoder-parallel.d.mts',
      'dist/prores-encoder-mediabunny.mjs', 'dist/prores-encoder-mediabunny.d.mts',
      'dist/prores-core.mjs', 'LICENSE', 'README.md', 'package.json',
    ]) {
      assert.ok(packedFiles.includes(f), `${f} is not in the tarball`);
    }
    for (const f of ['dist/prores-encoder.core.wasm', 'dist/prores-encoder.wasm.js', 'dist/prores-worker.mjs']) {
      assert.ok(!packedFiles.includes(f), `${f} should not be published`);
    }
  });

  it('exports the documented API via import', () => {
    const specs = Object.keys(API_SURFACE);
    const code = describeExports(specs, 'await import(spec)');
    checkExports(runInApp('check.mjs', `(async () => { ${code} })();`), specs);
  });

  it('exports the documented API via require', () => {
    const specs = Object.keys(API_SURFACE).filter((s) => API_SURFACE[s].cjs);
    const code = describeExports(specs, 'require(spec)');
    checkExports(runInApp('check.cjs', code), specs);
  });

  it('encodes a frame from the installed package', () => {
    const out = runInApp('encode.mjs', `
      import { createProResEncoder, ProResProfile } from 'prores-wasm-encoder';
      const enc = await createProResEncoder();
      enc.initialize({ width: 32, height: 16, profile: ProResProfile.HQ });
      enc.addFrameRgba(new Uint8Array(32 * 16 * 4).fill(128));
      const mov = enc.finalize();
      enc.destroy();
      console.log(JSON.stringify({ size: mov.length, ftyp: String.fromCharCode(...mov.subarray(4, 8)) }));
    `);
    assert.strictEqual(out.ftyp, 'ftyp');
    assert.ok(out.size > 0);
  });

  function tsc(name, compilerOptions, files) {
    const app = join(work, 'app');
    const tsconfig = join(app, `tsconfig.${name}.json`);
    writeFileSync(tsconfig, JSON.stringify({
      compilerOptions: {
        strict: true,
        noEmit: true,
        target: 'es2022',
        lib: ['esnext', 'dom'],
        types: [],
        ...compilerOptions,
      },
      files,
    }));
    try {
      execFileSync(join(ROOT, 'node_modules', '.bin', 'tsc'), ['-p', tsconfig], { cwd: app, encoding: 'utf8' });
    } catch (err) {
      assert.fail(`tsc (${name}) failed:\n${err.stdout}${err.stderr}`);
    }
  }

  for (const resolution of ['bundler', 'nodenext']) {
    it(`type-checks documented usage with tsc --strict (${resolution})`, () => {
      // Inside the scratch app, so imports resolve to the installed tarball.
      // skipLibCheck (as most apps use) keeps MediaBunny's own .d.ts out of
      // scope; our declarations get a full check below.
      const usage = join(work, 'app', 'usage.mts');
      copyFileSync(join(ROOT, 'test', 'types', 'usage.mts'), usage);
      tsc(resolution, {
        module: resolution === 'bundler' ? 'esnext' : 'nodenext',
        moduleResolution: resolution,
        skipLibCheck: true,
      }, [usage]);
    });
  }

  it('has declaration files that type-check on their own', () => {
    const dist = join(work, 'app', 'node_modules', 'prores-wasm-encoder', 'dist');
    tsc('declarations', { module: 'nodenext', moduleResolution: 'nodenext', skipLibCheck: false }, [
      'prores-encoder.d.ts', 'prores-encoder.d.mts',
      'prores-encoder-parallel.d.ts', 'prores-encoder-parallel.d.mts',
    ].map((f) => join(dist, f)));
  });

  it('has types that resolve in every TypeScript module mode', () => {
    const attw = join(ROOT, 'node_modules', '.bin', 'attw');
    // CommonJS + ESM entries: all resolution modes must be clean.
    execFileSync(attw, [tarball, '--entrypoints', '.', 'parallel'], { stdio: 'pipe' });
    // The MediaBunny entry is ESM-only.
    execFileSync(attw, [tarball, '--entrypoints', 'mediabunny', '--profile', 'esm-only'], { stdio: 'pipe' });
  });
});
