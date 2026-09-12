#include <mediaserver/render/linked_scene_ingest.hpp>
#include <mediaserver/scene/linked_light.hpp>
#include <mediaserver/render/environment_budget.hpp>
#include <mediaserver/assets/content_fingerprint.hpp>
#include <mediaserver/scene/scene_schema.hpp>
#include <mediaserver/scene/instance_batch.hpp>
#include <cgltf.h>
#include <stb_image.h>
#include <algorithm>
#include <cmath>
#include <tuple>
#include <utility>
#include <condition_variable>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <set>
#include <sstream>
#include <thread>

namespace mediaserver::render {
namespace {
namespace ls = scene::link_schema;
namespace ss = scene::schema;
namespace fs = std::filesystem;
std::string value(const doc::Node& n, std::string_view key) {
    const auto it = n.properties.find(key);
    if (it == n.properties.end()) return {};
    const auto* p = std::get_if<std::string>(&it->second);
    return p ? *p : std::string{};
}
bool owned(const doc::Node& n, doc::NodeId source) {
    const auto it = n.properties.find(ls::source);
    return it != n.properties.end() && it->second == doc::PropertyValue{static_cast<std::int64_t>(source.value())};
}
void require(bool ok, const char* message) { if (!ok) throw std::runtime_error{message}; }
void cancelled(std::stop_token stop) { require(!stop.stop_requested(), "Linked scene import cancelled"); }
// Validate before loader allocations and forbid dependencies outside the hashed
// payload. Prototype node transforms must be identity: placement is manifest-owned.
void qualify_glb(const fs::path& path) {
    cgltf_options options{};
    cgltf_data* raw{};
    require(cgltf_parse_file(&options, path.string().c_str(), &raw) == cgltf_result_success, "Invalid GLB payload");
    const std::unique_ptr<cgltf_data, decltype(&cgltf_free)> data{raw, cgltf_free};
    require(data->file_type == cgltf_file_type_glb && data->cameras_count == 0 && data->lights_count == 0,
        "Prototype must be embedded GLB without reference cameras or lights");
    const bool animated=data->animations_count != 0 || data->skins_count != 0;
    require(data->animations_count <= 1 && data->skins_count <= 32,"Animation assembly supports one clip and at most 32 skins");
    require(data->buffers_count == 1 && !data->buffers[0].uri && data->buffers[0].size <= 64U*1024U*1024U,
        "External or oversized prototype buffer");
    require(data->images_count <= 32 && data->meshes_count <= 64 && data->nodes_count <= 128, "Prototype capacity exceeded");
    for (std::size_t i = 0; i < data->images_count; ++i)
        require(!data->images[i].uri && data->images[i].buffer_view, "Textures must be embedded in the immutable GLB");
    require(cgltf_load_buffers(&options, data.get(), path.string().c_str()) == cgltf_result_success
        && cgltf_validate(data.get()) == cgltf_result_success, "Invalid GLB buffer references");
    for (std::size_t i=0;i<data->animations_count;++i) {
        const auto& animation=data->animations[i];
        for (std::size_t j=0;j<animation.channels_count;++j) {
            const auto& channel=animation.channels[j];
            require(channel.sampler && channel.sampler->interpolation==cgltf_interpolation_type_linear,
                "Linked animation requires explicitly sampled LINEAR node/skin channels");
            if (channel.target_path==cgltf_animation_path_type_scale) {
                for (std::size_t k=0;k<channel.sampler->output->count;++k) {
                    float scale[3]{};
                    require(cgltf_accessor_read_float(channel.sampler->output,k,scale,3)
                        && scale[0]>1e-6F && scale[1]>1e-6F && scale[2]>1e-6F,"Reflected or singular animated scale is not qualified");
                }
            }
        }
    }
    std::uint64_t pixels{};
    for (std::size_t i = 0; i < data->images_count; ++i) {
        const auto* view = data->images[i].buffer_view;
        require(view->size <= 64U*1024U*1024U, "Texture input capacity exceeded");
        const auto* bytes = cgltf_buffer_view_data(view);
        int width{}, height{}, components{};
        require(bytes && stbi_info_from_memory(bytes, static_cast<int>(view->size), &width, &height, &components)
            && width > 0 && height > 0 && width <= 4096 && height <= 4096, "Unsupported or oversized texture");
        pixels += static_cast<std::uint64_t>(width)*static_cast<std::uint64_t>(height);
        require(pixels <= 16U*1024U*1024U, "Decoded texture scratch capacity exceeded");
        auto* decoded = stbi_load_from_memory(bytes, static_cast<int>(view->size), &width, &height, &components, 4);
        require(decoded != nullptr, "Embedded texture cannot be decoded");
        stbi_image_free(decoded);
    }
    require(pixels <= 16U*1024U*1024U, "Decoded texture scratch capacity exceeded");
    std::size_t elements{};
    for (std::size_t i = 0; i < data->accessors_count; ++i) {
        require(data->accessors[i].count <= 1'000'000, "Prototype accessor capacity exceeded");
        elements += data->accessors[i].count;
    }
    require(elements <= 8'000'000, "Prototype scratch capacity exceeded");
    std::size_t drawn_elements{};
    for (std::size_t i = 0; i < data->nodes_count; ++i) {
        if (data->nodes[i].mesh) for (std::size_t p = 0; p < data->nodes[i].mesh->primitives_count; ++p) {
            const auto& primitive = data->nodes[i].mesh->primitives[p];
            require(primitive.type == cgltf_primitive_type_triangles && primitive.targets_count == 0,
                "Prototype requires evaluated triangles without morph targets");
            if (primitive.indices) drawn_elements += primitive.indices->count;
            for (std::size_t a = 0; a < primitive.attributes_count; ++a)
                if (primitive.attributes[a].type == cgltf_attribute_type_position) drawn_elements += primitive.attributes[a].data->count;
        }
        require(drawn_elements <= 2'000'000, "Expanded prototype geometry exceeds scratch budget");
        float matrix[16]{}; cgltf_node_transform_local(&data->nodes[i], matrix);
        if (animated) {
            float world[16]{};cgltf_node_transform_world(&data->nodes[i],world);
            const auto determinant=scene::dot(scene::Vec3{world[0],world[1],world[2]},
                scene::cross({world[4],world[5],world[6]},{world[8],world[9],world[10]}));
            require(determinant>1e-10F,"Reflected or singular animated bind transform is not qualified");
        }
        for (std::size_t j = 0; j < 16; ++j) {
            require(std::isfinite(matrix[j]),"Nonfinite prototype node transform");
            if (!animated) require(matrix[j] == (j % 5 == 0 ? 1.0F : 0.0F), "Prototype placement must be identity; put transforms in the manifest");
        }
    }
}
}

LinkedIngestResult ingest_linked_scene(const std::string& path, std::stop_token stop) {
    LinkedIngestResult result;
    try {
        const auto file = fs::absolute(path);
        require(fs::file_size(file) <= 1024U*1024U, "Manifest exceeds 1 MiB");
        std::ifstream input{file};
        std::string magic; unsigned version{};
        input >> magic >> version >> std::quoted(result.manifest.source_uuid) >> std::quoted(result.manifest.revision);
        require(input.good() && magic == "MSLINK" && version == 1 && !result.manifest.revision.empty(), "Invalid MSLINK 1 header");
        std::string line; std::getline(input, line);
        require(line.find_first_not_of(" \t\r") == std::string::npos, "Unexpected header fields");
        std::set<std::pair<std::string,std::string>> ids;
        while (std::getline(input, line)) {
            cancelled(stop);
            if (line.empty() || line[0] == '#') continue;
            std::istringstream row{line}; std::string kind, uuid;
            row >> kind >> std::quoted(uuid);
            LinkedManifestRecord record;
            auto& n = record.node; n.type = "link." + kind;
            n.properties[std::string{ls::uuid}] = uuid;
            require(ids.emplace(kind, uuid).second, "Duplicate manifest identity");
            if (kind == "prototype") {
                std::string hash; row >> std::quoted(hash) >> std::quoted(record.payload);
                require(assets::sha256_from_hex(hash).has_value(), "Prototype revision must be SHA-256");
                n.properties[std::string{ls::revision}] = hash;
                n.properties["link_resource"] = "sha256:" + hash;
                n.properties[std::string{ss::source}] = "sha256:" + hash;
                if (!row.eof()) row >> std::ws;
                if (!row.eof()) {
                    std::string tag; row >> tag >> record.lod_pixels >> std::quoted(record.lod_hash) >> std::quoted(record.lod_payload);
                    require(tag == "lod" && std::isfinite(record.lod_pixels) && record.lod_pixels > 0
                        && assets::sha256_from_hex(record.lod_hash).has_value(), "Invalid authored LOD");
                }
            } else if (kind == "object") {
                std::string name, origin; int visible{};
                row >> std::quoted(name) >> std::quoted(record.parent) >> std::quoted(record.prototype)
                    >> std::quoted(record.material) >> std::quoted(origin) >> visible;
                require(visible == 0 || visible == 1, "Invalid visibility");
                n.properties[std::string{ss::group_name}] = name;
                n.properties[std::string{ls::origin}] = origin;
                n.properties[std::string{ss::visible}] = visible == 1;
                scene::Mat4 matrix{}; for (auto& v : matrix.m) row >> v;
                require(scene::valid_link_affine(matrix), "Invalid local affine matrix");
                scene::write_link_matrix(n, ls::local, matrix);
                if (!row.eof()) row >> std::ws;
                if (!row.eof()) {
                    std::string tag; row >> tag;
                    if (tag == "light") {
                        require(record.prototype.empty(), "Light cannot reference a mesh prototype");
                        std::string type, unit; row >> std::quoted(type) >> std::quoted(unit);
                        n.properties[std::string{ss::light_kind}]=type;
                        n.properties[std::string{ss::light_unit}]=unit;
                        for (auto key : scene::linked_light_fields) { double v{}; row >> v; n.properties[std::string{key}]=v; }
                    } else if (tag == "animation") {
                        core::i64 start{}; int loop{}; row >> start >> loop;
                        require(start >= 0 && (loop == 0 || loop == 1) && !record.prototype.empty(),"Invalid animation start/loop or missing assembly prototype");
                        n.properties[std::string{ss::animation_enabled}]=true;
                        n.properties[std::string{ss::animation_start_frame}]=start;
                        n.properties[std::string{ss::animation_loop}]=loop == 1;
                    } else {
                        std::string batch; row >> std::quoted(batch);
                        std::vector<scene::Mat4> transforms;
                        require(tag == "batch" && scene::decode_instance_batch(batch,transforms), "Invalid compact instance batch");
                        require(!record.prototype.empty(), "Instance batch requires a prototype");
                        n.properties[std::string{scene::instance_batch_property}] = std::move(batch);
                    }
                }
            } else if (kind == "material") {
                std::string revision; row >> std::quoted(revision);
                n.properties[std::string{ls::revision}] = revision;
                for (const auto key : {ss::color_r, ss::color_g, ss::color_b, ss::roughness, ss::metallic, ss::emissive_r, ss::emissive_g, ss::emissive_b}) {
                    double v{}; row >> v; n.properties[std::string{key}] = v;
                }
            } else throw std::runtime_error{"Unknown manifest record"};
            require(!row.fail(), "Missing or invalid manifest fields");
            if (!row.eof()) row >> std::ws;
            require(row.eof(), "Unexpected manifest fields");
            result.manifest.records.push_back(std::move(record));
            require(result.manifest.records.size() <= 1024, "Manifest record capacity exceeded");
        }
        // Validate the entire graph before decoding even the first prototype.
        doc::Root validation;
        validation = validation.set({doc::NodeId{1}, std::string{ss::stage_type}, {}});
        auto checked = plan_linked_manifest(validation, result.manifest, doc::NodeId{1});
        require(static_cast<bool>(checked), checked ? "" : checked.error().message.c_str());
        std::size_t prototypes{}; std::uintmax_t bytes{};
        for (const auto& r : result.manifest.records) if (r.node.type == ls::prototype_type) {
            require(++prototypes <= 64, "Prototype count exceeds 64");
            for (const auto& payload : {r.payload,r.lod_payload}) {
            if (payload.empty()) continue;
            const fs::path relative{payload};
            require(!relative.empty() && relative.is_relative() && relative.extension() == ".glb", "Expected relative GLB payload path");
            for (const auto& part : relative) require(part != "..", "Payload path cannot escape manifest directory");
            if (!fs::is_regular_file(file.parent_path() / relative))
                throw std::runtime_error{"Missing prototype resource: " + r.payload};
            const auto size = fs::file_size(file.parent_path() / relative);
            bytes += size;
            require(size <= 64U*1024U*1024U && bytes <= 256U*1024U*1024U, "Pending payload byte capacity exceeded");
            }
        }
        const auto cache = file.parent_path() / ".mslink-cache-v1";
        std::uintmax_t package_bytes{};
        std::set<std::string> required_packages;
        for (auto& r : result.manifest.records) if (r.node.type == ls::prototype_type) {
            cancelled(stop);
            const auto source = file.parent_path() / r.payload;
            const auto hash = value(r.node, ls::revision);
            const auto fingerprint = assets::content_fingerprint(source.string(), stop);
            require(fingerprint && assets::sha256_hex(fingerprint.value().sha256) == hash, "Missing or hash-mismatched prototype resource");
            fs::create_directories(cache);
            std::string key = hash;
            if (!r.lod_hash.empty()) {
                const auto low = assets::content_fingerprint((file.parent_path()/r.lod_payload).string(),stop);
                require(low && assets::sha256_hex(low.value().sha256) == r.lod_hash,"Authored LOD resource hash mismatch");
                const auto identity = hash + r.lod_hash + std::to_string(r.lod_pixels);
                key = assets::sha256_hex(assets::sha256_digest({reinterpret_cast<const core::u8*>(identity.data()),identity.size()}));
                r.node.properties[std::string{ls::revision}] = key;
                r.node.properties["link_resource"] = "sha256:" + key;
            }
            const auto frozen = cache / (key + ".glb");
            const auto index = cache / (key + ".package");
            std::string package, package_hash;
            std::ifstream cached{index}; std::getline(cached, package); std::getline(cached, package_hash);
            // The index stores only a filename, never a machine's absolute path.
            if (!package.empty() && !package_hash.empty() && fs::path{package}.filename() == fs::path{package}
                && fs::is_regular_file(cache / package)) {
                ++result.manifest.reused;
            } else {
                std::uintmax_t retained_bytes{}; std::size_t retained_packages{};
                for (const auto& entry : fs::directory_iterator(cache)) if (entry.is_regular_file()) {
                    retained_bytes += entry.file_size();
                    if (entry.path().extension() == ".package") ++retained_packages;
                }
                require(retained_packages < 128 && retained_bytes < 1536ULL*1024ULL*1024ULL,
                    "Linked resource cache is full (128 generations / 1.5 GiB); use a new manifest directory");
                if (!fs::exists(frozen)) fs::copy_file(source, frozen);
                const auto frozen_hash = assets::content_fingerprint(frozen.string(), stop);
                require(frozen_hash && assets::sha256_hex(frozen_hash.value().sha256) == hash, "Immutable payload cache mismatch");
                qualify_glb(frozen);
                if (!r.lod_hash.empty()) {
                    const auto low = cache/(r.lod_hash+"-lod.glb");
                    if (!fs::exists(low)) fs::copy_file(file.parent_path()/r.lod_payload,low);
                    const auto verified_low = assets::content_fingerprint(low.string(),stop);
                    require(verified_low && assets::sha256_hex(verified_low.value().sha256) == r.lod_hash,"Immutable LOD cache mismatch");
                    qualify_glb(low);
                    std::ofstream companion{frozen.string()+".lod",std::ios::trunc};
                    companion << "MSLOD 1\n" << std::setprecision(9) << r.lod_pixels << ' ' << std::quoted(low.filename().string()) << '\n';
                    companion.close(); require(companion.good(),"Cannot stage authored LOD chain");
                }
                cancelled(stop);
                auto ingested = ingest_model(frozen.string());
                require(ingested.diagnostic.empty() && !ingested.source.empty(), ingested.diagnostic.c_str());
                package = fs::path{ingested.source}.filename().string();
                cancelled(stop);
                const auto package_fingerprint = assets::content_fingerprint((cache / package).string(), stop);
                require(static_cast<bool>(package_fingerprint), "Cannot fingerprint immutable package");
                package_hash = assets::sha256_hex(package_fingerprint.value().sha256);
                std::ofstream write{index, std::ios::trunc}; write << package << '\n' << package_hash << '\n';
                require(write.good(), "Cannot write local package index");
                ++result.manifest.ingested;
            }
            const auto verified = assets::content_fingerprint((cache / package).string(), stop);
            require(verified && assets::sha256_hex(verified.value().sha256) == package_hash,
                "Immutable model package is missing or corrupt");
            if (required_packages.insert(package).second) package_bytes += verified.value().byte_length;
            require(package_bytes <= 256U*1024U*1024U, "Candidate package/upload bytes exceed 256 MiB");
            r.node.properties[std::string{ss::source}] = (cache / package).string();
        }
        cancelled(stop);
        auto candidate=plan_linked_manifest(validation,result.manifest,doc::NodeId{1});
        require(static_cast<bool>(candidate),candidate ? "" : candidate.error().message.c_str());
        const auto budget=measure_environment(candidate.value(),doc::NodeId{1},stop);
        require(budget.diagnostic.empty(),budget.diagnostic.c_str());
    } catch (const std::exception& e) { result.diagnostic = e.what(); result.manifest = {}; }
    return result;
}

scene::LinkedPlanResult plan_linked_manifest(const doc::Root& root, const LinkedManifest& manifest, scene::SceneId target) {
    const auto fail = [](std::string text) { return scene::LinkedPlanResult::failure({{}, std::move(text)}); };
    const auto all = doc::diff({}, root);
    core::u64 next = 1;
    doc::NodeId source{};
    for (const auto& c : all.changes) {
        next = std::max(next, c.id.value()+1);
        if (c.after->type == ls::source_type && value(*c.after, ls::uuid) == manifest.source_uuid) source = c.id;
    }
    if (!root.get(target) || root.get(target)->type != ss::stage_type) return fail("Target scene was deleted");
    for (const auto& c : all.changes) if (c.after->type == ls::instance_type && owned(*c.after,source)) {
        const auto paused = c.after->properties.find(ls::paused);
        if (paused != c.after->properties.end() && paused->second == doc::PropertyValue{true})
            return fail("source_paused: resume_the_collection_then_publish");
    }
    if (!source.value()) source = doc::NodeId{next++};
    std::map<std::pair<std::string,std::string>,doc::NodeId> identities;
    for (const auto& c : all.changes) if (owned(*c.after, source) && c.after->type != ls::instance_type)
        identities.emplace(std::pair{c.after->type, value(*c.after, ls::uuid)}, c.id);
    std::set<doc::NodeId> present;
    for (const auto& r : manifest.records) {
        auto [it, added] = identities.emplace(std::pair{r.node.type, value(r.node, ls::uuid)}, doc::NodeId{next});
        if (added) ++next;
        if (!present.insert(it->second).second) return fail("Duplicate manifest identity");
    }
    doc::Node src{source, std::string{ls::source_type}, {}};
    src.properties[std::string{ls::uuid}] = manifest.source_uuid;
    src.properties[std::string{ls::revision}] = manifest.revision;
    src.properties[std::string{ls::version}] = std::int64_t{1};
    if (!manifest.collection_name.empty()) src.properties[std::string{ss::group_name}] = manifest.collection_name;
    else if (const auto* old = root.get(source); old && old->properties.contains(ss::group_name))
        src.properties[std::string{ss::group_name}] = old->properties.at(std::string{ss::group_name});
    std::vector<doc::Node> records{src};
    for (const auto& r : manifest.records) {
        auto n = r.node;
        n.id = identities.at({n.type, value(n, ls::uuid)});
        n.properties[std::string{ls::source}] = static_cast<std::int64_t>(source.value());
        for (const auto& [key, type, uuid] : {std::tuple{ls::parent,ls::object_type,r.parent},
             std::tuple{ls::prototype,ls::prototype_type,r.prototype}, std::tuple{ls::material,ls::material_type,r.material}}) {
            if (uuid.empty()) continue;
            const auto it = identities.find({std::string{type},uuid});
            if (it == identities.end() || !present.contains(it->second)) return fail("Missing manifest hierarchy or asset reference");
            n.properties[std::string{key}] = static_cast<std::int64_t>(it->second.value());
        }
        records.push_back(std::move(n));
    }
    for (const auto& c : all.changes) if (owned(*c.after, source) && c.after->type == ls::object_type && !present.contains(c.id)) {
        auto removed = *c.after; removed.properties[std::string{ls::removed}] = true; records.push_back(std::move(removed));
    }
    auto candidate = scene::plan_link_source_update(root, records, doc::NodeId{next});
    if (!candidate) return candidate;
    for (const auto& c : all.changes) if (c.after->type == ls::instance_type && owned(*c.after, source)) {
        const auto it = c.after->properties.find(ss::scene_id);
        if (it != c.after->properties.end() && it->second == doc::PropertyValue{static_cast<std::int64_t>(target.value())}) return candidate;
    }
    for (const auto& c : doc::diff({}, candidate.value()).changes) next = std::max(next,c.id.value()+1);
    return scene::plan_attach_link(candidate.value(), source, target, doc::NodeId{next});
}

struct LinkedSceneIngest::Impl {
    std::mutex mutex;
    std::condition_variable_any changed;
    std::string pending;
    std::stop_source cancellation;
    core::u64 generation{};
    std::shared_ptr<const LinkedIngestResult> complete;
    std::jthread worker{[this](std::stop_token shutdown) {
        while (!shutdown.stop_requested()) {
            std::string path; core::u64 ticket{}; std::stop_token stop;
            {
                std::unique_lock lock{mutex};
                changed.wait(lock, shutdown, [this] { return !pending.empty(); });
                if (shutdown.stop_requested()) break;
                path = std::move(pending); pending.clear(); ticket = generation; stop = cancellation.get_token();
            }
            auto result = std::make_shared<LinkedIngestResult>(ingest_linked_scene(path, stop));
            std::lock_guard lock{mutex};
            if (ticket == generation && !stop.stop_requested()) complete = std::move(result);
        }
    }};
};
LinkedSceneIngest::LinkedSceneIngest() : impl_{std::make_unique<Impl>()} {}
LinkedSceneIngest::~LinkedSceneIngest() { cancel(); }
void LinkedSceneIngest::submit(std::string path) {
    std::lock_guard lock{impl_->mutex};
    impl_->cancellation.request_stop(); impl_->cancellation = std::stop_source{};
    ++impl_->generation; impl_->complete.reset(); impl_->pending = std::move(path); impl_->changed.notify_one();
}
void LinkedSceneIngest::cancel() {
    std::lock_guard lock{impl_->mutex};
    ++impl_->generation; impl_->cancellation.request_stop(); impl_->complete.reset(); impl_->pending.clear();
}
std::shared_ptr<const LinkedIngestResult> LinkedSceneIngest::take() {
    std::unique_lock lock{impl_->mutex, std::try_to_lock};
    if (!lock.owns_lock()) return {};
    return std::exchange(impl_->complete, {});
}
} // namespace mediaserver::render
