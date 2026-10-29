// Reads one buffer, writes another, and writes the indirect arguments the
// work after it runs with.
//
// A test probe for writable buffers and indirect arguments: every output is
// predictable from the inputs, so a test compares exact values. Each thread
// writes only its own element; thread 0 alone writes the arguments.

cbuffer ScaleParameters : register(b0, space1) {
    uint count;
    uint factor;
    uint2 scale_padding;
};

ByteAddressBuffer source : register(t0);       // a read-only storage buffer
RWByteAddressBuffer target : register(u0);     // writable buffer 0
RWByteAddressBuffer arguments : register(u1);  // writable buffer 1

[numthreads(64, 1, 1)]
void main(uint3 thread : SV_DispatchThreadID) {
    const uint i = thread.x;
    if (i == 0U) {
        // [0] a dispatch covering `count` threads in groups of 64.
        arguments.Store3(0U, uint3((count + 63U) / 64U, 1U, 1U));
        // [16] a draw of six vertices for each of `count` instances.
        arguments.Store4(16U, uint4(6U, count, 0U, 0U));
    }
    if (i >= count) {
        return;
    }
    target.Store(i * 4U, source.Load(i * 4U) * factor + i);
}
