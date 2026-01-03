import { nodeResolve } from '@rollup/plugin-node-resolve';

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
    nodeResolve()
  ],
  external: []
};

