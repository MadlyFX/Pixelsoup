#include <mediaserver/net/node_index_assignments.hpp>
#include <mediaserver/persist/settings.hpp>

#include <algorithm>
#include <array>
#include <mutex>
#include <random>
#include <stdexcept>
#include <vector>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <bcrypt.h>
#else
#include <fcntl.h>
#include <sys/file.h>
#include <unistd.h>
#endif

namespace mediaserver::net {
namespace {
constexpr std::size_t entry_bytes = 20U;
void put(std::vector<std::byte>& bytes, core::u64 value, std::size_t count) {
    for (std::size_t i = 0; i < count; ++i) {
        bytes.push_back(static_cast<std::byte>(value >> (i * 8U)));
    }
}
core::u64 get(std::span<const std::byte> bytes, std::size_t offset, std::size_t count) {
    core::u64 value{};
    for (std::size_t i = 0; i < count; ++i) {
        value |= static_cast<core::u64>(std::to_integer<core::u8>(bytes[offset + i])) << (i * 8U);
    }
    return value;
}
}

class NodeIndexAssignments::State final {
public:
    struct Entry { Id128 id; core::u32 index; };
    std::filesystem::path file;
    std::vector<Entry> entries;
    std::string failure;
    std::mutex mutex;
#if defined(_WIN32)
    HANDLE lock{INVALID_HANDLE_VALUE};
#else
    int lock{-1};
#endif
    ~State() {
#if defined(_WIN32)
        if (lock != INVALID_HANDLE_VALUE) CloseHandle(lock);
#else
        if (lock != -1) close(lock);
#endif
    }
};

NodeIndexAssignments::NodeIndexAssignments(std::filesystem::path file)
    : state_(std::make_unique<State>()) {
    state_->file = std::move(file);
    if (state_->file.empty()) {
        state_->failure = "No location for node assignments.";
        return;
    }
    std::error_code error;
    if (!state_->file.parent_path().empty()) {
        std::filesystem::create_directories(state_->file.parent_path(), error);
    }
    if (error) { state_->failure = "Could not create the node assignments directory."; return; }
    auto lock_file = state_->file;
    lock_file += ".lock";
#if defined(_WIN32)
    state_->lock = CreateFileW(lock_file.c_str(), GENERIC_READ | GENERIC_WRITE,
                              0U, nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    const bool locked = state_->lock != INVALID_HANDLE_VALUE;
#else
    state_->lock = open(lock_file.c_str(), O_CREAT | O_RDWR | O_CLOEXEC, 0600);
    const bool locked = state_->lock != -1 && flock(state_->lock, LOCK_EX | LOCK_NB) == 0;
#endif
    if (!locked) { state_->failure = "Node assignments are unavailable or owned by another editor."; return; }
    auto loaded = persist::load_node_assignments(state_->file);
    if (!loaded || loaded.value().size() % entry_bytes != 0U ||
        loaded.value().size() / entry_bytes > max_session_node_index + 1U) {
        state_->failure = "Could not read node assignments. The existing record was preserved.";
        return;
    }
    std::array<bool, max_session_node_index + 1U> used{};
    const auto& bytes = loaded.value();
    state_->entries.reserve(max_session_node_index + 1U);
    for (std::size_t offset = 0; offset < bytes.size(); offset += entry_bytes) {
        State::Entry entry{{get(bytes, offset, 8U), get(bytes, offset + 8U, 8U)},
                            static_cast<core::u32>(get(bytes, offset + 16U, 4U))};
        if (!entry.id || entry.index > max_session_node_index || used[entry.index] ||
            std::any_of(state_->entries.begin(), state_->entries.end(),
                        [&](const auto& other) { return other.id == entry.id; })) {
            state_->failure = "Invalid node assignments. The existing record was preserved.";
            return;
        }
        used[entry.index] = true;
        state_->entries.push_back(entry);
    }
}

NodeIndexAssignments::~NodeIndexAssignments() = default;
bool NodeIndexAssignments::ready() const noexcept { return state_->failure.empty(); }
std::string NodeIndexAssignments::error() const { return state_->failure; }

NodeIndexAssignment NodeIndexAssignments::assign(Id128 id, core::u32 requested) noexcept {
    try {
        const std::lock_guard lock{state_->mutex};
        if (!ready() || !id) return {};
        const auto existing = std::find_if(state_->entries.begin(), state_->entries.end(),
            [&](const auto& entry) { return entry.id == id; });
        if (existing != state_->entries.end()) {
            return requested == automatic_node_index || requested == existing->index
                ? NodeIndexAssignment{NodeAssignmentStatus::Assigned, existing->index}
                : NodeIndexAssignment{NodeAssignmentStatus::Conflict, existing->index};
        }
        if (requested != automatic_node_index && requested > max_session_node_index) {
            return {NodeAssignmentStatus::Conflict, automatic_node_index};
        }
        std::array<bool, max_session_node_index + 1U> used{};
        for (const auto& entry : state_->entries) used[entry.index] = true;
        if (requested == automatic_node_index) {
            const auto free = std::find(used.begin(), used.end(), false);
            if (free == used.end()) return {NodeAssignmentStatus::Full, automatic_node_index};
            requested = static_cast<core::u32>(free - used.begin());
        } else if (used[requested]) {
            return {NodeAssignmentStatus::Conflict, automatic_node_index};
        }
        std::vector<std::byte> bytes;
        bytes.reserve((state_->entries.size() + 1U) * entry_bytes);
        for (const auto& entry : state_->entries) {
            put(bytes, entry.id.high, 8U); put(bytes, entry.id.low, 8U); put(bytes, entry.index, 4U);
        }
        put(bytes, id.high, 8U); put(bytes, id.low, 8U); put(bytes, requested, 4U);
        if (!persist::save_node_assignments(state_->file, bytes)) return {};
        state_->entries.push_back({id, requested});
        return {NodeAssignmentStatus::Assigned, requested};
    } catch (const std::exception&) {
        return {};
    }
}

Id128 new_render_machine_id() {
    Id128 id;
#if defined(_WIN32)
    if (BCryptGenRandom(nullptr, reinterpret_cast<PUCHAR>(&id), sizeof(id),
                       BCRYPT_USE_SYSTEM_PREFERRED_RNG) != 0) {
        throw std::runtime_error("Could not generate this render machine's identity.");
    }
#else
    std::random_device random;
    id = {(static_cast<core::u64>(random()) << 32U) | random(),
          (static_cast<core::u64>(random()) << 32U) | random()};
#endif
    if (!id) throw std::runtime_error("Could not generate this render machine's identity.");
    return id;
}
} // namespace mediaserver::net
