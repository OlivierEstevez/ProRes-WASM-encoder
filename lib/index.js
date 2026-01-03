/**
 * ProRes WASM Encoder
 * WebAssembly-based ProRes encoder for browser canvas encoding
 */

// Import the WASM module (will be bundled)
import createProResModule from '../dist/prores-encoder.js';

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
  }

  /**
   * Initialize the encoder with given options
   * @param {Object} options - Encoder options
   * @param {number} options.width - Frame width (must be multiple of 16)
   * @param {number} options.height - Frame height (must be multiple of 16)
   * @param {number} [options.frameRateNum=30] - Frame rate numerator
   * @param {number} [options.frameRateDen=1] - Frame rate denominator
   * @param {number} [options.profile=ProResProfile.HQ] - ProRes profile
   * @param {number} [options.quality=85] - Quality (0-100)
   */
  initialize(options) {
    if (this._initialized) {
      throw new Error('Encoder already initialized');
    }

    const {
      width,
      height,
      frameRateNum = 30,
      frameRateDen = 1,
      profile = ProResProfile.HQ,
      quality = 85,
    } = options;

    // Validate dimensions
    if (width % 16 !== 0 || height % 16 !== 0) {
      throw new Error('Width and height must be multiples of 16');
    }

    if (width <= 0 || height <= 0) {
      throw new Error('Width and height must be positive');
    }

    // Create encoder context
    this._ctx = this._module._prores_wasm_create(
      width, height,
      frameRateNum, frameRateDen,
      profile, quality
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

    // Encode frame
    const result = this._module._prores_wasm_add_frame_rgba(this._ctx, this._rgbaPtr);
    if (result < 0) {
      throw new Error(`Failed to encode frame: error ${result}`);
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
   * Finalize encoding and get the MOV file data
   * @returns {Uint8Array} - MOV file data
   */
  finalize() {
    if (!this._initialized) {
      throw new Error('Encoder not initialized');
    }

    if (this._frameCount === 0) {
      throw new Error('No frames encoded');
    }

    // Allocate size output
    const sizePtr = this._module._malloc(8);  // size_t

    try {
      // Finalize and get MOV data
      const dataPtr = this._module._prores_wasm_finalize(this._ctx, sizePtr);
      if (!dataPtr) {
        throw new Error('Failed to finalize encoding');
      }

      // Read size (assumes 32-bit size_t in WASM)
      const size = this._module.HEAPU32[sizePtr >> 2];

      // Copy data out of WASM memory
      const movData = new Uint8Array(size);
      movData.set(new Uint8Array(this._module.HEAPU8.buffer, dataPtr, size));

      // Free the data buffer
      this._module._prores_wasm_free_buffer(dataPtr);

      return movData;
    } finally {
      this._module._free(sizePtr);
    }
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
 * @param {Uint8Array} movData - MOV file data
 * @param {string} [filename='output.mov'] - Download filename
 */
export function downloadMov(movData, filename = 'output.mov') {
  const blob = new Blob([movData], { type: 'video/quicktime' });
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
