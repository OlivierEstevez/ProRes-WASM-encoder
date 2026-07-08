import { nodeResolve } from '@rollup/plugin-node-resolve';
import terser from '@rollup/plugin-terser';
import { copyFileSync, mkdirSync, readFileSync } from 'fs';
import { resolve, dirname } from 'path';

function copyTypes() {
  return {
    name: 'copy-types',
    writeBundle() {
      const copies = [
        ['lib/index.d.ts', 'dist/prores-encoder.d.ts'],
        ['lib/parallel.d.ts', 'dist/prores-encoder-parallel.d.ts'],
        ['lib/mediabunny.d.ts', 'dist/prores-encoder-mediabunny.d.ts'],
      ];
      for (const [src, dest] of copies) {
        mkdirSync(dirname(resolve(dest)), { recursive: true });
        copyFileSync(resolve(src), resolve(dest));
      }
    }
  };
}

// Resolves the virtual `prores:wasm` import to the .wasm binary produced by
// the Emscripten build, as a default-exported base64 string. Embedded
// exactly once per bundle; workers receive the compiled module instead.
function inlineWasm() {
  const ID = 'prores:wasm';
  const RESOLVED = '\0' + ID;
  return {
    name: 'inline-wasm',
    resolveId(id) {
      return id === ID ? RESOLVED : null;
    },
    load(id) {
      if (id === RESOLVED) {
        const bin = readFileSync(resolve('dist/prores-encoder.core.wasm'));
        return `export default ${JSON.stringify(bin.toString('base64'))};`;
      }
      return null;
    }
  };
}

// Resolves the virtual `prores:worker` import to the pre-built worker bundle
// (glue + encode loop, no wasm payload) as a default-exported string. The
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

const umd = (input, file, name, minify) => ({
  input,
  output: { file, format: 'umd', name, sourcemap: true, exports: 'named' },
  plugins: [
    nodeResolve(),
    inlineWasm(),
    inlineWorker(WORKER_FILE),
    ...(minify ? [terser()] : []),
  ],
  external: []
});

export default [
  // Build 0: worker bundle (Emscripten glue + encode loop, NO wasm binary —
  // the compiled module is shared via postMessage). Minified because it is
  // embedded as a string in the parallel entry. Must be first so the
  // inline-worker plugin can read it.
  {
    input: 'lib/pool-worker.js',
    output: { file: WORKER_FILE, format: 'es', sourcemap: false },
    plugins: [nodeResolve(), terser()],
    external: []
  },
  // ESM builds: both entries in one pass so shared code (Emscripten glue,
  // embedded wasm, encoder class) lands in a single shared chunk instead of
  // being duplicated. Apps importing both entries load the shared chunk once.
  {
    input: {
      'prores-encoder.esm': 'lib/index.js',
      'prores-encoder-parallel.esm': 'lib/parallel.js',
      'prores-encoder-mediabunny.esm': 'lib/mediabunny.js',
    },
    output: {
      dir: 'dist',
      format: 'es',
      entryFileNames: '[name].js',
      chunkFileNames: 'prores-core.js',
      sourcemap: true
    },
    plugins: [
      nodeResolve(),
      inlineWasm(),
      inlineWorker(WORKER_FILE),
      copyTypes()
    ],
    // MediaBunny is a peer dependency of the /mediabunny entry — never
    // bundle it (the other entries don't import it).
    external: ['mediabunny']
  },
  // UMD builds (self-contained; for require() and <script>/CDN use)
  umd('lib/index.js', 'dist/prores-encoder.js', 'ProRes', false),
  umd('lib/index.js', 'dist/prores-encoder.min.js', 'ProRes', true),
  umd('lib/parallel.js', 'dist/prores-encoder-parallel.js', 'ProResParallel', false),
  umd('lib/parallel.js', 'dist/prores-encoder-parallel.min.js', 'ProResParallel', true),
];
