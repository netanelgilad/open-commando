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
  `#ifndef REBEL_PORTABLE`). Multiplayer needs a WebSocket/WebRTC transport
  and a lobby to replace them.

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

- **Frame pacing**: the game paces itself to 60 fps and yields with a zero-delay
  task; it no longer waits on requestAnimationFrame. On this Mac mini, rAF
  ticked at about 5 Hz during missions. M13 now runs at roughly 40-47 fps,
  limited by game CPU work (about 20 ms per frame); GPU time is about 1 ms per
  frame. `?raf` restores rAF pacing for comparison. Next target: CPU per frame
  (profile with tools/browser-test/mission-profile.mjs).
- **Engine debug logging** still costs CPU: WWDEBUG output, script spam such as
  M00_Play_Sound and "NULL Script Ptr". A release build (no WWDEBUG) removes it.
- **Level load** takes about 90 s (I/O through the OPFS proxy, plus asset parsing).
- **Debug build**: shipped with `-sASSERTIONS=1 -sSTACK_OVERFLOW_CHECK=2` and
  WWDEBUG. Asserts are logged and play continues; `Commando_Assert_Handler` no
  longer shuts down input or calls ExitThread under REBEL_PORTABLE. A release
  build (and possibly JSPI instead of ASYNCIFY) is still to do.
- **Pointer lock only**: mouse-look needs pointer lock (click the canvas).
  Without it, for example with `?nolock`, the mouse only drives the menu cursor.

## Verified

`tools/browser-test/complete.mjs` checks, in M13:
- Escape opens the EVA menu.
- Save writes save\webtest.sav and its description and map read back.
- Walking away and then loading puts the player back where they saved.
- Winning shows the score screen, and Continue loads M01.
- Samples and music streams are playing.

`menus.mjs` opens every Options sub-menu, and `eva.mjs` opens every EVA tab.

## Open bugs

- **Intermittent hang in the M13 intro cinematic**, about 55-60 s of game time.
  Seen twice before the varargs and pacing fixes and not reproduced since.
  tools/browser-test/input.mjs has a watchdog that dumps the hot stacks if the
  page goes quiet.
