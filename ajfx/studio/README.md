# ajfx Studio

The scene editor for [ajfx](../README.md). The authored work is a scene;
reusable effects and captured ranges are delivery choices. Studio exports
`.ajfx` packages for a host, today the media server.

**New** opens a scene with a static **Ground** object aligned to the grid,
a tree, central viewport and selected-item inspector. **Add > Floor** creates
another editable ground surface. Objects own geometry, visual and physical properties and fracture;
Scene settings own shared physics. Import accepts multiple models. Particle
systems own their sources, motion and appearance. See the
[current scene reference](../docs/studio-scene-reference.md) for implemented
behavior and limitations, and the [remaining work](../next-steps-plan.md).

The scene architecture is not yet complete: continuous population coupling,
cache-backed preview, captured actions and legacy conversion are pending.
Old artist files remain in the legacy workspace described below; originals
are preserved until explicit conversion is available.

## Legacy layer workspace

Existing layer documents use the block workspace. Each part is a **layer**: Flames, Embers or
Smoke. **Add layer** asks *What is it?* -- Solid, Sparks, Fire, Smoke, Dust,
Debris, Rain, Snow, Magic or Something else -- and that answer, shown as a chip
on the layer's heading, decides how the page's world acts on it (below) and
the layer's **kind**, which decides what moves its particles and so what it
can be built from:

- **Particles** -- points moved by forces: smoke, sparks, flames, rain. They
  pass through each other.
- **Rigid physics** -- solid pieces moved by a physics engine: they knock
  into each other, pile up and come to rest.

A Solid is a rigid physics layer; everything else is a particle layer. The two
runtimes accept different things, so the kind decides which blocks the palette
and the picker offer at all. Nothing that cannot play can be put on a layer,
rather than being offered and refused afterwards. A page mixes kinds freely, a
layer at a time. Clicking the heading's chip says what the layer is instead,
naming any block that stands in the way of the kind that goes with it rather
than throwing it away. The kind has its own chip under Advanced (the
**Details** view), for a layer that is, say, sparks moved by a physics engine.

## The page's world

A page has one **ground**, one **gravity** and one **wind**, set on the rows
under the page's title (each with a reset arrow), and the compiler gives them
to every layer. No layer needs a Gravity or a Ground block to fall and land,
and the recipes carry neither. What the layer is decides how much of them it
feels and what it does where it lands:

| What it is | Gravity | Wind | At the ground and on solid things |
| --- | --- | --- | --- |
| Solid | all | none | bounces a little (0.1), grips (0.8) |
| Sparks | all | 0.3 | bounces (0.35) |
| Debris | all | 0.1 | bounces (0.3), grips (0.6) |
| Fire, Smoke | none (they rise by their own blocks) | all | slides |
| Dust | a twentieth | all | sticks |
| Snow | a twenty-fifth | all | sticks |
| Rain | all | 0.3 | vanishes |
| Magic | none | half | slides |
| Something else | none, unless a block says | all | slides |

- **A layer's own blocks stand in for the page's.** A Ground block is the
  layer's own ground and says what it does on contact, at the ground and on
  solid things alike. A Gravity block is its own share of the page's gravity
  (1 is the page's, so setting the page to the Moon's takes it along).
- **Particles meet every solid thing on the page**, whichever layer holds it
  and wherever that layer sits: dust settles on the crates, sparks bounce off
  them. Nothing names a layer. The **Passes through** block is the way out:
  through solid things, and the ground too if it says so.
- **The page's solid layers are one world.** Every Solid layer that plays is
  part of one physics world (one rigid physics emitter), so a crate dropped in
  one layer lands on a crate standing in another, rocks poured in one pile up
  against a heap in the next, and a shelf in one catches what falls in all of
  them. Each layer still looks and starts as it did on its own: its life,
  spawn shape, launch, colour, size, colour and size over life and glow are
  scoped (`ajfx::Scope`) to its own kinds of body in that world, and each of
  its models is a body kind drawn as that model.
  - What there is one of in a world -- its gravity and ground, its damping and
    whether pieces rest, the material of its first layer's own pieces, glowing
    or solid, lights -- is the first solid layer's (the oldest, by uid). Another
    layer's own Gravity, Ground, Air resistance or colliders that differ act on
    the whole world. The coach says so on a solid layer drawn differently from
    the first ("Drawn as Crates is: one world."), and the layer's own seed
    reshuffles nothing in a shared world.
  - Local and order-free: the world's spawn stack is in the order of the
    blocks' uids and its kinds and props in the layers' uid order, so moving
    layers changes nothing, and adding a solid layer leaves every other one's
    pieces as they were wherever they do not touch, to the bit.
  - A solid layer that would take the world past what one holds (256 additional kinds of
    body, 255 ways to make things, 256 standing things) is left out, newest
    first, and says why; the rest play.
- **Smoke and dust are stirred by moving pieces** (`ajfx::Wake`): where
  anything in the solid world moves, a Smoke or Dust layer is pulled along
  by the pieces passing through it. The **Stirred by moving pieces** block
  (Motion) sets how hard and how far, which solid layer, or stirs any other
  particle layer.
- A world holding more than 16,384 pieces cannot be met or stir particles;
  the coach says so on its layers, and the particles pass through it rather
  than the layer refusing to play.
- What a layer meets, and the modules the page gives it, are keyed by the
  layers' uids rather than their places, so reordering layers changes nothing
  in the simulation, to the bit.

Each layer is a short list of **blocks**, each one a plain statement:

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
Contact, Rigid physics, Light), and the raw modules under the hood. Seeing
what there is gives ideas.
- **Drag** an entry onto the page: a line shows where it will land, between
  blocks of a layer or between layers. A block a layer may have once
  (Appearance, Tumble...) only lands in a layer without one.
- A block the target layer cannot hold is **greyed**, and hovering it says why
  ("Not in a rigid physics layer. Bodies move whole."). It cannot be clicked
  or dropped there.
- **Click** an entry to add it to the end of the **target layer**, whose
  heading is tinted and which the palette's own heading names ("Add to
  Yard"). A layer goes to the end of the page.
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
- **Materials, not blend modes.** Appearance offers Model (a 3D model each),
  Glow, Smoke, Solid and Trail. Each sets the blend mode, depth sorting, soft
  edges and geometry that suit it. A rigid physics layer is made of solid
  things, so it offers Model and Glow alone and starts on Model: with no
  Appearance block at all it still draws slabs (the flat block, half as tall
  as a cube), and an object draws as the model you picked, without anyone
  saying so. The built-in models are named for how they look: Cube, Rock,
  Slab and Shard.
- **A coach.** Notes appear under the layer or block they concern, each with
  a one-click fix, for what is wrong or would look cheap. What a layer is
  merely missing is not a note: it is a next step, below.
  - smoke slicing into the floor: soften its edges;
  - glow too dim to bloom: brighten it;
  - tumbling without debris: make it debris;
  - an object, or a Stream with a model, in a glowing rigid layer, which is
    a glowing dot: draw the layer as models;
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
"Each lives" row ("Stays for" on a rigid layer), deals the whole layer afresh. 0 is the block's or layer's
own. Seeds never recompile kernels, so trying one is instant.

**Another seed.** The die under the preview's sandbox tools ("Try another
random seed") deals the layer being worked on a new seed or, with none, every
layer on the page, as one edit, so Ctrl+Z puts the old look back exactly. The
new seed comes from the old one and the layer, so the same press on the same
page always gives the same seeds, and nothing else ever changes a seed. The
page's solid layers are one world with one seed, so re-rolling one re-rolls
them all.

## Guides, handles and paths

**The layer being worked on.** Guides, handles and paths follow what is
being worked on, and nothing else, so the stage stays readable. The working
layer is the one whose block was last opened or changed, or whose heading
was last clicked; an accent mark in the margin beside its heading shows
it. Esc, or folding the layer, leaves nothing worked on, and the preview
draws no guides or paths.

**The target layer.** Where a palette click or "/" adds is one rule:
the layer being worked on, else the last one worked on, else the last on the
page. Its heading is tinted, so what is marked is what is added to; after Esc
or a fold the tint stays and the accent mark goes.

**Guides.** The preview draws where the working layer's blocks act, in the
preview's own camera:
- its spawn shapes at both ends of their radius, and its colliders (a mesh
  collider as its model's outline), faintly;
- for the block being worked on (the one last opened or changed), in the
  accent: its launch cone, arrows for gravity, lift and wind, the swirl's
  axis and turn, draw in's pull and fade height, an attractor's reach, an
  explosion's reach, the ground's height, and turbulence's flow as streamlines that drift and churn
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

## Born of solid things

Three Birth blocks in a particle layer make its particles of the page's solid
things, and *Glows hot* (Look) gives any layer a glow that cools:
- **Puffs where things hit** and **Sparks where things hit**: particles where
  solid things hit something (`ajfx::SpawnFromContacts`). *Hits from* is how
  hard a hit must be; *Per hit* and *Per m/s* how many it makes, so a harder
  hit makes more; *Spread* how they scatter round the bounce. Puffs hang near
  the hit; sparks are thrown off it. Under More: how much of the hit's speed
  throws them up and across, new hits only or sliding too, and the most a
  step.
- **Trail**: particles shed behind moving solid things (`ajfx::SpawnFromBodies`), smoke behind a thrown rock or embers off fragments. *Per
  m/s* is how many a second for each metre a second they move, so a still
  thing sheds none; *Broken pieces only* keeps it to what broke off.
- *From* on each picks which solid layer to read, by name, or **All solids**
  (where it starts). Each reads the page's one solid world, scoped to that
  layer's bodies. The coach says when there is no solid layer to come from,
  or the one named is gone ("All solids" fixes it).
- **Glows hot**: a glow on top of the colour that starts at *How hot* (above
  1 is white hot) and halves every *Cools by half in* seconds, red to orange
  to white and back (`ajfx::Heat`). In a solid layer *On* can be its *Broken
  pieces* only, so the fragments glow where the whole never did.
- A page whose particles are born of a baked solid world bakes them with it.

## Rigid physics layers

A **rigid physics** layer's particles are solid bodies in a physics engine:
they knock into each other, stack, tumble off and come to rest. Object is
under **Rigid physics** in the palette and only appears there.

**Bodies**, on the layer's heading beside "Each lives", is what every piece in
the layer is made of unless an object says otherwise. It belongs to the layer,
not to a block, because a rigid layer has exactly one physics world and no way
to be without one.
- *Body* is what each piece collides as: *Its own shape* wraps a debris chunk
  tightly, *Ball* rolls, *Cube* stacks.
- *Friction* and *Bounciness* set the material; *Density*, *Spin damping*
  and *Rest when still* are under More settings.

**What a rigid layer can hold.** Stream, Burst, Spawn shape, Launch,
Gravity, Rise, Wind, Air resistance, Ground, Sphere and Box colliders,
Tumble, Appearance, Color, Size, Fade, Glows hot and Cast light, plus Object,
Break and Part. Turbulence, Swirl, Attract, Draw in, Spin and Mesh collider are not
offered: a physics engine moves whole bodies, so a force that stirs each
particle on its own has nothing to act on. Within the blocks it does have,
settings the engine cannot honour are not shown either -- Size over its life,
Rise's curve over life, Random spin, Vanish on contact and Keep inside.

**Forces act on the world, not on each piece.** The engine takes one gravity
for the whole layer and one damping for each body, so a force's *Strength*
is one number in a rigid layer rather than a spread: there is no per-particle
variation to promise. Tumble only sets each piece's spin and how it lies; the
engine does the rest.

## Objects

An **Object** is one thing standing in the layer's world: a crate, a heap of
rocks, a bucket, a wall. It carries its own model, how many of it there are
and how big it is, and **the blocks under it are its own**, so a page reads as
each thing in the layer and what becomes of it:

```
Rigid physics  "Crates"              Bodies | 35 of 64 max
  What is in it
    Object  Cube, a stack of 9, 0.40 m
      Break  Cut into 6, over 4 m/s
    Object  Rock, just one, 1.00 m
  The world
    Bodies    Cube, friction 0.60, bounce 0.15
```

Breaking belongs to the Object above it, so the Break and Part blocks under
one object are what *that* object comes apart into. Nothing names anything:
reading down the page is what says so, as it is for a particle layer landing
on the pieces of the rigid layer above it.

- *Model* is what it looks like and what it hits things as: a cube, a rock, a
  slab, a shard, one of your own (OBJ, glTF or GLB), or *The layer's own*,
  which draws it as every other piece in the layer is drawn.
- *How many* is *Just one*, *A heap* (a cone whose sides slope at *Slope*) or
  *A stack* (filling *Width* by *Depth*, layer by layer). Under More settings,
  *Spacing* is how far apart they start, as a share of the largest piece's
  reach, and *Untidiness* nudges each off its place.
- *Size* is its longest side, in metres, and is its own: one layer can hold a
  crate and the girder beside it. (Where the layer's Size is a spread, the
  objects keep that spread in proportion.)
- *Position* and *Turn* place it. *Let go at* holds it still until then; 0
  lets it go as soon as the effect plays. A paused preview at the start shows
  a heap standing.
- *It can* is how it moves:
  - *Be knocked about* is an ordinary piece: gravity and the others move it,
    and it is the only one that can come apart.
  - *Stay put* for a floor, a wall, or a bucket that only has to hold.
  - *Be moved* to tip or swing it yourself, from where it stands to *Moves to*
    and *Turns to*, starting at *Starts moving at* and *Taking* that long. It
    shoves whatever is in its way and nothing pushes it back, so a tip-over
    lands the same every time. Past 90 degrees a bucket is upside down and
    empties.
- *Collides as* is what the other pieces actually hit:
  - *Its own shape* follows the model. Where the object is knocked about this
    is the model's convex wrapping; where it stays put it is the model's own
    triangles exactly, hollows and all.
  - *A bucket*, *a hollow crate* and *a tube* are **hollow**, built from a few
    simple pieces, so things sit inside them. Measured by *Radius*, *Height*,
    *Size across* and *Wall thickness* rather than taken from the model.
  - *A ball* and *a block* are simple solids.
  - Only one object on its own can stay put, be moved or be hollow: those are
    an authored thing rather than a heap of them. The coach says so, with the
    fix.
- *Made of* is *The layer's*, *Concrete*, *Wood*, *Glass* or *Metal*. Picking
  one sets the object's weight, grip and bounce, and how its Break comes
  apart: how hard a hit or a blast must be, and how many pieces it is cut
  into. They are ordinary values afterwards, each with its reset arrow, so a
  material is where to start rather than a lock. Glass breaks at a touch into
  many shards, metal hardly at all and into a few. (The numbers are starting
  points for the eye, not measurements.)
- Under More settings, *Made of its own thing* gives it its own friction,
  bounciness and density, so a steel ball among wooden crates is heavier and
  bouncier. Off, it is made of what the layer's Bodies row says.
- The preview draws the model where the object stands, and, for one on its
  own that collides as something else, that shape faintly over the top: the
  difference between what is seen and what is hit is otherwise invisible.

**A Stream beside an Object.** A Stream meets an Object that the world moves
only inside one rigid layer, since a layer is one physics world (one that stays
put or is moved stands in every layer's world: see [The page's
world](#the-pages-world)), and a layer draws all its pieces
one way. So an Object added to a layer with a Stream leaves the Stream as it
was: the same look, the same shape it collides as and the same size it is born
at (0.1 m unless a Size block says otherwise). What an Object carries of its
own is its model, its size and, under More settings, its material. Its colour
and fade are the layer's, and its model is drawn where the layer draws models:
in a Glow layer an object is a glowing dot, which the coach says once, with a
fix that draws the layer as models (the layer's other pieces then become
models too).

A Stream or Burst in a rigid layer has a **Model** of its own too, *The layer's
own* until it picks one: a cube, a rock, a slab, a shard or a file of your own.
Its pieces then look like that model and hit things as its shape, so a Stream
of rocks can fall on a crate, each as itself. They are the layer's size (the
Size block's, else 0.1 m) and are made of what the Bodies row says; colour and
fade are still the layer's. The same rule holds as for an Object: a model shows
where the layer draws models, and in a Glow layer the coach says so, with the
same fix. Size and material of its own for a Stream are not yet possible.

The **Rock pile** recipe is the starting point: 220 rocks heaped steeper than
they can stand, which slump as the effect plays, and a low puff of dust as
they come down. **Bucket of stones** is a bucket standing full of stones that
tips over a second in and spills them across the floor. **Crates and a
dropped ball** is two objects in one world: nine crates that cut into six
pieces each, and the rock dropped on them that breaks them.

## Explosions

An **Explosion** block (under Motion) is one blast for the whole page: a
*Position*, a *Power* (how fast it throws what is at its middle, fading to
nothing at its edge), a *Radius* and when it *Goes off*. Whichever layer holds
it, every layer that plays answers it, by what it is: a solid piece takes the
blast as it is, sparks a little more or less, smoke and flame about half of
it. A layer that **Passes through** is left alone. Its dot in the preview drags
it across the floor (Shift raises it) and a diamond sets its reach; where it
goes off is drawn on the stage whether or not it is being worked on.

- **Breaking.** A Break's *Breaks when blasted at* shatters the object where
  it stands when a blast throws it that hard, so its pieces fly out from where
  each sat; nearer things break, farther ones are only pushed. 0 leaves
  explosions to push it whole. The Break's reading says it: "Cut into 6, over
  4 m/s, blasted over 6 m/s".
- **The coach** says when nothing on the page explodes ("Add explosion"), and
  when no explosion reaches the object or throws it hard enough ("Break
  easier").
- **Explode this.** An object in a solid layer has an *Explode* chip on its
  heading (while it is open or under the pointer), and *Explode this* is in
  its menu and its layer's. One click, one undo:
  - the object is let be knocked about, and solid if it was hollow;
  - its Break cuts its model into its material's pieces (Concrete's if it has
    none), and shatters when blasted; one is added under it if it has none;
  - an Explosion goes off at its middle half a second after it is let go,
    strong enough to shatter it and reaching four times its size, and is
    opened, so its handle is the one the preview offers;
  - a *Fireball*, *Sparks*, *Smoke* and *Dust* layer are added after its
    layer, born at its middle just before the blast so the blast is what
    throws them, all sized to the object. The dust is a ring level with its
    middle, so it is thrown out sideways rather than pinned to the ground;
  - its broken pieces glow hot and cool (*Glows hot* on its broken pieces),
    an *Embers* layer trails them as they fly (*Trail*, broken pieces only)
    and an *Impact dust* layer puffs where they land (*Puffs where things
    hit*), both reading its layer. A piece is as old as what it broke from, so
    the heat starts high enough to be about 1.4 when the blast breaks it.
- Its blast is named from the block's uid (`blast_<uid>`) and the module each
  layer answers it with is keyed by both uids, so moving or adding layers and
  blocks changes nothing. Page scale applies to its place, power and reach.
  It goes off while the block and its layer are on; soloing another layer
  leaves it going off.
- **Push.** A **Push** block (under Motion) is a short shove at one point:
  a *Position*, which way it pushes (*Toward*), a *Power* (the speed what
  stands at the point is given) and a *Reach*, at a time. It is a small blast
  set half its reach behind the point, so whatever is near the point is
  thrown a little less, and the same way out. Like an Explosion it is the
  page's: every layer that plays answers it.
- **Not yet:** a blast reaches everything at once rather than travelling out
  as a front, gives no spin, and cannot be published to the media server.

## The budget

Under the page's world, **Budget** is how many milliseconds of GPU a frame
the effect may take, simulation and drawing together (4 by default, saved
with the page, with its reset arrow). **Maximize budget** fills it:
- It scales every amount on the page by one factor: a Stream's amount, a
  Burst's count, the puffs and sparks born where things hit, a trail's
  amounts, the pieces of a heap or a stack and the pieces a Break cuts a
  thing into. With **Keep the look** on, particles get smaller as there are
  more, so they cover what they did. Module blocks are taken literally.
- The factor is measured, not guessed. A second preview, never shown, draws
  the page at a factor through the same renderer, at the preview's size and
  camera, and the GPU times it. The stage alone is measured first, and a
  page costs what it adds. A few factors are tried until the page uses 90
  to 95% of the budget.
- It stops where a limit would bind first, and says which ("Held by the
  16,384-piece limit"): a control's own range, a layer the compiler would
  refuse, a solid world too big for particles to meet, a baked layer too
  big to cache. Nothing is thinned to fit.
- A page already over its budget is left alone, and the row says so; the
  button then reads **Scale down**, and fits the page with the same tool.
- The page changes once, at the end: one edit, one undo, seeds untouched.
  An edit while it measures stops it, and so does **Stop**.

The row then reads what it came to: "Uses 3.7 of 4 ms, 2.3x the particles".

## Playing with the preview

A strip of tools runs down the preview's left edge, under its note. A click
in the picture -- a press and release that does not move, so a drag still
turns the camera -- does what the tool that is on says. Clicking a tool again
goes back to Select.

| Tool | A click |
| --- | --- |
| Select | Opens the block of the solid thing clicked (a broken piece opens its layer) |
| Drop a thing | A crate falls onto where you clicked, appearing at the playhead |
| Explode here | An Explosion where you clicked, going off at the playhead |
| Push | Pushes what you clicked away from you, at the playhead |

An effect plays the same everywhere, so a click never reaches into the
simulation. Each tool writes an ordinary block into the page at the
playhead's frame, as one edit, one undo:
- **Drop a thing** adds an Object to the target layer if it is solid, else to
  the page's first solid layer, else to a new one: a crate a metre above the
  point, made of what the Object being worked on (or the layer's last) is
  made of, wood otherwise. Its *Appears at* is the playhead: it is not there
  before, and falls from there. *Appears at* is on every lone Object; 0 is
  from the start.
- **Explode here** adds an Explosion to the target layer, with the usual
  power and reach.
- **Push** adds a Push to the target layer, at the middle of the thing
  clicked (or the floor), toward where the camera looks, never down.

What was made is opened, so its handle is the one the preview offers, and
the preview pauses on it. The transport marks every Explosion, Push and
Object that appears over the scrub bar; a click on a mark opens its block
and moves the playhead there.

A click picks against where the page's solid things stand at the playhead,
as the preview's CPU run sampled them (every 6 steps), each taken as a ball
around it. Too large an effect to run on the CPU has no samples, and then a
click lands on the floor.

## Your own pictures and models

- **Sprites.** Appearance's Texture offers *From a file*: a PNG, JPEG, TGA or
  BMP. *Frames across* and *Frames down* make it a flipbook played over each
  particle's life.
- **Models.** With the Model material, the layer's Model offers *From a
  file*: an OBJ, glTF or GLB, centred and fitted to a 1 m box, so Size sets
  how big each piece is. Its own normals and UVs are kept. An Object, and a
  Stream or Burst in a rigid layer, can carry a model of their own; the
  layer's Model is what the rest of the layer is drawn as.
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
bar, and the preview plays without it (a soft dot, slabs, nothing to
collide with); an export waits until it can be read. Exports carry the
pictures, meshes and fields, so the media server needs none of the files.

## Starting points

**New** opens the recipes: Campfire, Spark fountain, Explosion, Magic orb,
Smoke plume, Snowfall, Rain, Rock pile, Bucket of stones, Crates in the
snow, Crate blast (Explode this on one wooden crate), Rock slide (rocks in one
solid layer tumbling down a slope in another, with *Puffs where things hit*),
and a blank page.

A **Break** block either breaks the Object above it into the **Part** blocks
under it, or cuts that object's own model into cells that fill it. Cutting
happens when the package is built, since that is when the model is read, and
the cells become a body kind and a mesh each.
**Add layer** asks *What is it?*: an empty layer of each, named for it, then
the ready-made layers: Flames, Embers, Smoke, Sparks, Fireball, Debris, Dust,
Motes, Rock pile, Bucket of stones, Crate and others. These are ordinary blocks with
tuned values, each saying what it is, so everything they do can be seen and
changed.

**An empty layer asks what to start with.** With nothing born in it, a layer
shows tiles named for the block each adds: a particle layer *Stream* and
*Burst*; a rigid layer *Pour*, *Burst*, *Object* (stands still) and *Heap*. A
click adds it and opens it. The page's gravity and ground make it fall and
land.

**Then it offers what comes next.** Once something is born, what the layer is
missing is a row of chips, each named for its block, in the order to add:
- *Stream*: a rigid layer where nothing falls. Brings Gravity or a Ground only
  where the page has none.
- *Gravity*: rigid pieces that hang, on a page with no gravity.
- *Ground*: something falls and nothing stops it: the page has no ground, or
  the layer passes through it (not over the Void).
- *Look*: no Appearance.
- *Tumble*: a model look without a Tumble.
- *Fade*: no Fade.

A click adds the block and opens it, and the chip goes. The words are kept to a
few: the block is learned by using it. The coach says only what is wrong.

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
- tumble spin and roundness;
- Appearance's tint, brightness, opacity and size scale (under *More*).

**When a value takes effect.** Every published control is one of two kinds,
and the package says which, so the Inspector can tell them apart:
- *Bake-time*: everything above but the last line. The simulation reads it,
  so a baked layer cannot follow it (see [Baking](#baking)).
- *Playback-time*: Appearance's tint, brightness, opacity and size scale.
  They multiply each particle as it is drawn, so they apply to a baked layer
  as to a live one and cost nothing. They are the controls to publish for an
  operator to tint, dim, fade or scale a placement. The media server's
  particle pass does not apply them yet, and neither does the preview: the
  coach says so where one is set away from 1.

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
  drive several layers, as long as both are the same kind of value and take
  effect at the same time.

**Trying values.** "Published for the media server", under the page's
settings, lists what the effect publishes. Values tried there change only
the preview, through the same per-placement values the Inspector sets.

**Detaching.** Detaching a published block keeps its values and drops the
publication. Recombining puts the publication back.

## Baking

A layer is **Live** or **Baked**. A baked layer's heading says so; a live one
says it only while the pointer is over the heading or it is the target, since
that is what every layer is. Click the chip to switch. A live layer is simulated wherever it
plays. A baked one is worked out once, at whatever it costs, and played back
from a cache, so the media server reads it rather than simulating it.
- **What to bake.** A rigid layer is the one worth baking first: physics is
  the expensive half and what playback waits on, and its bodies cost 36
  bytes each a step. A particle layer bakes too, its particles quantised to
  about a millimetre, but a cheap one is cheaper to simulate than to stream.
  A particle layer of more than 262,144 cannot be baked.
- **Solid layers bake by default.** A new Solid layer is Baked; a particle
  layer is live. A page with a solid world bakes it by itself a moment after
  the last change, once the preview is up to date: the preview says
  "Baking..." quietly, and any edit stops that bake and takes it again once
  the page settles, so nothing waits on it. The coach then says a stale cache
  as a tip, not a warning. Marking the solid layers Live stops it.
- **Bake.** The *Bake* button beside *Export* runs the whole effect from the
  start, for as long as the loop, at the page's own values, and keeps what
  the baked layers did. Its tooltip says what that will come to before you
  press it: the size, what playing it reads a second, and how long it should
  take (from the last bake). While it runs the button shows how far it has
  got; click it to stop. Edits wait for a bake you asked for.
- **Solid layers bake together.** The page's solid layers are one world, so
  it is cached whole: a solid layer marked Baked beside a live one says it
  is not cached yet ("its world is shared with Rocks, which is live") until
  every solid layer is baked. A layer born of a cached world's hits or
  movement, or stirred by it, is cached with it, since a cache keeps bodies,
  not their hits or speeds.
- **Published controls on a baked layer.** A bake-time control is held where
  it is set instead of published, since a cache cannot follow a value its
  simulation reads; the coach says so on the block. Playback-time controls
  stay published.
- **When a cache goes stale.** The cache holds each baked layer as it was.
  Changing something a baked layer's simulation depends on -- a block's
  bake-time setting, its life, its seed, a solid layer it lands on, an
  Explosion or Push in any layer, the loop -- makes it stale: its chip turns amber and the coach names what changed
  ("Break's "Comes apart when hit at" changed since the bake") with what
  baking again would cost. Changing a live layer, or how a baked one looks,
  does not.
- **The transport** shows the steps the cache holds as a band over the
  scrub bar: green while it holds every baked layer as it is, amber once
  one is stale.
- **Export** writes the cache beside the package as `name.ajfxcache`, if it
  belongs to the exported effect (it checks as the media server will), and
  says why when it does not.

The preview still simulates baked layers: the cache is the same particles,
since the simulation is deterministic, and playing it back is the host's.

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
| Click | With a sandbox tool on: select, drop, explode or push there ([Playing with the preview](#playing-with-the-preview)) |
| Double-click, F | Frame the effect (double-click with Select on) |
| Drag a handle | Set what it marks; Shift raises or lowers a position |
| G | Guides on or off |
| H | Handles on or off |
| P | Particle paths on or off |
| Esc | Work on nothing: guides and paths step back |
| Space | Play or pause |
| L | Loop, or play once and stop |
| Home | Back to the start |
| Left, Right | Step a frame |
| / | Add a block to the target layer |
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
- `--maximize` presses *Maximize budget* once the effect is up, and writes
  what each probe measured to the console;
- `--gpu-debug` turns on the D3D12 validation layer, for chasing a lost GPU
  device. Run under a debugger (cdb, Visual Studio) to see what it reports;
- `--lose-device <seconds>` has D3D12 remove Studio's own GPU device after
  that long, as a fault or driver reset would.

**A lost GPU device.** A GPU fault, hang or driver reset removes the
device, and nothing can be drawn again. Studio notices when the next frame
cannot be shown, says why in a system dialog, offers to save the page if it
has unsaved changes, and closes. It does not sit frozen on its last frame.

**How an edit reaches the interface.** The page is compiled off the interface
thread (`CompileWorker`), so a drag never waits for the compiler: each frame
hands over the page as it stands, the newest replaces any still waiting, and a
compile is taken up only if it is newer than the one shown, so a slow compile
of an older page never replaces a newer one. The coach, budgets and *Details*
follow a frame or two behind the pointer.

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
- `.ajfxcache` is the cache of the page's baked layers, beside the package
  it belongs to.

## Layout

| Path | Contents |
| --- | --- |
| `src/block_kind.hpp` | `BlockKind`: every kind of block, named once, with the id a page stores |
| `src/document.*` | Pages, layers, blocks and editing operations; what a layer is and the page's world |
| `src/page_revision.hpp` | Knowing the page has changed: one look a frame, then a number to ask |
| `src/catalog.*`, `src/catalog_<category>.cpp`, `src/catalog_parts.hpp` | The block catalog: controls, defaults, summaries; each category of block is defined in its own file |
| `src/compile.*` | Page to `ajfx::Effect`: layers in order, budgets, scale, loops, cuts, the solid layers' one world |
| `src/solid_world.*` | The page's solid layers merged into one rigid physics emitter, each layer's modules scoped to its body kinds |
| `src/compile_worker.*` | The compile, off the interface thread: newest wins, never an older one after a newer |
| `src/layer_builder.*`, `src/build_core.hpp` | One layer to one emitter: each kind of block places its modules, what several say about one curve is merged, parameters are declared, the page's ground, gravity, wind and solids are added |
| `src/rigid_builder.*` | The part of a build only a rigid layer has: its world, Objects, Break and Part, props and bodies |
| `src/detach.cpp` | A block into the modules it compiles to, and back |
| `src/raw_modules.*` | Generic access to every ajfx module, through its `fields` list |
| `src/library.*` | Generated textures (glow, spark, flame and smoke flipbooks...) and meshes |
| `src/recipes.*` | Recipes and ready-made layers |
| `src/blast.*` | Materials, and Explode this: an object cut, blasted and given its fire, sparks, smoke and dust |
| `src/sandbox.*` | Picking the solid things in the preview, and the events a click writes: dropped things, explosions, pushes, new seeds |
| `src/sandbox_tools.*` | The preview's tool strip and what a click in the picture does with it (no renderer, so the UI tests drive it) |
| `src/coach.*` | The coach's notes and fixes |
| `src/guides.*` | Guides and handles: where blocks act, in the effect's space, and what a dragged handle sets |
| `src/document_io.*`, `src/json.*` | Saving and loading pages |
| `src/budget.*` | The page's budget: scaling its amounts, the limits that bind, and the search for the factor that fills it |
| `src/budget_meter.*` | Measuring a page's GPU time a frame in a second, hidden preview, for the budget |
| `src/package_builder.*` | The background worker: kernels, packages, the artist's files, CPU analysis |
| `src/user_assets.*` | The artist's files made package-ready: sprite textures, debris meshes, baked collision fields, outlines |
| `src/session.*` | Where Studio was left, written on close and read on open |
| `src/palette.*` | The palette of layers, blocks and modules, dragged onto the page |
| `src/preview.*` | The stage and the media server renderer |
| `src/stage_overlay.*` | Guides, handles and particle paths drawn over the preview, in its camera |
| `src/stage_camera.*` | The preview's camera as the page's toolkit sees it, and dragging handles through it (no renderer, so the UI tests drive it) |
| `src/page_view.*` | The block page: its state and selection, the header and the parameters |
| `src/page_view_layer.cpp`, `page_view_block.cpp`, `page_view_notes.cpp`, `page_view_overlays.cpp`, `page_view_support.hpp` | A layer's heading, settings and rows; a block's row and controls; the Module block, hood lines and coach notes; the pickers, dragging and floating parts |
| `src/widgets.*` | Studio's controls |
| `src/studio_app.*` | The window: top bar, preview, transport, history, files |
| `tests/studio_tests.cpp`, `tests/<area>_tests.cpp`, `tests/test_support.hpp` | Headless tests of the page model and compiler (`ajfx.studio`), one file for each part of Studio they are about |
| `tests/studio_ui_tests.cpp` | The page and controls against synthetic input (`ajfx.studio_ui`) |
| `tests/frame_bench.cpp` | `ajfx_studio_frame_bench`: times what the application does on its interface thread each frame and per edit. Not a test |

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

- A solid layer's own look where it differs from the world's first (glowing
  or solid, lights, tint), and its own seed in a shared world.
- Textures on imported debris models.
- Per-block thumbnails and a timeline of parameter keys.
- Playing the preview from the cache: the media server's renderer
  simulates the preview live, so a bake makes the export and the host's
  playback cheap, not Studio's scrubbing.
- Baking per value over a published range, so a baked layer could keep a
  bake-time control published.

The [roadmap](../docs/roadmap.md) has the rest.
