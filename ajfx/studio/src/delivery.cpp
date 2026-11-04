#include "delivery.hpp"

#include "document_io.hpp"
#include "performance.hpp"

#include <ajfx/field.hpp>
#include <ajfx/mesh.hpp>
#include <ajfx/package.hpp>

#include <algorithm>
#include <system_error>
#include <utility>

namespace ajfx::studio {

std::filesystem::path cache_path_for(const std::filesystem::path& package) {
    std::filesystem::path path = package;
    path.replace_extension(".ajfxcache");
    return path;
}

Result<Delivery> deliver(const Document& document, const Compiled& compiled, const gpu::KernelSet& kernels,
                         const BakeRecord* bake) {
    if (!compiled.effect) {
        return Error{ErrorCode::InvalidEffect, "There is nothing to deliver: nothing in the scene plays"};
    }
    const Effect& effect = *compiled.effect;
    const std::vector<CacheMode> modes = bake_modes(document, compiled);
    const bool baked = std::any_of(modes.begin(), modes.end(), [](CacheMode m) { return m != CacheMode::Live; });
    if (baked) {
        if (bake == nullptr) {
            return Error{ErrorCode::StaleCache, "The scene's solid things are baked and there is no cache yet: bake "
                                                "it, then deliver"};
        }
        // Studio's own check: the cache is of this scene and this
        // performance, as far as its steps read it.
        const std::shared_ptr<const ActionList> actions =
            document.scene ? performance_actions(document.scene->performance, act_targets(compiled)) : nullptr;
        Result<Cache> fits = Cache::open(bake->bytes, effect, bake->instance_seed, {}, compiled.meshes,
                                         compiled.fields, actions.get());
        if (!fits) {
            return Error{ErrorCode::StaleCache, "The cache is out of date (" + fits.error().message +
                                                    "): bake again, then deliver"};
        }
    }
    std::vector<PackageEntry> extra = gpu::kernel_package_entries(effect, kernels);
    for (PackageEntry& entry : asset_entries(compiled)) {
        extra.push_back(std::move(entry));
    }
    Result<std::vector<std::uint8_t>> package = write_package(effect, std::move(extra));
    if (!package) {
        return package.error();
    }
    // The host's check, from the bytes it will read: the package's effect,
    // meshes and fields, and the cache opened against them as baked.
    Delivery delivery;
    delivery.package = std::move(package).value();
    Result<Effect> read = read_package(delivery.package);
    if (!read) {
        return read.error();
    }
    delivery.effect = std::move(read).value();
    Result<MeshSet> meshes = read_package_meshes(delivery.package, delivery.effect);
    if (!meshes) {
        return meshes.error();
    }
    delivery.meshes = std::move(meshes).value();
    Result<FieldSet> fields = read_package_fields(delivery.package, delivery.effect);
    if (!fields) {
        return fields.error();
    }
    delivery.fields = std::move(fields).value();
    if (baked) {
        Result<Cache> played = Cache::open_as_baked(bake->bytes, delivery.effect, delivery.meshes, delivery.fields);
        if (!played) {
            return Error{ErrorCode::StaleCache,
                         "The host would refuse the cache: " + played.error().message + ". Bake again, then deliver"};
        }
        delivery.cache = bake->bytes;
        delivery.played = std::make_shared<const Cache>(std::move(played).value());
    }
    return delivery;
}

bool write_delivery(const Delivery& delivery, const std::filesystem::path& path, std::string& error) {
    const std::filesystem::path cache = cache_path_for(path);
    if (!delivery.cache.empty()) {
        if (!write_file(cache, delivery.cache, error)) {
            return false;
        }
    } else {
        // A cache left from an earlier delivery would be paired with this
        // package by the host, and refused or, worse, played.
        std::error_code ignored;
        std::filesystem::remove(cache, ignored);
    }
    return write_file(path, delivery.package, error);
}

} // namespace ajfx::studio
