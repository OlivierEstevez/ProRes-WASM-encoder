/**
 * prores-wasm-encoder/parallel — frame-parallel encoding entry point.
 */

import type { ProResEncoderOptions } from './prores-encoder';

export { ProResProfile, ProfileNames } from './prores-encoder';
export type { ProResProfileType, ProResEncoderOptions } from './prores-encoder';

/**
 * Options for the frame-parallel encoder pool.
 */
export interface ProResEncoderPoolOptions extends ProResEncoderOptions {
  /** Number of worker threads. Default: min(hardwareConcurrency, 8). */
  workers?: number;
}

/**
 * Frame-parallel ProRes encoder pool. ProRes is intra-only, so frames encode
 * in parallel across Web Workers with byte-identical output to the
 * single-thread encoder. All frame-adding methods are async.
 */
export declare class ProResEncoderPool {
  /** Frames fully encoded and recorded so far */
  readonly frameCount: number;
  readonly width: number;
  readonly height: number;
  /** Number of active worker threads */
  readonly workerCount: number;

  /** Submit a frame from RGBA data. Resolves when accepted (backpressured). */
  addFrameRgba(rgbaData: Uint8Array | Uint8ClampedArray): Promise<void>;
  /** Submit a frame from ImageData. */
  addFrameFromImageData(imageData: ImageData): Promise<void>;
  /** Submit a frame from a canvas. */
  addFrameFromCanvas(canvas: HTMLCanvasElement | OffscreenCanvas): Promise<void>;

  /** Wait for all frames, then return the whole MOV file. */
  finalize(): Promise<Uint8Array>;
  /** Wait for all frames, then return the MOV file as a Blob. */
  finalizeToBlob(): Promise<Blob>;
  /** Streaming mode: wait for all frames (delivered via onFrameData), then
   * return the header/moov to write around your chunks. */
  finalizeStreaming(): Promise<{ header: Uint8Array; moov: Uint8Array }>;
  /** Get the header/moov segments (frames must already be flushed). */
  finalizeHeaders(): { header: Uint8Array; moov: Uint8Array };

  /** Terminate workers and free resources. */
  destroy(): Promise<void>;
}

/**
 * Create a frame-parallel ProRes encoder pool. The WASM binary is compiled
 * once and shared with every worker. Requires Web Workers with module
 * support (Chrome 80+, Safari 15+, Firefox 114+). There is no automatic
 * fallback — use createProResEncoder() from 'prores-wasm-encoder' where
 * workers are unavailable.
 */
export declare function createProResEncoderPool(
  options: ProResEncoderPoolOptions
): Promise<ProResEncoderPool>;

export default createProResEncoderPool;
