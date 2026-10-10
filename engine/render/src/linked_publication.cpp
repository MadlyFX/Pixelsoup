#include <mediaserver/render/linked_publication.hpp>
#include <mediaserver/render/baked_lighting.hpp>
#include <mediaserver/render/environment_budget.hpp>
#include <mediaserver/render/model_package.hpp>
#include <mediaserver/assets/content_fingerprint.hpp>
#include <mediaserver/scene/scene_project.hpp>
#include <mediaserver/scene/scene_model.hpp>
#include <mediaserver/compositor/compositor_model.hpp>
#include <algorithm>
#include <atomic>
#include <bit>
#include <condition_variable>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <map>
#include <mutex>
#include <set>
#include <sstream>
#include <thread>

namespace mediaserver::render {
namespace {
namespace fs = std::filesystem;
namespace pub = scene::publication;
namespace ss = scene::schema;
struct Resource { std::string hash, path, role; core::u64 bytes{}; };
void require(bool ok, const std::string& message) { if (!ok) throw std::runtime_error{message}; }
void cancelled(std::stop_token stop) { require(!stop.stop_requested(), "Publication cancelled"); }
std::string hash_text(const std::string& s) {
    return assets::sha256_hex(assets::sha256_digest({reinterpret_cast<const core::u8*>(s.data()), s.size()}));
}
std::string canonical(const doc::Root& root, const std::set<doc::NodeId>& ids) {
    std::ostringstream out; out.imbue(std::locale::classic());
    for (auto id : ids) if (const auto* n = root.get(id)) {
        out << id.value() << ' ' << std::quoted(n->type) << '\n';
        for (const auto& [k,v] : n->properties) {
            if (k.starts_with("publication_") || k.starts_with("asset_commit_")) continue;
            out << std::quoted(k) << ' ' << v.index() << ' ';
            std::visit([&](const auto& x) {
                using T = std::decay_t<decltype(x)>;
                if constexpr (std::is_same_v<T,std::string>) out << std::quoted(x);
                else if constexpr (std::is_same_v<T,double>) out << std::bit_cast<core::u64>(x);
                else out << x;
            },v);
            out << '\n';
        }
    }
    return out.str();
}
std::vector<Resource> read_manifest(const std::string& path, std::string& revision, core::u64& document) {
    require(fs::is_regular_file(path) && fs::file_size(path) <= 1024*1024, "Publication manifest missing or exceeds 1 MiB: " + path);
    std::ifstream in{path}; std::string magic; unsigned version{};
    in >> magic >> version >> std::quoted(revision) >> document;
    require(in.good() && magic == "MSPUBLICATION" && version == 1 && assets::sha256_from_hex(revision).has_value(), "Invalid publication manifest");
    std::vector<Resource> result;
    std::string line; std::getline(in,line);
    while (std::getline(in,line)) {
        if (line.empty() || line.starts_with('#')) continue;
        Resource r; std::istringstream row{line};
        row >> std::quoted(r.role) >> std::quoted(r.hash) >> r.bytes >> std::quoted(r.path);
        require(!row.fail() && assets::sha256_from_hex(r.hash).has_value() && r.bytes > 0, "Invalid publication resource");
        row >> std::ws; require(row.eof(), "Unexpected publication resource fields");
        require(result.size() < 256, "Publication manifest resource capacity exceeded");
        result.push_back(std::move(r));
    }
    require(in.eof(), "Cannot read complete publication manifest");
    return result;
}
// Atomic, immutable exposure. A crashed writer leaves only an unreferenced temp.
void expose(const fs::path& path, const std::string& bytes, std::stop_token stop) {
    static std::atomic<core::u64> serial{};
    const auto temp = path.string() + ".tmp-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + "-" + std::to_string(++serial);
    try {
        std::ofstream out{temp,std::ios::binary|std::ios::trunc};
        out.write(bytes.data(),static_cast<std::streamsize>(bytes.size())); out.close();
        require(out.good(), "Cannot write publication staging file"); cancelled(stop);
        std::error_code ec; fs::rename(temp,path,ec);
        if (ec) {
            require(fs::is_regular_file(path), "Cannot expose publication file: " + ec.message());
            std::ifstream existing{path,std::ios::binary};
            std::string held{std::istreambuf_iterator<char>{existing},{}};
            require(held == bytes,"Immutable publication path already contains different bytes");
            fs::remove(temp,ec);
        }
    } catch (...) { std::error_code ec; fs::remove(temp,ec); throw; }
}
void verify_resources(const std::vector<Resource>& resources, const PublicationLimits& limits,
    PublicationResult& result, std::stop_token stop) {
    require(resources.size() <= limits.resources*2, "Publication resource capacity refused");
    std::set<std::string> counted;
    for (const auto& r : resources) {
        cancelled(stop);
        require(r.bytes <= limits.bytes && fs::is_regular_file(r.path) && fs::file_size(r.path) == r.bytes,
            "Publication resource missing or size mismatch: " + r.path);
        const auto fingerprint = assets::content_fingerprint(r.path,stop);
        require(fingerprint && fingerprint.value().byte_length == r.bytes
            && assets::sha256_hex(fingerprint.value().sha256) == r.hash, "Publication resource missing or hash mismatch: " + r.path);
        if (!counted.insert(r.hash).second) continue;
        require(r.bytes <= limits.bytes && result.bytes <= limits.bytes-r.bytes, "Publication old/new byte capacity refused");
        result.bytes += r.bytes;
        if (fs::path{r.path}.extension() == ".msmodel") {
            ModelAsset model;
            require(load_model_package(r.path,model), "Invalid immutable model package: " + r.path);
            result.vertices += model.mesh.vertices.size(); result.indices += model.mesh.indices.size();
            for (const auto& t : model.textures) {
                result.texture_bytes += t.rgba.size();
                for (const auto& mip : t.lower_mips) result.texture_bytes += mip.rgba.size();
            }
            require(result.vertices <= limits.vertices && result.indices <= limits.indices,
                "Publication old/new geometry capacity refused");
            require(result.texture_bytes <= limits.texture_bytes, "Publication old/new texture capacity refused");
        }
    }
}
bool file_property(const std::string& key) {
    return key == ss::source || key == ss::environment_map || key == ss::baked_source
        || key == ss::ies_source || key == ss::show_lut || key == ss::camera_inverse_lut
        || key.ends_with("_texture");
}
}

PublicationResult prepare_linked_publication(const PublicationRequest& request, std::stop_token stop) {
    PublicationResult result; result.scene=request.scene; result.document_revision=request.document.version().value();
    try {
        if (request.verify) {
            result.manifest = pub::text(request.document.get(request.scene),pub::manifest);
            require(fs::is_regular_file(result.manifest) && fs::file_size(result.manifest) <= 1024*1024,
                "Publication manifest missing or exceeds 1 MiB");
            const auto fingerprint=assets::content_fingerprint(result.manifest,stop);
            require(fingerprint && assets::sha256_hex(fingerprint.value().sha256) == pub::text(request.document.get(request.scene),pub::manifest_hash),
                "Publication manifest missing or hash mismatch");
            auto resources = read_manifest(result.manifest,result.revision,result.document_revision);
            require(result.revision == pub::text(request.document.get(request.scene),pub::revision), "Publication revision does not match document");
            verify_resources(resources,request.limits,result,stop);
            return result;
        }
        require(!request.storage.empty() && fs::path{request.storage}.is_absolute(), "Set an absolute shared Program storage folder in Scene Link details");
        // Check the authored scene before cache paths are rewritten for shared
        // storage. Frozen Program revisions were checked when created.
        if (pub::text(request.document.get(request.scene),pub::revision).empty()) {
            scene::SceneModel model; model.reset(request.document);
            const auto evaluated=model.evaluate(request.scene,{});
            require(!baked_lighting_stale(evaluated), "Stale GI/reflection probes: rebake in Environment or disable derived lighting before publishing; Program retained");
            const auto& b=evaluated.environment.baked;
            if (b.enabled || std::any_of(b.reflections.begin(),b.reflections.end(),[](const auto& p){return p.enabled;})) {
                const auto asset=load_probe_asset(b.source,b.content);
                require(asset.ready() && asset.asset->input_digest == bake_input_digest(evaluated),
                    "Baked lighting dependency revision does not match this geometry/material/light revision");
            }
        }
        const auto environment=measure_environment(request.document,request.scene,stop);
        require(environment.diagnostic.empty(),environment.diagnostic);
        if (request.program.value() && request.document.get(request.program)
            && pub::text(request.document.get(request.program),pub::revision).empty()) {
            auto retained=std::make_shared<PublicationResult>(prepare_linked_publication(
                {request.document,request.program,{},request.storage,false,request.limits},stop));
            cancelled(stop);
            if (retained->diagnostic.empty()) result.retained=std::move(retained);
            else result.notice="Rollback returns to the live Program scene; it could not be frozen: " + retained->diagnostic;
        }
        const auto directory = fs::path{request.storage}/"ms-publications-v1";
        fs::create_directories(directory);
        core::u64 retained_bytes{}; std::size_t files{};
        for (const auto& entry : fs::directory_iterator(directory)) if (entry.is_regular_file()) {
            retained_bytes += entry.file_size(); ++files;
        }
        require(files < 2048 && retained_bytes < 2ULL*1024*1024*1024, "Program storage capacity refused (2 GiB / 2048 files); select another folder");
        auto frozen = request.document;
        std::size_t published_scenes{};
        core::u64 settings_id{1}; bool has_settings{};
        for (const auto& c : doc::diff({},frozen).changes) {
            settings_id=std::max(settings_id,c.id.value()+1);
            has_settings=has_settings || c.after->type == ss::settings_type;
            if (c.after->type == ss::stage_type && !pub::text(c.after.get(),pub::revision).empty()) ++published_scenes;
        }
        require(published_scenes + (result.retained ? 2U : 1U) <= 16,
            "Publication history capacity refused (16 revisions); remove unneeded inactive revisions");
        if (!has_settings) {
            doc::Node settings{doc::NodeId{settings_id},std::string{ss::settings_type},{}};
            scene::write_renderer_settings(settings,{}); frozen=frozen.set(std::move(settings));
        }
        const auto deletion = scene::plan_delete_scene(frozen,request.scene);
        require(static_cast<bool>(deletion),"Preview scene no longer exists");
        require(deletion.value().erases.size() <= request.limits.nodes,"Publication scene node capacity refused");
        std::set<doc::NodeId> ids{deletion.value().erases.begin(),deletion.value().erases.end()};
        // Source identities enter the revision evidence even though the frozen
        // drawables no longer depend on any shared Blender record.
        std::ostringstream sources;
        for (const auto& c : doc::diff({},frozen).changes) if (c.after->type == scene::link_schema::instance_type
            && pub::integer(c.after.get(),ss::scene_id) == request.scene.value()) {
            const auto* source = frozen.get(doc::NodeId{pub::integer(c.after.get(),scene::link_schema::source)});
            sources << pub::text(source,scene::link_schema::uuid) << ':' << pub::text(source,scene::link_schema::revision) << '\n';
            auto snapshot = scene::snapshot_link(frozen,c.id);
            require(static_cast<bool>(snapshot),snapshot ? "" : snapshot.error().message);
            for (const auto& occurrence : doc::diff({},frozen).changes)
                if (pub::integer(occurrence.after.get(),scene::link_schema::instance) == c.id.value())
                    frozen = *frozen.erase(occurrence.id);
            frozen = *frozen.erase(c.id);
            for (const auto& n : doc::diff({},snapshot.value()).changes) frozen=frozen.set(*n.after);
        }
        // A previously frozen scene already owns its settings. Otherwise bind
        // the current global settings into this new immutable revision.
        const bool scoped_settings=std::any_of(ids.begin(),ids.end(),[&](auto id) {
            const auto* node=frozen.get(id); return node && node->type == pub::settings_type;
        });
        if (!scoped_settings)
            for (const auto& c : doc::diff({},frozen).changes) if (c.after->type == ss::settings_type) { ids.insert(c.id); break; }
        const std::string snapshot = canonical(frozen,ids) + sources.str();
        std::map<std::string,Resource> staged;
        std::set<std::string> in_place;
        core::u64 staged_bytes{};
        for (auto id : ids) if (const auto* n = frozen.get(id)) {
            auto copy=*n;
            // A frozen media file must not be redirected through a mutable
            // Library/proxy identity after its publication has been verified.
            if (copy.type == ss::media_type) copy.properties.erase(std::string{ss::asset_id});
            for (auto& [key,value] : copy.properties) {
                auto* path=std::get_if<std::string>(&value);
                if (!path || path->empty() || !file_property(key)) continue;
                cancelled(stop);
                if (path->find('%') != std::string::npos || path->find('#') != std::string::npos) {
                    // A clip's image sequence is many files that every node
                    // already reads from the project's shared path; it stays
                    // there, like any other project asset, and is checked by
                    // each node's ordinary asset report rather than staged.
                    require(copy.type == ss::media_type && key == ss::source,
                        "Publication requires immutable files; only clip image sequences may stay at their project path");
                    in_place.insert(*path);
                    continue;
                }
                if (copy.type == ss::model_type && key == ss::source)
                    require(fs::path{*path}.extension() == ".msmodel", "Publish ingested .msmodel packages; import native model sources first");
                auto found=staged.find(*path);
                if (found == staged.end()) {
                    require(staged.size() < request.limits.resources,"Publication resource count refused");
                    require(fs::is_regular_file(*path) && fs::file_size(*path) <= request.limits.bytes,"Publication source missing or byte capacity exceeded: " + *path);
                    const auto fingerprint=assets::content_fingerprint(*path,stop);
                    require(static_cast<bool>(fingerprint),"Cannot stage resource: " + *path);
                    const auto bytes=fingerprint.value().byte_length;
                    require(bytes > 0 && bytes <= request.limits.bytes && staged_bytes <= request.limits.bytes-bytes,"Publication staging byte capacity refused");
                    staged_bytes += bytes;
                    const auto hash=assets::sha256_hex(fingerprint.value().sha256);
                    const auto target=directory/(hash+fs::path{*path}.extension().string());
                    std::ifstream in{*path,std::ios::binary};
                    std::string data(static_cast<std::size_t>(bytes),'\0'); in.read(data.data(),static_cast<std::streamsize>(bytes));
                    require(in.good() && hash_text(data) == hash,"Resource changed during publication staging: " + *path);
                    expose(target,data,stop);
                    found=staged.emplace(*path,Resource{hash,target.string(),key,bytes}).first;
                }
                *path=found->second.path;
            }
            frozen=frozen.set(std::move(copy));
        }
        std::vector<Resource> resources;
        std::ostringstream body;
        for (const auto& [source,r] : staged) {
            static_cast<void>(source); resources.push_back(r);
        }
        // Reserve old plus new explicitly. Retained rows from an older manifest
        // are not recursively retained, so the envelope remains two revisions.
        const auto old=result.retained ? result.retained->manifest : pub::text(request.document.get(request.program),pub::manifest);
        if (!old.empty()) {
            std::string rev; core::u64 version{};
            for (auto r : read_manifest(old,rev,version)) if (r.role != "retained") {
                r.role="retained"; resources.push_back(std::move(r));
            }
        }
        verify_resources(resources,request.limits,result,stop);
        if (!in_place.empty()) {
            result.notice += (result.notice.empty() ? "" : " ") + std::to_string(in_place.size())
                + (in_place.size() == 1 ? " image sequence stays at its project path; every node must see it there."
                                        : " image sequences stay at their project paths; every node must see them there.");
        }
        for (const auto& r : resources) body << std::quoted(r.role) << ' ' << std::quoted(r.hash) << ' ' << r.bytes << ' ' << std::quoted(r.path) << '\n';
        result.revision=hash_text(std::to_string(result.document_revision)+"\n"+snapshot+body.str());
        result.manifest=(directory/(result.revision+".mspublication")).string();
        std::ostringstream manifest;
        manifest << "MSPUBLICATION 1 " << std::quoted(result.revision) << ' ' << result.document_revision << '\n' << body.str();
        manifest << "# Source revisions\n";
        std::istringstream source_lines{sources.str()}; std::string line;
        while (std::getline(source_lines,line)) manifest << "# " << line << '\n';
        expose(result.manifest,manifest.str(),stop);
        auto stage=*frozen.get(request.scene);
        stage.properties[std::string{pub::revision}]=result.revision;
        stage.properties[std::string{pub::manifest}]=result.manifest;
        stage.properties[std::string{pub::manifest_hash}]=hash_text(manifest.str());
        stage.properties[std::string{pub::document_revision}]=static_cast<core::i64>(result.document_revision);
        stage.properties[std::string{pub::storage}]=request.storage;
        if (pub::text(&stage,pub::revision).empty() || !stage.properties.contains(pub::source_scene))
            stage.properties[std::string{pub::source_scene}]=static_cast<core::i64>(request.scene.value());
        for (const auto key : {pub::candidate,pub::rollback,pub::previous,ss::asset_commit_digest,ss::asset_commit_control,ss::asset_commit_show}) stage.properties.erase(std::string{key});
        frozen=frozen.set(std::move(stage));
        result.document=std::move(frozen); cancelled(stop);
    } catch (const std::exception& e) { result.diagnostic=e.what(); result.document={}; }
    return result;
}

scene::LinkedPlanResult install_linked_publication(const doc::Root& current, const PublicationResult& prepared,
    doc::NodeId program, doc::NodeId first) {
    const auto fail=[](std::string s) { return scene::LinkedPlanResult::failure({{},std::move(s)}); };
    if (!prepared.diagnostic.empty() || prepared.document.empty() || !current.get(program)) return fail("Publication staging is incomplete");
    if (prepared.retained) {
        auto old=install_linked_publication(current,*prepared.retained,program,first);
        if (!old) return old;
        const auto retained=pub::render_scene(old.value(),program);
        auto candidate=prepared; candidate.retained.reset();
        auto installed=install_linked_publication(old.value(),candidate,program,first);
        if (!installed) return installed;
        auto stage=*installed.value().get(pub::render_scene(installed.value(),program));
        stage.properties[std::string{pub::previous}]=static_cast<core::i64>(retained.value());
        return scene::LinkedPlanResult::success(installed.value().set(std::move(stage)));
    }
    for (const auto& c : doc::diff({},current).changes) first=doc::NodeId{std::max(first.value(),c.id.value()+1)};
    auto clone=scene::plan_clone_scene(prepared.document,prepared.scene,first);
    if (!clone) return fail("Cannot clone publication scene");
    auto result=current; core::u64 next=first.value();
    // Named after the scene it was taken from, so the Program row says what is
    // on air rather than a hash.
    auto source_name=pub::text(prepared.document.get(prepared.scene),ss::scene_name);
    if (source_name.empty()) source_name="Scene";
    for (auto node : clone.value().puts) {
        next=std::max(next,node.id.value()+1);
        node.properties[std::string{ss::locked}]=true;
        if (node.id == clone.value().destination)
            node.properties[std::string{ss::scene_name}]=source_name+" (published "+prepared.revision.substr(0,6)+")";
        result=result.set(std::move(node));
    }
    auto graph=compositor::plan_clone_graph(prepared.document,prepared.scene,clone.value().destination,program,clone.value().remap,doc::NodeId{next});
    if (!graph) return fail("Cannot clone publication compositor");
    for (auto node : graph.value().puts) { next=std::max(next,node.id.value()+1); result=result.set(std::move(node)); }
    const bool scoped_settings=std::any_of(clone.value().puts.begin(),clone.value().puts.end(),[](const auto& n) { return n.type == pub::settings_type; });
    if (!scoped_settings) for (const auto& c : doc::diff({},prepared.document).changes) if (c.after->type == ss::settings_type) {
        auto settings=*c.after; settings.id=doc::NodeId{next++}; settings.type=std::string{pub::settings_type};
        settings.properties[std::string{ss::scene_id}]=static_cast<core::i64>(clone.value().destination.value());
        result=result.set(std::move(settings)); break;
    }
    auto stage=*result.get(program);
    stage.properties[std::string{pub::candidate}]=static_cast<core::i64>(clone.value().destination.value());
    result=result.set(std::move(stage));
    return scene::LinkedPlanResult::success(std::move(result));
}

doc::Root prune_publications(const doc::Root& current, doc::NodeId program, std::span<const doc::NodeId> in_use) {
    std::set<doc::NodeId> keep{in_use.begin(),in_use.end()};
    keep.insert(program);
    const auto* stage=current.get(program);
    for (const auto key : {pub::rollback,pub::candidate,pub::previous}) keep.insert(doc::NodeId{pub::integer(stage,key)});
    const doc::NodeId candidate{pub::integer(stage,pub::candidate)};
    for (const auto key : {pub::rollback,pub::previous}) keep.insert(doc::NodeId{pub::integer(current.get(candidate),key)});
    std::vector<doc::NodeId> stale;
    for (const auto& c : doc::diff({},current).changes)
        if (c.after->type == ss::stage_type && !pub::text(c.after.get(),pub::revision).empty() && !keep.contains(c.id))
            stale.push_back(c.id);
    auto result=current;
    for (const auto id : stale) if (auto deletion=scene::plan_delete_scene(result,id))
        for (const auto erased : deletion.value().erases) if (auto next=result.erase(erased)) result=std::move(*next);
    return result;
}

struct LinkedPublication::Impl {
    std::mutex mutex;
    std::condition_variable_any changed;
    std::optional<PublicationRequest> pending;
    std::stop_source cancellation;
    core::u64 generation{};
    std::shared_ptr<const PublicationResult> complete;
    std::jthread worker{[this](std::stop_token shutdown) {
        while (!shutdown.stop_requested()) {
            PublicationRequest request; core::u64 ticket{}; std::stop_token stop;
            { std::unique_lock lock{mutex}; changed.wait(lock,shutdown,[this]{return pending.has_value();});
              if (shutdown.stop_requested()) return;
              request=std::move(*pending); pending.reset(); ticket=generation; stop=cancellation.get_token(); }
            auto result=std::make_shared<PublicationResult>(prepare_linked_publication(request,stop));
            std::lock_guard lock{mutex};
            if (ticket == generation && !stop.stop_requested()) complete=std::move(result);
        }
    }};
};
LinkedPublication::LinkedPublication():impl_{std::make_unique<Impl>()} {}
LinkedPublication::~LinkedPublication() { cancel(); }
void LinkedPublication::submit(PublicationRequest request) {
    std::lock_guard lock{impl_->mutex}; impl_->cancellation.request_stop(); impl_->cancellation=std::stop_source{};
    ++impl_->generation; impl_->complete.reset(); impl_->pending=std::move(request); impl_->changed.notify_one();
}
void LinkedPublication::cancel() {
    std::lock_guard lock{impl_->mutex}; ++impl_->generation; impl_->cancellation.request_stop(); impl_->pending.reset(); impl_->complete.reset();
}
std::shared_ptr<const PublicationResult> LinkedPublication::take() {
    std::unique_lock lock{impl_->mutex,std::try_to_lock};
    return lock.owns_lock() ? std::exchange(impl_->complete,{}) : nullptr;
}
bool PublicationVerification::observe(const doc::Root& document, doc::NodeId scene) {
    const auto manifest=pub::text(document.get(scene),pub::manifest);
    const auto expected_hash=pub::text(document.get(scene),pub::manifest_hash);
    const auto revision=pub::text(document.get(scene),pub::revision);
    if (scene != scene_ || manifest != manifest_ || expected_hash != manifest_hash_ || revision != revision_) {
        scene_=scene; manifest_=manifest; manifest_hash_=expected_hash; revision_=revision;
        diagnostic_.clear(); ready_=manifest.empty() && revision.empty();
        if (ready_) worker_.cancel();
        else { worker_.submit({document,scene,{}, {},true,{}}); diagnostic_="Verifying complete publication on this node"; }
    }
    if (auto result=worker_.take()) { ready_=result->diagnostic.empty(); diagnostic_=result->diagnostic; }
    return ready_;
}
} // namespace mediaserver::render
