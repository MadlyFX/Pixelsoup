# 23 — Cohort render-conformance self-check

Advanced Program now requires measured render evidence from the Director's
Program renderer and every configured render node. A missing, running, failed,
obsolete, or different report holds Program through the existing renderer
readiness gate, including for an unsynchronized show. Basic rendering and an
agreed optional Basic fallback retain their existing readiness behavior.

## Fixed scene and measured outputs

`render_conformance.cpp` contains a complete 128 × 128 scene and generated HDR
environment. It renders the whole canvas on every node, with sixteen fixed show
frames, a moving rough metallic/emissive object, a receiver, four sun cascades,
a shadow-casting spot, contact shadows, IBL, AO, fog, TAA and object motion blur.
The frame goes through `HybridSceneRenderer`'s production pass order. The fixture
asserts that its requested passes ran and its history initializes once then
advances fifteen times. Output uses the actual baked display-LUT and dither shader.

Twenty digest lanes cover environment conversion, irradiance, specular
prefiltering, BRDF LUT, cascades, shadow atlas, depth, object identity, normals,
camera/object velocity, object-only velocity, coverage, occlusion, lit color,
fog, temporal resolve, motion blur, bloom, display transform, and output dither.
All environment faces and mip levels are read. Copies are queued at pass
boundaries before reusable targets can be overwritten. Digests fold tightly
packed readback bytes in fixed order, including frame, format, dimensions, mip
and layer. Row padding is excluded. These FNV-1a digests detect conformance
differences; they are not a security authentication mechanism.

The fixture has no asset files, cache readiness dependency, wall clock, local
slice, output slot or authored project input. Its environment and display table
are supplied directly to isolated renderer instances. `render_conformance_version`
must change when the fixture or digest encoding changes.

## Worker, heartbeat and Program gate

A persistent worker creates a private D3D12 device and verifies that its adapter
identity matches the actual Program device. Allocation, GPU waits and readback
hashing all happen there. Frame/control code only observes a bounded value using
`try_lock`. An initial check is requested when an Advanced Program device is
observed, before it can satisfy readiness. Success and failure are cached;
there is no periodic recheck. Explicit requests or device identity changes
trigger another run. Newest-wins request coalescing and discarded stale
completions have diagnostic counters.

The Director's explicit request generation travels on the existing authenticated
heartbeat. The return heartbeat carries the generation, fixture version, state,
all pass digests, adapter name/vendor/device, DXGI driver version, SHA-256 shader
bundle and strict-float setting. The codec bounds the name and vectors, checks
enum values, and rejects a claimed pass with incomplete evidence. Failed runs
report their checkpoint; a failure never becomes a passing all-zero image.

`observe_renderer_lockstep` extends the existing cohort-level renderer readiness
fold. It requires current complete evidence even for a one-machine Advanced
show. For a cohort it compares pass bytes in fixed order and names the earliest
different pass and node. Matching bytes still require matching adapter, driver,
shader bundle and FP contract. An operator request invalidates acceptance of
every older generation immediately. Session protocol **v9** refuses older peers
at handshake, since an older Director would not enforce this gate.

The cached check establishes the deployed renderer's conformance. It is not a
digest of each production frame and does not certify every possible scene or
feature combination. Future renderer passes need a fixture lane and version
update. The current native Advanced implementation is D3D12; another backend
must provide its own actual device identity and conformance implementation
before it can claim the same evidence.

## Operator workflow

1. Deploy the updated build to the Director and every render node; protocol v9
   requires a coordinated upgrade.
2. Open an Advanced project and show **Output Monitor**. The initial check runs
   automatically. Program stays held while its renderer evidence is pending.
3. Use **Check render cohort** to request another check on the local Program
   device and all connected nodes. Each node renders its own complete fixture.
4. Use **Pass details** to inspect the local and node adapter/driver identities,
   shader SHA-256 and twenty pass hashes. Node rows report matching evidence,
   pending evidence, a failed check, or the first differing pass. The cohort
   status names the failing node; later matching nodes cannot clear that failure.
5. Bring a differing node to the same build, adapter type and driver, then check
   again. There is no quality fallback or override that accepts divergent
   Advanced evidence.

## Verification

- Fresh renderer instances produce identical twenty-lane digests, independently
  of request generation. The worker test covers real Program-device identity,
  sticky failure, explicit retries and superseded requests.
- A one-line fog-shader mutation changed **fog** first, followed by temporal
  resolve, motion blur, bloom, display and dither. Every earlier digest stayed
  unchanged. The shader source was restored byte for byte afterward.
- Cohort tests corrupt each pass lane and verify the first pass/node and sticky
  Program refusal, plus missing, failed, old-generation, wrong-version,
  wrong-bundle, relaxed/strict and adapter/driver mismatches.
- The actual node/session/Director heartbeat test carries distinct non-default
  evidence and explicit request generations in both directions. Codec tests
  cover all fields and reject incomplete or malformed evidence.
- The Output Monitor test verifies that the check emits its own action once
  and does not invoke a lifecycle operation.
- Strict Debug and Release each passed all **149/149** CTest entries. The final
  full Release run passed in 174.23 seconds. Both final GUI builds and the
  focused tests passed after the promotion-gate and panel-readability review.
- Native Release UI review used an isolated settings profile and the existing
  `out/step21-review.mstage` fixture at 1600 × 900. The automatic check passed;
  **Check render cohort** generated request 1 in the Director log and returned
  to the passing state. **Pass details** exposed the RTX 3090, driver
  `32.0.16.1656`, the complete two-line SHA-256 and all twenty matching digests.
  The small default dock was checked by scrolling from environment through
  output dither. Metadata is split across lines and expanded scrolling advances
  two pass rows per wheel step. The brief pending state was too fast to capture
  reliably in the native walkthrough; Program refusal is covered by the gate
  tests, including unsynchronized shows.

GPU verification here uses the local RTX 3090. A real multi-machine stage
cohort is still a deployment check: this implementation supplies the operation
that gathers and gates that evidence on those machines.
