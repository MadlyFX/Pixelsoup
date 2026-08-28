# Effect Rendering Requirements

This document is the implementation and review contract for every new
first-class scene effect. It applies to values represented by
`scene::EffectKind` and folded into `scene::EffectStack`. Compositor structure
operators such as `RadialArray` are not scene effects, but must follow the same
depth, identity, determinism, and regression-testing principles in their own
planner path.

## Non-negotiable rendering invariants

1. **Ordinary geometry renders once with shared depth.** Enabled effects must
   never split normal, effect-free geometry into camera-dependent flat passes.
2. **Identity is indistinguishable from no effect.** A disabled effect, its
   documented identity value, and invalid values normalized to identity must
   not allocate a pass or alter pixels, depth, ordering, statistics, or bounds.
3. **Document identity is not z-order.** Node IDs may break exact depth ties for
   determinism, but must never decide which physically separated object is in
   front.
4. **Depth behavior is explicit.** Every active effect declares whether it
   shares scene depth, tests against it, produces replacement depth, or has no
   depth relationship.
5. **Coverage behavior is explicit.** An effect that changes alpha or expands
   its source bounds is not a crisp occluder unless its processor produces a
   matching hard-depth result.
6. **Unsupported combinations fail visibly.** The planner or executor must
   return `InvalidConfiguration`; it must not silently omit an effect, flatten
   it into a different order, or render it through an unrelated processor.
7. **All render nodes make the same decision.** Registry traits, identity
   normalization, pass construction, and processor execution must be
   deterministic from replicated project state.

## Compile-time registry

The registry is declared in
`engine/render/include/mediaserver/render/effect_execution.hpp`. Each entry is
indexed by `scene::EffectKind` and supplies:

- a stable display/debug name;
- an execution domain;
- a depth policy;
- an implemented processor route;
- the identity value and identity predicate;
- whether the effect changes alpha;
- whether it expands source bounds;
- whether it reads the already-composited destination;
- a resolver from the folded `EffectStack`;
- a reset function used when an isolated layer is normalized for rendering.

`EffectKind::Count` is the enforcement sentinel. New kinds must be inserted
before it. The registry array then grows at compile time, and its `static_assert`
fails until a complete, correctly indexed entry is supplied.

### Current contracts

| Effect | Domain | Depth | Processor | Identity |
| --- | --- | --- | --- | --- |
| Brightness | Inline | Shared scene depth | Material brightness | Disabled or `1` |
| Blur | Isolated layer | Tests against scene depth | Gaussian blur | Disabled, non-finite, or `<= 0` |

### Execution domains

- **Inline:** computed in the drawable's normal material/media shader. It must
  remain in the shared-depth base pass.
- **Geometry:** changes instances or vertices before rasterization. Generated
  geometry must participate in normal depth testing and picking as applicable.
- **Isolated layer:** renders a source to an offscreen color target before a
  spatial or destination-dependent operation. Ordinary geometry still remains
  together in the base pass.
- **Full scene:** executes only after the complete 3D scene has resolved
  visibility. It must not be represented as a per-object layer.

## Required implementation steps

1. Add the new kind before `EffectKind::Count` and add its resolved parameters
   to `EffectStack` or an appropriate deterministic parameter carrier.
2. Update document schema/model evaluation, serialization, editor creation,
   Inspector controls, timeline/keyframing, and scene digesting.
3. Add one complete registry entry. Implement its resolver, identity predicate,
   and reset function. Non-finite and out-of-range inputs need a documented,
   deterministic result.
4. Implement or select a truthful `EffectProcessor`. A new processor requires
   an actual shader/render path and processor contract tests before an effect
   may reference it.
5. Route planning through registry traits. Do not add independent checks such
   as `if (new_effect_enabled)` to decide isolation, depth eligibility, or
   identity.
6. Preserve authored effect order. Folding is allowed only when the operations
   are mathematically associative and the folded result is deterministic.
7. Define bounds behavior. Spatial kernels, shadows, glows, displacement, and
   similar effects must expand allocation/culling bounds enough to retain every
   generated pixel.
8. Define depth and alpha behavior. If flat back-to-front compositing cannot
   represent an effect on intersecting geometry, implement color-plus-depth
   compositing rather than using average object depth as an approximation.
9. Add every test category below before exposing the effect in the editor.

## Mandatory tests for each effect

### Registry and identity tests

- The registry contains exactly one entry at the effect's enum index.
- Disabled and enabled-at-identity applications are identity.
- Identity produces the same render plan as no effect.
- Identity produces pixel-equivalent GPU output to no effect.
- Invalid/non-finite parameters normalize deterministically and do not create
  accidental passes.
- Resetting the effect's execution domain disables only effects in that domain.

### Depth and camera tests

- Effected object in front of ordinary geometry.
- Effected object behind ordinary geometry.
- The same two cases with document node IDs reversed.
- Camera viewed from both sides and orbited across the depth order.
- Object animated or dragged through the depth order.
- Multiple ordinary objects remain in one shared-depth base pass throughout.
- Coplanar placement has a deterministic, documented tie result.

### Coverage and composition tests

- Opaque, translucent, and alpha-bearing sources.
- Edges expanded by the effect are neither clipped nor promoted into hard
  occluders incorrectly.
- Interaction with every supported blend mode.
- Interaction with another instance of the same effect and with each other
  execution domain.
- Authored stack order changes output when the operations are non-commutative.
- Physical/mapped output views use the same visibility rules as Program and the
  free Stage camera.

### Editor and project tests

- Add, remove, enable, disable, and reorder survive save/load.
- Inspector edits update both the base value and a key at the playhead when the
  property is keyed.
- Timeline sampling updates the displayed property value.
- Undo/redo restores the full effect node and its ordering.

## Review checklist

Before merging a new effect, reviewers must be able to answer yes to all of the
following:

- Does it have a compile-time registry entry and a real processor?
- Is its identity value fully inert?
- Does ordinary geometry still render in one base pass?
- Are depth, alpha, bounds, and destination-read behavior truthful?
- Are all unsupported configurations rejected explicitly?
- Do reversed node IDs and moving cameras preserve physical visibility?
- Is the result deterministic across render nodes and output mappings?
- Are model, editor, keyframe, save/load, planner, and GPU tests present?

## Verification commands

Run the focused contracts first:

```powershell
cmake --build out/build/windows-msvc-debug-gui --config Debug --target `
  mediaserver_render_effect_execution_tests `
  mediaserver_render_compositor_plan_tests `
  mediaserver_render_compositor_scene_tests

ctest --test-dir out/build/windows-msvc-debug-gui --output-on-failure `
  -R "render_(effect_execution|compositor_plan|compositor_scene)"
```

Then run the complete Debug suite and rebuild the Release `media-server` target
used by `scripts/run-editor-demo.ps1`.
