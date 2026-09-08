# Windows/NVIDIA genlock and present-barrier bring-up

This runbook is the physical validation procedure for Step 03. It applies to
wall outputs only: editor, preview, and operator windows deliberately do not
join the barrier.

## Scope and prerequisites

- Use two Windows render nodes with supported NVIDIA RTX/Quadro professional
  adapters, one Quadro Sync group, matching output timing, and a supported
  NVIDIA driver. Both nodes need access to the same house/video reference that
  drives (or is phase-locked to) the camera.
- Cable and configure the Sync board and displays in NVIDIA's management tools
  before launching Media Server. Verify that both outputs are in the same
  hardware sync group and that the driver reports a locked reference.
- In the hardware map, bind every wall feed to its actual Windows display
  adapter/target. The runtime deliberately refuses to infer a lock for an
  unmapped display.
- Author the wall's external-genlock/framelock policy and its nonzero present
  barrier group and barrier IDs. Every wall output in the synchronization group
  must use the same pair.

## Build the optional backend

From the repository root, configure and build each configuration with the
optional backend enabled:

```powershell
cmake --preset windows-msvc-debug-gui -DMEDIASERVER_ENABLE_NVAPI=ON
cmake --build --preset windows-msvc-debug-gui --target media-server

cmake --preset windows-msvc-release-gui -DMEDIASERVER_ENABLE_NVAPI=ON
cmake --build --preset windows-msvc-release-gui --target media-server
```

No NVAPI SDK headers or import library are required. At startup the Windows
platform layer loads `nvapi64.dll` from the installed driver. If the driver,
adapter, board, or native D3D12 wall swapchain is unavailable, the output stays
`Unsupported` and the Program gate remains closed; it does not silently use an
ordinary DXGI present.

Before using hardware, run the focused automated checks:

```powershell
ctest --test-dir out/build/windows-msvc-debug-gui --output-on-failure `
  -R "(rhi_present_barrier|sync_topology|frame_boundary_driver|net_session_codec|server_sync_device_probe)"
```

## Bring up two nodes

1. Start the director and both lifecycle/render nodes with the normal deployment
   configuration. Install the present-barrier backend before the first wall
   swapchain is created; the application does this automatically at startup.
2. Open the Director editor's **Output Monitor**. Check the physical target and
   sync group membership for both wall feeds before promoting Program.
3. Confirm every wall row is locked and reports the authored barrier identity.
   In a healthy external-reference deployment, the monitor should show the
   equivalent of `SYNC LOCKED`, `BARRIER joined`, `PRESENT SYNC N/N`, and
   `SHOW CLOCK LOCKED #<frame>` for every output. `N/N` must remain equal: it is
   the driver's `presents_in_sync/presents` count, not a local present count.
4. Promote Program only after all feeds are healthy. The frame boundary is then
   advanced from the video-reference counter. Presentation uses immediate mode,
   so DXGI vsync is not a second pacing clock.

## Camera validation and sign-off

1. Put a high-contrast moving edge or frame marker across the join between the
   two wall sections.
2. Shoot the wall with the production camera at a shutter angle/exposure that
   would reveal a one-frame seam. Record a take long enough to cross several
   hundred reference frames.
3. Preserve the recorded clip with the build revision, NVIDIA driver version,
   Sync-board configuration, camera shutter settings, and an Output Monitor
   capture showing the locked counters. Review the join frame-by-frame: there
   must be no split-frame tear or persistent one-frame offset.

Hardware validation is intentionally a deployment test, not an automated claim.
This repository's automated tests cover the fail-closed contract; the recorded
two-node result is the required evidence for a specific rig and driver.

## Fault behaviour to verify

Under an approved maintenance test, remove the reference or interrupt the Sync
group, then observe the Director:

- A missing/lost reference reports a reference/show-clock loss and stops
  advancing Program from the OS clock.
- A driver leave/failure or an in-sync counter lag reports `SWAP BARRIER LOST`.
- Program holds the last safe frame. No output is allowed to continue with an
  unsynchronised fallback.

Restore the reference and hardware group, wait for every mapped output to report
locked reference and equal present counters again, then explicitly resume normal
Program operation. Treat `SWAP BARRIER INCOMPLETE` as a configuration or support
failure: check the build flag, loaded NVIDIA driver, native D3D12 wall path,
authored group/barrier IDs, and physical target mapping rather than bypassing
the gate.
