#include <mediaserver/render/director_link_service.hpp>
#include <mediaserver/render/environment_budget.hpp>
#include <mediaserver/render/linked_shot.hpp>
#include <mediaserver/net/authoring_link_protocol.hpp>
#include <mediaserver/net/tcp_framed_stream.hpp>
#include <mediaserver/assets/content_fingerprint.hpp>
#include <mediaserver/scene/scene_schema.hpp>
#include <asio/io_context.hpp>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <map>
#include <mutex>
#include <set>
#include <sstream>
#include <thread>
#include <vector>

namespace mediaserver::render {
namespace {
namespace fs = std::filesystem;
namespace ls = scene::link_schema;
using net::LinkMessage;
using K = net::LinkMessageKind;
using Clock = std::chrono::steady_clock;
using Rows = std::map<std::pair<std::string, std::string>, std::string>;
constexpr std::uint64_t resource_limit = 64ULL*1024ULL*1024ULL;
constexpr std::uint64_t session_byte_limit = link_session_byte_limit;
// One content-addressed store serves every pairing and every linked scene. It
// grows only with distinct content, so its budget counts it alone: pairing
// directories written by earlier builds stay for the projects that reference
// them and are neither counted nor touched.
constexpr std::uintmax_t store_byte_limit = 64ULL*1024ULL*1024ULL*1024ULL;
constexpr std::size_t store_package_limit = 65536;
void require(bool ok, const char* reason) { if (!ok) throw std::runtime_error{reason}; }
std::string property(const doc::Node& n, std::string_view key) {
    const auto it = n.properties.find(key);
    if (it != n.properties.end()) if (const auto* s = std::get_if<std::string>(&it->second)) return *s;
    return {};
}
std::string revision(const doc::Root& root, const std::string& source) {
    for (const auto& c : doc::diff({}, root).changes)
        if (c.after->type == ls::source_type && property(*c.after, ls::uuid) == source) return property(*c.after, ls::revision);
    return {};
}
bool source_paused(const DirectorLinkContext& current, const std::string& uuid) {
    for (const auto& c : doc::diff({},current.document).changes) {
        const auto& n = *c.after;
        if (n.type != ls::instance_type) continue;
        const auto paused = n.properties.find(ls::paused), src = n.properties.find(ls::source);
        if (paused == n.properties.end() || paused->second != doc::PropertyValue{true}
            || src == n.properties.end()) continue;
        const auto* id = std::get_if<core::i64>(&src->second);
        const auto* source = id && *id > 0 ? current.document.get(doc::NodeId{static_cast<core::u64>(*id)}) : nullptr;
        if (source && property(*source,ls::uuid) == uuid) return true;
    }
    return false;
}
std::string hash_text(const std::string& text) {
    return assets::sha256_hex(assets::sha256_digest({reinterpret_cast<const core::u8*>(text.data()), text.size()}));
}
bool hex_name(const std::string& s, std::size_t size) {
    return s.size() == size && std::all_of(s.begin(), s.end(), [](char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'); });
}
bool hash_id(const std::string& s) { return hex_name(s, 64); }
// A stored resource is trusted only after this worker hashes it again: names are
// server-chosen, but the store outlives the pairing that wrote it and is shared
// with other pairings and processes. Returns the verified length, 0 if absent.
std::uint64_t stored_resource(const fs::path& file, const std::string& hash, std::uint64_t length, std::stop_token stop) {
    std::error_code error;
    if (fs::symlink_status(file, error).type() != fs::file_type::regular) return 0;
    const auto size = fs::file_size(file, error);
    if (error || size == 0 || size > resource_limit || (length && size != length)) return 0;
    const auto fingerprint = assets::content_fingerprint(file.string(), stop);
    return fingerprint && fingerprint.value().byte_length == size
        && assets::sha256_hex(fingerprint.value().sha256) == hash ? size : 0;
}
// Staging files left by a pairing or ingest that ended without cleanup (a crash
// or power loss): `<16 hex>.partial|.mslink` and the package writers' `.tmp`
// siblings. Nothing a project references has these names. Windows refuses to
// remove a file still open; elsewhere, work stalled that long fails closed when
// its rename finds the staging file gone.
void sweep_stale_staging(const fs::path& store) {
    const auto cutoff = fs::file_time_type::clock::now() - std::chrono::hours{1};
    std::error_code error;
    for (fs::directory_iterator it{store, error}, end; !error && it != end; it.increment(error)) {
        const auto& path = it->path();
        const bool staging = path.extension() == ".tmp"
            || ((path.extension() == ".partial" || path.extension() == ".mslink") && hex_name(path.stem().string(), 16));
        if (!staging) continue;
        std::error_code entry;
        if (const auto written = it->last_write_time(entry); !entry && written < cutoff) fs::remove(path, entry);
    }
}
void end_row(std::istringstream& in) {
    require(!in.fail(), "invalid_fields"); in >> std::ws; require(in.eof(), "unexpected_fields");
}
// Bare tokens of one manifest record. Quoted fields are consumed with the same
// escaping the writer used, so a name can never be mistaken for a feature tag.
std::set<std::string> bare_tokens(const std::string& line) {
    std::set<std::string> tokens;
    std::istringstream in{line};
    while (in >> std::ws, in.good() && in.peek() != std::istringstream::traits_type::eof()) {
        if (in.peek() == '"') { std::string field; in >> std::quoted(field); if (in.fail()) break; continue; }
        std::string token; if (!(in >> token)) break;
        tokens.insert(std::move(token));
    }
    return tokens;
}
// Optional wire features are named on the pairing, so a publisher built against
// a different Director is refused by capability instead of failing inside a
// suffix one side cannot parse. See protocol-v1.md and qualification-v1.md.
void require_record_capabilities(const std::string& record, const std::set<std::string>& negotiated) {
    static const std::string environment{"environment"}, materials{"materials"}, animation{"animation"};
    for (const auto& tag : bare_tokens(record)) {
        const std::string* needed{};
        if (tag == "batch" || tag == "lod") needed = &environment;
        else if (tag == "light") needed = &materials;
        else if (tag == "animation") needed = &animation;
        if (needed && !negotiated.contains(*needed))
            throw std::runtime_error{"capability_not_negotiated: " + *needed
                + "; update_the_Blender_add_on_and_Media_Server_together"};
    }
}
core::u64 elapsed_ms(Clock::time_point from, Clock::time_point to) {
    if (to <= from) return 0;
    return static_cast<core::u64>(std::chrono::duration_cast<std::chrono::milliseconds>(to-from).count());
}
// Nearest-rank percentile over this pairing's successful revisions, matching
// the convention of the reference fixture measurements. Never sums unrelated phases.
core::u64 percentile(std::vector<core::u64> samples, double fraction) {
    if (samples.empty()) return 0;
    std::sort(samples.begin(), samples.end());
    auto rank = static_cast<std::size_t>(std::ceil(fraction*static_cast<double>(samples.size())));
    if (rank == 0) rank = 1;
    return samples[std::min(rank, samples.size())-1];
}
// The wire never accepts paths: even an authenticated authoring client may only
// refer to a completed, verified hash.glb in the shared store. `absorb` records
// are not content: they name objects whose Media Server override this revision
// replaces, and leave with the revision instead of joining the retained rows.
Rows merge_rows(const LinkMessage& m, Rows rows, const std::set<std::string>& negotiated,
        std::vector<std::pair<std::string, std::string>>& absorbed) {
    std::istringstream input{m.payload}; std::string line, magic, source, rev; unsigned version{};
    require(static_cast<bool>(std::getline(input, line)), "missing_manifest_header");
    std::istringstream header{line}; header >> magic >> version >> std::quoted(source) >> std::quoted(rev); end_row(header);
    require(magic == (m.kind == K::Snapshot ? "MSLINK" : "MSDELTA") && version == 1
        && source == m.source && rev == m.revision, "manifest_envelope_mismatch");
    if (m.kind == K::Snapshot) rows.clear();
    std::size_t operations{};
    while (std::getline(input, line)) {
        if (line.empty() || line[0] == '#') continue;
        require(++operations <= link_record_limit, "record_capacity");
        if (line.starts_with("absorb ")) {
            std::istringstream row{line}; std::string tag, uuid, what;
            row >> tag >> std::quoted(uuid) >> what; end_row(row);
            require(uuid.size() <= 128 && (what == "placement" || what == "visibility" || what == "light" || what == "material"
                    || what == "deleted"), "invalid_absorb_record");
            absorbed.emplace_back(std::move(uuid), std::move(what));
            continue;
        }
        std::istringstream row{line}; std::string op, kind, uuid;
        if (m.kind == K::Delta) row >> op;
        row >> kind >> std::quoted(uuid);
        require(!row.fail() && uuid.size() <= 128 && (kind == "object" || kind == "material" || kind == "prototype"), "invalid_record");
        const auto key = std::pair{kind, uuid};
        if (m.kind == K::Delta && op == "erase") {
            end_row(row); require(rows.erase(key) == 1, "erase_missing_record"); continue;
        }
        require(m.kind == K::Snapshot || op == "upsert", "unknown_delta_operation");
        if (kind == "prototype") {
            std::string hash, path; row >> std::quoted(hash) >> std::quoted(path);
            require(hash_id(hash) && path == hash + ".glb", "resource_path_requires_hash_glb");
            require(!row.fail(),"invalid_prototype"); row >> std::ws;
            if (!row.eof()) {
                std::string tag, low_hash, low_path; float pixels{};
                row >> tag >> pixels >> std::quoted(low_hash) >> std::quoted(low_path); end_row(row);
                require(tag == "lod" && std::isfinite(pixels) && pixels > 0 && hash_id(low_hash)
                    && low_path == low_hash+".glb","invalid_lod_resource");
            }
        }
        if (m.kind == K::Snapshot) require(!rows.contains(key), "duplicate_identity");
        // Within one atomic delta repeated replacements coalesce in order.
        // Erases remain ordering barriers and erase of an absent ID is rejected.
        auto record = m.kind == K::Delta ? line.substr(line.find_first_of(" \t")+1) : line;
        require_record_capabilities(record, negotiated);
        rows[key] = std::move(record);
        require(rows.size() <= link_record_limit, "record_capacity");
    }
    return rows;
}
std::string manifest_text(const LinkMessage& m, const Rows& rows) {
    std::ostringstream out;
    out << "MSLINK 1 " << std::quoted(m.source) << ' ' << std::quoted(m.revision) << '\n';
    for (const auto& [key, line] : rows) { static_cast<void>(key); out << line << '\n'; }
    auto text = out.str(); require(text.size() <= net::link_manifest_limit, "manifest_capacity"); return text;
}
}

scene::LinkedPlanResult DirectorLinkService::plan(const DirectorLinkCandidate& c, const DirectorLinkContext& now) {
    const auto fail = [](const char* reason) { return scene::LinkedPlanResult::failure({{}, reason}); };
    if (c.cancellation.stop_requested()) return fail("pairing_stopped");
    if (now.project != c.base.project || now.target != c.base.target) return fail("wrong_scene_resync_required");
    if (source_paused(now,c.manifest.source_uuid)) return fail("source_paused: resume_the_collection_then_publish");
    if (revision(now.document, c.manifest.source_uuid) != c.base_revision) return fail("stale_revision_resync_required");
    for (const auto& change : doc::diff(c.base.document, now.document).changes) {
        const auto record = [](const doc::NodeRef& n) { return n && n->type.starts_with("link."); };
        if (record(change.before) || record(change.after)
            || (change.before && scene::is_linked_occurrence(*change.before) && !change.after)) return fail("linked_state_changed_resync_required");
    }
    // The scene may be on air: an accepted revision is an ordinary document
    // edit, so Program and every output follow it on the next control frame.
    auto planned = plan_linked_manifest(now.document, c.manifest, now.target);
    if (!planned || c.absorbed.empty()) return planned;
    return scene::absorb_linked_overrides(planned.value(), c.manifest.source_uuid, now.target, c.absorbed);
}

struct DirectorLinkService::Impl {
    std::mutex sample_mutex;
    LinkedStageSample latest_sample;
    struct Start {
        std::string directory;
        DirectorLinkContext context;
        unsigned port{};
        std::stop_source stop;
    };
    struct Resolution { core::u64 ticket{}; std::string rejection; };
    std::atomic<std::shared_ptr<const DirectorLinkContext>> context;
    std::atomic<std::shared_ptr<Start>> pending;
    std::atomic<std::shared_ptr<const DirectorLinkStatus>> state;
    std::atomic<std::shared_ptr<const DirectorLinkCandidate>> candidate;
    std::atomic<std::shared_ptr<const Resolution>> resolution;
    std::shared_ptr<Start> active; // Control-thread-owned cancellation handle.
    core::u64 next_ticket{1}; // Worker-owned, never reused across pairings.
    std::jthread worker;

    Impl() : worker{[this](std::stop_token stop) { run(stop); }} {}
    void publish(const DirectorLinkStatus& s) { state.store(std::make_shared<const DirectorLinkStatus>(s)); }
    void run(std::stop_token shutdown) {
        while (!shutdown.stop_requested()) {
            auto config = pending.exchange({});
            if (!config) { std::this_thread::sleep_for(std::chrono::milliseconds{10}); continue; }
            try { serve(*config, shutdown); }
            catch (const std::exception& e) { publish({false, 0, {}, std::string{"Scene Link stopped: "}+e.what()}); }
        }
    }
    void serve(Start& config, std::stop_token shutdown) {
        const auto stop = config.stop.get_token();
        if (stop.stop_requested()) return;
        asio::io_context io;
        asio::ip::tcp::acceptor acceptor{io, {asio::ip::address_v4::loopback(), static_cast<unsigned short>(config.port)}};
        acceptor.non_blocking(true);
        DirectorLinkStatus status{true, acceptor.local_endpoint().port(), net::new_link_secret(), "Waiting for paired authoring client"};
        const std::string epoch = net::new_link_secret(), scene_token = net::new_link_secret();
        // Every pairing shares one content-addressed store: completed resources
        // are `<hash>.glb` and the store is also the ingest package cache, so
        // re-pairing reuses verified GLBs and packages instead of copying them.
        // This pairing's staging names carry a short epoch prefix (kept below
        // legacy Windows path limits); the full epoch remains on the wire.
        const auto store = fs::absolute(config.directory) / "shared";
        fs::create_directories(store);
        sweep_stale_staging(store);
        const auto pairing = epoch.substr(0, 16);
        const auto partial = store / (pairing + ".partial"), staged_manifest = store / (pairing + ".mslink");
        // The store is walked once per pairing and again only after an ingest
        // packaged new content; between walks this pairing's own additions are
        // counted as it makes them. A full walk per resource stalled pairing on
        // a large store or a network share (thousands of entries per announce).
        // Ingest measures the store itself before it writes packages, so
        // another pairing's concurrent additions are still bounded there.
        const auto measure_store = [&] {
            std::uintmax_t bytes{};
            std::error_code error;
            for (fs::recursive_directory_iterator it{store, error}, end; !error && it != end; it.increment(error)) {
                std::error_code entry;
                require(!it->is_symlink(entry), "resource_cache_symlink");
                if (!it->is_regular_file(entry)) continue;
                const auto size = it->file_size(entry);
                if (!entry) bytes += size; // Skips a file another pairing just renamed.
                require(bytes <= store_byte_limit, "resource_cache_full: shared_scene_link_store_exceeds_64_GiB");
            }
            require(!error, "resource_cache_unreadable");
            return bytes;
        };
        std::uintmax_t store_bytes = measure_store();
        const auto check_disk_budget = [&](std::uintmax_t incoming) {
            require(incoming <= store_byte_limit && store_bytes <= store_byte_limit - incoming,
                "resource_cache_full: shared_scene_link_store_exceeds_64_GiB");
        };
        // No client controls directory names, filenames, or cache retention.
        Rows accepted_rows;
        std::string accepted_source, accepted_revision;
        std::map<std::string, std::pair<std::string, LinkMessage>> receipts;
        std::set<std::string> resources;
        std::uint64_t resource_bytes{};
        // Admit already-stored content without transfer. This pairing's resource
        // count and byte capacities apply exactly as if it had been uploaded.
        const auto adopt = [&](const std::string& hash, std::uint64_t length) {
            length = stored_resource(store / (hash + ".glb"), hash, length, stop);
            if (!length) return false;
            require(resources.size() < link_resource_limit && length <= session_byte_limit-resource_bytes, "resource_capacity");
            resources.insert(hash); resource_bytes += length;
            status.metrics.retained_resources = resources.size();
            status.metrics.retained_bytes = resource_bytes;
            return true;
        };
        // Evidence for this pairing, retained across reconnects like receipts.
        std::vector<core::u64> samples;
        publish(status);
        while (!stop.stop_requested() && !shutdown.stop_requested()) {
            std::error_code error;
            asio::ip::tcp::socket socket{io}; acceptor.accept(socket, error);
            if (error == asio::error::would_block || error == asio::error::try_again) {
                std::this_thread::sleep_for(std::chrono::milliseconds{10}); continue;
            }
            if (error) throw std::system_error{error};
            net::FramedStreamLimits limits{net::link_packet_limit, std::chrono::milliseconds{2000},
                std::chrono::milliseconds{30000}, std::chrono::milliseconds{2000}};
            net::TcpFramedStream stream{std::move(socket), limits};
            std::stop_callback cancelled{stop, [&stream] { stream.interrupt(); }};
            std::stop_callback exiting{shutdown, [&stream] { stream.interrupt(); }};
            const auto reply = [&](LinkMessage message) {
                message.epoch = epoch; message.scene = scene_token;
                require(static_cast<bool>(stream.send(net::encode_link_message(message))), "connection_lost");
                return message;
            };
            const auto response = [&](const LinkMessage& m, K kind, std::string text) {
                if (kind == K::Reject || kind == K::Resync) {
                    if (kind == K::Reject) ++status.metrics.refused; else ++status.metrics.resynced;
                    status.pending = false; status.failed = kind == K::Reject && !text.starts_with("source_paused");
                    status.message = (kind == K::Reject ? "Rejected: " : "Resync required: ") + text;
                    publish(status);
                }
                auto r = m; r.kind = kind; r.payload = std::move(text); r.version = net::authoring_link_version;
                return reply(std::move(r));
            };
            bool paired{}, registered{};
            std::set<std::string> negotiated;
            std::string origin, source, source_name, transfer_hash;
            doc::Root shot_cached_document;
            std::string shot_cached_text, shot_cached_revision;
            doc::Root alignment_document;
            scene::Mat4 stage_alignment=scene::identity();
            core::u64 stage_delivered{};
            auto stage_sent=Clock::time_point{};
            std::uint64_t transfer_size{}, transferred{};
            // Revision boundaries: bulk bytes start a revision when they precede
            // its transaction, so the recorded span covers transfer and ingest.
            Clock::time_point transfer_started{}, transfer_finished{}, ingest_started{}, candidate_stored{};
            std::uint64_t revision_bytes{};
            std::ofstream transfer;
            assets::Sha256Hasher hasher;
            try {
                while (!stop.stop_requested() && !shutdown.stop_requested()) {
                    auto packet = stream.receive();
                    if (!packet) break;
                    auto decoded = net::decode_link_message(packet.value());
                    if (!decoded) { LinkMessage bad; response(bad, K::Reject, decoded.error()); break; }
                    const auto& m = decoded.value();
                    if (m.version != net::authoring_link_version) { response(m, K::Reject, "unsupported_version: supported=1"); break; }
                    if (!paired) {
                        if (m.kind != K::Hello || m.origin.empty()) { response(m, K::Reject, "pairing_required"); break; }
                        auto hello = net::parse_link_hello(m.payload);
                        if (!hello) { response(m, K::Reject, hello.error()); break; }
                        if (hello.value().code != status.pairing_code) { response(m, K::Reject, "pairing_required"); break; }
                        paired = true; origin = m.origin;
                        negotiated = {hello.value().capabilities.begin(), hello.value().capabilities.end()};
                        status.metrics.capabilities = net::link_capability_list(hello.value().capabilities);
                        status.metrics.client = hello.value().client.empty()
                            ? std::string{"unidentified publisher"} : hello.value().client;
                        response(m, K::Welcome, net::link_welcome_payload());
                        status.connected = true; status.failed = false;
                        status.message = "Paired: " + origin; publish(status); continue;
                    }
                    auto current = context.load();
                    if (stop.stop_requested()) break;
                    if (!current || current->project != config.context.project || current->target != config.context.target) {
                        response(m, K::Resync, "wrong_scene: pair_again"); break;
                    }
                    if (m.epoch != epoch || m.scene != scene_token || m.origin != origin) {
                        response(m, K::Resync, "epoch_scene_or_origin_mismatch"); continue;
                    }
                    if (m.kind == K::Register) {
                        if (m.payload.size() > 256 || m.payload.find_first_of("\r\n\t") != std::string::npos) {
                            response(m,K::Reject,"invalid_collection_name"); continue;
                        }
                        if (m.source.empty() || (registered && m.source != source)) { response(m, K::Reject, "one_source_per_connection"); continue; }
                        source = m.source; source_name = m.payload; registered = true; status.source_uuid = source; publish(status);
                        auto r = m; r.revision = revision(current->document, source);
                        const bool agreed = m.base == r.revision;
                        response(r, agreed ? K::Ack : K::Resync, agreed ? "registered_revision_agreed" : "revision_mismatch: snapshot_required");
                        continue;
                    }
                    if (!registered || m.source != source) { response(m, K::Reject, "source_registration_required"); continue; }
                    if (m.kind == K::Status) {
                        auto r = m; r.revision = revision(current->document, source);
                        response(r, K::Status, (source_paused(*current, source) ? std::string{"source_paused; "} : std::string{"source_resumed; "}) + status.message); continue;
                    }
                    try {
                        if (!negotiated.contains("stage") && m.kind == K::StageContext)
                            throw std::runtime_error{"capability_not_negotiated: stage; update_the_Blender_add_on_and_Media_Server_together"};
                        if (m.kind == K::StageContext) {
                            LinkedStageSample sample;
                            { std::lock_guard lock{sample_mutex}; sample=latest_sample; }
                            if (sample.scene != current->target.value() || sample.control_frame == stage_delivered
                                || Clock::now()-stage_sent < std::chrono::milliseconds{100}) {
                                response(m,K::StageContext,""); continue;
                            }
                            if (alignment_document.version() != current->document.version()) {
                                stage_alignment=scene::identity();
                                for (const auto& change : doc::diff({},current->document).changes) {
                                    const auto& n=*change.after;
                                    if (n.type != ls::instance_type) continue;
                                    const auto src=n.properties.find(ls::source), sc=n.properties.find(scene::schema::scene_id);
                                    if (src == n.properties.end() || sc == n.properties.end()
                                        || sc->second != doc::PropertyValue{static_cast<core::i64>(current->target.value())}) continue;
                                    const auto* id=std::get_if<core::i64>(&src->second);
                                    const auto* record=id ? current->document.get(doc::NodeId{static_cast<core::u64>(*id)}) : nullptr;
                                    if (!record || property(*record,ls::uuid) != source) continue;
                                    for (std::size_t i=0;i<16;++i) {
                                        const auto v=n.properties.find(std::string{ls::alignment}+std::to_string(i));
                                        if (v != n.properties.end()) if (const auto* f=std::get_if<double>(&v->second)) stage_alignment.m[i]=static_cast<float>(*f);
                                    }
                                    break;
                                }
                                alignment_document=current->document;
                            }
                            stage_delivered=sample.control_frame; stage_sent=Clock::now();
                            response(m,K::StageContext,linked_stage_text(sample,stage_alignment)); continue;
                        }
                        if (m.kind == K::ShotState) {
                            // Media Server's overrides on this scene, for writeback.
                            if (shot_cached_document.version() != current->document.version() || shot_cached_text.empty()) {
                                shot_cached_text = linked_shot_state(*current, source);
                                shot_cached_revision = hash_text(shot_cached_text);
                                shot_cached_document = current->document;
                            }
                            auto r = m; r.revision = shot_cached_revision;
                            response(r, K::ShotState, m.base == shot_cached_revision ? "" : shot_cached_text); continue;
                        }
                        if (m.kind == K::ResourceAnnounce) {
                            require(!source_paused(*current,source), "source_paused: resume_the_collection_then_publish");
                            require(hash_id(m.resource) && m.length > 0 && m.length <= resource_limit, "invalid_resource_size_or_hash");
                            if (transfer_hash == m.resource && transfer_size == m.length) {
                                auto r = m; r.offset = transferred; response(r, K::Ack, "resource_announced"); continue;
                            }
                            require(transfer_hash.empty(), "resource_transfer_in_progress");
                            if (resources.contains(m.resource)) { response(m, K::Ack, "resource_present"); continue; }
                            require(resources.size() < link_resource_limit && m.length <= session_byte_limit-resource_bytes, "resource_capacity");
                            if (adopt(m.resource, m.length)) { response(m, K::Ack, "resource_present"); continue; }
                            check_disk_budget(m.length);
                            transfer_hash = m.resource; transfer_size = m.length; transferred = 0; hasher = {};
                            if (transfer_started == Clock::time_point{}) transfer_started = Clock::now();
                            ++status.metrics.transfers;
                            transfer.open(partial, std::ios::binary | std::ios::trunc);
                            require(transfer.good(), "resource_open_failed");
                            status.pending = true; status.failed = false; status.message = "Receiving updated geometry"; publish(status);
                            response(m, K::Ack, "resource_announced"); continue;
                        }
                        if (m.kind == K::ResourceChunk) {
                            require(!transfer_hash.empty() && m.resource == transfer_hash && m.offset <= transferred, "resource_offset_or_identity");
                            require(!m.payload.empty() && m.payload.size() <= transfer_size-m.offset, "resource_chunk_size");
                            if (m.offset < transferred) {
                                require(m.offset+m.payload.size() <= transferred, "overlapping_resource_chunk");
                                transfer.flush(); std::ifstream previous{partial, std::ios::binary};
                                previous.seekg(static_cast<std::streamoff>(m.offset));
                                std::string bytes(m.payload.size(), '\0'); previous.read(bytes.data(), static_cast<std::streamsize>(bytes.size()));
                                require(previous.good() && bytes == m.payload, "duplicate_resource_chunk_mismatch");
                            } else {
                                transfer.write(m.payload.data(), static_cast<std::streamsize>(m.payload.size()));
                                require(transfer.good(), "resource_write_failed");
                                hasher.update({reinterpret_cast<const core::u8*>(m.payload.data()), m.payload.size()}); transferred += m.payload.size();
                            }
                            status.metrics.peak_pending_bytes = std::max(status.metrics.peak_pending_bytes,
                                revision_bytes + transferred);
                            auto r = m; r.offset = transferred; response(r, K::Ack, "resource_received"); continue;
                        }
                        if (m.kind == K::ResourceComplete) {
                            if (transfer_hash.empty() && resources.contains(m.resource)) { response(m, K::Ack, "resource_complete"); continue; }
                            require(!transfer_hash.empty() && transfer_hash == m.resource && transferred == transfer_size, "resource_incomplete");
                            transfer.close(); require(transfer.good(), "resource_close_failed");
                            require(assets::sha256_hex(hasher.finish()) == transfer_hash, "resource_hash_mismatch");
                            // The first pairing to complete a hash publishes it. A later
                            // one keeps that copy if it verifies, else replaces it.
                            const auto target = store / (transfer_hash+".glb");
                            if (!stored_resource(target, transfer_hash, transfer_size, stop)) {
                                std::error_code renamed;
                                fs::rename(partial, target, renamed);
                                require(!renamed || stored_resource(target, transfer_hash, transfer_size, stop), "resource_store_failed");
                                if (!renamed) store_bytes += transfer_size;
                            }
                            std::error_code ignored; fs::remove(partial, ignored);
                            resources.insert(transfer_hash); resource_bytes += transfer_size; transfer_hash.clear();
                            revision_bytes += transfer_size; transfer_finished = Clock::now();
                            status.metrics.retained_resources = resources.size();
                            status.metrics.retained_bytes = resource_bytes;
                            response(m, K::Ack, "resource_complete"); continue;
                        }
                        require(m.kind == K::Snapshot || m.kind == K::Delta, "unexpected_message");
                        require(!source_paused(*current,source), "source_paused: resume_the_collection_then_publish");
                        require(!m.transaction.empty() && !m.revision.empty() && m.revision != m.base, "invalid_transaction_or_revision");
                        const auto key = origin + '\n' + source + '\n' + m.transaction;
                        const auto digest = hash_text(std::string{reinterpret_cast<const char*>(packet.value().data()), packet.value().size()});
                        if (const auto found = receipts.find(key); found != receipts.end()) {
                            require(found->second.first == digest, "transaction_id_reused_with_different_content");
                            reply(found->second.second); continue;
                        }
                        require(receipts.size() < 256, "receipt_capacity: pair_again_and_resync");
                        if (m.base != revision(current->document, source)) { response(m, K::Resync, "stale_revision: register_again"); continue; }
                        if (m.kind == K::Delta && (accepted_source != source || accepted_revision != m.base)) {
                            response(m, K::Resync, "snapshot_required: delta_baseline_unavailable"); continue;
                        }
                        ingest_started = Clock::now();
                        revision_bytes += m.payload.size();
                        status.metrics.peak_pending_bytes = std::max(status.metrics.peak_pending_bytes, revision_bytes);
                        response(m, K::Status, "received");
                        std::vector<std::pair<std::string, std::string>> absorbed;
                        auto rows = merge_rows(m, accepted_rows, negotiated, absorbed);
                        // Every referenced hash, authored LOD included, is complete in
                        // this pairing or adopted from verified stored bytes.
                        for (const auto& [identity, line] : rows) if (identity.first == "prototype") {
                            std::istringstream in{line}; std::string kind, uuid, hash, path, tag, low; float pixels{};
                            in >> kind >> std::quoted(uuid) >> std::quoted(hash) >> std::quoted(path);
                            std::vector<std::string> needed{hash};
                            if (in >> tag >> pixels >> std::quoted(low) && tag == "lod") needed.push_back(low);
                            for (const auto& h : needed) require(resources.contains(h) || adopt(h, 0), "resource_not_complete");
                        }
                        const auto text = manifest_text(m, rows);
                        check_disk_budget(text.size());
                        { std::ofstream out{staged_manifest, std::ios::binary | std::ios::trunc}; out << text; require(out.good(), "manifest_write_failed"); }
                        status.pending = true; status.failed = false;
                        status.message = "Validating updated content"; publish(status);
                        auto ingested = ingest_linked_scene(staged_manifest.string(), stop,
                            LinkedIngestCache{true, store_package_limit, store_byte_limit});
                        { std::error_code ignored; fs::remove(staged_manifest, ignored); }
                        require(ingested.diagnostic.empty(), ingested.diagnostic.c_str());
                        if (ingested.manifest.ingested != 0U) store_bytes = measure_store();
                        require(!stop.stop_requested(), "pairing_stopped");
                        auto staged = std::make_shared<DirectorLinkCandidate>();
                        staged->manifest = std::move(ingested.manifest); staged->manifest.collection_name = source_name; staged->base = *current;
                        staged->base_revision = m.base; staged->transaction = m.transaction; staged->origin = origin;
                        staged->ticket = next_ticket++; staged->cancellation = stop;
                        staged->absorbed = std::move(absorbed);
                        auto valid = DirectorLinkService::plan(*staged, *current);
                        require(static_cast<bool>(valid), valid ? "" : valid.error().message.c_str());
                        response(m, K::Status, "validated");
                        const auto budget=measure_environment(valid.value(),current->target,stop);
                        require(budget.diagnostic.empty(),budget.diagnostic.c_str());
                        status.message = "Validated " + m.transaction + ": waiting for Director"; publish(status);
                        candidate.store(staged);
                        candidate_stored = Clock::now();
                        std::shared_ptr<const Resolution> result;
                        // One candidate owns the mailbox until resolved. The client
                        // must wait; TCP flow control bounds all subsequent traffic.
                        while (!stop.stop_requested() && !shutdown.stop_requested()) {
                            result = resolution.exchange({});
                            if (result && result->ticket == staged->ticket) break;
                            result.reset(); std::this_thread::sleep_for(std::chrono::milliseconds{2});
                        }
                        if (!result) break;
                        LinkMessage receipt = m;
                        receipt.kind = result->rejection.empty() ? K::Ack : K::Reject;
                        receipt.payload = result->rejection.empty() ? "accepted; live" : result->rejection;
                        receipt.epoch = epoch; receipt.scene = scene_token;
                        receipts.emplace(key, std::pair{digest, receipt});
                        if (result->rejection.empty()) { accepted_rows = std::move(rows); accepted_source = source; accepted_revision = m.revision; }
                        const auto completed = Clock::now();
                        auto& metrics = status.metrics;
                        metrics.last_transfer_ms = elapsed_ms(transfer_started, transfer_finished);
                        metrics.last_ingest_ms = elapsed_ms(ingest_started, candidate_stored);
                        metrics.last_admit_ms = elapsed_ms(candidate_stored, completed);
                        metrics.worst_admit_ms = std::max(metrics.worst_admit_ms, metrics.last_admit_ms);
                        metrics.last_ms = elapsed_ms(transfer_started == Clock::time_point{}
                            ? ingest_started : transfer_started, completed);
                        metrics.retained_receipts = receipts.size();
                        if (result->rejection.empty()) {
                            ++metrics.accepted;
                            if (samples.size() == DirectorLinkMetrics::sample_capacity) samples.erase(samples.begin());
                            samples.push_back(metrics.last_ms);
                            metrics.median_ms = percentile(samples, 0.5);
                            metrics.tail_ms = percentile(samples, 0.95);
                        }
                        transfer_started = transfer_finished = Clock::time_point{}; revision_bytes = 0;
                        status.pending = false;
                        status.failed = !result->rejection.empty() && !result->rejection.starts_with("source_paused");
                        status.message = result->rejection.empty() ? "Accepted " + m.revision
                            : "Rejected: " + result->rejection;
                        publish(status); reply(receipt);
                    } catch (const std::exception& e) {
                        // A bad resource poisons only its unfinished transfer.
                        if (m.kind == K::ResourceAnnounce || m.kind == K::ResourceChunk || m.kind == K::ResourceComplete) {
                            transfer.close(); transfer.clear(); transfer_hash.clear();
                            std::error_code ignored; fs::remove(partial, ignored);
                        }
                        // A refused revision closes its own measured span; the
                        // publisher's retry is timed as the revision it is.
                        if (m.kind == K::Snapshot || m.kind == K::Delta) {
                            transfer_started = transfer_finished = Clock::time_point{}; revision_bytes = 0;
                        }
                        status.pending = false; status.failed = !std::string_view{e.what()}.starts_with("source_paused");
                        status.message = std::string{"Rejected: "}+e.what(); publish(status);
                        response(m, K::Reject, e.what());
                    }
                }
            } catch (const std::exception&) { /* Reconnect recovers the retained receipt or requires a snapshot. */ }
            transfer.close();
            std::error_code ignored; fs::remove(partial, ignored);
            stream.close();
            status.connected = false; status.pending = false;
            constexpr std::string_view disconnected = "; disconnected - reconnect and register revision";
            if (!status.message.ends_with(disconnected)) status.message += disconnected;
            publish(status);
        }
        candidate.store({});
        publish({false, 0, {}, "Scene Link stopped; accepted Preview retained"});
    }
};

DirectorLinkService::DirectorLinkService() : impl_{std::make_unique<Impl>()} {}
DirectorLinkService::~DirectorLinkService() { stop(); impl_->worker.request_stop(); }
void DirectorLinkService::sample(const LinkedStageSample& value) noexcept {
    std::unique_lock lock{impl_->sample_mutex,std::try_to_lock};
    if (lock.owns_lock()) impl_->latest_sample=value;
}
void DirectorLinkService::start(std::string directory, DirectorLinkContext context, unsigned port) {
    stop();
    if (port > 65535) return;
    auto config = std::make_shared<Impl::Start>();
    config->directory = std::move(directory); config->context = context; config->port = port;
    impl_->active = config; update(std::move(context)); impl_->pending.store(std::move(config));
}
void DirectorLinkService::stop() {
    if (impl_->active) impl_->active->stop.request_stop();
    impl_->pending.store({}); impl_->candidate.store({});
}
void DirectorLinkService::update(DirectorLinkContext context) {
    const auto previous = impl_->context.load();
    if (previous && previous->document.version() == context.document.version() && previous->target == context.target
        && previous->program == context.program && previous->project == context.project) return;
    impl_->context.store(std::make_shared<const DirectorLinkContext>(std::move(context)));
}
std::shared_ptr<const DirectorLinkStatus> DirectorLinkService::status() { return impl_->state.load(); }
std::shared_ptr<const DirectorLinkCandidate> DirectorLinkService::take() {
    auto c = impl_->candidate.exchange({});
    return c && !c->cancellation.stop_requested() ? c : nullptr;
}
void DirectorLinkService::resolve(core::u64 ticket, std::string rejection) {
    impl_->resolution.store(std::make_shared<const Impl::Resolution>(Impl::Resolution{ticket, std::move(rejection)}));
}
} // namespace mediaserver::render
