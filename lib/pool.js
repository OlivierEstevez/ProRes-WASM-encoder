/**
 * Frame-parallel ProRes encoder pool.
 *
 * ProRes is intra-only, so every frame is independent — encoding frame N in
 * isolation is byte-identical to encoding it as the Nth frame of a
 * sequential stream (verified across all profiles). This pool exploits that:
 * N workers each hold their own WASM encoder instance and encode frames in
 * parallel, while the main thread records samples in frame order and reuses
 * the C muxer for the container. Output is bit-identical to the
 * single-thread encoder.
 *
 * This module is transport-agnostic: the caller supplies `module` (a loaded
 * WASM module used only for muxing) and `spawnWorker` (a factory returning a
 * postMessage/onmessage handle). parallel.js wires the default Blob-URL
 * Worker transport; tests use Node worker_threads or in-process fakes.
 */

import { validateOptions, validateWorkers } from './options.js';
import { readCanvasRgba } from './canvas.js';

export class ProResEncoderPool {
  static async create(options) {
    const pool = new ProResEncoderPool(options);
    try {
      await pool._init();
    } catch (err) {
      // Don't leak workers (or the muxer context) that did start.
      await pool.destroy();
      throw err;
    }
    return pool;
  }

  constructor(options) {
    const { module, wasmModule = null, spawnWorker } = options;

    if (!module) throw new Error('pool: a loaded WASM module is required for muxing');
    if (typeof spawnWorker !== 'function') throw new Error('pool: spawnWorker factory is required');

    const {
      width, height, fpsNum: num, fpsDen: den, profile, rangeValue, onFrameData,
    } = validateOptions(options);
    const workers = validateWorkers(options.workers);

    this._module = module;
    this._wasmModule = wasmModule;
    this._spawnWorker = spawnWorker;
    this._width = width;
    this._height = height;
    this._fpsNum = num;
    this._fpsDen = den;
    this._profile = profile;
    this._rangeValue = rangeValue;
    this._onFrameData = onFrameData;

    const hc = (typeof navigator !== 'undefined' && navigator.hardwareConcurrency) || 4;
    this._workerCount = workers ?? Math.min(hc, 8);

    this._rgbaSize = width * height * 4;
    this._scratch = { canvas: null, ctx: null };
    this._muxCtx = 0;

    // Worker handles and scheduling state.
    this._workers = [];
    this._idle = [];
    this._queue = [];          // frames awaiting a free worker
    this._freeBuffers = [];    // reusable RGBA ArrayBuffers

    // Reorder + accounting. Frames _nextRecord.._submitted-1 are unrecorded:
    // queued, in a worker, or encoded and waiting for an earlier frame.
    this._submitted = 0;       // frames accepted from the caller
    this._maxUnrecorded = this._workerCount * 2;
    this._nextRecord = 0;      // next frame index to record in order
    this._packets = new Map(); // frameIndex -> Uint8Array, awaiting in-order record
    this._chunks = [];         // buffered-mode ordered chunks
    this._frameCount = 0;      // frames fully recorded

    this._backpressure = [];   // resolvers waiting for frames to be recorded
    this._error = null;
    this._destroyed = false;
    this._finalized = false;
  }

  get frameCount() { return this._frameCount; }
  get width() { return this._width; }
  get height() { return this._height; }
  get workerCount() { return this._workerCount; }

  async _init() {
    // Main-thread context: used ONLY for muxing (record_sample + finalize).
    this._muxCtx = this._module._prores_wasm_create(
      this._width, this._height, this._fpsNum, this._fpsDen,
      this._profile, this._rangeValue
    );
    if (!this._muxCtx) throw new Error('pool: failed to create muxer context');

    // Spawn and initialize all workers.
    await Promise.all(
      Array.from({ length: this._workerCount }, () => this._startWorker())
    );
  }

  _startWorker() {
    return new Promise((resolve, reject) => {
      const worker = this._spawnWorker();
      worker.onmessage = (e) => this._onWorkerMessage(worker, e.data);
      worker.onerror = (e) => {
        if (e && typeof e.preventDefault === 'function') e.preventDefault();
        const detail = (e && e.message) || 'the worker failed to load';
        const hint = worker._readyResolve
          ? ' (if your Content-Security-Policy blocks blob: workers, add "worker-src blob:" ' +
            'or use createProResEncoder() from "prores-wasm-encoder")'
          : '';
        this._fail(new Error(`pool: worker error: ${detail}${hint}`));
      };
      worker._readyResolve = resolve;
      worker._readyReject = reject;
      this._workers.push(worker);

      worker.postMessage({
        type: 'init',
        width: this._width, height: this._height,
        fpsNum: this._fpsNum, fpsDen: this._fpsDen,
        profile: this._profile, range: this._rangeValue,
        // Compiled WebAssembly.Module, shared with every worker via
        // structured clone (no recompile, no second copy of the binary).
        wasmModule: this._wasmModule,
      });
    });
  }

  _onWorkerMessage(worker, msg) {
    // After destroy(), only the workers' shutdown acknowledgements matter.
    if (this._destroyed && msg.type !== 'destroyed') return;
    if (msg.type === 'ready') {
      this._idle.push(worker);
      if (worker._readyResolve) { worker._readyResolve(); worker._readyResolve = null; }
      this._pump();
    } else if (msg.type === 'packet') {
      // Recycle the returned input buffer.
      if (msg.rgba) this._freeBuffers.push(msg.rgba);
      this._idle.push(worker);
      this._packets.set(msg.frameIndex, new Uint8Array(msg.packet));
      this._drainRecords();
      this._releaseBackpressure();
      this._pump();
    } else if (msg.type === 'error') {
      this._fail(new Error(msg.error));
    } else if (msg.type === 'destroyed') {
      if (worker._destroyResolve) { worker._destroyResolve(); worker._destroyResolve = null; }
    }
  }

  _fail(err) {
    if (!this._error) this._error = err;
    // Reject any pending readiness/backpressure waiters.
    for (const w of this._workers) {
      if (w._readyReject) { w._readyReject(err); w._readyReject = null; }
    }
    this._releaseBackpressure();
  }

  _releaseBackpressure() {
    const waiters = this._backpressure;
    this._backpressure = [];
    for (const r of waiters) r();
  }

  // Move queued frames onto idle workers.
  _pump() {
    while (this._idle.length > 0 && this._queue.length > 0) {
      const worker = this._idle.pop();
      const job = this._queue.shift();
      worker.postMessage(
        { type: 'encode', frameIndex: job.frameIndex, rgba: job.buffer },
        [job.buffer]
      );
    }
  }

  // Record any packets that are now contiguous from _nextRecord.
  _drainRecords() {
    while (this._packets.has(this._nextRecord)) {
      const chunk = this._packets.get(this._nextRecord);
      this._packets.delete(this._nextRecord);

      const rec = this._module._prores_wasm_mux_record_sample(this._muxCtx, chunk.length);
      if (rec < 0) { this._fail(new Error('pool: failed to record sample ' + this._nextRecord)); return; }

      if (this._onFrameData) {
        this._onFrameData(chunk);
      } else {
        this._chunks.push(chunk);
      }
      this._frameCount++;
      this._nextRecord++;
    }
  }

  _takeBuffer() {
    const buf = this._freeBuffers.pop();
    if (buf && buf.byteLength === this._rgbaSize) return buf;
    return new ArrayBuffer(this._rgbaSize);
  }

  /** Throw unless the pool can accept frames. */
  _checkWritable() {
    if (this._error) throw this._error;
    if (this._destroyed) throw new Error('pool: encoder destroyed');
    if (this._finalized) throw new Error('pool: cannot add frames after finalize');
  }

  /** Wait on the backpressure queue while `condition()` holds. */
  async _waitWhile(condition) {
    while (condition() && !this._error && !this._destroyed) {
      await new Promise((r) => this._backpressure.push(r));
    }
    if (this._error) throw this._error;
    if (this._destroyed) throw new Error('pool: encoder destroyed');
  }

  /**
   * Submit one RGBA frame for encoding. Resolves once the frame has been
   * accepted into the pipeline (applying backpressure so at most 2x the
   * worker count of frames are unrecorded at once, bounding memory).
   */
  async addFrameRgba(data) {
    this._checkWritable();
    if (!data || data.length !== this._rgbaSize) {
      throw new Error(`pool: invalid RGBA size ${data && data.length}, expected ${this._rgbaSize}`);
    }

    // Backpressure: count every unrecorded frame, not only those in
    // workers, so a slow frame can't grow the reorder buffer.
    await this._waitWhile(() => this._submitted - this._nextRecord >= this._maxUnrecorded);

    const frameIndex = this._submitted++;

    // Copy the caller's pixels into a transferable buffer we own.
    const buffer = this._takeBuffer();
    new Uint8Array(buffer).set(data);

    const job = { frameIndex, buffer };
    if (this._idle.length > 0) {
      const worker = this._idle.pop();
      worker.postMessage({ type: 'encode', frameIndex, rgba: buffer }, [buffer]);
    } else {
      this._queue.push(job);
    }
  }

  async addFrameFromImageData(imageData) {
    if (!imageData || imageData.width !== this._width || imageData.height !== this._height) {
      const got = imageData ? `${imageData.width}x${imageData.height}` : String(imageData);
      throw new Error(`pool: ImageData ${got} != ${this._width}x${this._height}`);
    }
    return this.addFrameRgba(imageData.data);
  }

  async addFrameFromCanvas(canvas) {
    this._checkWritable();
    // Read synchronously, in the caller's task, so WebGL canvases still hold
    // the frame; the async part (backpressure) comes after.
    return this.addFrameRgba(readCanvasRgba(canvas, this._width, this._height, this._scratch));
  }

  /**
   * Wait until every submitted frame has been encoded and recorded (and, in
   * streaming mode, delivered via onFrameData). Does not finalize anything.
   */
  async flush() {
    await this._waitWhile(() => this._nextRecord < this._submitted);
  }

  finalizeHeaders() {
    if (this._destroyed) throw new Error('pool: encoder destroyed');
    if (this._frameCount === 0) throw new Error('pool: no frames encoded');
    const sizePtr = this._module._malloc(8);
    try {
      const headerPtr = this._module._prores_wasm_finalize_header(this._muxCtx, sizePtr);
      if (!headerPtr) throw new Error('pool: failed to finalize MOV header');
      const headerSize = this._module.HEAPU32[sizePtr >> 2];
      const header = new Uint8Array(headerSize);
      header.set(new Uint8Array(this._module.HEAPU8.buffer, headerPtr, headerSize));

      const moovPtr = this._module._prores_wasm_finalize_moov(this._muxCtx, sizePtr);
      if (!moovPtr) throw new Error('pool: failed to finalize MOV moov box');
      const moovSize = this._module.HEAPU32[sizePtr >> 2];
      const moov = new Uint8Array(moovSize);
      moov.set(new Uint8Array(this._module.HEAPU8.buffer, moovPtr, moovSize));

      return { header, moov };
    } finally {
      this._module._free(sizePtr);
    }
  }

  async finalize() {
    if (this._onFrameData) {
      throw new Error('pool: finalize() is unavailable in streaming mode (onFrameData); use finalizeHeaders()');
    }
    await this._finishFrames();
    const { header, moov } = this.finalizeHeaders();
    const chunks = this._chunks;
    this._chunks = [];

    let total = header.length + moov.length;
    for (const c of chunks) total += c.length;

    const out = new Uint8Array(total);
    let off = 0;
    out.set(header, off); off += header.length;
    for (const c of chunks) { out.set(c, off); off += c.length; }
    out.set(moov, off);
    return out;
  }

  async finalizeToBlob() {
    if (this._onFrameData) {
      throw new Error('pool: finalizeToBlob() is unavailable in streaming mode (onFrameData); use finalizeHeaders()');
    }
    await this._finishFrames();
    const { header, moov } = this.finalizeHeaders();
    const chunks = this._chunks;
    this._chunks = [];
    return new Blob([header, ...chunks, moov], { type: 'video/quicktime' });
  }

  /** Streaming mode: flush all frames (delivered via onFrameData), then
   * return the header/moov to write around them. */
  async finalizeStreaming() {
    await this._finishFrames();
    return this.finalizeHeaders();
  }

  /** Close the pool to new frames, then wait for the in-flight ones. */
  async _finishFrames() {
    if (this._destroyed) throw new Error('pool: encoder destroyed');
    if (this._finalized) throw new Error('pool: already finalized; create a new pool for the next file');
    this._finalized = true;
    await this.flush();
  }

  async destroy() {
    if (this._destroyed) return;
    this._destroyed = true;
    // Anything still waiting (addFrame backpressure, flush, finalize)
    // rejects with "encoder destroyed" instead of hanging.
    this._releaseBackpressure();

    await Promise.all(this._workers.map((w) => new Promise((resolve) => {
      // Fallback in case the worker never replies.
      const timer = setTimeout(resolve, 250);
      w._destroyResolve = () => { clearTimeout(timer); resolve(); };
      try { w.postMessage({ type: 'destroy' }); } catch { w._destroyResolve(); }
    })));

    for (const w of this._workers) {
      if (typeof w.terminate === 'function') w.terminate();
    }
    this._workers = [];
    this._idle = [];

    if (this._muxCtx) {
      this._module._prores_wasm_destroy(this._muxCtx);
      this._muxCtx = 0;
    }
  }
}
