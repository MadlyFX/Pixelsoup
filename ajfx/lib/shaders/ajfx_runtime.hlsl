// The kernels every effect shares: a stable two-level prefix sum over the
// survivor flags, the scatter that compacts survivors in birth order, and the
// one-thread bookkeeping at the end of a step. Integer-only, so their results
// do not depend on scheduling.

#include "ajfx_particles.hlsli"

groupshared uint ajfx_scan[1024];

// Inclusive scan of one value per thread across a 1024-thread group.
uint ajfx_group_scan(uint value, uint thread) {
    ajfx_scan[thread] = value;
    GroupMemoryBarrierWithGroupSync();
    for (uint offset = 1u; offset < 1024u; offset <<= 1u) {
        uint add = thread >= offset ? ajfx_scan[thread - offset] : 0u;
        GroupMemoryBarrierWithGroupSync();
        ajfx_scan[thread] += add;
        GroupMemoryBarrierWithGroupSync();
    }
    return ajfx_scan[thread];
}

// One group per 4096 particles: each survivor's index among its block's
// survivors, and each block's total.
[numthreads(1024, 1, 1)]
void scan_blocks(uint3 group : SV_GroupID, uint thread : SV_GroupIndex) {
    uint alive = ajfx_counters.Load(0u);
    uint first = group.x * ajfx_block_size + thread * 4u;
    uint flags[4];
    uint total = 0u;
    for (uint k = 0u; k < 4u; ++k) {
        uint i = first + k;
        flags[k] = i < alive ? ajfx_flags.Load(i * 4u) : 0u;
        total += flags[k];
    }
    uint running = ajfx_group_scan(total, thread) - total;
    for (uint k = 0u; k < 4u; ++k) {
        uint i = first + k;
        if (i < alive) {
            ajfx_offsets.Store(i * 4u, running);
        }
        running += flags[k];
    }
    if (thread == 1023u) {
        ajfx_blocks.Store(group.x * 4u, ajfx_scan[1023u]);
    }
}

// One group: each block's offset among all survivors, and the survivor count.
[numthreads(1024, 1, 1)]
void scan_totals(uint thread : SV_GroupIndex) {
    uint alive = ajfx_counters.Load(0u);
    uint blocks = (alive + ajfx_block_size - 1u) / ajfx_block_size;
    uint totals[4];
    uint total = 0u;
    for (uint k = 0u; k < 4u; ++k) {
        uint b = thread * 4u + k;
        totals[k] = b < blocks ? ajfx_blocks.Load(b * 4u) : 0u;
        total += totals[k];
    }
    uint running = ajfx_group_scan(total, thread) - total;
    for (uint k = 0u; k < 4u; ++k) {
        uint b = thread * 4u + k;
        if (b < blocks) {
            ajfx_blocks.Store((ajfx_block_size + b) * 4u, running);
        }
        running += totals[k];
    }
    if (thread == 1023u) {
        ajfx_counters.Store(4u, ajfx_scan[1023u]);
    }
}

// Copies each survivor to its compacted slot.
[numthreads(256, 1, 1)]
void scatter(uint3 group : SV_GroupID, uint thread : SV_GroupIndex) {
    uint i = ajfx_index(group, thread);
    if (i >= ajfx_counters.Load(0u) || ajfx_flags.Load(i * 4u) == 0u) {
        return;
    }
    uint slot = ajfx_offsets.Load(i * 4u) + ajfx_blocks.Load((ajfx_block_size + i / ajfx_block_size) * 4u);
    uint from = i * ajfx_stride;
    uint to = slot * ajfx_stride;
    for (uint b = 0u; b < ajfx_stride; b += 4u) {
        ajfx_dst.Store(to + b, ajfx_src.Load(from + b));
    }
}

// The live count after births, refusals past capacity, the next step's
// dispatch sizes ([0] particles in groups of 256, [12] blocks of 4096) and
// the sprite draw ([24] six vertices per live particle).
[numthreads(1, 1, 1)]
void finalize() {
    uint compacted = ajfx_counters.Load(4u);
    uint room = ajfx_capacity - compacted;
    uint births = ajfx_births();
    uint accepted = min(births, room);
    uint alive = compacted + accepted;
    ajfx_counters.Store(0u, alive);
    ajfx_counters.Store(8u, ajfx_counters.Load(8u) + (births - accepted));
    ajfx_write_groups(0u, alive, ajfx_group_size);
    ajfx_write_groups(12u, alive, ajfx_block_size);
    ajfx_args.Store4(24u, uint4(6u, alive, 0u, 0u));
}
