#pragma once

#include <mediaserver/doc/document.hpp>

#include <optional>
#include <string>
#include <vector>

// Library organisation as document edits: folders, filing, and names. None
// of these touch what an asset is or how it renders; they only change where
// it is listed and what it is called there.
namespace mediaserver::assets {

struct LibraryEdit final {
    enum class Kind : core::u8 {
        // A new asset.folder named `name` under `destination` (zero: the top
        // level), with the id the caller allocated.
        CreateFolder,
        // `target` - an asset or a folder - takes `name`.
        Rename,
        // Every asset and folder in `items` is filed under `destination`. A
        // folder is never filed inside itself or its own descendants.
        Move,
        // `target` folder goes; what it held moves up to its parent.
        DeleteFolder,
    };

    Kind kind{Kind::CreateFolder};
    doc::NodeId target{};
    doc::NodeId destination{};
    std::string name;
    std::vector<doc::NodeId> items;

    bool operator==(const LibraryEdit&) const = default;
};

// The document after `edit`, or nothing when it would change nothing or
// names something that is not there: a stale request is a no-op, never a
// partial edit. `new_folder` is used only by CreateFolder and must be free.
[[nodiscard]] std::optional<doc::Root>
plan_library_edit(const doc::Root& root, const LibraryEdit& edit,
                  doc::NodeId new_folder = {});

} // namespace mediaserver::assets
