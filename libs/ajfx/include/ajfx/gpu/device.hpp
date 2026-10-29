#pragma once

#include <ajfx/result.hpp>

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
// argument buffer. Every compute kernel sees the same binding layout: sixteen
// 32-bit root constants (register b0) and up to eight raw read-write buffers
// (registers u0 to u7). A backend tracks each buffer's state and inserts the
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
inline constexpr std::uint32_t root_constant_count = 16U;

enum class ShaderFormat {
    Dxil,
};

struct Dispatch final {
    Pipeline pipeline;
    std::array<std::uint32_t, root_constant_count> constants{};
    // Bound at u0, u1, ... in order. Invalid entries end the list.
    std::array<Buffer, max_bound_buffers> buffers{};
    // Direct dispatch size, used when `indirect` is invalid.
    std::uint32_t groups_x{1U};
    std::uint32_t groups_y{1U};
    // Three uint32 group counts read from this buffer at the offset.
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
    virtual void destroy_pipeline(Pipeline pipeline) = 0;

    // Recorded work, executed in order at the next flush.
    virtual void write_buffer(Buffer buffer, std::uint64_t offset, std::span<const std::uint8_t> bytes) = 0;
    virtual void copy_buffer(Buffer destination, std::uint64_t destination_offset, Buffer source,
                             std::uint64_t source_offset, std::uint64_t bytes) = 0;
    virtual void dispatch(const Dispatch& dispatch) = 0;
    // Every earlier read-write access completes before any later one starts.
    virtual void barrier() = 0;

    // Submits the recorded work and waits for it.
    [[nodiscard]] virtual Result<bool> flush() = 0;
    // Flushes, then returns the bytes.
    [[nodiscard]] virtual Result<std::vector<std::uint8_t>> read_buffer(Buffer buffer, std::uint64_t offset,
                                                                       std::uint64_t bytes) = 0;
};

// Turns HLSL into the device's bytecode. Hosts that ship precompiled kernels
// (the media server reads them from packages) never need one at runtime.
class ShaderCompiler {
public:
    virtual ~ShaderCompiler() = default;

    [[nodiscard]] virtual ShaderFormat format() const noexcept = 0;
    // Compiles one compute entry point with IEEE-strict arithmetic.
    [[nodiscard]] virtual Result<std::vector<std::uint8_t>> compile_compute(std::string_view source,
                                                                            std::string_view entry_point,
                                                                            std::string_view name) = 0;
};

} // namespace ajfx::gpu
