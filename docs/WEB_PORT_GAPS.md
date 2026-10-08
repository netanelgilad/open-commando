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
- **Stream loop blocks** (`AIL_set_stream_loop_block`) are ignored; streams
  loop the whole file.
- **Stereo samples can't be panned** (OpenAL limitation); only mono pans.

## Graphics / text

- **Italic fonts aren't synthesized.** GDI text runs on stb_truetype
  (`Code/Platform/gfx/gdi_text.cpp`); bold is emulated by 1px widening,
  italic requests render upright. The shipped fonts (Arial MT, Regatta
  Condensed LET) have no italic files.
- **Gamma ramp** (`SetDeviceGammaRamp` / `IDirect3DDevice9::SetGammaRamp`)
  must be applied in the present pass; until then the brightness slider
  does nothing.
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
- **Cutscenes (Bink)**: not yet handled; plan is offline transcode to WebM.
