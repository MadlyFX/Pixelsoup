#include <mediaserver/render/linked_scene_ingest.hpp>
#include <mediaserver/assets/content_fingerprint.hpp>
#include <mediaserver/cmd/director_scheduler.hpp>
#include <mediaserver/scene/scene_schema.hpp>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <future>
#include <iostream>
#include <set>
#include <string_view>
#include <thread>
#include <stdexcept>
#include <vector>

using namespace mediaserver;
namespace fs = std::filesystem;
namespace ls = scene::link_schema;
namespace ss = scene::schema;
void check(bool value, const char* message) { if (!value) throw std::runtime_error{message}; }
std::string uuid(core::u64 n) { return scene::link_instance_uuid(doc::NodeId{n}); }
std::string digest(const fs::path& p) {
    auto hash = assets::content_fingerprint(p.string()); check(static_cast<bool>(hash), "fixture hash");
    return assets::sha256_hex(hash.value().sha256);
}
void manifest(const fs::path& file, bool revision2, float placement = 0) {
    const auto dir = file.parent_path();
    std::ofstream out{file};
    out << "MSLINK 1 \"" << uuid(10) << "\" \"" << (revision2 ? "r2" : "r1") << "\"\n";
    out << "prototype \"" << uuid(11) << "\" \"" << digest(dir / (revision2 ? "r2.glb" : "r1.glb"))
        << "\" \"" << (revision2 ? "r2.glb" : "r1.glb") << "\"\n";
    out << "prototype \"" << uuid(12) << "\" \"" << digest(dir / "r1.glb") << "\" \"r1.glb\"\n";
    for (core::u64 i=0; i<3; ++i) {
        out << "object \"" << uuid(20+i) << "\" \"Prop " << i << "\" \"\" \"" << uuid(i==2 ? 12 : 11)
            << "\" \"\" \"origin-v1\" 1";
        auto matrix = scene::translation({static_cast<float>(i)*2+placement,0,0});
        for (auto x : matrix.m) out << ' ' << x;
        out << '\n';
    }
}
// A Director store manifest: payloads are named by their hash, as on the wire.
void store_manifest(const fs::path& file, const std::string& revision, const std::vector<std::string>& hashes) {
    std::ofstream out{file};
    out << "MSLINK 1 \"" << uuid(10) << "\" \"" << revision << "\"\n";
    for (std::size_t i=0; i<hashes.size(); ++i)
        out << "prototype \"" << uuid(11+i) << "\" \"" << hashes[i] << "\" \"" << hashes[i] << ".glb\"\n";
    for (std::size_t i=0; i<hashes.size(); ++i) {
        out << "object \"" << uuid(20+i) << "\" \"Prop " << i << "\" \"\" \"" << uuid(11+i) << "\" \"\" \"origin-v1\" 1";
        for (auto x : scene::translation({static_cast<float>(i)*2,0,0}).m) out << ' ' << x;
        out << '\n';
    }
}
std::size_t count_files(const fs::path& directory, std::string_view extension) {
    std::size_t count{};
    for (const auto& entry : fs::directory_iterator(directory)) count += entry.path().extension() == extension;
    return count;
}
std::set<std::string> packages(const render::LinkedManifest& manifest) {
    std::set<std::string> result;
    for (const auto& r : manifest.records) if (r.node.type == ls::prototype_type)
        result.insert(std::get<std::string>(r.node.properties.at(std::string{ss::source})));
    return result;
}
int main(int argc, char** argv) {
    try {
        check(argc == 2, "fixture directory argument");
        const auto dir = fs::temp_directory_path() / ("mslink-test-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
        fs::create_directories(dir);
        fs::copy_file(fs::path{argv[1]}/"prop-r1.glb",dir/"r1.glb");
        fs::copy_file(fs::path{argv[1]}/"prop-r2.glb",dir/"r2.glb");
        manifest(dir/"r1.mslink",false); manifest(dir/"r2.mslink",true); manifest(dir/"move.mslink",true,1);
        auto r1 = render::ingest_linked_scene((dir/"r1.mslink").string());
        if (!r1.diagnostic.empty()) throw std::runtime_error{r1.diagnostic};
        check(r1.manifest.ingested==1 && r1.manifest.reused==1,"shared prototype reused");
        doc::Root root; root=root.set({doc::NodeId{1},std::string{ss::stage_type},{}});
        auto p1=render::plan_linked_manifest(root,r1.manifest,doc::NodeId{1}); check(static_cast<bool>(p1),"first import"); root=p1.value();
        auto state=scene::read_linked_scene(root); check(state && state.value().occurrences.size()==3,"complete hierarchy");
        const auto hero=state.value().occurrences.begin()->first;
        auto held=scene::plan_linked_world(root,hero,scene::translation({7,1,2})); check(static_cast<bool>(held),"shot edit"); root=held.value();
        const auto before=scene::read_linked_scene(root).value().occurrences;
        auto r2=render::ingest_linked_scene((dir/"r2.mslink").string());
        check(r2.diagnostic.empty() && r2.manifest.ingested==1 && r2.manifest.reused==1,"only changed geometry ingested");
        auto p2=render::plan_linked_manifest(root,r2.manifest,doc::NodeId{1}); check(static_cast<bool>(p2),"revision update"); root=p2.value();
        cmd::DirectorScheduler director;
        check(static_cast<bool>(director.schedule_document_replace(p1.value(),cmd::UndoContextId{2},cmd::TransactionId{1},cmd::FrameNumber{1})),"Director imports entire hierarchy");
        check(static_cast<bool>(director.schedule_document_replace(held.value(),cmd::UndoContextId{2},cmd::TransactionId{2},cmd::FrameNumber{1})),"Director accepts intervening shot edit");
        check(static_cast<bool>(director.schedule_document_replace(root,cmd::UndoContextId{2},cmd::TransactionId{3},cmd::FrameNumber{1})),"Director accepts revision with fresh transaction");
        check(static_cast<bool>(director.schedule_undo(cmd::UndoContextId{2},cmd::TransactionId{4},cmd::FrameNumber{1})),"revision is one undo step");
        check(doc::diff(director.projected_document(),held.value()).changes.empty(),"undo revision preserves intervening placement");
        const auto after=scene::read_linked_scene(root).value().occurrences;
        for (const auto& [id,old] : before) check(old.world==after.at(id).world,"shot placement survived mesh update");
        check(before.rbegin()->second.source==after.rbegin()->second.source,"unaffected package identity unchanged");
        const auto count=render::model_texture_compression_count();
        auto moved=render::ingest_linked_scene((dir/"move.mslink").string());
        check(moved.diagnostic.empty() && moved.manifest.ingested==0 && moved.manifest.reused==2,"placement performs no geometry ingestion");
        check(count==render::model_texture_compression_count(),"placement performs no texture compression");
        auto invalid=r2.manifest; invalid.records.back().parent=uuid(999);
        check(!render::plan_linked_manifest(root,invalid,doc::NodeId{1}),"missing parent rejected atomically");
        invalid=r2.manifest; invalid.records.push_back(invalid.records.back());
        check(!render::plan_linked_manifest(root,invalid,doc::NodeId{1}),"duplicate identity rejected");
        invalid=r2.manifest; invalid.records.back().parent=std::get<std::string>(invalid.records.back().node.properties.at(std::string{ls::uuid}));
        check(!render::plan_linked_manifest(root,invalid,doc::NodeId{1}),"cycle rejected");
        std::ofstream{dir/"bad.mslink"} << "MSLINK 1 \"bad\" \"r1\"\n";
        check(!render::ingest_linked_scene((dir/"bad.mslink").string()).diagnostic.empty(),"malformed manifest refused");
        fs::rename(dir/"r2.glb",dir/"missing.glb");
        check(!render::ingest_linked_scene((dir/"r2.mslink").string()).diagnostic.empty(),"missing resource refused even with cache");
        fs::rename(dir/"missing.glb",dir/"r2.glb");
        std::stop_source stop; stop.request_stop();
        check(!render::ingest_linked_scene((dir/"r2.mslink").string(),stop.get_token()).diagnostic.empty(),"cancelled ingestion refused");
        render::LinkedSceneIngest queue;
        queue.submit((dir/"r1.mslink").string()); queue.submit((dir/"r2.mslink").string());
        std::shared_ptr<const render::LinkedIngestResult> latest;
        for (int i=0; i<1000 && !latest; ++i) { latest=queue.take(); std::this_thread::sleep_for(std::chrono::milliseconds{2}); }
        check(latest && latest->diagnostic.empty() && latest->manifest.revision=="r2","superseded completion cannot publish");
        queue.submit((dir/"r1.mslink").string()); queue.cancel();
        std::this_thread::sleep_for(std::chrono::milliseconds{20}); check(!queue.take(),"cancel clears completion");
        render::ModelAsset textured; render::ModelTexture image; image.width=4; image.height=4; image.rgba.assign(64,127);
        textured.textures.push_back(image); render::ModelPart part; part.base_color_texture=0; textured.parts.push_back(part);
        auto second=textured; check(render::compress_model_textures(textured),"first texture compression");
        const auto compressed=render::model_texture_compression_count();
        check(render::compress_model_textures(second) && compressed==render::model_texture_compression_count(),"independent unchanged texture reused");
        check(textured.textures==second.textures,"texture cache preserves bytes and mips");
        const auto package = std::get<std::string>(r2.manifest.records.front().node.properties.at(std::string{ss::source}));
        std::ofstream{package, std::ios::binary | std::ios::trunc} << "corrupt";
        check(!render::ingest_linked_scene((dir/"r2.mslink").string()).diagnostic.empty(),"corrupt cached package rejected before commit");
        // A content-addressed store is its own cache: each stored resource is
        // its own frozen copy, and manifests ingested at the same time (one per
        // linked scene) serialize on one package index, building each package once.
        const auto store = dir / "store";
        fs::create_directories(store);
        const auto h1 = digest(dir/"r1.glb"), h2 = digest(dir/"r2.glb");
        fs::copy_file(dir/"r1.glb", store/(h1+".glb")); fs::copy_file(dir/"r2.glb", store/(h2+".glb"));
        store_manifest(store/"a.mslink", "a", {h1,h2}); store_manifest(store/"b.mslink", "b", {h1,h2});
        const render::LinkedIngestCache shared{true, 4096};
        auto link_a = std::async(std::launch::async, [&] { return render::ingest_linked_scene((store/"a.mslink").string(), {}, shared); });
        auto link_b = std::async(std::launch::async, [&] { return render::ingest_linked_scene((store/"b.mslink").string(), {}, shared); });
        const auto a = link_a.get(), b = link_b.get();
        check(a.diagnostic.empty() && b.diagnostic.empty(), "concurrent shared-cache ingests");
        check(a.manifest.ingested+b.manifest.ingested == 2 && a.manifest.reused+b.manifest.reused == 2,
            "serialized ingest builds each shared package once");
        check(packages(a.manifest) == packages(b.manifest) && packages(a.manifest).size() == 2, "both scenes reference the same packages");
        check(!fs::exists(store/".mslink-cache-v1") && count_files(store,".glb") == 2, "stored resources are their own frozen copies");
        check(count_files(store,".package") == 2 && count_files(store,".tmp") == 0, "indexes appear whole, by rename");
        const auto relinked = render::ingest_linked_scene((store/"a.mslink").string(), {}, shared);
        check(relinked.diagnostic.empty() && relinked.manifest.ingested == 0 && relinked.manifest.reused == 2
            && packages(relinked.manifest) == packages(a.manifest), "re-linking reuses the shared index");
        // At its limit a shared cache still serves indexed content; only a new
        // package generation is refused, by name.
        const render::LinkedIngestCache full{true, 2};
        check(render::ingest_linked_scene((store/"a.mslink").string(), {}, full).manifest.reused == 2,
            "a full shared cache still serves indexed content");
        const auto h3 = digest(fs::path{argv[1]}/"workload-256.glb");
        fs::copy_file(fs::path{argv[1]}/"workload-256.glb", store/(h3+".glb"));
        store_manifest(store/"c.mslink", "c", {h1,h3});
        check(render::ingest_linked_scene((store/"c.mslink").string(), {}, full).diagnostic.starts_with("Linked resource cache is full"),
            "a full shared cache refuses a new package generation");
        std::cout << "Linked scene ingestion acceptance passed\n";
        // Temporary immutable files deliberately survive until the queue is
        // destroyed. They can be reclaimed with the operating system temp cache.
        return 0;
    } catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
}
