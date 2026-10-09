# Remaster: research and plan

Status: research only, nothing started. Goal: a remastered Renegade that
looks like a modern game but plays exactly like the original, in the browser.
Later it uses our own assets (needed for public hosting and the RA2 mod).

## Where we are (measured, 2026-10-09, Apple M4, 1280x800)

- About 20k triangles and 150-700 draw calls per frame, GPU time 1-2 ms, game
  work 4-7 ms.
- The GPU is about 95% idle. The headroom for better graphics is huge; the
  limits are art effort, download size and draw-call overhead, not raw GPU
  power.
- Rendering today: WW3D (the engine) emits D3D9 fixed-function calls, and our
  layer (`Code/Platform/gfx`) emulates them on WebGL2.
- The look is 2002's:
  - vertex lighting plus baked lightmaps;
  - 256-512 px textures;
  - no normal maps, no dynamic shadows except blob/projected ones;
  - no post-processing.

## What defines "plays the same" (must not change)

Gameplay comes from data that is separate from what you see. Keep these
identical and the visuals can change almost freely:

| Gameplay data | Where it lives | Rule for the remaster |
|---|---|---|
| Collision / physics geometry | W3D meshes flagged physical/projectile/vis, level AABTrees | Keep the original collision meshes. New visuals are render-only and must match the old silhouettes closely, or players hit invisible walls and shoot through visible ones |
| Hitboxes (head/torso/arm/leg damage zones) | Character collision boxes on skeleton bones | Keep the skeleton and bone names, so new characters are skinned to the original rigs |
| Animation timings (reload, fire, vehicle) | Animations plus object definitions | New animations may look better, but their length and events (muzzle flash bones, footsteps) must keep the same timing |
| Pathfinding, AI | Baked pathfind data in the level files | Unchanged if the collision is unchanged |
| Visibility culling (VIS sectors) | Baked per level | Either keep the old volumes or turn VIS off: modern GPUs can draw the whole level |
| Scripts, objectives, cinematics | Mission scripts, object names and IDs, cinematic text files | Object names and IDs must stay stable; cameras keep their positions |
| Weapons, damage, speeds | Definition database (presets) | Untouched |

Practical rule: **new render meshes on top of the old gameplay meshes.** The
renderer draws the new asset; physics, AI and hit detection use the original.

## How far it can go

1. **Renderer remaster, original assets** (no art needed):
   - per-pixel lighting, real-time shadows (cascaded sun shadows, local
     lights), SSAO, bloom, HDR tonemapping, fog that matches each map, MSAA or
     TAA, anisotropic filtering, a proper widescreen HUD/FOV;
   - optional AI-upscaled textures, plus generated normal and roughness maps
     from the originals (user-side, from their own copy).
   - Biggest win per hour of work.
2. **Asset remaster, same gameplay**:
   - new characters, vehicles, weapons and buildings with PBR materials (2-4k
     textures, 10-50k triangles per hero asset), skinned to the original
     skeletons;
   - map render geometry and terrain rebuilt over the original collision,
     with new baked GI or lightmaps (a modern bake, for example from Blender);
   - this is the real "remaster" and mostly an art pipeline problem.
3. **Remake** (new map layouts, new animation sets, physics changes): no
   longer the same game. It needs new pathfinding/VIS bakes and
   re-balancing. Out of scope except for new content (for example RA2-themed
   maps), which would go through LevelEdit anyway.

## Renderer architecture

- **Keep "Classic" mode:** today's D3D9-on-WebGL2 path, pixel-faithful, runs
  anywhere.
- **Add "Remastered" mode on WebGPU** (Chrome/Edge today; Safari and Firefox
  shipping it). It plugs in at the WW3D level (meshes, materials, lights,
  cameras), not at the D3D9 level: D3D9 calls carry too little information
  for PBR or shadows.
  - **Materials:** each W3D shader/vertex material maps to a PBR material.
    Asset overrides are keyed by W3D name (e.g. `c_havoc` -> remastered
    glTF).
  - **Lighting:** a sun from the level's lighting settings plus the level's
    lights, cascaded shadow maps, clustered lights, baked GI per map.
  - **Post-processing:** TAA, bloom, SSAO, tonemapping, color grading per
    map.
  - **Performance:** instancing and GPU-driven culling. WebGPU's lower
    per-call overhead keeps draw calls cheap, which today's numbers show
    matters.
- **No ray tracing:** browsers have no hardware ray tracing (WebGPU has no RT
  extension yet). Global illumination comes from offline bakes, which also
  run on every GPU.
- **Asset format:**
  - glTF 2.0 meshes, with KTX2/Basis textures (GPU-compressed, about a
    quarter of the size of PNG in memory);
  - per-asset LODs;
  - streamed per map from browser storage.

## Spec tiers (estimates, to be validated with a vertical slice)

| Preset | Features | Target hardware for 60 fps |
|---|---|---|
| Classic | Original look (today) | Anything with WebGL2, including phones and old laptops |
| Remastered Low | PBR, normal maps, 1 shadow cascade, FXAA, 1080p, 1k textures | Integrated GPUs from ~2020 on: Intel Iris Xe, AMD 680M, Apple M1 |
| Remastered High | 3-4 cascades, SSAO, TAA, bloom, 1440p, 2k textures | Apple M2-M4, GTX 1660 / RX 6600 class |
| Remastered Ultra | 4k, 4k hero textures, higher shadow resolution, more lights | RTX 3060 / RX 6700 class and up |

**Do we need a stronger graphics card?**
- No for Classic.
- For Remastered we design down to integrated GPUs and scale up with
  presets, because a browser game's audience is mostly laptops.
- A dedicated GPU helps only at High/Ultra and high resolutions. This Mac mini
  (M4) should run High at 1440p.

**Other limits that bite before the GPU does:**
- **Download and storage size.** The original is 1.3 GB. A full remaster with
  2k PBR textures is likely 5-15 GB.
  - Mitigations: stream per map, Basis-compressed textures, and an
    optional "HD texture pack".
- **Memory:** wasm is limited to 4 GB (game logic is fine); GPU memory is
  separate. Plan for 2-4 GB VRAM at High.
- **CPU and draw calls:** game logic is cheap. More objects and materials
  mean more draw calls, so batching and instancing are mandatory in the new
  renderer.

## Tools we already have in the source

- `Code/Tools/max2w3d`: the 3ds Max exporter source. It documents the W3D
  format exactly; a Blender add-on or a glTF<->W3D converter can be written
  from it.
- `Code/Tools/W3DView`: model and animation viewer.
- `Code/Tools/LevelEdit`: the level editor (MFC/Windows). It's needed for map
  work: run it on Windows, or port it.
- `Code/Tools/LightMap`: the original lightmap tool.

## Phases

1. **Renderer groundwork** (no new art):
   - WebGPU device next to WebGL2;
   - a WW3D-level render path with PBR, shadows, post-processing;
   - original assets with generated normal/roughness maps;
   - Classic/Remastered toggle; benchmark scene and FPS tests per preset.
2. **Asset pipeline:**
   - W3D -> glTF export (meshes, skeletons, animations) into Blender;
   - glTF override loading keyed by W3D name;
   - automated checks that collision, bones and animation timings still
     match.
3. **Vertical slice:**
   - one map (C&C_Field or M01), Havoc plus one enemy type, one vehicle,
     two weapons, fully remastered;
   - validates the look, the spec tiers, download size and the "plays the
     same" rule (automated playthroughs, hitbox comparisons).
4. **Scale out:**
   - all characters, vehicles and weapons, then maps by priority
     (multiplayer maps first if online comes before the campaign);
   - art is the long pole here: hundreds of assets and 12 campaign plus
     ~12 multiplayer maps;
   - AI tools can help with textures and concepts; models and rigs still need
     an artist pass.
5. **Own content:** the same pipeline makes the original-free asset set
   needed for public hosting and for the RA2 mod.

## Decisions to make

None of these blocks phase 1 (renderer groundwork with the original assets);
all of them shape phases 2-5.

| # | Decision | Options | Notes / recommendation |
|---|---|---|---|
| 1 | Art direction | (a) faithful "HD original": same designs, more detail; (b) reimagined designs; (c) faithful first, reimagined later | (a) is cheaper, keeps silhouettes close to the collision meshes (the "plays the same" rule) and is what fans of a remaster expect |
| 2 | Who makes the art | in-house, contractors, community/modders, AI-assisted (textures, concepts, upscales) with an artist pass | Decides the timeline far more than the code. AI helps most with textures and concepts; models, rigs and animations still need artists |
| 3 | Target audience hardware | laptops (built-in GPUs), gaming PCs, both | Decides the default preset. Recommendation: design down to Remastered Low on ~2020 built-in graphics |
| 4 | Remaster level | 1: renderer only; 2: renderer + new assets; 3: remake | Recommendation: 1, then 2. Level 3 only for new content (e.g. RA2-themed maps) |
| 5 | Renderer technology | WebGPU (with WebGL2 kept for Classic); WebGL2-only remaster | WebGPU: lower per-call overhead, compute for culling and post-processing. Keep Classic on WebGL2 for old devices and phones |
| 6 | Modes in the product | Classic/Remastered toggle; Remastered only | Keep the toggle: the faithful look is a feature, and it's the fallback |
| 7 | Lighting approach | Offline-baked GI/lightmaps per map + real-time sun/local shadows; fully real-time | No browser ray tracing exists, so baked GI plus real-time shadows |
| 8 | Texture resolution and download size | 1k / 2k / 4k; one package vs. base + optional "HD texture pack"; stream per map | Estimate 5-15 GB for a full 2k remaster vs. 1.3 GB original. Recommendation: Basis-compressed 2k base, optional 4k pack, per-map streaming |
| 9 | Order of content | campaign first; multiplayer maps first; one map per mode | If online multiplayer comes first, start with the MP maps (fewer, reused a lot) |
| 10 | Vertical slice content | C&C_Field (MP) or M01 (campaign); Havoc + one enemy + one vehicle + two weapons | Recommendation: C&C_Field (all core assets: infantry, vehicles, buildings, terrain) |
| 11 | Hitbox / collision policy | Keep the original collision exactly; allow small adjustments | Keep exact. Any change is a gameplay change and needs re-testing |
| 12 | Animations | Keep the originals (on new models); new animations with identical timing and event bones | Start with the originals; replace later where they look dated |
| 13 | Level editing | Run the original LevelEdit on Windows; port LevelEdit; build our own glTF-based map pipeline | Needed for new maps and re-baking. Decide when phase 3 starts |
| 14 | Relation to our own assets | Remaster assets = the original-free asset set for public hosting and the RA2 mod; keep them separate | If designs stay faithful to EA's, they are still derivative of EA's art (see OPEN_SOURCE_PLAN.md) |

## Effort and risk notes

- Code (phases 1-2): a renderer and a pipeline, weeks to a few months of
  focused work. Benchmarked against the presets above.
- Art (phases 3-4): hundreds of models (infantry classes, vehicles,
  buildings, weapons, props) and ~24 maps. This is the bulk of a remaster.
- Biggest gameplay risk: visual geometry that no longer matches collision.
  It is caught by automated checks (silhouette/bounds comparisons per asset)
  and scripted playthroughs (the existing browser tests).
- Biggest platform risk: download size and browser storage quota, handled by
  streaming and compression.
