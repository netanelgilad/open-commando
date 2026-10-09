// Copies the game files (picked by the player, or listed in
// gamedata/manifest.json when self-hosting) into the Origin Private File System (renegade/...). Runs in a worker because synchronous
// access handles (the fast write path, and the only one Safari offers) are
// worker-only. Files that already exist with the right size are skipped, so
// an interrupted import resumes where it stopped.
'use strict';

async function dirFor(root, parts) {
  let dir = root;
  for (const p of parts) dir = await dir.getDirectoryHandle(p, { create: true });
  return dir;
}

async function existingSize(dir, name) {
  try {
    const fh = await dir.getFileHandle(name);
    return (await fh.getFile()).size;
  } catch (e) {
    return -1;
  }
}

// Two sources: { manifestUrl, baseUrl } fetches files from the serving
// machine (self-hosting); { files: [{ path, file }], version } copies File
// objects the player picked from their own install.
self.onmessage = async (ev) => {
  const { manifestUrl, baseUrl, files: picked } = ev.data;
  try {
    const manifest = picked
      ? { version: ev.data.version, files: picked.map((f) => ({ path: f.path, size: f.file.size, file: f.file })) }
      : await (await fetch(manifestUrl, { cache: 'no-store' })).json();
    const root = await (await navigator.storage.getDirectory()).getDirectoryHandle('renegade', { create: true });
    const total = manifest.files.reduce((n, f) => n + f.size, 0);
    let done = 0;
    for (const f of manifest.files) {
      const parts = f.path.split('/');
      const name = parts.pop();
      const dir = await dirFor(root, parts);
      if ((await existingSize(dir, name)) === f.size) {
        done += f.size;
        self.postMessage({ type: 'progress', done, total, file: f.path, skipped: true });
        continue;
      }
      const fh = await dir.getFileHandle(name, { create: true });
      const access = await fh.createSyncAccessHandle();
      access.truncate(0);
      let body;
      if (f.file) {
        body = f.file.stream();
      } else {
        const res = await fetch(baseUrl + f.path.split('/').map(encodeURIComponent).join('/'));
        if (!res.ok) throw new Error(`${f.path}: HTTP ${res.status}`);
        body = res.body;
      }
      const reader = body.getReader();
      let offset = 0, lastReport = 0;
      for (;;) {
        const { value, done: eof } = await reader.read();
        if (eof) break;
        access.write(value, { at: offset });
        offset += value.byteLength;
        done += value.byteLength;
        if (done - lastReport > (4 << 20)) {
          lastReport = done;
          self.postMessage({ type: 'progress', done, total, file: f.path });
        }
      }
      access.flush();
      access.close();
      if (offset !== f.size) throw new Error(`${f.path}: got ${offset} bytes, expected ${f.size}`);
    }
    // Marker: the version lets a future manifest force a re-check.
    const marker = await root.getFileHandle('.rebel-import', { create: true });
    const m = await marker.createSyncAccessHandle();
    const bytes = new TextEncoder().encode(String(manifest.version || 1));
    m.truncate(0);
    m.write(bytes, { at: 0 });
    m.flush();
    m.close();
    self.postMessage({ type: 'done', total });
  } catch (e) {
    self.postMessage({ type: 'error', message: String(e && e.message || e) });
  }
};
