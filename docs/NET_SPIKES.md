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

**Yes.** Two browser clients (separate profiles) play one C&C match on the
headless server (S3), all datagrams through the hub: the server's LAN
broadcast shows its game in the original LAN Game List, both join, spawn in
their bases (one per team, as C&C requires before gameplay starts) and walk.
A browser host that also plays (non-dedicated) works the same way.

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
  - **The big one: the tick clock.** Clients connected but never got the
    world or a soldier; both sides only exchanged keepalives. Cause:
    `GetTickCount`'s boot time was a global, and engine statics read the
    clock during static initialization (`SysTimeClass`) before it was set,
    so `TIMEGETTIME` started from the raw steady clock (this Mac's uptime,
    7.5 days) and every later reading "wrapped" to ~2^32. The netcode's
    signed time comparisons then always failed
    (`Server_Update_Dynamic_Objects` skipped every player). Fixed by taking
    the boot time on first use. Found with a temporary trace in the server's
    update loop, after ruling out bandwidth (balancer, sbbo) and the hub.
    Single player only uses unsigned differences, which is why it never
    showed there.
  - Clients in a LAN game keep processing LAN broadcasts, and each one
    checks the map exists (a file lookup): 100 ms+ stalls per broadcast in
    debug builds. Matchmade games won't use LAN discovery (see S4).
- **Version lock:** clients and server must be the same build. The exe key
  (`cNetwork::Compute_Exe_Key`) is the build's git hash plus a CRC of the
  data files; a mismatch shows as "Exe version mismatch" in the game list
  and the server ignores the join. So deploys must ship the page and the
  server image together, and the matchmaker must only pair players with
  servers of their build (keep old servers until their matches end).
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
- Under game load on this Mac (two browser clients + server + workerd),
  the probes (`net.probe()`: a frame to yourself through the hub, through
  the same event loop as the game) showed medians of 1.5-5 ms but spikes of
  100 ms to 3.4 s through `wrangler dev`. The same protocol as a plain Node
  WebSocket server (`tools/server/net-hub.mjs`): median 0.7-0.9 ms, worst
  7-17 ms. So local workerd stalls under load; production DOs to measure
  (S5). The engine itself adds latency on top: it sends at NetUpdateRate
  (10/s) and reported 50-160 ms pings to the clients on localhost.

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
  broadcast going.
- Two players in a match: ~16% of one core (29.5 s CPU over 185 s,
  including the map load), 65 MB resident; ~3.2 KB/s out for two players
  (~1.6 KB/s each) right after spawning. Larger matches to measure.
- The server console reads stdin: `game_info`, `player_info` (ping,
  address, score per player), `amsg`, `kick`... as on the original FDS.
- The work directory mirrors the install with hard links (WasmFS's Node
  backend doesn't follow symlinks) plus our `server.ini` and
  `Data/svrcfg_open.ini`; the install is never written to.

## S4: direct join, the way matchmaking will do it

Question: can a client go straight from the page into a given match, with
no menus?

**Yes.** The match server joins its hub room with `?role=server` and always
gets 10.0.0.1; a client page opened with
`?net=<room URL>&args=+connect 10.0.0.1:4848 +netplayername <name>` skips
the menus (the engine's GameSpy-launch path: `cGameSpyAdmin` ->
`Connect_To_Game_Server`), loads the map and spawns. The bandwidth test that
path runs first fails harmlessly (no raw sockets) and falls back to a
default. Test: `JOIN=connect SERVER=node node lan.mjs`.
