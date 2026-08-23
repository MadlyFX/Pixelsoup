#pragma once

#include <cassert>
#include <utility>
#include <variant>

namespace mediaserver::core {

template <typename T, typename E>
class Result final {
public:
    [[nodiscard]] static Result success(T value) {
        return Result(std::in_place_index<0>, std::move(value));
    }

    [[nodiscard]] static Result failure(E error) {
        return Result(std::in_place_index<1>, std::move(error));
    }

    [[nodiscard]] bool has_value() const noexcept { return value_.index() == 0U; }
    [[nodiscard]] explicit operator bool() const noexcept { return has_value(); }

    [[nodiscard]] T& value() & noexcept {
        assert(has_value());
        return std::get<0>(value_);
    }

    [[nodiscard]] const T& value() const& noexcept {
        assert(has_value());
        return std::get<0>(value_);
    }

    [[nodiscard]] T&& value() && noexcept {
        assert(has_value());
        return std::get<0>(std::move(value_));
    }

    [[nodiscard]] E& error() & noexcept {
        assert(!has_value());
        return std::get<1>(value_);
    }

    [[nodiscard]] const E& error() const& noexcept {
        assert(!has_value());
        return std::get<1>(value_);
    }

private:
    template <std::size_t Index, typename Value>
    explicit Result(std::in_place_index_t<Index> index, Value&& value)
        : value_(index, std::forward<Value>(value)) {}

    std::variant<T, E> value_;
};

} // namespace mediaserver::core

