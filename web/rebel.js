// Page runtime for the Open Commando browser build: game-data import, Emscripten
// module setup, input forwarding (Win32 messages + DirectInput), audio
// unlock and the cutscene <video> overlay.
'use strict';

const $ = (id) => document.getElementById(id);
const overlay = $('overlay'), statusEl = $('status'), progressEl = $('progress'), startBtn = $('start');
const canvas = $('canvas'), movie = $('movie'), logEl = $('log');
const params = new URLSearchParams(location.search);
if (params.has('log')) document.body.classList.add('show-log');

// The on-page log (?log) is updated at most once per frame: touching the DOM
// per line (the engine logs a lot) costs whole frames.
const showLog = params.has('log');
let pendingLog = [];
function flushLog() {
  if (!pendingLog.length) return;
  let text = logEl.textContent + pendingLog.join('\n') + '\n';
  pendingLog = [];
  if (text.length > 200000) text = text.slice(-150000);
  logEl.textContent = text;
  logEl.scrollTop = logEl.scrollHeight;
}
// ?quiet: only tagged runtime lines ("[d3d9] ...") and errors reach the
// console; the engine's debug chatter is dropped.
const quietLog = params.has('quiet');
function log(line) {
  if (quietLog && line[0] !== '[' && !/MainMenuDialogClass|Assert|error/i.test(line)) return;
  console.log(line);
  if (!showLog) return;
  if (!pendingLog.length) requestAnimationFrame(flushLog);
  pendingLog.push(line);
}
// Frame counter (top right): on by default, ?nofps hides it.
// The engine calls Module.rebelFrame() once per presented frame.
const fpsEl = $('fps');
if (!params.has('nofps')) document.body.classList.add('show-fps');
let fpsFrames = 0, fpsSince = performance.now(), fpsWorst = 0, fpsLast = performance.now();
function rebelFrameTick() {
  const now = performance.now();
  fpsWorst = Math.max(fpsWorst, now - fpsLast);
  fpsLast = now;
  fpsFrames++;
  if (now - fpsSince >= 500) {
    fpsEl.textContent = `${(fpsFrames * 1000 / (now - fpsSince)).toFixed(0)} fps  ·  worst ${fpsWorst.toFixed(0)} ms` + (Module.rebelGpuMs ? `  ·  gpu ${Module.rebelGpuMs.toFixed(0)} ms` : "");
    fpsFrames = 0; fpsSince = now; fpsWorst = 0;
  }
}
// Alt+F toggles it (F8 is the game's console key); the page keeps the key.
window.addEventListener('keydown', (e) => {
  if (e.code === 'KeyF' && e.altKey && !e.ctrlKey && !e.metaKey) { e.preventDefault(); e.stopImmediatePropagation(); if (!e.repeat) document.body.classList.toggle('show-fps'); }
}, true);

function setStatus(text) { statusEl.textContent = text; }
function fmtBytes(n) { return n > 1 << 30 ? (n / (1 << 30)).toFixed(2) + ' GB' : (n / (1 << 20)).toFixed(0) + ' MB'; }

/* ------------------------------------------------------------ key mapping */

// KeyboardEvent.code -> [DirectInput scan code, Win32 virtual key]
const KEYS = {
  Escape: [0x01, 0x1B], Digit1: [0x02, 0x31], Digit2: [0x03, 0x32], Digit3: [0x04, 0x33], Digit4: [0x05, 0x34],
  Digit5: [0x06, 0x35], Digit6: [0x07, 0x36], Digit7: [0x08, 0x37], Digit8: [0x09, 0x38], Digit9: [0x0A, 0x39],
  Digit0: [0x0B, 0x30], Minus: [0x0C, 0xBD], Equal: [0x0D, 0xBB], Backspace: [0x0E, 0x08], Tab: [0x0F, 0x09],
  KeyQ: [0x10, 0x51], KeyW: [0x11, 0x57], KeyE: [0x12, 0x45], KeyR: [0x13, 0x52], KeyT: [0x14, 0x54],
  KeyY: [0x15, 0x59], KeyU: [0x16, 0x55], KeyI: [0x17, 0x49], KeyO: [0x18, 0x4F], KeyP: [0x19, 0x50],
  BracketLeft: [0x1A, 0xDB], BracketRight: [0x1B, 0xDD], Enter: [0x1C, 0x0D], ControlLeft: [0x1D, 0x11],
  KeyA: [0x1E, 0x41], KeyS: [0x1F, 0x53], KeyD: [0x20, 0x44], KeyF: [0x21, 0x46], KeyG: [0x22, 0x47],
  KeyH: [0x23, 0x48], KeyJ: [0x24, 0x4A], KeyK: [0x25, 0x4B], KeyL: [0x26, 0x4C], Semicolon: [0x27, 0xBA],
  Quote: [0x28, 0xDE], Backquote: [0x29, 0xC0], ShiftLeft: [0x2A, 0x10], Backslash: [0x2B, 0xDC],
  KeyZ: [0x2C, 0x5A], KeyX: [0x2D, 0x58], KeyC: [0x2E, 0x43], KeyV: [0x2F, 0x56], KeyB: [0x30, 0x42],
  KeyN: [0x31, 0x4E], KeyM: [0x32, 0x4D], Comma: [0x33, 0xBC], Period: [0x34, 0xBE], Slash: [0x35, 0xBF],
  ShiftRight: [0x36, 0x10], NumpadMultiply: [0x37, 0x6A], AltLeft: [0x38, 0x12], Space: [0x39, 0x20],
  CapsLock: [0x3A, 0x14], F1: [0x3B, 0x70], F2: [0x3C, 0x71], F3: [0x3D, 0x72], F4: [0x3E, 0x73],
  F5: [0x3F, 0x74], F6: [0x40, 0x75], F7: [0x41, 0x76], F8: [0x42, 0x77], F9: [0x43, 0x78], F10: [0x44, 0x79],
  NumLock: [0x45, 0x90], ScrollLock: [0x46, 0x91], Numpad7: [0x47, 0x67], Numpad8: [0x48, 0x68],
  Numpad9: [0x49, 0x69], NumpadSubtract: [0x4A, 0x6D], Numpad4: [0x4B, 0x64], Numpad5: [0x4C, 0x65],
  Numpad6: [0x4D, 0x66], NumpadAdd: [0x4E, 0x6B], Numpad1: [0x4F, 0x61], Numpad2: [0x50, 0x62],
  Numpad3: [0x51, 0x63], Numpad0: [0x52, 0x60], NumpadDecimal: [0x53, 0x6E], IntlBackslash: [0x56, 0xE2],
  F11: [0x57, 0x7A], F12: [0x58, 0x7B], NumpadEnter: [0x9C, 0x0D], ControlRight: [0x9D, 0x11],
  NumpadDivide: [0xB5, 0x6F], PrintScreen: [0xB7, 0x2C], AltRight: [0xB8, 0x12], Pause: [0xC5, 0x13],
  Home: [0xC7, 0x24], ArrowUp: [0xC8, 0x26], PageUp: [0xC9, 0x21], ArrowLeft: [0xCB, 0x25],
  ArrowRight: [0xCD, 0x27], End: [0xCF, 0x23], ArrowDown: [0xD0, 0x28], PageDown: [0xD1, 0x22],
  Insert: [0xD2, 0x2D], Delete: [0xD3, 0x2E], MetaLeft: [0xDB, 0x5B], MetaRight: [0xDC, 0x5C],
  ContextMenu: [0xDD, 0x5D],
};

const WM = { KEYDOWN: 0x100, KEYUP: 0x101, CHAR: 0x102, SYSKEYDOWN: 0x104, SYSKEYUP: 0x105, MOUSEMOVE: 0x200,
  LBUTTONDOWN: 0x201, LBUTTONUP: 0x202, RBUTTONDOWN: 0x204, RBUTTONUP: 0x205, MBUTTONDOWN: 0x207,
  MBUTTONUP: 0x208, MOUSEWHEEL: 0x20A, ACTIVATEAPP: 0x1C, ACTIVATE: 0x06, SETFOCUS: 0x07, KILLFOCUS: 0x08 };

/* ------------------------------------------------------------- resolution */

// Render at the window's size in device pixels (capped at 2560 wide), or at
// ?res=WxH. The game reads it at startup, so a change needs a reload.
function gameResolution() {
  const m = /^(\d+)x(\d+)$/.exec(params.get('res') || '');
  if (m) return `${m[1]}x${m[2]}`;
  const dpr = Math.min(window.devicePixelRatio || 1, 2);
  let w = Math.round(window.innerWidth * dpr), h = Math.round(window.innerHeight * dpr);
  const scale = Math.min(1, 2560 / w, 1600 / h);
  w = Math.max(800, Math.round(w * scale / 8) * 8);
  h = Math.max(600, Math.round(h * scale / 8) * 8);
  return `${w}x${h}`;
}

/* ----------------------------------------------------------------- module */

var Module = {
  rebelFrame: rebelFrameTick,
  rebelPacing: params.has('raf') ? 'raf' : params.has('noraf') ? 'spin' : 'auto', // see Rebel_JS_Wait_Frame
  canvas,
  arguments: [...(params.get('args') || '').split(' ').filter(Boolean), '--res=' + gameResolution()],
  print: (t) => log(t),
  printErr: (t) => log(t),
  locateFile: (p) => p,
  setStatus: (t) => { if (t) log('[emscripten] ' + t); },
  onAbort: (what) => {
    // Emscripten hands us the original error object: keep its wasm stack.
    if (what && what.stack) log('[abort] ' + what.stack);
    overlay.style.display = 'flex';
    setStatus('The game stopped: ' + what + '\nAdd ?log to the URL for details.');
  },
};
window.Module = Module;

let running = false;
function post(msg, wp, lp) { if (running) Module._Rebel_Post_Message(msg, wp >>> 0, lp | 0); }

function canvasPoint(e) {
  const r = canvas.getBoundingClientRect();
  // object-fit: contain letterboxing
  const scale = Math.min(r.width / canvas.width, r.height / canvas.height);
  const w = canvas.width * scale, h = canvas.height * scale;
  const x = (e.clientX - r.left - (r.width - w) / 2) / scale;
  const y = (e.clientY - r.top - (r.height - h) / 2) / scale;
  return [Math.max(0, Math.min(canvas.width - 1, x | 0)), Math.max(0, Math.min(canvas.height - 1, y | 0))];
}
let mouseX = 0, mouseY = 0;
const lparamXY = (x, y) => ((y & 0xFFFF) << 16) | (x & 0xFFFF);

let lastEscape = -1e9, sentEscape = -1e9;
function onKey(e, down) {
  if (!running) return;
  if (e.code === 'Escape') {
    // A real Escape right after one we synthesized for a pointer-lock exit
    // is the same press arriving late: drop it.
    if (performance.now() - sentEscape < 300) { e.preventDefault(); return; }
    if (down) lastEscape = performance.now();
  }
  const k = KEYS[e.code];
  if (!k) return;
  // Let the browser keep a few shortcuts when not in pointer lock.
  if (!document.pointerLockElement && (e.metaKey || (e.ctrlKey && (e.code === 'KeyR' || e.code === 'KeyL')))) return;
  e.preventDefault();
  const [scan, vk] = k;
  const extended = scan >= 0x80 ? 1 << 24 : 0;
  Module._Rebel_Input_Key(scan, down ? 1 : 0);
  const lp = 1 | ((scan & 0x7F) << 16) | extended | (down ? (e.repeat ? 1 << 30 : 0) : (3 << 30));
  const sys = e.altKey && vk !== 0x12 ? (down ? WM.SYSKEYDOWN : WM.SYSKEYUP) : (down ? WM.KEYDOWN : WM.KEYUP);
  post(sys, vk, lp);
  if (down && e.key.length === 1 && !e.ctrlKey && !e.metaKey) post(WM.CHAR, e.key.codePointAt(0), lp);
  else if (down && (vk === 0x08 || vk === 0x0D || vk === 0x1B || vk === 0x09)) post(WM.CHAR, vk, lp);
}

function installInput() {
  window.addEventListener('keydown', (e) => onKey(e, true));
  window.addEventListener('keyup', (e) => onKey(e, false));
  canvas.addEventListener('mousedown', (e) => {
    if (!running) return;
    canvas.focus();
    if (!document.pointerLockElement && !params.has('nolock')) canvas.requestPointerLock?.();
    if (!document.pointerLockElement) {
      [mouseX, mouseY] = canvasPoint(e);
      Module._Rebel_Input_Set_Cursor(mouseX, mouseY);
    }
    const b = e.button === 2 ? 1 : e.button === 1 ? 2 : 0;
    Module._Rebel_Input_Mouse_Button(b, 1);
    post([WM.LBUTTONDOWN, WM.MBUTTONDOWN, WM.RBUTTONDOWN][e.button] || WM.LBUTTONDOWN, 1 << b, lparamXY(mouseX, mouseY));
  });
  window.addEventListener('mouseup', (e) => {
    if (!running) return;
    const b = e.button === 2 ? 1 : e.button === 1 ? 2 : 0;
    Module._Rebel_Input_Mouse_Button(b, 0);
    post([WM.LBUTTONUP, WM.MBUTTONUP, WM.RBUTTONUP][e.button] || WM.LBUTTONUP, 0, lparamXY(mouseX, mouseY));
  });
  window.addEventListener('mousemove', (e) => {
    if (!running) return;
    if (document.pointerLockElement === canvas) {
      Module._Rebel_Input_Mouse_Move(e.movementX | 0, e.movementY | 0);
      mouseX = Math.max(0, Math.min(canvas.width - 1, mouseX + e.movementX));
      mouseY = Math.max(0, Math.min(canvas.height - 1, mouseY + e.movementY));
    } else {
      // Free pointer (menus): place the game cursor exactly under the mouse.
      [mouseX, mouseY] = canvasPoint(e);
      Module._Rebel_Input_Set_Cursor(mouseX, mouseY);
    }
    post(WM.MOUSEMOVE, 0, lparamXY(mouseX, mouseY));
  });
  canvas.addEventListener('wheel', (e) => {
    if (!running) return;
    e.preventDefault();
    const notches = Math.sign(e.deltaY) * -120;
    Module._Rebel_Input_Mouse_Wheel(notches);
    post(WM.MOUSEWHEEL, (notches & 0xFFFF) << 16, lparamXY(mouseX, mouseY));
  }, { passive: false });
  const release = () => { if (running) Module._Rebel_Input_Release_All(); };
  window.addEventListener('blur', () => { release(); post(WM.ACTIVATEAPP, 0, 0); });
  window.addEventListener('focus', () => post(WM.ACTIVATEAPP, 1, 0));
  // Chrome uses Escape to leave pointer lock and doesn't pass that key press
  // to the page, so the game would never open its pause menu. When the lock
  // goes away without an Escape we saw, send one to the game ourselves.
  document.addEventListener('pointerlockchange', () => {
    if (document.pointerLockElement) return;
    release();
    setTimeout(() => {
      if (!running || performance.now() - lastEscape < 300) return;
      sentEscape = performance.now();
      const lp = 1 | (0x01 << 16);
      Module._Rebel_Input_Key(0x01, 1);
      post(WM.KEYDOWN, 0x1B, lp);
      post(WM.CHAR, 0x1B, lp);
      setTimeout(() => { Module._Rebel_Input_Key(0x01, 0); post(WM.KEYUP, 0x1B, lp | (3 << 30)); }, 50);
    }, 150);
  });
}

/* ------------------------------------------------------------- fullscreen */

// Alt+Enter toggles fullscreen. In fullscreen Chrome's Keyboard Lock hands
// the game keys the browser would otherwise take (Ctrl+W closes the tab and
// Ctrl is crouch; Escape leaves pointer lock). Holding Escape still exits.
async function toggleFullscreen() {
  if (document.fullscreenElement) { await document.exitFullscreen().catch(() => {}); return; }
  try {
    await document.documentElement.requestFullscreen({ navigationUI: 'hide' });
    await navigator.keyboard?.lock?.();
  } catch (e) { log('[page] fullscreen: ' + e.message); }
}
window.addEventListener('keydown', (e) => {
  if (e.code === 'Enter' && e.altKey && !e.repeat) { e.preventDefault(); e.stopImmediatePropagation(); toggleFullscreen(); }
}, true);
document.addEventListener('fullscreenchange', () => { if (!document.fullscreenElement) navigator.keyboard?.unlock?.(); });
// Outside fullscreen, ask before leaving (Ctrl+W / Cmd+W mid-mission).
window.addEventListener('beforeunload', (e) => { if (running && !params.has('noguard')) { e.preventDefault(); e.returnValue = ''; } });

/* ------------------------------------------------------------------ audio */

// Browsers start AudioContexts suspended until a user gesture.
const audioContexts = [];
for (const name of ['AudioContext', 'webkitAudioContext']) {
  const Orig = window[name];
  if (!Orig) continue;
  window[name] = function (...a) { const c = new Orig(...a); audioContexts.push(c); return c; };
  window[name].prototype = Orig.prototype;
}
// Closed contexts (finished cutscenes) are dropped instead of resumed.
const resumeAudio = () => {
  for (let i = audioContexts.length - 1; i >= 0; i--) {
    const c = audioContexts[i];
    if (c.state === 'closed') audioContexts.splice(i, 1);
    else if (c.state !== 'running') c.resume().catch(() => {});
  }
};
['pointerdown', 'keydown'].forEach((t) => window.addEventListener(t, resumeAudio, true));

/* ----------------------------------------------------------------- movies */

// Cutscenes: the game's own .bik files from the imported data, decoded in the
// page (bink-player.js). A transcoded movies/<name>.webm (self-hosting with
// tools/transcode-movies.sh) is the fallback when the .bik isn't there.
const movieCanvas = $('moviecanvas');
let movieDone = null, binkPlayback = null;

async function findMovieFile(name) {
  try {
    let dir = await (await navigator.storage.getDirectory()).getDirectoryHandle('renegade');
    for (const part of ['data', 'movies']) {
      let next = null;
      for await (const [n, h] of dir.entries()) if (h.kind === 'directory' && n.toLowerCase() === part) next = h;
      if (!next) return null;
      dir = next;
    }
    const want = name.toLowerCase() + '.bik';
    for await (const [n, h] of dir.entries()) if (h.kind === 'file' && n.toLowerCase() === want) return await h.getFile();
  } catch (e) {}
  return null;
}

function playWebm(name) {
  movie.src = 'movies/' + encodeURIComponent(name.toLowerCase()) + '.webm';
  movie.style.display = 'block';
  movie.onended = () => Module.rebelStopMovie();
  movie.onerror = () => { log('[movie] not available: ' + name); Module.rebelStopMovie(); };
  movie.play().catch(() => Module.rebelStopMovie());
}

Module.rebelPlayMovie = (name, done) => {
  log('[movie] play ' + name);
  movieDone = done;
  window.rebelMovieActive = true;
  findMovieFile(name).then((file) => {
    if (movieDone !== done) return; // stopped meanwhile
    if (!file) return playWebm(name);
    movieCanvas.style.display = 'block';
    binkPlayback = BinkPlayer.play(file, movieCanvas, () => { binkPlayback = null; Module.rebelStopMovie(); });
  });
};
Module.rebelStopMovie = () => {
  if (binkPlayback) {
    const p = binkPlayback;
    binkPlayback = null;
    p.stop(); // calls back into rebelStopMovie, now with no playback
  }
  movieCanvas.style.display = 'none';
  movie.pause();
  movie.removeAttribute('src');
  movie.load();
  movie.style.display = 'none';
  window.rebelMovieActive = false;
  const d = movieDone;
  movieDone = null;
  if (d) d();
};
window.addEventListener('keydown', (e) => { if (movieDone && (e.code === 'Escape' || e.code === 'Space' || e.code === 'Enter')) Module.rebelStopMovie(); }, true);

/* ----------------------------------------------------------------- import */

// Game data lives in the browser's private storage (OPFS, renegade/...),
// copied once from either
//  - a folder the player picks: their own install (public site), or
//  - gamedata/ on the serving machine (self-hosting with tools/serve.mjs).
// Nothing is uploaded anywhere: the copy stays in this browser.
const LOCAL_IMPORT_VERSION = 'local-1';
// What the engine needs, relative to the install folder.
const REQUIRED_FILES = ['data/always.dat', 'data/always2.dat', 'data/m13.mix', 'data/m01.mix', 'ari_____.ttf', '54251___.ttf'];
const ROOT_FILES = /\.(ttf|dat|ini|lcf|txt|016|256)$/i;

async function storageRoot(create) {
  return (await navigator.storage.getDirectory()).getDirectoryHandle('renegade', { create: !!create });
}

async function serverManifest() {
  // Public builds (tools/deploy-web.sh) say so: no server copy to probe for.
  if (document.querySelector('meta[name="rebel-gamedata"][content="none"]')) return null;
  try {
    const r = await fetch('gamedata/manifest.json', { cache: 'no-store' });
    if (!r.ok || !(r.headers.get('content-type') || '').includes('json')) return null;
    return await r.json();
  } catch (e) {
    return null;
  }
}

async function haveGameData(manifest) {
  try {
    const root = await storageRoot(false);
    const marker = (await (await (await root.getFileHandle('.rebel-import')).getFile()).text()).trim();
    if (manifest) return marker === String(manifest.version || 1);
    return marker.length > 0;
  } catch (e) {
    return false;
  }
}

function runImport(message) {
  return new Promise((resolve, reject) => {
    progressEl.hidden = false;
    const w = new Worker('import-worker.js');
    w.onmessage = ({ data }) => {
      if (data.type === 'progress') {
        progressEl.value = data.done / data.total;
        setStatus(`Copying your game files into this browser (one time, nothing is uploaded)\n${fmtBytes(data.done)} of ${fmtBytes(data.total)}\n${data.file}`);
      } else if (data.type === 'done') {
        progressEl.hidden = true;
        w.terminate();
        resolve();
      } else if (data.type === 'error') {
        progressEl.hidden = true;
        w.terminate();
        reject(new Error(data.message));
      }
    };
    w.postMessage(message);
  });
}

// Picks the files to copy from a list of { path, getFile } relative to the
// chosen folder; accepts the install folder itself or its parent.
function selectInstallFiles(entries) {
  const lower = new Map(entries.map((e) => [e.path.toLowerCase(), e]));
  let prefix = null;
  for (const e of entries) {
    const p = e.path.toLowerCase();
    if (p.endsWith('data/always.dat') && p.split('/').length <= 3) { prefix = p.slice(0, -'data/always.dat'.length); break; }
  }
  if (prefix === null) {
    if (lower.has('always.dat')) throw new Error('That looks like the Data folder. Please choose the folder that contains it (the game\'s install folder).');
    throw new Error('No game files found there (looking for Data/always.dat). Please choose the game\'s install folder.');
  }
  const missing = REQUIRED_FILES.filter((f) => !lower.has(prefix + f));
  if (missing.length) throw new Error('This install is missing ' + missing.join(', ') + '. A complete, patched (1.037) install is needed, for example the Steam version.');
  const files = [];
  for (const e of entries) {
    const p = e.path.toLowerCase();
    if (!p.startsWith(prefix)) continue;
    const rel = e.path.slice(prefix.length);
    if (rel.toLowerCase().startsWith('data/') || (!rel.includes('/') && ROOT_FILES.test(rel))) files.push({ path: rel, entry: e });
  }
  return files;
}

// Lists a picked directory (File System Access API). Below the chosen folder
// only Data folders are walked in full, so unrelated big folders (editors,
// redistributables, other games when a parent folder is chosen) stay cheap.
async function listDirectory(handle, prefix = '', depth = 0, inData = false) {
  const out = [];
  for await (const [name, h] of handle.entries()) {
    const path = prefix + name;
    if (h.kind === 'file') {
      out.push({ path, getFile: () => h.getFile() });
      continue;
    }
    const isData = name.toLowerCase() === 'data';
    if ((depth === 0 || isData || inData) && depth < 5) out.push(...(await listDirectory(h, path + '/', depth + 1, inData || isData)));
  }
  return out;
}

// Shows the folder chooser and resolves with the files to import.
function chooseInstallFolder() {
  return new Promise((resolve) => {
    const box = $('choose');
    const pickBtn = $('pick'), input = $('pickinput'), err = $('pickerror');
    box.hidden = false;
    const done = async (entries) => {
      try {
        const files = selectInstallFiles(entries);
        box.hidden = true;
        resolve(files);
      } catch (e) {
        err.textContent = e.message;
      }
    };
    pickBtn.onclick = async () => {
      err.textContent = '';
      if (window.showDirectoryPicker) {
        try {
          const dir = await window.showDirectoryPicker({ id: 'game-install', mode: 'read' });
          setStatus('Reading the folder…');
          await done(await listDirectory(dir));
        } catch (e) {
          if (e.name !== 'AbortError') err.textContent = e.message;
        }
      } else {
        input.click();
      }
    };
    input.onchange = () => {
      const entries = [...input.files].map((f) => ({ path: f.webkitRelativePath.split('/').slice(1).join('/'), getFile: async () => f }));
      done(entries);
    };
  });
}

async function importFromFolder() {
  const files = await chooseInstallFolder();
  const est = await navigator.storage.estimate?.();
  const need = (await Promise.all(files.map(async (f) => (await f.entry.getFile()).size))).reduce((a, b) => a + b, 0);
  if (est && est.quota && est.quota - (est.usage || 0) < need) throw new Error(`Not enough browser storage: the game needs ${fmtBytes(need)}, ${fmtBytes(est.quota - (est.usage || 0))} available.`);
  const list = await Promise.all(files.map(async (f) => ({ path: f.path, file: await f.entry.getFile() })));
  await runImport({ files: list, version: LOCAL_IMPORT_VERSION });
}

// "Forget game files": removes the copy from this browser's storage.
async function forgetGameData() {
  if (!confirm('Remove the game files copied into this browser? Saves and settings are removed too.')) return;
  try { await (await navigator.storage.getDirectory()).removeEntry('renegade', { recursive: true }); } catch (e) {}
  try { await (await navigator.storage.getDirectory()).removeEntry('persist', { recursive: true }); } catch (e) {}
  location.reload();
}

/* ------------------------------------------------------------------- boot */

function loadScript(src) {
  return new Promise((resolve, reject) => {
    const s = document.createElement('script');
    s.src = src;
    s.onload = resolve;
    s.onerror = () => reject(new Error('could not load ' + src));
    document.body.appendChild(s);
  });
}

async function boot() {
  if (!window.crossOriginIsolated) {
    setStatus('This page must be served with cross-origin isolation (COOP/COEP) for threads to work.');
    return;
  }
  if (!navigator.storage?.getDirectory) {
    setStatus('This browser has no Origin Private File System; use a current Chrome, Edge, Firefox or Safari.');
    return;
  }
  try { await navigator.storage.persist?.(); } catch (e) {}
  const manifest = await serverManifest(); // self-hosting: the server offers its copy
  for (;;) {
    if (await haveGameData(manifest)) break;
    try {
      if (manifest) await runImport({ manifestUrl: 'gamedata/manifest.json', baseUrl: 'gamedata/' });
      else {
        setStatus('Choose your game folder to start.');
        await importFromFolder();
      }
    } catch (e) {
      setStatus('Import failed: ' + e.message);
      if (manifest) return;
    }
  }
  $('forget').hidden = false;
  $('forget').onclick = (e) => { e.preventDefault(); forgetGameData(); };
  setStatus('Loading the engine…');
  const ready = new Promise((resolve) => { Module.onRuntimeInitialized = resolve; });
  await loadScript('renegade.js');
  await ready;
  setStatus('Ready.');
  startBtn.hidden = false;
  startBtn.focus();
  await new Promise((resolve) => startBtn.addEventListener('click', resolve, { once: true }));
  resumeAudio();
  overlay.style.display = 'none';
  installInput();
  running = true;
  canvas.focus();
  Module.callMain(Module.arguments);
}

boot().catch((e) => { setStatus('Startup failed: ' + e.message); log(String(e.stack || e)); });
