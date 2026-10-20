# Repository guide

Read this guide and the short README first, then inspect only the modules and
documents relevant to the requested change. Search with `rg`; use the local
`CMakeLists.txt` to locate tests and dependencies. Avoid reading every planning
document or scanning binary model assets to understand a small change.

## Code map

| Area | Location |
| --- | --- |
| Immutable document, edits, undo and scheduling | `engine/doc`, `engine/cmd`, `engine/core` |
| Command codec, sessions, lifecycle, frame authority | `engine/wire`, `engine/net`, `engine/show` |
| Project files, checkpoints and WAL recovery | `engine/persist` |
| Scene, animation, tracking and asset identity | `engine/scene`, `engine/track`, `engine/assets` |
| Compositor graph and effect contracts | `engine/compositor`, `effect-rendering-requirements.md` |
| Feed mapping, hardware topology and sync policy | `engine/output`, `engine/topology`, `engine/sync` |
| GPU abstraction, renderer and HLSL | `engine/rhi`, `engine/render`, `engine/shaders` |
| Editor panels, widgets and SDL presentation | `engine/editor`, `engine/gui`, `engine/ui` |
| Director/node entry points and Blender service | `apps/server` |
| Director frame-loop phases and editor command authoring | `apps/server/director_show_loop.hpp`, `apps/server/director_*.cpp` |
| Offline lighting bake | `apps/bake` |
| Blender publisher | `integrations/blender`, `tests/blender`, `scripts/test-blender-livelink.py` |

Modules have colocated `tests/`; the headless spine also uses `tests/unit/`.
`tests/fixtures/blender-scene-link` is required by CTest and qualification
scripts. `Models/` is optional sample source data. `third_party/` includes
build inputs and notices and must not be treated as disposable cache.

## Development contracts

- This software is unreleased. Breaking schema and UI changes are acceptable;
  do not add migration or compatibility layers without a concrete need.
- Keep expensive file reads, hashing, parsing and blocking waits off control
  and frame recording paths. Use bounded, event-driven worker/cache adoption;
  avoid expensive periodic work on the control thread.
- Frame recording cannot grow capacity. Refuse and diagnose exceeded budgets.
  Keep authored document state distinct from transient health and discovery.
- Nodes derive image decisions from replicated state, the full authored canvas
  and exact show frame. Preserve fixed ordering, deterministic arithmetic,
  filter aprons and generation fences. A late node reports or holds according
  to policy; it must not silently lower image quality.
- New render-affecting fields must be normalized, replicated and included in
  the scene/settings digests. Capabilities mean callable, tested behavior;
  unsupported requested features fail readiness with a specific reason.
- Keep platform/GPU details behind the RHI and presenter seams. Blender edits
  enter through the Director's validated document authority.
- Prefer direct manipulation in the operator UI. Keep terminology about
  actual state, and hide technical details until they help an operator decide.
- Test observable properties with independent expectations and distinct
  non-default wire values. Count newest-wins overwrites. Measure stalls before
  changing their cause and after the fix.
- UI features must be reachable and exercised through the shipping workflow.
  Update the relevant current docs when behavior changes.

## Build and validation

Implementation changes require `windows-msvc-debug-gui` and
`windows-msvc-release-gui` builds from an MSVC developer shell with
`VCPKG_ROOT` set. `scripts/run-editor-demo.ps1` launches the current editor
workflow; `-Exe` selects a particular build. Run relevant CTest entries using
`ctest --preset <preset> -R <pattern> --output-on-failure`; use the full suite
when a shared contract changes. Headless changes can also use the headless
presets. Blender checks are listed in `integrations/blender/README.md`.

Documentation and generated-file cleanup need link/reference validation and
checks of affected launch, package and fixture paths; they do not need new
tests that merely duplicate those edits.

## Repository hygiene

Write disposable builds, logs, captures, demo projects and exports under
`out/`. Never commit Python bytecode, `.mslink-cache-v1` directories or
digest-named `.gltf.*.msmodel` / `.glb.*.msmodel` / `.obj.*.msmodel` caches.
Preserve source assets, test fixtures, third-party notices, user projects and
local configuration. Keep current behavior in focused reference docs and
future requirements in `next-steps-plan.md`; Git history holds completed
step plans and historical review output.
