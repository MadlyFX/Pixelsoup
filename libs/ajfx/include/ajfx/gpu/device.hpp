#pragma once

#include <ajfx/result.hpp>
#include <ajfx/texture.hpp>

#include <array>
#include <cstdint>
#include <span>
#include <string_view>
#include <vector>

// The GPU interface ajfx runs on. A backend implements it once; the
// simulation, compaction and (later) drawing are written against it alone.
//
// The model is deliberately narrow. Every buffer is device-local and usable
// as a raw read-write buffer, a copy source or destination, and an indirect
// argument buffer. Every kernel and draw sees the same binding layout: sixteen
// 32-bit root constants (register b0, space 1) and up to eight raw read-write buffers
// (registers u0 to u7), unbound slots left empty. Draws may also sample up to
// four textures (t0 to t3) through one linear, clamped, mipmapped sampler
// (s0). Textures are either render targets or sampled textures made from
// TextureData. A backend tracks each buffer's state and inserts the
// transitions a use needs; `barrier` orders read-write access between
// dispatches.
namespace ajfx::gpu {

struct Buffer final {
    std::uint32_t id{0U};
    [[nodiscard]] bool valid() const noexcept { return id != 0U; }
    friend constexpr bool operator==(Buffer, Buffer) = default;
};

struct Pipeline final {
    std::uint32_t id{0U};
    [[nodiscard]] bool valid() const noexcept { return id != 0U; }
    friend constexpr bool operator==(Pipeline, Pipeline) = default;
};

inline constexpr std::uint32_t max_bound_buffers = 8U;
inline constexpr std::uint32_t max_bound_textures = 4U;
inline constexpr std::uint32_t root_constant_count = 16U;

enum class ShaderFormat {
    Dxil,
};

struct Dispatch final {
    Pipeline pipeline;
    std::array<std::uint32_t, root_constant_count> constants{};
    // Bound at u0 to u7; invalid entries leave the slot empty.
    std::array<Buffer, max_bound_buffers> buffers{};
    // Direct dispatch size, used when `indirect` is invalid.
    std::uint32_t groups_x{1U};
    std::uint32_t groups_y{1U};
    // Three uint32 group counts read from this buffer at the offset.
    Buffer indirect;
    std::uint64_t indirect_offset{0U};
};

struct Texture final {
    std::uint32_t id{0U};
    [[nodiscard]] bool valid() const noexcept { return id != 0U; }
    friend constexpr bool operator==(Texture, Texture) = default;
};

enum class TextureFormat {
    Rgba8Unorm,
    Rgba16Float,
};

// How a draw's output combines with the target. Pixel shaders output
// premultiplied colour, so Alpha and Premultiplied share one equation.
enum class Blend {
    Opaque,
    Premultiplied, // one, one minus source alpha
    Additive,      // one, one
};

enum class ShaderStage {
    Compute,
    Vertex,
    Pixel,
};

struct GraphicsPipelineDesc final {
    std::span<const std::uint8_t> vertex;
    std::span<const std::uint8_t> pixel;
    Blend blend{Blend::Premultiplied};
    TextureFormat format{TextureFormat::Rgba8Unorm};
};

// A triangle-list draw with no vertex buffers: vertex shaders read what they
// need from the bound buffers using SV_VertexID and SV_InstanceID. Bindings
// are the same as a dispatch's.
struct Draw final {
    Pipeline pipeline;
    std::array<std::uint32_t, root_constant_count> constants{};
    std::array<Buffer, max_bound_buffers> buffers{};
    // Sampled at t0 to t3; invalid entries leave the slot empty.
    std::array<Texture, max_bound_textures> textures{};
    std::uint32_t vertex_count{0U};
    std::uint32_t instance_count{0U};
    // Four uint32s (vertex count, instance count, first vertex, first
    // instance) read from this buffer at the offset.
    Buffer indirect;
    std::uint64_t indirect_offset{0U};
};

class Device {
public:
    virtual ~Device() = default;

    [[nodiscard]] virtual ShaderFormat shader_format() const noexcept = 0;
    // Name of the adapter, for diagnostics.
    [[nodiscard]] virtual std::string_view adapter_name() const noexcept = 0;

    // Zero-filled.
    [[nodiscard]] virtual Result<Buffer> create_buffer(std::uint64_t bytes, std::string_view name) = 0;
    virtual void destroy_buffer(Buffer buffer) = 0;
    [[nodiscard]] virtual Result<Pipeline> create_compute_pipeline(std::span<const std::uint8_t> bytecode,
                                                                   std::string_view name) = 0;
    [[nodiscard]] virtual Result<Pipeline> create_graphics_pipeline(const GraphicsPipelineDesc& desc,
                                                                    std::string_view name) = 0;
    virtual void destroy_pipeline(Pipeline pipeline) = 0;
    [[nodiscard]] virtual Result<Texture> create_render_target(std::uint32_t width, std::uint32_t height,
                                                               TextureFormat format, std::string_view name) = 0;
    // A sampled texture holding `texture`'s mips (sRGB-decoded when sampled,
    // for an sRGB texture). The upload is recorded work.
    [[nodiscard]] virtual Result<Texture> create_texture(const TextureData& texture, std::string_view name) = 0;
    virtual void destroy_texture(Texture texture) = 0;

    // Recorded work, executed in order at the next flush.
    virtual void write_buffer(Buffer buffer, std::uint64_t offset, std::span<const std::uint8_t> bytes) = 0;
    virtual void copy_buffer(Buffer destination, std::uint64_t destination_offset, Buffer source,
                             std::uint64_t source_offset, std::uint64_t bytes) = 0;
    virtual void dispatch(const Dispatch& dispatch) = 0;
    // Draws go to the target of the enclosing begin_rendering/end_rendering.
    // Nothing but draws may be recorded in between.
    virtual void begin_rendering(Texture target, const std::array<float, 4>* clear_color) = 0;
    virtual void draw(const Draw& draw) = 0;
    virtual void end_rendering() = 0;
    // Every earlier read-write access completes before any later one starts.
    virtual void barrier() = 0;

    // Submits the recorded work and waits for it.
    [[nodiscard]] virtual Result<bool> flush() = 0;
    // Flushes, then returns the bytes.
    [[nodiscard]] virtual Result<std::vector<std::uint8_t>> read_buffer(Buffer buffer, std::uint64_t offset,
                                                                       std::uint64_t bytes) = 0;
    // Flushes, then returns the pixels row by row with no padding.
    [[nodiscard]] virtual Result<std::vector<std::uint8_t>> read_texture(Texture texture) = 0;
};

// Turns HLSL into the device's bytecode. Hosts that ship precompiled kernels
// (the media server reads them from packages) never need one at runtime.
class ShaderCompiler {
public:
    virtual ~ShaderCompiler() = default;

    [[nodiscard]] virtual ShaderFormat format() const noexcept = 0;
    // Compiles one entry point with IEEE-strict arithmetic.
    [[nodiscard]] virtual Result<std::vector<std::uint8_t>> compile(std::string_view source,
                                                                    std::string_view entry_point, ShaderStage stage,
                                                                    std::string_view name) = 0;
};

} // namespace ajfx::gpu
