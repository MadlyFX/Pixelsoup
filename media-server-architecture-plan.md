# Media Server Platform — Architecture & Delivery Plan

**Target domain:** virtual production / ICVFX, LED stages, live events
**Class of product:** disguise, Pixera, Modulo Kinetic, Smode, Ventuz, Green Hippo — plus a real-time 3D renderer competitive with nDisplay.

---

## 0. Two things to settle before anything else

### 0.1 Renderer selection

This is the largest single line item in the budget, so it is worth being precise about.

#### R3D (Bigfoot71) — good code, wrong foundation

[R3D](https://github.com/Bigfoot71/r3d) is a 3D extension library for raylib: C, zlib-licensed, hybrid deferred + forward, Burley/SchlickGGX PBR, IBL with reflection probes, SSAO/SSGI/SSR/DoF/bloom, Assimp model loading, and an Odin binding. It is genuinely well-made and the feature list is close to what you need.

Two properties disqualify it **as a dependency**:

1. **OpenGL 3.3 core.** GL 3.3 gives you no explicit barriers or frame graph, no timeline semaphores (you need those for the cluster commit barrier, §3), no external-memory interop (you need that to import Notch's D3D11 textures and to serve Spout, §7.1), no GPUDirect, no ray tracing, no async compute, and no control over a 10/12-bit or HDR swapchain. Every one of those is load-bearing here.
2. **It sits on raylib.** raylib owns the window, the GL context, input, and the main loop. This application has a bespoke UI, N offscreen render targets feeding SDI/2110/NDI simultaneously, and multiple independently-clocked viewports. You would be fighting raylib within weeks, or forking the renderer out of it immediately.

Secondary gaps: Assimp rather than USD; no shutter-angle-matched motion blur (required for inner-frustum plate matching); sRGB-ish colour rather than OCIO scene-referred; pre-1.0 with stated API breakage; effectively a single maintainer.

**But the zlib licence makes it valuable in a different role.** You can read it, lift from it, and port it with zero legal friction. Its IBL prefilter, GGX/Burley implementation, and SSGI/SSR passes are exactly the pieces that eat weeks to get right from papers. Treat R3D as a **reference implementation to port from**, not a library to link against. That is a real schedule accelerator.

One honest point in GL's favour, worth knowing: `WGL_NV_swap_group` / swap barrier is the *oldest and best-tested* framelock path — more mature than `VK_NV_present_barrier`. GL is the one thing R3D would make easy. It is not enough.

#### Iray — worse fit, for a structural reason

Iray is a **progressive path tracer**. It converges over time. Your budget is a hard 1/24s–1/60s deadline, with a moving camera and content changing every frame. You would ship noise, or ship a denoiser.

The denoiser is the actual blocker, not the noise. OptiX-class denoising is temporally unstable and — critically — very hard to make **bit-identical across nodes**. The entire cluster design in §1 rests on determinism. Adaptive sampling plus a temporal denoiser is close to the worst possible case for that requirement.

Add: NVIDIA commercial licensing, NVIDIA-only lock-in, MDL as the material language (your art department speaks USD and MaterialX, not MDL), and you do not own the frame boundary.

**Where a path tracer does belong: offline baking.** Bake lightmaps, irradiance volumes, and reflection probes for the stage set; bake the static outer-frustum environment. That is genuine value and it is how a lot of high-end VP work actually gets made. Phase 4+, and a renderer you own — or Cycles — does it without the NVIDIA licensing conversation.

#### Candidate evaluation

Filament is out: no GI, no ray tracing, mobile-first tuning. It cannot reach a photoreal bar.

Assessed against four criteria — **photoreal ceiling** (real GI, HW ray tracing), **embeddability** (does it let you own the window, main loop, and frame boundary), **licence**, and **maintenance reality**:

| Project | API | GI / RT | Embeddable | Licence | Verdict |
|---|---|---|---|---|---|
| **SpartanEngine** | Vulkan + D3D12, universal HLSL | **ReSTIR path-traced GI**, HW RT reflections + shadows via ray queries | Partial — owns the loop, but cleanly layered | MIT | **Strongest starting point.** |
| **Atlas-Engine** | Vulkan | DDGI, HW + SW RT, path tracing | **Yes** — `ATLAS_NO_APP` and `ATLAS_HEADLESS` build flags | MIT | **Best architectural fit.** Self-described "toy engine", v0.2.0. |
| **hybrid-rendering** | Vulkan | RT shadows, AO, reflections, GI in a deferred pipeline | No — it is a sample | MIT | **Best reference implementation.** Port from it. |
| **EveryRay** | D3D11 + D3D12 | Voxel cone tracing GI, IBL probes. No HW RT. | **No** — author states it is not a library and not modular | MIT | Read the RHI design; don't depend on it. |
| **VQEngine** | D3D12 | None. IBL + SSSR only. MSAA-only AA. | n/a | **GPL-3.0** | **Disqualified.** Copyleft kills commercial use. |
| **Veselov RayTracing** | Vulkan + D3D12 | Wavefront path tracer, ray query, CPU BVH fallback | No | MIT | Same convergence problem as Iray. Useful for its dual-backend abstraction and AOV generation. |
| **ZEngine** | Vulkan | None advertised | Partial | MIT | Earliest stage of the seven; .NET 8 + PowerShell build chain. Skip. |

#### The uncomfortable conclusion

**No open-source renderer is simultaneously photoreal-capable, architecturally embeddable, and production-maintained.** You get two of three. Every project above is one person's work, and the two best ones say so explicitly in their READMEs — Spartan is "a personal R&D engine, not a commercial product. No roadmap promises, no support queue."

So the decision is not *which library to depend on*. It is **which codebase to fork and take permanent ownership of**. MIT makes that legal. The question is whether it is a codebase you would want to own.

**Recommendation: fork SpartanEngine as the starting point.**

What it brings that matters here:
- ReSTIR path-traced GI and HW ray-traced reflections/shadows — a real photoreal ceiling
- Universal HLSL compiled to both SPIR-V and DXIL, so Vulkan **and** D3D12 from one shader source. D3D12 is the cleaner host for the D3D11 interop that Notch and Spout require (§7.1)
- Physically based camera with lumens and kelvin, ACES/AgX tonemapping, HDR10 output — the right vocabulary for matching a cinema camera
- TAAU with Halton jitter, motion blur, async compute, bindless GPU-driven rendering
- GPU crash breadcrumbs for post-mortem debugging, which is a production mindset most hobby engines lack

Evidence the code is liftable: Godot uses Spartan's TAA resolve, and a S.T.A.L.K.E.R. Anomaly rendering addon is built on its source.

**Strip aggressively on day one.** Spartan is a *game* engine; roughly 60% of it is scaffolding you will fight. The twelve years of value are almost entirely GPU-side. Be ruthless about the split:

| Keep — this is what you're buying | Delete immediately |
|---|---|
| Shader library (BSDF, ReSTIR reservoirs, RT passes, volumetrics, TAAU) | PhysX, vehicle dynamics, character kinematics |
| Bindless descriptor and resource management | Lua/Sol2 scripting |
| GPU-driven indirect pipeline, meshlet culling, Hi-Z | SDL3 audio |
| RHI and the universal HLSL → SPIR-V/DXIL build | Skeletal animation, IK |
| Frame structure and async compute scheduling | ECS + XML serialization (replaced by §8's document model) |
| Profiler, breadcrumbs, RenderDoc hooks | ImGui editor (replaced by §9) |
| | Assimp (replaced by USD/MaterialX) |
| | The main loop and windowing (replaced by §2's command bus) |

The CPU-side scaffolding is both where your requirements diverge most from a game engine's and where rewriting is cheapest. The GPU-side is where deep understanding is expensive and modification is dangerous — which is exactly the argument for §13.

Then bolt on what it lacks: OCIO colour management (it has tonemappers, not a managed pipeline), USD/MaterialX ingest, deterministic frame boundaries, and the frustum-split GI strategy in §5.2.

**Take Atlas's DDGI and `ATLAS_HEADLESS`/`ATLAS_NO_APP` structure as the model for embedding**, and **port the denoisers and RT passes from hybrid-rendering**, which is the cleanest MIT reference for exactly this hybrid architecture.

**The honest alternative:** if photorealism is the top priority and time-to-market matters more than owning the stack, Unreal + Lumen as a slave process is the only thing that clears the bar off the shelf today, and it is what the industry has largely settled on. You pay for it in determinism, royalties, and not owning the frame. That trade is defensible; it is just a different product.

### 0.2 C++ vs Odin

You asked for either. **Choose C++23 for the core.** Odin is a good language, and I would happily use it for tooling, but the deciding factor is not language quality — it is vendor SDK surface area:

- **Blackmagic DeckLink SDK** — C++ with a COM-style `IUnknown` interface model. Consuming this from Odin means hand-writing vtable layouts and reference counting for ~80 interfaces, and re-doing it on every SDK bump.
- **AJA NTV2 SDK** — C++ classes, heavy template use.
- **Notch C API** — C ABI, fine from Odin, but the host process must be a D3D11 device (see §7).
- **NVAPI, CUDA, NVENC/NVDEC, Rivermax, NDI, OpenColorIO, OpenEXR, FFmpeg** — C or C-ABI, fine from either.

The DeckLink and AJA SDKs alone represent a permanent, recurring integration tax that C++ eliminates. Add to that: shipping-quality profilers (Superluminal, Nsight, PIX, Tracy), sanitizers, and a hiring pool of people who have built real-time graphics systems before.

**If you want Odin anyway**, the workable shape is: a thin C++ `libdevice` shim exposing a flat C ABI over every vendor SDK, with all application logic, the document model, the scene graph, and the UI in Odin. Odin's SIMD intrinsics, `context` system, and lack of hidden allocation are genuinely nice for a frame-budget-sensitive app. Budget ~2 extra engineer-months up front and accept that you will occasionally be the first person to hit a given bug.

The rest of this plan is language-agnostic; it says "C++" where it matters.

---

## 1. System topology

**Design principle: separate *who renders* from *who outputs*, and make roles composable.**

Three roles, one binary, selected at startup. Any machine can hold one, two, or all three. A small show runs all three on one box; a volume separates them. The software should not care.

```
┌─────────────────────────────────────────────────────────────┐
│  DIRECTOR (operator workstation)                            │
│  • Authoritative document store (versioned, immutable)      │
│  • Full UI: timeline, compositor, 3D viewport, feed map     │
│  • Command bus origin, transport master, show clock         │
└──────────────┬──────────────────────────────────────────────┘
               │  state deltas + frame-scheduled commands (~kB/frame)
               │  content sync (content-addressed, per-node slices)
     ┌─────────┼──────────┬──────────────────┬────────────────┐
     ▼         ▼          ▼                  ▼                ▼
┌──────────┐ ┌──────────┐ ┌──────────┐ ┌──────────┐   ┌──────────────┐
│ NODE 1   │ │ NODE 2   │ │ NODE 3   │ │ NODE 4   │   │ FRUSTUM      │
│          │ │          │ │          │ │          │   │ RENDERER     │
│ renders  │ │ renders  │ │ renders  │ │ renders  │   │ (may be one  │
│ own wall │ │ own wall │ │ own wall │ │ own wall │   │  of the four)│
│ region   │ │ region   │ │ region   │ │ region   │   │              │
│    +     │ │    +     │ │    +     │ │    +     │   │ inner        │
│ composites│ │ composites│ │ composites│ │ composites│  │ frustum      │
│ frustum  │ │ frustum  │ │ frustum  │ │ frustum  │   │ @ camera res │
└────┬─────┘ └────┬─────┘ └────┬─────┘ └────┬─────┘   └──────┬───────┘
     ▲            ▲            ▲            ▲                │
     └────────────┴────────────┴────────────┴────────────────┘
              inner frustum over ST 2110 (multicast, PTP)
              ~20 Gb/s — bounded by CAMERA, not wall

     │            │            │            │
     ▼            ▼            ▼            ▼
  LED proc     LED proc     LED proc    Projector/scaler
```

### 1.1 The governing principle: transport what is camera-bounded, distribute what is wall-bounded

Neither pure state replication nor pure pixel transport survives contact with a real volume. The correct model picks per source, and the deciding question is **what bounds the source's resolution.**

Work the numbers on an 11K wall — 11520 × 2160, 10-bit RGB, no chroma subsampling:

| Source | Bounded by | At 60p | At 24p |
|---|---|---|---|
| Full-wall 2D plate | **Wall** (24.9 Mpx) | ~45 Gb/s | ~18 Gb/s |
| Taller volume (11520 × 4320) | **Wall** (49.8 Mpx) | ~90 Gb/s | ~36 Gb/s |
| Inner frustum, 4K + 15% overscan | **Camera** (11.0 Mpx) | ~20 Gb/s | ~8 Gb/s |

A single full-wall plate at 60p does not fit in a 100 GbE link. Add a ceiling and side walls and you are past 150 Gb/s for content that four machines could have produced locally with no network traffic at all. Meanwhile the inner frustum is bounded by *camera* resolution regardless of how large the wall grows — it fits comfortably in 25 GbE and scales with camera count, not wall count.

So:

- **Distribute** anything wall-bounded. Each node computes only its own crop. Network pixel cost: zero. Cost scales with node count, not wall size.
- **Transport** anything camera-bounded or inherently single-producer. Network cost is fixed and small relative to the wall.

This is what the Unreal Rivermax workflow does — outer frustum rendered per node, inner frustum sent over Rivermax — and it is correct for exactly this reason.

### 1.2 Every node is a renderer *and* a compositor *and* a receiver

There is no "render node" and "output node" species. There is one node type with a pipeline of optional stages, and a deployment picks which stages are active:

```
   ┌─ SOURCES ──────────────────────────────┐
   │  • Local media decode (own crop only)  │
   │  • Local 3D render (own wall region)   │  ← wall-bounded, distributed
   │  • ST 2110 / NDI receive               │  ← camera-bounded, transported
   │  • Capture card input (SDI)            │
   └──────────────────┬─────────────────────┘
                      ▼
   ┌─ COMPOSITE ────────────────────────────┐
   │  2D graph over all sources, in this    │
   │  node's region of the canvas           │
   └──────────────────┬─────────────────────┘
                      ▼
   ┌─ OUTPUT ───────────────────────────────┐
   │  Feed map → warp/blend → cal LUT →     │
   │  DP / SDI / ST 2110 / NDI              │
   └────────────────────────────────────────┘
```

A full ICVFX node runs all three. A **thin receiver** — the Phase 0.5 wedge product in §11 — is the same binary with local sources disabled: 2110 in, composite, feed map, out. Same code path, no second implementation.

### 1.3 Inner frustum compositing

The frustum is rendered once at camera resolution plus overscan, sent as an ST 2110 stream, and received by every node whose wall region intersects it. Each node projects it into its own outer frustum render using the same tracking sample every node already has, so the composite is consistent by construction — no cross-node agreement protocol needed.

Frame alignment comes from RTP timestamps against PTP: frustum frame *N* composites with outer-frustum frame *N*, with one to two frames of receive buffering. Multiple cameras are simply multiple streams; two cameras double frustum bandwidth and leave outer-frustum cost untouched.

### 1.4 What determinism you still need

The seam problem from earlier drafts does not disappear — it just narrows to a well-defined case. Two nodes rendering adjacent regions of one continuous surface must agree at the boundary. That means:

- **Temporally stateful techniques still need care on the outer frustum.** ReSTIR reservoirs and TAA history diverge across nodes. Use deterministic probe GI (DDGI with replicated probe state) and frame-number-keyed jitter for the outer frustum; save full temporal ReSTIR for the inner frustum, which is single-producer by construction and therefore immune.
- **Overlap and cross-fade** adjacent node regions by 64–128 px. Cheap, and it converts a hard seam into a soft gradient.
- Fixed timestep from frame number, RNG seeded from `(frame_number, layer_id, instance_id)`, frame-exact media decode (§6).

Transported sources carry no determinism requirement at all, which is precisely why the inner frustum — the part you actually photograph — goes over the wire.

### 1.5 What the fabric costs

- **One extra hop of latency** on transported sources only. ST 2110 is scanline-oriented and PTP-locked, so sub-frame to one frame with sensible buffering. Fold it into the tracking-latency compensation budget (§5), and note it applies to the frustum, not to locally-rendered content.
- **The network becomes a failure domain.** PTP-aware switching and correct multicast are now your problem, and you get blamed for a customer's misconfigured switch. Ship a diagnostic; publish a validated reference design.
- **Per-node Rivermax licences and ConnectX NICs** on every node — see §13.3. Note that with the distributed model the fabric only carries frustum-class traffic, so 25 GbE per node is often sufficient and 100 GbE is generous.

### 1.6 Redundancy

A spare node subscribes to the same multicast groups, holds the same replicated state, and renders the same region in parallel. Promotion is an output-stage switch at a frame boundary. Because transported sources are stateless at the receiver and distributed sources are recomputed from replicated state, a promoted spare is correct on its first frame.

---

## 2. The spine: frame-scheduled command bus

This single mechanism resolves four separate requirements at once — live editing (1), remote node management (3), version staging (7), and cluster coherence. It deserves to be built first and built well.

Every mutation to the document is a **command**: a small, serializable, side-effect-free description of a change, carrying its own inverse.

```cpp
struct Command {
    CommandId       id;
    DocumentVersion base;        // version this was authored against
    FrameNumber     apply_at;    // show-clock frame, not "now"
    TransactionId   txn;         // grouping for undo
    Payload         forward;
    Payload         inverse;     // for undo
};
```

Rules:

1. Commands are authored on the Director, stamped with `apply_at = current_frame + N` (N ≈ 4–8 frames of lookahead, tunable per show).
2. They are broadcast to all nodes over an ordered, reliable channel.
3. Every node applies commands in `(apply_at, id)` order at the frame boundary. A node that receives a command late reports a missed-frame and requests resync.
4. Undo is not special: `undo` broadcasts the inverse as a new command with a fresh `apply_at`.
5. Node management (assign feed, restart output, change EDID) is the same command type. There is no separate control protocol.

Lookahead is what makes a live compositor over a cluster feel instant while remaining glitch-free. The operator drags a slider; the change lands ~100 ms later on the wall, simultaneously on every node, never torn.

**Resync path:** if a node's version diverges (it dropped, joined late, or crashed), it requests a full snapshot. Snapshots are cheap because the document uses persistent data structures (§8) — a snapshot is a root pointer plus a content-addressed diff against whatever the node last had.

---

## 3. Synchronisation stack

Five layers, all required, all independently verifiable. Build a diagnostics page that shows the health of each.

| Layer | Mechanism | Purpose |
|---|---|---|
| **Reference** | Tri-level sync / blackburst into Quadro Sync II and into every SDI card | House clock |
| **GPU framelock** | NVIDIA Quadro Sync II swap groups + swap barrier across nodes | Every GPU scans out on the same edge |
| **Network time** | PTP (IEEE 1588 / SMPTE ST 2059) | Required for ST 2110; also a good cross-check on everything else |
| **Show clock** | LTC / MTC / SDI VITC ingest, or internal master | Timeline position, camera-shutter alignment |
| **Software barrier** | Per-frame commit barrier over the command bus | Guarantees state coherence independently of scanout |

Implementation notes:

- **Vulkan** is the primary API. Swap group / barrier support is via `VK_NV_present_barrier` (plus `VK_EXT_full_screen_exclusive` on Windows). This extension is newer than the OpenGL `WGL_NV_swap_group` path and the D3D `NvAPI_D3D1x_*` swap group path, so **verify current driver behaviour on your exact GPU/driver combination early** — this is a common source of nasty surprises, and the workaround (an OpenGL or D3D11 presentation surface that the Vulkan renderer blits into) is ugly enough that you want to know in month 2, not month 20.
- Framelock ≠ state lock. A cluster can be perfectly genlocked and still show frame *N* on one node and *N+1* on another. The software barrier in §2 is what prevents that. Instrument both and display them separately.
- Expose measured **end-to-end latency** (tracking sample → photons off the LED wall) as a first-class, calibratable number per output, because ICVFX correctness depends on compensating it.

---

## 4. Graphics foundation

**API:** Vulkan 1.3 as primary, on Windows and Linux. A thin RHI (~4–6k lines) abstracts device, queues, resources, and sync. A D3D12 backend is a phase-4 nice-to-have; a **D3D11 interop device is mandatory in phase 3** purely to host Notch (§7) and Spout.

**Frame graph.** All rendering is expressed as passes declaring reads/writes; the graph solves barriers, transitions, transient memory aliasing, and async-compute overlap. This is not gold plating — with 2D comp, 3D scenes, ICVFX frustums, warping, and colour conversion all in one frame, hand-written barriers become unmaintainable by about month 8.

**Colour management is the foundation, not a feature.** Get it wrong and no amount of good rendering saves you on an LED wall.

- Working space: linear, scene-referred, `RGBA16F` default, `RGBA32F` selectable per comp branch.
- **OpenColorIO v2** throughout, with ACES 1.3 / ACEScg as the default config, and full support for loading a show-supplied config.
- Per-input transform (camera log → working), per-output transform (working → LED processor's expected space, e.g. Rec.2020 PQ or a custom calibrated space).
- Per-panel / per-wall calibration LUTs (3D LUT, 33³ or 65³), applied last, on the node that owns the output.
- Dithering before quantisation to 10/12-bit, always. LED walls show banding that a monitor hides.

**Compositing model: node graph underneath, layer stack on top.**

The internal representation is a DAG of operators. The default UI presents an After Effects–style layer stack, because that is what your operators know and it is faster for 90% of work. A "show graph" toggle reveals the DAG for the other 10%. A precomp is a subgraph; a subgraph can be promoted to a reusable asset. This gets you AE ergonomics without painting yourself into AE's limitations (no true multi-input operators, awkward re-use).

---

## 5. 3D and ICVFX

### 5.1 Renderer

Per §0.1, a clustered forward+ PBR renderer:

- GGX specular with multiscatter energy compensation, Burley diffuse, proper Fresnel; metallic-roughness *and* specular-glossiness inputs.
- Punctual, area (LTC), and emissive-mesh lights; IBL via prefiltered cubemaps with split-sum.
- Cascaded shadow maps + `VK_KHR_ray_tracing` for contact shadows, reflections, and optionally RTXGI-class dynamic GI.
- TAA with a jitter sequence keyed to frame number (determinism!) and DLSS as an optional upscaler on the outer frustum only.
- glTF 2.0 + USD ingest. USD matters — it is how sets will arrive from the art department.
- **MaterialX as the material interchange**, with shader codegen into your pipeline. This is the bet, not MDL: MaterialX is what USD carries, and what Houdini, Maya, Blender, and Substance export. It is the single decision that determines whether a set handed over by the art department renders correctly on first import or turns into a week of manual material rebuilding per show.

### 5.2 ICVFX pipeline

This is the part that differentiates you from a 2D media server, and it is mostly plumbing precision rather than graphics research.

**Tracking ingest: FreeD and OpenTrackIO only.** That is the right call — between them they cover effectively the whole installed base, and cutting the long tail of proprietary protocols removes a permanent integration tax.

**OpenTrackIO** is the one to build around. <cite index="19-1">It is an open standard for real-time on-set virtual production metadata, developed by the SMPTE RIS OSVP group, with v1 published in January 2025; the specification lives at opentrackio.org and is generated by RIS's CamDKit, and it uses the OpenLensIO mathematical model.</cite> <cite index="19-1">Support landed in Unreal Engine 5.6, and it is being implemented by Canon, Sony, Mo-Sys, Pixotope, Stage Precision, disguise and Chaos Arena.</cite> Two properties matter enormously here:

- **PTP sample timestamps at nanosecond precision**, alongside timecode and camera frame rate. This is the fix for FreeD's central weakness and it plugs straight into the §3 sync stack — you can align a tracking sample to a render frame properly instead of guessing.
- **OpenLensIO distortion model** carried in-band, plus entrance pupil, focal length, focus distance, and active sensor dimensions. Much of the §5 lens-calibration problem is solved by the protocol when the vendor supplies good data. <cite index="21-1">Canon added OpenLensIO as a distortion output option alongside OpenCV format in a firmware update for its Virtual Production System.</cite>

There is an open-source C++ reference library, which suits a C++ core directly — use it rather than writing a JSON parser against the spec.

**FreeD** is the legacy path and still ubiquitous: a compact fixed-point UDP message carrying pan, tilt, roll, position, zoom and focus. Support it, but know its limitation — **the standard message carries no timestamp**, so you must stamp on arrival, which means network jitter becomes tracking jitter. Mitigate with arrival-time filtering and a slightly deeper prediction window, and treat FreeD sources as inherently lower-confidence in the UI. Do not let a FreeD deployment set the latency budget for an OpenTrackIO one.

Both feed the same internal representation: a ring buffer of samples keyed to show-clock time. Never consume "the latest sample" — consume *the sample for the frame being rendered*, interpolated, with a configurable predictive lead covering measured system latency. Every node receives the same tracking stream, which is what makes the §1.3 frustum composite consistent without any additional agreement protocol.

**Lens model.** Where OpenTrackIO supplies OpenLensIO data in-band, use it. Where it does not — FreeD sources, or vendors shipping partial metadata — you still need the full model: focal length, entrance pupil / nodal offset, and a distortion model that varies with focus and zoom. Support Brown-Conrady, anamorphic (separate x/y with squeeze), and — most importantly — an imported **STMap / distortion grid LUT** interpolated across a focus-zoom lattice, because that is what a real lens calibration produces. Ship a calibration tool.

**Inner / outer frustum.**
- Inner frustum: the camera's exact projection plus overscan margin (typically 5–15%, so a whip pan does not reveal the seam), rendered at full quality, motion-blurred to the camera's shutter angle.
- Outer frustum: everything else on the wall. Lower cost, independently exposable and colour-shiftable, because its job is interactive lighting and reflections, not being photographed sharp.
- Cross-node frustum ownership: when the inner frustum spans walls served by different nodes, either (a) each node renders its own slice — simple, costs duplicated draw calls, or (b) one node owns the frustum and pushes it to peers via GPUDirect RDMA over the same ConnectX NIC used for ST 2110. Build (a) first; (b) is a phase-4 optimisation for high-density stages.

**GI strategy: the constraint largely dissolves with pixel replication.**

Modern photoreal GI (ReSTIR, DDGI, RT reflections, TAA) is *temporally stateful* — it carries history across frames. In a replicated-state cluster this is a serious problem: two nodes rendering adjacent sections of the same wall will diverge in their histories after any dropped frame, restart, or resize, and the seam between them shimmers in a way that moves. On a 24-foot wall that is immediately visible and gets blamed on the panels.

**With the topology in §1, this mostly stops being a problem**, because no two renderers ever produce adjacent pixels of the same continuous surface. Splitting at the output stage (§1.2) means one GPU renders a whole wall and the slice happens downstream. Full temporal GI is then safe on both frustums.

Two cases where it still bites, and both are manageable:

1. **A surface too large for one GPU.** Split at a physical discontinuity if one exists. If not, overlap adjacent render regions by 64–128 pixels and cross-fade at the output stage. The blend hides history divergence rather than requiring it not to exist.
2. **Reflections of one region appearing in another.** Screen-space techniques see only their own region, so a reflection near a split will disagree across the boundary. Use ray-traced or probe-based reflections near known split lines, and keep splits away from highly specular set geometry.

The frustum distinction still matters, but now for cost rather than correctness:

| | Inner frustum | Outer frustum |
|---|---|---|
| **Job** | Be photographed sharp | Light the set, fill reflections |
| **Budget** | Full quality — ReSTIR, RT reflections, shutter-matched motion blur | Cheaper — DDGI or baked probes are usually enough |
| **Exposure/colour** | Matched to camera IDT | Independently offsettable for lighting purposes |

The outer frustum's output is bounce light and reflections, not a photographed image. It needs to be *plausible*, not converged. Spending inner-frustum quality on it is a waste of a frame budget you will want back.

**Light cards.** A dedicated 2D layer type living in wall space, editable from the operator UI, addressable over DMX/OSC by the gaffer, with soft-edge falloff and colour temperature controls. Small feature, enormous day-to-day value on stage.

**Moiré and artefact mitigation.** Per-wall subtle blur/sharpen, panel-pitch-aware, plus a diagnostic overlay showing panel boundaries and pixel-perfect test patterns.

**Colour correctness on stage.** The chain is: content → your colour pipeline → LED processor (Brompton / Megapixel / NovaStar) → LED panels → camera sensor → camera's own log encoding. Ship a workflow that ingests the processor's calibration and the camera's IDT, and a verification mode that puts known patches on the wall and reports measured deltas.

---

## 6. Media pipeline

**Codecs.**

| Codec | Path | Notes |
|---|---|---|
| HAP / HAP Q / HAP R | Snappy decompress on CPU (or GPU) → BC1/BC7 upload | The workhorse. Highest throughput per disk byte. |
| NotchLC | GPU decode | Required for Notch interop, and good in its own right. |
| ProRes 422/4444 | FFmpeg or licensed Apple SDK | Ubiquitous in delivery. |
| DNxHR | FFmpeg | Broadcast houses will hand you this. |
| H.264 / H.265 / AV1 | NVDEC | Preview and low-bandwidth content, not hero playback. |
| JPEG XS | For ST 2110-22, and increasingly for storage | Low-latency mezzanine. |
| Image sequences | EXR (half & full, DWAA/DWAB/ZIP), DPX, TIFF, PNG | Non-negotiable for VFX-supplied content. |
| RED R3D | RED SDK, CUDA debayer | If plates arrive raw. Optional; behind a licensing agreement. |

**Playback engine.** Per-layer transport with independent rate, direction, and in/out points. Ring buffer with prefetch depth tuned to codec and storage. Disk I/O via `DirectStorage` (Windows) / `io_uring` (Linux), with GPU decompression where the codec allows. Frame-exact seek is mandatory — "seek to nearest keyframe" is not acceptable when the show is cued to timecode.

### 6.1 Distributing wall-bounded content

This is the load-bearing consequence of §1.1 and the hardest part of the media pipeline. An 11K plate must never be decoded whole on a node that only shows a quarter of it.

**Region-of-interest propagation.** Each node knows its output region. Walk the compositing graph *backwards* from that region to compute the input region each source must supply. Any spatially-extended operator — blur, glow, displacement, distortion, chromatic aberration, warp — expands the region by its kernel radius, and the expansions accumulate down a chain. This is the same ROI/bbox mechanism Nuke uses, and it is the correct general solution: it handles arbitrary effect graphs rather than a fixed guess at margin size.

**The apron is not optional.** A node compositing its slice with a 60 px blur needs 60 px of neighbour pixels or the seam shows. ROI propagation computes the apron; the media layer must be able to deliver it. Getting this wrong produces seams that only appear when someone adds an effect — the worst kind of bug to find on a shoot day.

**Three ways to get a node only the pixels it needs**, in preference order:

1. **Pre-slice at ingest.** Transcode source media into per-node slices during content ingest, sized to the node's ROI including apron. Cheapest at playback and what disguise and Pixera do. Cost: re-slicing whenever the feed map changes, which must be a tracked background job gated by the same content-ready check as §6.2. Never let a mapping change silently invalidate content.
2. **Tile-native partial decode.** Tiled EXR, JPEG XS, and chunk-aligned HAP support genuine partial reads. Best when it applies, because it survives mapping changes with no re-transcode. Note that scanline-ordered formats (DPX, scanline EXR) only help for horizontal splits — a vertically-split wide wall still touches every scanline.
3. **Full decode and crop.** Decode everything, discard most. Wasteful, but robust to arbitrary and dynamic mapping. Acceptable for cheap codecs — HAP is BC-family decompression and an 11K HAP frame is not the disaster it sounds like — and the right fallback while a re-slice job is still running.

Ship all three and choose per source. The operator should see which mode each piece of content is in, and be warned when something has fallen back to full-decode on a hot path.

### 6.2 Content distribution

A content-addressed store (BLAKE3 hashes). The Director tracks which blobs and which *slices* each node holds; sync is delta-based, bandwidth-throttled, and resumable. **A version cannot be promoted to Live until every node reports content-ready for its own slices.** The UI must show this as a hard gate, because "it played back on my machine" is how shows get embarrassed — and with per-node slicing, "my machine" is now literally a different set of pixels.

---

## 7. External interfaces

### 7.1 Notch blocks

Notch's runtime is **Direct3D 11** and Windows-only, and requires a Notch Playback licence (dongle) plus a commercial relationship with Notch Ltd. Two consequences:

1. **Interop.** Host a D3D11 device alongside the Vulkan device. Notch renders to a D3D11 texture created with `D3D11_RESOURCE_MISC_SHARED_NTHANDLE`; import that handle into Vulkan via `VK_KHR_external_memory_win32`, and synchronise with a shared fence via `VK_KHR_external_semaphore_win32`. Zero copies, one frame of latency at worst.
2. **Isolation.** Run the Notch host in a **separate child process**. A malformed block should degrade to a black layer and an operator alert, not take down a render node mid-show. The same sandbox hosts third-party plugins. Communicate over shared memory + the shared texture handle.

Notch-exposed properties are reflected into your parameter system as first-class animatable parameters — keyframeable on the timeline, presettable, DMX/OSC-addressable, undoable. That reflection layer is what turns "we support Notch" into "Notch feels native."

### 7.2 Everything else

- **Video I/O:** DeckLink and AJA (12G-SDI, quad-link 12G for 8K, HDR metadata, embedded audio and ANC), Bluefish444 and Deltacast as phase-4 additions. GPUDirect where the card supports it.
- **IP video:** NDI 6 (send and receive, with discovery), **Rivermax** for SMPTE ST 2110-20/-22/-30/-40 with ST 2059 PTP — note this requires NVIDIA ConnectX-6/7 class NICs and a properly provisioned network; document the switch requirements, because customers will blame you for their network.
- **Texture sharing:** Spout on Windows (D3D11 shared handle — you have the interop device already), and a Vulkan external-memory equivalent on Linux. Syphon if macOS ever appears.
- **Control:** OSC (in/out), Art-Net and sACN (in/out, so you can both be controlled by and control lighting), MIDI, MIDI Show Control, LTC/MTC, GPIO/contact closure, and a documented WebSocket + REST API mirroring the command bus.
- **Show control integration:** tested profiles for grandMA3 and ETC Eos, because that is who will be cueing you.
- **Plugin SDK:** a stable **C ABI** (not C++, to survive compiler churn), covering custom effects, generators, device drivers, and tracking sources. Versioned, with an ABI compatibility test suite.

---

## 8. Document model, presets, undo, and versions

This is requirement 7, and it is the requirement most often bolted on too late. Design it first; everything else inherits from it.

**Immutable, persistent document.** The project is a tree of nodes stored as a persistent (structurally-shared) data structure — a HAMT or RRB-tree. Mutating a parameter produces a new root sharing all untouched subtrees. Consequences that fall out for free:

- A document version is one pointer. Snapshots are O(1).
- Diffing two versions is O(changes), not O(document).
- Sending state to a node is sending a diff.
- Undo is retaining an old root.
- Concurrent read (render thread) and write (UI thread) need no locks — the render thread holds a root and is immune to edits until it swaps.

**Undo/redo.** A command stack with explicit transactions. Dragging a slider is one transaction, not 400. Grouping is explicit (`begin_txn` / `end_txn`), with a coalescing window for continuous controls. Undo respects the cluster because it goes through the same frame-scheduled bus (§2). Separate stacks per top-level context (comp, 3D scene, feed map) so undoing an output-mapping change doesn't undo a keyframe.

**Versions and branching.** A git-like DAG of document roots, with two named pointers:

```
                    ┌── stage/lookB ──┐
main ──●──●──●──●───┤                 │
       │            └── stage/lookA ──●  ◄── STAGE  (renders to preview outputs only)
       │
       └──────────────────────────────●  ◄── LIVE   (renders to real outputs)
```

- **Stage** renders to preview outputs, a preview LED tile, or a windowed multiview. It is the operator's sandbox.
- **Push to Live** is an atomic root-pointer swap, scheduled at a frame, gated on all-nodes-content-ready. It cannot half-apply.
- **A/B compare** renders both roots and wipes/splits between them — the single most useful feature for a supervisor standing behind the operator.
- **Visual diff** lists what changed between two versions in domain terms ("Layer 3 opacity 0.8 → 0.55; added Blur to Comp 'Sky'"), not JSON.
- Branch, merge (with conflict resolution at the parameter level), tag, and annotate.

**Presets.** Granular and composable, at every level: parameter, effect, layer, comp, 3D scene, material, output map, whole project. Each carries a thumbnail rendered from the live engine, tags, and an author. Hovering a preset in the browser previews it live on the Stage branch; clicking applies it as a transaction. Presets are content-addressed too, so a project archive can collect exactly the presets it uses.

**Safety net.** Continuous autosave to a write-ahead log; crash recovery restores to the last committed frame. Full command log recording, so any bug report replays deterministically — combined with §1's determinism requirement, this is worth its weight in support hours.

---

## 9. UI without Qt

You have ruled out Qt, which is fine, because the widgets that matter here do not exist in Qt anyway. Roughly 70% of the UI surface is bespoke:

- Multi-track timeline with keyframes, curve editor, nested comps, timecode ruler, trim/ripple/slip
- Node graph editor
- 3D viewport with gizmos
- Feed-mapping canvas (2D and 3D UV)
- Colour wheels, curves, scopes (waveform, vectorscope, histogram, false colour)
- Media browser with scrubbable thumbnails
- Node health / cluster status wall

**Recommended approach:** build a **retained-mode widget layer over a GPU vector renderer** of your own. Layout via a flexbox-like solver (Clay or Yoga are good references, both trivially portable). Text via HarfBuzz + FreeType, with an SDF or MSDF atlas — do not skimp here; operators read tiny numbers in a dark room for 14 hours. Rendering shares the same RHI as the engine, so viewports are just textures in the widget tree with zero copies.

Use **Dear ImGui for internal tooling only** — the profiler, the frame-graph inspector, the node diagnostics. It is excellent for that and wrong for a product surface an operator uses under pressure. Do not let it leak into the shipping UI, because the migration later is painful and always gets deferred.

**Deliberately reject:** embedded browser (CEF/Ultralight) for the main surface — the latency and memory cost are not defensible when the app must stay inside a frame budget. It is a reasonable choice for a *secondary* remote-control web UI, which you should also ship.

**Operator ergonomics that are non-negotiable:**
- Every action reachable by keyboard; a full remappable shortcut system
- Dark theme calibrated for a dim stage, with a high-contrast mode
- Multi-monitor with detachable panels and saved workspaces
- Hardware surface support (Stream Deck, X-keys, generic MIDI, Contour ShuttlePro)
- A "panic" state: an operator must always be able to get to black, to a holding slate, or back to the last-known-good version in under two seconds

---

## 10. Reliability engineering

Everything below is a feature, not a phase-5 cleanup task. The competitive moat in this market is not visual quality — it is that the box does not fail during the show.

- **Watchdogs** per subsystem, with automatic restart of isolated processes (Notch host, plugin host, media decoders).
- **Crash isolation** for all third-party code. Your process should never die because a codec did.
- **Per-frame telemetry:** GPU time by pass, CPU time by system, VRAM, disk queue depth, network jitter, missed frames, sync status. Logged continuously, exportable as a show report.
- **Hot spare promotion** in under one frame, manual or automatic.
- **Degradation ladder:** if the frame budget is exceeded, drop in a defined order (outer frustum quality → effects → resolution scaling) rather than dropping frames. Make the ladder visible and configurable.
- **Pre-show checklist mode:** verify genlock lock, PTP lock, content-ready on all nodes, EDID stability, output signal presence, disk headroom, and thermal margin. One button, one green/red answer.

### 10.1 Recovery model

This document originally left the recovery model unstated, and the implementation filled the gap by building for the strictest reading available — exact frame resumption from a durable log. That is the wrong requirement for a show, and paying for it per frame put a hard ceiling on frame rate. The decision, recorded here because it belongs at this level:

- **Node failure is covered by hot-spare promotion** (§1.6), which needs replicated runtime state and no durable log at all. A promoted spare is correct on its first frame because distributed sources are recomputed from replicated state and transported sources are stateless at the receiver.
- **Director failure is covered by restart and re-cue.** The show stops, the operator re-cues, playback resumes from a cue point. Nobody resumes frame 84,102 under a director who is standing there waiting.
- **The Director's durable log is a project artifact and an audit trail** — what the operator saved and what they changed during the show — not a frame-resumption mechanism. It records edits, not frame ordinals.
- **Director redundancy stays open**, and belongs with the hardening phase. When it is answered it will be a spare Director holding replicated state, matching the node story, not log replay.

What this does *not* relax is state agreement. Two nodes must never present different frames, and §2's software barrier — not genlock — is what guarantees that. That path stays exactly as designed and stays in memory, where it is fast. Durability and agreement are separate concerns and were previously conflated.

---

## 11. Delivery plan

Assume a team of **12–18 senior engineers**. This is a large system; the honest comparison set has been at it for a decade or more with bigger teams. The phasing below is aggressive but achievable, and it is deliberately ordered so that the hardest de-risking happens first.

### Phase 0 — Output node and foundations (months 0–6)
RHI + frame graph + OCIO colour pipeline + immutable document model + command bus (§2, §8). **And the thin output node: Rivermax ST 2110 receive, feed mapping, warp/blend, per-wall calibration LUTs, output to DP/SDI.** *Exit criterion below.*

> **Status.** The document model and command bus (§2, §8) are done, along with a
> frame-scheduled cluster transport, a show clock, and a two-node playback loop
> with output windows. The RHI, frame graph, colour pipeline and the entire
> output node are **not started** — and they are the half of this phase that
> carries the risk. `implementation-plan-cpp.md` §0 re-orders the immediate work
> to close that gap: frame-rate headroom first, then an RHI over SDL_GPU, then
> feed mapping. Phase 0.5 below is unchanged and remains the target.

### Phase 0.5 — The wedge (months 6–9)
**Ship the output node as a standalone product that receives ST 2110 from your existing Unreal nDisplay rig.**

This is the highest-leverage move available in the whole plan and it falls directly out of §1. Your current setup already emits Rivermax ST 2110. An output node that ingests that and does better feed mapping, warping, blending, and colour calibration than the current path is a *usable product on day one* — no renderer required, no media engine required, no 2D compositor required.

What it buys:
- Revenue and real production feedback while the hard parts are still being built
- Your ST 2110 stack validated on real hardware in a real volume, two years before it would otherwise be exercised
- A migration path with no cliff: customers adopt the output layer first, and the render layer replaces Unreal later, per-surface, at their pace
- Honest de-risking — if the output node cannot beat what nDisplay already does, you learn that for the cost of six months instead of three years

*Exit criterion: your own 4-node volume runs a shoot with Unreal rendering and your output nodes driving the processors.*

### Phase 1 — Media and 2D (months 9–16)
Media playback (HAP, ProRes, EXR/DPX sequences), 2D compositing, timeline, operator UI v1. **ROI propagation and per-node content slicing (§6.1) — this is the phase that makes an 11K wall tractable and it is bigger than it looks.** NDI and Spout. Remote node management. *Exit criterion: an 11K-class wall driven by four nodes playing a full-width plate with a blur chain across it, no visible seam, eight hours without a dropped frame.*

### Phase 2 — Compositing depth and versioning (months 16–22)
Full 2D effect library, node graph editor, precomps. Presets, undo/redo, branching, Stage/Live push, A/B compare. Colour tools and scopes. Plugin SDK v1. *Exit criterion: an operator can build and revise a complex show live without a restart, and roll back a bad change in one action.*

### Phase 3 — 3D and ICVFX (months 22–34)
Forked renderer (§0.1) integrated. USD/MaterialX ingest. Camera tracking ingest and lens calibration. Inner/outer frustum, overscan, light cards. Notch block hosting with process isolation. *Exit criterion: a tracked camera shot on a real stage that a VFX supervisor accepts without complaint about parallax, latency, or colour.*

### Phase 4 — Hardening and breadth (months 34–42)
AJA, SDI-only topologies for customers without a 2110 fabric, redundancy and hot spare, Art-Net/sACN/OSC/MSC, degradation ladder, telemetry, show reports, pre-show checklist. *Exit criterion: a rental house puts it on a paying show without you in the room.*

---

## 12. Risk register

| Risk | Severity | Mitigation |
|---|---|---|
| `VK_NV_present_barrier` behaves poorly or is under-supported on target drivers | **High** — invalidates the Vulkan-primary decision | Test in month 2 on the exact target hardware. Fallback: D3D11/OpenGL presentation surface fed by the Vulkan renderer. |
| Determinism violations within a node break temporal stability and replay | Medium — no longer cross-node critical (§1.5) | Bit-comparison harness in Phase 0: render frame N twice on one node and diff. Run it in CI. |
| Notch commercial terms, or Notch moving off D3D11 | Medium | Engage Notch Ltd. before Phase 3. Process isolation limits blast radius either way. |
| Temporal GI causes visible, moving seams between adjacent nodes on the outer frustum | **High** — on-camera, and gets blamed on the panels | Deterministic probe GI + frame-keyed jitter on the outer frustum; full temporal ReSTIR only on the single-producer inner frustum (§1.4). Overlap 64–128px and cross-fade. Test a two-node seam on a real wall by month 16. |
| ROI/apron miscalculation produces seams that appear only when an effect is added | **High** — latent, and surfaces on a shoot day | ROI propagation through the comp graph (§6.1), not a fixed margin. CI test: render a full canvas on one node vs. sliced across four, with a blur chain, and diff the seam region. |
| Feed-map change silently invalidates pre-sliced content | Medium — content plays back wrong, not absent | Re-slice is a tracked job gated by the content-ready check (§6.2). Fall back to full-decode-and-crop while it runs, and surface the fallback in the UI. |
| ST 2110 fabric misconfiguration at customer sites gets blamed on you | **High** — support cost and reputation | Publish a validated switch/NIC reference design. Ship a network diagnostic that proves PTP lock, multicast health, and non-blocking capacity before a show. |
| Added latency on the transported inner frustum is unacceptable for a given shoot | Medium — affects only the frustum, not local content | Measure per-configuration and expose as a calibratable number (§3). Where the frustum renderer is co-located with a wall node, the hop disappears for that node. |
| Forked Spartan proves too costly to own | **High** | Two engineers, six to nine months of dedicated internalisation before committing. Evaluate Atlas as the fallback — smaller surface, already headless-capable, at the cost of a lower visual ceiling. |
| Odin chosen and DeckLink/AJA binding cost compounds | Medium | Prototype the DeckLink binding in week 1 before committing. |
| Colour pipeline correctness on real walls | **High** — reputational | Get on a real stage with a real camera by month 12, not month 30. |

---

## 13. Commercial and licensing strategy

### 15.1 There is nothing to license in the renderer shortlist

SpartanEngine, Atlas, hybrid-rendering, EveryRay, Veselov's path tracer and ZEngine are **MIT**. R3D is **zlib**. Both licences already grant everything you need: commercial use, modification, sublicensing, sale, and a private fork with no obligation to publish changes. The only requirement is preserving the copyright notice and licence text in source distributions.

(VQEngine's GPL-3.0 is the exception, and it is not negotiable-around at arm's length. A relicense would require the author's agreement and the agreement of every contributor whose code is in it. Not worth pursuing when better options are permissive.)

So the licence question is closed. **Don't buy what you already own.**

### 15.2 Buy the person, not the code

The real asset in a one-person engine is not the source — it's the twelve years of context that never got written down. That is what your six-to-nine-month internalisation estimate is actually paying for, in your engineers' time instead of the author's.

Options for Spartan's author, in descending value:

1. **Employment or fractional employment as renderer owner.** Highest leverage available anywhere in this project. It converts your single largest technical risk into a solved problem and collapses the internalisation period substantially.
2. **Retained consulting** — a fixed weekly commitment with a knowledge-transfer deliverable: architecture documentation, recorded walkthroughs, and review of your fork at defined milestones.
3. **Milestone code review.** Cheaper, narrower. Useful if the answer to 1 and 2 is no.
4. **Sponsorship.** He has GitHub Sponsors. It costs little, opens the relationship, and is decent citizenship if you are building a commercial product on his work. Not a substitute for the above.

Two things to understand before approaching:

- His README points to a `plan.md` describing "a destination that gives all this tech a purpose." **He appears to have his own commercial ambitions for the engine.** Read that document first. A hiring approach that ignores it will land badly, and a genuine conflict of interest is possible.
- If you do engage him, **get the IP boundary in writing.** He will likely continue the open-source project. Work-for-hire or a clear CLA for contributions to your fork, and an explicit statement of what flows back upstream. Ambiguity here becomes a serious problem the moment the product has value.

The same logic applies at smaller scale to Bigfoot71 (R3D, Ko-fi) and Dihara Wijetunga (hybrid-rendering) if you port meaningful work from them. Low cost, correct behaviour.

### 15.3 Where money is actually required

These are unavoidable and easy to under-budget:

| Item | Why | Notes |
|---|---|---|
| **Notch playback licences** | No alternative for Notch block support | Per-node dongle. Engage Notch Ltd. before Phase 3. |
| **Rivermax licences** | Required for ST 2110 | **Per-node fee.** Multiplies across every render node you ship. Model this into unit economics early. |
| **AVC/HEVC patent pools** | You are shipping a decoder | Via LA / MPEG LA. Real royalties and real legal exposure. Very commonly forgotten by teams shipping media software. |
| **ProRes** | Ubiquitous in delivery | Apple's licensing programme; decode and encode are separately governed. |
| **RED SDK** | Only if you support R3D | Requires an agreement with RED. |
| **Tracking vendor relationships** | Mo-Sys, stYpe, Ncam, OptiTrack | Not licences so much as access to reference hardware and protocol documentation. Buy or borrow units early; you cannot develop lens calibration against a spec sheet. |

### 15.4 Free, despite appearances

Do not spend on these — register and use them: OpenColorIO, OpenUSD, MaterialX, meshoptimizer, AMD FidelityFX, NVIDIA DLSS, NVIDIA Real-Time Denoisers (NRD), RTXGI/RTXDI, Intel XeSS, NDI SDK, DeckLink SDK, AJA NTV2 SDK. Several require accepting a vendor agreement; none require a cheque.

NRD and RTXDI deserve a specific look. Spartan implements ReSTIR itself; NVIDIA's SDKs are the battle-tested equivalents and may be worth substituting for the denoising stage specifically, where robustness matters more than architectural purity.

### 15.5 The one number worth getting before you commit

Get a quote from Epic for an enterprise Unreal licence.

Not because you should take it — your stated position is that you want to own the renderer, and this plan is built on that. But disguise and Pixera both went the Unreal route, so the market has already revealed its preference, and Epic negotiates bespoke terms for exactly this category of product. You should know the number you are declining, and be able to articulate to investors or partners why you declined it. "We didn't check" is a much weaker answer than "we checked, and here is what owning the stack buys us instead."

---

## 14. Reference hardware

- **Render node:** 1× RTX 6000 Ada / RTX Pro 6000 class, Quadro Sync II, 128 GB RAM, dual-socket or high-core-count single, NVMe RAID with ≥6 GB/s sustained read, ConnectX-6/7 for 2110 (25 GbE is usually sufficient — only frustum-class traffic crosses the fabric, not wall-resolution content), DeckLink 8K Pro or AJA Corvid 44 for SDI, 10/25 GbE for control and content sync.
- **Director:** similar GPU (for local preview), 3–4 displays, no Quadro Sync required.
- **Network:** separate VLANs for control, content sync, and ST 2110 media. PTP-aware switching for the media VLAN.
- **OS:** Windows 11 IoT Enterprise LTSC as primary (Notch, DeckLink, driver maturity); Linux for headless render nodes once Notch is not required on them.

---

## 15. Scope discipline — what to cut

If schedule pressure arrives, cut in this order, and refuse to cut anything below the line:

**Cuttable:** AV1 decode · Bluefish/Deltacast · ray-traced GI (ship SSGI + probes) · merge/conflict resolution in versioning (ship branch + push only) · macOS · the web remote UI · SDI-only topologies (ship 2110-first, add SDI-only in Phase 4).

**Never cuttable:** colour management · frame-scheduled command bus · ROI propagation and per-node slicing · content-ready gating before live push · crash isolation of third-party code · hot spare · the panic path.

The features above the line are what make demos impressive. The features below the line are what make a rental house buy twelve of them.
