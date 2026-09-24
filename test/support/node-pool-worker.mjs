/**
 * Node worker_threads host for the pool's worker core, so the pool can be
 * tested with real threads outside a browser. Browsers use the Blob-URL
 * Worker that 'prores-wasm-encoder/parallel' spawns itself.
 */
import { parentPort } from 'node:worker_threads';
import { createWorkerCore } from '../../dist/prores-worker.mjs';

const core = createWorkerCore();
parentPort.on('message', (msg) => core.handle(msg, (m, t) => parentPort.postMessage(m, t)));
