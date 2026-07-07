/**
 * ProRes WASM Encoder TypeScript Declarations
 */

/**
 * ProRes profile constants
 */
export declare const ProResProfile: {
  readonly PROXY: 0;
  readonly LT: 1;
  readonly STANDARD: 2;
  readonly HQ: 3;
  readonly P4444: 4;
  readonly P4444XQ: 5;
};

export type ProResProfileType = typeof ProResProfile[keyof typeof ProResProfile];

/**
 * Profile display names
 */
export declare const ProfileNames: {
  readonly [key in ProResProfileType]: string;
};

/**
 * Encoder initialization options
 */
export interface ProResEncoderOptions {
  /** Frame width in pixels */
  width: number;
  /** Frame height in pixels */
  height: number;
  /** Frame rate (e.g. 23.976, 30, 60). Standard rates like 23.976 and 29.97 are mapped to exact num/den pairs. Default: 30 */
  frameRate?: number;
  /** Frame rate numerator — advanced override, takes precedence over frameRate when both num and den are provided */
  frameRateNum?: number;
  /** Frame rate denominator — advanced override, takes precedence over frameRate when both num and den are provided */
  frameRateDen?: number;
  /** ProRes profile (default: HQ) */
  profile?: ProResProfileType;
  /** Color range (default: "limited") */
  range?: 'full' | 'limited';
  /**
   * Streaming mode: called with each frame's encoded bytes instead of
   * buffering them internally. Write the chunks to disk/OPFS in order and
   * assemble the final file as [header, ...chunks, moov] via
   * finalizeHeaders(). Memory usage stays constant regardless of
   * recording length. finalize()/finalizeToBlob() are unavailable in
   * this mode.
   */
  onFrameData?: (chunk: Uint8Array) => void;
}

/**
 * ProRes Encoder class
 */
export declare class ProResEncoder {
  /** Number of frames encoded */
  readonly frameCount: number;
  /** Encoder width */
  readonly width: number;
  /** Encoder height */
  readonly height: number;
  /** Whether encoder is initialized */
  readonly initialized: boolean;

  /**
   * Initialize the encoder with given options
   */
  initialize(options: ProResEncoderOptions): void;

  /**
   * Add a frame from RGBA data
   * @param rgbaData - RGBA pixel data (width * height * 4 bytes)
   */
  addFrameRgba(rgbaData: Uint8Array | Uint8ClampedArray): void;

  /**
   * Add a frame from ImageData (e.g., from canvas.getImageData())
   */
  addFrameFromImageData(imageData: ImageData): void;

  /**
   * Add a frame directly from a canvas
   */
  addFrameFromCanvas(canvas: HTMLCanvasElement | OffscreenCanvas): void;

  /**
   * Finalize encoding and get the MOV file data.
   * Unavailable in streaming mode (onFrameData).
   * @returns MOV file data
   */
  finalize(): Uint8Array;

  /**
   * Finalize encoding and get the MOV file as a Blob. Preferred for long
   * recordings (no single contiguous allocation of the whole file).
   * Unavailable in streaming mode (onFrameData).
   */
  finalizeToBlob(): Blob;

  /**
   * Get the MOV header and moov box for streaming-mode assembly:
   * the final file is [header, ...frame chunks in order, moov].
   */
  finalizeHeaders(): { header: Uint8Array; moov: Uint8Array };

  /**
   * Destroy the encoder and free resources
   */
  destroy(): void;
}

/**
 * Create a new ProRes encoder
 */
export declare function createProResEncoder(): Promise<ProResEncoder>;

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
 * Create a frame-parallel ProRes encoder pool. Requires Web Workers with
 * module support (Chrome 80+, Safari 15+, Firefox 114+). There is no
 * automatic fallback — use createProResEncoder() where workers are
 * unavailable.
 */
export declare function createProResEncoderPool(
  options: ProResEncoderPoolOptions
): Promise<ProResEncoderPool>;

/**
 * Save MOV data as a downloadable file
 */
export declare function downloadMov(movData: Uint8Array | Blob, filename?: string): void;

/**
 * Convert MOV data to a Blob
 */
export declare function movToBlob(movData: Uint8Array): Blob;

/**
 * Convert MOV data to an Object URL
 */
export declare function movToObjectUrl(movData: Uint8Array): string;

export default createProResEncoder;
