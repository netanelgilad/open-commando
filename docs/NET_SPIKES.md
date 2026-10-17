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

## Architecture so far (from S1-S4)

```
 browser (game page)                       Cloudflare
 ------------------                        ----------
 engine netcode (unchanged)
   | UDP socket calls
 vnet.cpp  (virtual datagram sockets)
   | frames {dst, src, payload}
 net.js  -- transport --------------->  Worker (nearest PoP)
           (WebSocket today;                 |
            WebRTC/WebTransport later)       v
                                         Match Durable Object  = the room's hub:
                                           addresses, routing, auth, slots
                                             |  one WebSocket (containerFetch)
                                             v
                                         Container: Node + the same renegade.wasm,
                                           STARTSERVER + NODX, server = 10.0.0.1,
                                           console on stdin (admin, game_info)
```

- **One engine everywhere.** The match server is the browser build run
  headless; protocol, data and version always match the clients (the exe
  key enforces it).
- **The Match DO is both the container's owner and the room's hub.**
  Players' WebSockets end at the DO, which forwards frames for 10.0.0.1 to
  the container over a single WebSocket and the container's frames to the
  players. The container needs no public endpoint, and the DO already knows
  who is connected (slots, backfill, kicking a player = closing a socket).
  (Spikes used the reverse, the server dialing out to the hub; both work
  with the same frames.)
- **Joining is a URL:** matchmaking returns a room; the page opens with
  `net=<room>` and `+connect 10.0.0.1:4848 +netplayername <name>`.
- **Transport is swappable per player:** the DO can accept a WebRTC data
  channel (through Cloudflare TURN) or, later, WebTransport for the same
  room; the engine and server don't change.

Next spikes:
- **S5:** done, see below.
- **S6:** the server in a Cloudflare Container: image, cold start, CPU and
  memory per match, the DO <-> container WebSocket. Needs a decision on the
  game data in a private image (ONLINE_PLAN O6) and a Docker that can build
  linux/amd64.
- **S7:** an unreliable transport: browser WebRTC data channel
  (`maxRetransmits: 0`) through Cloudflare TURN to the server (TURN over
  TLS on 443 works from containers), vs. WebSocket under packet loss
  (`?netloss=`).
- **S8:** the matching pass is done (below); next its Durable Object.

## S7a: WebRTC data channel transport (local)

Question: can the game's datagrams to the match server go over an
unreliable, unordered WebRTC data channel, negotiated through the hub,
without the engine knowing?

**Yes.** `web/net.js` keeps a table of direct peers: frames to a peer with
an open data channel (`ordered: false, maxRetransmits: 0`) go over it,
everything else through the hub. Negotiation is JSON control messages
(`{"type":"signal","to":ip,...}`) the hub forwards with the sender stamped.
The server side is the same net.js on node-datachannel's `RTCPeerConnection`
polyfill (libdatachannel). Page: `?netrtc` (or `?netrtc=<ice servers JSON>`
for TURN); server: on by default (`REBEL_ICE_SERVERS`).

- Two clients joined the Node server with `+connect`, channels opened in
  under a second, both spawned and walked (`NETRTC=1 JOIN=connect
  SERVER=node node lan.mjs`). Server 86 MB resident with libdatachannel.
- Round trip Node <-> Node over the channel on localhost: median 3.0 ms, p99
  5.9 ms (`tools/server/rtc-test.mjs`), vs ~0.8 ms through the WebSocket
  hub: SCTP/DTLS cost, negligible next to internet latency.
- Still to measure on a real network: loss behaviour (WebSocket stalls a
  ~RTT on a lost packet; the channel just drops it, which the engine's own
  reliability layer handles) and TURN-relayed paths from a container.

## S5: the hub on real Cloudflare

`services/hub` deployed (open-commando-hub.netanelgilad.workers.dev).
`tools/server/hub-probe.mjs`: two endpoints in a room, A -> DO -> B -> DO ->
A every 50 ms for 30 s, from Israel (Cloudflare PoP: TLV, 6.6 ms ping):

```
592/592 round trips: min 141.9 median 145.1 p90 148.1 p99 170.1 max 194.6 ms (lost 0)
```

- That's two DO round trips, so ~72 ms per endpoint <-> DO round trip: the
  Durable Object runs in a European location, not at the TLV PoP (DOs live
  in a limited set of locations). Very steady (p99 within 25 ms of the
  median), no spikes like `wrangler dev` had.
- For a player the path is player -> PoP -> match DO -> container, so the
  DO's location matters: put the match DO and its container in the same
  region (location hints / container regions) and pick the region by the
  players' pings. From Israel, European servers at ~70 ms is the normal
  internet distance anyway.

## S6: the match server in a Cloudflare Container (in progress)

- `services/match`: one Worker with the hub's `Room` Durable Object plus a
  `MatchServer` Container class (`@cloudflare/containers`): `POST
  /match/<id>/start` starts the server with `REBEL_NET=wss://<host>/room/<id>`
  (it dials into its own room as 10.0.0.1), `GET /match/<id>` shows its
  status page (`run-server.mjs --status-port`: ready once the map is
  loaded, recent log), `POST /match/<id>/stop`. Start/stop need the
  START_TOKEN secret.
- Image (`services/match/Dockerfile`, context from
  `tools/build-server-image.sh`): node:24-slim, the release build, the
  server scripts and the game data without movies or the single-player
  missions (697 MB of data, 1.6 GB image). PRIVATE (owner's game data):
  only in this account's registry.
- Locally (Docker, amd64 emulated on this arm64 Mac): the container dials
  the deployed hub, loads C&C_Field in ~4 s and reports ready.
- This Mac's disk is nearly full (2 GB free): local Docker builds of a
  1.6 GB image fill it (the VM's disk doesn't shrink back). Building images
  in CI (GitHub Actions) would avoid that.

## S8: the matching pass

`services/match/src/matchmaker.js`: `pass({ tickets, matches, now })`,
pure, unit-tested (`node --test services/match/test`): backfill first
(onto the weaker team, only into matches under half played), then new
matches from the oldest ticket, ping limit growing +20 ms per 10 s waited
(60 -> 150 ms), start at 8 humans or after 45 s with bots filling to 8
(never for one player alone), teams balanced by summed skill with parties
kept whole.

## S9: the lobby (warmup) and the match, locally

Goal: two players press Play online, land in a lobby where they can fight
and respawn while it fills, and wait there until the match starts.

The shipped engine only has C&C mode (Deathmatch and Team Deathmatch were
cut: commented out in `cGameData::Create_Game_Of_Type`, no source, no DM
maps), but C&C already respawns players. So the lobby is a C&C map in
**warmup**:

- Engine: `CombatManager::Set_Warmup` and the server console command
  `warmup on|off`: buildings take no damage and the game can only end by
  `gameover` (no time limit, base destruction or "gameplay pending" end).
  Players spawn on both teams, fight and respawn.
- `suicide` (development-only in the original) is a player command in our
  builds.
- `services/match/src/lobby.js` (pure, unit-tested): lobby -> countdown ->
  live. Starts when full (`startPlayers`) or once it has had `minPlayers`
  (2) for `lobbyWaitMs`; a countdown that a leaving player cancels; status
  lines in the chat (`msg`; `amsg` is a modal popup, unusable here).
- Start: `warmup off` + `gameover`: the engine's own end of game, score
  screen and map rotation move every connected client to the match map
  (`--next-map`); no page reload. After the match the rotation returns to
  the lobby map and warmup goes back on.
- `tools/server/local-service.mjs`: the online service on this machine
  (matchmaker queue, hub rooms, a `run-server.mjs` process per match, lobby
  ticks): what services/match does with Durable Objects and containers.
  Players are placed at once: into a new match, or backfilled into one still
  in its lobby (or early in play).
- Test: `ONLINE=http://127.0.0.1:8790 node lan.mjs` with
  `node tools/server/local-service.mjs --lobby-wait 60 --start-players 4`:

```
Charlie queued / Bravo queued
new match m0azvnt (lobby C&C_Field.mix, then C&C_Volcano.mix)
Bravo -> m0azvnt (joins the booting match)          backfill
Loading level C&C_Field.mix / Warmup on
Player Charlie joined the game / Player Bravo joined the game
client suicide in the lobby: 20.57 -16.26 -> respawned at 83.38 -25.68
match starts (2 players) / Warmup off / Loading level C&C_Volcano.mix
client in the match: 95.07 -50.85 -3.44   client2 in the match: -47.49 93.02 -3.48
```

- Found on the way: **wide printf `%s`**. The compat layer mapped
  `_vsnwprintf` to POSIX `vswprintf`, where `%s` means a narrow string
  (Windows: wide), so every wide `Format` with `%s` stopped after one
  character: the HUD's "Time Remaining" showed as "T: 0", chat lines as
  "H: L". `Rebel_vsnwprintf` (Platform/win32/win32_wprintf.cpp) rewrites
  Windows format spellings to POSIX ones first.

## S10: the whole loop on Cloudflare

Deployed `services/match` (open-commando-match.netanelgilad.workers.dev) with
the container (wrangler 4.149 device login; image built with buildx, 1.6 GB,
private registry; `basic` instances, max 3). The matchmaker Durable Object
runs each match's lobby itself: it polls the server's status page and sends
console commands (`POST /console`: `warmup on`, `msg`, `warmup off` +
`gameover`), and holds each player's "go to room" until the server has its
map loaded.

- On-demand server: `start` returns in ~0.6 s; the container is running and
  has C&C_Field loaded ~5.5 s after the start. (The first start right after
  the deploy failed with error 1101 while the image was still rolling out.)
- Two browser players pressing Play online (pages from this Mac, service on
  Cloudflare): matched into one room, server started on demand, both in the
  Field lobby (warmup, chat status line), a death respawned at another spawn
  point, the lobby started the match after its wait and both moved to
  Volcano and spawned there. `ONLINE=https://open-commando-match.netanelgilad.workers.dev node lan.mjs`.
- Lobby numbers for the spike: start at 4 players, or once 2 have waited
  60 s (`START_PLAYERS`, `MIN_PLAYERS`, `LOBBY_WAIT_S`; plan: 8 / 2 / 90).
- Servers stop once their match has been empty for a minute.

## S11: Play online on the public page

- open-commando.netanelgilad.workers.dev (build a32348a): Play, a nickname
  field (kept in localStorage) and **Play online**; while queued "Finding a
  match... 0:07 · N players looking" and Cancel. The page finds the match
  service through `<meta name="rebel-online">`; `?online=<url>` overrides it,
  `?offline` hides online play.
- **Version lock relaxed:** online play now matches on `REBEL_NET_VERSION`
  (cnetwork.cpp, "OPEN COMMANDO NET <n>") instead of the build's git hash,
  so a page-only deploy keeps working with running servers. Bump it with any
  change client and server must agree on (netcode, objects, physics,
  rules), and redeploy the server image together with the page then.
- Deploy order: build release -> `tools/build-server-image.sh` ->
  `services/match` `wrangler deploy --secrets-file .secrets.json` (wait for
  the container rollout: the first start right after can fail) ->
  `tools/deploy-web.sh` -> `wrangler deploy --config wrangler.jsonc`.
- Verified against the live service with two players (lobby -> match).
- Open: anyone can start servers by queueing (capped by max_instances = 3);
  no accounts, rate limits or region choice yet (one region, WEUR).

## S12: front screen, random maps, regions

- **Showroom** (`Code/Platform/web/showroom_web.cpp`): the page boots the
  engine at once with `--showroom` (intro movies skipped): a small scene of
  its own (Havoc, `S_A_HUMAN.H_A_A0A0` idle, key + rim lights, a slowly
  swaying camera) drawn instead of the main menu; the page's controls float
  over it (title top-left, name / Play / PLAY ONLINE bottom-right). Play
  leaves it for the classic main menu; Play online queues with the
  character on screen and joins at runtime (`Rebel_Online_Connect`: the
  +connect GameSpy-launch path; it marks the launch splash done, which the
  path waits for and a normal boot never shows). `?classic`: the old start.
- **Maps:** each match picks a random C&C map (`services/match/src/maps.js`,
  not the lobby map C&C_Field; the Flying variants left out for now).
- **Regions:** Cloudflare's four container regions (WEUR, ENAM, WNAM,
  EEUR). A container class per region pinned with `constraints.regions`
  (placement is per application), the region's matchmaker and the match
  rooms placed with Durable Object location hints, match ids carry the
  region. The page pings each region's echo object (`/ping`, median of 5)
  and queues in the first region (in preference order) within 30 ms of
  the closest, so players between two regions don't split. From Israel:
  WEUR ~78, EEUR ~74, ENAM ~170, WNAM ~190 ms. Servers start on demand in
  every region (first start in a region can take ~1 min: image pull).
- Deploys that change the image roll the containers out; starts fail
  (error 1101) for a minute or two meanwhile: the matchmaker retries a start
  every 15 s. Code-only deploys: `--containers-rollout=none`.
- Verified on Cloudflare with two players: showroom -> Play online ->
  region weur -> lobby (respawn) -> random map C&C_Complex, both spawned.

## S13: playing alone in the lobby, signs of life

- Warmup lifts C&C's "both teams need a player" rule
  (`cGameDataCnc::Is_Gameplay_Permitted`), so a lone player can move, shoot
  and buy at once. The flag reaches the clients in `cGameDataUpdateEvent`
  (the `warmup` command re-broadcasts it); network protocol version 2.
- Warmup tops everyone up to 1500 credits every 2 s (server,
  `cGameData::Think`), to try the purchase terminals.
- Front screen: a live line above Play online ("2 players online · 1 in a
  lobby · 1 match live"), refreshed every 10 s from `/stats` (every
  region's queue, lobbies and live matches + front-screen visitors). The
  visitors are counted by the `Presence` Durable Object: one hibernating
  WebSocket per open front screen, dropped when the game starts.
- Verified live: one player alone in the lobby: "Gameplay in progress",
  moved, died and respawned, 1512 credits; a second browser's front screen
  showed "2 players online · 1 in a lobby".
- One live join right after the image rollout connected but never joined
  (cause not found; the next attempts all worked). Known gap: the page has
  no timeout for a join that never completes.

## S14: bots

- A bot is an engine player with a negative id (`Code/Platform/web/bots_web.cpp`):
  cGod gives it a soldier and respawns it, the soldier runs the engine's
  soldier AI (sees, shoots), it shows on the scoreboard and counts for its
  team (so 1 human + bots satisfies C&C's "both teams" rule). Console:
  `bot_add [gdi|nod] [n]`, `bot_remove`, `bot_fill <players>`, `bot_list`,
  `bot_order <name|all> <attack|defend|goto x y z>` (the hook for a model
  "commander"), `bot_nav [file]` (the walking grid as an image).
- **In every game:** the matchmaker (and local-service.mjs) sends
  `bot_fill 6` (BOT_FILL) when a lobby opens: one bot in or out per second
  until the game has 6 players and even teams; humans joining push bots
  out. With MIN_PLAYERS 1 a lone visitor's lobby starts after LOBBY_WAIT_S,
  and they play a 3v3 with five bots. Verified live (weur, C&C_Canyon).
- **Navigation:** multiplayer maps shipped without AI soldiers, and their
  pathfinding data covers parts only. The server builds its own walking
  grid per map (`navgrid_web.cpp`): downward ray casts on a 1 m grid (every
  upward surface with 1.9 m headroom is a node: ground, floors, tunnels),
  links where a soldier-sized box sweeps between neighbors, and a node at
  each door's center linked to both trigger zones (doors stay solid for the
  sweeps, else paths clip the door frames). Field: 72k nodes, Islands 122k,
  Walls 167k; 3-5 s of work, spread over frames (10 ms a frame) so the
  server keeps running. A* (unreachable goals: as near as it gets),
  straight-line shortening, waypoints walked with the engine's Goto
  (`MovePathfind = false`). Stuck: jump, penalize the spot, replan; stuck
  again: wander off 6 m. Bots that reach an enemy building shoot it.
- Engine fixes: AI paths only resolved when there is a camera (a renderless
  server never planned); doors opened only for human-controlled soldiers
  (now also for bots); the attack action casts its target to a physical
  object, so it can't target a building (crash): bots aim at a point.
- Left: Nod bots are slow down the Hand of Nod's stairs; bots don't buy
  characters or vehicles; no commander yet.

## S15: a brain for the bots, step 1 (hooks, blackboard, scripted)

The plan for "AI that feels human": each bot gets a brain with three layers.
A language-model agent loop (slow: seconds, on events) owns the bot's
character: candidate plans, standing orders, memory, what it says. A
decision model (Clef-flash, ~40 ms, typed questions answered with
probabilities) decides the moment 2-4 times a second: which plan, how to
move, what to shoot, what to call out, whether to rethink (waking the loop).
The engine does the rest every frame. The two models share a blackboard per
bot; bots coordinate only through real game chat, like players.

Step 1 (this), no models yet:
- Engine (`bots_web.cpp`, brain mode): `bot_brain on` stops the bots' own
  goal loop; the server streams the world 4 times a second (`Module.
  rebelBotState`: buildings with health, players with health/credits/
  kills/deaths/character/vehicle, vehicles, and per bot what it can see,
  by line of sight) and every chat line (`Module.rebelBotChat`, hooked in
  cScTextObj::Init). The brain acts through `bot_do <id> go x y z | stop |
  target <obj> | shoot x y z | hold_fire | jump | say team|all <text>`;
  walking uses the walking grid, shooting while walking is the engine's
  attack action with a move.
- Found on the way: every team chat line marked the message for each team
  member's network slot by player id, so bots (negative ids) wrote outside
  the per-client table. NetworkObjectClass now ignores client ids outside
  it.
- `tools/server/bot-brain.mjs` (`run-server.mjs --bot-brain scripted
  [--bot-log file]`, or REBEL_BOT_BRAIN/REBEL_BOT_LOG): named places from
  the buildings (each building, each base, the middle), a per-bot view
  (me, enemies in sight with kind/health/distance/place, teammates, base
  status, alerts, last chat lines, life events, blackboard), the moment's
  questions in Clef's format (plan / move / target / callout / rethink,
  options built from what is possible now), a scripted planner and decider
  in the models' seats, answers turned into bot_do (only what changed),
  callouts as written lines, and a JSON-lines log of every decision.
- Verified: 3v3 bots under the scripted brain fight and score (about 700
  decisions a minute); their team chat shows on a human's screen.

## S16: the brain, steps 2-3 (Clef-flash decides, DeepSeek V4 Flash plans and talks)

- Models run through the match service: `POST /ai/<match>` with the
  Worker's Workers AI binding (`services/match/src/ai.js`: clef-flash,
  clef, deepseek-v4-flash). A match server gets a key for its own match
  only (HMAC of the match id with START_TOKEN) as REBEL_AI_KEY, with
  REBEL_AI_URL, when the matchmaker starts it with BOT_BRAIN set; it holds
  no Cloudflare credentials.
- `--bot-brain clef`: Clef-flash answers each bot's questions (plan, move,
  target, callout, rethink) 2-3 times a second in a fight or with the base
  under attack, about once a second otherwise; bots decide in parallel; a
  call over 1.5 s or failed gets the scripted answers. Clef rejects a
  choice with one option: such questions are left out. Measured from a
  home machine through the Worker: p50 ~330-410 ms, p90 ~450-510 ms (the
  model alone is ~40 ms), ~1,000 input tokens a call.
- `--bot-brain hybrid` adds the planner: DeepSeek V4 Flash (thinking off:
  `chat_template_kwargs: {thinking: false}`; with it on, replies take
  seconds and fill max_tokens with reasoning) writes each bot's 2-4 plans
  (kind attack/defend/go/follow, checked against the map's buildings,
  places and teammates), standing orders, a note, and maybe a line of team
  chat, as JSON. Each bot has a persona. Called on spawn, death, being
  named in chat (any human team line wakes one bot), Clef's "rethink",
  and every 22-28 s; p50 3-4.5 s. Workers AI returned "Capacity
  temporarily exceeded" for a while: failures fall back to scripted plans
  and retry in 5 s.
- Lessons from watching it: whether to shoot is not a question (Clef chose
  "none" with enemies in sight 64% of the time: scoring stopped); it is a
  reflex now, and the question is which target. Base defense guns (the
  AGT's, the Obelisk's) are dropped from what bots "see". The language
  model invents map features (tunnels, ridges) unless told only the
  listed places exist, and talks far too much unless told players mostly
  don't; bots' lines are rate-limited per team. "C&C is won by destroying
  buildings" in the prompt moved plans from holding to attacking.
- 3v3, 3 minutes, hybrid: 9-8 (the highest of the brain runs; Clef alone
  before the reflex fix: 0-0), ~1,000 Clef calls and ~35-45 planner calls;
  about $1 a 30-minute match at list prices. Sample chat: "Nash, I'm
  covering your flank on the WF. Keep shooting." / "On your six, Nash.
  Keep burning." / "I'm hurt, pulling back a bit".
- Live (weur, hybrid on): Clef p50 ~210-250 ms from the container (p90
  ~250-500); the planner falls back DeepSeek V4 Flash -> gpt-oss-20b ->
  Llama 4 Scout -> scripted (a failed model is skipped a minute; gpt-oss
  needs room for its reasoning: max_tokens 1200, no JSON mode). Per-match
  model budget REBEL_AI_BUDGET_USD (default $2); about $0.04 for 2.5
  minutes with 1 human + 5 bots. Server ~420 MB of the basic instance's
  1 GiB (0.25 vCPU).
- No bots-only games: bots join only while a human is in the game and
  leave with the last one; no model calls without a human; lobbies no
  human reached stop after 90 s, matches 30 s after the last human left.
- Gotchas: the Dockerfile copies files by name (a new script left out
  crashed every server: it now copies tools/server/*.mjs); a deploy's
  container rollout replaces running servers for minutes after `wrangler
  deploy` returns, so live tests right after a deploy fail for reasons
  that aren't bugs. The matchmaker now prints a dead server's last log
  lines, and servers log memory every 10 s.

## S17: no lobby

With bots there is no reason to wait: a match server starts straight on a
real C&C map (a random rotation per server, looping; buildings can be
destroyed), bots fill the game to BOT_FILL as soon as a human is in, and
each human who joins takes a bot's place (bot_fill removes a bot from the
bigger team). Players go into any running match with room for another
human (8), or a new one; servers stop 30 s after their last human leaves
(90 s if no one ever arrived). Teams are no longer remixed between maps
(RemixTeams=no: the remix was why bots once shot their own team). The
lobby (lobby.js, warmup, START_PLAYERS/MIN_PLAYERS/LOBBY_WAIT_S) is gone;
`warmup` stays as a console command. Verified locally: one player straight
into C&C_City with five bots; two players: four bots, 3 a side.

## S18: bots buy, board and drive; 32 players; match history

- Games take 32 players (16 a side) as Renegade servers commonly did; bots
  top games up to 8 (BOT_FILL), humans take their places.
- Bots buy: the state carries each team's shop (the purchase terminal's
  characters and vehicles, prices, base power: double without it); `bot_do
  <id> buy character|vehicle <n>` buys through VendorClass::Purchase_Item
  (the game's own checks). The brain: the planner sees credits and the shop
  and can plan "buy <item>" (back to base, buy it); at the base Clef gets a
  "what do you buy" question over what is affordable now. A bought vehicle
  comes out of the factory with the team's colors and no driver: the bot
  walks to it and `bot_do <id> enter <vehicle>` puts it in
  (VehicleGameObj::Add_Occupant, as the vehicle's door transition does: a
  bot can't aim for the door zone, and the use key from 4-6 m off a
  Mammoth's center doesn't board). Driving: orders go to the vehicle's own
  action (the engine's vehicle AI steers), waypoints count as reached at
  4 m; a vehicle stuck for a while is left on foot. Verified: rocket
  soldiers, engineers, a buggy and a Mammoth Tank driven across Field.
  Rough: vehicles follow the soldiers' walking grid, so they snag.
- Match history: the matchmaker samples each match every 10 s (map,
  humans, bots, fps, timer lateness, memory, AI spend), keeps the last 50
  matches per region with how they ended and the server's last log lines:
  `GET /history`, `GET /history/<match>`.

## S19: game records, lower ping, reconnecting sockets, smarter driving

- **Records** (tools/server/recorder.mjs): every match server records its
  games as JSON lines and sends a gzipped batch every 30 s to `POST
  /record/<match>` (match key), stored in R2 (`open-commando-games`,
  `matches/<id>/<seq>.jsonl.gz`, plus `summary.json` from the matchmaker);
  `GET /record/<match>` (START_TOKEN) returns it all. Types: decision (the
  model's state, the questions with their options, the probabilities),
  command, plans, planner_call (prompt, reply, model, latency, tokens),
  planner_error, chat, kill, death, character, vehicle_new/lost/enter/exit,
  building_damaged/destroyed, snapshot (every 5 s: buildings, players with
  positions/credits/scores, vehicles, bots' path state), perf (every 10 s:
  fps, timer lateness, CPU %, memory, AI spend, traffic), log (server
  console), server/stop. A 3-minute live game: 3.5 MB (about 0.4 MB gzipped).
- **Ping**: the engine's packet manager held packets to send at most 10
  times a second (FlushFrequency 100 ms, a 2002 bandwidth saving): ~50 ms
  each way on average. Now every frame (REBEL_PORTABLE; console
  `send_frequency <ms>`), and servers send object updates 20 times a
  second. Local ping (1 ms relay) 65 -> 32 ms; live from here 201-320 ->
  141-159 ms with an 80-95 ms relay round trip. Region choice: on a tie the
  player's fastest region (it was REGIONS order).
- **Reconnects**: the room's hello carries a token; a socket that drops
  reconnects at once (for up to 30 s) and takes its address back
  (`?claim=<ip>&token=`), so the game sees a short loss. The room relay
  also completes the WebSocket close handshake (hibernating sockets don't
  by themselves: clients never noticed a clean close). Verified on the live
  relay: back in 553 ms, same address, packets flowing.
- **Deploys**: tools/deploy-match.sh waits for every region's container
  rollout; servers started during a rollout get SIGTERM'd minutes later
  (two "broken" live tests were just that).
- **Driving**: a second walking grid for vehicles (2 m cells, a 3 m-wide
  box, 3 m headroom, no door routes); bots replan when getting in or out.
  The state carries each bot's "feel" (clearance ahead, ahead-left/right,
  sides, behind; speed); when a bot is stuck or crawling into something,
  Clef gets a maneuver question (go around left/right, back up left/right,
  another route, wait, get out) with only the open options, carried out by
  `bot_do <id> maneuver ...` (a short detour, in reverse for vehicles, then
  a new path). Test: drivers covered 150-270 m at 7-9 m/s (before: ~40 m
  and stuck).
