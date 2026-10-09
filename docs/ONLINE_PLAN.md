# Online multiplayer: plan

Status: plan only, not started. Target: public online play once the
copyrighted assets have been replaced by our own (until then, friends-only
over the tailnet).

## How Renegade's netcode works (what we must preserve)

- Client-server. One process runs the server and holds the authoritative
  world: all objects, AI, vehicles, buildings, credits, purchases, score,
  spawning, win conditions. It streams object updates and events
  (explosions, wins) to clients.
- Clients send their controls every frame and are authoritative over their own
  soldier's movement. The server only corrects them when they drift too far
  (`Combat/soldier.cpp`, CLIENT_INTERPOLATION_*).
- Hits are client-trusted by default (`cCsDamageEvent`, `AreClientsTrusted`).
  The client says "I hit X for N" and the server applies it.
- Transport is UDP with Renegade's own reliability layer (WWNet). It expects
  lost and reordered packets, not head-of-line blocking.
- Modes: LAN (broadcast discovery), Westwood Online/GameSpy (internet lobby,
  excluded under REBEL_PORTABLE), and the dedicated server (FDS).

## Architecture

```
 browser (player)           Cloudflare                     game server
 ----------------   ------------------------------   ---------------------------
 game wasm          Lobby DO   accounts, presence,    dedicated server (headless,
  WWNet transport     open games, quick-match queue    native or tab) or a host
  shim  <--WS-->    Match DO   per game: settings,     player's tab
                      roster, scores, WebRTC
                      signaling, fallback relay
        <====== WebRTC data channel (unordered, unreliable) ======>
                    (Cloudflare TURN when a direct connection fails)
```

### Durable Objects

- **Lobby DO** (one global, later one per region):
  - Holds accounts/sessions, who is online, the list of open games and a
    quick-match queue.
  - WebSockets use the hibernation API, so it is nearly free when idle.
  - Replaces the WOL/GameSpy lobby and LAN broadcast discovery in the game's
    menus.
- **Match DO** (one per game, created on "host" or by matchmaking):
  - Holds map, rules, roster and the result the server reports.
  - Exchanges WebRTC offers, answers and ICE between server and clients.
  - Last-resort relay over WebSocket when neither a direct connection nor
    TURN works.
  - Closes when the server reports the game over or goes silent.

DOs do not run the game simulation:
- Their memory limit (~128 MB) is too small for level data.
- They have no threads.
- A 30-60 Hz tick defeats hibernation.
- The simulation needs the game assets.

### Game traffic

- WebRTC data channels configured `ordered: false, maxRetransmits: 0` behave
  like UDP, so WWNet's reliability layer keeps working as designed.
- Peers connect straight to the server; Cloudflare TURN is the fallback.
- WebSocket relay through the Match DO only as a last resort: TCP
  head-of-line blocking causes lag spikes, and every packet detours through
  the DO's location.

### Authoritative server

1. **Dedicated server** (preferred for public play).
   - Renegade's FDS mode built headless: natively (macOS/Linux, using the
     POSIX platform layer) or in a browser tab.
   - Registers with the Lobby DO like any host and is always on.
   - Hosting location:
     - First: the Mac mini.
     - Later: a cheap VM per region, scaled by the lobby.
2. **Player-hosted** ("Host game" in a tab).
   - Comes almost for free once the transport exists.
   - Limited by the host's tab staying open, in focus and fast enough.

## Work items

1. **Transport shim in the game.**
   - Route WWNet's socket sends and receives (`Platform/win32/winsock.cpp`)
     to the page instead of POSIX sockets.
   - Address peers by connection id instead of IP:port.
   - Unit test: two tabs playing LAN through a local loopback relay.
2. **Page networking module.**
   - WebSocket to the lobby and WebRTC peer connections.
   - Packet queues between JS and wasm, using a SharedArrayBuffer ring
     buffer to avoid per-packet calls.
3. **Lobby and Match DOs** (Worker + DOs, TypeScript).
   - Message schema covers: hello/auth, list games, host, join, quick-match,
     signal, relay and report-result.
4. **Menus.**
   - Point the existing LAN/Internet game list and host dialogs at the lobby,
     replacing the excluded WOL/GameSpy paths.
5. **Dedicated server build.**
   - Headless (no renderer or audio).
   - Native first, if the platform layer allows; otherwise a headless tab
     under Playwright.
6. **Accounts.**
   - Invite links or tokens first; then OAuth (e.g. GitHub/Google) for
     public play.
   - Nicknames and clans later.
7. **Anti-cheat basics for public games.**
   - Set `AreClientsTrusted = false`, or validate damage claims (range,
     line of sight, fire rate, weapon).
   - Rate-limit messages.
   - Server-side checks on movement correction.
8. **Assets for public play.**
   - Until our own assets exist, each player imports their own copy (folder
     picker in the import screen).
   - The engine (GPL) can be hosted publicly; the original game files never
     are.
9. **Operations.**
   - Server health in the Lobby DO.
   - Region hints (`locationHint`) for DOs.
   - Metrics: ping, packet loss, relay fallback rate.

## Open questions

- Native dedicated server: does the POSIX platform layer build headless on
  macOS/Linux as is?
- Matchmaking rules: skill rating, or just fill games by region and ping?
- Version pinning: clients and servers must run the same game build and
  asset set. The lobby should match on a content hash.
