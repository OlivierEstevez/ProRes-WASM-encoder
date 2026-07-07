import { nodeResolve } from '@rollup/plugin-node-resolve';
import terser from '@rollup/plugin-terser';
import { copyFileSync, mkdirSync, readFileSync } from 'fs';
import { resolve, dirname } from 'path';

function copyTypes() {
  return {
    name: 'copy-types',
    writeBundle() {
      const src = resolve('lib/index.d.ts');
      const dest = resolve('dist/prores-encoder.d.ts');
      mkdirSync(dirname(dest), { recursive: true });
      copyFileSync(src, dest);
    }
  };
}

// Resolves the virtual `prores:worker` import to the pre-built, self-contained
// worker bundle (dist/prores-worker.js) as a default-exported string. The
// worker build below must run first so the file exists when this loads it.
function inlineWorker(workerFile) {
  const ID = 'prores:worker';
  const RESOLVED = '\0' + ID;
  return {
    name: 'inline-worker',
    resolveId(id) {
      return id === ID ? RESOLVED : null;
    },
    load(id) {
      if (id === RESOLVED) {
        const code = readFileSync(resolve(workerFile), 'utf8');
        return `export default ${JSON.stringify(code)};`;
      }
      return null;
    }
  };
}

const WORKER_FILE = 'dist/prores-worker.js';

export default [
  // Build 0: self-contained worker bundle (WASM inlined). Must be first so
  // the inline-worker plugin can read it for the main builds.
  {
    input: 'lib/pool-worker.js',
    output: {
      file: WORKER_FILE,
      format: 'es',
      sourcemap: false
    },
    plugins: [
      nodeResolve()
    ],
    external: []
  },
  // UMD build (works with require() and <script> tag global)
  {
    input: 'lib/index.js',
    output: {
      file: 'dist/prores-encoder.js',
      format: 'umd',
      name: 'ProRes',
      sourcemap: true
    },
    plugins: [
      nodeResolve(),
      inlineWorker(WORKER_FILE),
      copyTypes()
    ],
    external: []
  },
  // UMD build (minified, for CDN <script> tag)
  {
    input: 'lib/index.js',
    output: {
      file: 'dist/prores-encoder.min.js',
      format: 'umd',
      name: 'ProRes',
      sourcemap: true
    },
    plugins: [
      nodeResolve(),
      inlineWorker(WORKER_FILE),
      terser()
    ],
    external: []
  },
  // ESM build (for npm import)
  {
    input: 'lib/index.js',
    output: {
      file: 'dist/prores-encoder.esm.js',
      format: 'es',
      sourcemap: true
    },
    plugins: [
      nodeResolve(),
      inlineWorker(WORKER_FILE)
    ],
    external: []
  }
];
