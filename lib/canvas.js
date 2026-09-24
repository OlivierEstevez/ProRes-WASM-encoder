/**
 * Canvas readback shared by the single-thread encoder and the worker pool.
 *
 * Works with any canvas, not only 2D ones: getContext('2d') returns null on
 * a canvas that already holds a WebGL/WebGPU/bitmaprenderer context, so
 * those are copied into a scratch 2D canvas with drawImage first. Call it in
 * the same task as the draw (or use preserveDrawingBuffer) so WebGL canvases
 * still hold the frame.
 */

/**
 * Read a canvas's pixels as RGBA.
 * @param {HTMLCanvasElement|OffscreenCanvas} canvas
 * @param {number} width - Encoder width (must match canvas.width)
 * @param {number} height - Encoder height (must match canvas.height)
 * @param {{canvas: any, ctx: CanvasRenderingContext2D|null}} scratch -
 *   Per-encoder cache for the scratch canvas (mutated)
 * @returns {Uint8ClampedArray}
 */
export function readCanvasRgba(canvas, width, height, scratch) {
  if (!canvas || typeof canvas.getContext !== 'function') {
    throw new TypeError('addFrameFromCanvas: expected an HTMLCanvasElement or OffscreenCanvas');
  }
  if (canvas.width !== width || canvas.height !== height) {
    throw new Error(
      `addFrameFromCanvas: canvas is ${canvas.width}x${canvas.height} but the encoder is ` +
      `${width}x${height}. The encoder reads the canvas backing store (canvas.width/height), ` +
      `not its CSS size — resize the canvas or initialize the encoder with matching dimensions.`
    );
  }

  const ctx = canvas.getContext('2d');
  if (ctx) {
    return ctx.getImageData(0, 0, width, height).data;
  }

  // Non-2D canvas (WebGL, WebGPU, bitmaprenderer): copy through a 2D canvas.
  if (!scratch.ctx) {
    if (typeof OffscreenCanvas !== 'undefined') {
      scratch.canvas = new OffscreenCanvas(width, height);
    } else if (typeof document !== 'undefined') {
      scratch.canvas = Object.assign(document.createElement('canvas'), { width, height });
    } else {
      throw new Error('addFrameFromCanvas: cannot read a non-2D canvas without OffscreenCanvas or document');
    }
    scratch.ctx = scratch.canvas.getContext('2d', { willReadFrequently: true });
  }
  scratch.ctx.clearRect(0, 0, width, height);
  scratch.ctx.drawImage(canvas, 0, 0);
  return scratch.ctx.getImageData(0, 0, width, height).data;
}
