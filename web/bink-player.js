// Plays the game's own Bink cutscenes (.bik from the imported game files) on
// a canvas, with audio through Web Audio. Decoding happens in bink.wasm (a
// minimal FFmpeg build: bink demuxer + decoders, see tools/build-bink.sh),
// a little ahead of playback. The audio clock drives the video frames.
'use strict';

const BinkPlayer = (() => {
  let modulePromise = null;
  function loadModule() {
    if (!modulePromise) {
      modulePromise = new Promise((resolve, reject) => {
        const s = document.createElement('script');
        s.src = 'bink.js';
        s.onload = () => createBinkModule().then(resolve, reject);
        s.onerror = () => reject(new Error('could not load bink.js'));
        document.head.appendChild(s);
      });
    }
    return modulePromise;
  }

  // Plays `blob` (a .bik file) on `canvas`; calls onDone() once at the end,
  // on stop() or on error. Returns { stop }.
  function play(blob, canvas, onDone) {
    let stopped = false, finished = false, M = null, dataPtr = 0, ctx = null, raf = 0, timer = 0;
    const sources = [];
    const finish = () => {
      if (finished) return;
      finished = true;
      stopped = true;
      cancelAnimationFrame(raf);
      clearTimeout(timer);
      for (const s of sources) try { s.stop(); } catch (e) {}
      if (ctx) ctx.close().catch(() => {});
      if (M) {
        M._bp_close();
        if (dataPtr) M._free(dataPtr);
      }
      onDone();
    };
    (async () => {
      M = await loadModule();
      const bytes = new Uint8Array(await blob.arrayBuffer());
      if (stopped) return finish();
      dataPtr = M._malloc(bytes.length);
      M.HEAPU8.set(bytes, dataPtr);
      const err = M._bp_open(dataPtr, bytes.length);
      if (err < 0) throw new Error('bink open failed ' + err);
      const w = M._bp_width(), h = M._bp_height();
      canvas.width = w;
      canvas.height = h;
      const g = canvas.getContext('2d');
      g.fillStyle = '#000';
      g.fillRect(0, 0, w, h);
      const rate = M._bp_sample_rate(), channels = M._bp_channels();
      ctx = rate ? new AudioContext({ sampleRate: rate }) : null;
      if (ctx && ctx.state !== 'running') ctx.resume().catch(() => {});
      const startWall = performance.now();
      const start = ctx ? ctx.currentTime + 0.2 : 0;
      const now = () => (ctx ? ctx.currentTime - start : (performance.now() - startWall) / 1000 - 0.2);
      const frames = []; // { pts, image }
      let audioTime = 0, eof = false;

      // Decode until ~1 s of video is queued and audio is scheduled ahead.
      const decode = () => {
        if (stopped) return;
        let budget = 30;
        while (!eof && budget-- > 0 && (frames.length < 15 || audioTime < now() + 1.0)) {
          const r = M._bp_next();
          if (r === 1) {
            const px = new Uint8ClampedArray(M.HEAPU8.buffer, M._bp_rgba(), w * h * 4);
            frames.push({ pts: M._bp_pts(), image: new ImageData(new Uint8ClampedArray(px), w, h) });
          } else if (r === 2 && ctx) {
            const n = M._bp_audio_samples();
            const pcm = new Float32Array(M.HEAPF32.buffer, M._bp_audio(), n * channels);
            const buf = ctx.createBuffer(channels, n, rate);
            for (let c = 0; c < channels; c++) {
              const out = buf.getChannelData(c);
              for (let i = 0; i < n; i++) out[i] = pcm[i * channels + c];
            }
            const src = ctx.createBufferSource();
            src.buffer = buf;
            src.connect(ctx.destination);
            src.start(start + audioTime);
            sources.push(src);
            src.onended = () => { const k = sources.indexOf(src); if (k >= 0) sources.splice(k, 1); };
            audioTime += n / rate;
          } else if (r <= 0) {
            eof = true;
          }
        }
        timer = setTimeout(decode, 30);
      };

      // Shows the newest due frame; false once playback is over.
      const tick = () => {
        if (stopped) return false;
        const t = now();
        let frame = null;
        while (frames.length && frames[0].pts <= t) frame = frames.shift();
        if (frame) g.putImageData(frame.image, 0, 0);
        if (eof && !frames.length && t >= audioTime) {
          finish();
          return false;
        }
        return true;
      };
      const rafLoop = () => { if (tick()) raf = requestAnimationFrame(rafLoop); };
      // requestAnimationFrame can be throttled (background tabs, some remote
      // sessions): a timer keeps frames moving as a fallback.
      const backup = () => { if (tick()) setTimeout(backup, 66); };
      decode();
      raf = requestAnimationFrame(rafLoop);
      setTimeout(backup, 66);
    })().catch((e) => { console.log('[movie] ' + (e && e.message || e)); finish(); });
    return { stop: finish };
  }

  return { play, load: loadModule };
})();
