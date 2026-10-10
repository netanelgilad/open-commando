# Web port: known gaps

Features the web build currently lacks or approximates. Each entry says
where the shortcut lives and what "done" looks like.

## Audio

- **Reverb (EAX room effects) is off.** `AIL_enumerate_filters` reports no
  filter provider, so WWAudio disables reverb; `AIL_set_3D_sample_effects_level`
  stores the level but nothing uses it.
  *Where:* `Code/Platform/audio/mss_openal.cpp`.
  *Done when:* a Web Audio convolver/reverb send (or OpenAL EFX if Emscripten
  gains it) honours the room type and per-sample effects level.
- **Stereo samples can't be panned** (OpenAL limitation); only mono pans.

## Graphics / text

- **Italic fonts aren't synthesized.** GDI text runs on stb_truetype
  (`Code/Platform/gfx/gdi_text.cpp`); bold is emulated by 1px widening,
  italic requests render upright. The shipped fonts (Arial MT, Regatta
  Condensed LET) have no italic files.
- **Gamma ramp** is applied as an SVG `feComponentTransfer` filter on the
  canvas (`Rebel_JS_Set_Gamma` in `d3d9_device.cpp`). Check that Chrome keeps
  it on the GPU; if a non-default brightness costs frames, move it into a
  final present pass.
- **Resolution** is chosen by the page: the window size in device pixels
  (capped at 2560x1600), or `?res=WxH`. It is written to the registry before
  WinMain (`main_web.cpp`), because the Windows game gets it from the separate
  WWConfig tool. The in-game Video tab only displays it, and a change needs a
  reload. Menus are 4:3 layouts stretched to the window, as on Windows.
- **Frame grabber** (AVI movie capture, developer tool) is inert.

## Text encoding

- **WCHAR is 4 bytes in memory, 2 on disk/wire.** UTF-16 is converted at the
  chunk macros (`WWLib/chunkio.h`), translation objects, registry and
  `BitStreamClass` wide chars. Any other place that reads/writes raw WCHAR
  bytes (none found yet) would need the same treatment. Characters outside
  the BMP are written as '?'.

## Networking

- **Westwood Online / GameSpy** code paths are excluded (`connect.cpp`,
  `#ifndef REBEL_PORTABLE`) and their services are offline.
- **Datagram sockets are virtual** (`Code/Platform/net/vnet.cpp`, page side
  `web/net.js`): `?net=ws://.../room/<name>` joins a hub room
  (`services/hub`), `?net=tab:<room>` other tabs. Stream (TCP) sockets still
  go to libc and fail (bandwidth test, GameSpy, WOL). There is no DNS:
  only dotted quads and the local host name resolve. Plan and spike log:
  `docs/ONLINE_PLAN.md`, `docs/NET_SPIKES.md`.
- **One game per browser profile.** Two tabs of the same profile share the
  OPFS game files, and the first tab holds exclusive sync access handles on
  them (the read-fd pool), so a second tab's file reads fail and it crashes
  while booting. Fix later: read-only access handles (Chrome's
  `createSyncAccessHandle({mode: 'read-only'})`) or a "game already open in
  another tab" message (Web Locks).

## Platform

- **Win32 dialogs** (`DialogBoxParam` etc.) return cancel; any engine path
  that relies on a native dialog needs an in-game or HTML replacement.
- **`SuspendThread` / `TerminateThread`** are no-ops (pthreads can't do it).
- **Blocking waits on the browser main thread** return immediately
  (`Sleep`, `WaitForSingleObject`); revisit once the main-loop model
  (ASYNCIFY/JSPI vs. worker) is settled.
- **Cutscenes (Bink)** are transcoded offline to WebM (`tools/transcode-movies.sh`)
  and played in a `<video>` overlay. Esc, Space or Enter skips. (No subtitles: the
  retail data has no subtitle.ini, so Windows shows none either.)
- **Pointer lock and Escape**: Chrome uses Escape to leave pointer lock and
  doesn't pass it to the page, so the page sends the game an Escape when the
  lock is lost (`web/rebel.js`). Losing focus also opens the pause menu.

## Performance / build

State as of 2026-10-09: M13 at 1280x800 runs at the 60 fps cap with 4-7 ms of
game work per frame (GPU ~1 ms). The M13 level load takes ~3 s and boot to
the main menu ~2.5 s.

- **Builds:** `build/wasm-release` (-O3, no WWDEBUG or asserts, function names
  kept for profiles) is served at `/`. `build/wasm` (debug: -O0, ASSERTIONS,
  WWDEBUG logs) is served at `/dbg/` on the same origin, so both share the
  imported game data. `tools/build-web.sh release` and `tools/build-web.sh debug` build
  them.
- **WebGL state cache** (`gfx/gl_state.*`): only changed state is issued
  (4123 down to ~400 calls per frame). Re-issuing everything per draw had
  saturated Chrome's GPU process. New GL state code must go through `gls::`.
- **Dynamic buffers** are drawn from their CPU copy via per-draw scratch
  uploads. Partial updates of in-use buffers made ANGLE copy whole buffers.
- **Pacing:** the game follows requestAnimationFrame when it ticks at display
  rate; otherwise it spins zero-delay message hops to 60 fps. On throttled
  machines timers fire 50 ms late. `?raf` / `?noraf` force a mode.
- **File I/O:**
  - Read-only opens use OPFS sync access handles (WasmFS blob reads were async
    round trips), and their descriptors are pooled.
  - Lookups of absent files are answered from the directory cache.
  - Registry writes are batched to once per frame.
  - Other writes are buffered, and files created for writing (saves) live in
    memory until closed: an autosave took 25 s before and takes ~0.1 s now.
- **Not done:** JSPI instead of ASYNCIFY (smaller and faster wasm), not
  needed at current frame times.
- **Pointer lock only:** mouse-look needs pointer lock (click the canvas).
  Without it, for example with `?nolock`, the mouse only drives the menu cursor.
- **Profiling tools** (tools/browser-test):
  - `load-profile.mjs` prints the [load] phase timeline; BOOT=1 profiles boot.
  - `worker-profile.mjs` profiles the pthreads.
  - `timers.mjs` counts GL calls and probes timer latency.
  - `analyze.mjs`, `callers.mjs` and `window.mjs` read the profiles.

## Verified

Browser tests are in `tools/browser-test/`, headless Chromium at 1280x800:
- `complete.mjs` checks the M13 flow:
  - Escape opens the EVA menu, and a second Escape returns to the game.
  - Saving, walking away and loading puts the player back where they saved.
  - Winning shows the score screen, and Continue loads M01.
  - Samples and music are playing, and the output level is nonzero.
- `campaign.mjs` wins every mission in turn: M13 and M01-M11 all load, are
  playable and reach the score screen (about 30 min).
- `practice.mjs` starts Multiplayer Practice (C&C mode against bots): it is
  playable, credits tick up and firing works.
- `menus.mjs` covers the menus:
  - Every Options sub-menu and every Configuration tab open.
  - Rebinding a key in Controls works.
  - Escape backs out to the right parent.
- `eva.mjs` opens every EVA tab.
- Settings persist in OPFS (`/opfs/persist/registry.txt`).

## Open bugs

- **Rare hang in the M13 intro cinematic** (main thread busy, about 55-60 s
  in).
  - Seen twice before the varargs and pacing fixes, and once on 2026-10-09 in
    roughly 25 intro runs.
  - It hasn't reproduced since, including 12 runs under the debugger or
    tracing.
  - A watchdog thread (`main_web.cpp`) now prints `[hang] main thread stuck
    for N s; last step: ...; scopes: ...` (the engine's WWPROFILE scope chain)
    to the console after 10 s without progress.
  - The next occurrence will say where it is.
