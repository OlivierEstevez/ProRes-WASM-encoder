/**
 * Shared WASM loading helpers.
 *
 * The .wasm binary is embedded exactly ONCE (as base64, injected at build
 * time via the virtual `prores:wasm` import) instead of being baked into
 * the Emscripten glue. Both the single-thread encoder and the worker pool
 * instantiate through Emscripten's `instantiateWasm` hook:
 *
 *  - single-thread: instantiate directly from the embedded bytes
 *  - pool: compile ONCE to a WebAssembly.Module, share it with every
 *    worker via postMessage (compiled modules are structured-cloneable),
 *    so N workers add no extra compiles and no extra copies of the binary
 */
import createProResModule from '../dist/prores-encoder.wasm.js';
import wasmBase64 from 'prores:wasm';

let cachedBytes = null;

/** Decode the embedded base64 wasm binary (cached). */
export function getWasmBytes() {
  if (!cachedBytes) {
    const bin = atob(wasmBase64);
    const bytes = new Uint8Array(bin.length);
    for (let i = 0; i < bin.length; i++) bytes[i] = bin.charCodeAt(i);
    cachedBytes = bytes;
  }
  return cachedBytes;
}

/** Compile the embedded wasm to a shareable WebAssembly.Module. */
export function compileWasmModule() {
  return WebAssembly.compile(getWasmBytes());
}

/**
 * Create an Emscripten module instance from either raw bytes or a
 * pre-compiled WebAssembly.Module.
 */
export function createModuleFrom(source) {
  return createProResModule({
    // Never fetched (instantiateWasm below); providing locateFile keeps the
    // glue from computing new URL(..., import.meta.url), which throws in
    // contexts without a valid URL base (e.g. Blob-URL workers).
    locateFile: (path) => path,
    instantiateWasm(imports, successCallback) {
      WebAssembly.instantiate(source, imports).then((result) => {
        // instantiate(bytes) resolves {module, instance};
        // instantiate(Module) resolves the Instance directly.
        if (result.instance) {
          successCallback(result.instance, result.module);
        } else {
          successCallback(result);
        }
      });
      return {};
    },
  });
}

/** Create a module instance from the embedded bytes (single-thread path). */
export function createDefaultModule() {
  return createModuleFrom(getWasmBytes());
}
