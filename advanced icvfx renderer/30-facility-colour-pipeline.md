# 30 — Facility colour pipeline

Implemented 2026-09-08. Extends step 10 with OpenEXR ingest, a pinned OCIO
ACES 1.3 display transform, deeper native outputs and authored compensation
for each physical wall. The scene working space remains linear Rec.709 in
premultiplied RGBA16F.

## Operator workflow

1. Import `.exr` media through the Library, including numbered image sequences,
   or select an EXR environment in Project Settings > Environment. Tag ACEScg
   files with AP1/D60 chromaticities or an `ocio:colorspace = ACEScg` attribute.
   Untagged images use linear Rec.709. HALF and FLOAT RGB(A) are supported.
2. In **File > Project Settings > Colour**, select **ACES 1.3** and Apply.
   The existing sRGB and ACES Approx choices remain available. The show-look
   `.cube` field remains the project-wide creative grade.
3. In **Window > Feed Mapping**, select the physical surface, open **Colour**,
   enable compensation and enter its measured wall matrix and camera inverse
   `.cube` path. Widen or dock the panel in the central workspace to expose
   the calibration rail. Matrix rows are output R/G/B and columns are input
   R/G/B. Identity is the default; values are finite and bounded to [-16,16].
4. In **Window > Hardware Map**, select the render server and its saved output
   binding. **Precision** cycles through **8-BIT RGB**, **10-BIT RGB** and **16-FLOAT LINEAR**.
   Each binding keeps its own precision, display identity and raster. Changes
   use the existing output-window reconfiguration path.
5. Save the project. These controls use ordinary replicated document edits
   and Undo/Redo. After replacing a LUT file at the same path, use the Library
   **Rescan** action to advance the shared asset generation and retry the bake.
   A missing or invalid LUT holds Program; the worker log identifies its path.

The wall stage consumes calibration supplied by the facility. It does not
measure the LED wall, derive a camera IDT, or invert an arbitrary OCIO graph.
The camera slot expects a **pre-baked inverse compensation** taking and returning
unit-domain, display-encoded RGB. It is not a slot for a raw camera-log IDT.
Per-panel uniformity correction still belongs to the LED processor.

## Transform and worker contract

The bake order is scene-linear Rec.709 → display transform → show look →
sRGB decode → wall primaries matrix → sRGB encode → camera inverse LUT.
An identity matrix skips the decode/encode round trip. A disabled wall uses
the base display/show table. The result is baked into the existing 33³ atlas;
the frame shader retains its explicit, fixed-order trilinear lookup.

ACES uses OpenColorIO's built-in
`cg-config-v1.0.0_aces-v1.3_ocio-v2.1`, input `Linear Rec.709 (sRGB)`, display
`sRGB - Display`, view `ACES 1.0 - SDR Video`, and CPU optimization `NONE`.
This selects the reference RRT/SDR ODT supplied with the ACES 1.3 configuration,
independently of machine OCIO environment variables. The dependencies are
pinned through the repository's vcpkg baseline. See the official
[OCIO 2.2 release notes](https://opencolorio.readthedocs.io/en/latest/releases/ocio_2_2.html)
and [ACES configuration documentation](https://opencolorio.readthedocs.io/en/v2.5.2/configurations/aces_cg.html).

The CPU bake matches that processor at grid vertices. The 33-point shaper
and half-float atlas introduce interpolation and storage error between vertices;
the runtime result is not an exact per-pixel OCIO evaluation. The tested neutral,
primary and mixed-colour samples have maximum float-table error 0.001954 in
encoded RGB. The reference comparison permits 0.018; grid vertices permit 1e-6.
The existing nonnegative rational shaper remains in use.

One process-wide worker reads, parses, transforms and packs LUTs. Its bounded
256-request resident set prevents walls from replacing each other's tables.
Observations use `try_lock`, refreshes fence stale publications, and failures
remain sticky until a different request or generation. OutputTransform
preallocates sixteen wall atlas textures per window, selected by physical
surface ID for each viewport. More than sixteen compensated walls in a window
is refused. Wall settings join scene, mapping and asset digests; show/camera
LUT paths join the required asset manifest and shared Program asset gate.
Atlas uploads retain command-recording receipts: canceled recordings retry
the upload, while submitted recordings make residency persistent.

EXR decode uses the existing image worker. The raw decoder preserves HALF
channel bits, including subnormals and alpha; FLOAT is rounded to HALF. AP1 is
converted to the scene working space on that worker. EXR media is unpremultiplied
for the clip shader, which composites it once; environment decode forces alpha
to one. See [OpenEXR's read/write documentation](https://openexr.com/en/latest/ReadingAndWritingImageFiles.html).
Flat single-part scanline and tiled RGB(A) files are supported, including
nonzero data-window origins. Deep/multipart files, UINT or subsampled channels,
unknown/conflicting colour tags, AP0 containers, malformed/truncated files and
oversized images are refused. Limits are 512 MiB encoded, 32,768 per dimension,
and 64 million pixels. This does not add EXR material-texture packaging.

## Output precision

Native D3D12/DXGI supports R8G8B8A8, R10G10B10A2 and RGBA16F swapchains.
The output target, pipelines, retained Program image, backbuffer and resize path
all preserve the selected format. Dither uses 255 or 1023 levels and absolute
wall-pixel coordinates. Float output bypasses integer quantization and converts
the final encoded colour to linear Rec.709/scRGB with DXGI G10/P709 tagging.
Integer output uses DXGI G22/P709 tagging.

This is SDR colour at greater precision. It does not provide PQ/HLG, HDR10
metadata, twelve-bit packing, or automatic brightness calibration. The SDL GPU
fallback refuses deeper swapchains; it does not silently downgrade them.
Display mode, driver and LED processor settings must still preserve the selected
signal on the physical connection.

## Verification

`mediaserver.facility_color` covers exact HALF bits with NONE/ZIP/PIZ compression,
tiled files and cropped origins, FLOAT conversion, truncation, unknown tags,
ACEScg conversion, alpha, reference OCIO values, independent wall residency,
camera inverse file loading, refresh, invalid LUTs and document/digest contracts.

`mediaserver.facility_color_gpu` uses native D3D12. It verifies a 10-bit ramp
with 699 distinct values, byte-identical whole-versus-sliced dither, linear
float output, two differently compensated walls in one target, an EXR decoded
through the media worker onto an HDR quad, and creation of both deeper
swapchain formats. It skips only if native D3D12 device creation is unavailable.
Editor and deployment tests exercise ACES selection, precision cycling, matrix
edits, per-surface isolation, asset manifests and propagation to output windows.

Both full Windows Debug and Release GUI builds pass. All 153 CTest entries
passed in each configuration, including the corrected UI interaction test.
The final cancellation-recovery change was rebuilt in both configurations and
the affected colour, output, presenter, editor and deployment tests passed again.

The native editor was launched with an isolated local profile and colour test
project for layout, interaction and save/reopen checks. Physical LED output,
camera-chart matching and two-machine colour agreement still require the
stage procedure in [step 10](10-wall-colour-procedure.md) and hardware validation
in step 36.
