#include <ajfx/gpu/sort.hpp>

#include <bit>
#include <optional>
#include <span>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

namespace ajfx::gpu {
namespace {

constexpr std::uint32_t block_size = 4096U;
constexpr std::uint32_t digit_count = 16U;
// Four bits each, over the key's top 24 bits.
constexpr std::uint32_t passes = 6U;
constexpr std::uint32_t first_bit = 8U;

Error device_error(const std::string& what, const Error& cause) {
    return Error{cause.code, what + ": " + cause.message};
}

} // namespace

bool sorts(const Emitter& emitter) noexcept {
    return emitter.renderer.sort && emitter.renderer.blend != BlendMode::Additive &&
           emitter.renderer.geometry != Geometry::Ribbon;
}

bool interleaved(const Effect& effect, std::size_t index) noexcept {
    const auto candidate = [](const Emitter& emitter) {
        return emitter.renderer.geometry == Geometry::Sprites &&
               (sorts(emitter) || emitter.renderer.blend == BlendMode::Additive);
    };
    if (index >= effect.emitters.size() || !candidate(effect.emitters[index])) {
        return false;
    }
    std::size_t candidates = 0U;
    bool sorted = false;
    for (const Emitter& emitter : effect.emitters) {
        if (candidate(emitter)) {
            ++candidates;
            sorted = sorted || sorts(emitter);
        }
    }
    return candidates >= 2U && sorted;
}

struct ParticleSorter::State final {
    Device* device{nullptr};
    Pipeline keys;
    Pipeline count;
    Pipeline scan;
    Pipeline scatter;
    Pipeline range;
    Pipeline slices;
    // The interleaved emitters' shared span of keys; invalid when none
    // interleave.
    Buffer span;
    struct Scratch final {
        // Keys and values ping-pong between the pairs; six passes end in
        // the first.
        Buffer keys[2];
        Buffer values[2];
        Buffer digits;
        std::uint32_t max_blocks{0U};
        std::uint32_t capacity{0U};
        // An interleaved emitter's slice draw arguments, sixteen bytes a
        // slice; invalid otherwise.
        Buffer slice_args;
    };
    // Empty for an emitter that does not sort.
    std::vector<std::optional<Scratch>> emitters;

    ~State() {
        if (device == nullptr) {
            return;
        }
        for (const Pipeline pipeline : {keys, count, scan, scatter, range, slices}) {
            device->destroy_pipeline(pipeline);
        }
        device->destroy_buffer(span);
        for (const std::optional<Scratch>& scratch : emitters) {
            if (scratch) {
                for (const Buffer buffer : {scratch->keys[0], scratch->keys[1], scratch->values[0], scratch->values[1],
                                            scratch->digits, scratch->slice_args}) {
                    device->destroy_buffer(buffer);
                }
            }
        }
    }
};

ParticleSorter::ParticleSorter(std::unique_ptr<State> state) : state_{std::move(state)} {}
ParticleSorter::ParticleSorter(ParticleSorter&&) noexcept = default;
ParticleSorter& ParticleSorter::operator=(ParticleSorter&&) noexcept = default;
ParticleSorter::~ParticleSorter() = default;

Result<ParticleSorter> ParticleSorter::create(Device& device, const Effect& effect, const KernelSet& kernels) {
    if (kernels.format != device.shader_format()) {
        return Error{ErrorCode::GpuUnavailable, "the kernel set was not compiled for this device"};
    }
    auto state = std::make_unique<State>();
    state->device = &device;
    state->emitters.resize(effect.emitters.size());
    bool any = false;
    bool any_interleaved = false;
    for (std::size_t i = 0U; i < effect.emitters.size(); ++i) {
        const Emitter& emitter = effect.emitters[i];
        const bool slices = interleaved(effect, i);
        if (!sorts(emitter) && !slices) {
            continue;
        }
        any = true;
        any_interleaved = any_interleaved || slices;
        State::Scratch& scratch = state->emitters[i].emplace();
        const std::uint64_t capacity = emitter.capacity;
        scratch.capacity = emitter.capacity;
        scratch.max_blocks = static_cast<std::uint32_t>((capacity + block_size - 1U) / block_size);
        const std::string name = "ajfx sort " + effect.name + "/" + emitter.name;
        // Counts, then their offsets.
        const std::uint64_t digit_bytes = 2U * digit_count * std::uint64_t{scratch.max_blocks} * 4U;
        // The order, then where each slice starts in it.
        const std::uint64_t order_bytes = (capacity + (slices ? depth_slices : 0U)) * 4U;
        const std::uint64_t slice_bytes = slices ? std::uint64_t{depth_slices} * 16U : 0U;
        for (const auto& [bytes, label, out] :
             {std::tuple{capacity * 4U, " keys A", &scratch.keys[0]},
              std::tuple{capacity * 4U, " keys B", &scratch.keys[1]},
              std::tuple{order_bytes, " order", &scratch.values[0]},
              std::tuple{capacity * 4U, " values B", &scratch.values[1]},
              std::tuple{digit_bytes, " digits", &scratch.digits},
              std::tuple{slice_bytes, " slices", &scratch.slice_args}}) {
            if (bytes == 0U) {
                continue;
            }
            Result<Buffer> buffer = device.create_buffer(bytes, name + label);
            if (!buffer) {
                return device_error("creating " + name + label, buffer.error());
            }
            *out = buffer.value();
        }
    }
    if (any_interleaved) {
        Result<Buffer> span = device.create_buffer(8U, "ajfx sort " + effect.name + " span");
        if (!span) {
            return device_error("creating the sort span of " + effect.name, span.error());
        }
        state->span = span.value();
    }
    if (any) {
        for (const auto& [code, name, out] :
             {std::tuple{&kernels.sort_keys, "ajfx sort_keys", &state->keys},
              std::tuple{&kernels.sort_count, "ajfx sort_count", &state->count},
              std::tuple{&kernels.sort_scan, "ajfx sort_scan", &state->scan},
              std::tuple{&kernels.sort_scatter, "ajfx sort_scatter", &state->scatter},
              std::tuple{&kernels.sort_range, "ajfx sort_range", &state->range},
              std::tuple{&kernels.sort_slices, "ajfx sort_slices", &state->slices}}) {
            if (code->empty()) {
                return Error{ErrorCode::GpuUnavailable, "the kernel set has no sort kernels"};
            }
            Result<Pipeline> pipeline = device.create_compute_pipeline(*code, name);
            if (!pipeline) {
                return device_error(std::string{"creating "} + name, pipeline.error());
            }
            *out = pipeline.value();
        }
    }
    return ParticleSorter{std::move(state)};
}

Buffer ParticleSorter::sort(const GpuSimulation& simulation, std::size_t emitter, Vec3 axis) {
    State& s = *state_;
    if (emitter >= s.emitters.size() || !s.emitters[emitter]) {
        return Buffer{};
    }
    State::Scratch& scratch = *s.emitters[emitter];
    Device& device = *s.device;
    const EmitterBuffers buffers = simulation.emitter_buffers(emitter);

    Dispatch d;
    d.constants[0] = buffers.layout.stride;
    d.constants[1] = static_cast<std::uint32_t>(buffers.layout.position);
    d.constants[3] = scratch.max_blocks;
    d.constants[4] = std::bit_cast<std::uint32_t>(axis.x);
    d.constants[5] = std::bit_cast<std::uint32_t>(axis.y);
    d.constants[6] = std::bit_cast<std::uint32_t>(axis.z);
    // One group per block of live particles, as the simulation sized them.
    const auto per_block = [&](Pipeline pipeline, std::uint32_t from) {
        Dispatch pass = d;
        pass.pipeline = pipeline;
        pass.buffers = {buffers.particles, buffers.counters, scratch.keys[from], scratch.values[from],
                        scratch.keys[1U - from], scratch.values[1U - from], scratch.digits, Buffer{}};
        pass.indirect = buffers.args;
        pass.indirect_offset = buffers.block_args_offset;
        return pass;
    };

    // Keys into pair 0 (the "from 1" binding writes pair 0), with the first
    // pass's digit counts.
    d.constants[2] = first_bit;
    device.dispatch(per_block(s.keys, 1U));
    device.barrier();
    for (std::uint32_t pass = 0U; pass < passes; ++pass) {
        const std::uint32_t from = pass % 2U;
        d.constants[2] = first_bit + pass * 4U;
        if (pass > 0U) {
            device.dispatch(per_block(s.count, from));
            device.barrier();
        }
        Dispatch scan = per_block(s.scan, from);
        scan.indirect = Buffer{};
        device.dispatch(scan);
        device.barrier();
        device.dispatch(per_block(s.scatter, from));
        device.barrier();
    }
    return scratch.values[0];
}

void ParticleSorter::slice(const GpuSimulation& simulation) {
    State& s = *state_;
    if (!s.span.valid()) {
        return;
    }
    Device& device = *s.device;
    // Nothing yet: the farthest key starts at the largest, the nearest at
    // the smallest.
    const std::uint32_t empty[2] = {0xffffffffU, 0U};
    device.write_buffer(s.span, 0U, std::span{reinterpret_cast<const std::uint8_t*>(empty), sizeof(empty)});
    device.barrier();
    // Every interleaved emitter widens the span, then each is cut by it.
    for (const Pipeline pipeline : {s.range, s.slices}) {
        for (std::size_t e = 0U; e < s.emitters.size(); ++e) {
            if (!s.emitters[e] || !s.emitters[e]->slice_args.valid()) {
                continue;
            }
            const State::Scratch& scratch = *s.emitters[e];
            Dispatch d;
            d.pipeline = pipeline;
            d.constants[8] = scratch.capacity;
            d.constants[9] = depth_slices;
            d.buffers = {Buffer{}, simulation.emitter_buffers(e).counters, scratch.keys[0], scratch.values[0],
                         Buffer{}, scratch.slice_args, Buffer{}, s.span};
            device.dispatch(d);
        }
        device.barrier();
    }
}

DepthSlice ParticleSorter::depth_slice(std::size_t emitter, std::uint32_t slice) const noexcept {
    const State& s = *state_;
    if (emitter >= s.emitters.size() || !s.emitters[emitter] || !s.emitters[emitter]->slice_args.valid()) {
        return DepthSlice{};
    }
    const State::Scratch& scratch = *s.emitters[emitter];
    return DepthSlice{scratch.slice_args, std::uint64_t{slice} * 16U, scratch.capacity + slice};
}

} // namespace ajfx::gpu
