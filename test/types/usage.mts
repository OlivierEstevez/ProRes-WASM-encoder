/**
 * Type-level usage of every documented API. package.test.mjs compiles this
 * with `tsc --strict` against the installed tarball (bundler and nodenext
 * resolution). It is never run.
 */
import createDefault, {
  createProResEncoder, ProResProfile, ProfileNames, downloadMov, movToBlob, movToObjectUrl,
  type ProResEncoder, type ProResEncoderOptions, type ProResProfileType, type ProResBytes,
} from 'prores-wasm-encoder';
import createPoolDefault, {
  createProResEncoderPool, type ProResEncoderPool, type ProResEncoderPoolOptions,
} from 'prores-wasm-encoder/parallel';
import { registerProResEncoder } from 'prores-wasm-encoder/mediabunny';

declare const canvas: HTMLCanvasElement;
declare const offscreen: OffscreenCanvas;
declare const imageData: ImageData;

export async function singleThread(): Promise<Blob> {
  const enc: ProResEncoder = await createProResEncoder();
  const profile: ProResProfileType = ProResProfile.HQ;
  const opts: ProResEncoderOptions = { width: 1920, height: 1080, frameRate: 30, profile };
  enc.initialize(opts);
  enc.addFrameFromCanvas(canvas);
  enc.addFrameFromCanvas(offscreen);
  enc.addFrameFromImageData(imageData);
  enc.addFrameRgba(new Uint8Array(1920 * 1080 * 4));
  enc.addFrameRgba(new Uint8ClampedArray(1920 * 1080 * 4));
  const count: number = enc.frameCount;
  void count;
  const name: string = ProfileNames[ProResProfile.P4444];
  void name;

  const mov: ProResBytes = enc.finalize();
  // The report's issue 6: this must compile without a cast.
  const blob = new Blob([mov], { type: 'video/quicktime' });
  downloadMov(mov, 'a.mov');
  downloadMov(blob);
  movToObjectUrl(mov);
  enc.destroy();

  const enc2 = await createDefault();
  enc2.initialize({ width: 64, height: 64, frameRateNum: 24000, frameRateDen: 1001 });
  void movToBlob(mov);
  return enc2.finalizeToBlob();
}

export async function streaming(): Promise<Blob> {
  const chunks: ProResBytes[] = [];
  const enc = await createProResEncoder();
  enc.initialize({ width: 64, height: 64, onFrameData: (chunk) => chunks.push(chunk) });
  const { header, moov } = enc.finalizeHeaders();
  return new Blob([header, ...chunks, moov]);
}

export async function pool(): Promise<Blob> {
  const opts: ProResEncoderPoolOptions = {
    width: 1920, height: 1080, frameRate: 60, profile: ProResProfile.P4444XQ, workers: 4,
  };
  const p: ProResEncoderPool = await createProResEncoderPool(opts);
  await p.addFrameFromCanvas(canvas);
  await p.addFrameFromImageData(imageData);
  await p.addFrameRgba(new Uint8Array(1920 * 1080 * 4));
  await p.flush();
  const workers: number = p.workerCount;
  void workers;
  const blob = new Blob([await p.finalize()]);
  await p.destroy();

  const p2 = await createPoolDefault({ width: 64, height: 64, onFrameData: (c) => void new Blob([c]) });
  const { header, moov } = await p2.finalizeStreaming();
  void new Blob([header, moov]);
  void blob;
  return p2.finalizeToBlob();
}

export function mediabunny(): void {
  registerProResEncoder();
  registerProResEncoder({ workers: 0 });
}

export async function misuse(): Promise<void> {
  const enc = await createProResEncoder();
  // @ts-expect-error: not a ProRes profile
  enc.initialize({ width: 64, height: 64, profile: 7 });
  // @ts-expect-error: width is required
  enc.initialize({ height: 64 });
  // @ts-expect-error: range is 'full' | 'limited'
  enc.initialize({ width: 64, height: 64, range: 'tv' });
}
