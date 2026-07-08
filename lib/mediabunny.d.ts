/**
 * prores-wasm-encoder/mediabunny — MediaBunny custom-encoder integration.
 * Requires the 'mediabunny' peer dependency.
 */

import type { CustomVideoEncoder } from 'mediabunny';

export { ProResProfile } from './prores-encoder';

/**
 * The MediaBunny CustomVideoEncoder implementation backed by this library.
 * Usually you don't use this directly — call registerProResEncoder().
 */
export declare class ProResVideoEncoder extends CustomVideoEncoder {
  static supports(codec: unknown, config: { codec?: string }): boolean;
  init(): Promise<void>;
  encode(sample: unknown): Promise<void>;
  flush(): Promise<void>;
  close(): Promise<void>;
}

export interface RegisterProResEncoderOptions {
  /**
   * Worker threads per encoding. Default: automatic (multi-threaded when
   * Web Workers are available). Pass 0 to force single-threaded encoding.
   */
  workers?: number;
}

/**
 * Register this library as MediaBunny's ProRes encoder. After calling this,
 * any MediaBunny Output/Conversion targeting codec 'prores' encodes through
 * prores-wasm-encoder. Select the ProRes variant via the video encoding
 * config: `fullCodecString` set to a ProRes fourcc ('apco', 'apcs', 'apcn',
 * 'apch', 'ap4h', 'ap4x'), or let MediaBunny infer one from bitrate + alpha.
 */
export declare function registerProResEncoder(
  options?: RegisterProResEncoderOptions
): void;

export default registerProResEncoder;
