import { nodeResolve } from '@rollup/plugin-node-resolve';
import terser from '@rollup/plugin-terser';
import { copyFileSync, mkdirSync } from 'fs';
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

export default [
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
      nodeResolve()
    ],
    external: []
  }
];
