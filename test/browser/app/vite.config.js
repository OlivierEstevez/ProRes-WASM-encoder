// The README's recommended setup: pre-bundle the lazily imported entries so
// Vite's dev server doesn't discover them mid-export and reload the page.
export default {
  optimizeDeps: {
    include: [
      'prores-wasm-encoder',
      'prores-wasm-encoder/parallel',
      'prores-wasm-encoder/mediabunny',
      'mediabunny',
    ],
  },
};
