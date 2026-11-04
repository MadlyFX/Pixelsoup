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
separate named objects and asset assignments. Decoding and fracture preparation
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
creates a Single object or one Grid/Pile instance set. A Single object of
any motion starts with its middle at its Position, where its handle is.
Repeated Static and
Animated objects, and fracture on either, are diagnosed as unsupported.

A particle system owns its sources, launch direction and speed, gravity
share, a Motion stack, appearance and object-collision switch. Radius,
lifetime, speed, size, brightness and spin are **ranges**: two values, and
each particle picks its own between them once, at birth. A range whose two
values are equal is a constant. The system's seed, and each behavior's seed,
choose another set of picks; seeds are explicit, undoable and never change as
a side effect of another edit. Sources have stable
identities and are Initial, Continuous or Burst. Additional sources are
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

Particles with **Collide with objects** on meet loose (Dynamic) objects
through the shared world, and each Static object as its own shape: the
floor as a plane at its top, a Sphere collider as a sphere, and a Box or
Convex hull collider as the box of the object's longest side, without its
rotation. Animated objects are not met by particles yet. Switching the
collision off lets particles pass through everything.

A **relationship** is one rule on the scene feeding a particle system from
an object (or every object) or from another particle system, stored once
and shown from either endpoint and under Scene settings: `Crate / Hit ->
Dust / Emit` has particles born where the object hits something (hits from,
per hit, per m/s, spread), `Crate / Moving -> Smoke / Trail` sheds them
behind it while it moves (per m/s, moving from, base amount, broken pieces
only), and `Embers / Moving -> Smoke / Trail` sheds them behind another
system's particles while those move (per m/s, moving from, base amount),
each puff born along the step's stretch of the particle's path. Each rule
has a seed. **Add relationship** on a particle system offers the Dynamic
objects, the rest listed but not takeable, and under **Trail from system**
the other particle systems; on a Dynamic object it offers the particle
systems, and an object that is not Dynamic has no button. A system fed by a
rule needs no source of its own, and its last source can be removed; a
persistent population cannot be fed by one. A Dynamic object's inspector
offers **New system born of its hits** and **New system trailing it**, and a
particle system's **New system trailing its particles**: one edit that
creates a particle system with no source and the rule that feeds it, so
nothing plays until the source hits or moves. Hits and trails come from
Dynamic objects: a rule on a Static or Animated object, or on every object
when none is Dynamic, is marked as born nothing, as is a trail from a system
that is switched off. A system cannot trail itself, and systems trailing one
another round in a ring are refused: the system trailed steps first, so the
puffs sit where this frame left its particles. Removing either endpoint
removes the rule rather than re-aiming it at everything. Rules lower into
the system in identity order and reach the runtime's contact, body and
particle spawners. Fracture, time and threshold events are not relationships
yet.

At the foot of the viewport a **HUD** says what the frame costs and what
the scene is: GPU time and the cache's state, particles (the most alive at
once through the loop when the CPU traced it, and the room reserved),
bodies (room, things standing, kinds of 256), particle state in KiB, and
the item counts. A limit is named in the caution colour once four fifths of
it is used: body kinds, forces on one system, blasts, relationships and
bodies. The same lines appear under Scene settings as Statistics. Nothing
is thinned or dropped to stay inside a limit; the number is there so the
artist can decide.

Under the transport, the **timeline** shows a track for every item with
time in it and nothing for the rest: an object's release and, when
Animated, its move as a bar from its start for its duration; a particle
system's sources, Initial at 0, Continuous as a bar across the range, Burst
at its time; a blast at the moment it goes off. Points are diamonds and
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
Record actions and checkpoint adoption are not implemented.

## Ownership, determinism and delivery

The authored scene is `Document::scene`. `scene.hpp` defines typed owners;
`scene_properties.cpp` defines property metadata, defaults, bounds, units,
resets, storage and motion invalidation. New scene files use
`ajfx-studio-scene`, schema 2 (schema 1 lacked the Motion stack and drivers
and is refused). Readers refuse unknown, duplicate, malformed
or missing authored properties and unsupported versions. Window state is not
part of this schema. Existing layer documents retain their existing format.

`lower_scene` produces transient compiler IR; it is never another authored
copy. Items reserve component identities at creation, and lowering sorts by
UID, including sources. Fracture is emitted beside its typed owner, so tree
neighbors cannot capture it. Local body material values are explicit; shared
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
state. The cache is written successfully before publishing the package.
The current viewport still simulates: a green cache band means the cache is
available, not that the viewport is reading it. Cache-backed preview and host
cache adoption still need their own end-to-end integration.

## Remaining boundaries

Studio opens only scenes. The old page editor (palette, block pages, coach,
budget search and recipe gallery) has been removed without a conversion step,
since no projects needed migrating. Its layers and blocks remain as the
compiler's IR, and its recipes as compiler test fixtures.

Continuous cross-population attraction/repulsion or flow coupling, event
endpoints beyond collision and motion, light scene items, reusable assemblies, library
organization, authored checkpoints, captured actions, simulation-independent
visibility/isolation, controls publishing and the full export review panel
are not implemented in the scene workspace. There are no decorative controls
claiming these capabilities.

The scene acceptance tests exercise strict save/load, independent surfaces in
one colliding world, package round trips, tint/collider cache invalidation,
order-independent reruns and seeks, bounded persistent populations, and ten
distinct generated mesh inputs with fracture beyond the former 64-kind limit.
The host renderer additionally checks surface color in rendered pixels. This
is not yet proof of both full product-defining sandbox scenarios or a timed
usability comparison.
