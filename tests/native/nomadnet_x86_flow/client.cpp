#include <microStore/Adapters/UniversalFileSystem.h>
#include <microStore/FileSystem.h>
#include <UDPInterface.h>
#include <microReticulum.h>
#include "TCPClientInterface.h"

#include "NomadNetActionMailbox.h"
#include "NomadNetDocument.h"
#include "NomadNetCompactPage.h"
#include "NomadNetForm.h"
#include "NomadNetHistory.h"
#include "NomadNetLibrary.h"
#include "NomadNetMailbox.h"
#include "NomadNetOwner.h"
#include "NomadNetProtocol.h"
#include "NomadNetUrl.h"
#include "BuildManifest.h"

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace NN = UI::LXMF::NomadNet;

static RNS::Reticulum reticulum({RNS::Type::NONE});
static RNS::Interface network_interface({RNS::Type::NONE});
static TCPClientInterface* tcp_interface = nullptr;
static RNS::Destination destination({RNS::Type::NONE});
static RNS::Link active_link({RNS::Type::NONE});
static RNS::RequestReceipt receipt({RNS::Type::NONE});
static RNS::Identity local_identity({RNS::Type::NONE});
static NN::AsyncMailbox mailbox;
static NN::ActionMailbox actions;
static std::string scenario;
static std::string destination_hex;
static std::string expected_destination_hex;
static bool announce_seen = false;
static bool link_established = false;
static bool request_started = false;
static double request_started_at = 0.0;
static bool application_deadline_fired = false;
static bool completed = false;
static bool passed = false;
static bool progress_seen = false;
static int progress_callbacks = 0;
static bool cancellation_started = false;
static double cancellation_at = 0.0;
static int stale_callback_rejections = 0;
static bool resource_started = false;
static bool resource_progress = false;
static bool receipt_failed = false;
static bool pending_empty = false;
static bool link_closed = false;
static int link_callbacks = 0;
static int reuse_requests = 0;
static NN::PageHistory owner_history;
static NN::PageHistory::PendingOpen owner_pending_history;
static int owner_requests = 0;
static bool owner_submit = false;
static bool owner_history_bytes = false;
static bool owner_retained_link = false;
static bool owner_back_restored = false;
static bool owner_reload_reused = false;
static NN::ExternalVector<uint8_t> owner_first_request;
static NN::OwnerController owner;

class ScreenSubmissionSource final : public NN::OwnerSubmissionSource {
public:
    explicit ScreenSubmissionSource(const char* value) : _value(value) {}
    bool prepare_submission(uint16_t, uint32_t, std::string& target,
                            NN::ExternalVector<uint8_t>& output,
                            NN::FormEncodeResult& result) override {
        NN::DocumentParser parser;
        const auto document = parser.parse(
            "`<name`Initial> `<!|password`Initial> "
            "`<?|color|red|*`Red> `<?|color|blue|*`Blue> "
            "`[:/page/form.mu`name|password|color|fixed=yes`Submit]");
        NN::CompactPage page;
        NN::FormState state;
        if (!page.assign(document) || !state.assign(page) ||
            !state.set_value(0, _value) || !state.set_value(1, "example-pass")) {
            result = NN::FormEncodeResult::ALLOCATION_FAILED;
            return false;
        }
        result = state.encode("name|password|color|fixed=yes", output);
        target = destination_hex + ":/page/form.mu";
        return result == NN::FormEncodeResult::OK;
    }
private:
    const char* _value;
};

static bool prepare_form_request(NN::ExternalVector<uint8_t>& output) {
    NN::DocumentParser parser;
    const auto document = parser.parse(
        "`<name`Initial> `<!|password`Initial> "
        "`<?|color|red|*`Red> `<?|color|blue|*`Blue>");
    NN::CompactPage page;
    NN::FormState state;
    if (!page.assign(document) || !state.assign(page) ||
        !state.set_value(0, "Example User") ||
        !state.set_value(1, "example-pass")) return false;
    return state.encode("name|password|color|fixed=yes", output) ==
        NN::FormEncodeResult::OK;
}

static std::vector<uint8_t> bytes_vector(const RNS::Bytes& bytes) {
    if (bytes.size() == 0) return {};
    return std::vector<uint8_t>(bytes.data(), bytes.data() + bytes.size());
}

static void fail(const char* message) {
    std::printf("FAIL %s\n", message);
    completed = true;
    passed = false;
}

static bool validate_page(const RNS::Bytes& response, bool expect_large) {
    NN::ResponseBuffer normalized;
    if (!NN::normalize_response(response.data(), response.size(), normalized)) {
        fail("response normalization");
        return false;
    }
    const std::string source(reinterpret_cast<const char*>(normalized.bytes().data()), normalized.size());
    NN::Document document = NN::DocumentParser().parse(source);
    if (document.malformed || document.truncated || document.blocks.empty()) {
        fail("Micron parse");
        return false;
    }
    const bool form_scenario = scenario == "form-anonymous" || scenario == "form-identified" ||
                               scenario == "owner-form-history";
    const char* marker = form_scenario ? "Form response" :
        (expect_large ? "Resource-backed page" : "Immediate page");
    std::string lan_heading;
    if (scenario == "lan") {
        for (const auto& block : document.blocks) {
            if (block.type != NN::BlockType::HEADING) continue;
            for (const auto& run : block.runs) lan_heading += run.text;
            if (!lan_heading.empty()) break;
        }
        std::printf("LAN PAGE bytes=%zu blocks=%zu links=%zu heading=%s\n",
                    normalized.size(), document.blocks.size(), document.links.size(),
                    lan_heading.c_str());
        if (source.empty()) {
            fail("LAN page empty");
            return false;
        }
        marker = lan_heading.empty() ? "NomadNet LAN page" : lan_heading.c_str();
    } else if (source.find(marker) == std::string::npos) {
        fail("page content marker");
        return false;
    }

    const std::string path = form_scenario ? "/page/form.mu" :
                             scenario == "lan" ? "/page/index.mu" :
                             scenario == "near-limit" ? "/page/near-limit.mu" :
                             (expect_large ? "/page/resource.mu" : "/page/immediate.mu");
    const std::string url = destination_hex + ":" + path;
    NN::Url parsed;
    std::string error;
    if (!NN::Url::parse(url, parsed, error) || parsed.destination_hex != destination_hex || parsed.path != path) {
        fail("absolute URL parse");
        return false;
    }
    NN::Url relative;
    if (!NN::Url::parse(":/page/next.mu", relative, error, destination_hex) ||
        relative.destination_hex != destination_hex || relative.path != "/page/next.mu") {
        fail("relative URL parse");
        return false;
    }

    NN::Library library;
    if (!library.hear_node(destination_hex, "x86 NomadNet peer", 100, 1) ||
        !library.record_page(url, marker, 101) ||
        !library.set_page_saved(url, true)) {
        fail("library record/save");
        return false;
    }
    const auto encoded = library.encode();
    NN::Library restored;
    if (encoded.empty() || !restored.decode(encoded.data(), encoded.size()) ||
        !restored.page_saved(url) || restored.nodes().size() != 1 || restored.pages().size() != 1) {
        fail("library persistence reload");
        return false;
    }

    // Exercise the Save -> Back ordering used by the owner loop. Save remains
    // durable work, but the terminal action is guaranteed to follow it.
    if (scenario == "owner-form-history") return true;
    if (!actions.publish(NN::UserActionKind::SAVE, url) ||
        !actions.publish(NN::UserActionKind::BACK, "")) {
        fail("Save/Back publication");
        return false;
    }
    NN::UserAction action;
    if (!actions.pop(action) || action.kind != NN::UserActionKind::SAVE ||
        !actions.pop(action) || action.kind != NN::UserActionKind::BACK) {
        fail("Save/Back ordering");
        return false;
    }
    return true;
}

static void on_response(const RNS::RequestReceipt& callback_receipt) {
    const RNS::Bytes token = callback_receipt.get_request_id();
    const RNS::Bytes response = callback_receipt.get_response();
    if (!mailbox.publish_response(bytes_vector(token), response.data(), response.size(),
                                  callback_receipt.response_transfer_size())) {
        ++stale_callback_rejections;
    }
}

static void on_failed(const RNS::RequestReceipt& callback_receipt) {
    const auto token = bytes_vector(callback_receipt.get_request_id());
    const bool published = callback_receipt.response_size() > NN::AsyncMailbox::MAX_WIRE_BYTES
        ? mailbox.publish_oversized(token, callback_receipt.response_size())
        : mailbox.publish_failed(token);
    if (!published) {
        ++stale_callback_rejections;
    }
}

static void on_progress(const RNS::RequestReceipt& callback_receipt) {
    ++progress_callbacks;
    std::printf("CALLBACK progress status=%d transfer=%zu response=%llu progress=%.3f\n",
                static_cast<int>(callback_receipt.get_status()),
                callback_receipt.response_transfer_size(),
                static_cast<unsigned long long>(callback_receipt.response_size()),
                callback_receipt.get_progress());
    if (!mailbox.publish_progress(bytes_vector(callback_receipt.get_request_id()),
                                  callback_receipt.response_transfer_size())) {
        ++stale_callback_rejections;
    }
}

static void on_resource_progress(const RNS::Resource& resource) {
    const float progress = resource.get_progress();
    if (progress <= 0.0f || progress >= 1.0f) return;
    resource_progress = true;
    std::printf("RESOURCE progress=%.3f transfer=%zu data=%zu\n",
                progress, resource.get_transfer_size(), resource.get_data_size());
    if (scenario == "cancel" && !cancellation_started) {
        actions.publish(NN::UserActionKind::BACK, "");
    }
}

static void on_resource_started(const RNS::Resource& resource) {
    resource_started = true;
    std::printf("RESOURCE started transfer=%zu data=%zu\n",
                resource.get_transfer_size(), resource.get_data_size());
    const_cast<RNS::Resource&>(resource).set_progress_callback(on_resource_progress);
}

static void on_link_closed(RNS::Link& closed_link) {
    if (!mailbox.publish_link(bytes_vector(closed_link.link_id()), false)) {
        ++stale_callback_rejections;
    }
}

static bool owner_request_data(const NN::ExternalVector<uint8_t>& request_data,
                               RNS::Link* established = nullptr) {
    RNS::Link& request_link = established ? *established : active_link;
    receipt = request_link.request(RNS::Bytes("/page/form.mu"),
        RNS::Bytes(request_data.data(), request_data.size()), on_response, on_failed,
        on_progress, 8.0, NN::AsyncMailbox::MAX_WIRE_BYTES);
    if (!receipt) return false;
    mailbox.expect_request(bytes_vector(receipt.get_request_id()));
    request_started = true;
    request_started_at = RNS::Utilities::OS::time();
    ++owner_requests;
    return true;
}

static bool owner_request_current(RNS::Link* established = nullptr) {
    return owner_request_data(owner_history.current_request_data(), established);
}

static bool service_submit_through_owner(const char* name, RNS::Link* established = nullptr) {
    NN::UserAction action;
    if (!actions.pop(action) || action.kind != NN::UserActionKind::SUBMIT) return false;
    ScreenSubmissionSource screen(name);
    auto command = owner.service(action, owner_history, screen, 0);
    if (command.result != NN::OwnerResult::REQUEST) return false;
    owner_submit = true;
    if (owner_requests == 0) owner_first_request.assign(
        command.request_data.begin(), command.request_data.end());
    owner_pending_history = std::move(command.pending_history);
    return owner_pending_history.ready() && owner_request_data(command.request_data, established);
}

static void on_link_established(RNS::Link& established_link) {
    // Link construction invokes this callback before the assigning expression
    // completes; take the established production handle before owner requests.
    active_link = established_link;

    link_established = true;
    ++link_callbacks;
    mailbox.begin(bytes_vector(established_link.link_id()));
    established_link.set_resource_started_callback(on_resource_started);
    std::string path;
    double timeout = 8.0;
    if (scenario == "immediate") path = "/page/immediate.mu";
    else if (scenario == "reuse") path = "/page/reuse-first.mu";
    else if (scenario == "lan") path = "/page/index.mu";
    else if (scenario == "resource") path = "/page/resource.mu";
    else if (scenario == "near-limit") path = "/page/near-limit.mu";
    else if (scenario == "oversized") path = "/page/oversized.mu";
    else if (scenario == "cancel") path = "/page/cancel.mu";
    else if (scenario == "form-anonymous" || scenario == "form-identified" ||
             scenario == "owner-form-history")
        path = "/page/form.mu";
    else {
        path = "/page/missing.mu";
        timeout = 1.5;
    }
    NN::ExternalVector<uint8_t> request_data;
    if (scenario == "owner-form-history") {
        if (!actions.publish_submit(0, 1) ||
            !service_submit_through_owner("Example User", &established_link)) {
            fail("production-owner initial Submit");
        }
        return;
    } else if (scenario == "form-anonymous" || scenario == "form-identified") {
        if (!prepare_form_request(request_data)) {
            fail("form request encoding");
            return;
        }
        if (scenario == "form-identified") established_link.identify(local_identity);
    } else {
        const auto nil = NN::no_form_request_data();
        request_data.assign(nil.begin(), nil.end());
    }
    receipt = established_link.request(RNS::Bytes(path),
                                       RNS::Bytes(request_data.data(), request_data.size()),
                                       on_response, on_failed, on_progress, timeout,
                                       NN::AsyncMailbox::MAX_WIRE_BYTES);
    NN::clear_encoded_form(request_data);
    if (!receipt) {
        fail("request creation");
        return;
    }
    mailbox.expect_request(bytes_vector(receipt.get_request_id()));
    request_started = true;
    if (scenario == "reuse") ++reuse_requests;
    request_started_at = RNS::Utilities::OS::time();
}

class NomadAnnounceHandler : public RNS::AnnounceHandler {
public:
    NomadAnnounceHandler() : RNS::AnnounceHandler("nomadnetwork.node") {}

    void received_announce(const RNS::Bytes& destination_hash,
                           const RNS::Identity& identity,
                           const RNS::Bytes&) override {
        if (announce_seen) return;
        if (!expected_destination_hex.empty() && destination_hash.toHex() != expected_destination_hex) return;
        announce_seen = true;
        destination_hex = destination_hash.toHex();
        if (!RNS::Transport::has_path(destination_hash)) {
            fail("announce did not install local path");
            return;
        }
        destination = RNS::Destination(identity, RNS::Type::Destination::OUT,
                                       RNS::Type::Destination::SINGLE,
                                       "nomadnetwork", "node");
        mailbox.prepare();
        active_link = RNS::Link(destination, on_link_established, on_link_closed);
        // Deliberately no link.identify(): ordinary Pyxis page retrieval is anonymous.
    }
};

static RNS::HAnnounceHandler announce_handler(new NomadAnnounceHandler());

static void consume_event() {
    NN::AsyncMailbox::Event event;
    if (!mailbox.take(event)) return;
    switch (event.kind) {
        case NN::AsyncMailbox::Kind::PROGRESS:
            progress_seen = true;
            break;
        case NN::AsyncMailbox::Kind::RESPONSE: {
            if (scenario == "cancel") {
                fail("response arrived after Back cancellation");
                return;
            }
            const RNS::Bytes response(event.data.data(), event.data.size());
            const bool expected_resource = scenario == "resource" || scenario == "near-limit" ||
                                           (scenario == "reuse" && reuse_requests == 2);
            if (expected_resource && progress_callbacks == 0) {
                fail("Resource response had no progress callback");
                return;
            }
            passed = validate_page(response, expected_resource);
            if (!passed) return;
            if (scenario == "owner-form-history") {
                if (owner_pending_history.ready()) {
                    if (!owner_history.commit(std::move(owner_pending_history))) {
                        fail("production-owner response history commit");
                        return;
                    }
                    owner_history_bytes = owner_history.current_has_request_data();
                }
                const bool retained_owner = NN::OwnerController::retain_active_link(
                    destination_hex, destination_hex,
                    active_link && active_link.status() == RNS::Type::Link::ACTIVE);
                if (owner_requests < 3 &&
                    (!retained_owner || !active_link.pending_requests().empty())) {
                    fail("owner response did not retain active Link");
                    return;
                }
                if (owner_requests < 3) owner_retained_link = true;
                if (owner_requests == 1) {
                    mailbox.prepare();
                    if (!actions.publish_submit(0, 2) ||
                        !service_submit_through_owner("Changed User")) {
                        fail("production-owner changed Submit");
                    }
                    break;
                }
                if (owner_requests == 2) {
                    NN::UserAction back_action;
                    back_action.kind = NN::UserActionKind::BACK;
                    ScreenSubmissionSource unused("unused");
                    auto back_command = owner.service(back_action, owner_history, unused, 0);
                    if (back_command.result != NN::OwnerResult::REQUEST ||
                        !back_command.pending_history.ready()) {
                        fail("production-owner Back history restore");
                        return;
                    }
                    if (back_command.request_data != owner_first_request) {
                        fail("production-owner Back exact request bytes");
                        return;
                    }
                    if (!owner_history.commit(std::move(back_command.pending_history)) ||
                        owner_history.current_request_data() != owner_first_request) {
                        fail("production-owner Back publication commit");
                        return;
                    }
                    owner_back_restored = true;
                    NN::UserAction reload_action;
                    reload_action.kind = NN::UserActionKind::RELOAD;
                    auto reload_command = owner.service(reload_action, owner_history, unused, 0);
                    mailbox.prepare();
                    owner_reload_reused = reload_command.result == NN::OwnerResult::REQUEST &&
                        reload_command.request_data == owner_first_request && owner_request_current();
                    if (!owner_reload_reused) fail("production-owner Reload request");
                    break;
                }
                if (owner_requests != 3 || link_callbacks != 1) {
                    fail("production-owner retained-Link Reload invariants");
                    return;
                }
                completed = true;
                break;
            }
            if (scenario == "reuse" && reuse_requests == 1) {
                if (!active_link || active_link.status() != RNS::Type::Link::ACTIVE ||
                    !active_link.pending_requests().empty()) {
                    fail("first reuse request did not conclude on active Link");
                    return;
                }
                mailbox.prepare();
                active_link.set_resource_started_callback(on_resource_started);
                const auto nil = NN::no_form_request_data();
                receipt = active_link.request(RNS::Bytes("/page/reuse-second.mu"),
                    RNS::Bytes(nil.data(), nil.size()), on_response, on_failed,
                    on_progress, 8.0, NN::AsyncMailbox::MAX_WIRE_BYTES);
                if (!receipt) {
                    fail("second request creation on retained Link");
                    return;
                }
                mailbox.expect_request(bytes_vector(receipt.get_request_id()));
                ++reuse_requests;
                request_started_at = RNS::Utilities::OS::time();
                break;
            }
            if (scenario == "reuse" &&
                (reuse_requests != 2 || link_callbacks != 1 ||
                 !active_link.pending_requests().empty())) {
                fail("same-destination Link reuse invariants");
                return;
            }
            completed = true;
            break;
        }
        case NN::AsyncMailbox::Kind::FAILED:
            if (scenario != "timeout") {
                fail("unexpected request failure");
                return;
            }
            passed = true;
            completed = true;
            break;
        case NN::AsyncMailbox::Kind::OVERSIZED:
            std::printf("EVENT oversized transfer=%zu\n", event.transfer_size);
            if (scenario == "oversized" && event.transfer_size > NN::AsyncMailbox::MAX_WIRE_BYTES) {
                passed = true;
                completed = true;
            } else {
                fail("unexpected oversized response");
            }
            break;
        case NN::AsyncMailbox::Kind::LINK_CLOSED:
            if (!cancellation_started && scenario != "timeout") fail("unexpected link close");
            break;
        default:
            break;
    }
}

static void service_terminal_action() {
    if (!actions.terminal_pending()) return;
    NN::UserAction action;
    if (!actions.pop(action) || action.kind != NN::UserActionKind::BACK) {
        fail("terminal Back dequeue");
        return;
    }
    cancellation_started = true;
    cancellation_at = RNS::Utilities::OS::time();
    mailbox.seal();
    if (active_link && active_link.status() != RNS::Type::Link::CLOSED) active_link.teardown();
    if (receipt && receipt.get_status() != RNS::Type::RequestReceipt::FAILED) {
        receipt.request_timed_out(RNS::PacketReceipt(RNS::Type::NONE));
    }
}

static void service_application_deadline() {
    application_deadline_fired = true;
    mailbox.seal();
    if (active_link && active_link.status() != RNS::Type::Link::CLOSED) active_link.teardown();
    if (receipt && receipt.get_status() != RNS::Type::RequestReceipt::FAILED) {
        receipt.request_timed_out(RNS::PacketReceipt(RNS::Type::NONE));
    }
}

static bool cleanup_complete() {
    receipt_failed = receipt && receipt.get_status() == RNS::Type::RequestReceipt::FAILED;
    pending_empty = active_link && active_link.pending_requests().empty();
    link_closed = active_link && active_link.status() == RNS::Type::Link::CLOSED;
    return receipt_failed && pending_empty && link_closed && stale_callback_rejections > 0;
}

int main(int argc, char** argv) {
    if (argc == 2 && std::strcmp(argv[1], "--manifest") == 0) {
        std::printf("{\"schema\":1,\"base\":\"%s\",\"branch\":\"%s\","
                    "\"microreticulum\":\"%s\",\"sources\":%s}\n",
                    PYXIS_MANIFEST_BASE, PYXIS_MANIFEST_BRANCH,
                    PYXIS_MANIFEST_MICRORETICULUM, PYXIS_MANIFEST_SOURCES_JSON);
        return 0;
    }
    if (argc < 2) {
        std::fprintf(stderr, "usage: %s immediate|resource|near-limit|oversized|timeout|cancel|reuse|form-anonymous|form-identified|owner-form-history | lan host port destination\n", argv[0]);
        return 2;
    }
    scenario = argv[1];
    if (scenario != "immediate" && scenario != "resource" && scenario != "near-limit" &&
        scenario != "oversized" &&
        scenario != "timeout" && scenario != "cancel" && scenario != "reuse" &&
        scenario != "form-anonymous" && scenario != "form-identified" &&
        scenario != "owner-form-history" && scenario != "lan") return 2;
    if ((scenario == "lan" && argc != 5) || (scenario != "lan" && argc != 2)) return 2;

    microStore::FileSystem filesystem{microStore::Adapters::UniversalFileSystem(".")};
    filesystem.init();
    RNS::Utilities::OS::register_filesystem(filesystem);
    if (scenario == "lan") {
        expected_destination_hex = argv[4];
        tcp_interface = new TCPClientInterface("NomadNetLAN");
        tcp_interface->set_target_host(argv[2]);
        tcp_interface->set_target_port(std::stoi(argv[3]));
        network_interface = RNS::Interface(tcp_interface);
    } else {
        network_interface = new UDPInterface();
        network_interface.mode(RNS::Type::Interface::MODE_GATEWAY);
    }
    RNS::Transport::register_interface(network_interface);
    if (!network_interface.start()) return 3;
    reticulum = RNS::Reticulum();
    reticulum.transport_enabled(false);
    reticulum.start();
    local_identity = RNS::Identity();
    RNS::Transport::register_announce_handler(announce_handler);
    if (scenario == "lan") {
        RNS::Bytes target;
        target.assignHex(expected_destination_hex.c_str());
        RNS::Transport::request_path(target, network_interface);
        std::printf("LAN TCP online=%d target=%s destination=%s path-requested=1\n",
                    network_interface.online() ? 1 : 0, argv[2], expected_destination_hex.c_str());
    }

    const double started_at = RNS::Utilities::OS::time();
    while (!completed && RNS::Utilities::OS::time() - started_at < 15.0) {
        // Mirrors the corrected firmware ordering: terminal cancellation gets
        // an owner-loop chance before another inbound transport pass.
        service_terminal_action();
        reticulum.loop();
        consume_event();
        const double now = RNS::Utilities::OS::time();
        if (request_started && scenario == "timeout" && !application_deadline_fired &&
            now - request_started_at > 2.0) {
            service_application_deadline();
        }
        const double cleanup_at = cancellation_started ? cancellation_at : request_started_at + 2.0;
        if ((cancellation_started || application_deadline_fired) && cleanup_complete() &&
            now - cleanup_at > 0.25) {
            passed = scenario == "timeout" ||
                     (scenario == "cancel" && resource_started && resource_progress);
            completed = true;
        }
        RNS::Utilities::OS::sleep(0.005);
    }

    mailbox.seal();
    if (active_link && active_link.status() != RNS::Type::Link::CLOSED) active_link.teardown();
    const double drain_until = RNS::Utilities::OS::time() + 0.25;
    while (RNS::Utilities::OS::time() < drain_until) {
        reticulum.loop();
        RNS::Utilities::OS::sleep(0.005);
    }
    RNS::Transport::deregister_announce_handler(announce_handler);
    RNS::Transport::deregister_interface(network_interface);

    if (!completed) fail("scenario deadline");
    cleanup_complete();
    std::printf("RESULT scenario=%s announce=%d path=%d link=%d request=%d progress=%d callbacks=%d "
                "cancel=%d deadline=%d resource_started=%d resource_progress=%d receipt_failed=%d "
                "pending=%zu link_closed=%d stale_rejected=%d reuse_requests=%d link_callbacks=%d "
                "owner_submit=%d history_bytes=%d retained_link=%d back_restored=%d reload_reused=%d passed=%d\n",
                scenario.c_str(), announce_seen ? 1 : 0,
                destination_hex.empty() ? 0 : 1, link_established ? 1 : 0,
                request_started ? 1 : 0, progress_seen ? 1 : 0, progress_callbacks,
                cancellation_started ? 1 : 0, application_deadline_fired ? 1 : 0,
                resource_started ? 1 : 0, resource_progress ? 1 : 0,
                receipt_failed ? 1 : 0,
                active_link ? active_link.pending_requests().size() : 0,
                link_closed ? 1 : 0, stale_callback_rejections, reuse_requests, link_callbacks,
                owner_submit ? 1 : 0, owner_history_bytes ? 1 : 0,
                owner_retained_link ? 1 : 0, owner_back_restored ? 1 : 0,
                owner_reload_reused ? 1 : 0, passed ? 1 : 0);
    if (scenario == "lan") {
        std::printf("LAN TRANSPORT rx=%zu rxbytes=%zu tx=%zu txbytes=%zu\n",
                    network_interface.rx(), network_interface.rxbytes(),
                    network_interface.tx(), network_interface.txbytes());
    }
    return passed ? 0 : 1;
}
