#ifndef MEDIASERVER_OCTAHEDRAL_HLSLI
#define MEDIASERVER_OCTAHEDRAL_HLSLI

// Octahedral encoding for unit vectors.
//
// Two channels instead of three, with an angular error small enough at RG16F
// to be invisible in an occlusion term. The alternative - storing xyz in
// RGBA16F - costs a third more bandwidth on a target that every pixel of every
// frame writes, for precision nothing here consumes.
//
// Shared rather than copied because the encode and the decode have to be exact
// inverses. Two copies that drift by a sign in one hemisphere produce occlusion
// that is correct on one side of an object and wrong on the other, which reads
// as a lighting bug rather than as an encoding one.

float2 octahedral_encode(float3 direction) {
    const float3 n = direction
        / max(abs(direction.x) + abs(direction.y) + abs(direction.z), 1.0e-8);
    if (n.z >= 0.0) {
        return n.xy;
    }
    // The lower hemisphere folds outward across the octahedron's edges.
    return (1.0 - abs(n.yx))
        * float2(n.x >= 0.0 ? 1.0 : -1.0, n.y >= 0.0 ? 1.0 : -1.0);
}

float3 octahedral_decode(float2 encoded) {
    float3 n = float3(encoded.x, encoded.y,
                      1.0 - abs(encoded.x) - abs(encoded.y));
    const float fold = saturate(-n.z);
    n.x += n.x >= 0.0 ? -fold : fold;
    n.y += n.y >= 0.0 ? -fold : fold;
    return normalize(n);
}

#endif
