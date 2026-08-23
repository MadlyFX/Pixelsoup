#pragma once

#include <compare>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <type_traits>

namespace mediaserver::core {

template <typename Tag, typename Rep>
class StrongId final {
    static_assert(std::is_integral_v<Rep>, "StrongId requires an integral representation");

public:
    using tag_type = Tag;
    using value_type = Rep;

    constexpr StrongId() noexcept = default;
    explicit constexpr StrongId(Rep value) noexcept : value_(value) {}

    [[nodiscard]] constexpr Rep value() const noexcept { return value_; }
    [[nodiscard]] explicit constexpr operator bool() const noexcept { return value_ != Rep{}; }

    auto operator<=>(const StrongId&) const noexcept = default;

private:
    Rep value_{};
};

} // namespace mediaserver::core

namespace std {

template <typename Tag, typename Rep>
struct hash<mediaserver::core::StrongId<Tag, Rep>> {
    [[nodiscard]] std::size_t operator()(
        const mediaserver::core::StrongId<Tag, Rep>& id) const noexcept {
        using UnsignedRep = std::make_unsigned_t<Rep>;
        auto value = static_cast<std::uint64_t>(static_cast<UnsignedRep>(id.value()));
        value += 0x9e3779b97f4a7c15ULL;
        value = (value ^ (value >> 30U)) * 0xbf58476d1ce4e5b9ULL;
        value = (value ^ (value >> 27U)) * 0x94d049bb133111ebULL;
        value ^= value >> 31U;
        return static_cast<std::size_t>(value);
    }
};

} // namespace std
