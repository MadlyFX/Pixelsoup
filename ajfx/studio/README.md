# ajfx Studio

The editor for [ajfx](../README.md) particle effects. It exports `.ajfx`
packages that a host, today the media server, places, plays and animates.

Studio is a block editor, like a page in Notion, rather than a node graph or a
stack of modules. Each part of an effect is a **layer**: Flames, Embers or
Smoke. Each layer is a short list of **blocks**, each one a plain statement:

- *Stream* 70 a second
- *Spawn shape*: disc, 0.22 m
- *Rise* 3.5 m/s²
- *Color*: burns out, brightness 5
- *Fade*: in 8%, out 35%

A closed block reads as one line, so another artist can read a page from top
to bottom. Open a block to see its controls.

## The palette

Down the left, the **palette** shows everything a page is built from:
ready-made layers, every block by what it is for (Birth, Motion, Look,
Contact, Light), and the raw modules under the hood. Seeing what there is
gives ideas.
- **Drag** an entry onto the page: a line shows where it will land, between
  blocks of a layer or between layers. A block a layer may have once
  (Appearance, Tumble...) only lands in a layer without one.
- **Click** an entry to add it to the end of the layer being worked on (a
  layer goes to the end of the page).
- **Search** matches titles, descriptions and keywords. Headings fold.
- The minus button folds the palette to a rail; the plus brings it back.

## Why blocks

Node and module editors ask the artist to know the runtime: which stage a
module runs in, what order forces apply in, what capacity to reserve, which
blend mode suits smoke. Studio moves those decisions into its compiler:

- **Order is for reading.** Every block's modules go where the runtime needs
  them: spawn, init, forces, then turning and collisions, then appearance over
  life. Reordering blocks on a page never changes the effect.
- **Blocks that shape one thing are merged.** Color's "burns out" and Fade's
  fade-out become one colour-over-life curve. Size's growth becomes one
  size-over-life curve.
- **Budgets are worked out.** A layer's capacity is the most particles it can
  ever have alive, rounded up. Nobody sets a budget, and none is exceeded.
- **Edits stay local.** Module ids and random seeds come from each block's
  identity. Adding or moving one block leaves every other block's particles
  where they were.
- **Page scale.** Size on the page scales every distance, size, speed and
  force together. An effect gets bigger and keeps its look.
- **Materials, not blend modes.** Appearance offers Glow, Smoke, Solid, Debris
  and Trail. Each sets the blend mode, depth sorting, soft edges and geometry
  that suit it.
- **A coach.** Notes appear under the layer or block they concern, each with
  a one-click fix:
  - particles that pop at the end of their life: add a fade;
  - smoke slicing into the floor: soften its edges;
  - glow too dim to bloom: brighten it;
  - sparks falling through the ground: add ground;
  - tumbling without debris: make it debris;
  - particles living longer than the preview loop;
  - layers too heavy for a live show.

## Fire that moves like fire

Four blocks give flames the way real fire moves. The Campfire, Explosion and
smoke recipes use them:
- **Turbulence**, under More settings:
  - *Rolls upward* moves the swirls up with the plume, so particles ride an
    eddy and curl.
  - *Over its life* can be calm at the base and break up towards the tip.
- **Rise**, *Over its life*: *Eases as it cools* lets flames shoot up and
  slow at the tip.
- **Draw in** pulls particles towards the middle near the base, as a fire
  draws in air, so flames gather into tongues.
- **Spin** turns each particle's picture as it moves, slowing as it goes.

## Ranges and seeds

**Ranges.** Wherever a value can differ from particle to particle, its
control is a range, low to high, and each particle picks its own:
- the spawn shape's radius (equal ends a crisp edge, apart a soft edge or a
  thick shell);
- launch speed and spread (raising the low end of spread hollows the cone);
- brightness, size, spin and tumble;
- the strength of every force: gravity, lift, wind, turbulence, air
  resistance, swirl, draw in and attract. A particle keeps its share for
  life, so some sparks fall heavier and some smoke drifts further.

Equal ends behave exactly as one number did. A page saved when a control
was one number reads it as both ends; a spread reads as 0 to that angle.

**Seeds.** Every block that deals random values has a **Seed** under More
settings: another number deals its particles new values, the same settings
with another look, and changes that block alone. A layer's **Seed**, on its
"Each lives" row, deals the whole layer afresh. 0 is the block's or layer's
own. Seeds never recompile kernels, so trying one is instant.

## Guides, handles and paths

**The layer being worked on.** Guides, handles and paths follow what is
being worked on, and nothing else, so the stage stays readable. The working
layer is the one whose block was last opened or changed, or whose heading
was last clicked; an accent mark in the margin beside its heading shows
it. Esc, or folding the layer, leaves nothing worked on, and the preview
draws no guides or paths.

**Guides.** The preview draws where the working layer's blocks act, in the
preview's own camera:
- its spawn shapes at both ends of their radius, and its colliders (a mesh
  collider as its model's outline), faintly;
- for the block being worked on (the one last opened or changed), in the
  accent: its launch cone, arrows for gravity, lift and wind, the swirl's
  axis and turn, draw in's pull and fade height, an attractor's reach, the
  ground's height, and turbulence's flow as streamlines that drift and churn
  with the field as the playhead moves.

**Handles.** The block being worked on carries handles to drag:
- dots move positions across the floor; hold Shift to raise or lower them;
- diamonds set a radius (each end of a range), a reach, a height, a box's
  size, or the wind's heading.

Every drag writes the block's control, so the page, undo and the preview
follow it.

**On and off.** Guides (G), handles (H) and paths (P) each have a button over
the preview, and stay as they were left.

**Paths.** Effects are deterministic, so Studio can show the whole flight of
some particles at once: the worker runs the effect on the CPU reference
with the preview's own seed and traces up to 160 particles a layer, spread
evenly through the loop's births. Only the working layer's paths are drawn. Each path is drawn in its particle's
colour as it ages, with a dot where the particle is at the playhead. It
shows the shape of a burst of sparks or a throw of debris in one picture,
for dialling in speed, spread, gravity and drag. Effects too large for the
CPU reference (over 200,000 particles) show no paths.

## Solid pieces and piles

**Physics** (Contact) makes a layer's particles solid bodies in a physics
engine: they knock into each other, stack, tumble off and come to rest.
- *Body* is what each piece collides as: *Its own shape* wraps a debris
  chunk tightly, *Ball* rolls, *Cube* stacks.
- *Friction* and *Bounciness* set the material; *Density*, *Spin damping*
  and *Rest when still* are under More settings.
- Gravity, Air resistance and Ground, sphere and box colliders act on them.
  Other forces, mesh colliders and Vanish on contact cannot move an engine
  body, and the layer says so on the block at fault.
- With Physics, Tumble only sets each piece's spin and how it lies; the
  engine does the rest.

**Pile** (Birth) puts pieces in place before the effect starts: a heap of
rocks that collapses when the effect plays, or a stack that stands until
something knocks it down.
- *Stacked as* a Heap (a cone whose sides slope at *Slope*) or a Stack
  (filling *Width* by *Depth*, layer by layer).
- *Let go at* holds the pile still until then; 0 lets it fall as soon as
  the effect plays. A paused preview at the start shows it standing.
- Under More settings: *Position*, *Spacing* (how far apart pieces start,
  as a share of the largest piece's reach) and *Untidiness*.
- A Pile without a Physics block uses the default physics.
- A pile's pieces live on after they fall, so the automatic loop covers
  the fall and a few seconds of settling, not their whole life.

The **Rock pile** recipe is the starting point: 220 rocks heaped steeper
than they can stand, which slump as the effect plays, and a low puff of
dust as they come down.

## Your own pictures and models

- **Sprites.** Appearance's Texture offers *From a file*: a PNG, JPEG, TGA or
  BMP. *Frames across* and *Frames down* make it a flipbook played over each
  particle's life.
- **Debris.** With the Debris material, Chunk offers *From a file*: an OBJ,
  glTF or GLB, centred and fitted to a 1 m box, so Size sets how big each
  piece is. Its own normals and UVs are kept.
- **Mesh collider** (Contact): particles bounce off a model of your own, a
  table, rocks or terrain. Only its shape is used: Studio bakes it into a
  signed distance field, which is what the export carries; the model itself
  is never in the package. *Treat as* is Solid (its inside is inside), Thin
  surface (a floor or open shape, kept off both sides) or Automatic, solid
  when the model is closed. *Detail* sets how closely the field follows it.

Pick a file with its button, or drop one on it from the desktop. A page
keeps the path; a file not found there is looked for beside the page. The
worker reads and prepares files off the interface thread and keeps them
while they are unchanged. A file that cannot be read is named in the top
bar, and the preview plays without it (a soft dot, chips, nothing to
collide with); an export waits until it can be read. Exports carry the
pictures, meshes and fields, so the media server needs none of the files.

## Starting points

**New** opens the recipes: Campfire, Spark fountain, Explosion, Magic orb,
Smoke plume, Snowfall, Rain, Rock pile and a blank page. **Add layer** offers
ready-made layers: Flames, Embers, Smoke, Sparks, Fireball, Debris, Dust,
Motes, Rock pile and others. These are ordinary blocks with tuned values, so everything
they do can be seen and changed.

## Under the hood

The **Details** button in the top bar shows what each block compiles to: the
ajfx modules with all their values, the renderer settings and each layer's
budget in bytes.

The **Module** block, under "Under the hood" in the block picker, holds one
raw ajfx module with every field editable. It covers colour and size keys,
parameter bindings and anything the artist blocks do not. A block's menu can
**Detach into modules**: the block is replaced by the modules it compiled
to, and the picture does not change. Module blocks are taken literally, so
page scale does not apply to them.

Detaching can be undone at any time: each Module block remembers the block it
came from (its summary says "from Rise"), and its menu offers **Recombine
into Rise**. That puts the block back where its first module stands, with its
values, seed and publications, and its own uid, so its particles are the
very ones it had. When a module has been edited, disabled or deleted since,
the menu says the edits will be dropped.

## Publishing to the media server

**What can be published.** Controls that map onto one runtime input carry a
link button:
- amount, spawn radius, speed and spread;
- turbulence, drag, swirl and attract strengths;
- colour and size;
- bounciness and friction;
- tumble spin and roundness.

**Publishing a control.** Press the link and the control is published under a
name, `<layer>_<control>` until you change it, with a range of values. The
media server's Inspector shows it on every placement, where operators set and
key it.

**Budget.** The budget is worked out for the whole range, so no value an
operator picks can overrun it.

**Kinds of control.**
- A range control (radius, speed, spread, size, spin, a force's strength)
  publishes two parameters, `name_min` and `name_max`.
- A colour publishes the whole starting colour, with brightness and opacity
  in it.
- Two controls given the same name share one parameter, so one value can
  drive several layers.

**Trying values.** "Published for the media server", under the page's
settings, lists what the effect publishes. Values tried there change only
the preview, through the same per-placement values the Inspector sets.

**Detaching.** Detaching a published block keeps its values and drops the
publication. Recombining puts the publication back.

## The preview

The preview is drawn by the media server's own renderer:
- the Advanced D3D12 scene renderer;
- its particle pass, with depth sorting, soft particles, particle lights and
  motion vectors;
- temporal antialiasing, bloom and the display transform.

So an effect looks as it will on the wall.

The stage shows the effect on a floor ruled in metres, with a 1.8 m figure
for scale. The backdrop is Studio (a lit grey stage), Night (dim, for
glowing effects) or Void (black, no floor).

| Input | Action |
| --- | --- |
| Left drag | Orbit |
| Middle drag | Pan |
| Right drag, wheel | Move in and out |
| Double-click, F | Frame the effect |
| Drag a handle | Set what it marks; Shift raises or lowers a position |
| G | Guides on or off |
| H | Handles on or off |
| P | Particle paths on or off |
| Esc | Work on nothing: guides and paths step back |
| Space | Play or pause |
| L | Loop, or play once and stop |
| Home | Back to the start |
| Left, Right | Step a frame |
| / | Add a block to the layer last worked on |
| Ctrl+Z, Ctrl+Y | Undo, redo |
| Ctrl+S | Save |

For checking the interface without a mouse, `ajfx-studio` takes start-up
flags:
- `--recipe <id>` starts from that recipe;
- `--expert` shows what each block compiles to;
- `--open-blocks <kind|all>` opens those blocks;
- `--picker` opens the block picker;
- `--at <seconds>` starts paused at that time;
- `--once` plays once and stops instead of looping;
- `--try <name=value>` tries a value for a published parameter;
- `--paths` starts with particle paths shown, `--no-guides` with guides
  and handles hidden;
- `--fresh` starts without taking up where Studio was left;
- `--gpu-debug` turns on the D3D12 validation layer, for chasing a lost GPU
  device. Run under a debugger (cdb, Visual Studio) to see what it reports;
- `--lose-device <seconds>` has D3D12 remove Studio's own GPU device after
  that long, as a fault or driver reset would.

**A lost GPU device.** A GPU fault, hang or driver reset removes the
device, and nothing can be drawn again. Studio notices when the next frame
cannot be shown, says why in a system dialog, offers to save the page if it
has unsaved changes, and closes. It does not sit frozen on its last frame.

**How an edit reaches the preview.** A background worker turns each edit
into a package:
1. It compiles the page.
2. It compiles the kernels with DXC, cached by their source hash, so a
   changed value never waits for the shader compiler.
3. It rewrites the preview package in place.

The renderer adopts the package at the next asset generation. Studio raises
the renderer's particle step budget (`RendererLimits::particles`), so after
an edit the effect catches up with the playhead in one frame.

**While a value is dragged.** Each package restarts the effect and replays it
to the playhead, so a drag sends a new one at most every 0.12 s, and only
once the worker is idle and the preview has drawn the last for a few frames
(or 0.75 s has passed). Letting go always sends the final value. The
display transform is not keyed to the package's generation, so an edit does
not rebuild its lookup table.

**The loop.** The loop at the top of the page is automatic unless set: as
long as the effect takes to play out, worked out from the compiled
emitters. A burst plays until its last particle dies (its last repeat, at
the top of a published life's range); a stream long enough to build up and
show a whole generation at its full (twice its longest life). It is read
from the compiled effect alone, never from a run of it, so nothing depends
on itself. Typing or dragging a length sets your own; *Auto* gives it back.

**Framing, budgets and paths.** The worker also runs each new effect on the
CPU reference, with the preview placement's own instance seed. That shows
where its particles go, for framing the camera, the most alive at once in
each layer, shown beside the layer's budget, and the particle paths.

## Where Studio was left

Studio writes where it was when it closes (`%APPDATA%/ajfx Studio/session.json`)
and takes up there when it opens:
- the page that was open, and its changes if they were not saved (a page
  never saved comes back whole);
- how each recently open page was laid out: folded layers, open blocks,
  *More settings*, the scroll position and what was being worked on;
- the window's size, the palette and its folded headings, the split
  between page and preview, the overlays, the camera, the playhead and
  looping.

Changes discarded when closing are not kept. A page given at start, or
`--recipe`, replaces the page; `--fresh` forgets the lot.

## Files

- `.ajfxstudio` is the page, as JSON. It keeps only values that differ from
  a block's defaults, so it stays readable and picks up improved defaults.
- `.ajfx` is the exported package, with precompiled kernels and the
  generated textures and meshes it uses.

## Layout

| Path | Contents |
| --- | --- |
| `src/document.*` | Pages, layers, blocks and editing operations |
| `src/catalog.*` | The block catalog: controls, defaults, summaries |
| `src/compile.*` | Page to `ajfx::Effect`: placement, merging, budgets, scale |
| `src/raw_modules.*` | Generic access to every ajfx module, through its `fields` list |
| `src/library.*` | Generated textures (glow, spark, flame and smoke flipbooks...) and meshes |
| `src/recipes.*` | Recipes and ready-made layers |
| `src/coach.*` | The coach's notes and fixes |
| `src/guides.*` | Guides and handles: where blocks act, in the effect's space, and what a dragged handle sets |
| `src/document_io.*`, `src/json.*` | Saving and loading pages |
| `src/package_builder.*` | The background worker: kernels, packages, the artist's files, CPU analysis |
| `src/user_assets.*` | The artist's files made package-ready: sprite textures, debris meshes, baked collision fields, outlines |
| `src/session.*` | Where Studio was left, written on close and read on open |
| `src/palette.*` | The palette of layers, blocks and modules, dragged onto the page |
| `src/preview.*` | The stage and the media server renderer |
| `src/stage_overlay.*` | Guides, handles and particle paths drawn over the preview, in its camera |
| `src/page_view.*`, `src/widgets.*` | The block page and Studio's controls |
| `src/studio_app.*` | The window: top bar, preview, transport, history, files |
| `tests/studio_tests.cpp` | Headless tests of the page model and compiler (`ajfx.studio`) |

`ajfx_studio_core` (the page model, compiler, recipes and coach) depends on
the ajfx runtime alone. The application adds the media server's renderer and
UI toolkit (`MediaServer::Render`, `Gui` and `Editor`), so it builds from the
repository root, beside the media server.

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

## Not yet

- Textures on imported debris models.
- Per-block thumbnails and a timeline of parameter keys.

The [roadmap](../docs/roadmap.md) has the rest.
