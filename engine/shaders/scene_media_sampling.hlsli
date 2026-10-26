// Filtered sampling for a clip shown smaller than its source.
//
// A decoded frame is sampled in place - often the decoder's own surface - and
// carries one mip level, so a hardware bilinear fetch at a minified pixel reads
// one 2x2 neighbourhood and skips everything between. A 3840-wide plate drawn
// 100 pixels wide would show one texel in 38 and shimmer as it moves.
//
// Instead each output pixel averages a grid of bilinear taps laid across its
// footprint in the source: the parallelogram spanned by the screen-space
// derivatives of its texture coordinate. A bilinear fetch at a texel corner is
// the exact mean of the 2x2 texels around it, so ceil(extent / 2) taps along an
// axis cover the footprint without gaps. Each axis is counted on its own, which
// keeps a plate seen at a grazing angle sharp across while filtering it along
// its length.
//
// Along a texture axis minified more than twice, every tap is moved to its
// nearest texel corner. Between corners a fetch is a tent rather than a box,
// and taps spaced two texels apart then all meet one-texel detail at the same
// phase: a fine checker comes out as a uniform black, white or grey depending
// on where the plate sits, which is the moire this filter exists to remove.
// The move is at most half a texel, a small fraction of the pixel's footprint.
//
// A pixel that is not minified takes one tap at its own coordinate, which is
// exactly the fetch made before, so a clip shown at or above source size is
// unchanged. Work is bounded by the source and not the screen: a pixel
// covering k x k texels takes about (k / 2)^2 taps, and a clip covering n
// output pixels covers about n k^2 texels, so one view of one plane costs about
// a quarter of a fetch per source texel. The tap cap only stops a pixel
// at the extreme of a perspective plate taking more than its share.
//
// Taps average the encoded values the plane stores, as a video scaler does.
// The YCbCr-to-RGB matrix is linear, so averaging codes before conversion is
// the same as averaging the converted colour.

static const uint media_footprint_max_taps = 16u;

struct MediaFootprint {
    float2 centre;
    // The texture-coordinate step of one output pixel along screen x and y.
    float2 axis_x;
    float2 axis_y;
};

// Must be called in uniform control flow: it takes the derivatives of `uv`.
MediaFootprint media_footprint(float2 uv) {
    MediaFootprint footprint;
    footprint.centre = uv;
    footprint.axis_x = ddx(uv);
    footprint.axis_y = ddy(uv);
    return footprint;
}

uint media_footprint_taps(float texels) {
    return clamp((uint)ceil(texels * 0.5), 1u, media_footprint_max_taps);
}

// How one plane is sampled across a footprint.
struct MediaTapGrid {
    uint2 taps;
    float2 extent;
    // Per texture axis: whether taps move to texel corners.
    bool2 snap;
};

MediaTapGrid media_tap_grid(MediaFootprint footprint, float2 extent) {
    MediaTapGrid grid;
    grid.taps = uint2(
        media_footprint_taps(length(footprint.axis_x * extent)),
        media_footprint_taps(length(footprint.axis_y * extent)));
    grid.extent = extent;
    grid.snap =
        (abs(footprint.axis_x) + abs(footprint.axis_y)) * extent > 2.0;
    return grid;
}

float2 media_grid_tap(MediaFootprint footprint, MediaTapGrid grid, uint2 tap) {
    const float2 offset = (float2(tap) + 0.5) / float2(grid.taps) - 0.5;
    const float2 uv = footprint.centre
        + footprint.axis_x * offset.x
        + footprint.axis_y * offset.y;
    const float2 corner = round(uv * grid.extent) / grid.extent;
    return float2(
        grid.snap.x ? corner.x : uv.x,
        grid.snap.y ? corner.y : uv.y);
}

float4 media_sample_rgba(
    Texture2D<float4> plane, SamplerState plane_sampler,
    MediaFootprint footprint) {
    uint width;
    uint height;
    plane.GetDimensions(width, height);
    const MediaTapGrid grid =
        media_tap_grid(footprint, float2(width, height));
    float4 sum = 0.0;
    for (uint y = 0u; y < grid.taps.y; ++y) {
        for (uint x = 0u; x < grid.taps.x; ++x) {
            sum += plane.SampleLevel(
                plane_sampler, media_grid_tap(footprint, grid, uint2(x, y)),
                0.0);
        }
    }
    return sum / (float)(grid.taps.x * grid.taps.y);
}

// The chroma plane has its own extent - half the luma's for 4:2:0 - and so
// its own tap count.
float2 media_sample_chroma(
    Texture2D<float2> plane, SamplerState plane_sampler,
    MediaFootprint footprint) {
    uint width;
    uint height;
    plane.GetDimensions(width, height);
    const MediaTapGrid grid =
        media_tap_grid(footprint, float2(width, height));
    float2 sum = 0.0;
    for (uint y = 0u; y < grid.taps.y; ++y) {
        for (uint x = 0u; x < grid.taps.x; ++x) {
            sum += plane.SampleLevel(
                plane_sampler, media_grid_tap(footprint, grid, uint2(x, y)),
                0.0);
        }
    }
    return sum / (float)(grid.taps.x * grid.taps.y);
}
