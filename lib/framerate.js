/**
 * Frame rate helpers shared by the single-thread encoder and the worker pool.
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
export function frameRateToFraction(fps) {
  for (const std of STANDARD_FRAME_RATES) {
    if (Math.abs(fps - std.value) < FRAME_RATE_EPSILON) {
      return { num: std.num, den: std.den };
    }
  }

  if (Number.isInteger(fps)) {
    return { num: fps, den: 1 };
  }

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
 * Resolve num/den from an options object with frameRate or explicit
 * frameRateNum/frameRateDen (the latter takes precedence).
 */
export function resolveFrameRate({ frameRate, frameRateNum, frameRateDen }) {
  if (frameRateNum !== undefined && frameRateDen !== undefined) {
    return { num: frameRateNum, den: frameRateDen };
  }
  if (frameRate !== undefined) {
    return frameRateToFraction(frameRate);
  }
  return { num: 30, den: 1 };
}
