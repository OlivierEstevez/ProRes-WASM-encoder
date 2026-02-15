import { nodeResolve } from '@rollup/plugin-node-resolve';
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

export default {
  input: 'lib/index.js',
  output: [
    {
      file: 'dist/prores-encoder.esm.js',
      format: 'es',
      sourcemap: true
    },
    {
      file: 'dist/prores-encoder.js',
      format: 'cjs',
      sourcemap: true,
      exports: 'named'
    }
  ],
  plugins: [
    nodeResolve(),
    copyTypes()
  ],
  external: []
};

