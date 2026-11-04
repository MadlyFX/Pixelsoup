# ajfx Studio

The scene editor for [ajfx](../README.md). The authored work is a scene;
reusable effects and captured ranges are delivery choices. Studio exports
`.ajfx` packages for a host, today the media server.

**New** opens a scene with a static **Ground** object aligned to the grid,
a tree, central viewport and selected-item inspector. **Add > Floor** creates
another editable ground surface, and **Add** lists forces (Wind, Vortex,
Attract, Draw in, Blast) that are placed items acting on the particle
systems they name, and under Smoke a smoke grid: a box of air that particle
systems fill with smoke and ride. A relationship binds an object to a
particle system (particles born where it hits, or shed behind it as it
moves), a system to another system's particles, or a system to a smoke grid
(feeding it, riding it, or both). Objects own geometry, visual and physical
properties and fracture; Scene settings own shared physics. Import accepts
multiple models. Particle systems own their sources, a Motion stack of typed
behaviors (Drag, Turbulence, Vortex, Wind, Attract, Draw in) in the order
they run, and appearance, where size and colour are constant or follow keys
over each particle's age. Lifetime, speed, size and the like are ranges each
particle picks from, and every system and behavior has its own seed.
See the [current scene reference](../docs/studio-scene-reference.md) for
implemented behavior and limitations, and the
[remaining work](../next-steps-plan.md).

The scene architecture is not yet complete: continuous population coupling,
cache-backed preview and captured actions are pending.

Studio's planned direction is an independent professional creative application,
with its own visual identity, workspace and interaction model. Matching the
media server's UI or using its framework is not a requirement. The
[Studio design](../docs/studio-ui-architecture.md#product-direction-and-quality-bar)
defines that direction; the
[delivery plan](../next-steps-plan.md#workspace-and-application-design)
puts workflow and workspace prototypes before the UI technology decision.
The renderer, toolkit and build instructions below describe today's application;
no replacement framework has been selected or implemented.

## How a scene compiles

The compiler (`src/compile.*`) turns a scene straight into one
`ajfx::Effect`: every particle system an emitter of its own, every Object a
body kind or a prop of the scene's one solid world, every smoke grid a volume
and every blast a blast. Module ids and seeds come from items' reserved
identities, so names, tree order and grouping never change the effect, and
an edit to one item leaves every other emitter as it was. A scene that cannot
play whole does not play, and the first item at fault is named.
`ajfx-studio-cli <scene> compile` prints what a scene compiles to. The
[scene reference](../docs/studio-scene-reference.md#compiling-a-scene) has
the whole mapping.

## The preview

The preview is drawn by the media server's own renderer:
- the Advanced D3D12 scene renderer;
- its particle pass, with depth sorting, soft particles, particle lights and
  motion vectors;
- temporal antialiasing, bloom and the display transform.

So an effect looks as it will on the wall. A 1.8 m figure stands beside the
scene for scale.

| Input | Action |
| --- | --- |
| Left drag | Orbit |
| Middle drag | Pan |
| Right drag, wheel | Move in and out |
| Click | Select the solid thing under the pointer; Ctrl adds to the selection |
| Double-click, F | Frame the selection, or the whole scene |
| Drag a handle | Move or size the selected item; Shift raises or lowers a position |
| G | Guides on or off |
| H | Handles on or off |
| P | Particle paths on or off |
| Drag a timeline marker | Move a release, burst, blast or animated move in time; drag a move's end for its duration |
| Click a timeline name | Select that item |
| Space | Play or pause (letter and Space shortcuts act with the pointer over the viewport or timeline) |
| L | Loop over the capture range, or play on past it |
| Home | Back to the start |
| Left, Right | Step a frame |
| / | Open Add |
| Ctrl+Z, Ctrl+Y | Undo, redo |
| Ctrl+A, double-click a field | Select the field's whole text |
| Ctrl+S | Save |
| Ctrl+=, Ctrl+-, Ctrl+0 | Interface zoom |

For checking the interface without a mouse, `ajfx-studio` takes start-up
flags:
- `--at <seconds>` starts paused at that time;
- `--once` plays once and stops instead of looping;
- `--paths` starts with particle paths shown, `--no-guides` with guides
  and handles hidden;
- `--fresh` starts without taking up where Studio was left;
- `--gpu-debug` turns on the D3D12 validation layer, for chasing a lost GPU
  device. Run under a debugger (cdb, Visual Studio) to see what it reports;
- `--lose-device <seconds>` has D3D12 remove Studio's own GPU device after
  that long, as a fault or driver reset would.

**A lost GPU device.** A GPU fault, hang or driver reset removes the
device, and nothing can be drawn again. Studio notices when the next frame
cannot be shown, says why in a system dialog, offers to save the scene if it
has unsaved changes, and closes. It does not sit frozen on its last frame.

**How an edit reaches the interface.** The scene is compiled off the
interface thread (`CompileWorker`), so a drag never waits for the compiler:
each frame hands over the scene as it stands, the newest replaces any still
waiting, and a compile is taken up only if it is newer than the one shown.

**How an edit reaches the preview.** A background worker turns each edit
into a package:
1. It compiles the scene.
2. It compiles the kernels with DXC, cached by their source hash, so a
   changed value never waits for the shader compiler.
3. It rewrites the preview package in place.

The renderer adopts the package at the next asset generation. Studio raises
the renderer's particle step budget (`RendererLimits::particles`), so after
an edit the effect catches up with the playhead in one frame. While a value
is dragged, a new package goes at most every 0.12 s, once the worker is idle
and the preview has drawn the last for a few frames (or 0.75 s has passed).
Letting go always sends the final value.

**Live values.** The package reads the selected particle system's Motion
values (speed, spread, direction, radius, gravity share and each behavior's
strength) from the preview's placement as published parameters
(`live_parameters`), so an edit of only those sends no package: the renderer
restarts the effect with the new values on the next frame. Any other edit
builds a package as above. Selecting another system builds one package that
reads that system live. A frame still catching up with the playhead, or that
fails, shows the last complete picture for up to half a second instead.

**Framing and paths.** The worker also runs each new effect on the CPU
reference, on a thread of its own, with the preview placement's own instance
seed. That shows where its particles and bodies go, for framing the camera,
picking with a click and the particle paths. The selected system's paths come
first, from a run of it without the systems it is not born of
(`focus_scene`), then the whole scene's run, which a newer edit replaces
while one is being dragged. A live-value edit asks for these runs alone.

## Baking and export

The scene's simulation bakes by itself a moment after the last change, once
the preview is up to date; any edit stops that bake and takes it again once
the scene settles. **Bake** runs it on request and says beforehand what it
will cost. The transport shows the steps the cache holds as a band, green
while current and amber once stale. A colour edit keeps the cache; a motion or
collider edit makes it stale. **Export .ajfx** writes the package and, when it
belongs to the exported effect, the cache beside it as `name.ajfxcache`.

The preview still simulates: the cache is the same particles, since the
simulation is deterministic, and playing it back is the host's.

## Where Studio was left

Studio writes where it was when it closes (`%APPDATA%/ajfx Studio/session.json`)
and takes up there when it opens: the scene that was open and its changes if
they were not saved, the selection and folded groups, the window size and
interface zoom, the panel splits, night dim, the overlays, the camera, the
playhead and looping. Changes discarded when closing are not kept. A scene
given at start replaces the scene; `--fresh` forgets the lot.

## Files

- `.ajfxstudio` is the scene, as JSON in the `ajfx-studio-scene` format.
  Readers refuse unknown, duplicate, malformed or missing properties.
- `.ajfx` is the exported package, with precompiled kernels and the
  generated textures and meshes it uses.
- `.ajfxcache` is the baked simulation, beside the package it belongs to.

## Layout

| Path | Contents |
| --- | --- |
| `src/scene.*` | Typed scene owners, creation and editing operations, validation, live parameters |
| `src/scene_properties.*` | The one property registry: defaults, bounds, units, resets, storage and motion invalidation |
| `src/scene_io.*`, `src/document_io.*`, `src/json.*` | Saving and loading scenes |
| `src/scene_view.*` | The scene tree, the selected-item inspector and the Add menu |
| `src/document.hpp` | The document: the scene, its name and scale, and how the preview stands it |
| `src/controls.*` | The controls scene properties are edited with, and the values they hold |
| `src/page_revision.hpp` | Knowing the document has changed: one look a frame, then a number to ask |
| `src/compile.*` | The scene to `ajfx::Effect`: emitters, the solid world, budgets, scale, cuts, item reports |
| `src/compile_worker.*` | The compile, off the interface thread: newest wins, never an older one after a newer |
| `src/starter_scenes.*` | Whole scenes to start from, by name: what `new` makes, and the compiler's fixtures |
| `src/library.*` | Generated textures and meshes |
| `src/bake.*` | Baking and what a cache holds of the scene |
| `src/stage_pick.*` | The solid things in the preview at a step, and which one a click meets |
| `src/performance.*` | The performance as `ajfx::Action`s: drops, deletions, pushes, grabs, paint, blasts; editing and re-recording it |
| `src/hands.*` | The hands: the brush (paint, erase, push, pull), grab and throw, poke, blast; playing a scene headless |
| `src/kinds.*` | The palette's starter kinds and the effect templates attached to objects |
| `src/issues.*` | The issues list: validation, budgets, refused drops, unapplied acts, stale caches |
| `src/cli.cpp` | `ajfx-studio-cli`: the compiler, hands, the palette, issues and replays from the command line |
| `src/guides.*` | Guides and handles: where the selected item acts, and what a dragged handle sets in it |
| `src/package_builder.*` | The background worker: kernels, packages, the artist's files, CPU analysis |
| `src/user_assets.*` | The artist's files made package-ready: textures, meshes, baked collision fields, outlines |
| `src/session.*` | Where Studio was left, written on close and read on open |
| `src/preview.*` | The stage and the media server renderer |
| `src/stage_overlay.*` | Guides, handles and particle paths drawn over the preview, in its camera |
| `src/stage_camera.*` | The preview's camera, and dragging handles through it (no renderer, so the UI tests drive it) |
| `src/studio_app.*` | The window: top bar, viewport, transport, history, files |
| `tests/studio_tests.cpp`, `tests/<area>_tests.cpp`, `tests/test_support.hpp` | Headless tests of the scene and compiler (`ajfx.studio`) |
| `tests/imgui_tests.cpp` | The panels and the viewport on a headless Dear ImGui context with synthetic input (`ajfx.studio_imgui`) |
| `tests/frame_bench.cpp` | `ajfx_studio_frame_bench`: times what the application does on its interface thread each frame and per edit. Not a test |

`ajfx_studio_core` (the scene model and compiler) depends on the ajfx runtime
alone. The application adds the media server's renderer and window
(`MediaServer::Render`, `Gui` and `Editor`) and Dear ImGui, so it builds from the repository
root, beside the media server.

## The command line

`ajfx-studio-cli` acts on a scene's performance and replays it on the CPU
with no window, through the same calls the viewport uses. It builds with
ajfx's own presets (`../out/ajfx/<preset>/studio/ajfx-studio-cli.exe`).
Scenes may use the library's shapes; a scene naming a model file is refused,
since only the window reads those.

```powershell
ajfx-studio-cli new wall wall.ajfxstudio
```

```powershell
ajfx-studio-cli wall.ajfxstudio info
```

```powershell
ajfx-studio-cli wall.ajfxstudio do grab object:65 30 100 0,1.3,1.5 0,1.3,1.5 0,1.3,1.5 0,1.3,1.5 0,1.3,1.5 0,1.3,1.5 0,0.9,-0.4
```

```powershell
ajfx-studio-cli wall.ajfxstudio verify 200
```

`new <name>` writes a starter scene (`scenes` lists them: `logs`, a log kind
painted in that trails smoke; `wall`, a wall that breaks and a ball to throw
at it; `sandbox`; `campfire`; `explosion`; `smoke_room`; `rain`; `magic`;
`rock_pile`; `empty`). `compile [--json] [--live <system>]` prints what the
scene compiles to: each emitter with the items in it, its capacity and
modules, each item's report, the files it needs and the cuts it makes, and
the simulation digest; `--json` prints the effect itself.
`info` lists what can be acted on, `acts` the performance, `bodies <step>`
where the solid bodies stand, `play <step>` what stands and a digest of the
whole state, and `verify <step>` plays twice and once more after saving and
reopening, and fails unless all three agree bit for bit. `run <script>` does
a script's lines in order and `do` one line, writing the scene back (or to
`--out`). `--help` lists the lines: `drop`, `paint`, `stroke`, `erase`,
`push`, `pull`, `poke`, `blast`, `grab`, `delete`, `remove`, `retime`,
`rerecord` and `clear`; `grab ... hold=firm` holds the body firmly rather
than steering it.

The palette and the issues list are driven from it too:

```powershell
ajfx-studio-cli new sandbox sandbox.ajfxstudio
```

```powershell
ajfx-studio-cli sandbox.ajfxstudio wind 90 12 gust=0.3
```

```powershell
ajfx-studio-cli sandbox.ajfxstudio issues 360
```

`kinds` lists the starter kinds and effect templates; `new sandbox` stands
one of every starter kind on the stage. `add-kind <kind> [<point>]` adds a
starter kind with its effects, `attach <object> <template> [tint=r,g,b]`
attaches an effect to any object, `restyle <system> <template>` puts a
template back over an attached effect, `effects <object>` lists them, and
`wind <heading> <air m/s> [gust= strength=]` adds a wind that blows on
particles and light things alike. `issues [<step>] [--cache <file>]` prints
the issues list, played to the step (the loop by default), and with
`--cache` whether that cache still holds the scene.

Delivery is driven from the command line too:

```powershell
ajfx-studio-cli wall.ajfxstudio deliver wall.ajfx
```

```powershell
ajfx-studio-cli wall.ajfxstudio replay wall.ajfx 0 95 140 359
```

```powershell
ajfx-studio-cli wall.ajfxstudio gltf wall.gltf --cache wall.ajfxcache
```

`bake <out.ajfxcache>` bakes the scene's baked items with its performance
and `--seed` for its loop. `deliver <out.ajfx>` bakes (or takes `--cache`),
compiles the kernels and writes the package with its cache beside it, after
opening the cache against the written package as the media server will.
`replay <package> <step...>` plays a delivered package from its cache, seeking
straight to each step as a node does, and compares it with the scene played
live (bodies bit for bit; it fails otherwise, or when the host would refuse
the cache). `gltf <out.gltf>` writes the bodies as animated glTF with a
`.bin` beside it (`--frames` to shorten it). Writing a package needs the
shader compiler, so `deliver` is only in builds with the D3D12 backend.

## Building and testing

The headless core and `ajfx.studio` build with ajfx's own presets (see
[Building](../README.md#building)):

```powershell
ctest --preset windows-msvc-debug -R studio
```

The application needs the media server's renderer, so it is built from the
repository root by the GUI presets on Windows, which turn on
`AJFX_BUILD_STUDIO_APP`. From the root:

```powershell
cmake --build --preset windows-msvc-debug-gui --target ajfx-studio
```

```powershell
cmake --build --preset area-particles
```

```powershell
ctest --preset area-particles -R studio
```
