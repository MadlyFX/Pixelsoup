// Adds a constant to each of `count` values in place.
//
// The companion to buffer_scale_probe.comp.hlsl: dispatched indirectly with
// the group count that shader wrote, and reading what it wrote, so a missing
// UAV barrier or a dispatch that ignored its arguments shows as wrong values.

cbuffer AddParameters : register(b0, space1) {
    uint count;
    uint add;
    uint2 add_padding;
};

RWByteAddressBuffer values : register(u0);

[numthreads(64, 1, 1)]
void main(uint3 thread : SV_DispatchThreadID) {
    if (thread.x >= count) {
        return;
    }
    values.Store(thread.x * 4U, values.Load(thread.x * 4U) + add);
}
