# Remaining scene architecture work

The shipped foundation is described in
[the scene reference](docs/studio-scene-reference.md). Complete the acceptance
criteria in [the design](docs/studio-ui-architecture.md) before treating the
architecture as delivered.

1. Finish qualifying the ownership slice in the native workflow: drive file
   import, undo, an explicit bake and final export through the running app at
   common window sizes. Layout at wide and narrow windows, night dim,
   interface zoom, multi-edit, colour and handle drags are covered; the page
   editor is removed.
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
6. Run the full Debug and Release root qualification before merging. Record
   usability task times and mistakes from an observed baseline; no measured
   improvement is claimed by the current implementation.
