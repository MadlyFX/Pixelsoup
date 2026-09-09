# 24 — Frame budget instrumentation

**Implemented on D3D12, 2026-09-08.** Advanced frames report measured GPU pass
costs, CPU recording time, upload traffic and native present observations. An
authored deadline participates in the existing Program readiness gate. It never
changes the authored image quality.

## Operator controls

Open **File → Project Settings → Geometry**, scroll to **Frame budget**, and set
**Frame deadline** in milliseconds. The field accepts three decimal places,
from 0 to 1000 ms. Zero leaves measurement enabled and disables the deadline.
The project stores integer microseconds in `scene.settings.frame_budget_us`.
A nonzero deadline joins the renderer profile digest and is replicated to the
cohort; the default preserves existing project digests.

**Output Monitor → Frame pass costs** expands the named GPU measurements for the
local Program candidate and each reporting node. The summary shows CPU record,
GPU submission, upload MiB, native Present CPU time, present-barrier counters and
the authored budget. Expanded rows include the completed submission serial and
dropped timestamp intervals. The panel scrolls by small increments while costs
are expanded, including in the default small dock.

The late-frame policy in **Hardware Map** determines enforcement:

- **HoldGroup** holds Program when a member exceeds the CPU or GPU deadline,
  or has no valid completed measurement for the current profile. The readiness
  message names the local machine or failing node.
- **AdvanceIndependently** reports the same timing fault without closing the
  renderer gate. Other readiness failures still hold Program.
- A fresh matching measurement under the deadline clears the timing fault.
  Candidate rendering continues while the previous Program image is held.

The CPU and GPU deadlines are checked separately; their durations are not added
because recording and queue execution can overlap. The observation describes a
completed submission, so this is a readiness decision using recent evidence,
not a prediction that can prevent the first unexpectedly expensive frame.

## Measurement ownership and bounds

`core::FrameTiming` has fixed storage for 27 stable pass categories. Categories
aggregate repeated executions such as shadow cascades, auxiliary replays and
bloom levels. They include sky, shadows and RSM attributes, depth, normals,
identity, camera/object velocity, coverage, AO/history, lighting/media, fog's
three stages, temporal/spatial AA, motion blur, depth of field, bloom's three
stages, composition, output and environment preparation.

Each D3D12 command context reserves 4096 timestamp intervals plus one pair for
the whole submission when the device is created. The graph executor brackets
each pass's transitions and body. Submission resolves only written query slots.
`SubmittedWork::frame_timing()` reads them only after its fence completes; the
owning ticket keeps the context from being recycled. A cancelled recording
publishes nothing. Overflow counts dropped intervals and marks the GPU sample
incomplete while preserving all rendering work.

The upload counter measures bytes reserved in the recording's existing upload
arena, including alignment, texture row padding and uniform uploads. It is
traffic into that arena, not resident VRAM. CPU timing uses `steady_clock`
around recording. Native Present time measures the CPU call; the existing
present-barrier statistics provide cumulative presents, synchronized presents
and synchronized flips. Neither is presented as a hardware photon timestamp.

`RendererDiagnostics` holds CPU time and upload delta for each rendered surface,
graph/pool statistics, and the latest matching completed host submission.
Pass timestamps and the heartbeat summary cover the owning submission: when
Program, transition views and Stage share an editor submission, their GPU costs
are aggregated. Per-surface CPU deltas remain available in diagnostics. The
node summary reports the maximum CPU duration and the most expensive GPU
submission, retaining that submission's pass breakdown and upload/present
evidence. An output with missing or stale evidence still fails the timing gate.
Total submission time also includes work outside named pass intervals. The
editor retains a fixed four-slot set of tickets, preserving its existing frames
in flight; the output presenter observes its existing pending ticket. Neither
waits for profiling results.

The optional `FrameTiming` table travels in `HeartbeatAck` and reaches Output
Monitor through runtime renderer readiness. Missing timing remains missing;
the decoder validates the fixed pass-vector length. The default absent table
preserves wire goldens. SDL_GPU reports GPU timestamps as unavailable; completed
host recordings still have CPU evidence. Timing values never enter image or
temporal-history digests.

## Verification and limits

`mediaserver.frame_timing` measures one and 64 clears of a 2048×2048 RGBA16F
target, compares five-sample medians, and requires the larger load to be
attributed to Shadows and grow by more than 4×. It also checks fenced results,
upload accounting, cancellation, query overflow, and sealed-pool reuse/refusal.
Renderer lockstep tests cover CPU/GPU overrun, stale profiles, missing results,
both late-frame policies, recovery, and unrelated failures after a timing fault.
Scene and heartbeat tests cover authored and wire round trips.

Verification on 2026-09-08, NVIDIA GeForce RTX 3090:

| Check | Result |
| --- | --- |
| Debug and Release GUI builds | Passed |
| Complete Debug CTest suite | 155/155, 261.75 s |
| Complete Release CTest suite | 155/155, 215.98 s |
| Final Debug Output Monitor layout recheck | Passed |
| Release median Shadows, 1 clear / 64 clears | 3,072 ns / 556,032 ns |

The timing comparison is a controlled local load test, not a production frame
rate claim. Its five-sample medians exclude the warmup recording.

Local UI verification exercised the exact numeric deadline and the monitor's
measured costs and scrolling. In the Release application, a 0.001 ms deadline
held the six-object Advanced scene while candidates continued rendering;
changing only the deadline to zero released Program through the existing
asset-activation boundary. Physical multi-node genlock/present-barrier
behavior still requires the hardware procedure in step 03; this change does not
claim a new hardware validation.
