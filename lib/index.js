/**
 * ProRes WASM Encoder
 * WebAssembly-based ProRes encoder for browser canvas encoding
 */

// Import the WASM module (will be bundled)
import createProResModule from '../dist/prores-encoder.wasm.js';

/**
 * ProRes profile constants
 */
export const ProResProfile = {
  PROXY: 0,      // ProRes 422 Proxy (~45 Mbps @ 1080p30)
  LT: 1,         // ProRes 422 LT (~102 Mbps @ 1080p30)
  STANDARD: 2,   // ProRes 422 Standard (~147 Mbps @ 1080p30)
  HQ: 3,         // ProRes 422 HQ (~220 Mbps @ 1080p30)
  P4444: 4,      // ProRes 4444 (~330 Mbps @ 1080p30)
  P4444XQ: 5,    // ProRes 4444 XQ (~500 Mbps @ 1080p30)
};

/**
 * Profile names for display
 */
export const ProfileNames = {
  [ProResProfile.PROXY]: 'ProRes 422 Proxy',
  [ProResProfile.LT]: 'ProRes 422 LT',
  [ProResProfile.STANDARD]: 'ProRes 422',
  [ProResProfile.HQ]: 'ProRes 422 HQ',
  [ProResProfile.P4444]: 'ProRes 4444',
  [ProResProfile.P4444XQ]: 'ProRes 4444 XQ',
};

/**
 * Standard frame rate lookup table (value → num/den)
 */
const STANDARD_FRAME_RATES = [
  { value: 23.976, num: 24000, den: 1001 },
  { value: 29.97,  num: 30000, den: 1001 },
  { value: 59.94,  num: 60000, den: 1001 },
];

const FRAME_RATE_EPSILON = 0.01;

/**
 * Convert a frame rate number to a num/den pair.
 * Matches standard rates within epsilon, otherwise uses continued fraction approximation.
 */
function frameRateToFraction(fps) {
  // Check standard rates first
  for (const std of STANDARD_FRAME_RATES) {
    if (Math.abs(fps - std.value) < FRAME_RATE_EPSILON) {
      return { num: std.num, den: std.den };
    }
  }

  // Integer frame rates
  if (Number.isInteger(fps)) {
    return { num: fps, den: 1 };
  }

  // Continued fraction approximation (max denominator 1001)
  const maxDen = 1001;
  let bestNum = Math.round(fps);
  let bestDen = 1;
  let bestErr = Math.abs(fps - bestNum);

  for (let den = 2; den <= maxDen; den++) {
    const num = Math.round(fps * den);
    const err = Math.abs(fps - num / den);
    if (err < bestErr) {
      bestNum = num;
      bestDen = den;
      bestErr = err;
      if (err === 0) break;
    }
  }

  return { num: bestNum, den: bestDen };
}

/**
 * ProRes Encoder class
 */
export class ProResEncoder {
  constructor(module) {
    this._module = module;
    this._ctx = null;
    this._rgbaPtr = 0;
    this._rgbaSize = 0;
    this._initialized = false;
    this._frameCount = 0;
    this._chunks = [];
    this._onFrameData = null;
  }

  /**
   * Initialize the encoder with given options
   * @param {Object} options - Encoder options
   * @param {number} options.width - Frame width in pixels
   * @param {number} options.height - Frame height in pixels
   * @param {number} [options.frameRate=30] - Frame rate (e.g. 23.976, 30, 60)
   * @param {number} [options.frameRateNum] - Frame rate numerator (advanced, overrides frameRate)
   * @param {number} [options.frameRateDen] - Frame rate denominator (advanced, overrides frameRate)
   * @param {number} [options.profile=ProResProfile.HQ] - ProRes profile
   * @param {function} [options.onFrameData] - Streaming mode: called with a
   *   Uint8Array of each frame's encoded bytes instead of buffering them.
   *   Write the chunks to disk/OPFS in order, then assemble the file as
   *   [header, ...chunks, moov] using finalizeHeaders(). Memory usage stays
   *   constant regardless of recording length.
   */
  initialize(options) {
    if (this._initialized) {
      throw new Error('Encoder already initialized');
    }

    const {
      width,
      height,
      frameRate,
      frameRateNum: explicitNum,
      frameRateDen: explicitDen,
      profile = ProResProfile.HQ,
      range = 'limited',
      onFrameData = null,
    } = options;

    // Resolve frame rate: explicit num/den takes precedence over frameRate
    let frameRateNum, frameRateDen;
    if (explicitNum !== undefined && explicitDen !== undefined) {
      frameRateNum = explicitNum;
      frameRateDen = explicitDen;
    } else if (frameRate !== undefined) {
      const frac = frameRateToFraction(frameRate);
      frameRateNum = frac.num;
      frameRateDen = frac.den;
    } else {
      frameRateNum = 30;
      frameRateDen = 1;
    }

    if (width <= 0 || height <= 0) {
      throw new Error('Width and height must be positive');
    }

    // Create encoder context
    const rangeValue = range === 'full' ? 1 : 0;
    this._ctx = this._module._prores_wasm_create(
      width, height,
      frameRateNum, frameRateDen,
      profile,
      rangeValue
    );

    if (!this._ctx) {
      throw new Error('Failed to create encoder');
    }

    // Allocate RGBA buffer in WASM memory
    const rgbaSize = this._module._prores_wasm_get_rgba_buffer_size(width, height);
    this._rgbaPtr = this._module._prores_wasm_alloc(rgbaSize);
    if (!this._rgbaPtr) {
      this._module._prores_wasm_destroy(this._ctx);
      this._ctx = null;
      throw new Error('Failed to allocate RGBA buffer');
    }

    this._rgbaSize = rgbaSize;
    this._width = width;
    this._height = height;
    this._profile = profile;
    this._initialized = true;
    this._frameCount = 0;
    this._chunks = [];
    this._onFrameData = onFrameData;
  }

  /**
   * Add a frame from RGBA data
   * @param {Uint8Array|Uint8ClampedArray} rgbaData - RGBA pixel data (width * height * 4 bytes)
   */
  addFrameRgba(rgbaData) {
    if (!this._initialized) {
      throw new Error('Encoder not initialized');
    }

    const expectedSize = this._rgbaSize || (this._width * this._height * 4);
    if (rgbaData.length !== expectedSize) {
      throw new Error(`Invalid RGBA data size: expected ${expectedSize}, got ${rgbaData.length}`);
    }

    // Copy data to WASM memory (refresh heap view in case memory grew)
    const heap = this._module.HEAPU8;
    const end = this._rgbaPtr + rgbaData.length;
    if (end > heap.length) {
      throw new Error(`WASM heap too small: need ${end} bytes, heap size ${heap.length}`);
    }
    heap.set(rgbaData, this._rgbaPtr);

    // Encode one frame; the packet lives in WASM memory until the next
    // encode call, so copy it out immediately. Frame data accumulates on
    // the JS side (or streams to the caller), never in the WASM heap.
    const packetPtr = this._module._prores_wasm_encode_frame_rgba(this._ctx, this._rgbaPtr);
    if (!packetPtr) {
      throw new Error(`Failed to encode frame ${this._frameCount} (DCT/VLC stage)`);
    }
    const packetSize = this._module._prores_wasm_last_frame_size(this._ctx);

    const chunk = new Uint8Array(packetSize);
    chunk.set(new Uint8Array(this._module.HEAPU8.buffer, packetPtr, packetSize));

    // Record the sample in the MOV index (16 bytes of bookkeeping)
    const rec = this._module._prores_wasm_mux_record_sample(this._ctx, packetSize);
    if (rec < 0) {
      throw new Error(`Failed to record frame ${this._frameCount} in MOV index`);
    }

    if (this._onFrameData) {
      this._onFrameData(chunk);
    } else {
      this._chunks.push(chunk);
    }

    this._frameCount++;
  }

  /**
   * Add a frame from ImageData (e.g., from canvas.getImageData())
   * @param {ImageData} imageData - Canvas ImageData object
   */
  addFrameFromImageData(imageData) {
    if (imageData.width !== this._width || imageData.height !== this._height) {
      throw new Error(`ImageData dimensions (${imageData.width}x${imageData.height}) don't match encoder (${this._width}x${this._height})`);
    }
    this.addFrameRgba(imageData.data);
  }

  /**
   * Add a frame directly from a canvas
   * @param {HTMLCanvasElement|OffscreenCanvas} canvas - Canvas element
   */
  addFrameFromCanvas(canvas) {
    const ctx = canvas.getContext('2d');
    const imageData = ctx.getImageData(0, 0, this._width, this._height);
    this.addFrameRgba(imageData.data);
  }

  /**
   * Get the MOV file header and moov box (the bytes that go before and
   * after the frame data). Used with the onFrameData streaming mode:
   * the final file is [header, ...frame chunks in order, moov].
   * @returns {{header: Uint8Array, moov: Uint8Array}}
   */
  finalizeHeaders() {
    if (!this._initialized) {
      throw new Error('Encoder not initialized');
    }
    if (this._frameCount === 0) {
      throw new Error('No frames encoded');
    }

    const sizePtr = this._module._malloc(8);
    try {
      const headerPtr = this._module._prores_wasm_finalize_header(this._ctx, sizePtr);
      if (!headerPtr) throw new Error('Failed to finalize MOV header');
      const headerSize = this._module.HEAPU32[sizePtr >> 2];
      const header = new Uint8Array(headerSize);
      header.set(new Uint8Array(this._module.HEAPU8.buffer, headerPtr, headerSize));

      const moovPtr = this._module._prores_wasm_finalize_moov(this._ctx, sizePtr);
      if (!moovPtr) throw new Error('Failed to finalize MOV moov box');
      const moovSize = this._module.HEAPU32[sizePtr >> 2];
      const moov = new Uint8Array(moovSize);
      moov.set(new Uint8Array(this._module.HEAPU8.buffer, moovPtr, moovSize));

      return { header, moov };
    } finally {
      this._module._free(sizePtr);
    }
  }

  /**
   * Finalize encoding and get the MOV file data
   * @returns {Uint8Array} - MOV file data
   */
  finalize() {
    if (this._onFrameData) {
      throw new Error(
        'finalize() is unavailable in streaming mode (onFrameData): ' +
        'assemble [header, ...your chunks, moov] from finalizeHeaders()'
      );
    }

    const { header, moov } = this.finalizeHeaders();

    let total = header.length + moov.length;
    for (const c of this._chunks) total += c.length;

    const movData = new Uint8Array(total);
    let off = 0;
    movData.set(header, off); off += header.length;
    for (const c of this._chunks) {
      movData.set(c, off);
      off += c.length;
    }
    movData.set(moov, off);

    return movData;
  }

  /**
   * Finalize encoding and get the MOV file as a Blob. Preferred for long
   * recordings: Blob parts are browser-managed (and may be disk-backed),
   * so no single contiguous allocation of the whole file is needed.
   * @returns {Blob} - MOV file blob
   */
  finalizeToBlob() {
    if (this._onFrameData) {
      throw new Error(
        'finalizeToBlob() is unavailable in streaming mode (onFrameData): ' +
        'assemble [header, ...your chunks, moov] from finalizeHeaders()'
      );
    }

    const { header, moov } = this.finalizeHeaders();
    return new Blob([header, ...this._chunks, moov], { type: 'video/quicktime' });
  }

  /**
   * Get the number of frames encoded so far
   * @returns {number}
   */
  get frameCount() {
    return this._frameCount;
  }

  /**
   * Get encoder width
   * @returns {number}
   */
  get width() {
    return this._width;
  }

  /**
   * Get encoder height
   * @returns {number}
   */
  get height() {
    return this._height;
  }

  /**
   * Check if encoder is initialized
   * @returns {boolean}
   */
  get initialized() {
    return this._initialized;
  }

  /**
   * Destroy the encoder and free resources
   */
  destroy() {
    if (this._rgbaPtr) {
      this._module._free(this._rgbaPtr);
      this._rgbaPtr = 0;
      this._rgbaBuffer = null;
    }

    if (this._ctx) {
      this._module._prores_wasm_destroy(this._ctx);
      this._ctx = null;
    }

    this._chunks = [];
    this._onFrameData = null;
    this._initialized = false;
  }
}

/**
 * Create a new ProRes encoder
 * @returns {Promise<ProResEncoder>} - Encoder instance
 */
export async function createProResEncoder() {
  const module = await createProResModule();
  return new ProResEncoder(module);
}

/**
 * Helper: Save MOV data as a downloadable file
 * @param {Uint8Array|Blob} movData - MOV file data (Uint8Array or Blob)
 * @param {string} [filename='output.mov'] - Download filename
 */
export function downloadMov(movData, filename = 'output.mov') {
  const blob = movData instanceof Blob ? movData : new Blob([movData], { type: 'video/quicktime' });
  const url = URL.createObjectURL(blob);
  const a = document.createElement('a');
  a.href = url;
  a.download = filename;
  a.click();
  URL.revokeObjectURL(url);
}

/**
 * Helper: Convert MOV data to a Blob
 * @param {Uint8Array} movData - MOV file data
 * @returns {Blob}
 */
export function movToBlob(movData) {
  return new Blob([movData], { type: 'video/quicktime' });
}

/**
 * Helper: Convert MOV data to an Object URL
 * @param {Uint8Array} movData - MOV file data
 * @returns {string} - Object URL (remember to revoke when done)
 */
export function movToObjectUrl(movData) {
  return URL.createObjectURL(movToBlob(movData));
}

// Default export
export default createProResEncoder;
