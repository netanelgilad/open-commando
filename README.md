# Open Commando

The 2002 first-person shooter engine that Electronic Arts released as open
source, running in a web browser. It is a WebAssembly build of the GPL source,
and you play with **your own copy** of the original game's data (Command &
Conquer™ Renegade, v1.037, e.g. from Steam).

Not affiliated with or endorsed by Electronic Arts. See [NOTICE.md](NOTICE.md)
and [LICENSE.md](LICENSE.md) (GPL v3 with EA's additional terms).

**Play:** https://open-commando.netanelgilad.workers.dev

1. Use a desktop Chrome or Edge (Firefox and Safari should work but are less
   tested).
2. Choose your game's install folder. On Steam: Library → right-click the
   game → Manage → Browse local files.
3. Wait while it copies the ~1.3 GB of game files into the browser's private
   storage, once. Nothing is uploaded.

## Status

- **Complete single-player campaign:** all 12 missions load, play and
  finish, and save/load, autosave, menus, options and key rebinding work.
- **Multiplayer Practice** (C&C mode against bots) works. Online
  multiplayer is planned ([docs/ONLINE_PLAN.md](docs/ONLINE_PLAN.md)).
- **Performance** (Apple M4, 1280x800): 60 fps with 4-7 ms of game work per
  frame, a ~3 s level load and ~2.5 s to the main menu.
- **Known gaps:** [docs/WEB_PORT_GAPS.md](docs/WEB_PORT_GAPS.md).

## How it works

- **Platform layer** (`Code/Platform`, `REBEL_PORTABLE`): the engine's
  Windows dependencies are replaced, so the original game code runs mostly
  unchanged:
  - Win32 window, files, registry and threads on POSIX;
  - Direct3D 9 fixed-function on WebGL 2 (with a state cache);
  - Miles audio on OpenAL;
  - DirectInput on browser events;
  - Bink cutscenes on HTML video.
- **Emscripten build:** pthreads, WasmFS with the Origin Private File System
  (game data and saves stay in your browser), ASYNCIFY for the game's
  blocking main loop.
- **Page** (`web/`): imports your game files into browser storage once,
  then starts the engine.

## Running it locally

Requirements:
- emsdk, CMake and LLVM (`llvm-rc`) to build;
- a Chromium-based browser (Chrome, Edge) or another browser with OPFS and
  WebGL 2;
- your own install of the game.

```sh
tools/build-web.sh release                 # -> build/wasm-release/bin/renegade.{js,wasm}
REBEL_GAME_DIR=/path/to/your/game node tools/serve.mjs
# open http://127.0.0.1:8461/
```

- `tools/serve.mjs` serves the page with the cross-origin isolation headers
  the engine needs.
- It also serves your game folder to your browser **on your machine only**,
  for the one-time import. Don't expose it publicly: the game data is not
  ours to distribute.
- Cutscenes are optional. They are converted from the game's Bink files to
  WebM (VP9/Opus) with ffmpeg, then placed in a folder served as
  `/movies/<name>.webm` (`REBEL_MOVIES_DIR`).

URL options:
- `?res=1920x1080`: rendering resolution (default: window size);
- `?log`: on-page log;
- `?quiet`: less console output;
- `?nofps`: hide the frame counter (Alt+F toggles it);
- `?nolock`: no pointer lock;
- `?raf` / `?noraf`: frame pacing mode.

In game:
- Alt+Enter: fullscreen with keyboard lock (keeps Ctrl and Esc in the
  game);
- click: capture the mouse.

## Hosting your own public copy

```sh
tools/build-web.sh release && tools/build-bink.sh
tools/deploy-web.sh             # -> dist/ (page, engine, cutscene decoder, _headers)
tools/deploy-web.sh --publish   # Cloudflare Workers static assets (wrangler.jsonc)
```

- `dist/` holds code only.
- Players import their own game files in the page, and cutscenes are decoded
  in the page from their `.bik` files.
- Any static host works if it can send the `_headers` (COOP/COEP).
- The page links to the exact source commit it was built from.

## Development

- `tools/build-web.sh debug`: build with asserts and engine debug logs,
  served at `/dbg/`.
- `tools/browser-test/`: Playwright scripts that drive the real game in
  headless Chromium:
  - the full campaign, save/load, menus, Multiplayer Practice;
  - profilers for frames, loads and threads.
  - See [docs/WEB_PORT_GAPS.md](docs/WEB_PORT_GAPS.md).
- Plans: [online multiplayer](docs/ONLINE_PLAN.md),
  [remaster](docs/REMASTER_PLAN.md), [open source](docs/OPEN_SOURCE_PLAN.md).

---

# Upstream: Rebel

What follows is the README of the Rebel project this work builds on (Windows
builds).

# Rebel - Command & Conquer Renegade source code enhancement project

The project aimed at improving the source code of CnC: Renegade for running on modern platforms and adding new features.

## Major features

* DirectX 9 support.

See [CHANGES.md](CHANGES.md) for full changelog.

## Hosting your own public copy

```sh
tools/build-web.sh release && tools/build-bink.sh
tools/deploy-web.sh             # -> dist/ (page, engine, cutscene decoder, _headers)
tools/deploy-web.sh --publish   # Cloudflare Workers static assets (wrangler.jsonc)
```

- `dist/` holds code only.
- Players import their own game files in the page, and cutscenes are decoded
  in the page from their `.bik` files.
- Any static host works if it can send the `_headers` (COOP/COEP).
- The page links to the exact source commit it was built from.

## Development and Dependencies

* Microsoft Visual C++ 2022
* Microsoft Platform SDK 10.0
* CMake 3.20
* GoogleTest (vcpkg package, optional)
* Microsoft.DXSDK.D3DX 9.29.952.8 (vcpkg package)
* RAD Miles Sound System 6 SDK (`Code/third_party/miles6`)
* RAD Bink SDK (`Code/third_party/bink`)

## Runtime Dependencies

To use the compiled binaries, you must own the game, fully patched to v1.037. Best choice is purchase CnC: Renegade
on [Steam](https://store.steampowered.com/app/2229890/Command__Conquer_Renegade/).

Minimal requirements for system:

* Windows 10
* A processor that supports the SSE2 instruction set
* A DirectX 9 compliant video card

## Licensing

This repository and its contents are licensed under the GPL v3 license, with additional terms applied.
Please see [LICENSE.md](LICENSE.md) for details.

CnC: Rebel uses third party libraries, described below.

### crnlib

Crunch - advanced DXTc texture compression and transcoding library.

License: Public Domain

### stb

stb -  single-file public domain libraries for C/C++.

License: Public Domain / MIT
