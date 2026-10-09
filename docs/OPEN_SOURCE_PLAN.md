# Open-sourcing the browser port: plan

Status: plan and audit (2026-10-09). Nothing is published yet. Not legal
advice: where the stakes are real, a lawyer should read the EA terms below.

## Licensing position

- EA released the Renegade source under **GPL v3 with additional terms**
  (`LICENSE.md`). The fork we build on (winterheart/CnC_Rebel) keeps that
  license.
- Everything we wrote is a modification of that program (platform layer,
  renderer, page, tools), so it is GPL v3 under the same additional terms.
  Publishing it is allowed and in fact what the license expects, provided
  the conditions below are met.
- **EA's additional terms (GPL section 7):**
  - No EA trademark in a distributed modification: "Command & Conquer",
    "C&C", "Renegade" and logos. No claimed affiliation with EA.
  - Modified versions must be marked as modified and not presented as the
    original.
  - The copyright notice and these terms must travel with every copy.
  - Warranty disclaimer and indemnity clause (relevant only if we ever offer
    paid support or contracts).
- **The code license covers code only.** The game data (models, textures,
  maps, sounds, music, movies, fonts, strings) remains EA's copyright and is
  not licensed by the GPL release. It must never be in the repository or on
  a public server. Players use their own copy: the same model as OpenRA,
  OpenMW and the original EA source release.
- **Third-party code in the repository:** crunch (public domain), minimp3
  (CC0), stb (MIT / public domain). All GPL-compatible. The proprietary Miles
  and Bink SDKs are not included; the port replaces them (OpenAL backend,
  WebM video).
- **Generated output:** the Emscripten runtime (MIT/NCSA) is compatible.
  `renegade.wasm`/`renegade.js` are GPL object code; anyone hosting them must
  offer the corresponding source (GPL section 6). A link from the page to the
  exact commit does that.

## Audit of the repository (2026-10-09)

| Check | Result |
|---|---|
| Game data in tracked files or our commits (.mix/.dat/.w3d/.tga/.dds/.wav/.mp3/.bik/.webm/fonts) | None from us. The 251 binary files tracked are icons and editor resources from EA's own source release |
| Game data next to the repo | `game-data/`, `web-movies/` and the Steam download tools live outside `engine/` (the workspace root), so they are never in the git tree |
| Test artifacts | `tools/browser-test/profile/` (a full OPFS copy of the game data!), `out/` (screenshots, profiles) and `node_modules/` are gitignored |
| Personal info in tracked files | No home paths, hostnames or credentials (`git grep`) |
| Personal info in history | A local `build` symlink (pointing at a path on the developer's disk) and the commit identity; both fixed by a history rewrite before publication |
| Credentials | Game-store credentials were only used at runtime by local tooling outside the repository; nothing in the repo or history |

## Separating the work from the assets

The code is already separate. What changes for a public build is how players
get their game data into the browser:

1. **Local import (needed for public hosting).** Today the page copies files
   from `/gamedata/` on our own server into OPFS. For a public site:
   - "Choose your Renegade folder" with the File System Access API
     (`showDirectoryPicker`, Chrome/Edge) or `<input webkitdirectory>`
     (Firefox/Safari), copied into OPFS by the existing import worker;
   - a manifest of expected file **names and sizes only** (no content) to
     validate the folder and give clear errors ("this looks like the demo",
     "missing Always.dat");
   - optionally a hash check of known retail versions (Steam, Origin, disc
     1.037);
   - `tools/serve.mjs` stays as the self-hosting option ("serve your own
     install on your own machine or private network"), documented as such.
2. **Cutscenes.** Today they are Bink files transcoded offline to WebM. Options:
   - (a) transcode in the browser at import time with ffmpeg.wasm (LGPL;
     FFmpeg has a Bink decoder) — slow once, then cached;
   - (b) ship the transcode script and let players run it;
   - (c) a small wasm Bink decoder built from FFmpeg's decoder, playing the
     .bik directly;
   - (d) play without cutscenes.
   - Recommendation: (a) first, (c) later.
3. **Settings, saves, configs.** These are already per-browser in OPFS
   (`/opfs/persist`, `Data/save`), so nothing is shared.

## Name and branding

- The project name, page title, domain, repository name and icons must not
  use EA trademarks.
  - Today the page says "Command & Conquer: Renegade" (`web/index.html`
    title and heading); that must change.
- A neutral name, plus a factual compatibility line in the README:
  - name: e.g. "Rebel Web", or something new;
  - README line: "A browser port of the engine EA released as open source.
    Requires your own copy of the game."
  - This is the safest reading of "may not distribute any modification
    using any Electronic Arts trademark".
- The upstream fork's name (CnC_Rebel) uses "CnC". Our own published name
  should not.
- **Required notices:** keep `LICENSE.md` (GPL + EA terms) verbatim. Add a
  NOTICE/README section that:
  - says it is a modified version of EA's release, with what changed and
    when (GPL 5a);
  - says it is not affiliated with or endorsed by EA;
  - says game assets are not included and need a legally owned copy.
- The game's own splash screens, logos and text come from the player's data
  at run time. That is the player's copy, not something we distribute.

## Repository and history

- **Where:** a fork of winterheart/CnC_Rebel on GitHub (keeps upstream
  attribution and the upstream fixes), or a new repository with a neutral
  name that credits both upstreams.
- **History cleanup before the first push:**
  - remove the `build` symlink from all history (`git filter-repo --path
    build --invert-paths`) or publish our work as fresh commits;
  - choose the author identity to publish under (rewrite with a mailmap if
    not "a work git identity");
  - re-scan with `git grep` and the checks above on the rewritten history.
- **Upstreaming:** the portable platform layer (REBEL_PORTABLE) and the
  fixes found here are useful to the upstream fork as well (dialog CONTROL
  classes, Flush_Dialogs, non-POD varargs). Offering PRs is good citizenship
  and spreads maintenance.

## Hosting a public build

- **Isolation headers:** the page needs COOP/COEP (cross-origin isolation for
  SharedArrayBuffer/pthreads).
  - GitHub Pages can't set headers; the coi-serviceworker workaround exists
    but is fragile.
  - Cloudflare Pages (or Workers static assets) sets them with a `_headers`
    file, and fits the Durable Objects plan for online play.
- **Source offer:** the page footer links to the source repository and the
  exact commit (the build already embeds the git hash), plus the license.
- **Only code goes up:** wasm, js, html. No game data or movies on the
  server.

## Checklist before going public

1. Implement local folder import and its validation (manifest of names and
   sizes only).
2. Decide the cutscene approach (a/b/c/d above).
3. Pick the project name; rebrand the page; add NOTICE, disclaimer and the
   source link.
4. Rewrite history (build symlink, author identity) and re-audit.
5. README:
   - what it is;
   - "bring your own copy" instructions (Steam/Origin/disc);
   - browser requirements, the build instructions (emsdk, `/tmp/br.sh`
     equivalents as real scripts in the repo), and the test tooling.
6. Move developer scripts that live in `/tmp` or the workspace into the repo
   (build scripts). Credentials and Steam download scripts stay outside.
7. Host with COOP/COEP; verify that a fresh browser with only a local
   install works end to end (the existing browser tests, pointed at the
   public URL).

## Remastered assets and the license

- New code (renderer, pipeline) is GPL like the rest.
- Remastered assets that are faithful recreations of EA's designs (same
  characters, vehicles, maps) are likely derivative works of EA's art. The
  GPL release does not cover them, so they can't be hosted publicly as
  "ours".
- Truly original assets (our own designs) can be published under any license
  we choose (e.g. CC BY-SA).
- This is decision 14 in `REMASTER_PLAN.md`.

## Commercial route (if we ever sell our own version)

Question answered on 2026-10-09: with assets that are entirely our own, can
we sell the game? Yes, within these limits. Not legal advice: have a lawyer
review before a commercial launch.

**Allowed**
- Selling it (Steam, own store, paid web access). The GPL explicitly allows
  charging for copies.
- Keeping our own assets proprietary. Models, textures, maps, audio, music,
  story and UI art are separate works from the engine and can be under any
  license, including "all rights reserved". This is the Doom/Quake model:
  GPL engine, sold content.
- Keeping server-side services closed (lobby, matchmaking, accounts,
  Durable Objects). GPLv3 (unlike AGPL) only applies when the program is
  distributed. Services we only run don't have to be published.

**Required / limiting**
- **The engine stays GPLv3 + EA's terms forever.** It is EA's code plus our
  modifications, so we can't relicense it as proprietary.
  - Every buyer must get, or be offered, the complete corresponding source of
    the engine they received, including our changes and build scripts. A
    link to the exact public commit is enough.
  - Buyers may share or resell that engine. The moat is the content, not the
    code.
- **Distributed servers are GPL too.** A dedicated server binary we hand to
  players or hosts needs its source available. Servers we only run don't.
- **EA's additional terms apply even with zero EA content:**
  - no EA trademarks in the product name, store page, marketing or domain;
  - no claim of affiliation or endorsement;
  - mark it as a modified version of EA's released program and keep EA's
    copyright notice and terms in the source;
  - indemnity: if we give buyers warranties or other contractual promises,
    we must cover EA for liability those create. Ship an "as-is" EULA for
    the engine part and keep warranties minimal.
- **"Nothing from EA" includes IP beyond files:** GDI, Nod, Tiberium,
  Havoc and other characters, the story, unit/vehicle/structure designs, and
  recognizable map layouts. The code also contains hard-coded EA-flavored
  names and strings (team names, UI text, version strings). A commercial
  product renames them and doesn't depend on them.
- **Platforms:**
  - Steam and the web are fine with GPL engines.
  - Apple's App Store has historically conflicted with the GPL (extra usage
    restrictions); avoid depending on it, or get advice first.
  - Console stores (closed SDKs, NDAs) are generally incompatible with
    shipping GPL code.
- **The license can't be revoked:** EA's GPL release stays valid for us as
  long as we follow its terms.

**What a commercial release needs (on top of the public checklist above)**
1. A full original asset set (see `REMASTER_PLAN.md`, decision 14),
   audited for EA-derived designs and names.
2. EA-flavored hard-coded strings and names removed from the engine.
3. A product name and branding cleared for trademarks: no EA marks, and a
   search for conflicts with others.
4. Licensing split documented:
   - engine GPLv3 + EA terms, with the source link in the game and store
     page;
   - content under our own license;
   - EULA covering content and services, with the engine "as is".
5. A lawyer's review of the EA section 7 terms (indemnity) and the store
   agreements.
