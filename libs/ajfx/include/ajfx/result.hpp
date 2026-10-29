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
