# Next Steps: From a Working Prototype to a Deployable System

This software is unreleased and undeployed, no "legacy" features, workflows, or UX/UI elements need to be supported or migrated. Breaking changes are okay, new projects can easily be created.

This software is being run with .\scripts\run-editor-demo.ps1, so ensure the output executable that is run by this script.

This roadmap starts from the code that exists today. It supersedes the earlier
"vertical slice" plan: the native editor, keyframe timeline, FFmpeg media
decode, imported models, document-backed compositor, physical output mapping,
project files, and lifecycle-managed node workflow are now implemented.

The objective is not to add the broadest feature list next. It is to close the
largest end-to-end operational gaps in an order that makes the current system
useful on more than one machine. Architectural context is in
[media-server-architecture-plan.md](media-server-architecture-plan.md).

## Implemented baseline

The following are no longer roadmap work:

- Immutable project document, command/revision model, frame scheduling,
  checkpoints, Director/node sessions, and frame-boundary presentation.
- Project save/open, checksummed atomic `.mstage` files, WAL/release records,
  and recent-project recovery.
- Bespoke native operator UI: docking, Scene/Library, Stage, Inspector,
  Timeline, Compositor, Hardware Map, Feed Map, and Output Monitor.
- Scene authoring for primitives, lights, media, imported models, materials,
  effects, Program/Rehearsal switching, and component keyframes with hold,
  linear, and cubic interpolation.
- Image-sequence/still and FFmpeg video decode behind `MediaSource`, plus
  glTF/GLB/OBJ import and GPU caching.
- Typed compositor graph nodes for Scene Layer, Brightness, Blur, Radial Array,
  Composite, and Output; a deterministic evaluated plan and GPU executor.
- Canvas, planar, horizontal-arc, and imported-UV-mesh feed mapping.
- A lifecycle agent that owns a fixed local child, authenticates status/start/
  restart/stop requests, and reports transient machine/display inventory and
  physical output-timing state.
- A project-wide output synchronization policy, synchronization groups derived
  from the Hardware Map with exact timing validation, an authenticated sync
  device/reference/lock report, render-runtime present-barrier and frame
  accounting on the session heartbeat, a group late-frame hold rule, and a
  fail-closed Program gate that names the specific unmet condition.

The code is still a prototype. "Implemented" means an exercised capability
exists, not that it has the device coverage, operational tooling, performance
budget, or interoperability needed for a production show.

## Editorial and compositor depth

### Problem

The editor and compositor form a strong usable base, but the graph and
timeline are intentionally small. Broadening them before assets and deployment
are reliable would create projects that cannot be run consistently on nodes.

The effects as currently implemeted also create a transparency or depth issue in the editor 3D viewport. Applying the brightness or blur
effect to an object results it in always being drawn on top of other objects.

### Deliverable

Expand editing around real operator workflows while
keeping document evaluation, undo, and cross-node behavior deterministic.

### Candidates

- More layer/effect types, masks, mattes, color operations, and graph
  diagnostics; add GPU passes only when their model semantics and output scope
  are explicit.
- Add advanced per-effect settings in a Project Settings menu, to allow users to 
  adjust things like blur kernel or brightness curve without cluttering up the main UI
- Compound gestures, selection/group operations, robust undo coalescing, and
  richer curve/timeline manipulation.
- Project versions, presets, compare/rollback workflow, and an audit trail
  built on the existing document/persistence contracts.
- Asset/proxy/thumbnail workflows that consume the content manifest rather
  than invent a second asset identity.
- OFX and GLSL support

### Exit criteria

Every new authoring operation has a deterministic document representation,
clear undo behavior, focused model tests, and an answer for how its assets and
render requirements reach every selected node.

## virtual-production and professional interfaces

### Problem

The current renderer provides a local scene/output workflow. It is not yet an
ICVFX system or a broadcast/live-event I/O platform.

### Deliverable

Add external interfaces only after their ownership, timing, media contract, and
failure behavior are designed end to end.

### Candidates

- A vendor sync-device provider behind `net::ClusterSyncStatusProvider`
  (NVIDIA G-Sync via the NVAPI GSync topology/status functions, and the AMD
  equivalent), and a native D3D12/Vulkan presentation path behind
  `rhi::PresentBarrierBackend` so swapchains can actually join a swap group.
  Both seams exist and are exercised; only the vendor implementations are
  missing, and until they land a physical sync policy correctly reports
  SWAP BARRIER INCOMPLETE.
- A platform PTP/IEEE 1588 time provider for clock correlation, scheduled
  deadlines, and telemetry. It replaces neither genlock nor swap lock.
- LED processor and camera-chain reference integration plus a measured
  validation pass: a synchronized test pattern and high-speed camera or
  measurement rig. Software-reported GPU lock does not prove that the emitted
  photons are aligned.
- Camera tracking, calibrated camera/frustum workflows, and inner-frustum
  composition.
- SDI, NDI, ST 2110, audio, and GPU/video-device paths with a well-defined
  clock and color contract.
- Managed color/HDR/calibration integration where it belongs in the output
  chain, including the boundary with the LED processor.
- Plugin or external-render texture interfaces with explicit device/API and
  synchronization ownership.

### Exit criteria

Each interface has a supported-device matrix, an integration test or hardware
test plan, a documented clock/color model, and a failure mode that cannot
silently put an incorrect image on Program.

## Specific hardware API integration and control

### Problem

The current Hardware Map provides some basic generic pieces of hardware, for example "led processor." The goal is to 
integrate the APIs and SDKs for specific hardware, allowing users to, for example drag a "Novastar Coex LED Processor"
onto the hardware map. The software would be able to provide a much richer information about the hardware status, 
which ports are active, etc. as well as flag any potential issues such as refresh rate or bit depth mismatches.

This would also allow for remote control of hardware, for example if such a bit depth mismatch is detected, a "resolve"
button could be clicked by the user to automatically fix the configuration. 

### Deliverable

Add hardware devices with their APIs or SDKs, make configuration as transparent to the user as possible

### Candidates

- Novastar Coex, Brompton, Megapixel and Colorlight LED processors
- Blackmagic Hyperdeck and Aja Ki Pro recorders
- Evertz and Brainstorm master clock generators
