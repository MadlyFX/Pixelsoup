# The 3D renderer

Current renderer behavior and authoring controls. Run commands from the repository root.


`engine/rhi` and `engine/render` are the two seams below the scene layer.
`engine/rhi` is a small, backend-neutral GPU device - textures, storage
buffers, pipelines, and three draw shapes (instanced quads, a depth-tested
"pulled" draw with no vertex/index buffer bindings, and a fullscreen textured
pass) - with an SDL GPU Basic backend and a native D3D12 Advanced backend.
The native path owns its DXGI swapchain and polls command/backbuffer
availability without waiting on the frame path. Nothing above the RHI names
either implementation, which leaves room for a Vulkan backend later.
`engine/render` is everything that turns a
`scene::Scene` value into pixels: it never reads the document, and the scene
layer never learns a GPU exists. A render node and the editor's Program/Stage
viewports share this exact path, so what an operator sees while authoring is
what the wall shows.

`HybridSceneRenderer` now owns backend selection above `SceneRenderer`.
Selection and quality policy are replicated scene state. Basic is the current
SDL GPU implementation; Advanced raster is the native D3D12 scene-linear
implementation, not a feature alias. Optional requests report an explicit
Basic fallback when the native product contract cannot be met. Required
requests return `CapabilityUnsupported` and hold Program. The later RT, DDGI,
ReSTIR, denoise and TAAU options remain fail-closed until their
passes exist.
See [advanced-renderer-status.md](../advanced-renderer-status.md) for the exact
capability and integration matrix.

**The Advanced renderer is built when a scene asks for it.** Every pass that
hangs off it allocates at construction and holds that memory for the
renderer's life - full-extent targets, the cascade maps, the shadow atlas, the
temporal history - so building it for a show that never draws through it is a
permanent cost for nothing. It used to be built whenever the *device* could
run it, which meant choosing Basic in a project saved no memory at all. A
presenter builds two of these renderers and a node builds one presenter per
deployed output, so four of them stood behind a two-output node: on a 6 GB
card the renderer held the whole card and media had nowhere to land.

`HybridSceneRenderer::prepare` now builds it, from the adopted scene and
outside frame recording; `readiness` prepares before it answers, so an owner
that does not call `prepare` is still handed Advanced when it authors it,
one frame later. Measured with `mediaserver_render_vram_benchmark` at
1920x1080, four executors - a two-output node - a Basic show falls from
8903 MiB to 4159 MiB. An Advanced show is unchanged at 8915 MiB, which is
also what the Basic show used to cost, because the two were the same thing.

A sky is the exception, and `prepare` takes the whole scene to catch it: the
compositor draws the environment behind its layers through the Advanced
renderer whatever backend the show selected, so a Basic show that authored
`map_source` or a physical sky still builds it. Only a show with no sky to
draw goes without.

**The planar mirror waits on the same terms, one level down.** It is a second
complete `SceneRenderer` at the owner's extent - not a spare target - so it
very nearly doubles an Advanced renderer: measured alone, an Advanced renderer
holds 604 MiB at 1080p and 1497 MiB at 4K, and the mirror adds 559 MiB and
1450 MiB on top. `SceneRenderer::ensure_planar` builds it for a scene that
authored `environment.planar`, and `prepare` reaches it when it adopts that
scene. A show that selected Basic never gets one: a planar pass needs a
scene-linear renderer and is refused without it, so building the mirror would
buy nothing. As with the sky, the decision reads the authored flag and not a
resolved receiver - what a frame can find must not decide what exists.

**One camera, many exact sub-frustums.** The camera is authored once, against
the project's shared `Canvas`, and a render node never adjusts it. Instead each
node narrows the camera to its own `CanvasRect` - an off-axis perspective
projection whose near-plane window is a sub-rectangle of the full one, with the
eye point and near/far planes untouched (`scene::view_for_slice`). Two adjacent
nodes therefore evaluate the identical floating-point expression for their
shared edge. The one-to-one canvas-slice tests check exact shared pixels;
physical wall agreement still requires the deployed-cohort qualification in
[capabilities and limits](../advanced-renderer-status.md). A four-node canvas
renders four slices rather than four full frames cropped afterward.

**`SceneRenderer`: one scene, one target.** Given a `Scene` and a slice, it
resolves every mesh key through an in-process cache - procedural primitives
generate on a miss, imported glTF/OBJ geometry loads and uploads once per
source file - then writes one GPU instance record per drawable. Opaque
instances stay batched by mesh for one instanced draw each; anything that needs
destination pixels (non-Normal blend modes, or any media clip, since a decoded
frame may carry alpha even at full layer opacity) is queued instead, sorted
back-to-front with document order and insertion order as tie-breakers, and
drawn as its own depth-tested pass with its own fixed-function blend equation.
These layers write no depth while they composite, so the sort alone would
draw two that cross through each other one wholly over the other, swapping as
the camera moves. Before compositing, every Normal-blend layer at full layer
opacity except the last therefore draws its fully opaque pixels (alpha that
an 8-bit target would still store as 255) into depth alone, using the same
shader with `BatchConstants.w` set. The bias depth-only occluders use keeps
that pass off each layer's own plane, so a layer never hides itself and
clips stacked on one screen are still ordered by the composite. A crossing
media screen and Background card then hide each other wherever either is
opaque. Translucent pixels still rely on the sort, so where a translucent
edge crosses in front of a layer drawn after it, the later layer covers it.
Lighting is a small, deliberately bounded PBR core in `scene.frag.hlsl` -
Trowbridge-Reitz distribution, height-correlated Smith visibility, Schlick
Fresnel, Lambert diffuse, a two-colour hemisphere ambient term standing in for
image-based lighting, ACES tone mapping, and explicit sRGB encode/decode.
Advanced adds cascaded directional shadow maps and a bounded one-bounce
reflective-shadow-map diffuse term for the first directional light. How many
cascades a show renders, and how strong that bounce is, are replicated project
settings beside the backend: a node choosing either for itself would light its
share of the wall differently from the node beside it. Every cascade is fitted
from the authored full-canvas camera and the complete caster set - never from
the node's own slice - and a receiver picks its cascade by world position, so
two nodes read the same texels for the same point.

**Directional soft shadows (PCSS).** In Advanced, select the first directional
light and increase **Sun radius** in its Inspector. The light Inspector uses
turns: `0.000739` is approximately a 0.266-degree angular radius (a 0.532-degree
diameter). The physical sky's Sun radius control shows degrees. Larger sources
produce wider soft edges; shadows tighten as a caster approaches its receiver.
Zero source radius uses the existing fixed-width shadow filter. **Scene Settings
> Shadows > Filter radius** controls the minimum filtering footprint in shadow
texels, rather than the physical size of the sun.
The default minimum is two texels; lowering it toward 0.5-1 texel makes contact
hardening easier to see for a small, realistic sun, at the cost of less edge
antialiasing.

The directional filter uses 33 blocker-depth samples (the centre plus a fixed
32-point disk) and 32 manually bilinear comparison taps. Its search footprint
depends on source size and available light-space depth. Penumbra size uses
linear receiver/blocker separation and each projection axis's world texel size,
including rectangular full-scene fits. Search and filter radii are bounded at
16 texels per axis; very large sources or distant receivers saturate that bound.
Receiver-plane correction and cascade guard bands protect slopes and edges.
Sampling has no frame-varying noise or temporal history, and all nodes use the
same fixed work budget. PCSS adds no shadow maps, frame allocations or screen-space
apron requirement. It is an approximation: thin blockers between search samples
and large penumbras remain limited by shadow-map resolution and sample density.

Spotlight shadows retain their fixed-width atlas filter; spotlight PCSS is not
implemented. Point/area-light shadow maps also remain unsupported. IBL and
offline baked lighting have separate paths described below.

Brightness and Saturation are inline material/media operations, so active
values stay in the ordinary shared-depth draw. Saturation is colourfulness
around Rec.709 luminance, from monochrome at `0` through identity at `1` to a
bounded maximum of `4`; it runs in linear light for Exposure projects and on
encoded display values for Gain projects. Both lit geometry and decoded media
use the same contract. Brightness and Saturation factors retain one product
across first-class effects and graph modifiers, then apply their final clamp
and project response once. Blur remains a later isolated spatial operation. A
graph or first-class effect stack that authors an active inline operation after
Blur is rejected visibly instead of being silently reordered; identities are
inert. Because graph modifiers currently resolve back to authored scene nodes,
an Output-reachable graph also permits only one Scene Layer for a given source;
a duplicate is diagnosed rather than allowing two branches to leak effects
into each other.

Levels, Lift/Gamma/Gain, Contrast/Pivot, Channel Mixer, White Balance, and
Curves stay
as typed operations in that same bounded authored-order inline program; they
are not reduced to interchangeable scalars. Both lit geometry and media execute
the identical operation sequence. White Balance resolves its documented
D65/Bradford adaptation to a binary32 matrix in the model, while Channel Mixer
applies its authored row-major 3x3 matrix plus RGB offset without an intermediate
clamp. Curves validates strict master/optional RGB control points and derives
the same 1024-entry binary32 monotone-cubic LUT on every node; the GPU samples
that derived LUT without turning it into a second authored representation.
Identity operations remain visible for editing but add no shader op or render
pass.

Film Grain is another ordered inline operation. It adds deterministic
zero-mean monochrome or RGB texture without changing alpha, depth, or bounds.
Its integer hash uses the replicated seed, exact 64-bit show frame, authored
canvas-pixel coordinate, and channel; independently rendered adjacent wall
slices therefore produce byte-identical noise at their shared coordinates.
Amount `0` is identity and adds no shader operation.

A Background card's fill (`scene::Fill`, the same type as the compositor's
Background node) rides the front of the drawable's inline program as an
operation of its own kind, which every colour-effect evaluator skips. The CPU
measures the ramp in the card's world width and height (UV u along local X, v
down local Y on a quad or along local Z on a plane) with the deterministic
trigonometry the compositor uses, so the shader only places the pixel and
interpolates, premultiplied, between the two display colours. The decoded
colour multiplies the base and emissive colours before lighting and its alpha
the coverage; a translucent fill makes an opaque surface blend. The fill is
part of the material digest. It costs four parameter vectors of the bounded
inline budget, and exceeding that budget refuses the recording like any other
inline operation.

**The compositor graph runs as isolated render passes, not shader tricks.**
`CompositorSceneExecutor` evaluates the typed graph (`engine/compositor`) into
an ordered stack of layers, renders each into its own transparent offscreen
`SceneRenderer` target, spatially blurs it if the layer's style calls for a
blur, and composites it over an accumulator texture with its blend mode - one
fullscreen pass per layer, ping-ponging between two textures so no layer reads
the target it is writing. A project with no authored graph gets the same
treatment implicitly: consecutive Normal/no-blur drawables stay one pass,
and only a layer that actually needs isolation (a blur, a destination-reading
blend) pays for one.

The compositor owns one set of scene fog, bloom and object motion-blur targets.
Its isolated renderers retain their antialiasing and geometry products without
allocating duplicate copies of those deferred passes. The first layer also owns
the planar reflection capture of the complete scene; subsequent layers sample
that same capture without constructing extra reflection renderers. This reduces
the cold allocation cost of graphs with many plates, including LED Car Process,
while preserving layer order, source resolution, blends and reflection content.

**Isolated layers keep the frame's depth.** Isolating a layer costs it the
shared depth buffer, and a flat premultiplied composite has no way to put that
back: adding a brightness or blur node to an object used to lift that object in
front of everything else in the scene. Each pass is therefore also handed the
drawables belonging to *other* passes that stand in front of it, and renders
them for their depth alone - they write depth and return a fully transparent
black, so the layer is occluded exactly as it would be in a single-pass render
and nothing of theirs reaches the colour target. A clip does this through an
alpha-tested depth pass, so it occludes exactly where its own pixels are
opaque. Only crisply opaque geometry qualifies: a translucent or additively
blended drawable does not hide what is behind it, and a blurred one has no hard
silhouette to occlude with, so neither writes depth on another layer's behalf.
Depth-only geometry is biased a few thousand float steps away from the camera,
so two coplanar layers are still ordered by the composite rather than
cancelling each other in a reversed-depth test neither can win. Program, Stage, and each side of a Preview dissolve
composite through their own scene's graph independently - see
[Program and Preview](operator-guide.md#program-and-preview) - which is why rehearsing a
different composite in Preview never touches what Program is showing.

**Media and imported geometry share one decode contract.** `MediaSource` turns
a `(source, frame_index)` pair into pixels, frame-exact and pure - the same
pair always yields the same bytes on every node, with direct seek to any frame
rather than nearest-keyframe. Image sequences decode with stb_image; video
decodes through FFmpeg's demuxer/decoder registry - whatever codecs the linked
build carries - on an async worker so decode never blocks the frame path.
`MediaCache` is the GPU residency on top: sixteen resident frames
or 512 MiB including texture/staging ownership, whichever binds first, with current and
committed frames pinned across a frame so a reused slot is never overwritten
mid-draw. Large media transfers span bounded command recordings; partial images
are never published, and cancellation retries only unaccepted rows. Shared
upload pressure holds the last complete frame until the next transfer finishes.
See [media playback and codec limits](media-playback.md) for supported containers,
qualification and performance boundaries. `model_loader` reads glTF/GLB and OBJ into the same vertex/index
arenas `SceneRenderer` already uses, preserving per-primitive material
boundaries and PBR textures so an imported asset shades through the identical
pipeline as a procedural one.

**Advanced texture streaming.** Imported model textures use complete mip tails
and a deterministic full-canvas residency budget. In **File > Project Settings
> Geometry**, use **Mip streaming**, **Texture memory** and **Mip bias**; the
same pane reports residency and loading/refusal reasons. The default 512 MiB
envelope includes replacement headroom. New model imports prepare BC7 colour/data and
BC5 normal mip chains on a worker and reference immutable `.msmodel` packages.

**Extended materials and model ingest.** The renderer supports glTF clearcoat,
sheen, thin transmission, IOR, specular and anisotropy. Select an object and open
**Properties > Surface Layers** to edit its numeric factors, or use **Library >
Models > Import Model** to retain authored glTF factors and maps. Import, Add
Model and UV Mesh Browse report background job progress and specific refusals.
See [capabilities and limits](../advanced-renderer-status.md) for unsupported
rendering paths and qualification boundaries.

**Model animation.** Imported glTF node animation and skins now follow the exact
show frame, including Hold and seek. Select a model and use **Properties > Model
Animation** to choose its clip, looping and start frame, or add deterministic
foliage wind. Mark moving models **Bake Geometry: Dynamic** before baking.
The [two-joint example](../advanced%20icvfx%20renderer/fixtures/show-frame-skin.gltf)
provides a self-contained import fixture.

**HDRI background fog distance.** In **File > Project Settings > Environment >
Sky and Fog**, enable fog and set **Background distance** in metres. Start with
5-10 for an indoor panorama. Zero keeps the far-clip background and its existing
85% haze limit; a positive value resolves the full atmosphere at that view depth,
clamped to the camera's clip range. Scene objects retain their own fog depths.
This changes background fog only: the HDRI's perspective, reflections, and
baked lighting are unchanged, and no rebake is needed.

**Baked global illumination.** In **File > Project Settings > Environment >
Baked Lighting**, set the probe volume and choose **Bake Lighting** or **Rebake
Lighting**. A worker traces static geometry and marked lights with visible
progress and cancellation. The resulting immutable field adds soft diffuse
bounce and visibility-based contact darkening. Program holds its previous image
until every required node has the complete asset generation and receives the
shared activation frame. **Library > Rescan** uses the same publication path
for sources replaced at the same path. The release also includes `media-bake`.
Program displays the preparation reason, missing output node, activation
countdown, and elapsed wait. Bake progress remains in the main status bar after
closing Project Settings, and completion confirms when lighting is live in
Program. Reopening a project starts fresh readiness rather than replaying a
saved session's activation. Rebake existing HDR lighting assets to use the
corrected environment rotation, filtering, and bright-source sampling.
The baker and generation-readiness tests live in `apps/bake` and `engine/render`.

**Program transitions and the output stage.** On the native path,
`SceneMixer`, compositor processors, and `ProjectionMeshWarp` retain Advanced
colour in premultiplied scene-linear `RGBA16F`. `SceneMixer` blends two fully
rendered scene textures by one control-frame-driven scalar - never
object-level, so a dissolve is independent of layer count or effects and every
node performs the identical deterministic mix. `ProjectionMeshWarp` is the
nDisplay-style path for a curved or off-axis LED surface: the scene renders
once through a generalized asymmetric view, then this pass rasterizes the
physical surface in its own UV0 space and samples the scene render through
each vertex's projected clip position, so one warp handles a curved wall
instead of hand-aligning a camera per LED column. `OutputTransform` is the
final display boundary: it takes Advanced linear input through one baked
table - the authored display transform composed with the show look, built off
the frame path by a residency worker - and dithers it to the window's
selected integer output format with a fixed pattern keyed to the absolute wall pixel,
so two nodes round the pixel they share identically. Basic input remains a
straight sample. Applied last, after warp and after the switcher's dissolve;
per-panel calibration stays on the LED processor.

**Orthographic views.** `scene::Camera::orthographic_height`, when positive,
makes `view_for_slice` orthographic: the window is world-sized at every depth,
clip w is one, and depth is linear but still reversed (near 1, far 0), so
slices, jitter and the shared-edge guarantee keep their perspective forms.
`scene::orthographic(Mat4)` tells a projection apart from the matrix alone.
No document property sets the height, so every authored camera - and every
node's camera - is perspective; it is in the scene digest all the same. The
Stage's Top, Front and Side views reach the renderer as an explicit
`SceneRenderView` over the perspective authored camera, which keeps driving
level of detail and shadow-cascade fitting exactly as it does for the
perspective Stage view. A frame whose *own* camera is orthographic is refused
with `RendererReadinessCode::OrthographicCamera` ("ORTHOGRAPHIC CAMERA NEEDS
A STAGE VIEW"), because those camera-derived fits assume a perspective cone.

Each pass that reads the view was checked against an orthographic matrix:
ambient occlusion, contact shadows, the sky and the planar mirror reconstruct
through the inverse view-projection or the matrix itself and need nothing
more; the lit shading and contact-shadow separation take their view vector
from `eye_position`, whose w is 0 in an orthographic view to carry the one
view direction instead of a point; temporal resolve validates history against
an absolute depth tolerance (`orthographic_history_depth_tolerance`, two
pixels of world height in linear depth) instead of the perspective relative
one; and motion vectors are matrix differences. Volumetric fog would
integrate the air between the far-off eye and the stage, so in an
orthographic view it is reported `PassState::Idle` with "atmosphere is not
drawn in an orthographic view" and not drawn.

**Editor view shading.** `SceneRenderView::shading` lets an editor view
draw `Unlit` (base colour plus emission, unexposed), `LightsOnly` (a neutral
0.5 grey dielectric under the scene's own lights and shadows) or `Wireframe`.
The choice reaches the fragment stages as `view_mode.x` in the scene
constants, placed right after `grain_extent` so the media stage, which
declares the block only that far, can read it too. Wireframe draws the
colour passes through pipelines built with `rhi::RasterState{Wireframe}`
(`D3D12_FILL_MODE_WIREFRAME`, `SDL_GPU_FILLMODE_LINE`, gated by
`DeviceCapabilities::wireframe_fill`); a renderer makes them only when
`RendererLimits::view_shading` asks, which the Stage's executors do and no
output node does, and a wireframe request to one without them is refused.
The colour pass clears its own depth, so every edge shows through the others.
Shading folds into the view's history identity, so a change resets temporal
history; Unlit and Wireframe skip the planar mirror and report fog
`PassState::Idle` with "atmosphere is drawn only in Lit and Lights only
views". An output view is always `Lit`, which changes nothing it draws.

**Remaining unsupported paths:** a native Vulkan backend, GPU-driven
indirect/meshlet submission, point/area-light and ray-traced shadows, ray tracing,
DDGI, ReSTIR, denoising, and temporal upscaling. The integration contracts refuse those
unsupported Program requests; they do not manufacture render products. The
remaining work is tracked in
[roadmap](../next-steps-plan.md).
