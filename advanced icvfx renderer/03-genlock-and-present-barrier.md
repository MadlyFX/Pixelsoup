# 03 — Genlock and present barrier

**Track 1.**

## Why

A camera shooting an LED wall integrates whatever the wall is doing during the
shutter interval. If two panels advance to frame N+1 at different moments, the
camera photographs a tear across the wall that no post process removes. If the
wall is not locked to the camera's shutter, it bands.

Genlock aligns raster boundaries. A **present (swap) barrier** is what makes
every joined output advance from image N to image N+1 on the same boundary —
without it two perfectly genlocked outputs can sit one frame apart indefinitely,
which looks like a permanent parallax error between wall sections.

The contract for this already exists and has no implementation:
`rhi::PresentBarrierBackend` in `engine/rhi/include/mediaserver/rhi/device.hpp`,
with the process-wide registry in `engine/rhi/src/present_barrier.cpp`, and
`Swapchain::join_present_barrier` / `present_barrier_status` /
`present_barrier_statistics` already plumbed. Every query answers `Unsupported`
today, which the Program gate treats as `SWAP BARRIER INCOMPLETE` — honest, and
currently always true.

## Seams

- **New** `apps/server/nvapi_present_barrier.cpp` (or a small
  `platform/present-barrier` target) implementing `PresentBarrierBackend` against
  NVAPI's present-barrier functions on the native D3D12 swapchain. It must live
  **outside** `engine/rhi`: the RHI names no vendor, which is why the registry is
  a separate translation unit in the first place.
- `apps/server/` startup — `rhi::install_present_barrier_backend(...)` before any
  swapchain is claimed, behind a build option (`MEDIASERVER_ENABLE_NVAPI`) so a
  build without the NVAPI headers still compiles and reports `Unsupported`.
- `engine/sync/` — barrier status already has a home in sync readiness; make sure
  a node that has *left* the barrier is distinguished from one that never joined.
  `PresentBarrierStatus::Left` exists for exactly this and should hold Program.
- `engine/show/` — the show clock. See below; this is the half that is easy to
  forget.

## Two halves, and the second one is the one people forget

**Half one: the present barrier.** Implement the vendor backend, join every
output node's swapchain to the authored group/barrier
(`PresentBarrierDesc{group, barrier}` is already replicated project state), and
report `presents_in_sync` in node health. The number that matters is
`presents_in_sync`, not `presents`: a client still nominally joined whose
presents stop counting as in sync has lost the barrier, and the existing
`PresentBarrierStatistics` already carries both.

**Half two: the show clock must be driven by the video reference.** A present
barrier aligns *presents* to each other. It does not align them to the camera. If
the show clock is a free-running `steady_clock`, the whole cohort drifts against
genlock together — synchronised and wrong.

Check what currently advances the frame boundary in `engine/show` and make the
reference the timing authority: on Windows/NVIDIA that is the Quadro Sync board's
frame counter, exposed through NVAPI. The frame boundary already owns timing by
design (`PresentMode::Immediate` is deliberately chosen so presentation never
paces the show) — this step supplies the thing that *should* pace it.

## Design notes

- Joining is idempotent for the same descriptor, per the existing contract.
  Keep it that way; a re-join per frame is a stall.
- Never block the frame path on barrier state. Query the already-known status;
  the backend maintains it.
- A node that cannot join must **hold Program**, not present unsynchronised. The
  readiness path for this exists; wire the new statuses into it rather than
  inventing a second gate.
- Report the barrier's own numbers, not a local inference. "We called present 60
  times" is not evidence of sync.
- Vendor coverage: NVAPI covers NVIDIA Quadro/RTX with Quadro Sync. AMD has an
  equivalent through their pro driver. Nothing above `PresentBarrierBackend`
  should learn which one is installed.

## Multi-node hazards

- Any per-node fallback that presents anyway "so the operator sees something".
  An unsynchronised wall section is worse than a held one, because it looks
  correct in the room and wrong on camera.
- Treating `NotJoined` and `Left` the same. Losing the barrier mid-show is a
  distinct event and must surface as one.

## Tests

`present_barrier_tests.cpp` already exists and tests the contract against a fake
backend. Extend rather than replace:

- A fake backend reporting `Left` after N presents holds Program and surfaces the
  loss distinctly from `NotJoined`.
- `presents_in_sync` falling behind `presents` is reported as loss of sync even
  while status still reads `Joined`.
- Install/uninstall of the backend is safe around swapchain lifetime — a
  swapchain outliving its backend must not dereference it.
- With no backend installed, everything still answers `Unsupported` and the
  existing behaviour is byte-identical.

Hardware validation cannot be unit tested. It needs a documented bring-up
procedure: two nodes, one Quadro Sync group, a camera at a shutter angle that
would reveal tearing, and a recorded result.

## Definition of done

Two output nodes on one sync group present frame N on the same boundary, the
show clock is driven by the video reference rather than the OS clock, loss of
barrier holds Program and is visible in node health, and a build without NVAPI
still compiles and reports `Unsupported` honestly.

## Open questions

- **Is the show clock currently free-running?** Confirm before designing the
  reference-driven path; it changes how invasive this is.
- **AMD.** Worth implementing at the same time behind the same seam, or defer
  until a deployment needs it? Deferring is fine as long as nothing above the
  seam assumes NVIDIA.
- **Do editor/preview windows join the barrier?** They should not — they are not
  on the wall, and joining them would pace the operator UI to the wall's
  reference.
