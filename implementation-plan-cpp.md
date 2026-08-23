# Implementation Plan — C++ Core, Compositor, and Spartan-derived Renderer

Companion to `media-server-architecture-plan.md`. That document decides *what* and *why*; this one decides *how*, at the level an engineering lead can hand to a team on week one.

**Assumed decisions carried forward:** C++ core, no Qt, hybrid distribute/transport topology (camera-bounded over ST 2110, wall-bounded rendered locally), FreeD + OpenTrackIO tracking only.

**Revised (§0.3):** the first RHI is SDL_GPU behind an `engine/rhi` seam. The forked SpartanEngine renderer with Atlas and hybrid-rendering ports still stands, but it moves to the 3D phase rather than sitting on the critical path. §6 and §7 below are written against the original ordering and should be read as "when the 3D phase starts", not "next".

---

## 0. Current execution priority - de-risk the output path

**What changed, and why.** The control plane is built to a standard well beyond
what any stated requirement asks of it, while the parts that decide whether this
ships - GPU rendering, feed mapping, warp/blend, media decode - have no code at
all. V0 put a number on the imbalance: the durable seal path capped the Director
near 20-25 sealed frames per second, so the system as built **could not reach
the 60 fps the product requires**, let alone 120. That was not a tuning problem.
It was the foundation being shaped for a requirement nobody wrote down.

**V1 has since applied §0.1 and §0.2 and the ceiling is gone**: two nodes hold
120 fps for ten minutes, and the durable write has left the seal path. The three
decisions below stand as written; the notes on what they licensed are now
statements of what was built.

The correction is not a new plan. It is a return to the one in
`media-server-architecture-plan.md` §11, which already orders the work so the
hardest de-risking happens first, and whose Phase 0 (RHI, frame graph, colour
pipeline, **and the thin output node**) is the part that was skipped.

Three decisions follow. Each is recorded because it licenses work that would
otherwise look unsafe, or stops work that currently looks mandatory.

### 0.1 Decision - the recovery model is restart and re-cue

A show recovers from a Director failure by **restarting and re-cueing**. It does
not resume an exact frame. Node failure is covered by hot-spare promotion at a
frame boundary (architecture §1.6, §10), which needs replicated runtime state
and no durable log at all.

Consequences:

- The Director's durable log is a **project artifact and an audit trail**, not a
  frame-resumption mechanism.
- The WAL, release ledger, cohort journal, unresolved-release inventory, and
  recovery release authority are **frozen**. They are correct and tested, they
  stay in the tree, and no further work goes into them.
- The orchestration around them - Begin-before-WAL cohort journalling,
  Bind-before-fanout, Begin-to-Bind startup reconciliation, and a prepare-only
  recovery listener - is **cancelled, not deferred**. Under this recovery model
  it has no requirement to satisfy.
- Startup keeps its fail-closed refusal of an unresolved durable suffix. It
  costs nothing and it is correct under any recovery model.
- Director redundancy stays open, and belongs with the hardening phase. When it
  is answered it will be a spare Director holding replicated state, matching the
  node story - not log replay.

This decision was the one implicitly missing. A durability stack was built
without a recovery model to size it against, so it grew until it was internally
consistent rather than until it was sufficient.

### 0.2 Decision - durability cadence is not frame cadence

Today every sealed frame, including an empty one, costs a WAL append and sync
plus a release-ledger append and sync. That is where the 20-25 fps ceiling comes
from. Under §0.1 there is no reason to durably record that control frame 84,102
was empty.

Two changes, in order:

**Durability becomes edit-shaped.** The per-frame seal, release and permit path
stays in memory. That path is what guarantees two nodes agree, and architecture
§3 is explicit that the *software barrier*, not genlock, is what prevents one
node presenting frame N while another presents N+1. That guarantee is kept
intact. What changes is that an operator edit is journalled and a frame ordinal
is not.

*V1 status:* the frame ordinal is no longer journalled, and that is what removed
the ceiling. The edit journal itself is not built. The sealed-frame WAL is
contiguous in the control-frame ordinal by construction, so it cannot represent
a gapped stream of edit frames at all; an edit journal needs its own record,
keyed to edits, and it is sized with the project-save format at V5. Until then
`--durable-frames` restores the frame-cadence journal and its ceiling, and an
`edits-only` Director counts and reports the edits it did not write.

**The transport becomes an anchor keyed to the control frame, not to a clock.**
The document carries `(anchor_control_frame, anchor_show_frame, rate, direction,
state)`, and a node derives what it presents as

```
presented = anchor_show_frame + (control_frame - anchor_control_frame) * direction
```

with the whole term suppressed when the state is paused, and re-anchored on any
operator action.

This corrects the note carried in the V0 status below (§0.8), which proposed
anchoring to wall time and deferred the change to PTP. Anchoring
to wall time would have traded exact agreement for throughput and then bought it
back with a locked clock. Anchoring to the **control-frame ordinal** needs no
clock at all: the control frame stream is already an exactly replicated,
totally ordered timebase, so two nodes on control frame N compute the same show
frame by integer arithmetic on replicated state. Exact agreement is preserved,
per-frame document churn disappears, and no dependency on PTP is created.

The payoff is larger than the frame rate. A playing show makes **no document
edits at all**, which is exactly the property scene upload, delta propagation
and ROI (§5.5, §8.3) want during playback.

### 0.3 Decision - the first RHI is SDL_GPU, and the Spartan fork moves late

§6 below commits to forking SpartanEngine and stripping it over weeks 2-5. That
estimate does not survive contact with this codebase's standards, and the fork
is the single largest unknown in the plan.

SDL3 - already a pinned dependency, already carrying the output windows - ships
`SDL_gpu.h`, a portable device abstraction over Vulkan, D3D12 and Metal with
SPIR-V and DXIL shader paths. Verified present in the pinned 3.2.26.

- Take SDL_GPU as the first RHI, behind an explicit `engine/rhi` seam.
- Keep the seam narrow enough that a Vulkan-direct backend can replace it when
  profiling or a feature demands it: bindless, mesh shaders, or the ray-traced
  ports in §7.
- **The Spartan fork is deferred to the 3D phase**, where its value actually
  lives - the PBR renderer, the frame graph, the DDGI and denoiser ports. None
  of that is on the critical path for feed mapping, warp/blend, or 2D.

This trades control for months. It is reversible; the fork is not.

### 0.4 Delivery gates

| Gate | Scope | Binary exit condition |
|---|---|---|
| **V0 - Playback plumbing** *(met)* | SDL3 Director/Node windows, show-clock and frame-boundary driver, timecoded test frames, bounded document-to-playback handoff | Two local Node processes advance, pause, and seek to the same numbered test frame without allocating or performing network I/O at the commit boundary |
| **V1 - Frame-rate headroom** *(met)* | §0.1 applied, durability decoupled from frame cadence, transport anchored to the control-frame ordinal | Two nodes hold 120 fps for ten minutes with zero skipped show positions, the same show frame on both, and flat Director memory |
| **V2 - GPU output path** *(met)* | `engine/rhi` seam over SDL_GPU, presentation off the CPU blit, one offscreen target per output | Both node windows present from a GPU target at 120 fps with per-frame GPU timing reported |
| **V3 - Feed mapping** | Warp, soft-edge blend, per-output calibration LUT, several outputs per node | Two nodes drive one continuous surface with no visible seam, and a calibration reloads live without a restart |
| **V4 - Media and 2D** | Pinned LGPL FFmpeg decode, frame cache, asynchronous GPU upload, layer stack with position/scale/opacity | An imported clip plays across the mapped surface on two nodes; play/pause/exact seek repeatable, frame/skew/drop diagnostics visible |
| **V5 - Operator surface** | Source browser, timeline and scrubber, preview, layer list, transform controls, node-health panel | An operator builds and runs the V4 show without editing configuration files or using test-only controls |
| **V6 - Productionize** | OCIO, ROI propagation, real output (DeckLink/NDI first, Rivermax when the NICs exist), PTP, soak and hardware acceptance | Prototype seams are replaced without changing the operator workflow or the deterministic control contract |

V3 before V4 is deliberate: feed mapping is the untouched risk and the actual
differentiator, while FFmpeg decode is well-trodden ground. Swap them only if a
customer commitment demands media first.

TLS, log compaction, Rivermax, custom widget infrastructure, and advanced
compositor features gate none of V1-V5.

### 0.5 The wedge is still on the table

Architecture §11 Phase 0.5 proposes shipping the output node standalone,
ingesting ST 2110 from an existing Unreal nDisplay rig. That remains the
highest-leverage move in the plan and V1-V3 above are exactly its prerequisites:
after V3 the output path is a product that needs no renderer, no media engine
and no compositor.

It is worth stating the constraint that makes this matter. Architecture §11
sizes the full plan at **12-18 senior engineers over 42 months**. At smaller
scale the full scope - 3D, ICVFX, tracking, 2110, and an After Effects-class
interface - is not reachable on any timeline worth committing to. The wedge is
what makes a smaller team's version of this real: ship the output layer, take
revenue and production feedback, and let the render and media layers land
behind it. 3D scenes stay in the goal; they arrive last because the document
model and frame driver already accept them additively.

### 0.6 V2 status - complete

Two node windows present from a GPU target at 120 fps. The measured run: 1,200
control frames, both nodes ending on the identical show frame, zero present
failures, zero skipped presents, and a present path costing **0.25-0.65 ms** of
the 8.33 ms frame budget.

What was built:

- `engine/rhi` - a narrow device/queue/resource seam over SDL_GPU. Nothing
  above it names a backend type, and the surface is dictated by what an output
  node does rather than by what a graphics API offers: textures, a storage
  buffer, one pipeline shape, a command list, a fence, and a swapchain.
- `engine/shaders` - one universal HLSL source per stage, compiled at build
  time to bytecode embedded in the executable.
- A backend-neutral quad description of the test pattern in `engine/show`, so
  the CPU rasteriser and the GPU renderer draw the same layout from the same
  constants rather than from two copies of it.
- `ui::GpuOutputTarget` - one offscreen colour target per output, deliberately
  separable from any window. That is what makes it testable without a display,
  and it is the object real output hardware will take at V6.

Three things worth recording because they cost time to find:

**SDL_GPU has no timestamp query.** GPU execution time is therefore not
measurable through this backend at all. Two things are, and both are reported:
wall time inside `present()`, which is exact and answers whether presentation
fits the frame budget; and submit-to-fence-observed, which is only an upper
bound, because the fence is examined when the next frame comes round rather
than when it signals. Reading the second as GPU time is how a frame that
finished in 250 us gets reported as 8 ms.

**A claimed swapchain starts on vsync.** The CPU path had always disabled it -
presentation must never pace the show - and the GPU path silently reintroduced
the coupling until the negotiated mode was made visible. The present mode is
now reported per frame for exactly that reason.

**The dummy video driver cannot back a swapchain.** Offscreen rendering,
blitting, and readback all work headless; only presentation needs a real
window. The suite is split along that line rather than pretending otherwise.

### 0.7 V1 status - complete

`mediaserver_show_soak --seconds=600 --rate=120` drives a real Director service,
a real release-gated barrier and two node connectors over loopback TCP: 72,000
control frames at 120 seals per second, zero skipped show positions, identical
show frames on both nodes, **one** authored document edit for the whole run,
zero WAL bytes written, and a Director authoring footprint pinned at its
retention window. A two-second run of the same code is in the default suite.

Three changes produced it, and the third is the one that is easy to forget:

- The transport is an anchor keyed to the control-frame ordinal, so a playing
  show authors nothing. The Director's second clock is gone with it - what
  remains is a pacer that decides when a frame is due, while the position is
  replicated integer state that both the Director and every node compute with
  the same function.
- `FrameDurability::EditsOnly` takes the durable write off the seal path. The
  seal, release and permit sequence is byte-identical to the durable one; only
  the write cadence changed, which is why the agreement guarantee is unchanged.
- Director authoring indexes retire as frames seal. Without this the run is
  fast and still leaks: ten minutes at 120 fps is 72,000 entries in each of
  three maps.

One consequence is worth stating because it changes an operator-visible
behaviour. Under wall-clock transport a Director that fell behind held timecode
and skipped positions. Under an anchored transport it cannot skip - the position
is a function of the ordinal - so it runs slow instead, and reports the
divergence as `pacer-lag` rather than smoothing it.

### 0.8 V0 status - complete

A Director and two node processes run a show over TCP. The Director seals one
control frame per show frame through the release-gated barrier, and both nodes
independently pace their own boundaries and present the same numbered test
frame. Play, pause and exact seek reach both nodes, a node joining a running
show adopts its checkpoint at a boundary, and the commit boundary performs no
allocation, no deallocation and no network I/O.

Three checks carry that claim rather than asserting it:

- `mediaserver.show_playback` drives a real Director service and two node
  connectors over loopback TCP and asserts both nodes agree on show frame,
  control frame, revision, and rendered surface.
- The `engine/show` suites wrap each boundary operation in an
  allocation-counting guard that replaces every global `new`/`delete`. It
  already caught a publish path that released a document trie on the boundary
  when the handoff was full.
- `mediaserver.ui_sdl_presenter` reads the presented output back from the
  renderer and compares its digest against the rasterised source, so pixel
  order, pitch, upload and render are verified rather than assumed. It runs on
  SDL's dummy video driver and needs no display.

The design decision worth keeping from V0: **the show position is replicated
document state, not a local clock reading.** A node clock is only a pacer, so
two nodes whose boundaries are a third of a frame apart still present the same
frame, and a node that has not been approved for the next frame holds the
previous one rather than extrapolating. §0.2 preserves that property exactly
while removing the per-frame cost of it.

---

## 1. On openFrameworks, Cinder, and app frameworks generally

The underlying instinct is right — nobody should hand-write a thread pool, a socket layer, and a filesystem abstraction in 2026. But openFrameworks and Cinder are the wrong shape for this specific product, and the reason is the one that already disqualified raylib in the architecture doc.

**They own the application.** `ofRunApp()` and `CINDER_APP()` establish a window, a GL context, and a `setup/update/draw` lifecycle, and everything above assumes it. This system has a command-bus-driven frame (§5.5), N offscreen targets feeding 2110/SDI/NDI simultaneously, a bespoke UI, and headless nodes with no window at all. You would spend the first six months fighting the framework's lifecycle, exactly as with raylib.

**They are OpenGL-only.** oF's programmable renderer targets GL 3.2/4.1; Cinder's Vulkan branch was abandoned. You are building on Vulkan and D3D12 via the Spartan fork. There is no path where their renderers coexist with yours, which means you would be importing a framework and using none of the part that justifies it.

**Their infrastructure is weaker than what you need, not stronger.** This is the part worth being blunt about:

| Need | oF / Cinder provides | What this product requires |
|---|---|---|
| Threading | `ofThread` (a `std::thread` plus a mutex), `ofThreadChannel` | Work-stealing scheduler, task graph with dependencies, core pinning, frame-arena aware |
| Networking | `ofxNetwork` blocking TCP/UDP sockets | Rivermax kernel-bypass, multicast group management, PTP alignment, lock-free command bus |
| File access | Thin `std::filesystem` wrappers | DirectStorage / io_uring with GPU decompression for 11K plate streaming |

Adopting them would be a downgrade dressed as a shortcut.

### 1.1 What to use instead: a platform layer plus focused libraries

The honest observation is that oF and Cinder are themselves thin wrappers over libraries you can use directly. Skip the wrapper that owns your main loop; keep the libraries.

**SDL3 as the platform abstraction layer.** This is the good answer, and it is already free — Spartan depends on SDL3 (we delete its audio use, not the library). SDL3 gives you window creation, input, gamepad, display and EDID enumeration, clipboard, high-resolution timers, and platform paths. Critically, **it does not own your main loop** — it is usable as a library rather than a framework. zlib licensed.

**Taskflow for the job system.** Header-only, MIT, work-stealing executor with a first-class task-dependency graph. The dependency-graph model maps directly onto both the frame graph and the compositing graph, which means one scheduler serves both instead of two bespoke systems. (`enkiTS`, zlib, is the leaner alternative if you want pinned-thread control and a smaller surface.)

**Standalone asio for conventional networking.** Boost-licensed, no Boost dependency. Command bus transport, node discovery and management, REST/WebSocket API, OSC. Everything except the 2110 fabric, where Rivermax is the only option and you talk to it directly.

**Focused libraries for the rest** — see §4. The total integration cost is lower than one framework, and nothing dictates your architecture.

---

## 2. Language and toolchain policy

**C++20 baseline, C++23 where the toolchain allows.** Spartan and Atlas both require C++20; that sets the floor. Use C++23 selectively — `std::expected`, `std::mdspan`, deducing `this` — behind feature-test macros, never as a hard requirement in headers shared with vendored code.

| | Decision |
|---|---|
| Compilers | MSVC 19.4x (VS 2022) primary, Clang 18+ for Linux nodes and for sanitizer builds |
| Standard library | MSVC STL and libstdc++. No libc++ — it complicates vendor SDK interop on Windows |
| Exceptions | Permitted at startup, load, and module boundaries. **Forbidden below the frame boundary.** Recoverable errors in the frame path return `Result<T>` (a `std::expected` alias) |
| RTTI | Off in engine libraries (`/GR-`, `-fno-rtti`). On only where a vendored dependency demands it, isolated behind its own target |
| Allocation | **Zero heap allocation in the frame path.** Per-frame linear arena, reset at frame start. Pool allocators for persistent objects. `std::pmr` containers bound to the frame arena where STL is convenient |
| Threading | One job system, work-stealing, fixed worker count pinned to physical cores. No raw `std::thread` outside it. No `std::async` anywhere |
| Warnings | `/W4 /WX`, `-Wall -Wextra -Werror`. Vendored third-party compiled with warnings relaxed via target-level scoping only |
| Sanitizers | ASan + UBSan on every CI run of core libraries; TSan on the job system and command bus nightly |

**The frame-path allocation rule is the one that gets violated silently.** Enforce it: install an allocation hook in debug builds that asserts if `operator new` is called between `frame_begin()` and `frame_end()` on a render thread. This catches the STL container someone added at 2am far more reliably than code review.

---

## 3. Repository layout

Single repository, single version number, one shipped executable with a role flag.

```
/engine
  /core         allocators, containers, math, time, logging, jobs, result
  /rhi          device, queue, resource abstraction ← SDL_GPU first (§0.3)
  /render       frame graph, passes, PBR renderer   ← Spartan, 3D phase
  /shaders      universal HLSL → SPIR-V + DXIL      ← Spartan, 3D phase
  /color        OCIO integration, LUT pipeline, dithering
  /doc          immutable document model, persistent tree, versions
  /cmd          command bus, frame scheduling, undo/redo, branching
  /show         show clock, transport state, frame-boundary driver, playback
                handoff, presentation seam
  /ui           SDL3 output windows today; retained widget layer, vector
                renderer and text shaping later. Optional target: a headless
                node never links it
  /comp         2D compositing graph, ROI propagation
  /media        decode, slicing, disk I/O, content-addressed store
  /scene        USD + MaterialX ingest, bindless scene upload
  /icvfx        FreeD + OpenTrackIO, lens model, frustum management
  /net          Rivermax ST 2110, NDI, PTP, cluster state sync
  /devio        DeckLink, AJA, Spout, capture, EDID
  /map          feed mapping, warp, soft-edge blend, calibration LUTs
/apps
  /server       the one binary — role selected at startup
  /sandbox      out-of-process host for Notch and third-party plugins
  /tools        calibration, content transcode/slice, network diagnostic
/third_party    vendored, in-tree
/tests
  /unit  /golden  /seam  /soak  /hil
```

**`/apps/server` is one executable.** Roles compose (§1.2 of the architecture doc): a node enables local decode, local 3D, 2110 receive, composite, and output independently. The UI ships as a shared library loaded only when the director role is active, so headless nodes never page it in.

**Do not use a git submodule for the Spartan fork.** You will diverge permanently within weeks and submodule workflows fight that. Vendor it in-tree under `/third_party/spartan-import/` with the upstream commit hash recorded in `IMPORT.md`, so you can still diff against upstream to cherry-pick fixes. Same for Atlas and hybrid-rendering — import the specific files you port, with provenance and the original licence header preserved.

**Build:** CMake 3.28+ with presets, vcpkg in manifest mode for external dependencies. Ninja everywhere. Unity builds off in CI (they hide missing includes), on for local developer builds.

---

## 4. Dependency manifest

Pin every version. Licence column is not decoration — §4.1 below is a real compliance risk.

**Platform and infrastructure** — the substitute for an app framework (§1.1):

| Dependency | Purpose | Licence |
|---|---|---|
| SDL3 | Window, input, display/EDID enumeration, timers, platform paths, and `SDL_GPU` as the first RHI (§0.3) | zlib |
| Taskflow | Job system with task-dependency graph | MIT |
| asio (standalone) | TCP/UDP, timers, discovery, REST/WebSocket base | Boost |
| DirectStorage | High-throughput plate streaming (Windows) | Proprietary, free |
| liburing | Same, on Linux nodes | MIT / LGPL dual |
| mio | Memory-mapped file access | MIT |
| FlatBuffers | Command payloads, state deltas — zero-copy | Apache-2.0 |
| fmt | Formatting | MIT |
| spdlog | Logging | MIT |
| toml++ | Configuration | MIT |
| oscpack | OSC in/out | BSD-3 |
| magic_enum | Enum reflection for the parameter system | MIT |

**Graphics, colour, and media:**

| Dependency | Purpose | Licence |
|---|---|---|
| Vulkan-Headers, Vulkan-Loader | Primary API | Apache-2.0 |
| DirectX-Headers, D3DX12 | D3D12 backend + D3D11 interop | MIT |
| VulkanMemoryAllocator | Vulkan allocation | MIT |
| D3D12MemoryAllocator | D3D12 allocation | MIT |
| DirectXShaderCompiler (DXC) | Universal HLSL → SPIR-V + DXIL | NCSA/LLVM |
| SPIRV-Reflect | Pipeline layout reflection | Apache-2.0 |
| OpenColorIO | Colour management | BSD-3 |
| OpenImageIO | EXR/DPX/TIFF/PNG read | Apache-2.0 |
| Imath, OpenEXR | Half float, EXR | BSD-3 |
| OpenUSD | Scene ingest | Modified Apache-2.0 |
| MaterialX | Material interchange + codegen | Apache-2.0 |
| meshoptimizer | Meshlet clustering | MIT |
| Snappy | HAP decompression | BSD-3 |
| BLAKE3 | Content addressing | CC0 / Apache-2.0 |
| HarfBuzz | Text shaping | MIT |
| FreeType | Glyph rasterisation | FTL (**not** the GPLv2 option) |
| Tracy | Frame profiler | BSD-3 |
| moodycamel::ConcurrentQueue | Lock-free queues | BSD-2 / Boost |
| mimalloc | General allocator | MIT |
| doctest | Unit tests | MIT |
| Dear ImGui | **Internal tooling only** | MIT |
| FFmpeg | ProRes/DNxHR/H.264/HEVC decode | **LGPL-2.1+ — see §3.1** |
| NVIDIA Rivermax SDK | ST 2110 | Proprietary, per-node licence |
| NDI SDK | NDI send/receive | Proprietary, free with agreement |
| Blackmagic DeckLink SDK | SDI I/O | Proprietary, free |
| AJA NTV2 SDK | SDI I/O | Open source — verify current terms at import |
| Notch SDK | Notch blocks | Proprietary, commercial |
| OpenTrackIO C++ library | Tracking ingest | Open source — verify licence at import |

### 4.1 FFmpeg is a compliance trap

FFmpeg is **LGPL-2.1+**, and parts of it are GPL. Three rules, non-negotiable:

1. **Configure with `--disable-gpl --disable-nonfree`.** The GPL components (x264, x265) are encoders you do not need for a decode path. Enabling one makes your entire product GPL.
2. **Dynamically link.** LGPL static linking obliges you to provide relinkable object files. Shipping a DLL/`.so` is far simpler.
3. **Ship the licence text and a written offer for source.** Automate this — a CI step that regenerates the third-party notices file from the manifest, and fails the build if a dependency lacks a recorded licence.

Separately: shipping an H.264/HEVC decoder incurs patent-pool obligations independent of FFmpeg's licence. That is a Via LA conversation, not a code change. Budget it (architecture doc §13.3).

---

## 5. The 2D compositor and timeline

This is requirement 1, it is the surface operators actually touch, and it is the largest single body of work in the product — comparable to the renderer and with more design risk, because the renderer is a known problem with published solutions and this is a product-design problem.

### 5.1 Model: layer stack on top, DAG underneath

The internal representation is a directed acyclic graph of operators. The default UI is an After Effects–style layer stack, because that is what operators know and it is faster for most work. A "show graph" toggle exposes the DAG. **A precomp is a subgraph**, and any subgraph can be promoted to a reusable asset.

This is not a compromise — it is what lets you offer AE ergonomics without AE's limitations. A layer stack cannot express a multi-input operator or reuse one result in two places; a DAG can. The stack is a *view* over a DAG constrained to a linear chain.

```
LAYER STACK (what the operator sees)     DAG (what evaluates)

  ▸ 1  Title      [Glow][CC]              text ─▸ glow ─▸ cc ─┐
  ▸ 2  Logo       [Blur]      ◂ alpha     png  ─▸ blur ───────┤
  ▸ 3  Adjustment [Curves]                              curves─┼─▸ merge ─▸ out
  ▸ 4  Plate      [CC][Key]               plate─▸ cc ─▸ key ──┘
  ▸ 5  BG (precomp)                       [subgraph]──────────┘
```

### 5.2 Layer anatomy

Every layer carries, and every one of these is keyframeable and addressable from OSC/DMX:

- **Source** — media, image sequence, solid, gradient, text, shape, precomp, 3D scene render, 2110/NDI/capture input, Notch block
- **Transform** — anchor point, position, scale, rotation, skew, opacity. 3D layers add orientation and separate X/Y/Z rotation
- **Blend mode** — see §5.4
- **Effects stack** — ordered, each with its own parameter set
- **Masks** — multiple bezier paths per layer, each with feather, expansion, opacity, and a mode (add / subtract / intersect / difference). Mask paths are animatable
- **Track matte** — alpha, luma, and inverted variants, taken from the layer above
- **Parenting** — transform inheritance from another layer
- **Time** — in/out points, offset, stretch, and an optional time-remap curve
- **Motion blur** — per-layer toggle, sampled against the comp's shutter angle
- **Flags** — solo, lock, shy, adjustment-layer, 3D, collapse-transforms

**Adjustment layers** apply their effect stack to the composite of everything below them in the stack. In DAG terms this is an operator whose input is the accumulated merge below it — straightforward once the stack is understood as a view over a graph, and a nightmare if you implement the stack literally.

### 5.3 Alpha discipline

**Premultiplied everywhere internally. Straight alpha only at import and export boundaries.** Every format read applies an explicit unpremultiply-or-not decision recorded in the media metadata; every write applies the inverse.

Mixing conventions is the single most common source of dark-fringe and bright-fringe bugs in compositors, and the failures are subtle enough to survive review and ship. Make it a type-level distinction — `PremulImage` and `StraightImage` as distinct types that only convert through explicit functions — so the compiler catches it rather than the client.

### 5.4 Blend modes, and the linear/legacy problem

Ship the full set: normal, dissolve, the Porter-Duff operators, and the creative set — multiply, screen, overlay, soft light, hard light, colour dodge, colour burn, linear dodge/burn, darken, lighten, difference, exclusion, subtract, divide, hue, saturation, colour, luminosity.

**The subtlety that will cost you a week if you miss it:** these modes are *defined* on gamma-encoded values in Photoshop and After Effects. Your working space is linear scene-referred. `multiply` in linear does not look like `multiply` in AE, and a designer handing you a comp will file it as a bug.

Provide both, explicitly, per layer with a comp-level default:

- **Linear blend** — mathematically correct, matches physical light. Correct for anything the camera will photograph.
- **Legacy/perceptual blend** — encode to the working display transform, blend, decode back. Matches AE and Photoshop. Correct when the goal is "this looks like the designer's mockup."

Label them in the UI in those terms, not as "linear" versus "sRGB," because the operator's question is which one matches their reference.

### 5.5 Evaluation: pull-based, with ROI, cache, and fusion

This is the core engineering of the compositor and the part that determines whether it runs live at 11K or not.

**Pull-based evaluation.** Request `(node, region, frame)` at the output; recursively request what each input needs. Combined with ROI propagation (§8.3), each node computes only the pixels its own wall region requires plus accumulated apron.

**Content-keyed caching.** The cache key for any intermediate is a hash of:

```
  node identity
+ every upstream parameter value at frame T
+ upstream source content hashes
+ requested region
```

A still image with a colour correction on it hashes identically every frame until someone touches the CC — so it is computed once and reused indefinitely. An animated layer re-evaluates. **This is what makes a forty-layer comp affordable live**, and it is why the parameter system must be able to produce a cheap stable hash of its own state.

**Shader fusion.** A chain of pointwise operators — exposure → curves → LUT → saturation — must compile into a *single* GPU dispatch, not four passes with four intermediate targets. Partition each effect chain into fusion groups, broken only by operators that need neighbouring pixels or a full-frame reduction.

The fusion boundaries and the ROI expansion boundaries are the same boundaries: a pointwise operator neither expands ROI nor breaks fusion; a spatial operator does both. Implement it once.

```cpp
// engine/comp/eval.hpp
struct EvalKey {
    NodeId    node;
    FrameTime time;
    Region    region;
    u64       param_hash;    // upstream params + source content hashes
};

// Groups of ops fused into one dispatch. Boundaries fall exactly
// where an operator needs pixels other than its own.
struct FusionGroup {
    std::span<const OpId> ops;
    bool breaks_fusion;      // true for spatial / reduction operators
};
```

### 5.6 Effect taxonomy

Implementation strategy differs per class, so classify at the type level:

| Class | Examples | ROI | Fusion | Notes |
|---|---|---|---|---|
| **Pointwise** | Exposure, curves, levels, HSL, LUT, chroma/luma key, tint | 1:1 | Fuses | Cheap. The bulk of daily use |
| **Spatial** | Blur (box/gaussian/directional/radial/lens), sharpen, glow, edge | Expands by radius | Breaks | Separable where possible. Radius drives apron |
| **Warp** | Corner pin, mesh warp, displacement, distortion, optical flow | Arbitrary — needs inverse map | Breaks | ROI must be computed from the inverse transform, conservatively bounded |
| **Temporal** | Echo, frame blend, time displacement, motion blur | 1:1 spatially, multi-frame | Breaks | Needs N source frames — interacts hard with media prefetch |
| **Generator** | Gradient, noise, fractal, solid, text, shape | No input | Fuses if pointwise | Procedurals should evaluate at the requested region only |
| **Reduction** | Histogram, auto-levels, scopes, average | Full frame | Breaks | Requires readback — schedule off the critical path |

**Warp ROI is the one that bites.** For an arbitrary displacement you cannot analytically invert the map, so bound it conservatively by the maximum displacement magnitude. Get this wrong and you get seams at node boundaries under displacement — which the §10.3 seam matrix is designed to catch.

### 5.7 Text and shape layers are sub-projects, not features

Do not let these hide inside "the compositor" on a schedule.

**Text layers** need HarfBuzz shaping, an SDF or MSDF glyph atlas, per-character/word/line animators with range selectors, path text, and full paragraph layout. Realistically three to four engineer-months for a version that satisfies a motion designer.

**Shape layers** need bezier path representation, stroke and fill with gradients, boolean path operations, trim paths, repeaters, and offset paths. Similar magnitude.

Both are high-visibility — they are what an operator uses to build a lower third at 4pm on show day — and both are commonly under-scoped. Give them their own line items.

### 5.8 The timeline

**Time model.** Three frame rates coexist and conflating them is a classic bug: the **show** rate (genlock, from §3 of the architecture doc), the **comp** rate, and each **source's** rate. Conforming and pulldown are explicit operations, never implicit.

**Drop-frame timecode must be exactly right.** 29.97 and 59.94 drop-frame is not "approximately 30fps" — it is a specific counting rule that skips frame numbers 0 and 1 at each minute except every tenth. Broadcast clients notice immediately. Implement it once, in a well-tested time library, and never do timecode arithmetic ad hoc.

**Keyframes and curves.** Per-parameter keyframe lists with interpolation types: linear, bezier with independent in/out tangents, hold, and ease presets. Position parameters additionally need **spatial** interpolation — a bezier path through space, separate from the temporal easing curve, with roving keyframes that redistribute for constant velocity. The curve editor must expose both, and operators will judge the whole product by whether the tangent handles feel right.

**Nested time.** A precomp has internal time; its placement in the parent has offset, stretch, and an optional remap curve. Time remapping composes down the nesting chain. Define the composition rule once and test it at three levels of nesting, because that is where implementations fall apart.

**Transport and cues.** Play, pause, scrub, loop, in/out, per-layer offset and stretch, markers, and named cues addressable from show control (OSC/MSC). Scrubbing must be frame-exact against media, not nearest-keyframe.

### 5.9 Live operation — what differentiates this from After Effects

- **No render step.** The comp is evaluated every frame at show rate. There is no preview-versus-final distinction.
- **Every edit is a command.** Parameter changes, layer reordering, effect insertion — all go through the frame-scheduled bus (§8.2) and land on every node at the same frame. Drag a slider, the whole wall changes together four frames later, never torn.
- **Frame budget is enforced, not hoped for.** Every operator carries a measured cost estimate. The comp's projected cost is displayed live, and exceeding budget triggers the degradation ladder (architecture doc §10) rather than dropping frames. An operator who adds a fifth 200px blur should see the budget bar go red *before* the show does.
- **Distributed by construction.** Each node evaluates only its own region. The operator authors against a full-canvas view and never thinks about slicing — but §10.3's seam tests must pass for that abstraction to hold.

### 5.10 Plugin strategy: consider OpenFX

Worth a genuine evaluation early, because the decision shapes the effect ABI.

**OpenFX** is the standard plugin API for compositors — Nuke, Resolve, Flame, Vegas and Natron all host it. Supporting it would give day-one access to BorisFX, Sapphire, Neat Video and a substantial commercial ecosystem. "Your existing Sapphire licences work" is a strong argument to a facility that already owns them.

**The caveats are real:** OFX is CPU-oriented by default, with GPU rendering available through the CUDA/OpenCL suites rather than as the primary path; it was designed for interactive and offline work, not a hard frame deadline; and third-party plugin quality varies enough that process isolation (§12, rule 5) becomes mandatory rather than merely wise.

Recommendation: **build your own C-ABI effect SDK as the primary path** — it can be GPU-native, ROI-aware, and fusion-participating in ways OFX cannot — and evaluate an OFX *host* bridge as a Phase 3 compatibility layer, running out of process, for pointwise and spatial effects where the frame budget allows. Do not make OFX the primary ABI; do not dismiss it either.

---



## 6. Extracting the Spartan renderer

> **Deferred to the 3D phase (§0.3).** This section is unchanged and still
> correct, but it is no longer the next thing. The first RHI is SDL_GPU behind
> an `engine/rhi` seam, which is what V2 and V3 build on. Come back here when
> the 3D phase starts and the PBR renderer, frame graph and DDGI ports are what
> is actually wanted. Treat the week numbers below as relative to that start,
> and budget them at two to three times face value against this codebase's
> standards.

### 6.1 Establish a baseline (week 1)

Fork, build, run the sample worlds on your actual target hardware. Capture a RenderDoc frame and a Tracy trace. Record GPU timings per pass. **You cannot tell later whether you broke something if you never measured it working.**

Record the import commit in `IMPORT.md`. Everything after this is a tracked divergence.

### 6.2 Strip, in dependency order (weeks 2–5)

Delete in this sequence so the tree compiles at every step:

| Order | Remove | Why |
|---|---|---|
| 1 | Worlds, car simulation, vehicle dynamics | Largest, most isolated, depends on PhysX |
| 2 | PhysX integration, character kinematics | No physics requirement |
| 3 | Lua / Sol2 scripting | Your parameter system replaces it |
| 4 | SDL3 audio, procedural synthesis | Not a media server concern at this layer |
| 5 | Skeletal animation, IK, foot planting | Comes back later via USD if ever |
| 6 | Procedural roads, terrain, FFT ocean, clipmap | Set dressing |
| 7 | Cinematics sequencer | Your timeline (§8 of arch doc) replaces it |
| 8 | Entity system, prefabs, XML serialisation | **Replaced by the immutable document model** |
| 9 | ImGui editor | Replaced by `/ui` in Phase 2; keep ImGui for debug overlays |
| 10 | Assimp, FreeImage, FreeType asset import | Replaced by USD/MaterialX + OpenImageIO |
| 11 | Window creation, input, main loop | Replaced by the command-bus frame driver |

Expect to remove roughly 60% of the line count. **Do not "keep it in case" — dead code you don't understand is worse than code you deleted.** It is in git history if you need it.

### 6.3 What survives — this is what you forked for

- **The shader library.** BSDF, ReSTIR reservoirs, RT passes, volumetrics, TAAU, atmospheric scattering. This is the crown jewel and the reason the fork beats writing from scratch.
- **The universal HLSL build.** One shader source compiling to both SPIR-V and DXIL. Preserve this build system intact; do not "improve" it in year one.
- **The RHI**, both backends.
- **Bindless descriptor and resource management**, the global vertex/index buffer, vertex pulling.
- **GPU-driven indirect pipeline**: meshlet clustering, per-meshlet frustum culling, Hi-Z occlusion, backface cone culling.
- **Frame structure and async compute scheduling.**
- **Profiler hooks, GPU breadcrumbs, RenderDoc integration.**

### 6.4 The critical seam: scene upload

Spartan is bindless and GPU-driven — geometry, materials, textures, lights, transforms and AABBs live in persistent global buffers. That is a *clean* seam, and it is where your document model meets the renderer.

Define exactly one interface and never let the renderer reach past it:

```cpp
// engine/scene/scene_upload.hpp
//
// The ONLY path from document state into renderer-visible GPU memory.
// The renderer never reads the document. The document never knows about GPU.
class SceneUpload {
public:
    // Called once per frame, before the frame graph is built.
    // 'delta' is the structural diff between last frame's document
    // root and this frame's — usually tiny, which is the point.
    void apply(const doc::Root& root, const doc::Delta& delta, FrameArena&);

    // Stable handles into the bindless tables.
    MeshHandle      upload_mesh(const scene::Mesh&);
    MaterialHandle  upload_material(const scene::Material&);   // from MaterialX
    TextureHandle   upload_texture(const scene::TextureDesc&);
    LightHandle     upload_light(const scene::Light&);

    // Views the renderer will draw this frame. One per wall region
    // this node owns, plus the inner frustum if this node renders it.
    std::span<const ViewDesc> views() const;
};
```

Because the document is a persistent tree (§8), `Delta` is cheap to compute and usually touches a handful of nodes. Uploading only the delta is what keeps a 50,000-object set at frame rate.

### 6.5 Replace the frame driver (weeks 6–8)

Spartan's main loop becomes a function the command bus calls:

```cpp
// Driven by the frame scheduler, not by a while(!quit) loop.
void Node::on_frame(FrameNumber n, const ShowClock& clock) {
    frame_arena_.reset();
    cmd_bus_.apply_scheduled(n);            // §7 — all state changes land here
    auto root = doc_.current_root();         // O(1) — pointer to immutable tree
    scene_upload_.apply(root, doc_.delta_since(last_root_), frame_arena_);
    tracking_.sample_for(n, clock);          // interpolated, prediction applied
    render_.build_and_execute(frame_arena_); // frame graph
    comp_.composite(frame_arena_);           // 2D graph over all sources
    output_.present(n);                      // feed map → warp → LUT → out
    last_root_ = root;
}
```

Everything after this milestone is additive. Get here fast.

---

## 7. Ports from Atlas and hybrid-rendering

### 7.1 Atlas — DDGI and the embedding pattern

**Port the DDGI implementation** for outer-frustum GI. Per architecture doc §1.4, the outer frustum spans node boundaries and cannot use ReSTIR's temporal reservoirs without seams. DDGI with replicated probe state, updated identically on every node from the same command bus input, is the deterministic answer. Atlas's is the cleanest MIT implementation to work from.

The probe volume must be driven entirely by document state, never by per-node wall-clock or frame timing. Probe update order, ray directions, and hysteresis all seed from `(frame_number, probe_index)`.

**Study — do not copy — the `ATLAS_NO_APP` / `ATLAS_HEADLESS` structure.** It is the pattern for an engine that does not own `main()` and does not require a window. Spartan does own both; Atlas shows what the seams look like once removed. This informs §6.5 rather than producing ported code.

### 7.2 hybrid-rendering — the denoisers

Dihara Wijetunga's sample is the cleanest MIT reference for RT shadows, AO, reflections and GI composed into a deferred pipeline. **Port the denoiser passes and the pass composition structure**, not the sample framework.

Evaluate against NVIDIA's NRD before committing: NRD is battle-tested and free, and for the denoising stage specifically, robustness beats architectural purity. Port hybrid-rendering's passes if NRD's integration cost or its determinism characteristics prove worse.

### 7.3 R3D — shader math only

zlib, so lift freely. Its IBL prefilter and SSGI/SSR implementations are worth reading against Spartan's for a second opinion where Spartan's are hard to follow. Reference material, not a port target.

### 7.4 Provenance discipline

Every ported file carries a header: source project, upstream commit hash, original licence text, and a one-line note on what was changed. Automate the third-party notices file from these headers. This takes ten minutes per file and saves a legal review later.

---

## 8. Core type sketches

### 8.1 Document model

```cpp
// engine/doc/ — persistent (structurally shared) tree.
// Mutation returns a new root sharing all untouched subtrees.

using NodeId  = StrongId<struct NodeIdTag, u64>;
using Version = StrongId<struct VersionTag, u64>;

class Root {                       // one pointer; copying is free
    Hamt<NodeId, NodeRef> nodes_;  // hash array mapped trie
    Version               version_;
public:
    const Node* get(NodeId) const noexcept;
    [[nodiscard]] Root set(NodeId, Node) const;   // returns NEW root
    Version version() const noexcept { return version_; }
};

// Diffing two roots is O(changes), not O(document) — shared subtrees
// compare by pointer identity and are skipped wholesale.
Delta diff(const Root& from, const Root& to, FrameArena&);
```

Four requirements fall out of this one structure for free: O(1) snapshots, cheap deltas to nodes, undo as retained old roots, and lock-free concurrent read from the render thread while the UI thread writes.

### 8.2 Command bus

```cpp
struct Command {
    CommandId     id;
    Version       base;      // authored against this version
    FrameNumber   apply_at;  // show-clock frame, NOT "now"
    TransactionId txn;       // grouping for undo
    Payload       forward;
    Payload       inverse;
};

class CommandBus {
public:
    void submit(Command);                  // director only
    void apply_scheduled(FrameNumber n);   // every node, at frame boundary
    void undo(TransactionId);              // broadcasts inverse as a new command
};
```

Lookahead of 4–8 frames. Node management, transport, parameter edits, and push-to-live are all the same command type — there is no second control protocol.

### 8.3 ROI propagation

The mechanism that makes an 11K wall tractable (architecture doc §6.1).

```cpp
// engine/comp/roi.hpp
struct Region { i32 x, y, w, h; };

// Walk the comp graph BACKWARDS from this node's output region.
// Each spatially-extended operator expands by its kernel radius;
// expansions accumulate down a chain.
Region required_input_region(const CompGraph&, NodeHandle source,
                             Region output_region);

// A 60px blur followed by a 40px glow needs 100px of apron.
// Guessing a fixed margin is how you ship seams that only appear
// when someone adds an effect on a shoot day.
```

Test this harder than almost anything else — see §10.3.

---

## 9. Workstreams and staffing

Nine long-range tracks, ~18 engineers. For the current V0-V2 slice, Core,
Media, UI, and the minimal compositor/playback seam work concurrently. The
full renderer and infrastructure tracks continue where they directly unblock
that vertical path; broad subsystem expansion resumes after the playable demo.

**Track H is co-equal with Track B in importance and is the one most often under-staffed.** The renderer is a solved problem with published literature; the compositor is a product-design problem with more unknowns and it is the surface customers evaluate you on.

| Track | Scope | Eng | Starts |
|---|---|---|---|
| **A — Core** | Allocators, jobs, math, document model, command bus | 2 | Week 1 |
| **B — Renderer** | Spartan fork, strip, RHI, frame graph, scene upload, DDGI + denoiser ports | 3 | Week 1 |
| **C — Media** | Decode, frame cache, ROI propagation, slicing, content store, disk I/O | 2 | Now - V1 owner |
| **D — Net & I/O** | Rivermax, PTP, NDI, DeckLink, sync stack, cluster state | 2 | Week 1 |
| **E — UI** | Thin SDL3 operator shell first; later vector renderer, text shaping, timeline, node graph, and feed-map canvas | 3 | Now - V0/V2 owner |
| **H — Compositor** | One-layer transform/opacity seam first; later comp graph, cache/fusion, effects, keyframes, and curves | 3 | Now for V2; full track Month 4 |
| **I — Text & shapes** | Text layers with animators, shape layers with trim/repeat | 1–2 | Month 10 |
| **F — ICVFX** | OpenTrackIO, FreeD, lens model, frustum, calibration tools | 2 | Month 14 |
| **G — Infra** | Build, CI, HIL rig, test harnesses, telemetry | 1 | Week 1 |

Track B is the longest technical pole and the hardest to hire for. If you engage Spartan's author (architecture doc §13.2), that is the track he joins, and it is worth reordering the schedule around his availability rather than the reverse.

---

## 10. Test strategy

### 10.1 Unit — Tracks A, C and H
Document model invariants (persistence, diff correctness, undo round-trip), command ordering under reordered delivery, ROI arithmetic, colour transform round-trips, keyframe interpolation, drop-frame timecode arithmetic, cache-key stability. Fast, on every commit, with ASan+UBSan.

### 10.2 Golden image — render passes
Reference images per pass with a perceptual tolerance, not bit-exact — GPU float differs across drivers and vendors. Regenerate deliberately, review diffs in PRs. Catches the shader refactor that subtly changed energy conservation.

### 10.3 Seam tests — the one that protects you on a shoot day

```
Render canvas C on ONE node at full resolution.
Render canvas C sliced across FOUR simulated nodes.
Composite the four slices.
Diff the seam regions.

Run this matrix:
  · no effects                     (baseline)
  · 60px blur                      (single-operator apron)
  · blur → glow → displacement     (accumulated apron)
  · effect chain crossing 3 nodes  (multi-boundary)
  · 11K plate, dynamic feed map    (re-slice under change)
```

This runs in CI on every change to `/comp` or `/media`. It is the highest-value test in the suite because the bug it catches is latent — seams that appear only when someone adds an effect, which means they appear in front of a client.

### 10.4 Compositor correctness

Three suites that pay for themselves repeatedly:

- **Alpha round-trip.** Every format read → composite → write, checked for premultiply consistency. Catches fringe bugs before a client does.
- **Blend-mode reference.** Every mode in legacy mode compared against a Photoshop/AE-generated reference set, within tolerance. This is how you make "matches my mockup" a testable claim rather than an argument.
- **Cache correctness.** Evaluate a comp with caching disabled and enabled; results must match bit-for-bit. A stale-cache bug shows up as content that silently fails to update on one node — the hardest class of bug to diagnose live.

### 10.5 Determinism — within a node
Render frame *N* twice from the same document version and diff. Bit-exact expected. Catches unseeded RNG, wall-clock dependencies, and non-deterministic parallel reductions before they become an eight-hour debugging session.

### 10.6 Soak
Eight-hour and twenty-four-hour runs with full telemetry. Assert zero dropped frames, no VRAM growth, no descriptor leak, stable frame time percentiles. Nightly on the HIL rig.

### 10.7 Hardware-in-the-loop
A permanent rack: four nodes, real GPUs with Quadro Sync, real ConnectX NICs on a PTP-aware switch, real DeckLink cards, and a capture path that reads back what actually came out of the connector. Expensive. Non-negotiable — every bug that matters in this product lives in hardware interaction, and none of them reproduce on a developer laptop.

---

## 11. Milestones — first 19 months

Each has a binary acceptance test. "Mostly working" is not a milestone.

The V0-V3 gates in §0 are the current delivery sequence and precede further
horizontal subsystem expansion. In particular, V1 must use real imported media
and V2 must expose it through an operator-facing GUI. The M-series below remains
the long-range production roadmap; its prototype media/UI pieces are pulled
forward into V0-V2 and are hardened or replaced at their original milestones.

| M | Month | Deliverable | Acceptance test |
|---|---|---|---|
| **M0** | 1 | Repo, CMake, CI, HIL rig, Spartan baseline captured | Fork builds and runs sample worlds on target hardware; Tracy trace and RenderDoc capture archived |
| **M1** | 3 | Spartan stripped to renderer core; document model and command bus | Renderer draws a USD scene driven by document state; strip removes ≥50% of LOC with no visual regression against M0 goldens |
| **M2** | 4 | `VK_NV_present_barrier` validated on target driver | **Two genlocked nodes, scrolling pattern, stable measured frame relationship over 8h.** If this fails, fall back to a D3D11/GL presentation surface — decide here, not in year two |
| **M3** | 6 | Rivermax ST 2110 receive; feed map, warp, blend, calibration LUT; DP + SDI out | Receives your existing Unreal nDisplay 2110 output and drives the LED processors correctly |
| **M4** | 9 | **Wedge product ships** — thin receiver role, hardened | A real shoot runs with Unreal rendering and your nodes driving the processors |
| **M5** | 11 | Media decode: HAP, ProRes, EXR/DPX sequences; frame-exact seek | Four-node UHD wall plays a synchronised plate, 8h, zero drops |
| **M6** | 13 | **ROI propagation and per-node slicing** | **11K plate across four nodes with a blur→glow chain, seam diff under tolerance** (§10.3) |
| **M7** | 14 | Comp graph: pull evaluation, content-keyed cache, shader fusion; pointwise + spatial effect set | 40-layer comp with mixed pointwise and blur chains holds show rate at UHD/node; cache hit rate reported and stable when only one layer animates |
| **M8** | 15 | Timecode/time model, keyframes, curve editor, nested comp time | Drop-frame 29.97 arithmetic passes a conformance suite; time remap composes correctly through three levels of nesting |
| **M9** | 16 | Layer model complete: masks, track mattes, adjustment layers, parenting, blend modes (linear + legacy) | Reference AE project rebuilt by an operator and visually matched in legacy blend mode |
| **M10** | 17 | Operator UI v1: timeline, layer stack, effect controls, live editing over the bus | Operator builds and revises a multi-layer show live; edits land within 8 frames, glitch-free, on all nodes; frame-budget bar reflects real cost |
| **M11** | 18 | Presets, undo/redo, Stage/Live branching, A/B compare | Bad change rolled back in one action mid-show; push-to-live gated on content-ready across all nodes |
| **M12** | 19 | DDGI port; outer-frustum 3D across nodes | Two adjacent nodes render a continuous 3D surface with no visible seam over a 30-minute camera move |

Months 19–42 follow the architecture doc's Phases 3 and 4: ICVFX (Track F), Notch hosting, redundancy, show control, hardening.

Text and shape layers (Track I) run in parallel from month 10 and land alongside M10; they are not gated by the comp graph beyond the effect SDK.

**M2, M6 and M7 are the three that can invalidate large parts of the plan.** M7 is the compositor's equivalent of M2: if pull-evaluation with caching and fusion cannot hold a 40-layer comp at show rate, the whole live-editing premise needs rethinking, and month 14 is when you want that news. Both are scheduled early on purpose. If present-barrier support is broken on your driver stack, you want to know in month 4. If ROI slicing cannot hold a seam at 11K, you want to know in month 13, not when a client is standing behind the operator.

---

## 12. Standing rules

1. **No allocation below the frame boundary.** Enforced by hook, not by review.
2. **The renderer never reads the document.** Everything goes through `SceneUpload` (§6.4).
3. **Every state change is a command.** No side channels, no "just this once" direct mutation. This is what makes undo, cluster sync, and push-to-live one mechanism instead of three.
4. **Every ported file carries provenance.** Source, commit, licence, changes.
5. **Third-party code runs out of process.** Notch, plugins, and exotic codecs live in `/apps/sandbox`. A bad Notch block degrades to a black layer and an operator alert; it does not take down a node.
6. **Determinism tests run in CI.** The moment they are allowed to fail "temporarily," they are gone.
7. **Measure before optimising, and after stripping.** M0's baseline trace exists so M1 can prove it did not regress.
