#include <mediaserver/render/director_link_service.hpp>
#include <mediaserver/render/linked_shot.hpp>
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
              std::string declaration="capabilities=base,shot,stage,environment,materials,animation\nclient=test-publisher/1.0") {
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
    std::size_t commits{};
    fs::path cache;
    Rig() {
        context.document=context.document.set({doc::NodeId{1},std::string{scene::schema::stage_type},{}});
        context.target=doc::NodeId{1}; context.project=1; context.preview=true;
        check(static_cast<bool>(director.schedule_document_replace(context.document,cmd::UndoContextId{2},cmd::TransactionId{1},cmd::FrameNumber{1})),"initial document");
        context.document=director.projected_document();
        cache=fs::temp_directory_path()/("ms-link-"+net::new_link_secret().substr(0,8)); restart();
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
void upload(Client& client,const std::string& hash,const std::string& bytes) {
    auto m=client.message(K::ResourceAnnounce); m.resource=hash; m.length=bytes.size();
    auto r=client.request(m); check(r.kind==K::Ack,"resource announce"); if (r.payload=="resource_present") return;
    check(client.request(m).kind==K::Ack,"duplicate resource announcement");
    m.kind=K::ResourceChunk; m.payload=bytes; r=client.request(m); check(r.kind==K::Ack,"resource chunk");
    check(client.request(m).kind==K::Ack,"duplicate resource chunk");
    m.kind=K::ResourceComplete; m.payload.clear(); check(client.request(m).kind==K::Ack,"resource completion");
    check(client.request(m).kind==K::Ack,"duplicate completion");
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
          check(welcome.kind==K::Welcome && welcome.payload.find("capabilities=base,shot,stage,environment,materials,animation")!=std::string::npos,
              "Welcome advertises this build's capability set");
          legacy.epoch=welcome.epoch; legacy.scene=welcome.scene;
          check(legacy.request(legacy.message(K::Register)).kind==K::Ack,"undeclared publisher still registers");
          auto scatter=transaction(legacy,"env","",snapshot(hash,"env"));
          scatter.payload.pop_back(); scatter.payload+=" batch \"zz\"\n";
          check(rig.transact(legacy,scatter).payload.starts_with("capability_not_negotiated: environment"),
              "step 09 suffix refused when the publisher never declared it");
          check(rig.commits==0,"refused capability admits no document"); }
        { Client narrow{rig.status->port}; narrow.pair(*rig.status,{},"capabilities=base");
          check(narrow.request(narrow.message(K::ShotState)).payload.starts_with("capability_not_negotiated: shot"),
              "reverse placement refused without its capability");
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
        upload(restarted,hash,bytes);
        check(rig.transact(restarted,transaction(restarted,"r3","r2",snapshot(hash,"r3",9))).kind==K::Ack,"restart snapshot converges");
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
        changed=rig.context; changed.preview=false; check(!render::DirectorLinkService::plan(c,changed),"Program gate");
        // Reverse placement uses property CAS, independent of source revisions.
        check(rig.transact(restarted,transaction(restarted,"shot-source","r4",snapshot(hash,"shot-source",15))).kind==K::Ack,"restore for reverse placement");
        auto state_request = restarted.message(K::ShotState);
        const auto shot_state = restarted.request(state_request);
        check(shot_state.kind == K::ShotState && shot_state.payload.starts_with("MSSHOT 1\n"), "reverse state");
        state_request.base = shot_state.revision;
        check(restarted.request(state_request).payload.empty(), "unchanged reverse state has empty payload");
        auto claim = restarted.message(K::ShotClaim); claim.offset = hero.value(); claim.transaction = "gesture-1";
        claim.payload = "placement";
        claim.base = render::linked_shot_revision(rig.context, uuid(10), hero, "placement");
        check(restarted.request(claim).kind == K::Ack, "placement ownership claim");
        check(restarted.request(claim).kind == K::Reject, "overlapping gesture refused");
        // An independent source metadata/geometry revision leaves the token intact.
        check(rig.transact(restarted,transaction(restarted,"geometry-only","shot-source",snapshot(hash,"geometry-only",15))).kind==K::Ack,"independent source during claim");
        auto commit = claim; commit.kind = K::ShotCommit; commit.payload = "world";
        for (const auto v : scene::translation({4,5,6}).m) commit.payload += " " + std::to_string(v);
        check(rig.transact(restarted,commit).kind == K::Ack, "shot commit after independent geometry");
        const auto after_shot = rig.context.document;
        check(rig.transact(restarted,commit).kind == K::Ack && doc::diff(after_shot,rig.context.document).changes.empty(), "shot receipt replay idempotent");
        check(scene::read_linked_scene(after_shot).value().occurrences.at(hero).world == scene::translation({4,5,6}), "accepted reverse world");
        claim.transaction = "gesture-conflict";
        claim.base = render::linked_shot_revision(rig.context,uuid(10),hero,"placement");
        check(restarted.request(claim).kind == K::Ack, "second placement claim");
        auto local = scene::plan_linked_world(rig.context.document,hero,scene::translation({8,5,6}));
        check(static_cast<bool>(local),"local Director move");
        check(static_cast<bool>(rig.director.schedule_document_replace(local.value(),cmd::UndoContextId{2},cmd::TransactionId{60},cmd::FrameNumber{1})),"local priority edit");
        rig.context.document = rig.director.projected_document(); rig.service.update(rig.context);
        commit.transaction = claim.transaction; commit.base = claim.base;
        check(rig.transact(restarted,commit).payload.starts_with("shot_conflict"), "Director intervention explicitly conflicts");
        check(scene::read_linked_scene(rig.context.document).value().occurrences.at(hero).world == scene::translation({8,5,6}), "conflict preserves Director");
        check(rig.transact(restarted,transaction(restarted,"after-local","geometry-only",snapshot(hash,"after-local",15))).kind==K::Ack,"source update after local shot");
        check(static_cast<bool>(rig.director.schedule_undo(cmd::UndoContextId{2},cmd::TransactionId{61},cmd::FrameNumber{1})),"native local Undo after remote source");
        rig.context.document=rig.director.projected_document(); rig.service.update(rig.context);
        check(scene::read_linked_scene(rig.context.document).value().occurrences.at(hero).world == scene::translation({4,5,6}),"native Undo restores only local shot");
        check(restarted.request(restarted.message(K::Status)).revision=="after-local","native Undo retains remote source revision");
        check(static_cast<bool>(rig.director.schedule_redo(cmd::UndoContextId{2},cmd::TransactionId{62},cmd::FrameNumber{1})),"native Redo after remote source");
        rig.context.document=rig.director.projected_document(); rig.service.update(rig.context);
        claim.transaction = "visibility-1"; claim.payload = "visibility";
        claim.base = render::linked_shot_revision(rig.context,uuid(10),hero,"visibility");
        check(restarted.request(claim).kind == K::Ack,"independent visibility claim");
        commit = claim; commit.kind = K::ShotCommit; commit.payload = "hide";
        check(rig.transact(restarted,commit).kind == K::Ack,"visibility committed");
        check(!scene::read_linked_scene(rig.context.document).value().occurrences.at(hero).visible,"reverse visibility");
        render::DirectorLinkCandidate expired; expired.base=rig.context; expired.manifest.source_uuid=uuid(10);
        expired.shot_node=hero; expired.shot_property="placement"; expired.shot_edit="reset";
        expired.shot_base=render::linked_shot_revision(rig.context,uuid(10),hero,"placement");
        expired.shot_deadline=std::chrono::steady_clock::now()-1s;
        check(render::DirectorLinkService::plan(expired,rig.context).error().message.starts_with("shot_claim_expired"),"expired claim fails at Director admission");
        expired.shot_deadline=std::chrono::steady_clock::now()+15s;
        expired.shot_edit="world 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 1";
        check(!render::DirectorLinkService::plan(expired,rig.context),"singular reverse world refused");
        const auto before_pause = rig.context.document;
        auto paused = scene::plan_linked_edit(before_pause,hero,scene::LinkedEdit::Pause);
        check(static_cast<bool>(paused),"pause collection");
        rig.context.document = paused.value(); rig.service.update(rig.context);
        auto pause_request = transaction(restarted,"paused-update","after-local",snapshot(hash,"paused-update",22));
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
        std::cout << "Director link: framing, pairing, resources, atomic snapshots/deltas, replay, revisions, restart, reconnect and retained overrides passed\n";
        return 0;
    } catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
}
