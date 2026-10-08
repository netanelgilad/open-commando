// Page runtime for the Renegade web build: game-data import, Emscripten
// module setup, input forwarding (Win32 messages + DirectInput), audio
// unlock and the cutscene <video> overlay.
'use strict';

const $ = (id) => document.getElementById(id);
const overlay = $('overlay'), statusEl = $('status'), progressEl = $('progress'), startBtn = $('start');
const canvas = $('canvas'), movie = $('movie'), logEl = $('log');
const params = new URLSearchParams(location.search);
if (params.has('log')) document.body.classList.add('show-log');

function log(line) {
  console.log(line);
  logEl.textContent += line + '\n';
  if (logEl.textContent.length > 200000) logEl.textContent = logEl.textContent.slice(-150000);
  logEl.scrollTop = logEl.scrollHeight;
}
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

/* ----------------------------------------------------------------- module */

var Module = {
  canvas,
  arguments: (params.get('args') || '').split(' ').filter(Boolean),
  print: (t) => log(t),
  printErr: (t) => log(t),
  locateFile: (p) => p,
  setStatus: (t) => { if (t) log('[emscripten] ' + t); },
  onAbort: (what) => { overlay.style.display = 'flex'; setStatus('The game stopped: ' + what + '\nAdd ?log to the URL for details.'); },
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

function onKey(e, down) {
  if (!running) return;
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
    [mouseX, mouseY] = document.pointerLockElement ? [mouseX, mouseY] : canvasPoint(e);
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
      const [x, y] = canvasPoint(e);
      Module._Rebel_Input_Mouse_Move(x - mouseX, y - mouseY);
      mouseX = x; mouseY = y;
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
  document.addEventListener('pointerlockchange', () => { if (!document.pointerLockElement) release(); });
}

/* ------------------------------------------------------------------ audio */

// Browsers start AudioContexts suspended until a user gesture.
const audioContexts = [];
for (const name of ['AudioContext', 'webkitAudioContext']) {
  const Orig = window[name];
  if (!Orig) continue;
  window[name] = function (...a) { const c = new Orig(...a); audioContexts.push(c); return c; };
  window[name].prototype = Orig.prototype;
}
const resumeAudio = () => audioContexts.forEach((c) => c.state !== 'running' && c.resume());
['pointerdown', 'keydown'].forEach((t) => window.addEventListener(t, resumeAudio, true));

/* ----------------------------------------------------------------- movies */

let movieDone = null;
Module.rebelPlayMovie = (name, done) => {
  movieDone = done;
  movie.src = 'movies/' + encodeURIComponent(name.toLowerCase()) + '.webm';
  movie.style.display = 'block';
  const finish = () => { Module.rebelStopMovie(); };
  movie.onended = finish;
  movie.onerror = () => { log('[movie] not available: ' + name); finish(); };
  movie.play().catch(() => finish());
};
Module.rebelStopMovie = () => {
  movie.pause();
  movie.removeAttribute('src');
  movie.load();
  movie.style.display = 'none';
  const d = movieDone;
  movieDone = null;
  if (d) d();
};
window.addEventListener('keydown', (e) => { if (movieDone && (e.code === 'Escape' || e.code === 'Space' || e.code === 'Enter')) Module.rebelStopMovie(); }, true);

/* ----------------------------------------------------------------- import */

async function haveGameData() {
  try {
    const root = await (await navigator.storage.getDirectory()).getDirectoryHandle('renegade');
    const marker = await (await root.getFileHandle('.rebel-import')).getFile();
    const manifest = await (await fetch('gamedata/manifest.json', { cache: 'no-store' })).json();
    return (await marker.text()).trim() === String(manifest.version || 1);
  } catch (e) {
    return false;
  }
}

function importGameData() {
  return new Promise((resolve, reject) => {
    progressEl.hidden = false;
    const w = new Worker('import-worker.js');
    w.onmessage = ({ data }) => {
      if (data.type === 'progress') {
        progressEl.value = data.done / data.total;
        setStatus(`Copying your Renegade files into this browser (one time)\n${fmtBytes(data.done)} of ${fmtBytes(data.total)}\n${data.file}`);
      } else if (data.type === 'done') {
        progressEl.hidden = true;
        w.terminate();
        resolve();
      } else if (data.type === 'error') {
        w.terminate();
        reject(new Error(data.message));
      }
    };
    w.postMessage({ manifestUrl: 'gamedata/manifest.json', baseUrl: 'gamedata/' });
  });
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
  if (!(await haveGameData())) await importGameData();
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
