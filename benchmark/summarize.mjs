/**
 * Build the data tables (size + speed) from results/ and inject them into
 * README.md between the RESULTS markers. The prose around the markers is
 * hand written; this only refreshes the numbers.
 *
 *   node summarize.mjs
 */
import { readFileSync, writeFileSync, existsSync } from 'node:fs';
import { join } from 'node:path';
import { RESULTS_DIR, __dirname } from './bench-common.mjs';

const data = JSON.parse(readFileSync(join(RESULTS_DIR, 'results.json'), 'utf8'));
const rows = [...data.results];

const browserCsv = join(RESULTS_DIR, 'results_browser.csv');
if (existsSync(browserCsv)) {
  const lines = readFileSync(browserCsv, 'utf8').trim().split('\n');
  const cols = lines[0].split(',');
  for (const line of lines.slice(1)) {
    const cells = line.match(/("[^"]*"|[^,]*)(,|$)/g).map((c) => c.replace(/,$/, '').replace(/^"|"$/g, ''));
    const r = {};
    cols.forEach((c, i) => { r[c] = cells[i]; });
    r.fps = Number(r.fps); r.realtimeX = Number(r.realtimeX); r.frames = Number(r.frames);
    rows.push(r);
  }
}

const envOf = (r) => (r.environment === 'native' || r.environment === 'browser')
  ? r.environment : (r.contender === 'ffmpeg.wasm' ? 'wasm' : 'node');
const find = (p, c, t, e) => rows.find((r) => r.profile === p && r.contender === c && r.threads === t && envOf(r) === e);

const threadWord = (t) => (t === '1t' ? '1 thread' : `${t.replace('t', '')} threads`);

// Clean, dash-free display label per row.
function label(r) {
  if (r.contender === 'prores-wasm (ours)') {
    const where = r.environment === 'browser' ? 'WASM, browser' : 'WASM';
    return `prores-wasm (ours), ${threadWord(r.threads)} (${where})`;
  }
  if (r.contender === 'ffmpeg.wasm') {
    const where = r.environment === 'browser' ? 'WASM, browser' : 'WASM';
    return `ffmpeg.wasm, ${threadWord(r.threads)} (${where})`;
  }
  const mode = { 'sw-1core': 'software, 1 core', 'sw-allcores': 'software, all cores', 'hw-vt': 'hardware (VideoToolbox)' }[r.threads] || r.threads;
  return `FFmpeg native, ${mode}`;
}

// Canonical single set of contenders (ours + ffmpeg.wasm 1t from the Node run,
// ffmpeg.wasm 8t from the browser run, native from the CLI).
// Grouped by engine so each row set reads as one story (single then parallel).
const ORDER = [
  ['ffmpeg.wasm', '1t', 'wasm'],
  ['ffmpeg.wasm', '8t', 'browser'],
  ['prores-wasm (ours)', '1t', 'node'],
  ['prores-wasm (ours)', '2t', 'node'],
  ['prores-wasm (ours)', '4t', 'node'],
  ['prores-wasm (ours)', '8t', 'node'],
  ['FFmpeg (native)', 'sw-1core', 'native'],
  ['FFmpeg (native)', 'sw-allcores', 'native'],
  ['FFmpeg (native)', 'hw-vt', 'native'],
];

function speedTable(profileKey) {
  const lines = ['| Encoder | fps | Real time | Encode time |', '|---|--:|--:|--:|'];
  for (const [c, t, e] of ORDER) {
    const r = find(profileKey, c, t, e);
    if (!r) continue;
    const secs = r.encodeSecMedian ? `${(+r.encodeSecMedian).toFixed(1)}s` : '';
    lines.push(`| ${label(r)} | ${r.fps} | ${r.realtimeX}x | ${secs} |`);
  }
  return lines.join('\n');
}

let md = '';
for (const [p, name] of [['422hq', 'ProRes 422 HQ'], ['4444', 'ProRes 4444 with alpha']]) {
  if (!rows.some((r) => r.profile === p)) continue;
  md += `### ${name} at ${data.meta.width}x${data.meta.height}\n\n${speedTable(p)}\n\n`;
}

// Browser cross-check: same engine, same page. Also the only place
// ffmpeg.wasm multi thread can run.
md += '### Browser run\n\n';
md += 'The WASM encoders were also run in a Chromium tab, both to measure ';
md += 'ffmpeg.wasm multi thread (which only runs in a browser) and to check the ';
md += 'Node numbers. They agree within a few percent.\n\n';
md += '| Encoder | 422 HQ fps | 4444 fps |\n|---|--:|--:|\n';
for (const [c, t] of [['ffmpeg.wasm', '1t'], ['prores-wasm (ours)', '1t'], ['ffmpeg.wasm', '8t'], ['prores-wasm (ours)', '8t']]) {
  const a = find('422hq', c, t, 'browser');
  const b = find('4444', c, t, 'browser');
  if (a || b) md += `| ${label(a || b)} | ${a ? a.fps : ''} | ${b ? b.fps : ''} |\n`;
}
md += `\n_Machine: ${data.host.cpu || '?'}, ${data.host.cores || '?'} cores. Node ${data.host.node}. `;
md += `${(data.host.ffmpeg || '').split(' ').slice(0, 3).join(' ')}. Footage: Big Buck Bunny `;
md += `${data.meta.width}x${data.meta.height}, ${data.meta.frames} frames at ${data.meta.fps} fps (browser run uses 30 frames)._\n`;

console.log(md);

const readmePath = join(__dirname, 'README.md');
let readme = readFileSync(readmePath, 'utf8');
readme = readme.replace(/<!-- RESULTS:START -->[\s\S]*<!-- RESULTS:END -->/,
  `<!-- RESULTS:START -->\n${md}<!-- RESULTS:END -->`);
writeFileSync(readmePath, readme);
console.log('Injected into README.md');
