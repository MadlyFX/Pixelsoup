// The depth sort: orders an emitter's live particles back to front along a
// view axis, for alpha-blended drawing. An LSD radix sort of the top 24 bits
// of a 32-bit depth key in six passes of four bits, each pass stable: a
// block's digit counts, a digit-major scan over the blocks, then a scatter
// that keeps index order within each digit. 24 bits resolve depth to one part
// in 32768 of its distance; particles closer together than that keep birth
// order.
//
// The order depends only on the keys. Ranks within a block come from an
// integer group scan; the only atomics are additions into a block's digit
// histogram, whose totals do not depend on the order threads add them. So
// particles at equal depth keep birth order, and every run, seek and node that
// sees the same positions draws them in the same order.
//
// Memory access is coalesced: thread t of a block touches elements t, t + 256,
// t + 512, ... Each scatter reorders its block in group-shared memory first,
// so a digit's run leaves as one contiguous write.

cbuffer AjfxSort : register(b0, space1) {
    uint sort_stride;     // bytes per particle record
    uint sort_position;   // byte offset of the position in a record
    uint sort_shift;      // the low bit of this pass's digit
    uint sort_max_blocks; // blocks the capacity spans; where the offsets start in sort_digits
    float4 sort_axis;     // xyz: depth grows along this axis, in the particles' own space
    uint4 sort_reserved0;
    uint4 sort_reserved1;
};

RWByteAddressBuffer sort_particles : register(u0);
RWByteAddressBuffer sort_counters : register(u1);   // the live count first
RWByteAddressBuffer sort_keys_in : register(u2);
RWByteAddressBuffer sort_values_in : register(u3);
RWByteAddressBuffer sort_keys_out : register(u4);
RWByteAddressBuffer sort_values_out : register(u5);
RWByteAddressBuffer sort_digits : register(u6);     // per-block digit counts, then their offsets

static const uint sort_block_size = 4096u;
static const uint sort_threads = 256u;
static const uint sort_per_thread = 16u;

uint sort_blocks(uint alive) {
    return (alive + sort_block_size - 1u) / sort_block_size;
}

uint sort_digit(uint key) {
    return (key >> sort_shift) & 15u;
}

groupshared uint sort_histogram[16];

// A block's digit counts, digit-major so the scan over them visits every
// block's zeros, then every block's ones, ...
void sort_write_histogram(uint group, uint thread, uint blocks) {
    GroupMemoryBarrierWithGroupSync();
    if (thread < 16u) {
        sort_digits.Store((thread * blocks + group) * 4u, sort_histogram[thread]);
    }
}

// One group per block: each live particle's key, farthest first, and its
// index as the value; and the block's counts of the first pass's digit.
[numthreads(256, 1, 1)]
void sort_keys(uint3 group : SV_GroupID, uint thread : SV_GroupIndex) {
    uint alive = sort_counters.Load(0u);
    if (thread < 16u) {
        sort_histogram[thread] = 0u;
    }
    GroupMemoryBarrierWithGroupSync();
    for (uint k = 0u; k < sort_per_thread; ++k) {
        uint i = group.x * sort_block_size + k * sort_threads + thread;
        if (i < alive) {
            float3 p = asfloat(sort_particles.Load3(i * sort_stride + sort_position));
            float depth = p.x * sort_axis.x + p.y * sort_axis.y + p.z * sort_axis.z;
            // Float order as unsigned order, then reversed so the farthest
            // particle has the smallest key.
            uint bits = asuint(depth);
            uint ordered = (bits & 0x80000000u) != 0u ? ~bits : (bits | 0x80000000u);
            sort_keys_out.Store(i * 4u, ~ordered);
            sort_values_out.Store(i * 4u, i);
            InterlockedAdd(sort_histogram[sort_digit(~ordered)], 1u);
        }
    }
    sort_write_histogram(group.x, thread, sort_blocks(alive));
}

// One group per block: the block's counts of this pass's digit.
[numthreads(256, 1, 1)]
void sort_count(uint3 group : SV_GroupID, uint thread : SV_GroupIndex) {
    uint alive = sort_counters.Load(0u);
    if (thread < 16u) {
        sort_histogram[thread] = 0u;
    }
    GroupMemoryBarrierWithGroupSync();
    for (uint k = 0u; k < sort_per_thread; ++k) {
        uint i = group.x * sort_block_size + k * sort_threads + thread;
        if (i < alive) {
            InterlockedAdd(sort_histogram[sort_digit(sort_keys_in.Load(i * 4u))], 1u);
        }
    }
    sort_write_histogram(group.x, thread, sort_blocks(alive));
}

groupshared uint sort_totals[1024];

// One group: where each block's run of each digit starts in the output.
// 16 digits by at most 4096 blocks is at most 64 counts a thread.
[numthreads(1024, 1, 1)]
void sort_scan(uint thread : SV_GroupIndex) {
    uint count = 16u * sort_blocks(sort_counters.Load(0u));
    uint first = thread * 64u;
    uint total = 0u;
    for (uint k = 0u; k < 64u; ++k) {
        uint at = first + k;
        total += at < count ? sort_digits.Load(at * 4u) : 0u;
    }
    sort_totals[thread] = total;
    GroupMemoryBarrierWithGroupSync();
    for (uint offset = 1u; offset < 1024u; offset <<= 1u) {
        uint add = thread >= offset ? sort_totals[thread - offset] : 0u;
        GroupMemoryBarrierWithGroupSync();
        sort_totals[thread] += add;
        GroupMemoryBarrierWithGroupSync();
    }
    uint running = sort_totals[thread] - total;
    uint base = 16u * sort_max_blocks;
    for (uint k = 0u; k < 64u; ++k) {
        uint at = first + k;
        if (at < count) {
            uint here = sort_digits.Load(at * 4u);
            sort_digits.Store((base + at) * 4u, running);
            running += here;
        }
    }
}

// The scatter's working set: the block itself (keys, then values), and the
// sixteen digit counts of each thread's run of keys, two to a word - a block
// holds at most 4096 keys, so each half fits sixteen bits and packed sums
// never carry.
groupshared uint sort_stage[4096];
groupshared uint4 sort_scan_low[256];
groupshared uint4 sort_scan_high[256];
groupshared uint sort_run_start[16];  // where each digit's run starts in the block
groupshared uint sort_run_target[16]; // and in the output

// Inclusive scan of every packed count across the 256-thread group.
void sort_group_scan(inout uint4 low, inout uint4 high, uint thread) {
    sort_scan_low[thread] = low;
    sort_scan_high[thread] = high;
    GroupMemoryBarrierWithGroupSync();
    for (uint offset = 1u; offset < 256u; offset <<= 1u) {
        uint4 add_low = thread >= offset ? sort_scan_low[thread - offset] : uint4(0u, 0u, 0u, 0u);
        uint4 add_high = thread >= offset ? sort_scan_high[thread - offset] : uint4(0u, 0u, 0u, 0u);
        GroupMemoryBarrierWithGroupSync();
        sort_scan_low[thread] += add_low;
        sort_scan_high[thread] += add_high;
        GroupMemoryBarrierWithGroupSync();
    }
    low = sort_scan_low[thread];
    high = sort_scan_high[thread];
}

uint sort_unpack(uint4 low, uint4 high, uint digit) {
    uint word = digit >> 1u;
    uint packed = word < 4u ? low[word] : high[word - 4u];
    return (packed >> ((digit & 1u) * 16u)) & 0xffffu;
}

// One group per block: the block reordered by digit in group-shared memory,
// stably, then each digit's run written where the scan put it.
[numthreads(256, 1, 1)]
void sort_scatter(uint3 group : SV_GroupID, uint thread : SV_GroupIndex) {
    uint alive = sort_counters.Load(0u);
    uint blocks = sort_blocks(alive);
    uint first = group.x * sort_block_size;
    uint count = min(sort_block_size, alive - first);

    // The keys, loaded coalesced; then each thread takes a run of sixteen
    // consecutive ones, so thread order then run order is index order.
    for (uint k = 0u; k < sort_per_thread; ++k) {
        uint j = k * sort_threads + thread;
        if (j < count) {
            sort_stage[j] = sort_keys_in.Load((first + j) * 4u);
        }
    }
    GroupMemoryBarrierWithGroupSync();
    uint keys[16];
    uint words[8] = {0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u};
    for (uint k = 0u; k < sort_per_thread; ++k) {
        uint j = thread * sort_per_thread + k;
        keys[k] = j < count ? sort_stage[j] : 0u;
        if (j < count) {
            uint digit = sort_digit(keys[k]);
            words[digit >> 1u] += 1u << ((digit & 1u) * 16u);
        }
    }
    uint4 own_low = uint4(words[0], words[1], words[2], words[3]);
    uint4 own_high = uint4(words[4], words[5], words[6], words[7]);
    uint4 low = own_low;
    uint4 high = own_high;
    sort_group_scan(low, high, thread);
    if (thread == 0u) {
        // The last thread's inclusive counts are the block's totals.
        uint start = 0u;
        for (uint digit = 0u; digit < 16u; ++digit) {
            sort_run_start[digit] = start;
            start += sort_unpack(sort_scan_low[255], sort_scan_high[255], digit);
            sort_run_target[digit] =
                sort_digits.Load((16u * sort_max_blocks + digit * blocks + group.x) * 4u);
        }
    }
    GroupMemoryBarrierWithGroupSync();

    // Each key's place within the reordered block.
    uint ranks[16];
    uint next[16];
    for (uint digit = 0u; digit < 16u; ++digit) {
        next[digit] = sort_run_start[digit] + sort_unpack(low, high, digit) - sort_unpack(own_low, own_high, digit);
    }
    for (uint k = 0u; k < sort_per_thread; ++k) {
        uint j = thread * sort_per_thread + k;
        ranks[k] = 0u;
        if (j < count) {
            uint digit = sort_digit(keys[k]);
            ranks[k] = next[digit];
            next[digit] += 1u;
        }
    }

    // Keys: reorder, then write out coalesced, remembering where each slot
    // went for the values.
    GroupMemoryBarrierWithGroupSync();
    for (uint k = 0u; k < sort_per_thread; ++k) {
        if (thread * sort_per_thread + k < count) {
            sort_stage[ranks[k]] = keys[k];
        }
    }
    GroupMemoryBarrierWithGroupSync();
    uint targets[16];
    for (uint k = 0u; k < sort_per_thread; ++k) {
        uint j = k * sort_threads + thread;
        targets[k] = 0u;
        if (j < count) {
            uint key = sort_stage[j];
            uint digit = sort_digit(key);
            targets[k] = sort_run_target[digit] + j - sort_run_start[digit];
            sort_keys_out.Store(targets[k] * 4u, key);
        }
    }

    // Values: the same moves through the same space.
    GroupMemoryBarrierWithGroupSync();
    for (uint k = 0u; k < sort_per_thread; ++k) {
        uint j = k * sort_threads + thread;
        if (j < count) {
            sort_stage[j] = sort_values_in.Load((first + j) * 4u);
        }
    }
    GroupMemoryBarrierWithGroupSync();
    uint values[16];
    for (uint k = 0u; k < sort_per_thread; ++k) {
        uint j = thread * sort_per_thread + k;
        values[k] = j < count ? sort_stage[j] : 0u;
    }
    GroupMemoryBarrierWithGroupSync();
    for (uint k = 0u; k < sort_per_thread; ++k) {
        if (thread * sort_per_thread + k < count) {
            sort_stage[ranks[k]] = values[k];
        }
    }
    GroupMemoryBarrierWithGroupSync();
    for (uint k = 0u; k < sort_per_thread; ++k) {
        uint j = k * sort_threads + thread;
        if (j < count) {
            sort_values_out.Store(targets[k] * 4u, sort_stage[j]);
        }
    }
}
