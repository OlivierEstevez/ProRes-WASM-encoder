/**
 * prepare-footage.mjs
 *
 * Turns the Big Buck Bunny 1080p clip into a single flat file of raw RGBA
 * frames that every encoder in the benchmark consumes identically. Producing
 * the raw frames once, up front, means the benchmark measures pure ProRes
 * ENCODE throughput, with no H.264 decode, no scaling, and no disk-format
 * differences leaking into the numbers.
 *
 * The source clip is downloaded automatically if missing (footage is
 * gitignored), so a fresh clone just runs `npm run footage`. Override the
 * source with the BBB_URL env var.
 *
 *   footage/bbb_1080p.mp4  ->  footage/frames_1080p.rgba  (+ meta.json)
 *
 * Alpha: BBB is opaque, but a ProRes 4444 benchmark should exercise the alpha
 * plane. We overwrite the alpha channel with a smooth, time-varying gradient
 * (a moving sinusoidal matte). It is full-range and non-constant so the alpha
 * RLE path does real work, while staying representative of a real matte
 * (smooth, not per-pixel noise). The 422 profiles ignore alpha entirely, so
 * one file serves both profiles.
 */
import { execFileSync } from 'node:child_process';
import {
  existsSync, mkdirSync, openSync, readSync, writeSync, closeSync,
  writeFileSync, statSync, createWriteStream, renameSync,
} from 'node:fs';
import { pipeline } from 'node:stream/promises';
import { Readable } from 'node:stream';
import { fileURLToPath } from 'node:url';
import { dirname, join } from 'node:path';

const __dirname = dirname(fileURLToPath(import.meta.url));
const FOOTAGE = join(__dirname, 'footage');

const WIDTH = 1920;
const HEIGHT = 1080;
const FPS = 30;
const SECONDS = 5;
const FRAMES = FPS * SECONDS;               // 150
const BYTES_PER_FRAME = WIDTH * HEIGHT * 4; // 8,294,400

const SRC = join(FOOTAGE, 'bbb_1080p.mp4');
const RAW = join(FOOTAGE, 'frames_1080p.rgba');
const META = join(FOOTAGE, 'meta.json');

const BBB_URL = process.env.BBB_URL
  || 'https://test-videos.co.uk/vids/bigbuckbunny/mp4/h264/1080/Big_Buck_Bunny_1080_10s_30MB.mp4';

mkdirSync(FOOTAGE, { recursive: true });

// 0) Fetch the source clip if it is not already there (footage is gitignored).
if (!existsSync(SRC)) {
  console.log('Downloading Big Buck Bunny 1080p ...');
  console.log(`  ${BBB_URL}`);
  const res = await fetch(BBB_URL);
  if (!res.ok || !res.body) {
    console.error(`Download failed (HTTP ${res.status}). Set BBB_URL to a working 1080p mp4, or place one at ${SRC}.`);
    process.exit(1);
  }
  const part = `${SRC}.part`;
  await pipeline(Readable.fromWeb(res.body), createWriteStream(part));
  renameSync(part, SRC);
  console.log(`Saved ${SRC} (${(statSync(SRC).size / 1e6).toFixed(0)} MB)`);
}

// 1) Decode the first SECONDS seconds of BBB to flat RGBA via native ffmpeg.
console.log(`Decoding ${SECONDS}s of BBB -> ${FRAMES} RGBA frames ...`);
execFileSync('ffmpeg', [
  '-y', '-loglevel', 'error',
  '-t', String(SECONDS),
  '-i', SRC,
  '-vf', `fps=${FPS},scale=${WIDTH}:${HEIGHT}:flags=bicubic,format=rgba`,
  '-frames:v', String(FRAMES),
  '-f', 'rawvideo',
  RAW,
], { stdio: 'inherit' });

const size = statSync(RAW).size;
const expected = FRAMES * BYTES_PER_FRAME;
if (size !== expected) {
  console.error(`Raw size ${size} != expected ${expected} (${size / BYTES_PER_FRAME} frames). Aborting.`);
  process.exit(1);
}

// 2) Paint a moving sinusoidal alpha matte over the opaque alpha channel,
//    frame by frame, in place.
console.log('Painting time-varying alpha matte ...');
const fd = openSync(RAW, 'r+');
const buf = Buffer.allocUnsafe(BYTES_PER_FRAME);
for (let f = 0; f < FRAMES; f++) {
  const off = f * BYTES_PER_FRAME;
  readSync(fd, buf, 0, BYTES_PER_FRAME, off);
  const t = f / FPS;
  let i = 3;
  for (let y = 0; y < HEIGHT; y++) {
    const fy = y / HEIGHT;
    for (let x = 0; x < WIDTH; x++) {
      const fx = x / WIDTH;
      // Smooth full-range matte that drifts diagonally over time.
      const s = Math.sin(2 * Math.PI * (fx + fy + t * 0.5));
      buf[i] = (128 + 127 * s) & 0xff;
      i += 4;
    }
  }
  writeSync(fd, buf, 0, BYTES_PER_FRAME, off);
}
closeSync(fd);

const meta = {
  source: 'Big Buck Bunny (Blender Foundation, CC-BY 3.0)',
  clip: 'first 5 seconds',
  width: WIDTH,
  height: HEIGHT,
  fps: FPS,
  frames: FRAMES,
  seconds: SECONDS,
  bytesPerFrame: BYTES_PER_FRAME,
  pixelFormat: 'rgba',
  alpha: 'synthetic moving sinusoidal matte (full-range, non-constant)',
  rawFile: 'frames_1080p.rgba',
};
writeFileSync(META, JSON.stringify(meta, null, 2));

console.log(`Done. ${FRAMES} frames, ${(size / 1e6).toFixed(1)} MB raw -> ${RAW}`);
console.log(JSON.stringify(meta, null, 2));
