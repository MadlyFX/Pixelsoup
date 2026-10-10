#include <mediaserver/render/director_link_service.hpp>
#include <mediaserver/net/authoring_link_protocol.hpp>
#include <mediaserver/net/tcp_framed_stream.hpp>
#include <mediaserver/assets/content_fingerprint.hpp>
#include <mediaserver/cmd/director_scheduler.hpp>
#include <mediaserver/scene/scene_schema.hpp>
#include <asio/io_context.hpp>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <future>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string_view>
#include <thread>

using namespace mediaserver;
using K = net::LinkMessageKind;
using namespace std::chrono_literals;
namespace fs = std::filesystem;
void check(bool yes, const char* reason) { if (!yes) throw std::runtime_error{reason}; }
std::string uuid(core::u64 n) { return scene::link_instance_uuid(doc::NodeId{n}); }
struct Client {
    asio::io_context io;
    std::unique_ptr<net::TcpFramedStream> stream;
    std::string epoch, scene;
    Client(unsigned port) {
        asio::ip::tcp::socket socket{io}; socket.connect({asio::ip::address_v4::loopback(), static_cast<unsigned short>(port)});
        stream = std::make_unique<net::TcpFramedStream>(std::move(socket), net::FramedStreamLimits{
            net::link_packet_limit, 3s, 10s, 3s});
    }
    net::LinkMessage message(K kind) {
        net::LinkMessage m; m.kind=kind; m.epoch=epoch; m.scene=scene; m.origin="test-publisher"; m.source=uuid(10); return m;
    }
    net::LinkMessage request(net::LinkMessage m, std::vector<std::string>* stages=nullptr) {
        check(static_cast<bool>(stream->send(net::encode_link_message(m))), "send");
        for (;;) {
            auto received=stream->receive(); check(static_cast<bool>(received), "receive");
            auto decoded=net::decode_link_message(received.value()); check(static_cast<bool>(decoded), "decode");
            auto r=decoded.value();
            if (r.kind==K::Reject || r.kind==K::Resync) std::cerr << "Reply: " << r.payload << '\n';
            if (r.kind==K::Status && (r.payload=="received" || r.payload=="validated")) { if (stages) stages->push_back(r.payload); continue; }
            return r;
        }
    }
    void pair(const render::DirectorLinkStatus& status, std::string base={},
              std::string declaration="capabilities=base,stage,environment,materials,animation\nclient=test-publisher/1.0") {
        auto hello=message(K::Hello); hello.payload=status.pairing_code;
        if (!declaration.empty()) hello.payload += '\n'+declaration;
        auto r=request(hello); check(r.kind==K::Welcome, "paired welcome"); epoch=r.epoch; scene=r.scene;
        auto reg=message(K::Register); reg.base=std::move(base); r=request(reg);
        check(r.kind==K::Ack || r.kind==K::Resync, "register");
    }
};
struct Rig {
    render::DirectorLinkService service;
    cmd::DirectorScheduler director;
    render::DirectorLinkContext context;
    std::shared_ptr<const render::DirectorLinkStatus> status;
    std::size_t commits{}, ingested{}, reused{};
    fs::path cache;
    // Rigs given one root share its content-addressed store, like the
    // Director's concurrent per-scene links.
    explicit Rig(fs::path root={}) {
        context.document=context.document.set({doc::NodeId{1},std::string{scene::schema::stage_type},{}});
        context.target=doc::NodeId{1}; context.project=1;
        check(static_cast<bool>(director.schedule_document_replace(context.document,cmd::UndoContextId{2},cmd::TransactionId{1},cmd::FrameNumber{1})),"initial document");
        context.document=director.projected_document();
        cache=root.empty() ? fs::temp_directory_path()/("ms-link-"+net::new_link_secret().substr(0,8)) : std::move(root);
        restart();
    }
    void restart() {
        const auto old=status;
        service.start(cache.string(),context);
        const auto deadline=std::chrono::steady_clock::now()+5s;
        while (std::chrono::steady_clock::now()<deadline) {
            status=service.status();
            if (status && status->listening && (!old || status->pairing_code!=old->pairing_code)) return;
            std::this_thread::sleep_for(5ms);
        }
        throw std::runtime_error{"listener did not start"};
    }
    net::LinkMessage transact(Client& client, net::LinkMessage m, std::vector<std::string>* stages=nullptr) {
        auto work=std::async(std::launch::async,[&] { return client.request(m,stages); });
        const auto deadline=std::chrono::steady_clock::now()+15s;
        while (work.wait_for(1ms)!=std::future_status::ready) {
            if (auto c=service.take()) {
                ingested=c->manifest.ingested; reused=c->manifest.reused;
                auto plan=render::DirectorLinkService::plan(*c,context);
                std::string error;
                if (!plan) error=plan.error().message;
                else {
                    auto scheduled=director.schedule_document_replace(plan.value(),cmd::UndoContextId{render::director_link_undo_context},cmd::TransactionId{(core::u64{1} << 63U) | c->ticket},cmd::FrameNumber{1});
                    check(static_cast<bool>(scheduled),"Director transaction"); ++commits;
                    context.document=director.projected_document(); service.update(context);
                }
                service.resolve(c->ticket,error);
            }
            if (std::chrono::steady_clock::now()>deadline) { service.stop(); throw std::runtime_error{"transaction timeout"}; }
        }
        return work.get();
    }
};
std::string object_row(float x, std::string parent={}) {
    std::ostringstream out;
    out << "object " << std::quoted(uuid(20)) << " \"Hero\" " << std::quoted(parent) << ' ' << std::quoted(uuid(11)) << " \"\" \"origin-v1\" 1";
    for (const auto n:scene::translation({x,0,0}).m) out << ' ' << n;
    return out.str();
}
std::string snapshot(const std::string& hash,const std::string& revision,float x=0) {
    std::ostringstream out;
    out << "MSLINK 1 " << std::quoted(uuid(10)) << ' ' << std::quoted(revision) << '\n'
        << "prototype " << std::quoted(uuid(11)) << ' ' << std::quoted(hash) << ' ' << std::quoted(hash+".glb") << '\n' << object_row(x) << '\n';
    return out.str();
}
net::LinkMessage transaction(Client& client, const std::string& rev,const std::string& base,const std::string& payload,K kind=K::Snapshot) {
    auto m=client.message(kind); m.transaction="tx-"+rev; m.base=base; m.revision=rev; m.payload=payload; return m;
}
// Returns true when bytes crossed the wire; a stored resource is only announced.
bool upload(Client& client,const std::string& hash,const std::string& bytes) {
    auto m=client.message(K::ResourceAnnounce); m.resource=hash; m.length=bytes.size();
    auto r=client.request(m); check(r.kind==K::Ack,"resource announce"); if (r.payload=="resource_present") return false;
    check(client.request(m).kind==K::Ack,"duplicate resource announcement");
    m.kind=K::ResourceChunk; m.payload=bytes; r=client.request(m); check(r.kind==K::Ack,"resource chunk");
    check(client.request(m).kind==K::Ack,"duplicate resource chunk");
    m.kind=K::ResourceComplete; m.payload.clear(); check(client.request(m).kind==K::Ack,"resource completion");
    check(client.request(m).kind==K::Ack,"duplicate completion");
    return true;
}
std::string read_bytes(const fs::path& file) {
    std::ifstream input{file,std::ios::binary};
    return {std::istreambuf_iterator<char>{input},{}};
}
std::string sha256(const std::string& bytes) {
    return assets::sha256_hex(assets::sha256_digest({reinterpret_cast<const core::u8*>(bytes.data()),bytes.size()}));
}
std::size_t count_files(const fs::path& directory, std::string_view extension) {
    std::size_t count{};
    for (const auto& entry : fs::directory_iterator(directory)) count+=entry.path().extension()==extension;
    return count;
}
std::string package_of(const doc::Root& document) {
    for (const auto& change : doc::diff({},document).changes)
        if (change.after->type==scene::link_schema::prototype_type)
            return std::get<std::string>(change.after->properties.at(std::string{scene::schema::source}));
    return {};
}
int main(int argc,char** argv) {
    try {
        check(argc==2,"fixture directory");
        net::LinkMessage codec;
        auto encoded=net::encode_link_message(codec); check(static_cast<bool>(net::decode_link_message(encoded)),"codec round trip");
        for (std::size_t i=0;i<encoded.size();++i) check(!net::decode_link_message(std::span{encoded}.first(i)),"truncation rejection");
        auto oversized=encoded; oversized[12]=std::byte{255}; oversized[13]=std::byte{255}; check(!net::decode_link_message(oversized),"oversized envelope");
        codec.kind=K::ResourceChunk; codec.payload.resize(net::link_chunk_limit+1); check(!net::decode_link_message(net::encode_link_message(codec)),"chunk limit");
        std::ifstream input{fs::path{argv[1]}/"prop-r1.glb",std::ios::binary};
        const std::string bytes{std::istreambuf_iterator<char>{input},{}};
        check(!bytes.empty(),"GLB fixture");
        const auto hash=assets::sha256_hex(assets::sha256_digest({reinterpret_cast<const core::u8*>(bytes.data()),bytes.size()}));
        Rig rig;
        { Client bad{rig.status->port}; auto h=bad.message(K::Hello); h.payload=rig.status->pairing_code; h.version=99;
          check(bad.request(h).payload.starts_with("unsupported_version"),"unsupported version rejected"); }
        { Client bad{rig.status->port}; auto h=bad.message(K::Hello); h.payload="wrong";
          check(bad.request(h).kind==K::Reject,"pairing enforced"); }
        // Capability negotiation: the frame version stays 1, so a mismatched
        // publisher must be refused by feature name rather than by parse error.
        { Client bad{rig.status->port}; auto h=bad.message(K::Hello);
          h.payload=rig.status->pairing_code+"\ncapabilities=base,teleport";
          check(bad.request(h).payload.starts_with("unsupported_capability: teleport"),"unknown capability refused by name"); }
        { Client legacy{rig.status->port};
          auto h=legacy.message(K::Hello); h.payload=rig.status->pairing_code;
          auto welcome=legacy.request(h);
          check(welcome.kind==K::Welcome && welcome.payload.find("capabilities=base,stage,environment,materials,animation")!=std::string::npos,
              "Welcome advertises this build's capability set");
          legacy.epoch=welcome.epoch; legacy.scene=welcome.scene;
          check(legacy.request(legacy.message(K::Register)).kind==K::Ack,"undeclared publisher still registers");
          auto scatter=transaction(legacy,"env","",snapshot(hash,"env"));
          scatter.payload.pop_back(); scatter.payload+=" batch \"zz\"\n";
          check(rig.transact(legacy,scatter).payload.starts_with("capability_not_negotiated: environment"),
              "step 09 suffix refused when the publisher never declared it");
          check(rig.commits==0,"refused capability admits no document"); }
        { Client narrow{rig.status->port}; narrow.pair(*rig.status,{},"capabilities=base");
          check(narrow.request(narrow.message(K::ShotState)).kind==K::ShotState,"writeback state is part of base");
          check(narrow.request(narrow.message(K::StageContext)).payload.starts_with("capability_not_negotiated: stage"),
              "stage references refused without their capability"); }
        // Interrupted bulk bytes are discarded; no document is admitted.
        { Client c{rig.status->port}; c.pair(*rig.status); auto m=c.message(K::ResourceAnnounce); m.resource=hash; m.length=bytes.size();
          check(c.request(m).kind==K::Ack,"partial announce"); m.kind=K::ResourceChunk; m.payload=bytes.substr(0,7); check(c.request(m).kind==K::Ack,"partial chunk"); }
        Client client{rig.status->port}; client.pair(*rig.status);
        auto invalid_resource=client.message(K::ResourceAnnounce); invalid_resource.resource=hash; invalid_resource.length=64ULL*1024ULL*1024ULL+1;
        check(client.request(invalid_resource).kind==K::Reject,"resource byte limit");
        auto first=transaction(client,"r1","",snapshot(hash,"r1"));
        check(rig.transact(client,first).payload=="resource_not_complete","partial resource cannot admit scene");
        check(rig.commits==0,"no partial commit");
        upload(client,hash,bytes);
        std::vector<std::string> stages;
        check(rig.transact(client,first,&stages).kind==K::Ack,"snapshot accepted");
        check(stages==std::vector<std::string>{"received","validated"},"distinct admission stages");
        const auto accepted=rig.context.document;
        scene::Scene reference; reference.id=rig.context.target; reference.show_rate={24000,1001}; reference.show_frame=42;
        for (core::u64 frame=1;frame<=10000;++frame) rig.service.sample(render::linked_stage_sample(reference,frame));
        auto sample=client.request(client.message(K::StageContext));
        check(sample.kind==K::StageContext && sample.payload.starts_with("MSSTAGE 1\n1 10000 42 24000 1001"),"latest tracking sample coalesces without queue growth");
        check(client.request(client.message(K::StageContext)).payload.empty(),"unchanged sample has no payload");
        check(doc::diff(accepted,rig.context.document).changes.empty() && rig.commits==1,"tracking is outside source revision and undo");
        check(rig.transact(client,first).kind==K::Ack && rig.commits==1,"duplicate transaction idempotent");
        auto collision=first; collision.payload+='\n'; check(rig.transact(client,collision).kind==K::Reject,"transaction collision rejected");
        auto stale=transaction(client,"stale","",snapshot(hash,"stale")); check(rig.transact(client,stale).kind==K::Resync,"stale revision rejected");
        auto wrong=first; wrong.scene="wrong-scene"; check(rig.transact(client,wrong).kind==K::Resync,"wrong scene rejected");
        auto path=transaction(client,"path","r1",snapshot(hash,"path"));
        const auto at=path.payload.find(hash+".glb"); path.payload.replace(at,68,"../outside.glb");
        check(rig.transact(client,path).payload=="resource_path_requires_hash_glb","path rejection before ingest");
        auto cycle=transaction(client,"cycle","r1","MSDELTA 1 \""+uuid(10)+"\" \"cycle\"\nupsert "+object_row(0,uuid(20))+"\n",K::Delta);
        check(rig.transact(client,cycle).kind==K::Reject,"hierarchy cycle rejected");
        check(doc::diff(accepted,rig.context.document).changes.empty(),"all rejected snapshots atomic");
        auto occurrences=scene::read_linked_scene(rig.context.document); check(static_cast<bool>(occurrences),"occurrences");
        const auto hero=occurrences.value().occurrences.begin()->first;
        auto placed=scene::plan_linked_edit(rig.context.document,hero,scene::LinkedEdit::Hold); check(static_cast<bool>(placed),"hold override");
        check(static_cast<bool>(rig.director.schedule_document_replace(placed.value(),cmd::UndoContextId{2},cmd::TransactionId{2},cmd::FrameNumber{1})),"operator hold");
        rig.context.document=rig.director.projected_document(); rig.service.update(rig.context);
        const auto held_world=scene::read_linked_scene(rig.context.document).value().occurrences.at(hero).world;
        auto delta=transaction(client,"r2","r1","MSDELTA 1 \""+uuid(10)+"\" \"r2\"\nupsert "+object_row(3)+"\nupsert "+object_row(7)+"\n",K::Delta);
        check(rig.transact(client,delta).kind==K::Ack,"coalesced sparse delta");
        check(scene::read_linked_scene(rig.context.document).value().occurrences.at(hero).world==held_world,"retained held shot placement");
        client.stream->close();
        { Client reconnect{rig.status->port}; reconnect.pair(*rig.status,"r2");
          delta.epoch=reconnect.epoch; delta.scene=reconnect.scene;
          check(rig.transact(reconnect,delta).kind==K::Ack && rig.commits==2,"lost ack reconnect replay"); }
        const auto old_epoch=delta.epoch;
        rig.restart();
        Client restarted{rig.status->port}; restarted.pair(*rig.status,"r2");
        check(restarted.epoch!=old_epoch,"restart rotates epoch");
        check(rig.transact(restarted,delta).kind==K::Resync,"old epoch rejected");
        auto unavailable=transaction(restarted,"r3","r2","MSDELTA 1 \""+uuid(10)+"\" \"r3\"\n",K::Delta);
        check(rig.transact(restarted,unavailable).kind==K::Resync,"restart demands fresh delta baseline");
        // The shared store already holds the verified GLB and its package, so
        // re-linking after a Director restart moves and rebuilds nothing.
        check(!upload(restarted,hash,bytes) && rig.service.status()->metrics.transfers==0,"restart reports the stored GLB present");
        check(rig.transact(restarted,transaction(restarted,"r3","r2",snapshot(hash,"r3",9))).kind==K::Ack,"restart snapshot converges");
        check(rig.ingested==0 && rig.reused==1,"restart reuses the stored package");
        for (const auto& entry : fs::directory_iterator(rig.cache)) check(entry.path().filename()=="shared","pairings create no private directories");
        check(count_files(rig.cache/"shared",".partial")==0 && count_files(rig.cache/"shared",".mslink")==0,"no staging file outlives its step");
        check(scene::read_linked_scene(rig.context.document).value().occurrences.at(hero).world==held_world,"restart retains shot override");
        auto removed=transaction(restarted,"r4","r3","MSDELTA 1 \""+uuid(10)+"\" \"r4\"\nerase object \""+uuid(20)+"\"\n",K::Delta);
        check(rig.transact(restarted,removed).kind==K::Ack,"ordered source deletion");
        check(scene::read_linked_scene(rig.context.document).value().occurrences.at(hero).removed,"source deletion retains recovery tombstone");
        auto restored=transaction(restarted,"r5","r4","MSDELTA 1 \""+uuid(10)+"\" \"r5\"\nupsert "+object_row(15)+"\n",K::Delta);
        check(rig.transact(restarted,restored).kind==K::Ack,"ordered source restoration");
        check(scene::read_linked_scene(rig.context.document).value().occurrences.at(hero).world==held_world,"restoration retains held shot placement");
        check(static_cast<bool>(rig.director.schedule_undo(cmd::UndoContextId{render::director_link_undo_context},cmd::TransactionId{50},cmd::FrameNumber{1})),"remote revision has independent authoring undo");
        rig.context.document=rig.director.projected_document(); rig.service.update(rig.context);
        check(scene::read_linked_scene(rig.context.document).value().occurrences.at(hero).removed,"one undo restores source deletion");
        auto after_undo=transaction(restarted,"r6","r4","MSDELTA 1 \""+uuid(10)+"\" \"r6\"\n",K::Delta);
        check(rig.transact(restarted,after_undo).kind==K::Resync,"undo requires snapshot to renew delta baseline");
        auto c=render::DirectorLinkCandidate{}; c.base=rig.context; c.manifest.source_uuid=uuid(10); c.base_revision="r4";
        auto changed=rig.context; changed.project=2; check(!render::DirectorLinkService::plan(c,changed),"project replacement invalidates candidate");
        changed=rig.context; changed.locked=true; check(!render::DirectorLinkService::plan(c,changed),"frozen revision gate");
        // A scene on air takes Blender edits like any other: Program follows them live.
        changed=rig.context; changed.program=changed.target;
        { const auto on_air=render::DirectorLinkService::plan(c,changed);
          check(on_air || on_air.error().message.find("frozen") == std::string::npos,"Program scene accepts edits"); }
        // Writeback: Blender reads Media Server's overrides on its objects as one
        // MSSHOT 2 state, recomputed only when the document changes.
        check(rig.transact(restarted,transaction(restarted,"shot-source","r4",snapshot(hash,"shot-source",15))).kind==K::Ack,"restore for writeback");
        auto state_request = restarted.message(K::ShotState);
        const auto shot_state = restarted.request(state_request);
        check(shot_state.kind == K::ShotState && shot_state.payload.starts_with("MSSHOT 2\n")
            && shot_state.payload.find(uuid(20)) != std::string::npos, "writeback state names the source object");
        state_request.base = shot_state.revision;
        check(restarted.request(state_request).payload.empty(), "unchanged writeback state has empty payload");
        // A Director drag is an override the next poll shows Blender.
        auto local = scene::plan_linked_world(rig.context.document,hero,scene::translation({8,5,6}));
        check(static_cast<bool>(local),"local Director move");
        check(static_cast<bool>(rig.director.schedule_document_replace(local.value(),cmd::UndoContextId{2},cmd::TransactionId{60},cmd::FrameNumber{1})),"local priority edit");
        rig.context.document = rig.director.projected_document(); rig.service.update(rig.context);
        const auto moved_state = restarted.request(state_request);
        check(!moved_state.payload.empty() && moved_state.revision != shot_state.revision, "Director move changes the writeback state");
        // A source revision that absorbs nothing keeps the Director's override.
        check(rig.transact(restarted,transaction(restarted,"after-local","shot-source",snapshot(hash,"after-local",15))).kind==K::Ack,"source update after local shot");
        check(scene::read_linked_scene(rig.context.document).value().occurrences.at(hero).world == scene::translation({8,5,6}),"source update keeps the Director override");
        check(static_cast<bool>(rig.director.schedule_undo(cmd::UndoContextId{2},cmd::TransactionId{61},cmd::FrameNumber{1})),"native local Undo after remote source");
        rig.context.document=rig.director.projected_document(); rig.service.update(rig.context);
        check(scene::read_linked_scene(rig.context.document).value().occurrences.at(hero).world == held_world,"native Undo restores only local shot");
        check(restarted.request(restarted.message(K::Status)).revision=="after-local","native Undo retains remote source revision");
        check(static_cast<bool>(rig.director.schedule_redo(cmd::UndoContextId{2},cmd::TransactionId{62},cmd::FrameNumber{1})),"native Redo after remote source");
        rig.context.document=rig.director.projected_document(); rig.service.update(rig.context);
        // The artist moved the object in Blender: the revision names it absorbed,
        // so the override clears in the same transaction and Blender's value wins.
        auto absorb = transaction(restarted,"absorbed","after-local",snapshot(hash,"absorbed",8));
        absorb.payload += "absorb \"" + uuid(20) + "\" placement\n";
        check(rig.transact(restarted,absorb).kind==K::Ack,"absorbing revision accepted");
        const auto absorbed = scene::read_linked_scene(rig.context.document).value().occurrences.at(hero);
        check(!absorbed.held && absorbed.world == absorbed.basis && absorbed.world != scene::translation({8,5,6}),"absorbed placement follows the source");
        auto hidden = scene::plan_linked_edit(rig.context.document,hero,scene::LinkedEdit::Hide);
        check(static_cast<bool>(hidden),"local hide");
        check(static_cast<bool>(rig.director.schedule_document_replace(hidden.value(),cmd::UndoContextId{2},cmd::TransactionId{63},cmd::FrameNumber{1})),"local hide scheduled");
        rig.context.document = rig.director.projected_document(); rig.service.update(rig.context);
        auto shown = transaction(restarted,"shown","absorbed",snapshot(hash,"shown",8));
        shown.payload += "absorb \"" + uuid(20) + "\" visibility\n";
        check(rig.transact(restarted,shown).kind==K::Ack && scene::read_linked_scene(rig.context.document).value().occurrences.at(hero).visible,
            "absorbed visibility follows the source");
        // Deleted in Media Server: the writeback state says so, and the artist
        // bringing the object back absorbs the deletion.
        auto removal = scene::plan_linked_edit(rig.context.document,hero,scene::LinkedEdit::Delete);
        check(static_cast<bool>(removal),"local delete");
        check(static_cast<bool>(rig.director.schedule_document_replace(removal.value(),cmd::UndoContextId{2},cmd::TransactionId{64},cmd::FrameNumber{1})),"local delete scheduled");
        rig.context.document = rig.director.projected_document(); rig.service.update(rig.context);
        {
            const auto deleted_state = restarted.request(restarted.message(K::ShotState)).payload;
            std::istringstream shot_record{deleted_state.substr(deleted_state.find('\n') + 1)};
            core::u64 shot_node{}, shot_attachment{}; std::string shot_object;
            int shot_held{}, shot_override{}, shot_visible{}, shot_removed{}, shot_deleted{};
            shot_record >> shot_node >> shot_attachment >> std::quoted(shot_object) >> shot_held >> shot_override
                >> shot_visible >> shot_removed >> shot_deleted;
            check(shot_object == uuid(20) && shot_deleted == 1 && shot_visible == 0 && shot_removed == 0,
                "writeback state names the deletion");
        }
        auto back = transaction(restarted,"restored","shown",snapshot(hash,"restored",8));
        back.payload += "absorb \"" + uuid(20) + "\" deleted\n";
        check(rig.transact(restarted,back).kind==K::Ack && !scene::read_linked_scene(rig.context.document).value().occurrences.at(hero).deleted,
            "absorbed deletion restores the object");
        auto bogus = transaction(restarted,"bogus","restored",snapshot(hash,"bogus",8));
        bogus.payload += "absorb \"" + uuid(20) + "\" teleport\n";
        const auto commits_before_bogus = rig.commits;
        check(rig.transact(restarted,bogus).payload.starts_with("invalid_absorb_record") && rig.commits == commits_before_bogus,
            "unknown absorbed property refused");
        const auto before_pause = rig.context.document;
        auto paused = scene::plan_linked_edit(before_pause,hero,scene::LinkedEdit::Pause);
        check(static_cast<bool>(paused),"pause collection");
        rig.context.document = paused.value(); rig.service.update(rig.context);
        auto pause_request = transaction(restarted,"paused-update","restored",snapshot(hash,"paused-update",22));
        check(rig.transact(restarted,pause_request).payload.starts_with("source_paused"),"paused content rejected before ingestion");
        check(doc::diff(paused.value(),rig.context.document).changes.empty(),"pause retains accepted scene");
        check(rig.service.status()->connected && !rig.service.status()->failed && !rig.service.status()->pending,"pause distinct from asset failure and disconnection");
        rig.context.document = before_pause; rig.service.update(rig.context);
        check(rig.transact(restarted,pause_request).kind == K::Ack,"explicit publish after resume accepted");
        check(rig.service.status()->connected && !rig.service.status()->failed && !rig.service.status()->pending,"accepted update clears refusal");
        render::DirectorLinkCandidate late;
        late.base=rig.context; late.base_revision="paused-update"; late.manifest.source_uuid=uuid(10);
        auto paused_late=scene::plan_linked_edit(rig.context.document,hero,scene::LinkedEdit::Pause);
        auto pause_context=rig.context; pause_context.document=paused_late.value();
        check(render::DirectorLinkService::plan(late,pause_context).error().message.starts_with("source_paused"),"pause rechecked after worker completion");
        rig.context.project=2; rig.service.update(rig.context);
        check(restarted.request(restarted.message(K::Status)).kind==K::Resync,"wrong project on reconnect");
        rig.service.stop();
        // Concurrent links (one service per linked scene) share one store. Both
        // stream the same new GLB at once: the first completion publishes it and
        // the second keeps that verified copy. Their concurrent ingests serialize
        // on the store's package index, so one package serves both scenes.
        {
            const auto second=read_bytes(fs::path{argv[1]}/"prop-r2.glb"), second_hash=sha256(second);
            check(!second.empty(),"second GLB fixture");
            Rig a, b{a.cache};
            const auto store=a.cache/"shared";
            Client ca{a.status->port}, cb{b.status->port}; ca.pair(*a.status); cb.pair(*b.status);
            auto announce_a=ca.message(K::ResourceAnnounce); announce_a.resource=second_hash; announce_a.length=second.size();
            auto announce_b=cb.message(K::ResourceAnnounce); announce_b.resource=second_hash; announce_b.length=second.size();
            check(ca.request(announce_a).payload=="resource_announced" && cb.request(announce_b).payload=="resource_announced",
                "both links stream the same new content");
            auto chunk_a=announce_a, chunk_b=announce_b;
            chunk_a.kind=chunk_b.kind=K::ResourceChunk; chunk_a.payload=chunk_b.payload=second;
            check(ca.request(chunk_a).kind==K::Ack && cb.request(chunk_b).kind==K::Ack,"interleaved chunks");
            check(count_files(store,".partial")==2,"each link stages under its own name");
            auto complete_a=announce_a, complete_b=announce_b; complete_a.kind=complete_b.kind=K::ResourceComplete;
            check(ca.request(complete_a).kind==K::Ack && cb.request(complete_b).kind==K::Ack,"second completion keeps the published copy");
            check(count_files(store,".partial")==0 && read_bytes(store/(second_hash+".glb"))==second,"one verified copy published");
            auto link_a=std::async(std::launch::async,[&] { return a.transact(ca,transaction(ca,"a1","",snapshot(second_hash,"a1"))); });
            auto link_b=std::async(std::launch::async,[&] { return b.transact(cb,transaction(cb,"b1","",snapshot(second_hash,"b1"))); });
            check(link_a.get().kind==K::Ack && link_b.get().kind==K::Ack,"concurrent links ingest through one store");
            check(a.ingested+b.ingested==1 && a.reused+b.reused==1,"serialized ingest builds the shared package once");
            check(count_files(store,".package")==1 && !package_of(a.context.document).empty()
                && package_of(a.context.document)==package_of(b.context.document),"both scenes reference one package");
            check(count_files(store,".mslink")==0 && count_files(store,".tmp")==0,"no staging file outlives its step");
            // Pairing directories from earlier builds stay for the projects that
            // use them; a restart sweeps only this build's stale staging names.
            const auto legacy=a.cache/"0123456789abcdef"; fs::create_directories(legacy);
            std::ofstream{legacy/"resource.partial"} << "legacy";
            const auto abandoned=store/"fedcba9876543210.partial", fresh=store/"0123456789abcdef.partial";
            std::ofstream{abandoned} << "abandoned"; std::ofstream{fresh} << "fresh";
            fs::last_write_time(abandoned,fs::file_time_type::clock::now()-std::chrono::hours{2});
            a.restart();
            check(!fs::exists(abandoned) && fs::exists(fresh) && fs::exists(legacy/"resource.partial"),"restart sweeps only stale staging");
            fs::remove(fresh);
            Client again{a.status->port}; again.pair(*a.status,"a1");
            check(!upload(again,second_hash,second) && a.service.status()->metrics.transfers==0,"re-link reports the stored GLB present");
            check(a.transact(again,transaction(again,"a2","a1",snapshot(second_hash,"a2",3))).kind==K::Ack
                && a.ingested==0 && a.reused==1,"re-link reuses the stored package");
            // Stored bytes are hashed again before they are trusted: damaged
            // content is transferred again and replaced by verified bytes.
            { std::fstream damage{store/(second_hash+".glb"),std::ios::binary|std::ios::in|std::ios::out}; damage.put('X'); }
            b.restart();
            Client healed{b.status->port}; healed.pair(*b.status,"b1");
            check(upload(healed,second_hash,second) && b.service.status()->metrics.transfers==1,"damaged stored GLB is transferred again");
            check(read_bytes(store/(second_hash+".glb"))==second,"verified bytes replace damaged store content");
            a.service.stop(); b.service.stop();
        }
        std::cout << "Director link: framing, pairing, resources, atomic snapshots/deltas, replay, revisions, restart, reconnect, retained overrides and shared resource store passed\n";
        return 0;
    } catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
}
