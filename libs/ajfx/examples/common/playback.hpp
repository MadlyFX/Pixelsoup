#pragma once

// Playback shared by the sample applications: loading an effect onto the GPU
// (using a package's precompiled kernels when it has them), seeking with
// checkpoints, and an orbiting camera.

#include <ajfx/d3d12.hpp>
#include <ajfx/gpu/renderer.hpp>
#include <ajfx/package.hpp>
#include <ajfx/presets.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <fstream>
#include <iterator>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace samples {

inline constexpr std::array<float, 4> background{0.015F, 0.017F, 0.022F, 1.0F};

inline std::optional<ajfx::Effect> preset(const std::string& name) {
    if (name == "sparks") {
        return ajfx::presets::sparks();
    }
    if (name == "smoke") {
        return ajfx::presets::smoke();
    }
    if (name == "confetti") {
        return ajfx::presets::confetti();
    }
    return std::nullopt;
}

template <class Path>
std::optional<std::vector<std::uint8_t>> read_file(const Path& path) {
    std::ifstream stream{path, std::ios::binary};
    if (!stream) {
        return std::nullopt;
    }
    return std::vector<std::uint8_t>{std::istreambuf_iterator<char>{stream}, {}};
}

// One effect on the GPU, with checkpoints so the timeline can be scrubbed in
// either direction and land exactly where playing would have.
class Player final {
public:
    static constexpr std::uint64_t checkpoint_interval = 60U;
    static constexpr std::size_t checkpoint_limit = 64U;

    // `package` is the file the effect came from, if any; its kernels are
    // used when they match, otherwise `compiler` compiles them.
    static ajfx::Result<std::unique_ptr<Player>> create(ajfx::gpu::Device& device, ajfx::gpu::ShaderCompiler* compiler,
                                                        ajfx::Effect effect,
                                                        const std::vector<std::uint8_t>* package) {
        std::optional<ajfx::gpu::KernelSet> kernels;
        std::string note;
        if (package != nullptr) {
            ajfx::Result<ajfx::gpu::KernelSet> carried =
                ajfx::gpu::read_package_kernels(*package, effect, device.shader_format());
            if (carried) {
                kernels = std::move(carried).value();
                note = "using the package's precompiled kernels";
            } else {
                note = carried.error().message;
            }
        }
        if (!kernels) {
            if (compiler == nullptr) {
                return ajfx::Error{ajfx::ErrorCode::KernelsUnavailable,
                                   note.empty() ? "no precompiled kernels and no compiler"
                                                : note + ", and no compiler is available"};
            }
            ajfx::Result<ajfx::gpu::KernelSet> compiled = ajfx::gpu::compile_kernels(effect, *compiler);
            if (!compiled) {
                return compiled.error();
            }
            kernels = std::move(compiled).value();
            note = package != nullptr ? note + "; compiled them instead" : "compiled the kernels";
        }
        ajfx::Result<ajfx::gpu::GpuSimulation> simulation =
            ajfx::gpu::GpuSimulation::create(device, effect, *kernels, 1U);
        if (!simulation) {
            return simulation.error();
        }
        // A package carries its textures; the presets generate theirs.
        ajfx::Result<ajfx::TextureSet> textures =
            package != nullptr ? ajfx::read_package_textures(*package, effect) : ajfx::presets::textures();
        if (!textures) {
            return textures.error();
        }
        // Only what the effect names, so an exported package carries no more.
        std::erase_if(textures.value(), [&](const auto& entry) {
            return std::none_of(effect.emitters.begin(), effect.emitters.end(),
                                [&](const ajfx::Emitter& emitter) { return emitter.renderer.texture == entry.first; });
        });
        ajfx::Result<ajfx::gpu::ParticleRenderer> renderer =
            ajfx::gpu::ParticleRenderer::create(device, effect, *kernels, textures.value(),
                                                ajfx::gpu::TextureFormat::Rgba8Unorm);
        if (!renderer) {
            return renderer.error();
        }
        ajfx::Result<ajfx::gpu::ParticleSorter> sorter = ajfx::gpu::ParticleSorter::create(device, effect, *kernels);
        if (!sorter) {
            return sorter.error();
        }
        auto player = std::unique_ptr<Player>(new Player(std::move(effect), *std::move(kernels),
                                                         std::move(simulation).value(), std::move(renderer).value(),
                                                         std::move(sorter).value()));
        player->kernel_note_ = std::move(note);
        player->textures_ = std::move(textures).value();
        return player;
    }

    // Moves to `target` steps, from the nearest checkpoint when going back.
    void seek(std::uint64_t target) {
        if (target < simulation_.tick()) {
            auto at = checkpoints_.upper_bound(target);
            if (at == checkpoints_.begin()) {
                simulation_.reset();
            } else {
                simulation_.restore(std::prev(at)->second);
            }
        }
        while (simulation_.tick() < target) {
            if (simulation_.tick() % checkpoint_interval == 0U && !checkpoints_.contains(simulation_.tick())) {
                ajfx::Result<ajfx::gpu::GpuCheckpoint> saved = simulation_.save();
                if (saved) {
                    if (checkpoints_.size() == checkpoint_limit) {
                        checkpoints_.erase(checkpoints_.begin());
                    }
                    checkpoints_.emplace(simulation_.tick(), std::move(saved).value());
                }
            }
            simulation_.step();
        }
    }

    // The space the particles occupy, sprite sizes included, sampled at a few
    // moments up to `seconds` (reads back from the GPU; for framing the view,
    // not for every frame). Nothing if no particle is ever alive.
    std::optional<std::pair<ajfx::Vec3, ajfx::Vec3>> bounds_until(double seconds) {
        std::optional<std::pair<ajfx::Vec3, ajfx::Vec3>> box;
        for (const double at : {0.5, 1.0, 2.0, 4.0, seconds}) {
            if (at > seconds) {
                continue;
            }
            seek(static_cast<std::uint64_t>(at * effect_.simulation_rate));
            for (std::size_t i = 0U; i < effect_.emitters.size(); ++i) {
                ajfx::Result<ajfx::EmitterState> state = simulation_.read_emitter(i);
                if (!state) {
                    continue;
                }
                const ajfx::EmitterState& s = state.value();
                for (std::size_t p = 0U; p < s.alive(); ++p) {
                    const ajfx::Vec3 c = s.position[p];
                    const float r = std::isfinite(s.size[p]) ? std::abs(s.size[p]) * 0.5F : 0.0F;
                    if (!std::isfinite(c.x) || !std::isfinite(c.y) || !std::isfinite(c.z)) {
                        continue;
                    }
                    const ajfx::Vec3 lo{c.x - r, c.y - r, c.z - r};
                    const ajfx::Vec3 hi{c.x + r, c.y + r, c.z + r};
                    if (!box) {
                        box = std::pair{lo, hi};
                    }
                    box->first = {std::min(box->first.x, lo.x), std::min(box->first.y, lo.y),
                                  std::min(box->first.z, lo.z)};
                    box->second = {std::max(box->second.x, hi.x), std::max(box->second.y, hi.y),
                                   std::max(box->second.z, hi.z)};
                }
            }
        }
        return box;
    }

    // Live particles per emitter (reads back from the GPU).
    std::vector<std::uint32_t> alive_counts(ajfx::gpu::Device& device) {
        std::vector<std::uint32_t> counts;
        for (std::size_t i = 0U; i < effect_.emitters.size(); ++i) {
            ajfx::Result<std::vector<std::uint8_t>> bytes =
                device.read_buffer(simulation_.emitter_buffers(i).counters, 0U, 4U);
            std::uint32_t count = 0U;
            if (bytes && bytes.value().size() == 4U) {
                count = static_cast<std::uint32_t>(bytes.value()[0]) | (static_cast<std::uint32_t>(bytes.value()[1]) << 8U) |
                        (static_cast<std::uint32_t>(bytes.value()[2]) << 16U) |
                        (static_cast<std::uint32_t>(bytes.value()[3]) << 24U);
            }
            counts.push_back(count);
        }
        return counts;
    }

    const ajfx::Effect& effect() const { return effect_; }
    const ajfx::gpu::KernelSet& kernels() const { return kernels_; }
    // The textures the effect names, for writing into a package.
    const ajfx::TextureSet& textures() const { return textures_; }
    // Where the kernels came from, or why a package's could not be used.
    const std::string& kernel_note() const { return kernel_note_; }
    ajfx::gpu::GpuSimulation& simulation() { return simulation_; }
    // Records drawing the current particles, sorted where the effect asks.
    void render(const ajfx::gpu::Camera& camera, ajfx::gpu::Texture target, const std::array<float, 4>* clear) {
        renderer_.render(simulation_, sorter_, camera, target, clear);
    }

private:
    Player(ajfx::Effect effect, ajfx::gpu::KernelSet kernels, ajfx::gpu::GpuSimulation simulation,
           ajfx::gpu::ParticleRenderer renderer, ajfx::gpu::ParticleSorter sorter)
        : effect_{std::move(effect)},
          kernels_{std::move(kernels)},
          simulation_{std::move(simulation)},
          renderer_{std::move(renderer)},
          sorter_{std::move(sorter)} {}

    ajfx::Effect effect_;
    ajfx::gpu::KernelSet kernels_;
    ajfx::gpu::GpuSimulation simulation_;
    ajfx::gpu::ParticleRenderer renderer_;
    ajfx::gpu::ParticleSorter sorter_;
    std::map<std::uint64_t, ajfx::gpu::GpuCheckpoint> checkpoints_;
    std::string kernel_note_;
    ajfx::TextureSet textures_;
};

struct Orbit final {
    ajfx::Vec3 target{0.0F, 1.2F, 0.0F};
    float yaw{0.6F};
    float pitch{0.25F};
    float distance{7.0F};

    [[nodiscard]] ajfx::gpu::Camera camera(float aspect) const {
        const ajfx::Vec3 eye{target.x + distance * std::cos(pitch) * std::sin(yaw),
                             target.y + distance * std::sin(pitch),
                             target.z + distance * std::cos(pitch) * std::cos(yaw)};
        return ajfx::gpu::Camera::look_at(eye, target, ajfx::Vec3{0.0F, 1.0F, 0.0F}, 0.9F, aspect, 0.05F, 500.0F);
    }

    // Aims at the middle of a box from far enough away to see all of it.
    void frame(const std::pair<ajfx::Vec3, ajfx::Vec3>& box) {
        target = (box.first + box.second) * 0.5F;
        const ajfx::Vec3 extent = box.second - box.first;
        const float radius = 0.5F * std::sqrt(ajfx::dot(extent, extent));
        distance = std::clamp(radius / std::tan(0.45F) * 1.15F, 1.0F, 400.0F);
    }
};

} // namespace samples
