# Online: spikes

Small experiments that answer one question each before the real thing is
built. The goals and the product shape are in `ONLINE_PLAN.md`; this log
records what each spike showed, and the architecture follows from it
(bottom of this file).

## Prior art used

- **QuakeJS** (ioquake3 under Emscripten): the dedicated server is the same
  code compiled to JS/wasm and run under Node; clients and server talk
  WebSockets. Shows the "same engine, headless, in Node" server works, and
  that a socket shim under the engine is enough
  ([quakejs](https://github.com/inolen/quakejs)).
- **Q3JS**: a gateway between browser WebSockets and UDP servers
  ([q3js.com](https://q3js.com/quake-3-webassembly)), the relay pattern.
- **Cloudflare's multiplayer Doom**: a Durable Object as the message router
  between wasm game instances
  ([blog](https://blog.cloudflare.com/doom-multiplayer-workers)). Our hub
  is the same idea with a datagram/addressing model.
- **geckos.io / node-datachannel (libdatachannel)**: WebRTC data channels
  with `maxRetransmits: 0` as "UDP for browsers" with a Node server.

## The layering (decided)

```
 engine netcode (WWNet: cConnection, packet manager, reliability, bandwidth)
        |  socket / bind / sendto / recvfrom ...   (unchanged engine code)
 vnet   |  Code/Platform/net/vnet.cpp: virtual datagram sockets, one virtual
        |  IPv4 per endpoint, ports, broadcast. Linker --wrap of libc calls.
        |  Rebel_JS_Net_Send / Rebel_JS_Net_Poll
 page   |  web/net.js: frames {dst ip:port, src ip:port, payload};
        |  network conditions simulator (latency, jitter, loss); stats
 transport (pluggable, same frames):
        |  tab:<room>   BroadcastChannel between tabs (dev, no server)
        |  ws(s)://     hub: Cloudflare Durable Object per room (services/hub)
        |  later:       WebRTC data channel (unreliable), WebTransport
```

The engine never knows which transport carries its datagrams. The hub
stamps the sender's address, so endpoints can't spoof each other.

## S1: two browser players over the virtual network

Question: does the engine's multiplayer work in the wasm build at all, over
emulated sockets?

**Yes.** Host and client pages (separate browser profiles) meet through the
hub: the host's LAN broadcast shows its game in the client's original LAN
Game List, and the client joins.

- **Built:** vnet.cpp, web/net.js (`?net=tab:<room>`), LAN discovery
  broadcast re-enabled (`cNetUtil::Broadcast` had its `sendto` commented out
  in the EA release), test `tools/browser-test/lan.mjs` (host from the LAN
  menu in one tab, join from the LAN list in another).
- **Found and fixed:**
  - `getsockopt` must be emulated too (the engine reads socket buffer sizes
    back after setting them).
  - INI wide strings (`INIClass::Put/Get_Wide_String`) stored raw 4-byte
    `wchar_t` memory as if it were UTF-16: the saved LAN game title came back
    as garbage and crashed the font code in the host-options dialog. Now
    converted to UTF-16 like the chunk helpers.
  - Two tabs of one browser profile can't both run the game (shared OPFS
    files, exclusive access handles): each player needs its own profile,
    so `tab:` transport only helps once that's fixed (WEB_PORT_GAPS.md).
  - The LAN nickname is the Windows computer name: now the page's player
    name (`?name=`, `Module.rebelPlayerName`). The engine's own LAN
    nickname-collision check fired correctly when both were "BROWSER".
  - Two debug builds on this Mac mini run at 0-3 fps; use release builds
    for multiplayer tests.
- **Engine features found that the product needs, already in the code:**
  - `+connect <ip>:<port>` and `+netplayername <name>` on the command line
    (GameSpy-era direct join: `cGameSpyAdmin::Connect_To_Game_Server`, a
    C&C client connection with no lobby). Matchmaking hands this to the
    client.
  - `STARTSERVER=<server.ini>` plus `NODX`: the Free Dedicated Server's
    auto-start (server.ini + svrcfg_*.ini, map rotation, auto restart) in
    the renderless console mode (`WW3D::Init(..., lite)`, no audio/input).

## S2: the hub on Durable Objects (local)

Question: can a Durable Object route the datagrams, and what does it cost
in latency?

- `services/hub`: Worker + `Room` Durable Object (WebSocket hibernation
  API). Assigns 10.0.x.y, routes unicast by destination address, broadcasts
  to everyone else, drops frames for unknown addresses.
- Local (`wrangler dev`): routing verified with two Node endpoints;
  round trip endpoint -> DO -> endpoint -> DO -> endpoint median 1.9 ms,
  p99 4.5 ms (no network in the way: this is the DO's own overhead).

## S3: headless dedicated server under Node

Question: can the browser build itself be the dedicated server (QuakeJS
style), so the server is the same code as the client?

**Yes.** `tools/server/run-server.mjs` loads the same release
`renegade.js/.wasm` under Node (`-sENVIRONMENT=web,worker,node`; WasmFS's
Node backend mounts a host directory where the browser has the OPFS;
`--pre-js` hands the launcher's settings to `Module`). It starts with
`STARTSERVER=server.ini NODX`, connects its virtual network to a hub room,
and runs the FDS flow unchanged:

```
Console mode active / Initializing LAN Mode / Applying server settings
Loading level C&C_Field.mix ... Level loaded OK      (2.4 s)
Gameplay Pending  Map : C&C_Field.mix  Fps : 55  GDI : 0/16  NOD : 0/16
>Player Bravo joined the game                       (a browser client)
```

- Idle server: ~11% of one core (16.5 s CPU in 150 s) with the LAN
  broadcast going. To measure: CPU and bandwidth per player in a match.
- The work directory mirrors the install with hard links (WasmFS's Node
  backend doesn't follow symlinks) plus our `server.ini` and
  `Data/svrcfg_open.ini`; the install is never written to.
