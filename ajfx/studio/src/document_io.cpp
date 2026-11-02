#include "document_io.hpp"

#include "json.hpp"
#include "scene_io.hpp"

#include <fstream>
#include <iterator>
#include <system_error>

namespace ajfx::studio {

std::string save_document(const Document& document) {
    return save_scene(document);
}

Loaded load_document(std::string_view text) {
    json::ParseError parse_error;
    const std::optional<json::Value> root = json::parse(text, parse_error);
    if (!root) {
        return Loaded{{}, "Not a readable scene: " + parse_error.message + " (at byte " +
                              std::to_string(parse_error.offset) + ")"};
    }
    if (root->string_or("format", "") != "ajfx-studio-scene") {
        return Loaded{{}, "This file is not an ajfx Studio scene"};
    }
    return load_scene(*root);
}

bool write_file(const std::filesystem::path& path, std::span<const std::uint8_t> bytes, std::string& error) {
    // Written beside the target and renamed over it, so a reader never sees
    // half a file.
    std::filesystem::path temporary = path;
    temporary += ".partial";
    {
        std::ofstream out{temporary, std::ios::binary | std::ios::trunc};
        if (!out) {
            error = "Could not write " + path.string();
            return false;
        }
        out.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
        if (!out.good()) {
            error = "Could not write " + path.string();
            return false;
        }
    }
    std::error_code code;
    std::filesystem::rename(temporary, path, code);
    if (code) {
        std::filesystem::remove(temporary, code);
        error = "Could not replace " + path.string() + " (it may be open elsewhere)";
        return false;
    }
    return true;
}

std::optional<std::vector<std::uint8_t>> read_file(const std::filesystem::path& path, std::string& error) {
    std::ifstream in{path, std::ios::binary};
    if (!in) {
        error = "Could not open " + path.string();
        return std::nullopt;
    }
    std::vector<std::uint8_t> bytes{std::istreambuf_iterator<char>{in}, std::istreambuf_iterator<char>{}};
    return bytes;
}

} // namespace ajfx::studio
