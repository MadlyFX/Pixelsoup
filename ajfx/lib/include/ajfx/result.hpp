#pragma once

#include <string>
#include <utility>
#include <variant>

// Errors are values. Nothing in ajfx throws: a host that builds without
// exceptions gets the same diagnostics as one that does not.
namespace ajfx {

enum class ErrorCode {
    InvalidEffect,
    UnknownParameter,
    ParameterTypeMismatch,
    BudgetExceeded,
    MalformedPackage,
    UnsupportedVersion,
    // The GPU side: no usable device or compiler, a failed compile, or a
    // device error while running.
    GpuUnavailable,
    ShaderCompilation,
    DeviceFailure,
    // A package has no precompiled kernels for the device, or they were
    // compiled from source that no longer matches.
    KernelsUnavailable,
    // An emitter names a texture its package (or texture set) does not hold.
    MissingTexture,
    // An emitter collides with a distance field its package does not hold.
    MissingField,
    // An emitter draws a mesh its package does not hold.
    MissingMesh,
    // An effect has rigid physics emitters, and the host gave no RigidSolver
    // to run them (see rigid_solver.hpp).
    RigidSolverMissing,
    // The rigid solver could not build or restore an emitter's world.
    RigidSolverFailure,
    // Saved simulation state or a cache (cache.hpp) is truncated, damaged
    // or inconsistent.
    MalformedCache,
    // A cache that does not belong to the effect in front of it: another
    // package, instance seed or bake-time parameter value.
    StaleCache,
};

struct Error final {
    ErrorCode code{};
    std::string message;
};

template <class T>
class [[nodiscard]] Result final {
public:
    Result(T value) : storage_{std::in_place_index<0>, std::move(value)} {}
    Result(Error error) : storage_{std::in_place_index<1>, std::move(error)} {}

    [[nodiscard]] bool ok() const noexcept { return storage_.index() == 0U; }
    explicit operator bool() const noexcept { return ok(); }

    // Calling these on the wrong alternative is a programming error.
    [[nodiscard]] T& value() & { return *std::get_if<0>(&storage_); }
    [[nodiscard]] const T& value() const& { return *std::get_if<0>(&storage_); }
    [[nodiscard]] T&& value() && { return std::move(*std::get_if<0>(&storage_)); }
    [[nodiscard]] const Error& error() const& { return *std::get_if<1>(&storage_); }

private:
    std::variant<T, Error> storage_;
};

} // namespace ajfx
