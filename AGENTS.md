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
| Device protocols (NovaStar COEX, Brompton Tessera), processor monitor and search | `engine/devices`, `scripts/coex-simulator.py`, `scripts/tessera-simulator.py` |
| GPU abstraction, renderer and HLSL | `engine/rhi`, `engine/render`, `engine/shaders` |
| Editor panels, widgets and SDL presentation | `engine/editor`, `engine/gui`, `engine/ui` |
| Director/node entry points and Blender service | `apps/server` |
| Director frame-loop phases and editor command authoring | `apps/server/director_show_loop.hpp`, `apps/server/director_*.cpp` |
| Offline lighting bake | `apps/bake` |
| Particle effects in the scene renderer (ajfx host side) | `engine/render/particle_*`, `engine/scene` (`scene.particle_effect`) |
| ajfx runtime and Studio | Separate ajfx checkout (default `../ajfx`) |
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
- Editor UI follows `docs/ui-design.md`: rounded shapes and colours from
  `gui::Theme` only (no inline RGB), glow reserved for live or waiting state,
  icons with tooltips via the shared `operator_widgets` controls, sentence
  case, `color_well` for every colour, a reset arrow on every editable value,
  and short `ease`/`spring` motion that never delays input.
- Test observable properties with independent expectations and distinct
  non-default wire values. Count newest-wins overwrites. Measure stalls before
  changing their cause and after the fix.
- UI features must be reachable and exercised through the shipping workflow.
  Update the relevant current docs when behavior changes.

## Build and validation

Build from an MSVC developer shell with `VCPKG_ROOT` set. GUI builds use
the sibling ajfx checkout or the path set by `MEDIASERVER_AJFX_SOURCE_DIR`.
`scripts/run-editor.ps1` launches the editor (Director) workflow and
`scripts/run-output-node.ps1` a render node on another machine; `-Exe`
selects a particular build. Both log to `%LOCALAPPDATA%\MediaServer\logs`.

Testing is tiered so a change builds and runs only what it affects:

- **Area**: each code-map area has a build preset and a test preset of the
  same name: `area-spine`, `area-session`, `area-persist`, `area-scene`,
  `area-compositor`, `area-output`, `area-render`,
  `area-editor`, `area-server` and `area-blender`. `cmake --build --preset area-render`
  links only that area's tests (on Debug GUI); `ctest --preset area-render`
  runs them. Run every area the change touches. `-R <pattern>` narrows further.
- **Quick**: `ctest --preset quick` runs everything except tests labelled
  `slow`, after a full `windows-msvc-debug-gui` build. Use it when a change
  crosses areas or alters a shared contract.
- **Full**: the `windows-msvc-debug-gui` and `windows-msvc-release-gui` test
  presets run every entry. `scripts/package-windows-release.ps1` builds both
  configurations and runs both full suites before it packages. Run them
  yourself before merging to main.

Areas and the `slow`/`blender` labels are assigned in
`cmake/TestGroups.cmake`; a new test directory needs an area there. Only
`area-server` and the full builds relink `media-server.exe`. Headless changes
can also use the headless presets. Blender checks are listed in
`integrations/blender/README.md`.

Documentation and generated-file cleanup need link/reference validation and
checks of affected launch, package and fixture paths; they do not need new
tests that merely duplicate those edits.

## Repository hygiene

Write disposable builds, logs, captures, demo projects and exports under
`out/`. Never commit Python bytecode, `.mslink-cache-v1` directories or
digest-named `.gltf.*.msmodel` / `.glb.*.msmodel` / `.obj.*.msmodel` caches.
Preserve source assets, test fixtures, third-party notices, user projects and
local configuration. Keep current behavior in focused reference docs and
an area's future requirements in its plan document (for example
`asset-viewer-plan.md`); Git history holds completed step plans, the former
roadmap and historical review output.
