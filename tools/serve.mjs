#!/usr/bin/env node
// Static server for the Open Commando browser build.
//
//   /                -> engine/web (page, runtime, import worker)
//   /renegade.*      -> the Emscripten build output (js, wasm)
//   /gamedata/...    -> the user's own Renegade install (imported into OPFS)
//   /gamedata/manifest.json  generated listing of the files to import
//   /bink.{js,wasm}  -> the page's cutscene decoder (tools/build-bink.sh)
//   /movies/*.webm   -> transcoded cutscenes (optional fallback)
//   /dbg/...         -> the same, with the debug build of renegade.*
//
// Sends COOP/COEP so the page is cross-origin isolated (SharedArrayBuffer
// for pthreads), supports Range requests, and binds to localhost only:
// Tailscale serve exposes it to the tailnet.
import { createServer } from 'node:http';
import { createReadStream, promises as fs } from 'node:fs';
import path from 'node:path';
import { fileURLToPath } from 'node:url';

const here = path.dirname(fileURLToPath(import.meta.url));
const engine = path.resolve(here, '..');          // engine/
const workspace = path.resolve(engine, '..');     // the folder holding engine/, game-data/, web-movies/
const cfg = {
  port: Number(process.env.PORT || 8461),
  web: process.env.REBEL_WEB_DIR || path.join(engine, 'web'),
  // Optimized build at /, the debug build (asserts, WWDEBUG logs) under /dbg/
  // on the same origin, so both share the imported game data.
  build: process.env.REBEL_BUILD_DIR || path.join(engine, 'build/wasm-release/bin'),
  debugBuild: process.env.REBEL_DEBUG_BUILD_DIR || path.join(engine, 'build/wasm/bin'),
  game: process.env.REBEL_GAME_DIR || path.join(workspace, 'game-data/renegade'),
  movies: process.env.REBEL_MOVIES_DIR || path.join(workspace, 'web-movies'),
  bink: process.env.REBEL_BINK_DIR || path.join(engine, 'build/bink'), // tools/build-bink.sh
};

// What the browser needs from the install (no executables, editors, redists).
const IMPORT_DIRS = ['Data'];
const IMPORT_EXCLUDE = []; // cutscenes (Data/Movies/*.bik) are played in the page
const IMPORT_ROOT_FILES = /\.(ttf|dat|ini|lcf|txt|016|256)$/i;
const MANIFEST_VERSION = 2; // 2: includes Data/Movies

const MIME = {
  '.html': 'text/html; charset=utf-8', '.js': 'text/javascript; charset=utf-8', '.mjs': 'text/javascript; charset=utf-8',
  '.wasm': 'application/wasm', '.json': 'application/json', '.css': 'text/css', '.webm': 'video/webm',
  '.mp4': 'video/mp4', '.png': 'image/png', '.ico': 'image/x-icon', '.data': 'application/octet-stream',
};

async function walk(dir, rel = '') {
  const out = [];
  for (const e of await fs.readdir(path.join(dir, rel), { withFileTypes: true })) {
    const r = rel ? `${rel}/${e.name}` : e.name;
    if (e.isDirectory()) out.push(...(await walk(dir, r)));
    else if (e.isFile()) out.push(r);
  }
  return out;
}

let manifestCache = null;
async function manifest() {
  if (manifestCache) return manifestCache;
  const files = [];
  for (const name of await fs.readdir(cfg.game)) {
    const st = await fs.stat(path.join(cfg.game, name));
    if (st.isFile() && IMPORT_ROOT_FILES.test(name)) files.push({ path: name, size: st.size });
  }
  for (const d of IMPORT_DIRS) {
    for (const rel of await walk(cfg.game, d)) {
      if (IMPORT_EXCLUDE.some((re) => re.test(rel))) continue;
      files.push({ path: rel, size: (await fs.stat(path.join(cfg.game, rel))).size });
    }
  }
  manifestCache = { version: MANIFEST_VERSION, files };
  return manifestCache;
}

function headers(res, type, extra = {}) {
  res.setHeader('Cross-Origin-Opener-Policy', 'same-origin');
  res.setHeader('Cross-Origin-Embedder-Policy', 'require-corp');
  res.setHeader('Cross-Origin-Resource-Policy', 'same-origin');
  res.setHeader('Content-Type', type);
  for (const [k, v] of Object.entries(extra)) res.setHeader(k, v);
}

async function sendFile(req, res, file, cache) {
  let st;
  try {
    st = await fs.stat(file);
    if (!st.isFile()) throw new Error('not a file');
  } catch {
    headers(res, 'text/plain');
    res.statusCode = 404;
    return res.end('not found');
  }
  const type = MIME[path.extname(file).toLowerCase()] || 'application/octet-stream';
  const range = /^bytes=(\d*)-(\d*)$/.exec(req.headers.range || '');
  const common = { 'Accept-Ranges': 'bytes', 'Cache-Control': cache, 'Last-Modified': st.mtime.toUTCString() };
  if (range) {
    const start = range[1] ? Number(range[1]) : st.size - Number(range[2]);
    const end = range[1] && range[2] ? Number(range[2]) : st.size - 1;
    if (start > end || end >= st.size) {
      headers(res, type, { 'Content-Range': `bytes */${st.size}` });
      res.statusCode = 416;
      return res.end();
    }
    headers(res, type, { ...common, 'Content-Range': `bytes ${start}-${end}/${st.size}`, 'Content-Length': end - start + 1 });
    res.statusCode = 206;
    if (req.method === 'HEAD') return res.end();
    return createReadStream(file, { start, end }).pipe(res);
  }
  headers(res, type, { ...common, 'Content-Length': st.size });
  if (req.method === 'HEAD') return res.end();
  createReadStream(file).pipe(res);
}

// Resolve a URL path inside a base directory, refusing traversal.
function inside(base, urlPath) {
  const p = path.resolve(base, '.' + path.posix.normalize('/' + urlPath));
  return p === base || p.startsWith(base + path.sep) ? p : null;
}

const server = createServer(async (req, res) => {
  try {
    const url = new URL(req.url, 'http://localhost');
    let p = decodeURIComponent(url.pathname);
    const debug = p === '/dbg' || p.startsWith('/dbg/');
    if (p === '/dbg') { res.writeHead(302, { Location: '/dbg/' + url.search }); return res.end(); }
    if (debug) p = p.slice(4);
    if (p === '/gamedata/manifest.json') {
      headers(res, MIME['.json'], { 'Cache-Control': 'no-store' });
      return res.end(JSON.stringify(await manifest()));
    }
    let file = null, cache = 'no-cache';
    if (p.startsWith('/gamedata/')) {
      const rel = p.slice('/gamedata/'.length);
      const m = await manifest();
      if (!m.files.some((f) => f.path === rel)) file = null; // only manifest entries are served
      else file = inside(cfg.game, rel);
      cache = 'public, max-age=86400';
    } else if (p.startsWith('/movies/')) {
      file = inside(cfg.movies, p.slice('/movies/'.length));
      cache = 'public, max-age=86400';
    } else if (/^\/bink\.(js|wasm)$/.test(p)) {
      file = inside(cfg.bink, p.slice(1));
    } else if (/^\/renegade\.(js|wasm|worker\.js|data)$/.test(p)) {
      file = inside(debug ? cfg.debugBuild : cfg.build, p.slice(1));
    } else {
      file = inside(cfg.web, p === '/' ? 'index.html' : p.slice(1));
    }
    if (!file) {
      headers(res, 'text/plain');
      res.statusCode = 404;
      return res.end('not found');
    }
    await sendFile(req, res, file, cache);
  } catch (e) {
    headers(res, 'text/plain');
    res.statusCode = 500;
    res.end(String(e));
  }
});

server.listen(cfg.port, '127.0.0.1', () => {
  console.log(`open-commando on http://127.0.0.1:${cfg.port}  (web=${cfg.web} build=${cfg.build} game=${cfg.game})`);
});
