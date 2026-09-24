/**
 * Option validation shared by the single-thread encoder and the worker pool.
 */
import { resolveFrameRate } from './framerate.js';

/** Largest side the ProRes frame header and our 32-bit buffer math allow. */
const MAX_DIMENSION = 16384;
/** Largest frame (8K square): keeps 4444 YUV buffer sizes inside int32. */
const MAX_PIXELS = 8192 * 8192;

const PROFILES = new Set([0, 1, 2, 3, 4, 5]);
const RANGES = new Set(['limited', 'full']);

let warnedFullRange = false;

function isPositiveInt(v) {
  return Number.isInteger(v) && v > 0;
}

/**
 * Validate encoder options and resolve defaults.
 * @returns {{width: number, height: number, fpsNum: number, fpsDen: number,
 *   profile: number, rangeValue: number, onFrameData: function|null}}
 */
export function validateOptions(options) {
  if (!options || typeof options !== 'object') {
    throw new TypeError('ProRes encoder: options object with width and height is required');
  }
  const { width, height, profile = 3, range = 'limited', onFrameData = null } = options;

  if (!isPositiveInt(width) || !isPositiveInt(height)) {
    throw new RangeError(`ProRes encoder: width and height must be positive integers (got ${width}x${height})`);
  }
  if (width > MAX_DIMENSION || height > MAX_DIMENSION || width * height > MAX_PIXELS) {
    throw new RangeError(
      `ProRes encoder: ${width}x${height} is too large ` +
      `(max ${MAX_DIMENSION} per side and ${MAX_PIXELS} pixels per frame)`
    );
  }
  if (!PROFILES.has(profile)) {
    throw new RangeError(`ProRes encoder: unknown profile ${profile} (use ProResProfile.PROXY … ProResProfile.P4444XQ)`);
  }
  if (!RANGES.has(range)) {
    throw new RangeError(`ProRes encoder: range must be "limited" or "full" (got ${JSON.stringify(range)})`);
  }
  if (range === 'full' && !warnedFullRange) {
    warnedFullRange = true;
    console.warn('prores-wasm-encoder: range "full" is not supported yet; output uses limited (TV) range.');
  }
  if (onFrameData !== null && typeof onFrameData !== 'function') {
    throw new TypeError('ProRes encoder: onFrameData must be a function');
  }

  const { frameRate, frameRateNum, frameRateDen } = options;
  if ((frameRateNum === undefined) !== (frameRateDen === undefined)) {
    throw new TypeError('ProRes encoder: frameRateNum and frameRateDen must be passed together');
  }
  if (frameRateNum !== undefined) {
    if (!isPositiveInt(frameRateNum) || !isPositiveInt(frameRateDen)) {
      throw new RangeError(
        `ProRes encoder: frameRateNum/frameRateDen must be positive integers (got ${frameRateNum}/${frameRateDen})`
      );
    }
  } else if (frameRate !== undefined && !(Number.isFinite(frameRate) && frameRate > 0)) {
    throw new RangeError(`ProRes encoder: frameRate must be a positive number (got ${frameRate})`);
  }
  const { num, den } = resolveFrameRate(options);

  return {
    width,
    height,
    fpsNum: num,
    fpsDen: den,
    profile,
    rangeValue: range === 'full' ? 1 : 0,
    onFrameData,
  };
}

/**
 * Validate the pool's worker count.
 * @returns {number|undefined}
 */
export function validateWorkers(workers) {
  if (workers === undefined) return undefined;
  if (!isPositiveInt(workers)) {
    throw new RangeError(
      `ProRes encoder pool: workers must be a positive integer (got ${workers}). ` +
      'For single-threaded encoding use createProResEncoder() from "prores-wasm-encoder".'
    );
  }
  return workers;
}
