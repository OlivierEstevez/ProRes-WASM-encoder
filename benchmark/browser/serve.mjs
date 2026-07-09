/**
 * Static server for the in-browser benchmark.
 *
 * Sets the cross-origin isolation headers (COOP/COEP) that SharedArrayBuffer —
 * and therefore ffmpeg.wasm multi-thread (pthreads) — requires. Serves the
 * repo root so the page can reach /dist, /benchmark/footage and
 * /benchmark/node_modules/@ffmpeg from one origin. Supports Range requests so
 * the page can fetch just the first N frames of the big raw footage file.
 */
import { createServer } from 'node:http';
import { createReadStream, statSync, existsSync } from 'node:fs';
import { extname, join, normalize } from 'node:path';
import { fileURLToPath } from 'node:url';

const ROOT = fileURLToPath(new URL('../..', import.meta.url)); // repo root
const PORT = process.env.PORT ? Number(process.env.PORT) : 3210;

const TYPES = {
  '.html': 'text/html', '.js': 'text/javascript', '.mjs': 'text/javascript',
  '.wasm': 'application/wasm', '.json': 'application/json', '.css': 'text/css',
  '.rgba': 'application/octet-stream', '.map': 'application/json',
};

const server = createServer((req, res) => {
  // Cross-origin isolation for SharedArrayBuffer / pthreads.
  res.setHeader('Cross-Origin-Opener-Policy', 'same-origin');
  res.setHeader('Cross-Origin-Embedder-Policy', 'require-corp');
  res.setHeader('Cross-Origin-Resource-Policy', 'cross-origin');

  let urlPath = decodeURIComponent(new URL(req.url, 'http://x').pathname);
  if (urlPath === '/') urlPath = '/benchmark/browser/bench.html';
  const filePath = normalize(join(ROOT, urlPath));
  if (!filePath.startsWith(ROOT) || !existsSync(filePath)) {
    res.writeHead(404); res.end('not found'); return;
  }

  const stat = statSync(filePath);
  if (stat.isDirectory()) { res.writeHead(404); res.end('dir'); return; }
  const type = TYPES[extname(filePath)] || 'application/octet-stream';

  const range = req.headers.range;
  if (range) {
    const m = /bytes=(\d+)-(\d*)/.exec(range);
    const start = Number(m[1]);
    const end = m[2] ? Number(m[2]) : stat.size - 1;
    res.writeHead(206, {
      'Content-Type': type,
      'Content-Range': `bytes ${start}-${end}/${stat.size}`,
      'Accept-Ranges': 'bytes',
      'Content-Length': end - start + 1,
    });
    createReadStream(filePath, { start, end }).pipe(res);
  } else {
    res.writeHead(200, { 'Content-Type': type, 'Content-Length': stat.size, 'Accept-Ranges': 'bytes' });
    createReadStream(filePath).pipe(res);
  }
});

server.listen(PORT, () => {
  console.log(`benchmark server: http://localhost:${PORT}/  (COOP/COEP enabled, root=${ROOT})`);
});
