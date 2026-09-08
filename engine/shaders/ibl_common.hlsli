// Shared geometry for the image-based-lighting prefilter passes.
//
// Every function here has to produce the same bits on every node in a cohort.
// That rules out anything seeded by time, frame, adapter or resolution, and it
// is why the sample sequences below are fixed tables rather than anything
// generated. A "random rotation per frame to hide banding" is exactly the kind
// of thing that looks better on one machine and seams across two.

#ifndef MEDIASERVER_IBL_COMMON_HLSLI
#define MEDIASERVER_IBL_COMMON_HLSLI

static const float kPi = 3.14159265358979323846F;

// The six cube faces in D3D order: +X, -X, +Y, -Y, +Z, -Z.
//
// `uv` is in [0,1] across the face with the origin at its top-left corner,
// matching how a texel index maps to a face.
float3 ibl_face_direction(uint face, float2 uv) {
    // To [-1, 1], with v flipped because texel rows run downward.
    const float2 c = float2(uv.x * 2.0F - 1.0F, 1.0F - uv.y * 2.0F);
    float3 direction;
    switch (face) {
    case 0U:
        direction = float3(1.0F, c.y, -c.x);
        break;
    case 1U:
        direction = float3(-1.0F, c.y, c.x);
        break;
    case 2U:
        direction = float3(c.x, 1.0F, -c.y);
        break;
    case 3U:
        direction = float3(c.x, -1.0F, c.y);
        break;
    case 4U:
        direction = float3(c.x, c.y, 1.0F);
        break;
    default:
        direction = float3(-c.x, c.y, -1.0F);
        break;
    }
    return normalize(direction);
}

// Equirectangular lookup. `rotation` is in turns about the world up axis, so an
// operator can spin the environment without re-exporting the map.
float2 ibl_equirect_uv(float3 direction, float rotation) {
    const float angle = rotation * 2.0F * kPi;
    const float sine = sin(angle);
    const float cosine = cos(angle);
    const float3 turned = float3(
        direction.x * cosine - direction.z * sine,
        direction.y,
        direction.x * sine + direction.z * cosine);
    const float u = atan2(turned.z, turned.x) / (2.0F * kPi) + 0.5F;
    const float v = acos(clamp(turned.y, -1.0F, 1.0F)) / kPi;
    return float2(u, v);
}

// Solid angle subtended by one texel of a cube face, for an exact convolution.
//
// Without it a naive sum weights the face corners - which cover less of the
// sphere - as heavily as the centres, and the result is wrong by several
// percent in a way that looks like a slightly tinted environment rather than
// like a bug.
float ibl_texel_solid_angle(float2 uv, uint face_size) {
    const float step = 2.0F / float(face_size);
    const float x = uv.x * 2.0F - 1.0F;
    const float y = 1.0F - uv.y * 2.0F;
    // The exact spherical-rectangle area of the texel's footprint.
    const float x0 = x - step * 0.5F;
    const float x1 = x + step * 0.5F;
    const float y0 = y - step * 0.5F;
    const float y1 = y + step * 0.5F;
    const float area =
        atan2(x1 * y1, sqrt(x1 * x1 + y1 * y1 + 1.0F))
        - atan2(x0 * y1, sqrt(x0 * x0 + y1 * y1 + 1.0F))
        - atan2(x1 * y0, sqrt(x1 * x1 + y0 * y0 + 1.0F))
        + atan2(x0 * y0, sqrt(x0 * x0 + y0 * y0 + 1.0F));
    return area;
}

// The nine real spherical harmonic basis functions up to band two.
void ibl_sh9(float3 n, out float basis[9]) {
    basis[0] = 0.282095F;
    basis[1] = 0.488603F * n.y;
    basis[2] = 0.488603F * n.z;
    basis[3] = 0.488603F * n.x;
    basis[4] = 1.092548F * n.x * n.y;
    basis[5] = 1.092548F * n.y * n.z;
    basis[6] = 0.315392F * (3.0F * n.z * n.z - 1.0F);
    basis[7] = 1.092548F * n.x * n.z;
    basis[8] = 0.546274F * (n.x * n.x - n.y * n.y);
}

// Van der Corput radical inverse, base two. The low-discrepancy half of a
// Hammersley pair.
float ibl_radical_inverse(uint bits) {
    bits = (bits << 16U) | (bits >> 16U);
    bits = ((bits & 0x55555555U) << 1U) | ((bits & 0xAAAAAAAAU) >> 1U);
    bits = ((bits & 0x33333333U) << 2U) | ((bits & 0xCCCCCCCCU) >> 2U);
    bits = ((bits & 0x0F0F0F0FU) << 4U) | ((bits & 0xF0F0F0F0U) >> 4U);
    bits = ((bits & 0x00FF00FFU) << 8U) | ((bits & 0xFF00FF00U) >> 8U);
    return float(bits) * 2.3283064365386963e-10F;
}

float2 ibl_hammersley(uint index, uint count) {
    return float2(float(index) / float(count), ibl_radical_inverse(index));
}

// GGX importance sample, in the hemisphere around `n`.
float3 ibl_importance_sample_ggx(float2 sample_point, float3 n,
                                 float roughness) {
    const float a = roughness * roughness;
    const float phi = 2.0F * kPi * sample_point.x;
    const float cos_theta = sqrt(
        (1.0F - sample_point.y)
        / (1.0F + (a * a - 1.0F) * sample_point.y));
    const float sin_theta = sqrt(1.0F - cos_theta * cos_theta);
    const float3 h = float3(
        cos(phi) * sin_theta, sin(phi) * sin_theta, cos_theta);

    // A deterministic tangent basis. The branch is on the normal alone, so two
    // nodes evaluating the same normal pick the same basis.
    const float3 up =
        abs(n.z) < 0.999F ? float3(0.0F, 0.0F, 1.0F) : float3(1.0F, 0.0F, 0.0F);
    const float3 tangent = normalize(cross(up, n));
    const float3 bitangent = cross(n, tangent);
    return normalize(tangent * h.x + bitangent * h.y + n * h.z);
}

#endif
