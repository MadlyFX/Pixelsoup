# 29 — Physical sky and sun

Implemented 2026-09-08. This is the precomputed-scattering option from item 29:
a fixed single-scattering atmosphere built at GPU adoption, an authored solar
arc, and the existing SH/GGX environment pipeline. It requires Advanced.

## Operator workflow

1. Open **File → Project Settings → Renderer** and select **Advanced**.
2. Open **Environment → Sky and Fog** and enable **Physical sky**.
3. Set **Solar time**, **Latitude**, **North rotation**, **Haze**,
   **Solar illuminance**, **Sun radius**, and **Ground albedo**, then **Apply**.
   The time is local solar hours on an equinox day: sunrise 6, noon 12, sunset 18.
   North rotation is in turns; the sun-radius UI is in degrees.
4. To animate, seek to a show frame, set Solar time and click
   **KEY TIME AT PLAYHEAD**. Repeat at another frame. Time is a normal replicated
   scalar channel on the stage, using the existing key interpolation and undo
   path. The key buttons act immediately; Apply commits the other settings.
   **CLEAR TIME KEYS** removes the stage's solar-time channel keys.
5. Save the project. All eight sky settings and time keys survive save/reopen.
   Disable Physical sky to use the retained authored HDRI again.

The generated sun appears as a locked light in the scene. Its direction,
transmitted RGB illuminance and angular radius come from the sky controls.
It occupies the first directional-light slot so the existing cascade and
contact-shadow paths apply to it. Additional authored lights retain their own
controls. Physical sky ignores the retained HDRI source, intensity and rotation;
it does not wait for an unused HDRI to load. Camera exposure and the project
photometric scale still apply. Bright daylight needs a suitable camera exposure.

## Model and bounds

The implementation uses the standard spherical-atmosphere transport equations:
Rayleigh scattering, a Henyey–Greenstein aerosol phase function, exponential
density and Beer–Lambert extinction. The atmospheric geometry and transport
are described in the primary reference
[Bruneton's atmospheric scattering equations](https://ebruneton.github.io/precomputed_atmospheric_scattering/atmosphere/functions.glsl.html).
This implementation evaluates its own bounded single-scattering integral;
it does not implement that reference's full multiple-scattering precomputation.

| Quantity | Fixed value or authored range |
| --- | --- |
| Earth / atmosphere radius | 6,360 / 6,460 km |
| Observer | 1 metre above the ground, independent of the camera |
| Rayleigh RGB coefficients | 0.005802, 0.013558, 0.033100 per km |
| Rayleigh / aerosol scale height | 8 / 1.2 km |
| Aerosol scattering / extinction | 0.003996 / 0.00444 per km, multiplied by Haze |
| Aerosol asymmetry | 0.8 |
| Integration | 32 view samples × 16 solar-transmittance samples |
| Adoption table | 256 × 128 equirectangular RGBA16F |
| Default environment products | 256-pixel cube faces, six specular mips, nine SH coefficients |
| Specular samples | Existing 128-sample GGX chain plus 32 fixed solar-disc samples |
| Solar time / latitude | 0–24 hours / −89° to +89° |
| North rotation / haze | −1 to +1 turns / 0–8 |
| Solar illuminance | 0–200,000 lux above the atmosphere |
| Sun angular radius | 0.018°–9°; default approximately 0.266° |
| Ground albedo | 0–1 |

The ground hemisphere uses direct Lambert reflection. The solid Earth blocks
solar rays; twilight follows the illuminated upper atmosphere and midnight
has no solar illumination. This is an equinox art-direction model, with no
calendar, longitude, civil timezone, observer-altitude animation, ozone,
multiple scattering, clouds, stars or moon.

## Rendering and adoption

The atmosphere table is allocated with the renderer. Changed evaluated sky
settings or photometric scale record the complete table, cube, SH and specular
chain in one command list; unchanged/held state reuses it. Canceled recordings
invalidate the products and the next recording rebuilds them. There is no disk
read, wall-clock input, random seed, adaptive sampling or slice-dependent fit.
Animating time therefore incurs the full bounded adoption cost on each changed
frame; it never skips an update to meet a local budget.

Diffuse SH contains scattered sky and ground. Direct sunlight comes from the
generated photometric directional light, with the existing shadow visibility,
so it is not duplicated in diffuse SH. The finite disc is integrated into the
specular environment and drawn analytically as the visible sky sun. The generated
light omits a second direct specular term. Solar reflections consequently use
the existing environment occlusion/visibility approximation, rather than the
directional light's per-pixel shadow visibility. Existing local baked probes
remain static snapshots; use global IBL when continuously retiming exterior
reflections, or rebake the authored captures for the chosen time.

The compositor adopts the physical sky before drawing its background, including
the first frame and scenes containing only a camera. Background and geometry
use the same evaluated time. Sky-only scenes publish asset readiness through
the renderer. Advanced layer composition retains HDR radiance until the display
transform, instead of clipping a daylight sky when a geometry layer is added.
The fog environment term uses the same already-scaled SH. Offline diffuse and
reflection bakes build an equivalent worker-owned CPU atmosphere table, and
sky settings participate in bake-input identity. Rebake static lighting after
changing time or atmosphere parameters.

## Replication and evidence

The stage stores `sky_physical_enabled`, `sky_time_of_day`, `sky_latitude`,
`sky_north_rotation`, `sky_haze`, `sky_solar_illuminance`, `sky_angular_radius`
(turns), and `sky_ground_albedo`. The key channel is `sky_time_of_day`.
All fields enter the scene digest; default-disabled projects retain their
existing digest. Solar direction uses the scene's deterministic trigonometric
functions. Shader compilation retains the strict floating-point contract.

The cohort fixture is version **4**, with **24** pass digests. The three added
passes capture the physical scattering table, SH and all specular faces/mips.
They travel through the existing authenticated readiness and Program gate.
Operators can use **Output Monitor → Check renderer cohort** on the actual wall
machines. Local tests compare independent renderer instances on one adapter;
they do not constitute a physical two-machine wall or cross-vendor guarantee.

`mediaserver.physical_sky_model` covers non-default persistence, field and bake
digests, clamping, key evaluation and editing, solar direction, spectral
extinction, blue zenith and night. `mediaserver.physical_sky_gpu` covers repeated
and independent table/SH/specular bytes, CPU/GPU transport agreement, angular
radius, elevation, canceled adoption recovery, four solar times rendered as a
whole canvas versus two slices, held reuse, mode switching, and compositor
startup/HDR/same-frame updates. The editor workspace harness covers the controls,
key intent and Apply payload. Existing render-conformance and protocol tests
exercise the expanded evidence.

The Debug editor was also exercised with a scratch sky-only `.mstage` project:
Program reached **Advanced renderer ready**, time keys were authored at frames
0 (12.130436 h) and 7,850 (17.804348 h), and seeking between them changed the
sun and sky together. Haze was changed with Apply and the project was saved.
Reading the saved document confirmed both linear keys, all eight sky fields,
and the applied haze value (3.065217). Local evidence is under
`out/validation/item29/` (`physical-sky.mstage`, `saved-sky-state.json`,
`ui-keyed-sky.jpg`). Both complete Windows GUI builds succeeded. The complete
Debug suite passed **157/157** in 260.34 seconds and Release passed **157/157**
in 297.69 seconds. Build and test logs are `out/item29-{debug,release}-{build,tests}.log`.

Validation commands:

```powershell
cmake --build --preset windows-msvc-debug-gui --parallel 8
ctest --preset windows-msvc-debug-gui --output-on-failure
cmake --build --preset windows-msvc-release-gui --parallel 8
ctest --preset windows-msvc-release-gui --output-on-failure
```

For a small sky-only project to inspect in the app:

```powershell
out/build/windows-msvc-debug-gui/engine/render/mediaserver_physical_sky_tests.exe --write-demo out/physical-sky.mstage
```
