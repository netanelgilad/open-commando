# Online multiplayer: plan (v2, 2026-10-09)

Status: research and plan, nothing built. Replaces the first draft
(player-hosted servers, WebRTC peer-to-peer). Goal: modern "press Play"
online. There's no server browser: the player clicks Play (optionally picks
a mode) and is put into the best match, the way Fortnite or Call of Duty
do it. Hosting is on Cloudflare.

## How Renegade's netcode works (unchanged by this plan)

- **Client-server.** The server runs the authoritative world: AI, vehicles,
  buildings, credits, purchases, score, win conditions.
- **Clients send their controls every frame.** They are authoritative over
  their own soldier's movement (the server corrects large errors) and,
  by default, report their own hits (`cCsDamageEvent`, client-trusted).
- **Transport:** UDP via WWNet (`Code/WWNet`: `sendto`/`recvfrom` on
  datagram sockets), with Renegade's own reliability layer on top. It
  expects packet loss, not delays.
- **Dedicated server mode exists in the source** (the Free Dedicated Server,
  `ConsoleMode`, `AutoStart`, `ServerSettings`). The original FDS ships with
  the Steam install as a Windows binary (`RenegadeFDS/`).
- **Update rate:** about 10 Hz by default (`NetUpdateRate`). That is gentle
  on bandwidth, roughly 5-20 KB/s per player.

## What Cloudflare offers (researched 2026-10-09)

- **Containers** ([docs](https://developers.cloudflare.com/containers/),
  [limits](https://github.com/cloudflare/cloudflare-docs/blob/production/src/content/docs/containers/platform-details/limits.mdx),
  [architecture](https://developers.cloudflare.com/containers/platform-details/architecture/),
  [pricing](https://developers.cloudflare.com/containers/pricing/)):
  - **Shape:** Linux containers, each owned by a Durable Object. Instance
    types range from 1/16 vCPU / 256 MiB up to 4 vCPU / 12 GiB. Cold start
    is about 1-3 s. They sleep after an inactivity timeout (10 min by
    default) and are billed per 10 ms while running.
  - **No inbound TCP or UDP.** All traffic comes in through the owning
    Durable Object as HTTP or WebSocket.
  - **No outbound UDP.** Egress is ports 80/443 and DNS only
    ([egress](https://github.com/cloudflare/containers/blob/main/docs/egress.md)).
  - **Placement:** region constraints ENAM, WNAM, EEUR, WEUR (Americas east
    and west, Europe east and west). Nothing in Asia/Oceania listed. The
    container is not guaranteed to be co-located with its Durable Object.
  - **Cost** for a 1 vCPU / 4 GiB match server at full CPU: about $0.07/h
    of vCPU plus $0.04/h of memory, plus egress ($0.025/GB in NA/EU; 32
    players at ~10 KB/s is ~1.2 GB/h, about $0.03/h).
    **About $0.10-0.15 per match-hour.**
- **Durable Objects:**
  - fit lobbies, parties and matchmaking queues;
  - WebSockets with hibernation, single-threaded consistency;
  - location hints.
  - Too small to run the game server itself (memory, no threads).
- **Realtime SFU DataChannels**
  ([docs](https://developers.cloudflare.com/realtime/sfu/datachannels/),
  [reliability changelog](https://developers.cloudflare.com/changelog/post/2026-08-13-datachannels-reliability-ordering/)):
  - WebRTC data channels through Cloudflare's network (330+ cities);
  - support unreliable, unordered delivery end to end (`maxRetransmits: 0`),
    which is UDP-like from browsers;
  - publish/subscribe per named channel; native (non-browser) endpoints
    can connect.
  - The catch: the game server must be a WebRTC endpoint, which needs UDP
    (or ICE over TCP/TLS 443, unverified). Containers can't do UDP.
- **WebTransport:**
  - Baseline since March 2026 (Chrome/Edge 97+, Firefox 114+, Safari 26.4)
    ([webrtc.ventures](https://webrtc.ventures/2026/04/webtransport-is-now-baseline-what-it-means-for-real-time-media/),
    [WebKit 26.4](https://webkit.org/blog/17862/webkit-features-for-safari-26-4/));
  - unreliable datagrams over QUIC, i.e. real UDP from the browser;
  - `serverCertificateHashes` allows short-lived self-signed certificates,
    a good fit for ephemeral game servers.
  - Not available in Workers yet
    ([workerd #6451](https://github.com/cloudflare/workerd/issues/6451)).
  - Needs a server that receives UDP, so not in Containers.

## Transport: the options

The browser speaks to the game server; something must carry Renegade's
datagrams.

| # | Path | UDP-like? | All on Cloudflare? | Verdict |
|---|---|---|---|---|
| T1 | Browser -WebSocket-> nearest Cloudflare PoP -> match Durable Object -> container (`container.fetch` WebSocket upgrade) -> game server | No (TCP: a lost packet delays the ones after it, ~1 RTT) | Yes | **Ship first.** Works today with everything we have. Renegade tolerates latency well: own movement is client-side, the server sends 10 Hz updates. Many browser shooters run on WebSockets |
| T2 | Browser -WebRTC DataChannel (unreliable)-> Realtime SFU -> game server as a WebRTC endpoint | Yes | Only if the server can reach the SFU without UDP (ICE-TCP/TURN-TLS on 443: to be tested) | **Prototype.** If containers can't reach it, T2 needs a UDP-capable server host |
| T3 | Browser -WebTransport datagrams-> game server directly (QUIC/UDP, self-signed certificate per match) | Yes, the best: real UDP, no relay hop | No: game servers on a UDP-capable host (e.g. Fly.io machines, bare metal); Cloudflare keeps lobbies and matchmaking | **Upgrade path** if T1's latency spikes matter, and for Asia/Oceania (no container regions there) |

**Recommendation:**
- Hide the transport behind one small interface in the engine (below).
- Launch on T1, fully on Cloudflare.
- Measure packet-loss stalls and ping per region in real matches.
- Add T3, or T2 if the SFU path works from containers, where measurements
  show it's needed. The matchmaker treats a "server" as an address plus a
  transport, so mixing is easy.

### Engine changes (shared by all transports)

- **Client (browser):**
  - WWNet's datagram socket calls (`netutil.cpp`, `winsock.cpp` platform
    layer) go to the page instead of POSIX sockets;
  - one virtual peer, the match server, carried over the chosen transport
    as framed binary messages (one Renegade datagram = one message);
  - a SharedArrayBuffer ring buffer between wasm and the page's network
    code, so there's no per-packet call.
- **Server:** the dedicated server keeps **real UDP on localhost, unchanged**.
  - A small relay in the same container (Node or Go) terminates each
    client's WebSocket and gives it its own local UDP socket, so the
    server sees one address per player.
  - The engine's server networking stays exactly as written.
  - The same relay pattern serves WebTransport (T3) later.
- **Dedicated server build** (the engine is 32-bit code; avoid a 64-bit
  port):
  - (a) the wasm32 build running headless under Node in the container:
    no renderer or audio, files on the container disk. It keeps exactly
    the browser's code and memory model. **Recommended first.**
  - (b) a native 32-bit Linux build (`-m32`) of the POSIX platform layer:
    faster, and more porting work.
  - Either way it needs the dedicated server's game data
    (decision O6).

## Matchmaking design

Researched practice:
- **Activision** puts connection (ping) first, time-to-match second, and
  uses skill for grouping and team balance; their A/B test removing
  skill-based matching hurt retention
  ([Activision research](https://research.activision.com/publications/2024/04/callofduty-matchmaking-intel-ping),
  [skill paper](https://www.activision.com/cdn/research/CallofDuty_Matchmaking_Series_2.pdf)).
- **Search windows** widen over time ([Edgegap](https://docs.edgegap.com/learn/matchmaking/matchmaker-in-depth)).
- **Open Match** handles backfill ([backfill](https://open-match.dev/site/docs/guides/backfill/)).
- **OpenSkill** (Plackett-Luce) is the rating model for team games
  ([overview](https://crux.supercraft.host/blog/skill-based-matchmaking-architecture-2026/)).

### Flow (what the player sees)

1. Open the site, click **Play** (and a mode later, e.g. C&C, Deathmatch).
2. The page measures ping to each server region (a WebSocket echo through
   the region's endpoint, 5 samples, median).
3. Matchmaking: "Finding a match… 0:07". Usually 5-30 s; joining a match
   in progress (backfill) is often instant.
4. A match is found: the server is already running (warm pool) or starts
   in 1-3 s, and the map loads in ~3 s.
5. At match end: the score screen, then **Play again** (keeps the party) or
   back to the menu.

### Services (all Cloudflare)

- **Gateway Worker:** sessions/accounts, `POST /play`, region ping
  endpoints, WebSocket routing.
- **Matchmaker Durable Object**, one per region and mode
  (`mm:EEUR:cnc`):
  - holds tickets: player or party, OpenSkill mu/sigma, pings per region,
    party size, enqueue time;
  - runs a match pass every ~1 s.
  - Sharding by region keeps each object small and avoids one global
    hotspot; tickets go to the 1-2 regions with the best ping.
- **Match Durable Object**, one per match, owning its container:
  - registry: slots, teams, start time, score;
  - proxies players' WebSockets into the container;
  - reports open slots back to the region's matchmaker (for backfill);
  - stops the container when the match ends.
- **Party Durable Object:** friends queue together (invite link).
- **Warm pool per region:** keep N started-but-idle match servers (sleeping
  containers cost nothing; a few awake ones cost cents per hour) so a
  match can start instantly. Sized from queue rate.

### The matching pass (per region and mode, every ~1 s)

1. **Backfill first.** Fill in-progress matches that have open slots and
   are early enough in the match (e.g. < 50% played, not about to end).
   - Put each ticket on the weaker team (by summed OpenSkill mu).
   - Parties stay together.
   - Prefer the match with the lowest ping for the ticket.
2. **New matches.** Sort waiting tickets by enqueue time (oldest first).
   For each, gather compatible tickets:
   - **Ping:** all players under the region's current ping limit. The limit
     starts at 60 ms and grows +20 ms per 10 s waited, capped at 150 ms.
   - **Skill:** within mu ± window. The window starts narrow and widens over
     time, and is ignored entirely while the player population is small
     (below).
   - **Size:** start when the minimum player count is reached (e.g. 8 for
     C&C, configurable), or after a time limit with bots filling the rest.
3. **Team balance.**
   - Split players into two teams minimizing the difference in summed mu.
   - Parties stay together; a greedy assignment plus a few swap
     improvements is enough at this size.
4. **Allocate a server** from the warm pool in that region (or start one),
   send each player its connection ticket (match id, region, signed token).

**Priorities** follow Activision: connection, then time to match, then
skill. With few players, waiting is the biggest risk, so at low population
the matcher:
- drops skill from matching and keeps it only for team balance;
- leans on backfill (Renegade C&C matches are long);
- fills empty slots with the engine's own AI bots. Multiplayer Practice
  already runs C&C matches against bots, so this is real code. Bots leave
  as humans join.

### Ratings

- **Model:** OpenSkill (Plackett-Luce) per mode; mu/sigma stored per
  account (D1 or the account's Durable Object).
- **Updates:** from match results the server reports (win/loss, team
  score; individual score as a tie-breaker).
- **New players** start with a high sigma, so a few matches place them.
- **Never shown** as a number at first: quietly used for balance.

## Accounts, identity, anti-abuse

- **v1 identity:** a guest account stored in the browser (an anonymous
  signed token) with a chosen nickname. "Sign in with GitHub/Google" is
  optional and moves ratings and stats across devices.
- **Tokens:** match tickets are signed by the Gateway, and the Match
  Durable Object checks them on connect.
- **Rate limits** on queue joins and chat.
- **Cheating:** the engine trusts clients for hits by default. For public
  matches, set `AreClientsTrusted = false` (server-side hit checks) or add
  plausibility checks (range, line of sight, fire rate).

## Game data on the servers (legal note)

- The server needs the game's level and definition data. Two routes:
  - our own image built from the owner's install (privately, in the
    container registry);
  - the original Free Dedicated Server's data, which was distributed for
    anyone to run servers.
- Either way, check the terms (EA's FDS distribution/EULA).
- Long term, our own assets remove the question
  (`OPEN_SOURCE_PLAN.md`, `REMASTER_PLAN.md`).

## Phases

1. **Local multiplayer over our transport:**
   - wasm32 dedicated server under Node;
   - the WebSocket<->UDP relay;
   - the client socket shim;
   - two browser tabs on this machine in one C&C match.
2. **One region on Cloudflare:** a container image (server + relay + data),
   the Match Durable Object proxy, and a manual "join match X" page.
   Measure ping, loss stalls and CPU per player.
3. **Matchmaking v1:**
   - Gateway, region pings, matchmaker Durable Objects (backfill, ping
     windows, team balance);
   - warm pool, **Play** button, Play again, parties;
   - bots fill low population.
4. **Production:**
   - accounts, OpenSkill, server-side hit checks, chat and reporting;
   - all four regions, metrics dashboard (queue time, ping, quit rate);
   - load test with headless clients (the existing Playwright tooling).
5. **Transport upgrade** (where measurements call for it):
   - T3 WebTransport datagrams on UDP-capable hosts (and for regions
     Cloudflare doesn't cover);
   - or T2 via the SFU if containers can reach it.
6. **More modes:** Deathmatch, Team DM, CTF (the engine has the settings
   types), custom matches with an invite link.

## Decisions to make

| # | Decision | Options | Recommendation |
|---|---|---|---|
| O1 | Modes at launch | C&C only; C&C + Deathmatch; several | C&C only (one queue keeps waits short) |
| O2 | Match size | 8-16 (fills fast); up to 32 (classic Renegade) | 8 minimum to start, 16 cap at launch; raise with population |
| O3 | Bots | Fill to minimum / fill to full / none | Fill to the minimum at low population, replaced by humans |
| O4 | Transport at launch | T1 WebSocket (all Cloudflare) / T3 hybrid now | T1, measure, then decide |
| O5 | Identity | Guest only / guest + OAuth / accounts required | Guest + optional OAuth |
| O6 | Server game data | Image from the owner's install / FDS data / wait for own assets | Check the FDS terms; owner's install for private testing only |
| O7 | Regions | NA + EU only (Cloudflare's four) / plus a UDP host for Asia | Four Cloudflare regions first |
| O8 | Skill matching | Off at launch (balance only) / on | Balance only until queues are healthy |
| O9 | Hit trust | Client-trusted (original) / server-checked | Server-checked for public queues |
