# 18 — Temporal stability in the inner frustum

Implemented 2026-09-07. This closes item 18 of the
[assessment](17-assessment-and-remaining-items.md).

## Implemented behavior

- Each participating node still renders the complete tracked plate. The
  compositor now reserves a guard band outside its visible image, widens the
  off-axis projection by the same ratio, runs neighborhood passes over that
  extent, and crops once after bloom. Padding is derived from authored filter
  support and stays fixed during camera motion. The history key describes the
  visible image, rather than its padded allocation. Tracked near/far planes
  accompany the view.
- Sequential filters add their support: AO, temporal resolve, layer filters,
  transmission, and bloom cannot consume the same border independently. A plate
  with bloom reserves the bounded six-level pyramid support so padding cannot
  deepen the pyramid beyond its budget. Ordinary slices align their origins to
  the derivative/pyramid grid, including odd-width node boundaries.
- The motion product is now **RGBA32F**: normalized UV motion in XY, current
  reversed device depth in Z, and the fragment's expected previous depth in W.
  The previous camera and rigid model transforms determine W, allowing valid
  camera/object motion to retain history. Initialize and Reset explicitly emit
  zero XY motion. Explicit views scale UV motion by their own padded raster;
  ordinary slices use the authored canvas.
- `TemporalResolve` stores matching color, object ID, and motion/depth history.
  It rejects each bilinear history tap independently on ID and predicted depth
  before renormalizing the accepted weights. The current 3×3 color box and fixed
  0.8 history weight remain. Depth tolerance is conservative: `1e-5 + 0.002 *
  abs(expected_depth)`. Very steep depth gradients can discard useful history
  rather than retain an invalid surface.
- With temporal AA enabled, AO accumulates through the same resolve
  implementation using R32F color history, the same geometry products, history
  slot, and Initialize/Advance/Hold/Reset decision. Its resolved scalar field
  lights the current frame. Hold returns the previous accumulation unchanged.
  Color and AO each have a bounded target pool; eviction reuses allocations.
  A canceled history write cannot make a later Hold read undefined GPU memory.
- Advanced shading widens the GGX and clearcoat roughness using fine geometric
  normal derivatives. The geometric variance is bounded and contains no random
  or time-dependent input. Material roughness and texture sampling remain
  authored values; the additional width accounts for subpixel geometry.
- Heartbeats carry temporal restart counts and the latest restart show frame.
  The director compares these alongside backend/profile readiness and holds
  Program on **TEMPORAL HISTORY MISMATCH**. Matching counts from different
  restart frames also fail. Reports include plate renderers and transition
  renderers from the current document/profile. A replicated document or renderer
  profile change resets history and starts a fresh reporting period. Increase
  history capacity or reduce the active views before clearing a persistent
  eviction fault.

The session protocol is **version 8**, and renderer digests include temporal
contract version 2. Update the director and all render nodes to matching builds
before joining a cohort. Restart evidence arrives through the existing heartbeat;
this is a readiness gate based on the latest reports, not a new per-frame GPU
consensus protocol.

## Operator controls

1. Open **File → Project Settings → Renderer**, select **Advanced**, and set
   antialiasing to **Temporal**. Enable occlusion and bloom as needed.
2. Under **Geometry → Filter Guard**, select **512 px** for the combined
   temporal AA, AO, and plate bloom path. The limit is saved with the project
   and replicated. It is a ceiling: targets allocate only the support required
   by the authored passes. A smaller limit refuses an unsupported render rather
   than reducing its filter. Wider layer effects may need a larger limit.
3. Play a camera move, then pause. Repeated frames should remain still. After
   changing renderer settings, Apply, save, and reopen the project to verify
   persistence. Cancel discards a settings draft.
4. A temporal history mismatch holds Program. Correct the resource/view
   pressure, then apply a replicated renderer-settings or project change to
   restart the cohort's histories together.

## Verification

`mediaserver.render_temporal_stability` verifies:

- Different current/previous depths under camera or object motion retain valid
  history, while depth discontinuities with an unchanged object ID reject it.
- A mismatched bilinear neighbor cannot leak through an accepted ID tap.
- Thirty-two scalar AO samples reduce variance, and held scalar/color frames
  remain byte-identical. Actual rendered AO also repeats exactly.
- Target eviction is bounded and reported; canceled initialization re-primes
  safely; a compositor document reset clears its restart evidence.
- Glossy curved geometry matches independently rendered slices across an
  odd-width node boundary.
- Thirty-two tracked plate frames combine TAA, AO, bloom, camera translation,
  rigid object motion, and changing plate position. Two independently rendered
  node slices equal the complete RGBA16F image byte for byte on every frame.
  Every repeated frame also matches, with zero unexpected restarts.

Network tests round-trip nonzero restart counts and a 64-bit restart frame,
and freeze the version-8 wire bytes. Renderer gate tests cover matching and
mismatching counts/frames and recovery after a profile reset. Existing motion,
media coverage, bloom, and scene tests cover the updated product contract.

Both full Windows GUI builds passed. Complete CTest suites passed **146/146
in Debug (237.03 s)** and **146/146 in Release (225.89 s)**. After the final
background-depth clear and guard-rounding fixes, both GUI builds passed again
and the focused nine-test suites passed **9/9 in Debug (94.90 s)** and **9/9
in Release (70.60 s)**. The additional guard-overflow assertion also passed in
Debug (3.12 s) and was included in the final Release focused run.

## Native editor walkthrough

The review uses `out/step18-review.mstage`, saved as a separate copy of the
textured-floor and photometric-lighting review project. In the native editor,
Temporal AA, 8 jitter samples, approximately 1.02 world units of occlusion,
strength 1, bloom 0.59, and a 512-pixel guard limit were applied. The timeline
played through frame 240 and paused with Advanced ready. The visible scene
showed the bloom contribution and retained stable geometry while paused.
Saving and reopening restored Temporal AA, all occlusion/bloom values, and the
512-pixel guard. The Running line reported temporal AA, occlusion, and bloom.
Changing the guard to 1024 and canceling left 512 selected on reopening the
dialog. The original editor session was preserved throughout.

The GPU comparisons run as independent renderers on this machine's RTX 3090.
Physical two-machine, cross-vendor, and photographed-wall qualification remain
the cohort-evidence work in item 23. Plate TAA upscaling remains deferred, as
specified by item 18. Higher-level CPU scene/history rollback after an abandoned
outer command list remains the existing limitation documented in
[renderer status](../advanced-renderer-status.md#deterministic-scene-and-temporal-state).
