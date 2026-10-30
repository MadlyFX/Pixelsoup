#include <ajfx/gpu/sort.hpp>

#include <bit>
#include <optional>
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
           emitter.renderer.geometry == Geometry::Sprites;
}

struct ParticleSorter::State final {
    Device* device{nullptr};
    Pipeline keys;
    Pipeline count;
    Pipeline scan;
    Pipeline scatter;
    struct Scratch final {
        // Keys and values ping-pong between the pairs; six passes end in
        // the first.
        Buffer keys[2];
        Buffer values[2];
        Buffer digits;
        std::uint32_t max_blocks{0U};
    };
    // Empty for an emitter that does not sort.
    std::vector<std::optional<Scratch>> emitters;

    ~State() {
        if (device == nullptr) {
            return;
        }
        for (const Pipeline pipeline : {keys, count, scan, scatter}) {
            device->destroy_pipeline(pipeline);
        }
        for (const std::optional<Scratch>& scratch : emitters) {
            if (scratch) {
                for (const Buffer buffer :
                     {scratch->keys[0], scratch->keys[1], scratch->values[0], scratch->values[1], scratch->digits}) {
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
    for (std::size_t i = 0U; i < effect.emitters.size(); ++i) {
        const Emitter& emitter = effect.emitters[i];
        if (!sorts(emitter)) {
            continue;
        }
        any = true;
        State::Scratch& scratch = state->emitters[i].emplace();
        const std::uint64_t capacity = emitter.capacity;
        scratch.max_blocks = static_cast<std::uint32_t>((capacity + block_size - 1U) / block_size);
        const std::string name = "ajfx sort " + effect.name + "/" + emitter.name;
        // Counts, then their offsets.
        const std::uint64_t digit_bytes = 2U * digit_count * std::uint64_t{scratch.max_blocks} * 4U;
        for (const auto& [bytes, label, out] :
             {std::tuple{capacity * 4U, " keys A", &scratch.keys[0]},
              std::tuple{capacity * 4U, " keys B", &scratch.keys[1]},
              std::tuple{capacity * 4U, " order", &scratch.values[0]},
              std::tuple{capacity * 4U, " values B", &scratch.values[1]},
              std::tuple{digit_bytes, " digits", &scratch.digits}}) {
            Result<Buffer> buffer = device.create_buffer(bytes, name + label);
            if (!buffer) {
                return device_error("creating " + name + label, buffer.error());
            }
            *out = buffer.value();
        }
    }
    if (any) {
        for (const auto& [code, name, out] :
             {std::tuple{&kernels.sort_keys, "ajfx sort_keys", &state->keys},
              std::tuple{&kernels.sort_count, "ajfx sort_count", &state->count},
              std::tuple{&kernels.sort_scan, "ajfx sort_scan", &state->scan},
              std::tuple{&kernels.sort_scatter, "ajfx sort_scatter", &state->scatter}}) {
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

} // namespace ajfx::gpu
