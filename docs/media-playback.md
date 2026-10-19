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
resolution or profile. The current generic software video conversion produces
RGBA8, including high-bit-depth ProRes and DNxHR input. It does not preserve
their full source precision through that conversion. Hardware-decoded Rec.709
P010 has a separate two-plane 10-bit path on supported Advanced configurations.
Hardware-decoded limited-range Rec.709 NV12 uses that same path: its 8-bit
codes are promoted exactly on the decode worker, avoiding CPU colour
conversion and reducing frame storage/upload from four to three bytes per
pixel. Full-range, untagged and other colour spaces retain the generic path.
HDR transfer-function handling and additional high-bit-depth video layouts
need explicit qualification before using them for precision-critical delivery.

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
