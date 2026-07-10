/**
 * prores-wasm-encoder/mediabunny — MediaBunny custom-encoder integration.
 *
 * Registers this library as MediaBunny's ProRes video encoder, so any
 * MediaBunny Output (or Conversion) that targets codec 'prores' encodes
 * through it. MediaBunny handles the container muxing; this adapter only
 * produces raw ProRes packets (one keyframe per frame — ProRes is
 * intra-only).
 *
 * MediaBunny expresses the chosen ProRes variant as the config codec
 * string, which for ProRes is the fourcc itself ('apch', 'ap4h', ...):
 * pass `fullCodecString` in the video encoding config to pick a profile
 * explicitly, or let MediaBunny infer one from bitrate + alpha.
 *
 * 'mediabunny' is a peer dependency of this entry point.
 */
import { CustomVideoEncoder, EncodedPacket, registerEncoder } from 'mediabunny';
import { createProResEncoder, ProResProfile } from './index.js';
import { createProResEncoderPool } from './parallel.js';

export { ProResProfile } from './index.js';

/** MediaBunny fourcc → our profile enum */
const FOURCC_TO_PROFILE = new Map([
  ['apco', ProResProfile.PROXY],
  ['apcs', ProResProfile.LT],
  ['apcn', ProResProfile.STANDARD],
  ['apch', ProResProfile.HQ],
  ['ap4h', ProResProfile.P4444],
  ['ap4x', ProResProfile.P4444XQ],
]);

/** fourccs that carry an alpha channel (4444 family) */
const ALPHA_FOURCCS = new Set(['ap4h', 'ap4x']);

/** Options set via registerProResEncoder (module-level: MediaBunny
 * instantiates the encoder class itself, so there is no per-instance
 * options channel). */
const settings = {
  workers: undefined,   // undefined = auto (workers if available), 0 = single-thread
};

export class ProResVideoEncoder extends CustomVideoEncoder {
  static supports(codec, config) {
    return codec === 'prores'
      && typeof config.codec === 'string'
      && FOURCC_TO_PROFILE.has(config.codec);
  }

  async init() {
    const profile = FOURCC_TO_PROFILE.get(this.config.codec);
    const width = this.config.width;
    const height = this.config.height;
    const frameRate = this.config.framerate || 30;

    const wantWorkers = settings.workers === undefined
      ? (typeof Worker !== 'undefined')
      : settings.workers > 0 && typeof Worker !== 'undefined';

    this._fallbackDuration = 1 / frameRate;
    this._hasAlpha = ALPHA_FOURCCS.has(this.config.codec);
    this._warnedAlphaFallback = false;
    this._canvas = null;
    this._canvasCtx = null;
    this._pixelBuf = null;

    if (wantWorkers) {
      // Frame-parallel path: packets emerge in frame order via onFrameData,
      // paired with timestamps through a FIFO (submission order == frame
      // order == delivery order).
      this._timing = [];
      this._pool = await createProResEncoderPool({
        width, height, frameRate, profile,
        workers: settings.workers,
        onFrameData: (chunk) => {
          const t = this._timing.shift();
          this.onPacket(
            new EncodedPacket(chunk, 'key', t.timestamp, t.duration),
            this._packetMeta()
          );
        },
      });
      this._encoder = null;
    } else {
      this._pool = null;
      this._encoder = await createProResEncoder();
      this._encoder.initialize({ width, height, frameRate, profile });
    }
  }

  _packetMeta() {
    // decoderConfig.codec carries the fourcc; MediaBunny's muxers use it
    // for the sample entry.
    return {
      decoderConfig: {
        codec: this.config.codec,
        codedWidth: this.config.width,
        codedHeight: this.config.height,
      },
    };
  }

  /** Get RGBA bytes out of a MediaBunny VideoSample. */
  async _extractRgba(sample) {
    const width = this.config.width;
    const height = this.config.height;
    const size = width * height * 4;

    // Fast path: WebCodecs-style copyTo with RGBA conversion. This is
    // lossless (straight, non-premultiplied alpha) and is what runs for
    // canvas, VideoFrame, and raw RGBA/RGBX samples in practice.
    let copyToError;
    try {
      if (!this._pixelBuf || this._pixelBuf.length !== size) {
        this._pixelBuf = new Uint8Array(size);
      }
      await sample.copyTo(this._pixelBuf, { format: 'RGBA' });
      return this._pixelBuf;
    } catch (err) {
      // Keep the reason instead of swallowing it — a copyTo failure that
      // reaches the fallback should not be invisible.
      copyToError = err;
    }

    // Fallback: draw into a 2D canvas and read back. 2D canvases store
    // premultiplied alpha, so this path loses precision for semi-transparent
    // pixels on the 4444 profiles. Warn once rather than silently degrading
    // alpha quality — the encode still completes.
    if (this._hasAlpha && !this._warnedAlphaFallback) {
      this._warnedAlphaFallback = true;
      console.warn(
        `prores-wasm-encoder (MediaBunny): VideoSample.copyTo(RGBA) failed ` +
        `(${copyToError && copyToError.message}); falling back to a 2D-canvas ` +
        `readback for this ${this.config.codec} (4444) encode. 2D canvases use ` +
        `premultiplied alpha, so semi-transparent pixels may lose precision.`
      );
    }

    if (!this._canvasCtx) {
      const canvasAvailable =
        typeof OffscreenCanvas !== 'undefined' || typeof document !== 'undefined';
      if (!canvasAvailable) {
        throw new Error(
          `prores-wasm-encoder (MediaBunny): cannot read RGBA from this ` +
          `VideoSample — copyTo(RGBA) failed and no canvas is available in ` +
          `this environment` +
          (copyToError ? ` (${copyToError.message})` : '') + '.'
        );
      }
      this._canvas = typeof OffscreenCanvas !== 'undefined'
        ? new OffscreenCanvas(width, height)
        : Object.assign(document.createElement('canvas'), { width, height });
      this._canvasCtx = this._canvas.getContext('2d', { willReadFrequently: true });
    }
    this._canvasCtx.clearRect(0, 0, width, height);
    sample.draw(this._canvasCtx, 0, 0, width, height);
    return this._canvasCtx.getImageData(0, 0, width, height).data;
  }

  async encode(sample) {
    const rgba = await this._extractRgba(sample);
    const timestamp = sample.timestamp;
    const duration = sample.duration || this._fallbackDuration;

    if (this._pool) {
      this._timing.push({ timestamp, duration });
      await this._pool.addFrameRgba(rgba);
    } else {
      const packet = this._encoder.encodePacketRgba(rgba);
      this.onPacket(
        new EncodedPacket(packet, 'key', timestamp, duration),
        this._packetMeta()
      );
    }
  }

  async flush() {
    if (this._pool) {
      await this._pool.flush();
    }
    // Single-thread path is synchronous: nothing pending.
  }

  async close() {
    if (this._pool) {
      await this._pool.destroy();
      this._pool = null;
    }
    if (this._encoder) {
      this._encoder.destroy();
      this._encoder = null;
    }
    this._canvas = null;
    this._canvasCtx = null;
    this._pixelBuf = null;
  }
}

/**
 * Register this library as MediaBunny's ProRes encoder.
 *
 * @param {Object} [options]
 * @param {number} [options.workers] - Worker threads per encoding.
 *   Default: automatic (multi-threaded when Web Workers are available).
 *   Pass 0 to force single-threaded encoding.
 */
let registered = false;

export function registerProResEncoder(options = {}) {
  if (options.workers !== undefined) {
    settings.workers = options.workers;
  }
  // Register with MediaBunny once; repeat calls only update the settings.
  if (!registered) {
    registerEncoder(ProResVideoEncoder);
    registered = true;
  }
}

export default registerProResEncoder;
