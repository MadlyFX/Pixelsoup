# Asset Viewer plan

A proposed panel for inspecting Library assets on their own, outside any
scene. Media plays in it like an editor's source monitor: scrub it, mark In
and Out, and cut the range into the timeline. A model sits in a neutral
look-dev scene, as in a game-engine asset viewer, and its asset-level
settings - materials, textures, pivot, interpretation - are set up there once
for every placement in the project.

Phases 1 and 2 are built; their As built sections
([phase 1](#as-built), [phase 2](#as-built-1)) record where they differ from
what follows. Phase 3 is not built. The plan has three phases that can ship
separately.
Each one lists what the operator sees, where the code goes, what it must not
break, its tests, and when it is done. The [decisions](#decisions) section
at the end records what has been settled and what is still open.

## What exists today

The plan reuses these pieces, so the work is smaller than the feature list
suggests. They also explain several of the constraints below.

- **Library.** `LibraryPanel::selection()` exposes the selected assets.
  Double-click or Enter places an asset in the scene. A drag from the Library
  resolves against Stage, the Timeline and the Compositor through
  `LibraryAssetDrop` (`engine/editor/include/mediaserver/editor/library_panel.hpp`).
  Media assets already carry their probe results (dimensions, frame count,
  rational rate, codec, start timecode), a fingerprint and a proxy policy
  (`engine/assets/include/mediaserver/assets/asset_registry.hpp`).
- **Derivatives.** Thumbnails, filmstrips, waveforms and proxies are keyed by
  asset id, fingerprint, profile and generator version, and cached in a
  machine-local cache that never enters a project
  (`engine/assets/include/mediaserver/assets/derivative_profile_id.hpp`,
  profiles 1-5). The editor uploads them through bounded texture caches.
- **Timeline editing.** `plan_insert`
  (`engine/scene/include/mediaserver/scene/timeline_edit.hpp`) already plans
  displacement and sync-locked tracks. Clipboard Insert and Overwrite (`,` and
  `.`) go through
  `apps/server/timeline_edit_integration.cpp`, aim at the targeted track (else
  the selected one) and commit at once. (The operator guide said they waited
  for `GO`; that was wrong, and is corrected.) Timeline
  shortcuts run only while `timeline_focused_` is set
  (`engine/editor/src/timeline/timeline_actions.cpp`). J/K/L, I/O, Space and
  the arrow keys are bound there; F is free.
- **Placements.** A `scene.media` placement carries `asset_id`, `source`,
  sequence fields, the source rate, and `ClipTiming` (start, duration,
  `source_in`, fades). A `scene.model` carries only `source`: a model
  placement is tied to its Library asset by path alone.
- **Model materials.** An imported package's `ModelPart` holds a material and
  texture indices but no source material index or name, so the Inspector
  lists parts as "Material part 1..N". Assigning a texture re-ingests the
  whole model into a new immutable package, repoints that one placement's
  `source` (`apps/server/model_source_authoring.hpp`), and imports the new
  package as another Library asset. A standalone placement's colour,
  roughness, metallic and emissive values override the imported ones only
  when `model_factor_overrides` is set; otherwise they are fallbacks for parts
  without a material. Today only the Blender detach path sets that flag.
- **Editor rendering.** Program, the transition input and Stage render on the
  editor window's device, are recorded into one command list and share
  `frame_media_cache`. Stage's views 2-4 create their
  `CompositorSceneExecutor` and `OutputTransform` lazily at 64x64 with
  `stage_view_limits()` and then follow their pane size
  (`engine/editor/src/shell/shell_frame_start.cpp`). A second scene device would switch
  off decoding onto the renderer's device
  ([media playback](docs/media-playback.md#decoding-onto-the-renderers-device)).
  Executors are expensive in VRAM: one measured about 1.7 GB at 1080p on
  2026-10-25, before the eager-allocation fixes. Re-measure before relying on
  that figure.
- **Shading.** `render::ViewShading` offers Lit, Unlit, Lights Only and
  Wireframe.
- **Audio.** There is no audio playback, only the waveform derivative.

## Rules for every phase

These follow from [AGENTS.md](AGENTS.md) and from how Program and Preview
already work.

1. **One identity.** The viewer edits the existing `asset.*` node. A
   derivative (thumbnail, filmstrip, prepared texture) never becomes a
   Library asset and never replaces a placement's `asset_id`.
2. **Viewer state stays on the Director.** The viewer's playhead, unsaved
   marks, zoom, pan, channel view, look-dev scene, camera and shading are never
   replicated, digested, sent to nodes or saved in the project, the same as
   Preview's playhead. View preferences may live in the workspace layout.
3. **The viewer shows what the renderer does.** Media decodes through the same
   source, proxy policy, colour path and alpha as a placement. Models resolve
   materials the way the renderer does. The viewer is not a second
   interpretation of the file.
4. **Bounded, and never at Program's expense.** The viewer renders on the
   window's device into a pane-sized target, created when first shown and
   released when closed. Decode, parsing and analysis run on workers, and
   frame recording stays inside fixed capacity. Over budget, the viewer
   refuses with a stated reason; it never stalls the control thread or
   lowers Program's quality. Measure Program decode and record time with the
   viewer idle and scrubbing before and after each phase.
5. **Edits go through the Director.** Each authored change is one undo step,
   validated by the Director. Locked targets, Blender-owned sources and the
   rehearsal lock refuse with a specific reason.
6. **New render-affecting fields** (phase 3) are normalized, replicated, part
   of `assets::digest` and of the scene digest nodes compare, and covered by
   checkpoint/delta equality tests. A node that cannot honour one fails
   readiness with a specific reason instead of drawing something else.

## Phase 1: media source viewer

### What the operator sees

**Opening.** Double-click a Library asset, or select it and press Enter, to
open it in the Asset Viewer. The panel comes to the front, creating its tab
if it is closed. This replaces the current place-on-double-click: dragging
an asset out of the Library is now the only way to add it to a scene. The
Library's right-click menu swaps **Add to Scene** for **Open in Asset
Viewer** (Enter). Double-clicking a folder still opens it, and a generator
click still adds one. A Library asset dropped onto the viewer also opens
there, and a timeline clip's menu gains **Open in Asset Viewer**.

Single-clicking only selects, so the viewer keeps its asset until another
one is opened and needs no pin. Back and forward buttons step through the
last 32 assets opened. **Window > Asset Viewer** also opens the panel. By
default it is a tab in Stage's dock beside Program Output, and **View >
Reset Workspace** puts it there.

**Header.** It shows the asset name, resolution, rate, codec, start
timecode, the version playing (`SOURCE`, `PROXY`, or
`PROXY REQUIRED - UNAVAILABLE` under Require Proxy) and the decode state
(`DECODING`, `HELD` with a count, or `OFFLINE` for a missing file, which also
draws the grey placeholder).

**Transport.** These keys work while the viewer has focus, set the same way
as the Timeline's (`timeline_focused_`), so one vocabulary drives whichever
panel is in use:

| Keys | Action |
| --- | --- |
| Space | Play / pause |
| J  K  L | Play backwards, stop, play forwards |
| Left  Right | Step a source frame (Shift: a second) |
| Home  End | Go to In / Out, or the first / last frame without marks |
| I  O | Mark In / Out at the viewer playhead |
| Alt+I  Alt+O  Alt+X | Clear In, clear Out, clear both |
| `,`  `.` | Insert / overwrite the marked range into the timeline |
| F | Match frame (from a selected timeline clip; Timeline focus) |

A loop toggle and **Play In to Out** sit beside the transport buttons. While
the viewer has focus, `,` and `.` use the viewer's range rather than the
clipboard, and the Timeline menu names both entries ("Insert Source",
"Insert Clipboard"). Both commit at once, as an editor's do (see below).

**Scrub bar.** The asset's filmstrip derivative draws behind the bar, with
the marked range shaded and the playhead flagged. A waveform strip appears
under it when the waveform derivative exists. It is display only: no sound
plays.

**Time readout.** Source timecode comes from the asset's start timecode plus
the frame at the asset's own rate, with drop-frame shown as `;` the way the
Library shows it. A toggle switches to zero-based frame numbers. The readout
gives the marked range in source frames and in the show frames it will
occupy. When the source and show rates do not divide evenly, it says so,
because the current integer `source_in` cannot keep the residual phase.

**Putting the range on the timeline.**

- Insert, Overwrite and Append target the same track the clipboard does: the
  targeted track, else the selected track (only while the selection holds
  it), else the selected clip's track, the primary clip's when the clips are
  on several. With none of those - no tracks, or none chosen - the range
  lands outside every track. Only a locked target refuses, and the viewer
  says **The target track is locked**. (It used to refuse whenever no track
  was chosen, and a track once clicked stayed the target after another clip
  was selected. Changed after the first live use.)
- Insert cuts a clip it lands inside and moves everything after the edit
  point along. Overwrite lays the range over the track, in front of every
  clip already on it: those stay whole underneath, nothing moves, and the
  edit keeps its length. Clipboard Overwrite works the same way. (Both first
  cut the covered clips into split remainders with `plan_overwrite`; in a
  track that lists each clip on its own row, that looked the same as Insert.
  Changed after the first live use.)
- The edit lands at the Stage scene's playhead, which in Preview is the
  Director-local Preview playhead.
- **It commits immediately, as one undo step.** There is no span preview and
  no `GO`. Afterwards the new clip is selected, and the status line names
  the edit. The Stage playhead does not move: on Program it is the live
  show's clock. A refused edit names its reason and changes nothing.
- Clipboard Insert and Overwrite already commit immediately, so `,` and `.`
  behave alike in both panels.
- Without marks the whole source is used. A still uses the duration a still
  gets when dropped on the timeline today.
- Model assets cannot go onto a track, and the viewer says so.
- **Dragging the picture** carries the asset and the range. Released on a
  track's lane it is laid over the track at the snapped drop frame, as
  Overwrite is. On empty timeline it
  makes a new track holding the trimmed clip. On Stage it makes a new
  placement there with that timing. Releasing on a track header still means
  **Replace source** and keeps the placement's timing.

**Match frame.** With a clip selected under the Timeline playhead, F loads
the clip's asset in the viewer. The viewer playhead goes to the source frame
showing at the Timeline playhead, and In/Out mark the source range the clip
uses. A looping clip marks the source frames from `source_in` to the end of
the first pass.

**Inspection.**

- Fit, 100% and 200%. Ctrl+wheel zooms about the pointer, and middle-drag
  pans. Zoom and pan change the synthetic camera, not the target size, so a
  4K source at 400% still renders at pane size.
- A channel view (RGB, R, G, B, A, luma), alpha over a checkerboard or over
  black, and a display-only exposure offset in stops.
- A pixel probe readout: the source pixel coordinate and linear RGBA under
  the pointer, one frame late, read back from the working-format target.
- An overlay of the target track's screen raster, so a plate of the wrong
  size or aspect is visible before it is cut in. Screens deliberately do not
  reshape to fit their clip.

**Used in.** A collapsible list shows each placement using this asset: scene,
track, start timecode and duration. A row in the Stage's scene selects that
clip. Rows in other scenes name their scene and do not switch scenes.

### Where the code goes

**Editor (`engine/editor`).**

- `asset_viewer_panel.hpp/.cpp`: an immediate-mode panel like the others. It
  takes the asset, the viewer's state, its rendered texture and
  derivative/decode observations, and returns an `AssetViewerFrame` of
  intents: source edit, drag, match frame, open-in-Library, and the pixel
  probe position.
- `AssetViewerState` (editor-local) holds the asset id, history, source
  frame, play direction, marks, zoom/pan, channel view and exposure.
- `library_panel.cpp`: double-click and Enter on an asset report a new
  `LibraryFrame::view_asset` instead of `instantiate_asset`, and the menu's
  Place command becomes Open in Asset Viewer. `instantiate_asset` remains
  for drops, which already place through the shell's drop resolution.
  Viewer-local preferences (channel view, overlay toggles, the loop toggle)
  go into the workspace layout, where the roadmap also wants the Library's
  view settings.
- `asset_viewer_scene.hpp/.cpp`: a pure function from (`MediaAsset`, source
  frame, framing, pane size) to a `scene::Scene` holding one media placement.
  It is always on, its `source_in` is the requested frame, and its proxy
  policy and rate come from the asset. Rendering this through the normal
  executor keeps decode, planar video, proxy fallback, colour and alpha
  identical to a real placement (rule 3), and makes it headless-testable.
- Shell wiring: a new panel id `editor.panel.asset_viewer`, a Window menu entry
  (`window_panel_entries` grows to 22), the default dock tab, bringing the tab
  forward on `view_asset`, a Library drop target on the viewer, a keyboard
  focus flag, and routing `,`/`.` to either the viewer or the clipboard.
- The viewer's renderer copies the Stage views 2-4 pattern: one
  `CompositorSceneExecutor` and one `OutputTransform`, created lazily on the
  window's device with `stage_view_limits()`, recorded into the shared command
  list with `frame_media_cache`, following the pane size one frame behind,
  and released when the panel closes. The same executor serves phase 2's
  model mode.
- A viewer display pass in `engine/render`, behind the RHI, provides channel
  view, checkerboard and exposure. It runs on the viewer's image only;
  `OutputTransform` stays the wall's boundary and gains no editor modes.
- The pixel probe uses one `rhi::Readback` of one texel, polled without
  waiting.

**Playback.** The viewer keeps its own clock at the asset's rate from window
time. A decode miss holds the last complete frame and counts it as `HELD`,
never skipping silently. Reverse play is cold for read-ahead, which runs
forwards inside the loop window, and the `HELD` count makes that visible.

**Decode priority.** The viewer shares `frame_media_cache` and the decode
scheduler with Program and Stage. First measure whether scrubbing evicts
Program read-ahead or adds Program held frames. Only if it does, add a
low-priority request class capped at a few outstanding requests, so Program's
placements always win.

**Director (`apps/server`).**

- A new editor intent, `SourceRangeEditIntent {asset, source_in, source
  frames, mode (Insert / Overwrite / Append / NewTrack / StageDrop), scene,
  lane, start, gesture}`.
- `timeline_edit_integration.cpp` converts the source range to show frames
  with the rational mapping and rounding playback already uses. It plans
  displacement with `plan_insert` (Overwrite displaces nothing) and authors the new
  `scene.media` placement (asset_id, source, sequence fields, rate, lane,
  timing) the way `instantiate_asset` does. Everything is one transaction,
  scheduled on the next sealed frame with no confirmation step. The result
  reports the new clip's id so the editor can select it and move the
  playhead.
- Clipboard Insert and Overwrite drop their pending-preview state and go
  through the same immediate path.
- Refusals with names: locked or missing target, rehearsal lock, model asset,
  missing frame count or rate (`SOURCE NOT PROBED`), and a range outside the
  source.

### Tests

- `engine/editor/tests/asset_viewer_panel_tests.cpp`:
  - marks, clearing, Home/End
  - keys active only with viewer focus
  - `,`/`.` routed to the viewer or the clipboard by focus
  - a dropped Library asset opens, and history order
  - timecode at 24, 25, 29.97 drop-frame and 59.94 with a non-zero start
    timecode
  - intents carrying distinct, non-default values
- `asset_viewer_scene` tests:
  - the synthetic placement shows exactly source frame N for N at the first,
    middle and last frame
  - proxy policy and rate carried from the asset
  - an offline source still yields the placeholder path
- `apps/server/tests/timeline_edit_integration_tests.cpp`, source range cases:
  - exact `source_in` and duration at 24000/1001 into 25 and at 30 into 24
  - sync-locked tracks moving, a locked track refusing
  - Overwrite laying the range in front of the track's clips, cutting and
    moving nothing
  - Append after the last clip
  - the edit applying on the first sealed frame with no confirmation, and
    reporting the new clip
  - clipboard Insert and Overwrite applying the same way
  - one undo restoring the document exactly
  - each named refusal leaving the document unchanged
- `library_panel_tests.cpp`: double-click and Enter report `view_asset` and
  never `instantiate_asset`; a folder double-click still opens it; a drag
  still reports `asset_drop`; the menu offers Open in Asset Viewer and no
  Add to Scene. Rewrite the existing "a click selects and a double-click
  places" case.
- `editor_shell_workflow_tests.cpp` (and the shell tests that place through
  `instantiate_asset` today):
  - reachable from the Window menu
  - a Library double-click opens the viewer and places nothing
  - viewer `,` inserts the source range, not the clipboard, and the new clip
    is selected with the playhead after it
  - match frame lands on the clip's source frame
- A new `asset-viewer` view in `mediaserver_editor_ui_snapshot`.
- A live check on real 4K24 media:
  - Program held frames and record time with the viewer closed, idle and
    scrubbing (interleaved A/B)
  - VRAM with the viewer closed and open (`mediaserver_render_vram_benchmark`
    and process usage)

## Phase 3: asset-level settings

This phase makes the viewer an editor of the asset record, so an asset is set
up once and every placement inherits the setup. As in Unreal, a placement
can still override any material slot, textures included, without touching
the asset. It is the largest phase and touches the schema, scene evaluation,
the model package and the renderer.

### 3a. Models get a Library identity on their placements

- Add `asset_id` to `scene.model`, normalized exactly as `scene.media`'s
  (positive, optional). `source` stays the renderer's locator. Library drops
  (Stage, Timeline, Compositor), the viewer's drag and Replace source write
  `asset_id`, and `ModelInstance` carries it.
- Placements saved without `asset_id` keep rendering as they do now but
  inherit no asset settings. Per AGENTS.md there is no migration layer. The
  shipped test projects are re-placed or re-saved once.
- Texture assignment stops importing derived packages as Library assets and
  stops repointing `source`. 3b replaces it.

### 3b. Material slots on the asset and on each placement

This works the way Unreal's mesh assets and per-actor material overrides do.
The asset sets each slot's material once. A placement can then override any
property of any slot, textures included, and every property it leaves alone
keeps following the asset.

**Package format.** Each part records its source material index (the glTF
material or OBJ `usemtl` index). The package gains a material table: slot
index, source name, imported material and imported texture references. Bump
the package version; packages are rebuilt. This closes the roadmap item on
preserving material names in packages.

**Two override layers sharing one vocabulary.**

- `asset.material_slot` is project-wide: one node per (model asset, slot)
  that has overrides, holding `asset` and `slot`.
- `scene.material_slot` is scene content: one node per (model placement,
  slot) that has overrides, holding `target` (the `scene.model`), `scene_id`
  and `slot`. Because it is scene content, Create Copy duplicates it with
  `target` repointed (`apps/server/scene_duplicate.cpp`), deleting a
  placement deletes its slot nodes in the same transaction, and editing one
  in Preview cannot reach Program.
- Both use the existing scene material property names: colour, roughness,
  metallic, emissive and strength, normal scale, occlusion strength, alpha
  mode and cutoff, double-sided, base alpha, the extension lobes, and the 14
  texture roles. A present property is an override and an absent one
  inherits. A texture role holds an absolute image path, or an empty string
  meaning "no texture here", which differs from absent, meaning "inherit".
- Separate nodes keep each node small, make one slot's edit one node change,
  and make **Reset slot** a node delete. Capacity is fixed, and refused with
  a reason beyond it: 256 slots per asset, 4,096 asset slot nodes and 16,384
  placement slot nodes per project.
- The whole-model factor fields on `scene.model` (colour, roughness, metallic
  and emissive, gated by `model_factor_overrides`) are retired for imported
  models in favour of placement slot nodes. The Blender detach path writes
  slot nodes instead. Primitives keep their single material. The software is
  unreleased, so there is no migration.
- Blender-linked models refuse both layers, because "Materials follow
  Blender".

**Resolution.** For each part and each property, the value is the imported
one, then the asset slot's override, then the placement slot's override,
the last present one winning. Texture roles resolve the same way.
`AssetRegistry` gains the asset slots, which go into `assets::digest`; the
scene model gains the placement slots, which go into the scene digest. Values
clamp to the ranges the scene material reader uses, and texture paths follow
the source path rules (absolute, no `..`, at most 1,024 bytes).
`ModelInstance` carries its `asset` and a resolved table for only the slots
that have an override at either layer, so a model nobody has customized
costs nothing extra. The renderer maps each part to its slot through the
package and applies the table where it calls `model_part_material` today.
Factor overrides need no preparation and apply on the next frame.

**Textures are prepared once per image and shared.**

- Because placements can override textures, the earlier idea of rebuilding a
  whole model package for each material recipe would copy the model's
  geometry and every texture for each placement that differs. Textures are
  therefore prepared one image at a time.
- A prepared texture is a machine-local derivative, keyed by the image
  fingerprint, its role class (colour, data or normal) and the encoder
  version. It is a geometry-less package holding one BC7 or BC5 mip chain,
  encoded on the model worker.
- `TextureResidency` already shares textures by (source, index) across every
  renderer on a device. A prepared texture enters it as its own source, so
  one image used on fifty placements is one upload, counted once against the
  texture budget.
- At draw time each part's role binds either the package's own texture or the
  instance's prepared one. The residency planner receives prepared textures
  with the instance's projected size. Override textures per frame have a
  fixed capacity; exceeding it refuses and diagnoses the frame.
- Every machine prepares from the authored image, which is required content,
  so the encoder must produce identical bytes everywhere: hash the derivatives
  built on two machines.
- A machine without a prepared texture never draws the part untextured.
  Program holds the previous look until every required node reports the new
  texture generation, using the generation fence baked lighting uses. Stage
  says `PREPARING TEXTURES` while it waits.

**Readiness.** Override texture images are required content. They join the
asset manifest with their own expected byte length and SHA-256, so presence
checks, RESCAN and Output Monitor readiness cover them like any source.

**Where they are edited.**

- **The Asset Viewer's Materials tab edits the asset.** It has a slot list
  with names, a swatch and an override dot. The slot editor reuses the
  Inspector's field widgets and colour picker. Texture rows show a
  thumbnail, **Browse...**, a drop target for a still image from the
  Library, and **Clear**. Each slot shows "N placements, M override this
  slot", and **Reset slot** is there.
- **The Inspector edits the selected placement.** A Materials section
  replaces today's numbered "Material part" selector and its texture rows.
  It lists the slots by name. Each property shows where its value comes from
  (Imported, Asset, or This placement) and has a reset arrow that removes
  the placement's override, as in Unreal's Details panel. Texture rows have
  the same controls as in the viewer, plus the reset arrow.
- The Inspector also has **Reset slot** and **Open Asset**. **Push to asset**
  moves this placement's overrides onto the asset slot and clears them here,
  as one undo step that changes every placement of the asset.
- With several placements of the same asset selected, an edit applies to all
  of them, and differing values show as mixed.
- Each edit is one undo step. Texture preparation runs after the commit and
  shows its progress.
- Asset slot edits are project-wide, so they reach Program when a Program
  placement uses the asset. The viewer marks such assets `ON PROGRAM`, and
  how edits behave there is still [open](#decisions). Placement overrides
  are the way to change the look in Preview alone.

### 3c. Import transform and pivot

- New `asset.model` fields: uniform unit scale, up axis (Y-up or Z-up) and
  pivot (source origin, bounds bottom centre, or bounds centre). They compose
  before the placement's TRS, beside today's `model_affine` residual. They
  are normalized, replicated and digested.
- The registry keeps the source bounds and derives evaluated bounds. Stage
  drops and the rest-on-surface placement use the evaluated bounds.
- Changing the transform of an asset already placed either keeps placements
  where they stand (their positions are compensated in the same transaction)
  or moves them with the asset; this is still [open](#decisions).
- These stay asset-only. A placement already owns its transform, timing,
  animation and grade, so a per-placement pivot or interpretation would only
  duplicate those. The same applies to 3e.

### 3d. Model placement defaults

The default animation clip, looping and Bake Geometry setting are stored on
the asset and copied into each new placement when it is created. Placements
keep authority afterwards, so changing a default never rewrites existing
placements.

### 3e. Media asset settings

- **Interpretation:**
  - a frame-rate override (rational)
  - alpha (straight, premultiplied or ignored)
  - input colour, limited to what the decode path converts today
- Interpretation affects rendering, so it is replicated and digested and
  every node resolves it the same way. A setting that leaves Rec.709 takes
  the system-memory path, and the header says why.
- A rate override updates existing placements' cadence in the same
  transaction, the way Replace source does. Their show-frame timing stays.
- **Poster frame**: `poster_frame` on `asset.media`. The thumbnail frame rule
  uses it when present, so the thumbnail key changes with it. **Set Poster
  Frame** is in the viewer.
- **Subclips**: a new `asset.subclip` node (asset, name, source In, source
  Out, folder). The Library lists subclips under their asset, and they drag
  and insert like the asset with their range. Saving the marks in the viewer
  creates one. Placements copy the timing at insert and never reference the
  subclip, so deleting a subclip changes nothing on the timeline.

### Tests

- `engine/assets/tests`, for every new field and node:
  - parse, normalization and digest with distinct non-default values
  - malformed and over-capacity refusals
  - `AssetModel::reset` versus `apply` equality across checkpoint and delta
- `engine/scene/tests`:
  - imported < asset < placement precedence for every property, each layer
    with distinct non-default values
  - texture roles: absent inherits, a path overrides, empty means none at
    either layer
  - placement slot nodes parse, digest and survive checkpoint/delta equality
  - `asset_id` on models
  - pivot, unit and up-axis composition against independent matrices
- `engine/render/tests`:
  - packages keep the slot index and name
  - the prepared-texture key changes with the image fingerprint, role class
    and encoder version, and nothing else
  - two placements sharing an override image hold one resident texture,
    counted once against the budget
  - over-capacity override textures refuse the frame with a diagnosis
  - digest-mode pixel proof of asset and placement overrides
  - Program holds until the prepared textures are ready on every required
    node
- `apps/server/tests`:
  - each edit is one undo step, plus the Blender, locked and on-Program paths
  - Create Copy duplicates placement slots with `target` repointed, and
    deleting a placement deletes its slots in the same transaction
  - Push to asset moves overrides and clears them as one undo step
  - the Blender detach path writes placement slot nodes
  - the manifest lists override textures, and a missing one is reported
  - a rate override updates placements exactly
- `engine/editor/tests`: the Materials tab, the Inspector's Materials section
  (value sources, reset arrows, mixed multi-selection, Push to asset), and
  subclip save and drag.
- Two machines with different GPUs (the RTX 3090 Director and RTX 2060 node
  used for media qualification) render the same digest with asset and
  placement overrides active, and hash identical prepared textures.

### Done when

A model's materials, textures and pivot are set once in the viewer and every
placement in every scene shows them. Any placement can override any slot
from the Inspector and reset it back. Prepared textures are machine-local,
shared across placements, and verified on two machines. Media
interpretation, poster frames and subclips work end to end. The operator
guide, renderer guide and [architecture](media-server-architecture-plan.md)
describe both override layers.

## Order and size

| Step | Depends on | Relative size |
| --- | --- | --- |
| Phase 1 media viewer | - | Medium-large: new panel and renderer wiring, one new Director intent |
| Phase 2 model inspector | Phase 1 panel and executor | Medium: look-dev scene, checks, two shading modes, a thumbnail profile |
| 3a model `asset_id` | - | Small |
| 3e media settings | Phase 1 (poster frame, subclips) | Medium: interpretation touches decode and colour |
| 3b-1 slot factors, both layers | 3a, package format | Large: schema, evaluation, package slot table, Inspector and viewer UI |
| 3b-2 slot textures, both layers | 3b-1 | Large: prepared-texture derivative, renderer binding, residency, readiness, fence |
| 3c import transform | 3a | Small-medium |
| 3d placement defaults | 3a | Small |

3a and 3e do not need phase 2, so they can follow phase 1 directly. 3b ships
in two halves: factor overrides are useful without textures, and they keep
the renderer's texture-binding change out of the first half.

## Later

These were considered and left out of the first cut: the alpha view and
checkerboard, the target-screen raster overlay, the waveform strip under the
scrub bar, scopes (waveform, vectorscope, histogram), A/B wipe comparison against another asset or the
Program frame, nearest-neighbour pixel view, asset notes and markers, forced
LOD display, a texel-density view, multi-speed shuttle, and audio playback
(which needs an audio path the project does not have).

## Decisions

Settled:

| Question | Decision |
| --- | --- |
| How does an asset reach the viewer, and how is it added to a scene? | Double-click or Enter opens it in the viewer. Dragging and dropping is the only way to add it to a scene; the Library menu's Add to Scene becomes Open in Asset Viewer. |
| Do Insert and Overwrite confirm? | No. Viewer and clipboard edits both commit immediately, as in a video editor; undo restores. |
| Can a placement override materials? | Yes, as in Unreal: any property of any slot, textures included, through scene-owned placement slot nodes over the asset's slots. |
| How are override textures prepared? | One prepared texture per image, shared across placements and bound at draw time. This follows from placement texture overrides: one rebuilt package per recipe would duplicate geometry per placement. |

Still open:

| # | Question | Recommendation | Alternative |
| --- | --- | --- | --- |
| 1 | Where does the panel dock by default? | A tab beside Program Output in Stage's dock | Its own split left of Stage |
| 2 | What happens to asset edits on an asset used by Program? | Allow, marked `ON PROGRAM`, with a confirmation on each gesture's first edit. Placement overrides cover Preview-only changes. | Refuse while on Program |
| 3 | What happens to existing placements when an asset's pivot or units change? | They keep their place (positions compensated) | They move with the asset |
