# Studio design and remaining scene architecture work

The shipped foundation is described in
[the scene reference](docs/studio-scene-reference.md). The
[Studio design](docs/studio-ui-architecture.md) defines the product direction,
authoring model and capability acceptance criteria. This plan tracks future
work; none of the redesign or framework evaluation below is delivered yet.

Studio is an independent professional creative application. Its purpose is
scene and simulation authoring, and its identity, workspace and interactions
must serve that purpose. Matching MediaServer's UI or retaining its framework
is not a requirement. 
## Remaining scene capabilities and qualification

The workspace plan changes the presentation strategy; these engineering
requirements remain. Use the detailed acceptance scenes and observable checks
in [the design](docs/studio-ui-architecture.md#delivery-sequence-and-acceptance).

1. Finish qualifying the ownership slice in the native workflow: drive file
   import, undo, an explicit bake and final export through the running app at
   common window sizes. Earlier coverage includes wide/narrow layout, night
   dim, interface zoom, multi-edit, colour and handle drags. Requalify those
   interactions in the selected workspace; the page editor is removed.
2. Add typed force, light and relationship owners, event endpoints and
   dependency diagnostics. Expose supported repeated-instance inspection,
   assemblies and asset/material libraries. Extend object collider/transform
   representation only with tested runtime support.
3. Implement a bounded continuous population-coupling method and fixed-step
   schedule, evaluated from a shared completed state. Include coupled cache
   units, checkpoints, CPU/GPU agreement, overflow diagnosis, tree-order
   invariance and the two-merging-systems acceptance scene.
4. Integrate cache-backed viewport playback; keep simulation, bake and drawing
   state distinct. Add complete initial-state adoption, checkpoint restore and
   frame-stamped actions with deterministic replay and explicit invalidation.
5. Finish scene controls, target/resource reporting, dependency-aware assembly
   export and export review. Verify the shipping host's cache consumption
   before describing sidecar delivery as host cache playback.
6. Run the affected area checks during implementation and the full Debug and
   Release root qualification before merging. Include workspace usability and
   measured responsiveness results; no improvement is claimed without evidence.
