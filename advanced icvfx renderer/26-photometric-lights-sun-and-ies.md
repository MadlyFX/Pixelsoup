# 26 — Photometric lights, colour temperature, sun size and IES

Implemented on the D3D12 renderer. Step 16 supplies the shared lux / lumens /
candela conversion, deterministic temperature tint and replicated light scale.
Step 19 supplies angular-radius shadow penumbrae. This step completes the
visible sun disc and measured intensity distributions for point and spot lights.

## Light and asset contract

`scene.light` adds `ies_source` and `ies_rotation_turns`. An empty source retains
the original point/spot distribution. The source and roll survive document
commands, project persistence, evaluation and Inspector edits. They enter scene,
light-visual and bake-input identities; the source also enters asset-set and
project availability manifests. The renderer-profile capability tag changes so
older nodes cannot advertise this rendering contract.

The original Type C parser reads ASCII LM-63 files with `TILT=NONE`. It accepts
axial symmetry, horizontal coverage 0–90, 0–180, 0–360 and 90–270 degrees, and
vertical coverage 0–90, 0–180 or 90–180 degrees. Values outside the measured
vertical hemisphere are zero. Tables must be ordered and finite, intensities
nonnegative, and at least one intensity positive. Type A/B, tilt tables and
external tilt files are refused with a reason. The Type C conventions were
checked against [Radiance's ies2rad implementation](https://www.radiance-online.org/cgi-bin/viewcvs.cgi/ray/src/cv/ies2rad.c?revision=2.38&view=markup).

The worker expands symmetry and resamples to a 181 × 361 float web, including
the repeated azimuth endpoint. The atlas therefore has one-degree angular
resolution; sub-degree fixture detail is not preserved. Both surface shading
and volumetric fog use four explicit texture loads and bilinear interpolation.
The offline diffuse baker uses the same resampled web, orientation and intensity
normalization. It refuses a required profile that cannot be loaded.

The measured web is normalized to peak one. Candela is peak intensity; lumens
is total flux after the profile and the spot cone. Unitless intensity retains
its existing scale and is multiplied by the web. Lamp lumens, watts and common
candela/ballast multipliers in the file do not override the authored brightness.
Temperature and authored RGB remain the shared colour multiplier. Fixed
solid-angle rings are prepared by the worker; point flux is a lookup and spot
packing clips the rings at both cone limits before integrating the web times
smoothstep. This preserves narrow and hard-cone calibration, including cones
smaller than a ring. The ring approximation uses quarter-degree intervals.

The light's Aim vector is the zero-degree photometric axis. At zero roll,
horizontal zero points along world +X projected perpendicular to that axis;
near parallel to X it uses +Z instead. Positive roll rotates that tangent toward
`cross(axis, tangent)`. The spot cone and IES axis always agree. A point gains
Aim controls when a profile is assigned.

| Bound | Behavior |
| --- | --- |
| File | 8 MiB maximum; at most 721 vertical and 1441 horizontal input angles |
| Source | 1024 UTF-8 bytes maximum |
| Scene atlas | 16 distinct sources, 181 × 5776 R32F texels (about 4 MiB) |
| Shared worker | 64 cache entries with preallocated source strings; pinned data cannot be evicted |
| GPU adoption | Fixed scratch/texture allocation, one upload when changed, retried after canceled recordings |

No file read, hash, stat or blocking mutex occurs during recording. Observation
uses `try_lock`; loading, failure and capacity are visible in renderer statistics.
Failures are sticky until explicit refresh. Authored asset refresh and command
publication epochs fence worker results, and late replacements are discarded.
Slots come from the complete authored scene, independent of the camera, slice,
culling and compositor layer. A pending, failed or refused profile makes the
asset set unready and suppresses that light's new emission. The existing Program
asset-generation gate holds the previous committed image until the cohort is
ready. Replacing file bytes in place requires Library **RESCAN**; no timer reads
the files. Use a shared absolute path on every participating machine.

## Sun disc

Advanced's sky draws up to four directional-light discs, with or without an
HDR environment map. `angular_radius_turns` controls both the existing shadow
penumbra and the visible angular radius; zero preserves the previous background.
The disc centre is opposite the light's Aim direction. Radius is bounded to
0–0.249 turns. Its radiance is `E / (pi * sin(radius)^2)`, using the shared
photometric conversion, so a larger disc retains the same incident illuminance.
Camera exposure is applied once before the output transform.

The shader reconstructs rays in the complete canvas projection, including the
target-to-canvas scale used by preview rendering. Explicit adjacent rays set edge
coverage, avoiding derivatives that vary with slice placement. Half-float output
is bounded to 65504. Subpixel coverage is an approximation; there is no temporal
jitter. Basic supports IES surface shading but does not draw the Advanced sky;
an authored sun disc without that pipeline reports an idle reason.

This disc is a visible background source. Atmospheric scattering, time-of-day
authoring and feeding a physical sky/sun into SH and the specular environment
remain item 29. Rectangle/area IES emission remains outside this point/spot step.

## Operator walkthrough

1. In File > Project Settings > Renderer, select **Advanced**. The Colour tab's
   Photometric Lighting section retains step 16's **Light scale** reference.
2. Add or select a point/spot light. In Properties > Light, set **LM** or **CD**,
   intensity, range, and optional **Temperature ON** / Kelvin. Scroll below the
   cone and shadow controls to **IES profile (.ies)**.
3. Enter an absolute profile path and click **APPLY PROFILE**. For a repeatable
   example, use this repository's [synthetic asymmetric practical](fixtures/asymmetric-practical.ies).
   It has a bright zero-degree horizontal side and a dark opposite side.
   This is a test fixture, not a manufacturer's calibrated luminaire.
4. Double-click **IES rotation**, enter `0.5`, then Enter. The bright side turns
   halfway around the Aim axis. Change Aim to orient the fixture; for a spot,
   its ordinary inner/outer cone still clips the distribution. **CLEAR PROFILE**
   restores ordinary emission. Edit > Undo restores the source.
5. File > **Save Project**, then reopen it. Source and rotation should persist.
   If the file changes at the same path, use Library > **RESCAN**. Project
   Settings > Renderer > Geometry shows ready/pending/failed/refused IES counts
   and the last failure reason. Re-bake indirect lighting after a profile change.
6. Select a directional light, choose **LUX**, and aim it so its opposite
   direction lies in the camera's view. Change **Sun radius** from zero to a
   nonzero value (turns). For easy visual inspection try `0.01`, then `0.02`:
   the disc gets wider and its radiance drops while direct illumination keeps
   its authored lux value. Return to the intended angular size for the shot.
7. Output Monitor > **Check render cohort** reruns the compiled fixture. Fixture
   version 2 includes an asymmetric IES spot in the existing lighting/fog lanes,
   with embedded profile data and no filesystem dependency.

## Verification

`mediaserver.render_ies_lighting` tests known angular values, symmetry,
interpolation, malformed/unsupported input, independent numerical flux
integration, aim/roll, scene/asset/bake/bridge identities, shared worker
publication, sticky failure, refresh, overwritten requests and discarded late
results. D3D12 readbacks verify measured bright/dark directions, zero-profile
compatibility, rotation, fog response, sun size/radiance, zero-radius regression,
repeated bytes, exact adjacent-slice bytes and canceled-upload retry.
Uniform-profile flux tests include the minimum authored spot angle and hard
cones, and an excess-source GPU case verifies refusal and asset unready state.

Additional tests cover actual baked transport, required-file manifests and
nondefault project/wire round trips. Inspector tests exercise text apply/clear
and numeric rotation through normal edit intents.

Native UI review used a saved copy of the shadow-stability project. Profile
assignment, half-turn rotation, clear, Undo and File > Save Project were exercised;
Program's asset-ready transition was observed. The original review project was
preserved. Build and full-suite results are recorded in the renderer status.
Debug and Release each passed all 150 CTest entries. Both GUI builds and the six
affected suites passed again after the final cone-integration and Inspector
path-limit corrections.

GPU checks here establish repeatability and slice equality on the local GPU.
Physical multi-machine and cross-vendor qualification still require the deployed
cohort's render-conformance evidence.
