// Step 12 qualification harness. Drives the real Director link service, ingest
// planner, command scheduler and publication path through the documented
// scenario matrix and its controlled faults, then writes the measured evidence.
// Blender evaluation is measured separately by the publisher; nothing here sums
// the two into one figure.
#include <mediaserver/render/director_link_service.hpp>
#include <mediaserver/render/linked_publication.hpp>
#include <mediaserver/render/scene_renderer.hpp>
#include <mediaserver/render/scene_bridge.hpp>
#include <mediaserver/net/authoring_link_protocol.hpp>
#include <mediaserver/net/tcp_framed_stream.hpp>
#include <mediaserver/assets/content_fingerprint.hpp>
#include <mediaserver/cmd/director_scheduler.hpp>
#include <mediaserver/cmd/node_command_bus.hpp>
#include <mediaserver/scene/scene_model.hpp>
#include <mediaserver/scene/scene_schema.hpp>
#include <mediaserver/show/switcher_author.hpp>
#include <mediaserver/persist/project_file.hpp>
#include <mediaserver/persist/settings.hpp>
#include <asio/io_context.hpp>
#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <future>
#include <iomanip>
#include <iostream>
#include <optional>
#include <sstream>
#include <thread>
#include <vector>

using namespace mediaserver;
using namespace std::chrono_literals;
using K = net::LinkMessageKind;
using Clock = std::chrono::steady_clock;
namespace fs = std::filesystem;
namespace ss = scene::schema;
namespace pub = scene::publication;

namespace {
void check(bool ok, const std::string& why) { if (!ok) throw std::runtime_error{why}; }
std::string uuid(core::u64 n) { return scene::link_instance_uuid(doc::NodeId{n}); }
std::string digest_of(const std::string& bytes) {
    return assets::sha256_hex(assets::sha256_digest({reinterpret_cast<const core::u8*>(bytes.data()), bytes.size()}));
}
std::string read_file(const fs::path& path) {
    std::ifstream input{path, std::ios::binary};
    std::string bytes{std::istreambuf_iterator<char>{input}, {}};
    check(!bytes.empty(), "fixture "+path.string());
    return bytes;
}
// One more revision of the same authored prop: the glTF JSON chunk is padded
// with the spaces the format reserves for alignment, so the bytes and therefore
// the resource hash differ while the mesh, materials and validity do not. A
// re-sculpted or re-textured Blender mesh arrives as new bytes in the same way.
std::string resource_variant(const std::string& glb, unsigned generation) {
    check(glb.size()>28 && glb.compare(0,4,"glTF")==0,"GLB fixture header");
    const auto integer=[&glb](std::size_t at) {
        std::uint32_t value{};
        for (unsigned i=0;i<4;++i) value |= static_cast<std::uint32_t>(static_cast<unsigned char>(glb[at+i])) << (8U*i);
        return value;
    };
    const auto json_length=integer(12);
    check(glb.compare(16,4,"JSON")==0 && 20+json_length<=glb.size(),"GLB JSON chunk");
    const std::string padding(4U*static_cast<std::size_t>(generation),' ');
    std::string out=glb.substr(0,20)+glb.substr(20,json_length)+padding+glb.substr(20+json_length);
    const auto write=[&out](std::size_t at, std::uint32_t value) {
        for (unsigned i=0;i<4;++i) out[at+i]=static_cast<char>((value >> (8U*i)) & 0xFFU);
    };
    write(8,static_cast<std::uint32_t>(out.size()));
    write(12,json_length+static_cast<std::uint32_t>(padding.size()));
    return out;
}
double milliseconds(Clock::time_point from, Clock::time_point to) {
    return std::chrono::duration<double, std::milli>(to-from).count();
}
core::u64 percentile(std::vector<core::u64> samples, double fraction) {
    if (samples.empty()) return 0;
    std::sort(samples.begin(), samples.end());
    auto rank = static_cast<std::size_t>(std::ceil(fraction*static_cast<double>(samples.size())));
    return samples[std::min(std::max<std::size_t>(rank, 1), samples.size())-1];
}

// One authoring client, using the same bounded framing the add-on uses.
struct Client {
    asio::io_context io;
    std::unique_ptr<net::TcpFramedStream> stream;
    std::string epoch, scene;
    explicit Client(unsigned port) {
        asio::ip::tcp::socket socket{io};
        socket.connect({asio::ip::address_v4::loopback(), static_cast<unsigned short>(port)});
        stream = std::make_unique<net::TcpFramedStream>(std::move(socket),
            net::FramedStreamLimits{net::link_packet_limit, 3s, 20s, 3s});
    }
    net::LinkMessage message(K kind) {
        net::LinkMessage m; m.kind=kind; m.epoch=epoch; m.scene=scene;
        m.origin="qualification-publisher"; m.source=uuid(10); return m;
    }
    net::LinkMessage request(net::LinkMessage m) {
        check(static_cast<bool>(stream->send(net::encode_link_message(m))), "send");
        for (;;) {
            auto received=stream->receive(); check(static_cast<bool>(received), "receive");
            auto decoded=net::decode_link_message(received.value()); check(static_cast<bool>(decoded), "decode");
            auto r=decoded.value();
            if (r.kind==K::Status && (r.payload=="received" || r.payload=="validated")) continue;
            return r;
        }
    }
    void pair(const render::DirectorLinkStatus& status, std::string base={}) {
        auto hello=message(K::Hello);
        hello.payload=status.pairing_code
            +"\ncapabilities=base,stage,environment,materials,animation\nclient=qualification/0.7.0 blender/5.2.0";
        auto r=request(hello); check(r.kind==K::Welcome, "welcome"); epoch=r.epoch; scene=r.scene;
        auto reg=message(K::Register); reg.base=std::move(base); reg.payload="Qualification collection";
        r=request(reg); check(r.kind==K::Ack || r.kind==K::Resync, "register");
    }
    // Returns true when bytes actually crossed the wire; a retained resource
    // is acknowledged without transfer and must not be timed as one.
    bool upload(const std::string& hash, const std::string& bytes) {
        auto m=message(K::ResourceAnnounce); m.resource=hash; m.length=bytes.size();
        auto r=request(m); check(r.kind==K::Ack, "announce: "+r.payload);
        if (r.payload=="resource_present") return false;
        for (std::uint64_t offset=r.offset; offset<bytes.size();) {
            auto chunk=message(K::ResourceChunk); chunk.resource=hash; chunk.offset=offset;
            chunk.payload=bytes.substr(offset, net::link_chunk_limit);
            r=request(chunk); check(r.kind==K::Ack, "chunk: "+r.payload);
            offset=r.offset;
        }
        m.kind=K::ResourceComplete; m.payload.clear();
        check(request(m).kind==K::Ack, "resource completion");
        return true;
    }
};

// Director side: the same service, planner and scheduler playback drives. The
// adoption stall is the control-thread work one Director frame performs to
// admit a revision, which is what an operator feels as a stall.
struct Rig {
    render::DirectorLinkService service;
    cmd::DirectorScheduler director;
    render::DirectorLinkContext context;
    std::shared_ptr<const render::DirectorLinkStatus> status;
    scene::SceneModel model;
    // Node 2 is show::switcher_node_id; scenes start above the reserved ids.
    doc::NodeId preview{10}, program{11};
    core::u64 commits{}, refusals{}, next_node{1000}, transaction{1};
    double worst_stall_ms{};
    std::vector<core::u64> adoption;
    fs::path cache;

    explicit Rig(fs::path directory) : cache{std::move(directory)} {
        context.document=context.document.set({preview,std::string{ss::stage_type},
            {{"scene_name",std::string{"Blender Preview"}},{"canvas_width",core::i64{640}},
             {"canvas_height",core::i64{360}},{"ambient",0.8}}});
        context.document=context.document.set({program,std::string{ss::stage_type},{{"scene_name",std::string{"Program"}}}});
        context.document=context.document.set({doc::NodeId{12},std::string{ss::camera_type},
            {{"scene_id",static_cast<core::i64>(preview.value())},{"eye_z",6.0},{"eye_y",1.5}}});
        context.document=context.document.set(show::make_switcher_node({program,preview,program,program,{},0}));
        context.target=preview; context.program=program; context.project=1;
        check(static_cast<bool>(director.schedule_document_replace(context.document,cmd::UndoContextId{2},
            cmd::TransactionId{transaction++},cmd::FrameNumber{1})),"initial document");
        context.document=director.projected_document();
        model.reset(context.document);
        restart();
    }
    void restart() {
        const auto previous=status;
        service.start(cache.string(),context);
        const auto deadline=Clock::now()+8s;
        while (Clock::now()<deadline) {
            status=service.status();
            if (status && status->listening && (!previous || status->pairing_code!=previous->pairing_code)) return;
            std::this_thread::sleep_for(5ms);
        }
        throw std::runtime_error{"listener did not start"};
    }
    // Local Director authoring, in the ordinary editor undo context.
    void local(scene::LinkedPlanResult planned, const std::string& why) {
        check(static_cast<bool>(planned), why+(planned ? "" : ": "+planned.error().message));
        check(static_cast<bool>(director.schedule_document_replace(planned.value(),cmd::UndoContextId{2},
            cmd::TransactionId{transaction++},cmd::FrameNumber{1})),why+" transaction");
        context.document=director.projected_document(); service.update(context);
    }
    render::DirectorLinkMetrics metrics() {
        auto current=service.status(); check(static_cast<bool>(current),"service status");
        return current->metrics;
    }
    scene::LinkedSceneState state() {
        auto read=scene::read_linked_scene(context.document);
        check(static_cast<bool>(read),read ? "" : read.error().message);
        return read.value();
    }
    net::LinkMessage transact(Client& client, net::LinkMessage m) {
        auto work=std::async(std::launch::async,[&] { return client.request(m); });
        const auto deadline=Clock::now()+40s;
        while (work.wait_for(1ms)!=std::future_status::ready) {
            if (auto candidate=service.take()) {
                // Everything between these two stamps is control-thread work.
                const auto started=Clock::now();
                auto planned=render::DirectorLinkService::plan(*candidate,context);
                std::string rejection;
                if (!planned) rejection=planned.error().message;
                else {
                    const auto scheduled=director.schedule_document_replace(planned.value(),
                        cmd::UndoContextId{render::director_link_undo_context},
                        cmd::TransactionId{(core::u64{1} << 63U) | candidate->ticket},cmd::FrameNumber{1});
                    check(static_cast<bool>(scheduled),"Director transaction");
                    context.document=director.projected_document();
                    for (const auto& change : doc::diff({},planned.value()).changes)
                        next_node=std::max(next_node,change.id.value()+1);
                    model.reset(context.document);
                    static_cast<void>(model.evaluate(preview,{}));
                    ++commits;
                }
                if (!rejection.empty()) ++refusals;
                const auto stall=milliseconds(started,Clock::now());
                worst_stall_ms=std::max(worst_stall_ms,stall);
                adoption.push_back(static_cast<core::u64>(stall*1000.0));
                service.update(context);
                service.resolve(candidate->ticket,rejection);
            }
            if (Clock::now()>deadline) { service.stop(); throw std::runtime_error{"transaction timeout"}; }
        }
        return work.get();
    }
};

std::string object_row(core::u64 id, const scene::Mat4& placement, std::string name="Hero",
                       std::string parent={}, core::u64 prototype=11) {
    std::ostringstream out;
    out << "object " << std::quoted(uuid(id)) << ' ' << std::quoted(name) << ' ' << std::quoted(parent) << ' '
        << std::quoted(uuid(prototype)) << " \"\" \"origin-v1\" 1";
    for (const auto n : placement.m) out << ' ' << n;
    return out.str();
}
std::string prototype_row(core::u64 id, const std::string& hash) {
    std::ostringstream out;
    out << "prototype " << std::quoted(uuid(id)) << ' ' << std::quoted(hash) << ' ' << std::quoted(hash+".glb");
    return out.str();
}
std::string snapshot_text(const std::string& revision, const std::vector<std::string>& rows) {
    std::ostringstream out;
    out << "MSLINK 1 " << std::quoted(uuid(10)) << ' ' << std::quoted(revision) << '\n';
    for (const auto& row : rows) out << row << '\n';
    return out.str();
}
std::string delta_text(const std::string& revision, const std::vector<std::string>& operations) {
    std::ostringstream out;
    out << "MSDELTA 1 " << std::quoted(uuid(10)) << ' ' << std::quoted(revision) << '\n';
    for (const auto& row : operations) out << row << '\n';
    return out.str();
}
net::LinkMessage transaction(Client& client, const std::string& revision, const std::string& base,
                             std::string payload, K kind=K::Snapshot) {
    auto m=client.message(kind); m.transaction="tx-"+revision; m.base=base; m.revision=revision;
    m.payload=std::move(payload); return m;
}
} // namespace

int main(int argc, char** argv) {
    try {
        check(argc>=3,"usage: <fixture directory> <output directory> [--gpu]");
        const fs::path fixtures=fs::absolute(argv[1]), output=fs::absolute(argv[2]);
        fs::create_directories(output);
        const bool gpu=argc>3 && std::string_view{argv[3]}=="--gpu";
        // Only the single-mesh props are valid prototypes: the reference scene
        // and the 256-occurrence grid are whole scenes, which the importer
        // refuses as one prototype resource. The grid is used as bulk bytes.
        const std::string first=read_file(fixtures/"prop-r1.glb"), second=read_file(fixtures/"prop-r2.glb"),
            workload=read_file(fixtures/"workload-256.glb");
        const auto hash_first=digest_of(first), hash_second=digest_of(second), hash_workload=digest_of(workload);
        std::vector<std::string> scenarios;
        const auto passed=[&scenarios](std::string name) {
            std::cout << "scenario " << name << " passed" << std::endl;
            scenarios.push_back(std::move(name));
        };

        // Short directory name: immutable package paths stay inside the legacy
        // Windows path limit even under a deep evidence directory.
        // The link store is shared across pairings and kept between runs in
        // production; this qualification measures transfers from empty.
        fs::remove_all(output/"link");
        Rig rig{output/"link"};
        Client client{rig.status->port};
        client.pair(*rig.status);
        client.upload(hash_first,first);
        auto accepted=rig.transact(client,transaction(client,"r1","",
            snapshot_text("r1",{prototype_row(11,hash_first),object_row(20,scene::identity())})));
        check(accepted.kind==K::Ack,"first snapshot: "+accepted.payload);
        check(rig.state().occurrences.size()==1,"one occurrence");
        const auto hero=rig.state().occurrences.begin()->first;
        passed("first_publication");

        // Rename keeps identity and needs no geometry transfer.
        accepted=rig.transact(client,transaction(client,"r2","r1",
            delta_text("r2",{"upsert "+object_row(20,scene::identity(),"Renamed hero")}),K::Delta));
        check(accepted.kind==K::Ack,"rename: "+accepted.payload);
        check(rig.state().occurrences.at(hero).name=="Renamed hero","rename retains occurrence identity");
        check(rig.metrics().retained_resources==1,"rename transfers no geometry");
        passed("rename");

        // Duplicate: a second object sharing one immutable prototype.
        accepted=rig.transact(client,transaction(client,"r3","r2",
            delta_text("r3",{"upsert "+object_row(21,scene::translation({2,0,0}),"Duplicate")}),K::Delta));
        check(accepted.kind==K::Ack,"duplicate: "+accepted.payload);
        check(rig.state().occurrences.size()==2 && rig.metrics().retained_resources==1,
            "duplicate reuses the shared resource");
        passed("duplicate");

        // Reparent under the duplicate; the child world composes the parent.
        accepted=rig.transact(client,transaction(client,"r4","r3",
            delta_text("r4",{"upsert "+object_row(20,scene::translation({0,1,0}),"Renamed hero",uuid(21))}),K::Delta));
        check(accepted.kind==K::Ack,"reparent: "+accepted.payload);
        check(rig.state().occurrences.at(hero).world==scene::translation({2,1,0}),"reparent composes the source parent");
        passed("reparent");

        // A held shot placement survives a source origin change.
        rig.local(scene::plan_linked_edit(rig.context.document,hero,scene::LinkedEdit::Hold),"hold placement");
        const auto held=rig.state().occurrences.at(hero).world;
        accepted=rig.transact(client,transaction(client,"r5","r4",
            delta_text("r5",{"upsert "+object_row(20,scene::translation({0,4,7}),"Renamed hero",uuid(21))}),K::Delta));
        check(accepted.kind==K::Ack,"origin change: "+accepted.payload);
        check(rig.state().occurrences.at(hero).world==held,"origin change retains the held shot placement");
        passed("origin_change");

        // Delete leaves a recovery tombstone; the authoring undo context
        // restores it without touching local Director gestures.
        accepted=rig.transact(client,transaction(client,"r6","r5",
            delta_text("r6",{"erase object \""+uuid(20)+"\""}),K::Delta));
        check(accepted.kind==K::Ack,"delete: "+accepted.payload);
        check(rig.state().occurrences.at(hero).removed,"delete keeps a recovery tombstone");
        passed("delete");
        check(static_cast<bool>(rig.director.schedule_undo(cmd::UndoContextId{render::director_link_undo_context},
            cmd::TransactionId{rig.transaction++},cmd::FrameNumber{1})),"authoring undo");
        rig.context.document=rig.director.projected_document(); rig.service.update(rig.context);
        check(!rig.state().occurrences.at(hero).removed,"one undo restores the deleted source object");
        check(rig.state().occurrences.at(hero).world==held,"undo retains the operator's held placement");
        passed("undo");

        // A publisher whose delta baseline was invalidated must snapshot again.
        auto stale=rig.transact(client,transaction(client,"r7","r6",
            delta_text("r7",{"upsert "+object_row(20,scene::identity(),"Renamed hero",uuid(21))}),K::Delta));
        check(stale.kind==K::Resync,"undo invalidates the delta baseline");
        auto resynced=client.request(client.message(K::Register));
        accepted=rig.transact(client,transaction(client,"r8",resynced.revision,
            snapshot_text("r8",{prototype_row(11,hash_first),object_row(20,scene::translation({0,4,7}),"Renamed hero",uuid(21)),
                object_row(21,scene::translation({2,0,0}),"Duplicate")})));
        check(accepted.kind==K::Ack,"resync snapshot: "+accepted.payload);
        passed("resync_after_undo");

        // Interrupted transfer: partial bytes are discarded, never admitted.
        // The service serves one connection at a time, so each client below
        // disconnects before the next one pairs.
        client.stream->close();
        {
            Client interrupted{rig.status->port};
            interrupted.pair(*rig.status,"r8");
            auto announce=interrupted.message(K::ResourceAnnounce);
            announce.resource=hash_second; announce.length=second.size();
            check(interrupted.request(announce).kind==K::Ack,"partial announce");
            auto chunk=interrupted.message(K::ResourceChunk);
            chunk.resource=hash_second; chunk.payload=second.substr(0,64);
            check(interrupted.request(chunk).kind==K::Ack,"partial chunk");
            interrupted.stream->close();
        }
        passed("interrupted_transfer");
        // Reconnect: the same pairing keeps its completed resources and revision.
        Client reconnected{rig.status->port};
        reconnected.pair(*rig.status,"r8");
        {
            auto announce=reconnected.message(K::ResourceAnnounce);
            announce.resource=hash_second; announce.length=second.size();
            const auto reply=reconnected.request(announce);
            check(reply.kind==K::Ack && reply.offset==0,"interrupted transfer restarts at offset zero");
            auto complete=reconnected.message(K::ResourceComplete); complete.resource=hash_second;
            check(rig.transact(reconnected,complete).kind==K::Reject,"incomplete resource cannot complete");
        }
        reconnected.upload(hash_second,second);
        accepted=rig.transact(reconnected,transaction(reconnected,"r9","r8",
            delta_text("r9",{"upsert "+prototype_row(11,hash_second)}),K::Delta));
        check(accepted.kind==K::Ack,"reconnected geometry revision: "+accepted.payload);
        check(rig.state().occurrences.at(hero).world==held,"reconnect retains the held placement");
        passed("reconnect");

        // Malformed input: each refusal is specific and leaves the document intact.
        const auto before_faults=rig.context.document;
        const auto refused=[&](net::LinkMessage m, std::string_view expected, const std::string& why) {
            const auto reply=rig.transact(reconnected,std::move(m));
            check(reply.kind==K::Reject || reply.kind==K::Resync,why+" was not refused");
            check(reply.payload.find(expected)!=std::string::npos,why+" diagnostic was: "+reply.payload);
        };
        refused(transaction(reconnected,"bad-header","r9","MSLINK 2 \""+uuid(10)+"\" \"bad-header\"\n"),
            "manifest_envelope_mismatch","a manifest version the Director does not implement");
        refused(transaction(reconnected,"bad-record","r9",
            delta_text("bad-record",{"upsert widget \"x\""}),K::Delta),"invalid_record","an unknown record kind");
        refused(transaction(reconnected,"bad-erase","r9",
            delta_text("bad-erase",{"erase object \""+uuid(90)+"\""}),K::Delta),
            "erase_missing_record","erasing a record that does not exist");
        {
            auto nonfinite=snapshot_text("bad-matrix",{prototype_row(11,hash_second),
                object_row(22,scene::identity(),"Broken")});
            nonfinite.replace(nonfinite.rfind(" 1"),2," nan");
            refused(transaction(reconnected,"bad-matrix","r9",nonfinite),"","a non-finite placement");
        }
        refused(transaction(reconnected,"missing-resource","r9",
            snapshot_text("missing-resource",{prototype_row(12,std::string(64,'a')),
                object_row(23,scene::identity(),"Absent",{},12)})),
            "resource_not_complete","a prototype whose resource was never transferred");
        check(doc::diff(before_faults,rig.context.document).changes.empty(),"every refusal was atomic");
        passed("malformed_input");

        // Long session: repeated resource replacement and placement-only edits,
        // measured per revision. Textures travel inside the package, so a
        // texture change is a resource replacement on this wire.
        std::vector<core::u64> transfer_ms, ingest_ms, admit_ms;
        std::string base="r9";
        core::u64 replacements{}, placements{}, reuses{};
        for (core::u64 index=0; index<24; ++index) {
            const auto revision="session-"+std::to_string(index);
            std::vector<std::string> operations;
            bool moved=false;
            if (index%3==0) {
                // Every third revision replaces the geometry resource. Two of
                // them repeat an earlier revision's bytes, which the pairing
                // acknowledges from its retained copy without transfer.
                const auto generation=static_cast<unsigned>((index/3)%6);
                const auto body=generation<2 ? (generation ? second : first) : resource_variant(second,generation);
                const auto hash=digest_of(body);
                moved=reconnected.upload(hash,body);
                if (!moved) ++reuses;
                operations.push_back("upsert "+prototype_row(11,hash));
                ++replacements;
            } else {
                operations.push_back("upsert "+object_row(21,scene::translation({2,static_cast<float>(index)*0.25F,0}),"Duplicate"));
                ++placements;
            }
            accepted=rig.transact(reconnected,transaction(reconnected,revision,base,
                delta_text(revision,operations),K::Delta));
            check(accepted.kind==K::Ack,"session revision "+revision+": "+accepted.payload);
            base=revision;
                const auto m=rig.metrics();
            if (moved) transfer_ms.push_back(m.last_transfer_ms);
            ingest_ms.push_back(m.last_ingest_ms);
            admit_ms.push_back(m.last_admit_ms);
        }
        check(rig.metrics().accepted>=24,"every session revision was accepted");
        // Counters and percentiles belong to one pairing: a restart rotates the
        // pairing and forgets them, so the session evidence is captured here.
        const auto session=rig.metrics();
        check(rig.state().occurrences.at(hero).world==held,"a long session retains the operator's placement");
        passed("long_session");

        // Cancellation during ingest: the candidate is discarded, not admitted.
        {
            auto cancelled=transaction(reconnected,"cancelled",base,
                delta_text("cancelled",{"upsert "+prototype_row(11,hash_first)}),K::Delta);
            auto work=std::async(std::launch::async,[&] { return reconnected.request(cancelled); });
            std::this_thread::sleep_for(15ms);
            rig.service.stop();
            std::optional<net::LinkMessage> reply;
            work.wait();
            // Stopping interrupts the stream, so a lost connection is expected.
            try { reply=work.get(); } catch (const std::exception&) {}
            check(!reply || reply->kind!=K::Ack,"a stopped pairing cannot admit its candidate");
            check(!rig.service.take(),"a stopped pairing offers no candidate");
        }
        passed("cancellation");

        // Restart rotates the epoch and forgets receipts and baselines.
        const auto document_before_restart=rig.context.document;
        const auto previous_epoch=reconnected.epoch;
        reconnected.stream->close();
        rig.restart();
        Client restarted{rig.status->port};
        restarted.pair(*rig.status,base);
        check(restarted.epoch!=previous_epoch,"restart rotates the pairing epoch");
        check(rig.transact(restarted,transaction(restarted,"after-restart-delta",base,
            delta_text("after-restart-delta",{"upsert "+object_row(21,scene::identity(),"Duplicate")}),K::Delta)).kind==K::Resync,
            "restart requires a fresh snapshot baseline");
        restarted.upload(hash_second,second);
        accepted=rig.transact(restarted,transaction(restarted,"after-restart",base,
            snapshot_text("after-restart",{prototype_row(11,hash_second),
                object_row(20,scene::translation({0,4,7}),"Renamed hero",uuid(21)),
                object_row(21,scene::translation({2,0,0}),"Duplicate")})));
        check(accepted.kind==K::Ack,"snapshot after restart: "+accepted.payload);
        check(rig.state().occurrences.at(hero).world==held,"restart retains shot overrides");
        check(!doc::diff(document_before_restart,rig.context.document).changes.empty(),"restart admitted a new revision");
        passed("director_restart");

        // Save and reopen without a publisher; the accepted packages stand alone.
        check(static_cast<bool>(persist::save_project_file(output/"preview.mstage",rig.context.document)),"save project");
        auto reopened=persist::load_project_file(output/"preview.mstage");
        check(static_cast<bool>(reopened),"reopen project without Blender");
        {
            scene::SceneModel offline; offline.reset(reopened.value());
            const auto frame=offline.evaluate(rig.preview,{});
            check(frame.models.size()==2,"offline Preview evaluates both occurrences");
            auto live=rig.model.evaluate(rig.preview,{});
            check(scene::digest(live)==scene::digest(frame),"reopened document matches the live one");
        }
        check(static_cast<bool>(persist::save_recent_project_path(
            output/"local/MediaServer/settings/recent-project.bin",output/"preview.mstage")),"isolated startup fixture");
        passed("save_reopen");

        // Per-pairing resource capacity is refused explicitly, in its own pairing.
        restarted.stream->close();
        rig.restart();
        render::DirectorLinkMetrics capacity;
        {
            Client filling{rig.status->port};
            filling.pair(*rig.status,"after-restart");
            check(filling.upload(hash_workload,workload),"bulk bytes for the pending-byte peak");
            for (core::u64 index=0; index<62; ++index) {
                const auto bytes="ms-qualification-filler-"+std::to_string(index);
                filling.upload(digest_of(bytes),bytes);
            }
            filling.upload(hash_second,second);
            auto announce=filling.message(K::ResourceAnnounce);
            announce.resource=hash_first; announce.length=first.size();
            const auto reply=filling.request(announce);
            check(reply.kind==K::Reject && reply.payload.find("resource_capacity")!=std::string::npos,
                "the 64-resource pairing limit is refused by name: "+reply.payload);
            auto oversized=filling.message(K::ResourceAnnounce);
            oversized.resource=hash_first; oversized.length=64ULL*1024*1024+1;
            check(filling.request(oversized).kind==K::Reject,"the 64 MiB per-resource limit is refused");
            capacity=rig.metrics();
            check(capacity.retained_resources==64,"the pairing retained exactly its resource limit");
        }
        rig.service.stop();
        passed("resource_capacity");

        // Coordinated Program publication with controlled faults, from the
        // document this session actually produced.
        auto root=rig.context.document;
        auto program=*root.get(rig.program);
        program.properties[std::string{pub::storage}]=(output/"shared").string();
        root=root.set(program);
        const auto staging_started=Clock::now();
        auto prepared=render::prepare_linked_publication({root,rig.preview,rig.program,(output/"shared").string(),false,{}});
        check(prepared.diagnostic.empty(),prepared.diagnostic);
        const auto staged_at=Clock::now();
        auto installed=render::install_linked_publication(root,prepared,rig.program,doc::NodeId{rig.next_node+100});
        check(static_cast<bool>(installed),installed ? "" : installed.error().message);
        const auto installed_at=Clock::now();
        root=installed.value();
        const auto candidate=pub::render_scene(root,rig.program);
        check(candidate!=rig.program && !pub::linked(root,candidate),"frozen candidate has independent drawables");
        render::PublicationLimits tiny; tiny.vertices=0;
        check(render::prepare_linked_publication({root,candidate,{},{},true,tiny}).diagnostic.find("capacity refused")!=std::string::npos,
            "insufficient node capacity is refused before publication");
        const auto manifest_path=prepared.manifest;
        fs::rename(manifest_path,manifest_path+".withheld");
        check(!render::prepare_linked_publication({root,candidate,{},{},true,{}}).diagnostic.empty(),
            "a second node refuses an incomplete transfer");
        fs::rename(manifest_path+".withheld",manifest_path);
        check(render::prepare_linked_publication({root,candidate,{},{},true,{}}).diagnostic.empty(),
            "the completed transfer verifies on the second node");
        scene::SceneModel frozen; frozen.reset(root);
        const auto digest=scene::asset_set_digest(frozen.evaluate(candidate,{}));
        core::AssetSetReadiness node_a{digest,true,0},node_b{digest,false,0};
        core::AssetCohort cohort; cohort.observe(&node_a); cohort.observe(&node_b);
        check(!cohort.can_commit(),"an incomplete cohort cannot authorize a reveal");
        cohort={}; cohort.observe(&node_a); cohort.observe(nullptr);
        check(!cohort.can_commit(),"a disconnected required node blocks publication");
        node_b.ready=true; cohort={}; cohort.observe(&node_a); cohort.observe(&node_b);
        check(cohort.can_commit(),"a complete cohort authorizes the reveal");
        const core::AssetCommit permit{digest,40,7};
        auto authorized=*root.get(candidate);
        authorized.properties[std::string{ss::asset_commit_digest}]=static_cast<core::i64>(digest);
        authorized.properties[std::string{ss::asset_commit_control}]=core::i64{40};
        authorized.properties[std::string{ss::asset_commit_show}]=core::i64{7};
        root=root.set(authorized);
        cmd::DirectorScheduler show_director{root};
        cmd::NodeCommandBus nodes[]{cmd::NodeCommandBus{root},cmd::NodeCommandBus{root}};
        core::AssetPublicationGate gates[2];
        core::u64 held_frames{}, revealed_frames{}, dropped_frames{};
        for (core::u64 frame=1; frame<=40; ++frame) {
            const auto activated=show::activate_linked_publication(show_director,cmd::FrameNumber{frame});
            check(static_cast<bool>(activated),"activation admission");
            const auto seal=show_director.seal_frame(cmd::FrameNumber{frame});
            check(static_cast<bool>(seal),"seal cohort frame");
            for (std::size_t index=0; index<2; ++index) {
                auto& node=nodes[index];
                check(static_cast<bool>(node.receive_complete_batch(show_director.commands_for(cmd::FrameNumber{frame}),
                    seal.value())),"complete frame batch");
                if (node.commit_frame(cmd::FrameNumber{frame}).status!=cmd::CommitStatus::Committed) ++dropped_frames;
                check(show::read_switcher(node.document())->program_scene==(frame==40?candidate:rig.program),
                    "both nodes switch the same revision on the same frame");
            }
            const bool a=gates[0].present(node_a,permit,frame,7,true), b=gates[1].present(node_b,permit,frame,7,true);
            check(a==b,"both nodes reveal together");
            if (a) ++revealed_frames; else ++held_frames;
        }
        check(dropped_frames==0 && revealed_frames==1 && held_frames==39,"exactly one revealed frame after a held lead");
        check(gates[0].visible_commit()==gates[1].visible_commit(),"identical publication evidence on both nodes");
        const auto rollback=doc::NodeId{pub::integer(nodes[0].document().get(candidate),pub::rollback)};
        check(rollback.value() && !pub::text(nodes[0].document().get(rollback),pub::revision).empty(),
            "the previous Program is retained as an independent rollback revision");
        auto saved=scene::without_asset_commits(nodes[0].document());
        check(static_cast<bool>(persist::save_project_file(output/"published.mstage",saved)),"save published project");
        auto published=persist::load_project_file(output/"published.mstage");
        check(static_cast<bool>(published),"reopen published project");
        check(render::prepare_linked_publication({published.value(),candidate,{},{},true,{}}).diagnostic.empty(),
            "the published revision verifies offline, without Blender");
        passed("two_node_publication");

        core::u64 gpu_frames{};
        double gpu_residency_ms{};
        if (gpu) {
            // The delivered project, rendered by the actual Advanced renderer.
            scene::SceneModel program_model; program_model.reset(published.value());
            rhi::DeviceConfig config; config.debug=false; config.backend=rhi::BackendKind::D3d12;
            auto device=rhi::Device::create(config); check(static_cast<bool>(device),"D3D12 device");
            render::RendererLimits limits; limits.planar_reflections=false;
            auto created=render::SceneRenderer::create(device.value(),640,360,limits,render::SceneRendererMode::AdvancedLinear);
            check(static_cast<bool>(created),"Advanced renderer");
            auto renderer=std::move(created).value();
            const auto frame=program_model.evaluate(candidate,{});
            check(!frame.models.empty(),"published Program has drawables");
            const auto residency_started=Clock::now();
            bool ready=false;
            for (int attempt=0; attempt<150 && !ready; ++attempt) {
                auto acquired=device.value().acquire_commands(); check(static_cast<bool>(acquired),"commands");
                auto commands=std::move(acquired).value();
                ++gpu_frames;
                if (renderer.render(commands,frame,{0,0,640,360})) {
                    auto submitted=device.value().submit(std::move(commands));
                    check(static_cast<bool>(submitted) && submitted.value().wait(),"GPU submission");
                    const auto& stats=renderer.stats();
                    ready=stats.models_pending==0 && stats.models_failed==0 && stats.dropped_instances==0;
                }
                if (!ready) {
                    static_cast<void>(render::detail::wait_for_shared_model_cache_for_test());
                    std::this_thread::sleep_for(10ms);
                }
            }
            check(ready,"the published revision renders in Advanced D3D12 with no dropped instances");
            gpu_residency_ms=milliseconds(residency_started,Clock::now());
            passed("advanced_renderer");
        }

        std::ofstream report{output/"qualification.json"};
        report << std::fixed << std::setprecision(3) << "{\n  \"scenarios\": [";
        for (std::size_t i=0;i<scenarios.size();++i) report << (i?", ":"") << std::quoted(scenarios[i]);
        report << "],\n  \"publisher\": " << std::quoted(session.client)
               << ",\n  \"negotiated_capabilities\": " << std::quoted(session.capabilities)
               << ",\n  \"session\": {"
               << "\n    \"accepted_revisions\": " << session.accepted
               << ",\n    \"refusals\": " << session.refused
               << ",\n    \"director_plan_refusals\": " << rig.refusals
               << ",\n    \"resyncs\": " << session.resynced
               << ",\n    \"resource_transfers\": " << session.transfers
               << ",\n    \"resource_replacements\": " << replacements
               << ",\n    \"resource_reuses\": " << reuses
               << ",\n    \"placement_only_revisions\": " << placements
               << ",\n    \"peak_pending_bytes\": " << session.peak_pending_bytes
               << ",\n    \"retained_resources\": " << session.retained_resources
               << ",\n    \"retained_resource_bytes\": " << session.retained_bytes
               << ",\n    \"retained_receipts\": " << session.retained_receipts
               << ",\n    \"event_to_preview_ms\": {\"p50\": " << session.median_ms
               << ", \"p95\": " << session.tail_ms << ", \"last\": " << session.last_ms << "}"
               << ",\n    \"director_phase_ms\": {\"transfer_samples\": " << transfer_ms.size()
               << ", \"transfer_p95\": " << percentile(transfer_ms,0.95)
               << ", \"ingest_p50\": " << percentile(ingest_ms,0.5)
               << ", \"ingest_p95\": " << percentile(ingest_ms,0.95)
               << ", \"admit_p95\": " << percentile(admit_ms,0.95)
               << ", \"admit_worst\": " << session.worst_admit_ms << "}"
               << ",\n    \"adoption_stall_ms\": {\"p50\": " << static_cast<double>(percentile(rig.adoption,0.5))/1000.0
               << ", \"p95\": " << static_cast<double>(percentile(rig.adoption,0.95))/1000.0
               << ", \"worst\": " << rig.worst_stall_ms << "}"
               << "\n  },\n  \"capacity_pairing\": {\"retained_resources\": " << capacity.retained_resources
               << ", \"retained_resource_bytes\": " << capacity.retained_bytes
               << ", \"peak_pending_bytes\": " << capacity.peak_pending_bytes << "}"
               << ",\n  \"program_frames\": {\"held\": " << held_frames << ", \"revealed\": " << revealed_frames
               << ", \"dropped\": " << dropped_frames << "}"
               << ",\n  \"event_to_program_ms\": {\"stage\": " << milliseconds(staging_started,staged_at)
               << ", \"install\": " << milliseconds(staged_at,installed_at)
               << ", \"permit_lead_frames\": " << held_frames << ", \"gpu_residency\": " << gpu_residency_ms << "}"
               << ",\n  \"publication_revision\": " << std::quoted(prepared.revision)
               << ",\n  \"published_bytes\": " << prepared.bytes
               << ",\n  \"gpu_submissions\": " << gpu_frames << "\n}\n";
        check(report.good(),"write qualification report");
        std::cout << "Qualification: " << scenarios.size() << " scenarios, "
                  << session.accepted << " accepted revisions, event-to-Preview p50 "
                  << session.median_ms << " ms / p95 " << session.tail_ms
                  << " ms, worst adoption stall " << rig.worst_stall_ms << " ms\n";
        return 0;
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
