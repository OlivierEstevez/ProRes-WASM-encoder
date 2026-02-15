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
   * Finalize encoding and get the MOV file data
   * @returns MOV file data
   */
  finalize(): Uint8Array;

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
export declare function downloadMov(movData: Uint8Array, filename?: string): void;

/**
 * Convert MOV data to a Blob
 */
export declare function movToBlob(movData: Uint8Array): Blob;

/**
 * Convert MOV data to an Object URL
 */
export declare function movToObjectUrl(movData: Uint8Array): string;

export default createProResEncoder;
