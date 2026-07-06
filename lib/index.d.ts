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
