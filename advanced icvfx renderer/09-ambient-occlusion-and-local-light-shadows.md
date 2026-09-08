# 09 — Ambient occlusion and local-light shadows

**Track 3.** Depends on [04](04-compute-in-the-rhi.md) and
[05](05-padded-slice-render-and-crop.md). The local-light half is much more
useful once [16](16-photometric-and-spot-lights.md) adds spot lights.

Two related pieces of work: both are about **grounding** — making objects look
like they are in the space rather than pasted onto it.

---

## Part A — Ambient occlusion and contact shadows

### Why

With [07](07-image-based-lighting.md) landed, ambient becomes a bright,
omnidirectional environment term. Without occlusion that term reaches into
creases, under furniture, and into the contact between an object and the floor,
and every object starts to float. AO is what stops IBL from flattening the image
— the two are close to a package deal.

The `Material::occlusion_texture` path already exists for baked per-asset AO,
which handles small-scale detail. What is missing is screen-space occlusion
between separate objects, which no baked texture can know about.

### What is missing first: a normal product

Advanced currently produces scene colour, product depth, velocity, object ID and
coverage. It does **not** produce normals. AO needs them.

Two options:

- **Reconstruct from depth.** No new target, but derivatives are wrong at depth
  discontinuities exactly where AO matters most.
- **Add a packed normal product.** An `RG16F` octahedral-encoded world normal
  target alongside the existing auxiliaries, written by the same replay the other
  products use.

Recommend the normal target. The auxiliary-pass machinery already exists (media
variants sample the same alpha and coverage as the colour pass, zero-coverage
texels claim no product), so this is an additional pipeline in an established
pattern rather than new architecture. It is also needed by SSR later
([11](11-bloom-fog-and-reflections.md)).

### Design

GTAO or a ground-truth-style horizon-search AO in compute, at half resolution
with a depth-aware upsample. The sample kernel must be deterministic: rotate the
sample set by a value derived from **absolute canvas coordinates and the exact
show frame**, using the existing tested helper — never a per-node pixel index and
never a wall clock. Temporal accumulation of AO should ride the same history
decision as [08](08-temporal-antialiasing.md) rather than inventing a second
temporal path.

Contact shadows (a short screen-space ray march along the light direction from
each pixel) are cheap, use the same depth buffer, and fix the specific artifact
that cascade shadows cannot: the few-pixel gap at the base of an object where
the shadow map's texel is too coarse. On a volume this is the difference between
a prop sitting on the floor and hovering a centimetre above it.

Apron: AO's horizon search has a screen-space radius, so
`SliceApronRequirements` needs an AO lane, sized from the authored radius.

---

## Part B — Local-light shadows

### Why

`max_scene_point_lights` is 64 and only the **first directional light** casts a
shadow. Every practical in the set — a lamp, a window, a screen — currently
lights straight through walls. On a stage set that is more obviously wrong than
almost anything else, because interior sets are lit by practicals.

### Design

**A shadow atlas, not per-light maps.** One `R32F` atlas allocated at
construction, partitioned into fixed tile sizes. Authored per light:

- `casts_shadow` — a bool, not inferred. A 64-light scene where every light casts
  is not affordable and the operator should choose.
- A resolution tier — small/medium/large, mapping to tile sizes. Replicated, so
  every node allocates the same tiles.

**Spot before point.** A spot light is one 2D projection; a point light is six.
[16](16-photometric-and-spot-lights.md) adds spot lights, and doing spot shadows
first gets most of the benefit for a sixth of the cost and complexity. Point
shadows can follow as six atlas tiles.

**The determinism trap, and it is a bad one.** The obvious atlas policy is "give
the biggest tiles to the lights nearest the camera". *Every surface has a
different camera* — Program, Preview, Stage, each mapped view, and under
[01](01-camera-tracking-and-inner-outer-frustum.md) the inner and outer frusta
too. Distance-ranked allocation means two nodes assign different tiles to the
same light and shadow it at different resolutions, which is a seam.

So: **assign tiles by stable authored identity** — sort by the light's document
node id, allocate in that order, refuse the overflow and count it. Deterministic,
node-independent, and an operator who wants a specific light prioritised changes
its authored tier rather than hoping the heuristic agrees.

Reuse everything from the cascade work: reversed depth, texel-snapped fits, PCF
with bias expressed in shadow texels (the per-cascade texel-depth mechanism
generalises directly to per-tile texel depth), and the full caster set with no
per-light culling.

### Multi-node hazards

- Distance-ranked atlas allocation. Covered above; this is the one to watch.
- Culling casters per light by anything view-dependent.
- Lights beyond the atlas budget being dropped in a different order on different
  nodes. Sorting by node id fixes this too.

---

## Tests

**AO**

- The normal product is written where coverage is non-zero and nowhere else,
  matching the existing auxiliary-product rules.
- AO darkens a crease between two boxes and does not darken an open plane.
- Byte-exact across two node slices with AO active — this is what the apron lane
  is for.
- A repeated show frame produces identical AO.

**Local shadows**

- A spot light behind a wall leaves the far side dark.
- Atlas assignment is identical for two renderers given different cameras and the
  same scene.
- Lights past the atlas budget are refused and counted, and which ones are
  refused is identical across nodes.
- `casts_shadow` false means no tile is allocated at all.
- A floor with nothing above it does not self-shadow at any tier — the same acne
  guard the cascade work uses.

## What was built

Part A's foundations and the occlusion pass itself. Part B is untouched, and
what is missing from Part A is stated under "Still open" rather than implied.

### The normal product

`scene_normal.frag.hlsl` and its media variant write octahedral world normals
into `RG16F`, through the same auxiliary replay and under the same coverage
rules as identity and motion: a depth-only instance claims no normal, and media
with exact-zero coverage leaves the nearer surface's normal alone.

The recommendation in the plan was the right one. Reconstructing normals from
depth is cheaper and wrong in precisely the place occlusion matters most - at a
depth discontinuity the derivative spans two surfaces and belongs to neither,
so a crease gets a normal pointing nowhere near either of its walls.

`octahedral.hlsli` is shared rather than copied because the encode and decode
have to be exact inverses; two copies that drift by a sign in one hemisphere
give occlusion that is right on one side of an object and wrong on the other,
which reads as a lighting bug rather than an encoding one.

### Sampleable depth, in the RHI

This was the actual blocker and it was not in the plan. The auxiliary depth
product was created as a depth target only, and a depth-stencil format is not a
legal shader-resource format, so nothing could read it. Occlusion needs it, and
so does the depth rejection [08](08-temporal-antialiasing.md) had to leave out
for the same reason.

`d3d12_create_texture` now stores a texture that is both depth target and
sampled as `R32_TYPELESS`, with the depth view naming `D32_FLOAT` and the
shader view naming `R32_FLOAT`. It is the only place in the backend where the
resource format and the view formats differ, which is why they are three
separate lookups rather than one.

### The occlusion pass

A horizon search in compute at full resolution. Eight directions and six steps,
both compiled into the loop bounds - an adaptive quality dial is a seam, not an
optimisation, and a node that quietly took fewer samples than its neighbour
under load would show it along their shared edge.

Two decisions are load-bearing for a cohort:

- **The kernel rotation comes from absolute canvas coordinates and the exact
  show frame**, through the same integer avalanche the film grain uses, now
  extracted into `deterministic_hash.hlsli`. Integer-only because a float
  expression is at the mercy of whatever a driver fuses, and two nodes with
  different driver versions would then rotate differently for the same wall
  pixel. A node holding half the wall must rotate identically to a node holding
  all of it, or a crease crossing their seam is sampled two ways.
- **The screen-space radius is measured, not approximated.** The world radius
  is converted to pixels by reconstructing the pixel next door at the same
  depth and seeing how far it moved. A fixed pixel radius would occlude a
  distant wall as hard as a near one, and an approximation from the depth value
  alone would be a *different* approximation on a node whose slice has a
  different aspect.

The search is clamped to 32 pixels, which is exactly the apron lane the slice
reserves for it. Reaching further would read past the padded region, and on a
node that is not more scene - it is the edge of the world, along the seam.

`occlusion_radius` is authored and replicated, and zero is an explicit off
rather than a degraded fallback: with image-based lighting the ambient term is
bright and omnidirectional, and a show that wants it unoccluded should say so.

### Applying it

The pass reads this frame's depth and normals, and the lit pass reads this
frame's occlusion. That is a cycle unless the auxiliary replay moves, and it
used to run *after* colour - so the occlusion existed and nothing could
consume it.

Depth, identity, motion and normals now run before the colour pass; only
coverage stays behind it, because coverage samples the colour target. The
reorder is safe for a reason worth stating: each auxiliary call already clears
and rewrites the same auxiliary depth, so which one runs first does not change
what that depth ends up holding.

Occlusion multiplies the whole ambient term, specular included. That is not
physical - a mirror in a corner still sees the room - but it is what stops a
chrome prop reading as unlit and floating, and the choice matters far less than
it being the *same* choice on every node. That answers one of the open
questions below.

### What the work found

**One count in four places, again.** The lit pipeline's fragment texture count
was written out at the pipeline's sampler count, the per-batch arena
reservation, the draw's per-batch stride, and the composite layer's own binding
array. Adding a binding means finding all four, and three of four is a refused
draw with no explanation - which is exactly what happened when the environment
bindings were added in [07](07-image-based-lighting.md). It is now
`advanced_fragment_textures`, named once.

**Publishing had to be gated on the recording, not on the pass existing.** The
texture outlives a frame, so an accessor that returned it whenever it existed
would hand a consumer last frame's occlusion for a scene that had since turned
occlusion off.

**The fallback was darker than the thing it stood in for.** With no occlusion
recorded the shader binds a one-by-one white texture, and `Load` does not
clamp: an out-of-range read returns zero. Zero here is not "no occlusion", it
is "no ambient light at all", so a scene that switched occlusion *off*
rendered completely black. The tap is now clamped to whatever is actually
bound. This is the second time in this plan that a fallback path was the broken
one while the feature path worked - the case nobody looks at is the case worth
a test.

### Tests

- **Occlusion darkens a crease and not an open plane** - a floor and a wall
  meeting in frame, measured as two bands rather than two pixels so a
  silhouette cannot decide it. Also asserts every value is a visibility
  fraction, that the open floor is close to unoccluded (so this is occlusion in
  a corner rather than a global dimmer), and that a scene authoring no radius
  publishes no product at all.
- **And that it reaches the image.** The same two frames are read back as
  colour: the crease is measurably darker with occlusion on, and darker by more
  than the open floor moved - a term that dimmed the whole frame equally would
  satisfy the first check on its own. In practice the open floor comes back
  bit-for-bit identical and the crease drops about a quarter.

## Part B — what was built

The prerequisite and the deterministic half. The GPU passes are not written;
what is missing is stated under "Still open" rather than implied.

### Spots light like spots

`LocalLightKind::Spot` had been in the document for a long time - kind,
direction, inner and outer cone, all parsed and *validated* - and nothing in
the shader ever read the cone. Every practical on a set lit like a bare bulb,
spilling in every direction including through the wall behind it. On an
interior set that is more obviously wrong than almost anything else, because
practicals are what light interiors.

`GpuLight` grew a cone axis and the two cosines. Cosines rather than angles
because the shader compares against a dot product, and converting once on the
CPU is one deterministic evaluation instead of one per pixel per light - and it
goes through `deterministic::cos_turns`, because two nodes comparing against
differently rounded cosines would put the edge of the cone in different places
on the same wall.

### The atlas partition

`plan_shadow_atlas` is its own file for one reason: the allocation policy is
the trap this step warns about. The obvious policy is biggest tiles to the
lights nearest the camera, and **every surface has a different camera** -
Program, Preview, Stage, each mapped view, and the inner and outer frusta of a
tracked shot. A distance-ranked atlas means two nodes assign different tiles to
the same light and shadow it at two resolutions, which is a seam that moves
whenever the camera does.

So tiles go by stable authored identity: sort by document node id, allocate
first-fit in that order, refuse the overflow and count it. Tiles are aligned to
their own size, because without that where a large tile lands depends on the
gaps earlier lights happened to leave, and the packing stops being a pure
function of the ordered request list.

Only spots are served. A point needs six faces and a rectangle is not a
projection at all; both are refused *explicitly* rather than shadowed as if
they were a spot pointing down their authored direction, which would be a
shadow nobody asked for in a place nobody chose.

`casts_shadow` and `shadow_tier` are authored, replicated and digested, and -
having just been caught shipping a setting the document could not hold - they
go through the schema, the parser and the round-trip guard as well.

### The GPU passes

An `R32F` atlas and a matching depth target, allocated once at construction
like the cascades. Each tile is one `draw_pulled` confined to its own rectangle
by `PulledDraw::viewport`; the atlas is cleared once before them, because a
clear is not viewport-scoped and clearing per tile would wipe the tiles already
drawn.

The per-light table is built **where the plan is made**, not while rasterizing,
so the matrix a tile is drawn with and the matrix the lit pass reads back are
the same value and cannot drift. Each spot carries the index of its row in
`params.w`, an index rather than a flag because the table holds only the lights
that got a tile and is far shorter than the light list.

The lit pass mirrors the cascade lookup: project into the light's clip space,
reject behind and outside, bias in shadow texels rather than world units, and
filter three by three. The difference is that a tile is a rectangle inside a
shared atlas, so every coordinate is scaled into it and the kernel is clamped
to the **tile**, not the atlas - a kernel running off the edge of one tile
would read its neighbour's map, which is one light casting another's shadow.

### What the work found

Three failures, each worth keeping:

**The atlas render was nested under the directional block.** It only ran when
the scene had cascades - so a set lit purely by practicals, which is exactly
the scene this feature exists for, got a tile allocated and never rasterized.
The lookup then compared against a cleared atlas and reported everything lit.
It now sits outside that guard entirely.

**Basic bound four storage buffers against a signature that declares three.**
The shadow table is Advanced-only, and adding it to the shared binding array
broke every Basic draw with a refusal that says nothing about which count was
wrong. The bindings are now a span sized by mode.

**An empty table left the buffer with no address.** A scene with no casting
spot uploaded zero bytes, and the lit pipeline binds that buffer whatever the
scene holds - so the draw was refused and the frame came back black for a scene
that never asked for a shadow. The table always carries at least one row now.

All three are the same shape: the binding contract is a count and a lifetime
that several places have to agree on, and the failure mode is a refusal with no
diagnosis. Instrumenting every `return false` in `render()` found each of them
in minutes, which is the second time that technique has paid for itself here.

### Tests

- **A spot does not shine through a wall** - the whole of Part B as one
  assertion. Ambient is zero so every photon comes from the one lamp, and the
  comparison against the same light with `casts_shadow` off is load-bearing:
  without it the wall's own cone falloff would be indistinguishable from a
  shadow. In practice the far side drops to exactly zero while the near side
  comes back bit-for-bit identical.
- **Atlas assignment ignores the camera, and a floor does not self-shadow** -
  two renderers at different viewpoints on one scene agree on every tile and
  every refusal, which proves the renderer did not reintroduce a camera the
  allocator was careful to exclude. The acne half counts lit against dark
  pixels rather than averaging, because an average hides stripes.
- **Assignment does not depend on the order lights arrive** - the same lights
  in reverse produce an identical plan, and the order really is by node id.
- **Tiles are authored sized, aligned and disjoint** - no two share a texel,
  because a shadow map written over by its neighbour is one light casting
  another's shadow.
- **Overflow is refused and counted identically** - and the *same* lights are
  refused, not merely the same number of them.
- **Only spots that asked get a tile** - a light that did not ask is not a
  refusal; one that asked and cannot be served is.
- **An indivisible atlas allocates nothing** - a configuration error reports no
  refused lights, because conflating it with a full atlas sends an operator to
  the wrong place.
- **A spot lights a cone and a point does not** - on the GPU, the same light at
  the same position and intensity, differing only in kind. The comparison is
  the load-bearing half: it rules out a cone that "works" because the light is
  simply dim.

### Authoring

The renderer understood spots long before anything could make one. The scene
model parsed `kind: "spot"`, validated the cone, and the shader and atlas
consumed it - and the editor offered Directional and Point and nothing else,
with a light inspector of Colour, Intensity, Position and Range. A feature
reachable only by hand-editing the project file is not reachable.

- **Library → Lights → Spot Light**, its own entry rather than a point the
  operator converts afterwards, because the cone and its aim are the whole
  reason for placing one. It is created casting, with a cone wide enough to
  read as a pool of light; an operator who cannot afford the tile turns it off.
- **The Inspector** gained a Kind switch (point and spot are one node type with
  a different `kind`, so switching is a property edit rather than a different
  node), an Aim row, the two cone half-angles in the turns the document stores,
  and the shadow controls. The tier appears only once the light actually casts:
  a resolution for a shadow that is off is a control with nothing behind it.
- **`schedule_generic_edit`** translates the three that are not really numbers.
  The Inspector speaks in doubles because most of what it edits is a number; a
  boolean written as `1.0` parses as absent, so the translation happens at the
  writer rather than by widening the reader.
- **A spot is named a spot.** `kind_name` took an `ElementKind`, and a spot and
  a point share one - so every spot was labelled "point light" a second after
  being added from a menu entry that said Spot Light.

**Tested through the control, not around it.** `a_spot_is_authorable_from_the
_inspector` presses the actual rows and asserts the property *names*, because
an edit named something the parser does not read is a control that moves and
does nothing - which is the failure this area produced three times running.
Point lights are asserted to draw no cone rows at all, so the spot-only
controls are shown to be conditional rather than merely present.

## Definition of done

Objects sit on the floor; creases are dark; practicals cast; two nodes agree on
every atlas assignment and every AO sample.

## Still open

- **The occlusion is noisy and nothing denoises it.** Eight directions and six
  steps with a per-pixel rotation is a sparse estimator, and the grain is
  visible around a contact. The plan says the occlusion should ride the same
  history decision as [08](08-temporal-antialiasing.md) rather than inventing a
  second temporal path, and that machinery now exists - `TemporalResolve` holds
  a colour history keyed by the same slot. Accumulating occlusion through it is
  the obvious next step and would also make the sparse kernel affordable.
- **Contact shadows** are not written. They share the depth this step just made
  readable, so the ingredient they were waiting on is now there.
- **Point lights still cast nothing.** A point needs six faces and is refused
  and counted rather than approximated. Whether it is ever worth six tiles is
  the open question below; most practicals on a set are directional enough to
  model as spots.
- **The shadow is not filtered beyond a three by three kernel**, and a large
  tier on a wide cone will show a stepped edge. A wider kernel or a variance
  map is the ordinary answer and neither is here.
- ~~**`casts_shadow` has no inspector control.**~~ Closed, along with the rest
  of the authoring path - see "Authoring" above.
- ~~**Byte-exact two-node AO is not tested.**~~ Closed, and it holds. In
  `two_nodes_and_one_node_agree_byte_for_byte`, occlusion on its own and the
  casting spot on its own each report **zero** differing pixels between one node
  and two across eight frames. The kernel rotation keyed to absolute canvas
  coordinates and the atlas partitioned by document node id both do what they
  were written to do - which is worth stating plainly, because the same test
  found two defects in the temporal path before it passed (see
  [08](08-temporal-antialiasing.md)).
- **Full resolution, not half.** The plan asks for half-resolution with a
  depth-aware upsample; this is full-resolution. That is a deliberate ordering
  choice rather than an omission: the plan's own open question notes that the
  apron must survive the downsample without a half-pixel offset differing
  between nodes, and shipping a correct full-resolution pass first makes that
  optimisation a measurable change against a known-good reference instead of
  two new things at once.

## Open questions

- **Half-resolution AO with a padded slice** — the apron must be expressed in
  full-resolution pixels and survive the downsample without a half-pixel offset
  differing between nodes. Worth a dedicated test.
- **Point light shadows at all?** Six tiles per light is expensive and most
  practicals on a set are directional enough to model as spots. Possibly never
  worth it.
