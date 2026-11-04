# Studio scene workspace

This is the current implementation reference. The full design and acceptance
criteria remain in [studio-ui-architecture.md](studio-ui-architecture.md).
The architecture is only partially delivered; the outstanding work is listed
in [next-steps-plan.md](../next-steps-plan.md).

## Creating and editing

Studio starts with a scene containing a named, static **Ground** object.
**New** creates the same default scene. Its 20 m square floor is 0.2 m thick,
with its top at Y = 0 to match the viewport grid. The
left tree contains Scene settings and named objects, particle systems and
organizational groups. The viewport occupies the center and one inspector
on the right edits the selection. Both panel splits are resizable and saved
in the workspace, along with selection, group expansion, camera and transport.

**Add** creates a complete cube object, a working particle source with motion
and appearance, or a group. Creating a group places the current selection in
it. **Import** accepts multiple OBJ, glTF and GLB files in one edit, retaining
separate named objects and asset assignments; each object is named after
its file until renamed, and a model picked for an object still named for
what it held before takes the new file's name. A saved scene keeps its
models' paths relative to the scene's folder, so the folder can be moved or
copied with them; saved to another folder, the paths are worked out again
from there. Decoding and fracture preparation
remain on the package worker. **Add > Floor** creates another Ground object,
including in an existing floorless scene. Ground owns a built-in thin mesh
and derived convex collider, with ordinary transform, surface and physics
controls. It can be moved, resized, renamed or deleted. Loading a saved scene
preserves exactly its authored items; a deleted floor is not reinserted.
The scene viewport draws the authored floor beneath the reference grid,
without a second solid staging plane hiding floor edits or removal.

A click in the viewport selects the solid object under the pointer, and the
selected item's handles move it and set its size or spawn radius; the drag
is written back to the item, never kept in the compiler's copy. In a narrow
inspector, vectors drop under their label and then take one axis to a row,
and the tree's Import and the inspector's Duplicate and Delete keep only
their icons.

### The stage and the Stage panel

An object whose Distribution is **Placed by hand** is a kind: it has no bodies
of its own, and every body of it is put on the stage or dropped. The stage is
what stands before the scene plays, and is kept as drops at step 0, so what an
artist puts down and what they drop later are one record and replay the same.
The **Stage panel** (Dear ImGui, floating over the viewport) holds the tools
that act in the viewport, **Select** (Esc), **Place** (O), **Drop** (D) and
**Delete** (X), and the setup tools that act on what stands on the stage:

- **Place** puts a copy of the selected kind where it stands on the floor or a
  thing, at step 0, whether or not the scene is playing.
- **Settle** lets everything on the stage fall and come to rest, with the
  scene's own piles and props too, on the builder's worker thread, and keeps
  each thing where it lands. Anything still moving after eight seconds of
  simulated time is left where it was put and counted. The result is kept only
  if the scene's performance is the one the settle began with.
- **Jitter** shifts every thing on the stage by up to Shift metres each way and
  tilts it by up to Tilt degrees about each axis, drawn from the Seed and the
  thing's own serial, so the same seed does the same and taking a thing away
  moves no other.
- **Reset** puts everything Settle or Jitter moved back where it was put.
- **Move** and **Turn** pick what the gizmo over the picked thing does (ImGuizmo):
  with Select, a click on a placed or dropped thing picks it (its marker on the
  timeline is picked too), and dragging the arrows or the middle moves it, or
  the rings turn it about its own axes. What is dragged to becomes where it was
  put, so Reset does not undo it. The gizmo has the pointer where it is, and
  the camera and picking beneath it do not.
- A model file (OBJ, glTF or GLB) let go on the viewport from the desktop
  becomes a kind placed by hand, with a first copy on the stage where it was let
  go, and the tool becomes Place so more copies are a click each.

A kind placed by hand has to be Dynamic, since it is dropped; a Static or
Animated one is diagnosed. Placements count against **Most dropped at once**
with the drops.

### Drop and Delete: the performance

Four tools top right of the viewport say what a click does: **Select**
(Esc), **Place** (O), **Drop** (D) and **Delete** (X). With Drop, a click drops a copy of
the selected Dynamic object with its bottom 0.75 m above where the click
lands on the floor or on a thing, turned as the object is; with Delete, a
click takes away the thing under it, one of an object's own bodies or a
dropped copy. Each is an act of the scene's **performance**: stamped with
the simulation step it happens in, saved with the scene, and replayed
exactly, since the runtime takes acts as inputs of its step
(`ajfx::Action`) rather than as a new package. While the scene plays, an act
is stamped just past the steps the preview's physics may have worked out
(two ahead, `act_tick`), so it shows about three frames after the click and
nothing is simulated again; paused, it is stamped at the step shown and
happens as the scene plays on. An act past the end of a looping range is
refused, as it would never play.

Copies are born in the solid world, each of its object's body kind with the
act's serial as its spawn count, so a copy breaks, sheds trails and is hit
as the object is, and taking an earlier drop away leaves every later copy's
variation as it was. Scene settings > Simulation sets **Most dropped at
once** (64 by default, 0 for none): the world's room for copies is reserved
up front, and a drop past it is refused and said, never made room for.
Scene settings > Performance counts the drops and deletions and clears them.
Acts on the timeline are diamonds on the object's track, green for a drop
and red for a deletion; dragging one moves it, and a click picks it so
Delete takes it away (a drop takes its deletions with it). Undo covers
acts as it covers edits. Removing an object removes its acts.

### Hands: paint, grab, poke and blast

The performance also holds what hands do to the running scene. These are
implemented in the core (`studio/src/hands.*`, `performance.*`) and driven
from the command line (`ajfx-studio-cli`, see the
[Studio README](../studio/README.md#the-command-line)); the viewport has no
buttons for them yet.

- **Paint** lays copies of a Dynamic object down with a brush: each dab
  puts `count` copies (1 to 16, what the world bears in a step) over a disc
  of the brush's radius across the surface touched, gathered to the middle
  by its falloff, starting half their size and a little lift above it. Each
  copy has a serial of its own (a dab holds `count` in a row), and where it
  lands and how it is turned are drawn from that serial alone: *Realistic*
  stands it up along the surface and turns it about it, *Random* turns it
  any way, *Locked* keeps the object's turn. A dab with no room left in its
  step goes in the next step that has room; one past Most dropped at once is
  refused. One stroke is many dabs, named by its first dab's serial.
- **Erase**, **Push** and **Pull** are the brush on what is already there:
  every body whose middle is under it at that step is taken away, or pushed
  out from the brush's middle (drawn in, for Pull) along the surface at the
  brush's strength, less towards the edge by its falloff. They are recorded
  as deletions and pushes of those bodies.
- **Grab** takes hold of a body where the pointer pressed it and carries it
  across a plane facing the camera through that point, deeper or nearer
  with the wheel. It is one act holding the point the body's middle is
  steered to at every step it is held (`ajfx::ActionKind::Steer`, 10 per
  second gain, 20 m/s at most); between samples it is steered along the
  line between them. Letting go is the last sample: the body flies on with
  the speed the hand gave it, so a swing throws it.
- **Poke** pushes the body under the pointer along the pointer's way; off its
  middle the push spins it as it would a ball.
- **Blast** throws every body within its reach out from where it is set off,
  its power at the middle falling to nothing at the edge, and breaks first
  the ones thrown at least as hard as their impact threshold (an object's
  break sets its blast threshold to its impact threshold). Loose props are
  thrown too (`ajfx::ActionKind::Blast`).
- **Re-record from here** (`rerecord_from`) takes away every act from a step
  on and lets go of any grab still holding then; nothing before it changes,
  serials included.

On the timeline a grab is a bar on its body's object's track over the steps
it held, a stroke a bar from its first dab to its last on the painted
object's track, a push a diamond there, and blasts are diamonds on a
**Blasts** track after the rest. Moving a marker (`set_marker_time`) moves
its act whole (a stroke's dabs keep their spacing), and taking it away
(`remove_marker_act`) takes a stroke whole; a dab takes the acts on what it
made with it. The timeline panel draws these markers, but dragging the new
bars there has not been checked in the window yet. A hand that goes on holding
replays nothing: the performance differs only from the step after its last
sample (`first_difference`).

Ctrl-click tree rows to extend a selection. Common inspector values apply to
the selection in one undoable gesture; `*` marks mixed values. Each property
has its own reset. The Group control changes organization, never transform
parenting. Object and system names, tree order and grouping do not set runtime
identities or solver precedence.

An object owns its model, uniform longest-side size, color, collider,
density, friction, restitution, initial velocity, release time and fracture.
Static, Dynamic and Animated have their usual collision meanings. Animated
uses the runtime's supported single move, with start, duration and target
pose. Collider choices are explicit convex hull, sphere or box. Distribution
creates a Single object, one Grid/Pile instance set, or a kind placed by hand
(see the stage above). A Single object of
any motion starts with its middle at its Position, where its handle is.
Repeated Static and
Animated objects, and fracture on either, are diagnosed as unsupported.

A particle system owns its sources, launch direction and speed, gravity
share, a Motion stack, appearance and object-collision switch. Its
Appearance section also says how the scene's light reaches it: **Light** is
"Glows on its own" (the colour as authored, for sparks and fire) or "Lit by
the scene" (the preview's lights and sun shadows fall on it, as a rounded
puff), with **Rounded** (flat to a ball), **Light wraps** (light reaching
round the dark side), **Takes ambient** and **Takes shadows**. The scene
settings choose the preview **Backdrop**: Studio (a lit stage), Night (a dim
moonlit rig, for glowing effects) or Void (no light at all), which is the
quickest way to see lit and glowing systems differ. **Casts light** has
the particles give off light in their own colour, fading in and out with
their life, onto the scene and its smoke: **Lights** is the most that carry
one at once, and each light's **Strength** and **Reach** are ranges. A
campfire under the Night backdrop lights its own smoke this way. Radius,
lifetime, speed, size, brightness and spin are **ranges**: two values, and
each particle picks its own between them once, at birth. A range whose two
values are equal is a constant. The system's seed, and each behavior's seed,
choose another set of picks; seeds are explicit, undoable and never change as
a side effect of another edit. Sources have stable
identities and are Initial, Continuous or Burst; the inspector numbers them
in order and says what each gives ("Source 1: 40 a second"). Additional sources are
added inside the inspector. A bounded Initial/Burst population can be
persistent; an unbounded Continuous source cannot. Source Initial currently
uses the runtime's time-zero burst and becomes populated on the first step.
It is not yet an arbitrary saved initial-state checkpoint.

The Motion stack is an ordered list of typed behaviors, each with a stable
identity, an on/off switch, a seed and its own controls: **Drag** (amount),
**Turbulence** (amount, swirl size, churn), **Vortex** (strength, axis,
center), **Wind** (strength, heading), **Attract** (strength, point, reach)
and **Draw in** (pull, fade height, center). A behavior's strength is a
range: each particle keeps its own share for life, so some sparks drag more
and some smoke swirls harder. The seed picks another set of shares and, for
Turbulence, another noise field. **Add behavior** appends one with that
kind's defaults; a new system has an empty stack. The stack's authored order is the order the runtime
applies the forces, so moving a row up or down is a motion edit. A switched-off
behavior keeps its settings and lowers to nothing. Up to 16 behaviors per
system. Behaviors have no viewport handles yet; a vortex center is typed.

**Size over age** and **Color over age** are each **Constant** or **Keys**:
between 2 and 16 keys followed from birth (age 0) to death (age 1). Size keys
multiply the base size; color keys multiply the particle colour, alpha
included, so a fade-out is a key with alpha 0. Keys stay in age order: an
edited age is held between its neighbours, and **Add key** inserts one before
the last. A strip above the keys previews the curve, birth on the left. The
keys lower to the runtime's size-over-life and colour-over-life modules
exactly as written. Size over age is a motion edit (collision reads size);
colour over age keeps the motion cache. Drivers from other particle
attributes, such as speed, need runtime support and are not offered.

A **force** is a scene item in its own right: **Wind** (strength, heading),
**Vortex** (position, strength, axis), **Attract** (position, strength,
reach), **Draw in** (position, strength, fade height) and **Blast**
(position, power, radius, time). **Add** lists them under Forces. A force
acts on every particle system unless **Every particle system** is switched
off, when a switch per system names its targets; a system that is removed
drops out of every force. A blast throws everything, objects included, and
takes no target list. Selecting a force shows its guide and handles in the
viewport, drawn in the first system it acts on; the drag writes back to the
force. The force's seed is the item's seed. Each force lowers into the
systems it acts on by identity order, at most 16 per system, so tree order,
names and other systems never move it. Forces have no viewport handle for a
strength, and Wind has no position.

A **smoke grid** is a scene item too: a box of air that particle systems
fill with smoke and ride, the runtime's grid volume. **Add** lists it under
Smoke. It has a position (the centre of the box) and a size in metres, and
is cut into cells of **Cell size**; the inspector says what that comes to
("20 x 30 x 20 cells, 12,000 of 1,048,576"), in the caution colour once
past what one grid may hold (at most 256 cells along a side and 1,048,576 in
all), and the scene is refused for playback until the cell size is raised or
the box shrunk; saving keeps it. The air's settings are **Rises** (lift per
unit of heat), **Sinks** (fall per unit of smoke), **Swirl** (sharper eddies;
0 is off), **Fades** (smoke and heat lost each second), **Calms** (motion
lost each second) and **Solve quality** (pressure iterations); its look is
**Thickness** (light a unit of smoke takes), **Color** and **Brightness** (the
share scattered back), which the simulation never reads, so they keep a
cache. Selecting a grid draws its box in the viewport, with a corner handle
for its size and a centre handle for its position. A grid starts empty: a
particle system fills it through a relationship (below). The preview draws
the smoke with the media server's own volume pass; export writes the volume
in the `.ajfx` package. At most 16 grids in a scene. The HUD and
Statistics count them ("Smoke: 1 grid, 12,000 cells, 2 MiB on the GPU") and
name the limit once a grid is four fifths of the way to it. A system that
feeds smoke is never cached (a cache keeps particles, not what they deposit):
the bake takes the rest and the HUD says how many systems feeding smoke stay
live, rather than failing.

Particles with **Collide with objects** on meet loose (Dynamic) objects
through the shared world, and each Static object as its own shape, turned
as it stands: the floor as a plane at its top, a Sphere collider as a
sphere, an imported model with a Convex hull collider as its own surface
(a distance field of the model, fitted and turned as it is drawn), a
library shape as the box of its own bounds, and an imported model with a
Box collider as the box of its longest side. Animated objects are not met
by particles yet. Switching the
collision off lets particles pass through everything.

A **relationship** is one rule on the scene binding a particle system to
an object (or every object), to another particle system or to a smoke
grid, stored once and shown from either endpoint and under Scene settings:
`Crate / Hit -> Dust / Emit` has particles born where the object hits
something (hits from, per hit, per m/s, spread), `Crate / Moving -> Smoke /
Trail` sheds them behind it while it moves (per m/s, moving from, base
amount, broken pieces only), and `Embers / Moving -> Smoke / Trail` sheds
them behind another system's particles while those move (per m/s, moving
from, base amount), each puff born along the step's stretch of the
particle's path. Each of those rules has a seed. `Embers / Feeds -> Smoke
grid` has the system's particles leave smoke, heat and their own motion in
the grid's air as they pass through it (smoke and heat per second, push: the
share of their velocity), and `Ash / Rides -> Smoke grid` has them carried
by the air while inside it (strength: how quickly they take up its motion).
A system may feed and ride the same grid, once each; a system that only
rides smoke still needs a source. **Add relationship** on a particle system
offers the Dynamic objects, the rest listed but not takeable, under **Trail
from system** the other particle systems, and under **Feed smoke** and
**Ride smoke** the scene's grids; on a Dynamic object it offers the particle
systems, and an object that is not Dynamic has no button; on a smoke grid it
offers the particle systems under both smoke kinds. A system fed by a hit
or trail rule needs no source of its own, and its last source can be
removed; a persistent population cannot be fed by one. A Dynamic object's inspector
offers **New system born of its hits** and **New system trailing it**, and a
particle system's **New system trailing its particles**: one edit that
creates a particle system with no source and the rule that feeds it, so
nothing plays until the source hits or moves. Hits and trails come from
Dynamic objects: a rule on a Static or Animated object, or on every object
when none is Dynamic, is marked as born nothing, as is a trail from a system
that is switched off. A system cannot trail itself, and systems trailing one
another round in a ring are refused: the system trailed steps first, so the
puffs sit where this frame left its particles. Removing either endpoint
removes the rule rather than re-aiming it at everything, and a rule whose
grid is gone is diagnosed. Rules lower into the system in identity order
and reach the runtime's contact, body and particle spawners and its grid
modules. Fracture, time and threshold events are not relationships yet.

At the foot of the viewport a **HUD** says what the frame costs and what
the scene is: GPU time and the cache's state, particles (the most alive at
once through the loop when the CPU traced it, and the room reserved),
bodies (room, things standing, kinds of 256), particle state in KiB, smoke
(grids, cells and GPU memory) and the item counts. A limit is named in the
caution colour once four fifths of it is used: body kinds, forces on one
system, blasts, relationships, bodies, smoke grids and cells in one grid. The same lines appear under Scene settings as Statistics. Nothing
is thinned or dropped to stay inside a limit; the number is there so the
artist can decide.

Under the transport, the **timeline** shows a track for every item with
time in it and nothing for the rest: an object's release and, when
Animated, its move as a bar from its start for its duration; a particle
system's sources, Initial at 0, Continuous as a bar across the range, Burst
at its time; a blast at the moment it goes off; its object's drops,
deletions, pushes, grabs and paint strokes, and blasts set off by hand on a
track of their own (see the performance and hands above). Points are diamonds and
spans bars, lined up under the scrub bar with the playhead running through
them. Dragging a release, burst, blast or move writes that one authored
value, and dragging a move's end sets its duration; Initial and Continuous
cannot be dragged. Clicking a track's name selects its item. Six tracks show
before the list scrolls. There are no keyframe tracks for other values.

Scene settings own gravity, angular damping and sleep policy. The fixed
simulation step is 60 Hz. The capture range controls bake/export duration;
non-looping scene playback can continue beyond it. Looping is an explicit
transport choice. The current editing policy is **Initial setup**: a motion
edit keeps the playhead and playback and simulates again from the initial
setup up to the playhead, while visual edits preserve motion-cache validity.
An act replays from the latest checkpoint at or before its step, so moving
or taking one away costs the steps from there to the playhead.

## Ownership, determinism and delivery

The authored scene is `Document::scene`. `scene.hpp` defines typed owners;
`scene_properties.cpp` defines property metadata, defaults, bounds, units,
resets, storage and motion invalidation. New scene files use
`ajfx-studio-scene`, schema 2 (schema 1 lacked the Motion stack and drivers
and is refused). The performance is the root key `performance`
(`next_serial` and `acts`, each a drop, delete, push, grab, paint or blast
with its serial and step; a drop that Settle or Jitter moved also keeps the
`home` and `home_turn` it was put at; a grab keeps its `path` as
`[step, x, y, z]` samples; a dab of paint holds the serials from its own to
its own plus `count`); a scene saved without one opens with none, and acts
out of order, with serials that overlap or are not yet handed out, a grab
whose path goes back in time or a dab of more than a step bears are
refused. Readers refuse unknown, duplicate, malformed
or missing authored properties and unsupported versions. Window state is not
part of this schema. Existing layer documents retain their existing format.

`lower_scene` produces transient compiler IR; it is never another authored
copy. Items reserve component identities at creation, and lowering sorts by
UID, including sources. Fracture is emitted beside its typed owner, so tree
neighbors cannot capture it. A smoke grid lowers to a layer of one Smoke
grid block, which the compiler lists on the effect as a grid volume named
from the block (`page_volumes`) and builds no emitter from; its bindings
lower to the runtime's `VolumeSource` and `VolumeAdvect` modules naming that
volume, and the scene's scale applies to the grid's place and cells. A grid
the runtime refuses is reported against the grid item, with the runtime's
reason. Local body material values are explicit; shared
angular damping and sleep settings always come from ScenePhysics.

Rigid objects collide in one solver world. Per-kind draw tint and mesh
assignments feed render batches from that same simulation. Static and Animated
props also retain their own tint. Cut fragments inherit their parent's tint
and physical surface. Tint is serialized in effect format 5 and included in
content identity, but excluded from motion-cache identity. A tint edit can
reuse a verified cache; a collider edit invalidates it. The additional
body-kind budget is 256, with bounded scopes and emitter capacities retained.
Exceeding the scene's compiled capacity blocks its entire replacement preview
and export instead of publishing a partial scene.

Bake covers the selected capture range. Scene export requires a matching,
current cache for every baked unit and refuses missing assets and unsupported
state. The cache is written successfully before publishing the package. A bake
replays the performance and is keyed by it (`CacheIdentity`'s action
digest): a cache opens only with the acts it was baked with, and a drop or
deletion since makes it stale.
The current viewport still simulates: a green cache band means the cache is
available, not that the viewport is reading it. Cache-backed preview and host
cache adoption still need their own end-to-end integration.

## Remaining boundaries

Studio opens only scenes. The old page editor (palette, block pages, coach,
budget search and recipe gallery) has been removed without a conversion step,
since no projects needed migrating. Its layers and blocks remain as the
compiler's IR, and its recipes as compiler test fixtures.

Continuous cross-population attraction/repulsion, event endpoints beyond
collision and motion, light scene items, reusable assemblies, library
organization, authored checkpoints, viewport tools for the hands (paint,
grab, poke and blast exist in the core and the command line only),
simulation-independent
visibility/isolation, controls publishing and the full export review panel
are not implemented in the scene workspace. Flow coupling is a smoke grid
fed and ridden by systems; the smoke casts no shadow on the particles and
takes none from them, it is not met by objects or their bodies, and the
grid's detail is one cell size rather than a choice. There are no decorative
controls claiming these capabilities.

The scene acceptance tests exercise strict save/load, independent surfaces in
one colliding world, package round trips, tint/collider cache invalidation,
order-independent reruns and seeks, bounded persistent populations, and ten
distinct generated mesh inputs with fracture beyond the former 64-kind limit.
The host renderer additionally checks surface color in rendered pixels. The
two sandbox scenes are recorded, saved, reopened and replayed bit for bit
headless (`hands_tests.cpp`): logs painted in from a metre up that tumble and
trail smoke, and a ball picked up, swung and let go so a wall breaks. That
is not yet proof of them in the viewport, nor a timed usability comparison.
