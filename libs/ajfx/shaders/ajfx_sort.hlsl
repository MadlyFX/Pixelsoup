// The depth sort: orders an emitter's live particles back to front along a
// view axis, for alpha-blended drawing. An LSD radix sort of 32-bit keys in
// eight passes of four bits, each pass stable: a block's digit counts by an
// integer group scan, a digit-major scan over the blocks, then a scatter that
// keeps index order within each digit. No atomics and no wave operations, so
// the order depends only on the keys: particles at equal depth keep birth
// order, and every run, seek and node that sees the same positions draws them
// in the same order.

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
static const uint sort_per_thread = 16u; // 256 threads a block

uint sort_blocks(uint alive) {
    return (alive + sort_block_size - 1u) / sort_block_size;
}

uint sort_digit(uint key) {
    return (key >> sort_shift) & 15u;
}

// Sixteen digit counts, two to a word: a block holds at most 4096 keys, so
// each half fits sixteen bits and packed sums never carry.
groupshared uint4 sort_scan_low[256];
groupshared uint4 sort_scan_high[256];

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

// This thread's sixteen keys' digit counts, packed.
void sort_thread_counts(uint first, uint alive, out uint4 low, out uint4 high) {
    uint words[8] = {0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u};
    for (uint k = 0u; k < sort_per_thread; ++k) {
        uint i = first + k;
        if (i < alive) {
            uint digit = sort_digit(sort_keys_in.Load(i * 4u));
            words[digit >> 1u] += 1u << ((digit & 1u) * 16u);
        }
    }
    low = uint4(words[0], words[1], words[2], words[3]);
    high = uint4(words[4], words[5], words[6], words[7]);
}

// One group per block: each live particle's key, farthest first, and its
// index as the value.
[numthreads(256, 1, 1)]
void sort_keys(uint3 group : SV_GroupID, uint thread : SV_GroupIndex) {
    uint alive = sort_counters.Load(0u);
    uint first = group.x * sort_block_size + thread * sort_per_thread;
    for (uint k = 0u; k < sort_per_thread; ++k) {
        uint i = first + k;
        if (i < alive) {
            float3 p = asfloat(sort_particles.Load3(i * sort_stride + sort_position));
            float depth = p.x * sort_axis.x + p.y * sort_axis.y + p.z * sort_axis.z;
            // Float order as unsigned order, then reversed so the farthest
            // particle has the smallest key.
            uint bits = asuint(depth);
            uint ordered = (bits & 0x80000000u) != 0u ? ~bits : (bits | 0x80000000u);
            sort_keys_out.Store(i * 4u, ~ordered);
            sort_values_out.Store(i * 4u, i);
        }
    }
}

// One group per block: the block's count of each digit, digit-major so the
// scan over them visits every block's zeros, then every block's ones, ...
[numthreads(256, 1, 1)]
void sort_count(uint3 group : SV_GroupID, uint thread : SV_GroupIndex) {
    uint alive = sort_counters.Load(0u);
    uint blocks = sort_blocks(alive);
    uint4 low;
    uint4 high;
    sort_thread_counts(group.x * sort_block_size + thread * sort_per_thread, alive, low, high);
    sort_group_scan(low, high, thread);
    if (thread == 255u) {
        for (uint digit = 0u; digit < 16u; ++digit) {
            sort_digits.Store((digit * blocks + group.x) * 4u, sort_unpack(low, high, digit));
        }
    }
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

// One group per block: each key to its place, in index order within its digit.
[numthreads(256, 1, 1)]
void sort_scatter(uint3 group : SV_GroupID, uint thread : SV_GroupIndex) {
    uint alive = sort_counters.Load(0u);
    uint blocks = sort_blocks(alive);
    uint first = group.x * sort_block_size + thread * sort_per_thread;
    uint4 own_low;
    uint4 own_high;
    sort_thread_counts(first, alive, own_low, own_high);
    uint4 low = own_low;
    uint4 high = own_high;
    sort_group_scan(low, high, thread);
    uint base = 16u * sort_max_blocks;
    uint next[16];
    for (uint digit = 0u; digit < 16u; ++digit) {
        uint before = sort_unpack(low, high, digit) - sort_unpack(own_low, own_high, digit);
        next[digit] = sort_digits.Load((base + digit * blocks + group.x) * 4u) + before;
    }
    for (uint k = 0u; k < sort_per_thread; ++k) {
        uint i = first + k;
        if (i < alive) {
            uint key = sort_keys_in.Load(i * 4u);
            uint digit = sort_digit(key);
            uint slot = next[digit];
            next[digit] = slot + 1u;
            sort_keys_out.Store(slot * 4u, key);
            sort_values_out.Store(slot * 4u, sort_values_in.Load(i * 4u));
        }
    }
}
