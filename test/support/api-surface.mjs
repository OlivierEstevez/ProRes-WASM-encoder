/**
 * The documented public API: every name users (and the README, and the
 * video-export skill) may import from each entry point. package.test.mjs
 * checks the PACKED tarball against this list, so an entry or export
 * missing from what npm would publish fails the build.
 */
export const API_SURFACE = {
  'prores-wasm-encoder': {
    functions: ['createProResEncoder', 'downloadMov', 'movToBlob', 'movToObjectUrl', 'default'],
    classes: ['ProResEncoder'],
    objects: ['ProResProfile', 'ProfileNames'],
    cjs: true,
  },
  'prores-wasm-encoder/parallel': {
    functions: ['createProResEncoderPool', 'default'],
    classes: ['ProResEncoderPool'],
    objects: ['ProResProfile', 'ProfileNames'],
    cjs: true,
  },
  'prores-wasm-encoder/mediabunny': {
    functions: ['registerProResEncoder', 'default'],
    classes: ['ProResVideoEncoder'],
    objects: ['ProResProfile'],
    cjs: false, // ESM-only, like MediaBunny itself
  },
};

/** Instance methods users call on each encoder class. */
export const METHODS = {
  ProResEncoder: [
    'initialize', 'addFrameRgba', 'addFrameFromImageData', 'addFrameFromCanvas',
    'finalize', 'finalizeToBlob', 'finalizeHeaders', 'destroy',
  ],
  ProResEncoderPool: [
    'addFrameRgba', 'addFrameFromImageData', 'addFrameFromCanvas', 'flush',
    'finalize', 'finalizeToBlob', 'finalizeStreaming', 'finalizeHeaders', 'destroy',
  ],
};
