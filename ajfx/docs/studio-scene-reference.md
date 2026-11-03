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
creates a Single object or one Grid/Pile instance set. Repeated Static and
Animated objects, and fracture on either, are diagnosed as unsupported.

A particle system owns its sources, initial velocity, gravity share, a
Motion stack, appearance and object-collision switch. Sources have stable
identities and are Initial, Continuous or Burst. Additional sources are
added inside the inspector. A bounded Initial/Burst population can be
persistent; an unbounded Continuous source cannot. Source Initial currently
uses the runtime's time-zero burst and becomes populated on the first step.
It is not yet an arbitrary saved initial-state checkpoint.

The Motion stack is an ordered list of typed behaviors, each with a stable
identity, an on/off switch and its own controls: **Drag** (amount),
**Turbulence** (amount, swirl size, churn) and **Vortex** (strength, axis,
center). **Add behavior** appends one with that kind's defaults; a new system
has an empty stack. The stack's authored order is the order the runtime
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

Scene settings own gravity, angular damping and sleep policy. The fixed
simulation step is 60 Hz. The capture range controls bake/export duration;
non-looping scene playback can continue beyond it. Looping is an explicit
transport choice. The current editing policy is **Initial setup**: motion
edits pause and restart, while visual edits preserve motion-cache validity.
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

Continuous cross-population attraction/repulsion or flow coupling, explicit
event endpoints, force/light scene items, reusable assemblies, library
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
