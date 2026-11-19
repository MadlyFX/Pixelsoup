# Media playback and codec limits

Use **Library > Add Media** to place a file directly, or **Import Media Asset**
to register it first. Both pickers expose MP4, MOV, MKV, WebM, AVI, MXF, M4V,
MPG/MPEG and WMV, plus PNG, JPG/JPEG, BMP, TGA, EXR and HDR. The container
extension does not establish codec support: the background decode probe opens
the stream and receives a frame on the actual machine.

## Offline media

A missing image, sequence frame or video draws an opaque grey box with a dark
X on its existing surface. Its position, size, layer opacity and compositing
order stay the same, and other media and render output continue. Library and
Output Monitor still report the missing source so it can be relinked or repaired.
While an available source is decoding, playback keeps its previous complete
frame; a completed decode failure displays the placeholder. Missing media does
not hold Program adoption. Required proxy failures for an available source and
missing models, calibration or lighting assets retain their readiness checks.

## Large images and shared upload capacity

A 6000 × 4000 JPEG expands to 96,000,000 bytes of RGBA pixels before upload,
regardless of its compressed file size. The Advanced renderer's fixed upload
arena holds 80 MiB per recording, shared with other media, geometry and draw
data. Images exceeding that arena now transfer complete rows over successive
recordings. The texture becomes visible only when all rows have been recorded;
canceled recordings do not advance accepted progress. No scaling is applied to
make the upload fit.

This also handles several videos exhausting the shared arena. An unfinished
transfer retains its original source-frame identity and completes before that
placement admits another frame. Until it completes, the renderer holds the
placement's previous complete image. Overload can therefore still reduce
playback cadence, but upload exhaustion does not permanently mark a decoded
frame unusable or expose a partly filled texture.

GPU media residency remains bounded to 512 MiB, including conservative
texture/staging accounting and pending transfers. A frame whose accounted size
exceeds that budget cannot become resident; several large simultaneous images
also compete for this budget. Output dimensions, available GPU memory and
actual decode/upload throughput remain separate limits. Increasing output
resolution does not increase decode or upload capacity.

A frame decoded onto the renderer's own device is exempt from that budget,
because it occupies no memory the cache allocated: see the section below. The
decode scheduler bounds those frames separately, at a sixth of the chosen
adapter's own memory (clamped to 256 MiB … 2 GiB) — about 1 GiB on a 6 GB
node, or twenty 4K frames of read-ahead for each of four plates.

## Professional codecs

The Windows GUI dependency build includes FFmpeg decoders for ProRes,
DNxHD/DNxHR, HAP and NotchLC. All enter the ordinary MOV/AVI/MXF media path;
there is no dedicated codec mode to enable.

| Codec | Current path and qualification |
| --- | --- |
| ProRes 422 | Software decode; synthetic 10-bit 4:2:2 MOV frames are decoded and sought in reverse order by the regression suite. |
| DNxHR LB / HQX | Software decode; synthetic 8-bit and 10-bit 4:2:2 MOV frames are decoded and sought in reverse order by the regression suite. |
| DNxHD | FFmpeg decoder present; individual profiles, containers and operational throughput still require representative media. |
| HAP | FFmpeg decoder present; decoded CPU pixels follow the ordinary upload path. Native compressed-texture GPU playback is not implemented. |
| NotchLC | FFmpeg decoder present; software path only. Representative production-file correctness and throughput have not been qualified. |

These decoder entries are not a promise of real-time playback for every
resolution or profile. None of the hardware paths below apply to them: they
have no hardware decoder, so they neither reach the renderer's device
directly nor avoid the CPU conversion.

What they do share is the two-plane destination. A software conversion lands
on luma plus one interleaved chroma plane, at the source's own depth and its
own subsampling, rather than on eight-bit RGBA:

| source | destination | bytes/pixel (RGBA8 was 4) |
| --- | --- | --- |
| 4:2:0 8-bit | NV12 | 1.5 |
| 4:2:2 8-bit | NV16 | 2 |
| 4:4:4 8-bit | NV24 | 3 |
| 4:2:0 10/12-bit | P010 | 3 |
| 4:2:2 10/12-bit | P210 | 4 |
| 4:4:4 10/12-bit | P410 | 6 |

Ten bits reach the GPU as ten bits, and a 4:2:2 master stays 4:2:2 instead of
having half its chroma resolution resampled away to fit one fixed shape.
Anything deeper than ten bits is carried at ten, which is what the media
shader reads.

Where the source already says what the destination says, the renderer does
the conversion itself rather than calling libswscale: copy the luma rows,
interleave U and V, and shift each sample so its top bit reaches the top of
its word. That shift is what makes depth irrelevant - a code filling the top
of a sixteen-bit word is the same fraction of full scale whether it came from
ten bits or twelve - so there is no separate high-bit-depth path.

It runs only when nothing but data movement is wanted: one plane per
component, little-endian, no bit offsets, and a colour resolving to Rec.709,
with the range choosing between the Limited and Full layouts so neither needs
levelling. Another matrix, alpha, RGB or an unusual plane layout still goes
through libswscale, and a frame the pack refuses hands its clip back to
libswscale rather than failing.

Measured on an RTX 3090 at 3840x2160, conversion time per frame:

| | ProRes 422 HQ 10-bit | H.264 4:2:0 8-bit |
| --- | --- | --- |
| libswscale → RGBA8 | 16.9 ms | 5.9 ms |
| libswscale → two planes | 19.5 ms | 3.3 ms |
| the pack above | **7.0 ms** | **3.5 ms** |

libswscale has no tuned ten-bit semi-planar converter, which is the whole of
that difference; its eight-bit paths are good and show no gain. End to end,
one 4K ProRes 422 HQ plate at 24 fps reaches `ready_ratio` 0.958. High
-bit-depth 4:4:4 still uploads half as much again as RGBA8 did, which is the
one shape that costs bandwidth to keep its depth.

A source keeps the interleaved RGBA destination when it has an alpha channel,
when it is already RGB, when its subsampling has no two-plane equivalent, or
when its colour cannot be resolved to Rec.709 - which includes untagged
standard definition, where guessing between the 601 variants would be worse
than converting. Hardware-decoded Rec.709
P010 has a separate two-plane 10-bit path on supported Advanced configurations.
Hardware-decoded Rec.709 NV12, in either range, travels as eight-bit video
planes: no CPU colour conversion, and one and a half bytes per pixel stored
and uploaded instead of four. Colour spaces other than Rec.709 retain the
generic path.

H.264 and HEVC omit their colour description unless an encoder writes one,
which camera originals routinely do not. An unspecified range is limited by
definition, and an unspecified matrix at 720 lines or more is read as
Rec.709 - so such a plate takes the video-plane path rather than a CPU
conversion, and is converted with the right matrix either way. Standard
definition stays unresolved rather than guessing between the 601 variants.
HDR transfer-function handling and additional high-bit-depth video layouts
need explicit qualification before using them for precision-critical delivery.

## Which GPU decodes and renders

A machine with more than one adapter is the normal case for a laptop node, and
the wrong choice is expensive: the integrated part has a fraction of the
decode rate and, with 128 MiB of dedicated memory against a discrete card's
several gigabytes, cannot hold even two 4K video surfaces.

Both the D3D12 device and the hardware decoder therefore choose the adapter
with the most memory of its own, rather than accepting a platform default.
DXGI's `HIGH_PERFORMANCE` preference is not reliable for this: on at least one
laptop in this cluster it answers with the integrated Intel part first, ahead
of an RTX 2060. Dedicated memory is the honest discriminator, because an
integrated adapter shares system memory and reports almost none of its own.

The decode budget below is sized from that adapter in turn, so the same build
reads a laptop node and a workstation differently instead of assuming either.

## Decoding onto the renderer's device

On the native D3D12 backend - the output nodes and scene views - plates are
decoded straight onto the device the scene renders with, and the renderer
samples the decoder's own surface as two plane views. Nothing is read back to
system memory and nothing is uploaded, which removes both the largest cost in
the media path and all of its PCIe traffic.

This needs every one of the following, and falls back to the system-memory
video-plane path per source when any of them is not true:

- the renderer is on the native D3D12 backend; SDL Basic keeps the
  system-memory path;
- every live scene renderer in the process is on one device. A decoded
  surface belongs to the device that made it, so while a second device has a
  scene renderer the path is suspended rather than handing a surface to the
  wrong one, and it resumes when one device is left. Direct3D 12 gives every
  device created on one adapter the same device object, so the outputs of a
  node driving several displays from one GPU are one device here; two
  adapters are two;
- the source resolves to Rec.709;
- the surface is NV12 or P010. Both depths are carried: an eight-bit surface
  is read as R8/Rg8 plane views and a ten-bit one as R16/Rg16, and the RHI
  refuses a view whose depth does not match the surface rather than
  reinterpreting the bytes. A 4:2:2 or twelve-bit hardware surface, should a
  later codec produce one, goes back through system memory.

Each scene renderer offers its device for as long as it exists and the offer
is withdrawn with it, so an output redeploy - which replaces every renderer -
moves decode to the device the new renderers are on. Clips already open are
reopened onto it at their next frame, at the cost of one walk to a keyframe.
A frame left on a device that is no longer decoded onto is decoded again
instead of being handed to a renderer that cannot read it, so a paused clip
does not hold a picture nothing can draw. With no scene renderer at all,
video decodes through system memory.

Such a frame is held until the renderer is finished with it: releasing it
would return the surface to the decoder's pool while a submitted command list
is still reading it.

HEVC and AV1 surfaces are padded to 128 pixels, as the DXVA specification asks
and as FFmpeg's own D3D11VA path already does. FFmpeg 7.1's D3D12VA path
allocates them at the picture size instead, and a picture whose height is not
a whole number of coding blocks - 2160 is not - is then decoded past the end
of its surface: on an RTX 3090 the driver removes the device, which is the
renderer's device too. A shader samples a whole mip in normalized
coordinates and no view can crop one, so a padded surface cannot be sampled
where it lies; its picture is copied on the device into a surface of exactly
its size (`crop_video_surface` in `engine/render/src/d3d12_video_halve.cpp`)
and the decoder gets its own surface back at once. A half-resolution clip
halves straight from the padded surface instead. H.264 surfaces are already
the picture size and are still sampled in place.

## A clip shown smaller than its source

Decoded frames carry one mip level - a decoder surface has no others - so a
plate drawn smaller than its source is filtered in the media shader instead
(`engine/shaders/scene_media_sampling.hlsli`). Each output pixel averages a
grid of bilinear taps across its own footprint in the source, sized from the
screen-space derivatives of its texture coordinate, so a 4K plate shown 100
pixels wide reads all of the picture under each pixel instead of one texel in
38, and fine detail averages out rather than shimmering as the plate moves.
Each plane is filtered at its own size, and the clip's alpha coverage in the
Advanced products uses the same filter as its colour.

- A pixel at or above source size takes one tap at its own coordinate, the
  same fetch as before; only minified pixels change.
- Each screen axis takes its own tap count, so a plate at a grazing angle is
  filtered along its length and stays sharp across it. A pixel takes at most
  16 x 16 taps, which is enough for an exact average up to 32x reduction.
- Total cost is set by the source, not the screen: one view of one plane
  costs about a quarter of a fetch per source texel, however small the clip.
- The result depends only on how canvas pixels map to the source, so every
  node rendering the same canvas region at the same scale produces the same
  pixels. A scaled preview of the canvas filters at its own scale.

It does not reduce decode cost or decoded-frame memory: the full-resolution
frame is still decoded and held. Holding a clip at half resolution, below,
does.

## Editor proxies

The editor decodes a 720p H.264 proxy for every video asset larger than
1280 x 720 instead of its source. The Director makes these proxies itself,
in the background, as soon as such an asset is in the project; output nodes
never make or play them, and they are not saved with the project. Until an
asset's proxy is ready its clips show the source, so nothing waits.
Switching between proxy and source maps through the proxy's frame table, so
no cut or key moves by a frame.

The proxy cache sits beside the source, often on a network share, so frames
never wait on it: the Director checks each proxy's cache entry once, then a
background worker keeps that state current. A proxy or segment the Director
publishes is used from the next frame; a cache entry changed from outside -
deleted by hand, say - is noticed within about a quarter of a second. A
proxy's frame table is checked once, when its file is copied out for
playback, and a table that fails the check is reported as a frame-mapping
failure for that clip.

A proxy is made in segments of about ten seconds of source, each usable as
soon as it is published. The segment under each clip's playhead in Program
and Stage is made first, then the rest of the file from there, then its
start; when every segment is made they are joined, without encoding again,
into one proxy file, which then replaces them. Consecutive segments come
from one encode that starts a new file at each boundary, so making a proxy
in segments costs little more than making it whole. A clip whose segment is not made
yet shows its source. Moving from one file to another of the same asset -
source to proxy, segment to segment, segments to the joined file - is not a
cut: the clip keeps its last picture until the new file has a frame, and
within a second of a segment's end the next segment is opened ahead, so no
clip shows the offline placeholder or raises the loading banner while its
proxy is made. The Stage banner counts only clips with nothing to show.

Stage and Program Output each have a **Proxy / Full res** media menu beside
their preview resolution, and the Asset Viewer has one in its header. Each
view's choice is its own and is kept with the window layout. **Full res**
decodes the source in that view. Stage's menu carries a cyan bar along its
foot while any of the project's editor proxies is still being made; it fills
as they are made, whichever file Stage is showing, and goes away when all
are. Program's menu fills a bar while any of its clips still plays its
source because its proxy is being made. Hovering the menu says how many of
the view's clips drew a proxy on the last frame, how many are waiting, how
much of their proxies is made and, on Stage, how much of the project's.
The Library's Status column shows each clip's editor proxy as queued,
being made (with a percentage and a bar), ready or failed, and the Asset
Viewer's Proxy menu does the same for the open clip. While a mask is selected or the
eyedropper is armed, every view shows the source and returns to its choice
afterwards.

An asset's own **Prefer** or **Require Proxy** policy still applies in every
view, as it does on the wall, so such a clip can draw a proxy under Full res;
the menu's tooltip counts those. Proxies use the cache folder and size limit
in Preferences, and carry no audio track.

**Transcode priority** in Preferences is the most CPU a proxy may use while
nothing plays; it defaults to **Fast**. A segment that starts while the show,
or the Preview playhead, is playing uses the Background budget instead, so
generation does not take frames from playback. A render node always
generates its output proxies at Background, as whole files.

A clip whose source the output nodes can only decode on the CPU, such as
ProRes, asks for a full-size 10-bit HEVC output proxy when it is switched from
**Source**. It keeps the source's size, except that an odd side is one pixel
larger, since 4:2:0 video needs even sides: a 1920x1013 plate becomes
1920x1014 with no row dropped. Each node encodes it with NVENC
(`hevc_nvenc`) and, where NVENC cannot open, with the x265 software encoder,
which is slower and uses the node's CPU at the Background budget.

Measured on an RTX 3090 Director with one 4K H.264 plate, a seek in the
editor reaches its exact frame in 74 ms on the proxy against 149 ms on the
source, four plates play every frame at 24 fps (88% on the source), and
playback holds 0.28 GB of video memory per plate instead of 1.8 GB. The
proxy takes 76 s to make for a two-minute plate at Background priority and
18 s at Fast as one file. In segments at Fast, the first ten seconds are
drawable after 2 s; all thirteen segments take 19 s and the join 1.6 s.
The editor made the project's ten proxies in 829 s at Background and 213 s
at Fast. While a proxy is made, at any priority, editor playback
of four proxies and of a 4K source loses no more frames than it does idle.
Running more than one encode at once makes Fast proxies only about 20%
sooner: decoding the 4K source on the CPU is the limit. Dragging the playhead shows the frame one display tick behind
the pointer on proxies; on four 4K sources it can show nothing until the drag
stops. Playing backwards does not yet reach exact frames on either file.

## Holding a video clip at half resolution

A video clip can be held at half resolution (**Inspector > Content >
Resolution**, authored as `media_resolution = "half"` on the clip). Each decoded
frame is reduced to half its width and height as soon as it is decoded, so
every frame held for the clip - current, read-ahead and trail - costs a quarter
of the memory, and the full-size frame is released at once.

- On the native D3D12 path the decode worker averages each 2x2 block of the
  decoder's surface into a half-size NV12/P010 surface of its own
  (`engine/shaders/video_halve.comp.hlsl`, `d3d12_video_halve.cpp`), waits
  for it, and gives the decoder its surface back. The renderer adopts the
  half-size surface as plane views exactly as it would the decoder's, so
  drawing stays zero-copy. Half-size surfaces are pooled per size.
- A frame decoded into system memory - ProRes, DNxHR, NotchLC, or hardware
  video on the SDL path - is averaged on the CPU before it is held or
  uploaded (`halve_decoded_frame`), at its own depth and subsampling.
- Both paths take the 2x2 mean on integer codes and round half up, so a frame
  halved on the GPU and on the CPU are the same bytes, and every node holding
  the clip draws the same picture. The setting is authored and is part of the
  scene digest.
- A full-size and a half-size placement of the same file are different
  pictures, so each has its own decoder, read-ahead and cache entries.
- Switching a placement's resolution is not a cut. It keeps showing the
  picture it held at the old size until the new size has decoded a frame of
  its own - a second or two for eight cold 4K streams - and the old frame is
  freed as soon as nothing shows it. A placement newly cut in inherits
  nothing.
- Decoding costs exactly the same: a compressed video frame can only be
  decoded whole. Half resolution saves memory, upload and sampling, not
  decode throughput.
- A clip shown larger than half its source size is visibly softer. Stills and
  image sequences ignore the setting.
- A picture whose half size would not be even (width or height not a multiple
  of four) keeps its full-size frames on the device path.

Measured 2026-10-26 on the RTX 3090 with four 3840x2160 H.264 plates at
24 fps (`mediaserver_render_media_video_benchmark --buffered-output
[--half-resolution]`, 240 warm-up frames then 480 measured, same binary):

| D3D12 | Full | Half |
| --- | --- | --- |
| ready ratio | 0.998, 0.998 | 0.998, 0.996 |
| peak process VRAM growth | 6069, 6188 MiB | 1980, 2018 MiB |
| decoded frames held | 172 (2.14 GB, byte-bound) | 192 (0.60 GB, count-bound) |
| submit wait, mean | 1.18-1.24 ms | 0.58-0.60 ms |

On the SDL path the same four plates read back through system memory, which
is the limit there; holding them at half resolution still relieves the
upload and the render thread:

| SDL | Full | Half |
| --- | --- | --- |
| ready ratio | 0.700 | 0.854 |
| render call, mean | 3.28 ms | 1.22 ms |
| submit wait, mean | 4.75 ms | 2.28 ms |
| peak process VRAM growth | 3091 MiB | 2417 MiB |

Halving costs about 0.3 ms of decode-worker time per frame once warm, most
of it waiting for the GPU. A clip's first ~48 frames per plate each allocate
a new half-size surface (about 2.4 ms apiece), so while read-ahead fills after
a clip starts its ready ratio runs a few points below full resolution
(0.92-0.95 against 0.98 over a cold 10 s run) before converging.

## Clips the scene has stopped authoring

A clip removed from a scene gives its memory back, which for a long time it
did not. Removing one 4K plate from a show on a 6 GB node left around 800 MiB
held until the show was restarted, because the only eviction keyed on a
change of authored media hung off the asset generation key - and that key is
zero for the whole life of any project without a published package, so for
those projects it never ran at all.

The cache now compares the authored source list against the one it last saw,
whatever that key says. A source that has gone loses its resident textures,
its pending uploads and its decoded-frame accounting here, and the decode
side is asked to let go of it: that retires its ready frames and closes its
decoder. Closing the decoder is the part that matters on a node, because an
open one holds a hardware frames pool at the clip's full resolution, and on
the device-decode path above that pool is dedicated video memory.

The request travels the ordinary worker queue rather than blocking the frame
path, and a source is recorded as released only once that queue has taken it,
so a busy frame defers the work rather than losing it. The cost of letting go
is that bringing the clip back reopens its decoder and walks to a keyframe.

## Performance qualification

Video decoder continuity is retained per clip playhead; equal source-frame
requests share immutable process-wide CPU frames. FFmpeg hardware negotiation
is attempted where available, and unsupported profiles retain software decode.
The scheduler bounds software decode concurrency and CPU frame residency.
Current-frame demand takes eviction priority over speculative read-ahead;
a speculative result can be dropped when the active frames fill the cache.
Program and Stage rendering can share GPU media residency within a recording.

Run `mediaserver_render_media_video_benchmark` from the Release GUI build with
the intended files, adding `--video PATH` for each simultaneous source and
`--cadence-fps FPS` for the intended playback cadence. Its `--buffered-output`
mode exercises the deeper runway used by output nodes. Select `--d3d12` for the
Advanced backend (the default is SDL Basic). Use the intended file profiles,
active clip count and machine; a small synthetic fixture verifies
frame identity and pixels, not production throughput.

The `mediaserver.render_media` GPU suite checks a 6000 × 4000 image, cancellation
and exact readback across multiple recordings, plus three moving UHD playheads
competing for the Advanced upload budget. `mediaserver.render_media_video`
checks native ProRes/DNxHR decoding, exact seeking, shared CPU residency,
independent playheads and nonblocking scheduler admission.
