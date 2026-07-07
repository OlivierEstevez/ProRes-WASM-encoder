/**
 * ProRes encoder worker body.
 *
 * One WASM encoder instance per worker. The worker only ENCODES — it never
 * muxes. It receives an RGBA frame, produces one raw ProRes packet, and
 * transfers the packet back (plus the now-free RGBA buffer, for reuse).
 *
 * This file is bundled (Emscripten glue + this logic, WITHOUT the wasm
 * binary) into dist/prores-worker.js, which is embedded as a string and
 * spawned from a Blob URL — no bundler configuration needed. The compiled
 * WebAssembly.Module arrives in the init message (structured clone), so
 * the binary ships and compiles exactly once for the whole pool.
 */
import createProResModule from '../dist/prores-encoder.wasm.js';

/**
 * Create an independent worker core with its own encoder instance.
 * `handle(msg, post)` processes one message; `post(message, transfer)`
 * sends a reply. Kept instance-based so several cores can coexist in one
 * JS realm (used by the in-process test harness).
 */
export function createWorkerCore() {
  let mod = null;
  let ctx = 0;
  let rgbaPtr = 0;
  let rgbaSize = 0;

  async function handle(msg, post) {
    try {
      if (msg.type === 'init') {
        if (!msg.wasmModule) {
          throw new Error('worker: init message is missing the compiled wasmModule');
        }
        mod = await createProResModule({
          // Never fetched (instantiateWasm below), but without locateFile
          // the glue computes new URL(..., import.meta.url), which throws
          // inside a Blob-URL worker (blob: is not a valid URL base).
          locateFile: (path) => path,
          instantiateWasm(imports, successCallback) {
            WebAssembly.instantiate(msg.wasmModule, imports).then((instance) => {
              successCallback(instance, msg.wasmModule);
            });
            return {};
          },
        });
        ctx = mod._prores_wasm_create(
          msg.width, msg.height, msg.fpsNum, msg.fpsDen, msg.profile, msg.range
        );
        if (!ctx) throw new Error('worker: failed to create encoder');
        rgbaSize = mod._prores_wasm_get_rgba_buffer_size(msg.width, msg.height);
        rgbaPtr = mod._prores_wasm_alloc(rgbaSize);
        if (!rgbaPtr) throw new Error('worker: failed to allocate RGBA buffer');
        post({ type: 'ready' });
      } else if (msg.type === 'encode') {
        const rgba = new Uint8Array(msg.rgba);
        if (rgba.length !== rgbaSize) {
          throw new Error(`worker: RGBA size ${rgba.length} != expected ${rgbaSize}`);
        }
        mod.HEAPU8.set(rgba, rgbaPtr);

        const packetPtr = mod._prores_wasm_encode_frame_rgba(ctx, rgbaPtr);
        if (!packetPtr) throw new Error(`worker: encode failed for frame ${msg.frameIndex}`);
        const size = mod._prores_wasm_last_frame_size(ctx);

        // Copy the packet out of the WASM heap (the slot is reused next encode).
        const packet = new Uint8Array(size);
        packet.set(new Uint8Array(mod.HEAPU8.buffer, packetPtr, size));

        // Transfer the packet out and hand the input buffer back for reuse.
        post(
          { type: 'packet', frameIndex: msg.frameIndex, packet: packet.buffer, rgba: msg.rgba },
          [packet.buffer, msg.rgba]
        );
      } else if (msg.type === 'destroy') {
        if (mod && ctx) mod._prores_wasm_destroy(ctx);
        ctx = 0;
        rgbaPtr = 0;
        post({ type: 'destroyed' });
      }
    } catch (err) {
      post({ type: 'error', frameIndex: msg && msg.frameIndex, error: String(err && err.message || err) });
    }
  }

  return { handle };
}

/** Wire a core to a Worker-like scope (self, in a real Worker). */
export function installWorkerHandlers(scope) {
  const core = createWorkerCore();
  scope.onmessage = (e) => core.handle(e.data, (m, t) => scope.postMessage(m, t));
}

// In a real Worker, `self` is the global scope.
if (typeof self !== 'undefined' && typeof self.postMessage === 'function') {
  installWorkerHandlers(self);
}
