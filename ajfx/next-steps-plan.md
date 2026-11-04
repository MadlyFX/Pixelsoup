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
   state distinct. Add complete initial-state adoption. Frame-stamped actions
   (Drop and Delete, the scene's performance) replay deterministically and
   revise from their tick. Painting (and erasing, pushing and pulling with
   the brush), grabbing and throwing, poking, blasting and re-recording are
   in the core and `ajfx-studio-cli`, with both sandbox scenes replayed bit
   for bit headless; still to come are their viewport tools (brush cursor,
   grab under the pointer with the wheel for depth, timeline bars dragged in
   the window, a "re-record from here" confirmation) and a Setup mode at tick
   0 that freezes settled bodies as the stage. Delivery is in the core and
   the command line: the cache carries its seed and performance, the media
   server plays a package's cache beside it (refusing a stale one in
   readiness), `replay` proves a delivered package against the scene at a
   seek, and the performance exports to glTF from the cache. Still to come:
   the viewport playing its current bake through that cached path, the
   transport's cache band switching it, and the same check on a remote
   node's wall. The reactive palette is in the core and the command line
   too: starter kinds (smouldering log, crate, glass pane, brick, rock,
   cardboard box) with their effects attached, effect templates attached to
   and restyling any object, dust as the default on-hit effect, wind that
   blows on light bodies (`ajfx::BodyWind` through `RigidWorld::force`), a
   firm kinematic hold for grabs (`ActionKind::Hold`), and the issues list
   (refused drops, unapplied acts, kind and prop budgets, stale caches).
   Still to come: the palette panel, a template picker in the inspector, the
   Issues panel and a firm-grab choice in the viewport's grab tool.
5. Finish scene controls, target/resource reporting, dependency-aware assembly
   export and export review. Host cache playback is verified on the renderer's
   tests and headless replay; qualify it in a running show on a node, and
   stream large caches rather than read them whole on the package worker.
6. Run the affected area checks during implementation and the full Debug and
   Release root qualification before merging. Include workspace usability and
   measured responsiveness results; no improvement is claimed without evidence.
