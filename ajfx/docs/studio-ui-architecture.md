# AJFX Studio: proposed UI and authoring architecture

Status: design reference, partially implemented. Product direction revised on
November 2, 2026; workspace redesign and UI technology selection are pending.
The [scene reference](studio-scene-reference.md) records delivered behavior;
[remaining work](../next-steps-plan.md) tracks the unfinished acceptance criteria.

## Recommendation

AJFX Studio is a scene-based simulation sandbox. Replace the page of layers
and blocks with a viewport-centered scene editor. The scene contains many
named objects, particle systems, forces, and lights, evolving together over
time. Selecting an item exposes its properties and behaviors in one inspector.
An artist assembles a world, defines ongoing interactions, experiments with
initial conditions and motion, refines the result, and captures or exports it.

The scene is the primary authored work. An event is one operation within it.
Finite explosions, long-running particle environments, multi-object destruction,
and interacting magical systems must all fit the same authoring model. A scene
does not need a trigger, an expiration time, or an automatic loop to be valid.

The central rule is **one owner for every authored property**. A mesh object's
model, visual material, and physical behavior belong to that object. Scene-wide
physics belongs to the scene. Preview staging belongs to the viewport. A shared
asset can be reused, but its use and any overrides must be visible.

Do not replace the block palette with an equally large component palette.
Creation should produce complete, visible items; additional controls appear
where they are relevant to the selected item.

The default interface teaches general operations and exposes the artist's own
content. Avoid a product identity built around particular props, materials,
or demonstrations. Scenario presets are optional reference content, not the
navigation, the creation model, or a prerequisite for a working scene.

## Product direction and quality bar

Studio is a professional creative application for sustained scene and simulation
authoring. It has a different purpose from MediaServer's show-operation editor.
Its visual identity, workspace, information density and interaction model must
serve that authoring work. Matching MediaServer's appearance or using its UI
framework is not a requirement. This supersedes the earlier requirement to
reuse the host's theme and widgets throughout Studio.

The current product has been described as feeling toy-like. Treat that as a
product-design problem to investigate through actual editing tasks. Establish
the desired workflow and workspace before committing further architecture to
the current presentation. A palette change alone does not meet this requirement.

The quality bar is:

| Area | Required experience |
| --- | --- |
| Viewport | The scene occupies the main working area. Navigation, selection, framing and manipulation remain predictable across tools and selections. |
| Workspace | Panels resize, collapse and restore reliably. Useful information remains visible at practical densities, with a deliberate hierarchy and layouts suited to authoring tasks. |
| Precision | Numeric entry and scrubbing agree, units are consistent, mixed values are explicit, and supported snapping and coordinate modes are discoverable. |
| Editing | Selection, focus, shortcuts, drag cancellation and undo behave consistently across the tree, viewport, inspector and time controls. |
| Time | Playhead, playback range, simulation, baking, cached playback and stale results have distinct, truthful states. Seeking and frame stepping are central operations. |
| Visual design | Typography, spacing, alignment, contrast, icons and surface hierarchy form one Studio design system. Colour identifies selection, tools and status consistently; decorative treatment does not compete with the scene. |
| Long sessions | Common operations support keyboard use, readable display scaling and efficient repeated edits. Progress, cancellation and failures preserve context and the artist's work. |

Professional quality is assessed through a complete editing session and its
edge cases. Controls for unsupported features must not be added to make the
workspace appear more capable. General keyframing, new solver capabilities
and cache-backed preview retain their separate engineering acceptance criteria.

The detailed design and technology decision sequence is tracked in
[the delivery plan](../next-steps-plan.md#workspace-and-application-design).
Produce workspace prototypes and a working vertical slice before selecting
an application shell. Evaluate the existing toolkit alongside plausible
alternatives against those tasks; no replacement framework is selected here.
Record the choice, evidence, limitations, dependency/licensing implications
and implementation cost in this design when the evaluation is complete.

Keep the scene model, property metadata, edit operations, undo semantics,
compiler, bake and deterministic runtime independent of UI widgets and host
headers. Workspace preferences remain separate from authored scene state.
Renderer reuse is a separate decision from UI toolkit reuse: preserve
production-renderer fidelity through an explicit preview boundary where useful.
Assess viewport embedding, input ownership, GPU resource lifetime and device
loss behavior in the application prototype. Shared code is useful where it
meets Studio's requirements; it must not determine the product's interaction
or visual language.

## Product-defining scenes

Use these two scenes as design and engineering acceptance scenarios:

1. **Mixed-model destruction sandbox.** Import ten different models together,
   retain ten independently editable objects and appearances, arrange or throw
   them into a shared space, and let collisions cause fractures and further
   collisions. Fragments remain part of the simulation and can affect other
   objects; dust and sparks can read those interactions. The artist can adjust
   a selection, reset or restore a checkpoint, and try another initial state
   without reconstructing a chain of one-shot recipes.
2. **Two merging magical systems.** Create two independently art-directed
   particle populations, give them initial distributions and motion, and define
   continuous mutual attraction, repulsion, or flow influence. They approach,
   orbit, exchange motion, and form evolving structures. The behavior reads
   the other system's evolving state, rather than merely overlapping two
   independent effects or following two scripted attractors. A galaxy-like
   appearance does not imply an astrophysically exact solver; the chosen
   approximation and its quality/cost controls must be explicit.

These are first-class scene workflows, not elaborate templates assembled to
hide limitations in isolated effect playback. Simpler examples remain useful
onboarding exercises, but success on a single-object demonstration is insufficient
evidence that the sandbox architecture works.

## Findings in the current implementation

These findings motivated the scene redesign; several describe the retired
page editor and are not a current defect list. The
[scene reference](studio-scene-reference.md) records implemented behavior.
They are source findings, not observations from a running UI session:

| Finding | Evidence | Consequence |
| --- | --- | --- |
| Empty semantic layers, ready-made layers, all block categories, and raw modules occupy the same palette. | `studio/src/palette.cpp`, `Palette::build` | The artist browses implementation vocabulary before making a creative decision. |
| A layer's label chooses both its default solver and hidden responses to gravity, wind, contact, and explosions. | `studio/src/document.hpp`, `LayerWhat`, `WorldShare`; `document.cpp`, `world_share` | Renaming what a thing "is" can change how it behaves. |
| Object model selection can defer to Appearance; Appearance also contains a model selector and a renderer mode called Material. | `studio/src/catalog_rigid.cpp`; `catalog_look.cpp` | Geometry, drawing mode, and surface appearance have overlapping names and controls. |
| Every rigid layer stores a world block, but compilation merges the participating layers into one world, ordered by stable layer UID. | `studio/src/document.hpp`, `Layer::world`; `compile.cpp`; `solid_world.cpp` | Several editable local controls actually compete for shared state. |
| The first solid layer supplies the merged renderer and base physics settings; the coach warns when another layer's rendering differs. | `studio/src/solid_world.cpp`, `merge_solids`; `coach.cpp` | An apparently local change can fail to affect the selected item. Visual order is not the cause; the oldest participating layer is authoritative. |
| Fracture ownership follows the Object above the blocks in the page. | `studio/README.md`, Objects; `catalog.cpp`, `rigid_scope` | A reading arrangement also carries a relationship that should be explicit in the document model. |
| Backdrop and bloom are preview settings; page ground, gravity, and wind compile into the effect. | `studio/src/document.hpp`, `Backdrop`, `Document`, `PageWorld` | The UI must distinguish staging from authored simulation. |
| Bakes exist, but the current preview still simulates instead of playing the cache. | `studio/README.md`, The preview and Not yet | Rearranging controls alone cannot deliver fast baked scrubbing. |
| The typed runtime exposes analytic attractors/vortices and body-to-particle wakes, but no general particle-population mutual-coupling module in the inspected update-module set. | `lib/include/ajfx/modules.hpp`, `Attractor`, `Vortex`, `Wake`, `UpdateModule` | Two evolving particle systems influencing each other requires solver/runtime work, not just an event connection. |
| Rigid simulation has fixed body-kind, fracture, and prop capacities. | `lib/include/ajfx/modules.hpp`, rigid capacity constants | A mixed-model destruction scene must budget distinct fragment kinds as well as the number of visible pieces. |

Existing strengths to preserve: deterministic identities and seeds, fixed-step
simulation, scoped particle behavior, automatic capacity estimates, independent
core and host, shared production renderer, undoable edits, direct manipulation,
background workers, diagnostics, package export, and bake/playback distinctions.

## Authoring model

The document is a Scene. Its persistent items are:

| Item | Purpose | Typical examples |
| --- | --- | --- |
| Mesh object | A visible placed object, with optional rigid physics | Artist-imported model, primitive, static surface |
| Particle system | A population with its own initial state, sources, motion, appearance, and interactions | Initial particle cloud, continuous emission, mesh instances |
| Force | A spatial or timed influence with explicit targets | Wind field, blast, impulse |
| Light | Authored lighting, only to the extent supported by the export target | Particle light, supported scene light |
| Group | Organizes items and scopes reusable assemblies | Selected objects, related systems, artist-authored assembly |

Groups can contain other groups. Organization does not create a solver or
renderer. Grouping, sorting, and renaming do not change simulation settings,
random draws, event targets, or rendering precedence. Transform parenting is a
separate explicit action: moving an organizational group must not accidentally
reparent its contents. A reusable effect assembly can expose a root transform
and documented parameters.

A particle system owns the simulated population; an emitter is a source within
that system. A source creates particles, while the system defines what happens
to them. A system can begin with a populated distribution, emit continuously,
emit in bursts, read events, or combine sources. An initial distribution needs
no ongoing emitter. Start with one source folded into the system inspector;
expose additional sources only when the artist adds them, not as prerequisite
scene rows.

The system's inspector contains Initial state / Sources, Motion, Appearance,
and Interactions. Source-specific settings stay on sources; shared population
behavior stays on the system. Its output is Particles or Rigid bodies, shown
plainly with appropriate capability checks. Creation offers a neutral working
system with editable source, motion, and appearance. Optional saved starting
settings never become semantic types that continue to inject hidden rules.
Applying saved settings writes explicit authored values. Reusable effects contain
ordinary systems, objects, forces, and relationships with inspectable boundaries.

A mesh object's inspector contains:

- Transform: position, rotation, dimensions, with unit labels and viewport handles.
- Geometry: one mesh reference, import, and optional repeat/distribution settings.
- Surface: visual material, color, opacity, and supported emission or shading settings.
- Physics: Static, Dynamic, or Animated; collider, physical surface, mass/density, and release time where relevant.
- Behaviors: fracture, heat, and supported event responses, each with an explicit enabled state.

Static means the object collides but does not move. Dynamic means simulation
moves it. Animated means an authored transform moves it and pushes dynamic
objects. Tooltips can give the familiar terms rigid body and kinematic.

Shape is not duplicated in Appearance. Collider starts as a derived convex
hull or appropriate supported approximation of the mesh, with its derivation
visible. The artist can choose a different collider and display its overlay.
Dynamic hollow objects use supported compound shapes, not an unsupported
triangle-mesh simulation silently substituted by the compiler.

Repeat/distribution controls create one instance set, shown by its asset or
artist-assigned name and count, for example "Model A (9)".
The set supports grid, pile, or scatter where supported, with a stable seed and
per-instance inspection. It does not create nine top-level scene rows. A
time-varying source belongs in an emitter, rather than overloading repetition.

Import supports a batch of different files and preserves the distinction
between assets, placed objects, repeated instances, and generated fragments.
Multi-selection supports transform, physical surface, initial velocity, and
fracture edits together, while retaining independent per-object properties.
Bodies do not acquire a forced lifetime because they are represented by
particles internally; persistent scene objects remain until explicitly removed.

## Property ownership and sharing

| Property | Authoritative owner | UI location |
| --- | --- | --- |
| Mesh and dimensions | Mesh object or system output definition | Geometry / Output |
| Tint, opacity, visual emission | Item's visual material assignment | Surface / Appearance |
| Collider and physical surface | Object or emitted body definition | Physics |
| Friction, restitution, density | Physical surface assignment, with explicit overrides | Physics |
| Gravity, solver-global damping/sleep policy, simulation timestep | Scene simulation settings | Scene settings > Simulation |
| Wind or blast | Force item; targets are explicit | Selected force inspector |
| Ground collision | A named Ground mesh object, initially static | Scene tree and object inspector |
| Contact response and gravity/wind influence for particles | Particle system, with explicit source overrides where supported | Interactions / Motion |
| Initial distribution and velocity | Object or particle system initial state | Initial state |
| Birth schedule | Emitter source | Sources / Emission |
| Particle lifetime, including persistent particles | Particle system, with explicit source overrides where supported | Population |
| Continuous cross-system influence | One scene interaction rule with explicit endpoints | Interactions, also visible from either endpoint |
| Release, motion, and timed impulses | Object behavior or force event | Inspector and timeline |
| Preview backdrop, reference figure, preview lighting, bloom | Viewport staging | Viewport settings, marked Preview only |
| Exported controls | Scene's named controls with property bindings | Export > Controls |

Visual and physical surfaces may be offered together as a convenient preset
such as Wood, but must remain distinct editable assignments. Applying a combined
preset shows which sections it changes; subsequent edits do not reapply it.
For the initial implementation, visual presets use only available renderer
features. Textured imported meshes require additional renderer/asset support.

Avoid a new deep inheritance system. Items have explicit values or named asset
references. Shared assets show their user count and offer Edit shared asset or
Make unique. Local overrides show their source and reset target. Preset defaults
are copied into an item; changing a preset does not mutate existing scenes.
Scene-wide settings have no hidden per-layer override. If the runtime only
supports damping globally, the UI exposes it globally until support changes.

## Workspace

Use a stable workspace rather than a sequence of mandatory setup modes. The
following arrangement is a starting hypothesis for workflow prototypes, not
a fixed layout or a constraint imposed by the current toolkit:

- Top: project name, save/undo, Add, and Export package.
- Left: scene tree with named items, collapsible groups, selection, visibility, and simulation enablement.
- Center: largest possible viewport, direct selection, transform tools, framing, and optional overlays.
- Right: selection inspector with a small consistent set of relevant sections.
- Bottom: transport, time ruler, work range, event markers, cache status, and an expandable animation editor.
- On demand: asset library, scene interactions, curve editor, issues list, exported controls, and technical details.

The normal screen should contain the scene, selected item's properties, and
the simulation. An always-open catalog of everything AJFX can do does not belong
there. Resizable/collapsible panels and a viewport maximize shortcut preserve
space on smaller screens. Panel layouts are workspace preferences.

Keep Scene settings one click away as a persistent scene-root entry. Nothing
selected shows scene settings and scene statistics, not an empty inspector.
Multiple selection shows common editable properties, mixed values, and a single
undoable batch operation. Selecting a behavior or relationship reveals its
handles and relevant curve, without changing the current scene selection.

Define Studio's typography, spacing, density, colour roles and control behavior
as a coherent design system. Preserve useful interaction semantics such as
reliable colour editing, reset to documented defaults, visible keyboard focus
and immediate feedback. Their presentation and implementation may change.
Retain visible words for core creation and export actions and unfamiliar tools;
icon-only navigation is not a goal. Do not repeat the type's introductory
sentence under every object heading. Persistent and switchable task layouts,
including whether docking is needed, must be evaluated in the workspace
prototype before a panel system is chosen.

## Creation and discovery

Add opens a compact menu: Object, Particle system, Force, and supported Light creation,
plus Import. Each opens a small relevant chooser. Importing a model creates a
visible object with its geometry assigned. Creating a particle system starts
with a visible, valid population or source rather than an incomplete set of
blocks. Importing multiple files creates separate named objects in one action.

The library is an on-demand browser focused on the artist's content:

1. Assets: meshes, textures, flipbooks, and supported fields.
2. Materials: saved visual and physical surface definitions.
3. Assemblies: objects, systems, and relationships saved by the artist for reuse.

Complete demonstration scenes belong in optional learning/reference content,
outside the primary creation workflow. Do not populate the main library or Add
menu with a long shipped catalog of narrowly authored scenes.

These collections answer different questions and must not compete in one flat
list. Search is available within the browser; favorites and recents help reuse.
Procedural controls such as Turbulence are added in the system's Motion
section, with a short list of common actions and a searchable More menu.
Commands can be found through a command search with the current selection named.

Core properties such as geometry, color, collider, and lifetime are always
present when relevant. The artist should never have to discover and add a
Color, Appearance, Size, or Physics block to make an ordinary object usable.

## General tools and optional saved content

General capabilities are the primary vocabulary: Import, Transform, Duplicate,
Distribute, Emit, Apply force, Collide, Fracture, and Add interaction. Controls
describe properties and operations, not narratives about a particular prop.
For example, distribution exposes count, shape, spacing, and variation;
fracture exposes pattern, fragment detail, and thresholds. Either works on the
artist's chosen models and selections.

Remove entries such as "Rocks at the top" and "Bucket of stones" from the
default creation and preset surfaces. Remove the recurring emphasis on crates,
chips, and similar sample props from headings, empty states, defaults, tool
names, illustrations, and acceptance exercises. Their underlying capabilities
become generic import, distribution, animation, collision, and fracture tools.
Use neutral terms such as Object, Model, Instance, Fragment, Source, and System,
then use the artist's asset names once content exists. Basic geometry choices
describe actual shapes; built-in prop meshes do not stand in for geometry types.

Saved setups remain a convenience: artists can save materials, settings,
selections, and assemblies they have authored. A saved setup must be editable
through the same ordinary properties as a manually created scene. Specific
demonstrations can be optional documentation or reference projects, never
required steps or special-case application modes. A useful empty scene offers
Import model and Add particle system, rather than a wall of scene templates.

## Continuous interactions and events

Interactions are part of the scene model. Distinguish:

- **Continuous interactions:** collision, attraction, repulsion, velocity/flow
  influence, or another supported exchange evaluated during simulation.
- **Events:** a collision, fracture, time, or threshold causes an action such
  as emission, a blast, or a change in an authored control.

The artist should be able to select two particle systems and choose Add
interaction > Mutual attraction. That creates one editable scene rule whose
endpoints are the two systems. Its controls include direction (one-way or
mutual), strength, reach, falloff/softening, and supported sampling/detail
settings. Selecting either endpoint shows the same rule; its settings are not
stored twice. A scene Interactions view shows named rules and their participants
without requiring a node graph for normal work.

Rigid objects share collision space by default. Particle systems do not
automatically run every possible pairwise interaction; the artist enables
cross-system influences deliberately. Named selection sets support rules such
as "Fragments collide with Architecture" or "Blue and Gold attract each
other." Group membership does not itself create a solver boundary. A live
selection set's membership semantics and any exclusions must be inspectable.

Visual mixing (colors, trails, glow) and simulation influence are independent.
Two systems can retain distinct appearances while exchanging motion. Reading
the other system's centroid is a useful low-cost mode, but cannot be labeled
as a full population-dependent interaction. Analytic fields, sampled fields,
and neighborhood-based coupling are different supported modes with stated
approximations. No disabled or decorative control should imply unimplemented
two-way physics.

Use explicit references for relationships. No "the block above" ownership and
no global event hidden in a local item. A blast is a Force item with a radius,
time, and target set. A fracture behavior belongs to a particular object or
instance set. A system's event source can use a selected object's Collision output.

Represent relationships with short editable rows:

`Model A / Collision -> Particle system A / Emit`

`Model A / Fracture -> Particle system B / Start`

Selecting a row highlights both endpoints in the tree and viewport. The
inspector exposes threshold, amount, delay, and target settings supported by
the runtime. Deleting a source offers delete dependents or retain disabled
relationships with a visible missing-reference issue; never silently retarget
to All solids. Reject unsupported same-step event recursion with a clear reason.
Mutual continuous influence is a valid coupled simulation, not an invalid
dependency cycle. The simulation scheduler must support it explicitly.

An "Emit on collision" action can create a particle system with an event source
and bind it to the selected object in one undoable edit. It leaves appearance
and motion editable through normal controls. Artists can select any combination
of systems, objects, forces, and relationships and save it as an assembly with
explicit exposed controls. Do not privilege an "Explode" command that always
adds one prescribed combination of visual effects. Generic operations and
artist-authored assemblies provide that composition without a scenario-specific
branch of the application. Assembly insertion is undoable and previews its scope.

## Timeline and parameter editing

The timeline uses seconds and frames, with a clear playhead and a work range
independent of particle lifetime. A scene can run until paused; a finite work
range defines the interval to review, bake, or export and does not force its
contents to expire or restart. Loop playback is an explicit transport option.
Show only relevant tracks by default:
emission, object animation/release, blast events, and selected animated values.
Scrubbing, frame stepping, loop playback, and event dragging are primary tools.

Use consistent value controls: Constant, Random range, or Curve where the
runtime supports each. Particle age curves use normalized age; scene animation
uses scene time. Label the time domain explicitly. Stable seeds live beside
random controls. A seed change is local and undoable. No seed changes as a
side effect of sorting, duplication of unrelated items, or preview playback.

The first release can expose existing schedules and curves without promising
arbitrary parameter keyframing. General transform/parameter keyframe tracks
are a later authoring/runtime addition and must not appear as inert controls.

## Sandbox experimentation and initial state

Provide Play / Pause, Restart from initial state, frame stepping, and restoring
an authored checkpoint. Pausing does not discard the scene. Warm-up/settling
and Set initial state from current let an artist begin with a resting heap or
an established particle cloud instead of making every session begin empty.
Such states include velocities, fracture topology, and coupled field state
where required, rather than copying positions alone. Rebase scheduled actions
explicitly when making a later simulated state the new initial state.

Viewport tools support placement, selection transforms, initial velocity
handles, and supported pushes/impulses. Interactive manipulation must distinguish
an edit to the initial setup from an action at the current simulation frame.
Captured actions become timestamped, undoable scene commands or animation data;
they never depend on wall-clock timing during replay. A structural or physics
edit at a later frame offers restart/resimulation from a valid checkpoint or
an explicit new branch of initial state. It cannot silently rewrite the past
while keeping a stale continuation.

Show the active editing policy in the transport (Initial setup or Record
actions), with a predictable default and inline invalidation feedback. Routine
drags should not open repeated decision dialogs. Unsupported actions remain
clearly unavailable under a policy that cannot capture them.

Artists can save alternate scene setups, seeds, or checkpoints for comparison.
These are variations of one authored sandbox, not a growing list of mandatory
effect templates. Existing checkpoints and seed infrastructure are foundations;
complete state adoption and captured interactions are additional implementation
requirements and must be verified before being advertised.

## Preview, cache, and quality

Live simulation, cached playback, and viewport rendering are separate states:

- Preview renders the selected display settings and current simulation/cache.
- Bake computes a cache for the selected range and dependency closure.
- Export validates and packages the effect, supported controls, assets, kernels, and required current caches.

The initial timeline must accurately report Simulating, Baking, Cached,
Out of date, or Failed. It must never imply the current preview is reading a
cache when it is simulating. Implement cache-backed preview as an early
performance deliverable; the existing bake button alone does not provide it.
Keep the last verified preview visible while a replacement builds, with its
revision marked, and cancel stale work using existing generation fences.

Track invalidation by dependencies. Moving a dynamic object invalidates the
shared rigid world and contact-driven particle systems. Changing a draw-time tint does not
invalidate their motion. A shared solver means the whole connected rigid
simulation may require rebaking; do not promise per-object resimulation where
the solver cannot provide it. Mutually interacting particle systems likewise
form a coupled cache unit, with checkpoints for all members and shared derived
state at the same step. Render-only changes keep simulation caches.

Quality controls belong to named settings with explicit effects:

- Viewport display quality: supported render resolution, overlays, and display settings.
- Simulation detail: authored emission counts, fracture detail, fixed step, and supported solver settings.
- Delivery constraints: target renderer/features, frame-time budget, memory, and package/cache size.

If a fast proxy simulation is offered, it is explicitly selected, visibly
marked, and has separate caches. It cannot silently become the final export.
Draft and final must have explicit parameter differences. No automatic thinning
or adaptive solver step that changes deterministic output. More objects or
particles do not automatically mean better work; art direction, timing,
collision correctness, lighting, and depth matter as much as raw density.

Visibility and isolation are display operations by default. Hiding mesh objects
to inspect a particle system keeps their physics and event production. Simulation disablement
is a separately named operation that invalidates dependent results. An
explicit isolate-simulation tool must show its dependency policy.

## Export and reusable controls

Export package is a review panel, not a new authoring mode. It presents target,
range, loop behavior, assets, simulation/cache readiness, supported parameters,
and measured or estimated resource use. Label estimates and measurement
hardware. Package ready means all required data is current and compatible
with that target. Do not imply the host plays a cache until that host actually
supports its playback path.

Save scene always preserves the full editable sandbox. Export defaults to the
whole scene over an explicitly selected range. A selected assembly can be
exported only with its external dependencies included, its dependence baked
into supported data, or an explicit unsupported-dependency diagnostic. An
extracted galaxy must not silently lose the other galaxy's gravitational
influence. Package boundaries are delivery choices, not interaction boundaries
while authoring. A reusable effect is one possible product of a scene.

Expose controls intentionally through a Controls list: name, bound property,
unit, range, default, and Runtime or Requires rebake. Let artists test them in a
preview of the exported interface. A baked simulation cannot pretend to vary a
bake-time control at runtime. A transform that changes collisions is different
from placing an entire completed effect in the host. Display limits and reject
unsupported requests with an actionable reason.

Image/video rendering, full scene lighting export, general fluid simulation,
new shading models, and arbitrary keyframing are separate feature work. The
workspace should accommodate them without advertising capabilities it lacks.

## Diagnostics

Replace pervasive coaching prose with local validation and one issues entry.
Errors block the affected operation; warnings describe a concrete limitation;
optional artistic suggestions are collapsed and dismissible. Each issue names
its item/property, consequence, and relevant action. Clicking selects it.

Examples: "Model A: a moving triangle-mesh collider is unsupported. Use a
compound collider." "Particle system A: source Model B was deleted." "Physics world:
body-kind capacity exceeded by 8; reduce fracture types or instance variety."

Do not add a coach fix for states the normal UI should prevent, such as a
mesh object becoming a dot because an unrelated layer uses Glow. Do not
discard newest scene items to produce a plausible-looking incomplete export.
Retain a last valid preview with an explicit error, and block final export.

## Vocabulary

| Current wording/concept | Proposed wording/placement |
| --- | --- |
| Page | Scene |
| Layer | Named scene item, group, particle system, or source according to its actual role |
| Solid | Mesh object with Physics > Dynamic, Static, or Animated |
| What is in it | Remove; show named objects in the scene tree |
| Things that knock and pile up | Remove repeated description; explain Dynamic in its tooltip |
| The world under each solid layer | Scene settings > Simulation |
| Appearance > Material: Model/Glow/Smoke | System output / renderer type, separate from visual material |
| Made of | Physical surface |
| Made of its own thing | Explicit local physical values or Make unique |
| It can: Be knocked about / Stay put / Be moved | Motion: Dynamic / Static / Animated |
| Let go at | Release time |
| Each lives / Stays for | Lifetime where an emitted item has a lifetime |
| Untidiness | Position variation |
| Break | Fracture |
| Puffs where things hit | Emit on collision, with visible source and target bindings |
| Under the hood / Details | Technical details, opened intentionally |

Use clear nouns for properties, verbs for commands, consistent units, and
short tooltips for unfamiliar technical terms. Precision is welcome; a
conversational sentence is not automatically easier to understand.

## Compiler and runtime consequences

The UI needs a coherent authored model, not a projection that reconstructs
ownership from the old flat block list. Introduce typed SceneItem and behavior
identities, population/source identities, initial-state data, explicit asset
assignments, continuous interaction rules, event endpoints, and one ScenePhysics
settings object. Preserve stable UIDs and parameter identities. Store window
layout, visibility isolation, camera orbit, and expansion state in workspace
state; authored camera data is separate if later supported.

Compile this model into the existing deterministic runtime where feasible.
The compiler may still lower many objects into a shared rigid emitter; that
optimization must not become a user-visible ownership rule. Build world
settings from ScenePhysics, never from the oldest object. Explicitly scope
body properties, source settings, curves, and event filters.

Independent visual appearance inside one physics world requires a real draw
identity/material binding per object/body kind and scoped render batches.
One simulated body state can feed those batches; separate noninteracting
worlds or duplicate simulation are not an acceptable rendering workaround.
Assess existing per-kind mesh support and extend renderer/package descriptors
for genuinely independent material/render behavior. Include affected state
in content hashes, cache signatures, validation, and package versioning.

Cross-system coupling needs an explicit simulation schedule. Authoring-tree
order is never an evaluation order. A viable initial rule is to read all
participating populations from the same completed fixed step, calculate
influences, and integrate their next state with fixed documented ordering.
A bidirectional rule evaluates both contributions from the same state, with
stable tie-breaking and accumulation. Solver-specific collision/constraint
phases remain explicit. Any iterative coupling uses a fixed authored iteration
count; do not claim exact momentum conservation or fully coupled physics merely
because two directed forces were evaluated.

Resolve mutually dependent systems into coupled scheduling/cache units instead
of rejecting all cycles. Bound spatial domains, neighbor counts, field storage,
and precision/approximation modes up front. Spatial grids, sampled dynamic
fields, or hierarchical approximations can avoid unbounded all-to-all work;
choose and validate an actual method before offering a capability in the UI.
Overflow is a diagnosed limit, not permission to drop arbitrary interactions.
The existing packaged distance fields support collision and are not already
a dynamic density/velocity-field coupling solver. That capability needs its
own runtime data, hashing, checkpointing, and CPU/GPU validation.

The destruction sandbox must plan capacity for every imported model and its
potential fragments. The current 64 additional rigid body kinds can be
exhausted by ten distinct models with several unique fragments each, even when
the total body count is modest. Review/extend the runtime's kind and fracture
representation as needed for the target scene budgets. Do not expose "ten
models" in the product while quietly excluding bodies or substituting one
common model. Repeated instances should reuse geometry and fracture definitions
where appropriate, with stable identities for each generated piece and source
provenance for events and selection.

Use one typed property metadata registry for editor widgets, defaults, units,
reset targets, animation/randomization support, publishability, capability
checks, and bake invalidation. Avoid independently authored block labels,
compiler keys, and inspector fields drifting apart. Raw modules remain an
explicit expert escape hatch with clear ownership, constraints, and diagnostics;
they do not occupy the default creation browser.

## Delivery sequence and acceptance

First complete the workflow, workspace prototype and application-shell decision
in [the delivery plan](../next-steps-plan.md#workspace-and-application-design).
The sequence below describes the scene capability milestones; it does not
require building their presentation on the current UI framework. Preserve and
qualify the ownership work already delivered rather than restarting the core.

1. **Ownership foundation and an end-to-end vertical slice.** Model one scene
   physics configuration, explicit object/system properties, initial state,
   continuous relationships, and event references.
   Prove two differently colored/assigned rigid objects collide in one world
   and preserve their appearance through save, bake, and export. Address
   render bindings here; do not build the entire new UI on shared-appearance
   behavior. Update the document schema deliberately.
2. **Core sandbox and mixed-model destruction.** Ship tree, viewport selection,
   inspector, multi-file import, compact Add, shared physics, and scene settings.
   Prove ten distinct models can collide, fracture, and continue interacting
   while keeping independent appearances and source identities. Include the
   required capacity work. Rebuild simple particle and rigid examples.
   Retire the page/palette as the primary interface rather than maintain two
   independently authoritative editors.
3. **Coupled particle sandbox and iteration.** Implement continuous cross-system
   influence and prove the two-merging-systems scenario with a documented
   approximation. Add event bindings, repeated instances, editable assemblies,
   schedule timeline, checkpoint/reset workflows, and cache-backed preview.
   Extend existing curves before adding arbitrary keyframes. Mutual particle
   influence is a core sandbox milestone, not optional template polish.
4. **Finish and delivery.** Visual/physical library organization, controls,
   target validation, resource reporting, and export review. Add asset/render
   improvements only when implemented and verified end to end.

Studio is unreleased and there are no existing projects to migrate, so the
old page editor is retired without a conversion step or compatibility layer.

Required observable checks:

- An imported model is placed, resized, colored, and given physics from one selection.
- Two rigid objects keep different supported appearances in one colliding world.
- Ten different imported models retain their identities and appearances while colliding and fracturing together; resulting pieces can hit and fracture other eligible objects within explicit budgets.
- Two particle populations continuously influence each other through evolving state while retaining independent appearances; disabling their shared rule changes the motion, not just the rendering.
- Changing the tree or source ordering does not change either coupled scenario's result.
- A persistent initial particle population works without a continuous emitter, trigger, forced lifetime, or automatic loop.
- Restoring a coupled checkpoint and replaying captured frame-stamped actions matches uninterrupted simulation within the stated CPU/GPU agreement contract.
- A change to either mutually coupled system invalidates their shared simulation cache; visual-only changes preserve it.
- Export of an assembly preserves or explicitly resolves its external interaction dependencies.
- Renaming, regrouping, and tree sorting leave simulation and draw assignments unchanged.
- Fracture stays attached to its object regardless of neighboring items.
- Hiding a source preserves its simulation and dependent events.
- Applying optional saved settings has no hidden label-driven changes to gravity or contact.
- The core creation workflow works from a blank scene without selecting a scenario preset; Add and the main library expose general tools and the artist's own content.
- Changing tint preserves a valid motion cache; changing a collider invalidates its dependent simulation.
- Save/load, undo/redo, deterministic reruns, seeks, and package playback agree.
- Export refuses stale required caches, unsupported capabilities, and capacity overflow.
- The selected workspace supports a complete import, edit, preview, save and export session with consistent selection, focus, undo, units and keyboard behavior.
- Panel resize/collapse/restore, display scaling, input, hit targets, resets and supported themes are inspected at common display sizes in the shipping application.
- Application-shell acceptance includes an embedded production preview, responsive background work, cancellation, save/reopen and the documented device-loss path.

Use the same creation tasks for usability comparison: batch-import ten models
and create a shared destruction sandbox; build two mutually interacting magical
populations; import and drop a model; distribute a selection; fracture imported
models and bind collision-driven emission; adjust a particle system's sources
and appearance; expose a supported visual control and export.
Record task time, wrong-panel visits,
help requests, and successful results before and after. Proposed targets:
one inspector for the selected object's core properties, no prerequisite
blocks for a visible valid object, and completion of the simple import-and-edit task
without explanation. Set time targets from a measured baseline rather than
inventing evidence of improvement.

## Project guidance to revise

Replace the README's "block editor, like a page in Notion" premise and the
assumption that the authored work is a self-contained finite effect. The full
scene is the work; reusable effects and captured ranges are delivery choices.
Replace rules that make flat reading order carry object ownership. Stop requiring
every capability to be a palette entry. Replace first-solid-layer renderer
and world precedence with explicit scene and item ownership. Allow labels on
important creation actions despite the general preference for frequent
actions to use icons. Record these as intentional design changes.

Studio's own design governs its visual identity, widgets and application shell.
The host's UI design rules do not mandate a framework, shape language, palette,
icon-only controls or shared layout. Existing toolkit dependencies describe the
current implementation; change them only after the workflow prototype and
technology evaluation establish the replacement or justify retention.

Remove scenario-specific built-ins from the default creation model and stop
using a small set of sample props as the vocabulary for the whole product.
Keep optional learning projects separate from tools and artist-authored assets.

Retain determinism, honest budgets and capabilities, host-independent core,
independent behavioral tests, and actionable errors. Preserve useful renderer
and infrastructure reuse behind clear boundaries. The workspace should make
those guarantees understandable while supporting precise, sustained work.
