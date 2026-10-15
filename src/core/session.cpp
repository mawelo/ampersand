// session.cpp — Protokoll-Session (siehe session.hpp)
//
// Wer:      Martin.Lonkwitz
// Wann:     2026-09-04
// Projekt:  Ampersand
// Historie: git log
//
// Ablauf-Logik:
//   RX-Thread: transport.read -> Reassembler -> decode_body -> [gunzip] ->
//              Inbound -> (Waiter | Collectors) -> Listener. Verschluesselte
//              Nachrichten werden nicht unterstuetzt: sie bleiben roh und
//              tragen decrypt_failed.
//   Handshake: ResetCommsBuffers{session_id} -> Version READ
//              -> Version UPDATE{cortex_control_version} -> ModelRepo READ ->
//              Connection{true} -> READ-Burst (Subscriptions) -> settle
//   Keepalive: KeepAlive{UPDATE, is_online} jede Sekunde
// Korrektheit: Der RX-Thread stirbt nie an einer fehlerhaften Nachricht; jede
//   Stufe faengt ihren Fehler und setzt den Reassembler zurueck.
#include "qc/session.hpp"

#include "qc/compression.hpp"
#include "qc/log.hpp"

#include <ProductionAutomation.pb.h>

#include <algorithm>
#include <array>
#include <format>
#include <random>

namespace qc {

namespace pa = cortex_protobuf_v2;
using namespace std::chrono_literals;

namespace {

// The state types Cortex Control subscribes to right after Connection{true}.
// A READ acts as a subscription: the device pushes that state to clients that
// asked for it. Mirrors pyquadcortex's _SUBSCRIBE_TYPES.
constexpr std::array<MessageType, 22> kSubscribeTypes{
    MessageType::ModuleStats,     MessageType::License,        MessageType::UndoRedo,
    MessageType::IOSettings,      MessageType::GeneralSettings, MessageType::ShowGigView,
    MessageType::Mode,            MessageType::GlobalEQ,       MessageType::MasterVolume,
    MessageType::File,            MessageType::RecentsFavorites, MessageType::CompilerInhibitedModules,
    MessageType::RecallPreset,    MessageType::NewModels,      MessageType::PinnedModels,
    MessageType::DefaultParameters, MessageType::GlobalTempo,  MessageType::SetlistPosition,
    MessageType::PresetDirty,     MessageType::Scene,          MessageType::BulkOperation,
    MessageType::Updater,
};

constexpr std::array<std::uint8_t, 2> kActionRead{0x08, 0x03};  // field 1 varint = READ(3)

/// Zufaellige Sitzungskennung: `bytes` Zufallsbytes als Kleinbuchstaben-Hex
/// (2 * bytes Zeichen). Nur Korrelation des ResetCommsBuffers-Echos, kein
/// Geheimnis - std::random_device genuegt.
std::string random_hex(std::size_t bytes) {
    static constexpr std::string_view kDigits = "0123456789abcdef";
    std::random_device rd;
    std::uniform_int_distribution<int> dist(0, 255);
    std::string out;
    out.reserve(bytes * 2);
    for (std::size_t i = 0; i < bytes; ++i) {
        const auto b = static_cast<std::size_t>(dist(rd));
        out.push_back(kDigits[b >> 4U]);
        out.push_back(kDigits[b & 0x0fU]);
    }
    return out;
}

}  // namespace

Session::Session(std::unique_ptr<Transport> transport, Options options)
    : transport_(std::move(transport)), options_(std::move(options)) {}

Session::~Session() { stop(); }

void Session::start() {
    if (started_.exchange(true)) return;
    rx_thread_ = std::jthread([this](std::stop_token st) { rx_loop(st); });
}

void Session::stop() {
    if (keepalive_thread_.joinable()) {
        keepalive_thread_.request_stop();
        cv_.notify_all();
        keepalive_thread_.join();
    }
    if (rx_thread_.joinable()) {
        rx_thread_.request_stop();
        rx_thread_.join();
    }
    started_ = false;
    connected_ = false;
}

std::string Session::lost_reason() const {
    std::lock_guard lk{mutex_};
    return lost_reason_;
}

DeviceVersion Session::device_version() const {
    std::lock_guard lk{mutex_};
    return version_;
}

Session::Stats Session::stats() const {
    std::lock_guard lk{mutex_};
    return stats_;
}

std::optional<int> Session::midi_channel() const {
    std::lock_guard lk{mutex_};
    return midi_channel_;
}

std::optional<std::vector<std::uint8_t>> Session::model_repo_payload() const {
    std::lock_guard lk{mutex_};
    return model_repo_;
}

void Session::set_listener(Listener listener) {
    std::lock_guard dl{dispatch_mutex_};  // wartet, bis ein laufender Aufruf fertig ist
    std::lock_guard lk{callback_mutex_};
    listener_ = std::move(listener);
}

void Session::set_lost_handler(LostHandler handler) {
    std::lock_guard dl{dispatch_mutex_};
    std::lock_guard lk{callback_mutex_};
    lost_handler_ = std::move(handler);
}

// ── outbound ────────────────────────────────────────────────────────────────

std::expected<void, std::string> Session::write_reports(const std::vector<framing::Report>& reports) {
    if (lost_.load()) return std::unexpected("device lost: " + lost_reason());
    std::lock_guard lk{write_mutex_};
    for (const auto& r : reports) {
        if (!transport_->write(r)) {
            return std::unexpected("HID write failed (not the benign STALL) - device gone?");
        }
    }
    {
        std::lock_guard slk{mutex_};
        ++stats_.sent_messages;
        stats_.sent_reports += reports.size();
    }
    return {};
}

std::expected<void, std::string> Session::send_raw(MessageType type, std::span<const std::uint8_t> payload) {
    log::debug("tx {} ({} bytes): {}", to_string(type), payload.size(), framing::hex(payload, 24));
    return write_reports(framing::encode(raw(type), payload));
}

std::expected<void, std::string> Session::send(MessageType type, const google::protobuf::MessageLite& msg) {
    std::string bytes;
    if (!msg.SerializeToString(&bytes)) return std::unexpected("protobuf serialization failed");
    const std::span<const std::uint8_t> payload(reinterpret_cast<const std::uint8_t*>(bytes.data()),  // unsafe{}
                                                bytes.size());
    return send_raw(type, payload);
}

std::expected<void, std::string> Session::send_read(MessageType type) { return send_raw(type, kActionRead); }

// ── correlation ─────────────────────────────────────────────────────────────

std::expected<Inbound, std::string> Session::await(MessageType type, const std::function<void()>& trigger,
                                                   std::chrono::milliseconds quiet, Predicate match) {
    if (lost_.load()) return std::unexpected("device lost: " + lost_reason());
    auto waiter = std::make_shared<Waiter>(Waiter{type, std::move(match), std::nullopt});
    {
        std::lock_guard lk{mutex_};
        waiters_.push_back(waiter);
    }
    trigger();
    // `quiet` budgets SILENCE, not total time. The unit queues our request
    // behind its seed push and answers 60 ms after its own last message, but
    // the total that takes swings between 12 and 16 s (evidence/78). Giving up
    // while it is still sending would mean walking out on a device that is
    // merely busy; await_limit still bounds the whole wait.
    const auto start = std::chrono::steady_clock::now();
    const auto limit = start + std::min(options_.await_limit, quiet * 8);
    std::unique_lock lk{mutex_};
    while (!waiter->result && !lost_.load()) {
        const auto deadline = std::min(std::max(start, last_rx_) + quiet, limit);
        if (std::chrono::steady_clock::now() >= deadline) break;
        cv_.wait_until(lk, deadline);
    }
    std::erase(waiters_, waiter);
    if (waiter->result) return std::move(*waiter->result);
    if (lost_.load()) return std::unexpected("device lost: " + lost_reason_);
    const auto waited = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start);
    return std::unexpected(std::format("no {} reply: device silent for {} ms (waited {} ms)", to_string(type),
                                       quiet.count(), waited.count()));
}

std::vector<Inbound> Session::collect(MessageType type, const std::function<void()>& trigger,
                                      std::chrono::milliseconds duration, Predicate match) {
    std::vector<Inbound> bucket;
    Collector c{type, std::move(match), &bucket};
    {
        std::lock_guard lk{mutex_};
        collectors_.push_back(&c);
    }
    trigger();
    const auto deadline = std::chrono::steady_clock::now() + duration;
    {
        std::unique_lock lk{mutex_};
        cv_.wait_until(lk, deadline, [&] { return lost_.load(); });
        std::erase(collectors_, &c);
    }
    return bucket;
}

// ── inbound ─────────────────────────────────────────────────────────────────

void Session::rx_loop(std::stop_token st) {
    std::array<std::uint8_t, 512> buf{};
    int consecutive_losses = 0;
    while (!st.stop_requested()) {
        const auto r = transport_->read(buf, options_.read_timeout);
        switch (r.status) {
            case Transport::ReadStatus::Timeout:
                consecutive_losses = 0;
                continue;
            case Transport::ReadStatus::Lost:
                if (++consecutive_losses >= 2) {
                    mark_lost(r.error);
                    // Nothing more will ever arrive; idle until stop().
                    while (!st.stop_requested()) std::this_thread::sleep_for(50ms);
                    return;
                }
                reassembler_.reset();
                continue;
            case Transport::ReadStatus::Data:
                break;
        }
        consecutive_losses = 0;
        {
            std::lock_guard lk{mutex_};
            ++stats_.received_reports;
        }
        if (r.size < 3) continue;
        auto pushed = reassembler_.push(std::span<const std::uint8_t>(buf.data(), r.size));
        if (!pushed) {
            log::warn("rx framing: {}", pushed.error());
            std::lock_guard lk{mutex_};
            ++stats_.decode_errors;
            continue;
        }
        if (!*pushed) continue;
        handle_body(std::move(**pushed));
    }
}

void Session::handle_body(std::vector<std::uint8_t> body) {
    auto env = framing::decode_body(body);
    if (!env) {
        log::warn("rx envelope: {}", env.error());
        std::lock_guard lk{mutex_};
        ++stats_.decode_errors;
        return;
    }
    Inbound in;
    in.type = message_type_from_raw(env->type);
    in.encrypted = env->encrypted;
    in.compressed = env->compressed;
    in.at = std::chrono::steady_clock::now();
    in.payload = std::move(env->payload);

    if (in.encrypted) {
        // Verschluesselte Nachrichten werden nicht unterstuetzt: Nutzlast bleibt
        // roh, der Empfaenger erkennt das an decrypt_failed.
        in.decrypt_failed = true;
        std::lock_guard lk{mutex_};
        ++stats_.decrypt_failures;
    }
    if (!in.decrypt_failed && compression::is_gzip(in.payload)) {
        if (auto plain = compression::decompress_if_gzip(in.payload)) {
            in.payload = std::move(*plain);
        } else {
            log::debug("rx {}: gunzip failed: {}", to_string(in.type), plain.error());
        }
    }
    log::debug("rx {} ({} bytes{}{}): {}", to_string(in.type), in.payload.size(), in.encrypted ? ", enc" : "",
               in.compressed ? ", gz" : "", framing::hex(in.payload, 24));
    dispatch(std::move(in));
}

void Session::dispatch(Inbound in) {
    // Handshake bookkeeping first (cheap parses, only for two types).
    if (in.type == MessageType::Version && !in.decrypt_failed) auto_reply_version(in);
    // Den MIDI-Kanal im Vorbeigehen mitnehmen: Die GeneralSettings kommen im
    // Handshake ohnehin, und wer ihn spaeter eigens abfragt, wartet auf den
    // Seed-Push. Das Geraet zaehlt 1..16, die Leitung 0..15.
    if (in.type == MessageType::GeneralSettings && !in.decrypt_failed) {
        if (auto m = in.parse<pa::GeneralSettingsMessage>(); m && m->has_midi_channel()) {
            std::lock_guard lk{mutex_};
            midi_channel_ = std::clamp(m->midi_channel() - 1, 0, 15);
        }
    }
    if (in.type == MessageType::ModelRepo && !in.decrypt_failed) {
        if (auto m = in.parse<pa::ModelRepoMessage>(); m && m->has_model_repo_payload() && !m->model_repo_payload().empty()) {
            std::lock_guard lk{mutex_};
            const auto& p = m->model_repo_payload();
            model_repo_ = std::vector<std::uint8_t>(p.begin(), p.end());
        }
    }

    bool delivered = false;
    {
        std::lock_guard lk{mutex_};
        ++stats_.received_messages;
        last_rx_ = in.at;  // a waiter's silence budget starts over
        for (auto* c : collectors_) {
            if (c->type == in.type && (!c->match || c->match(in))) {
                c->bucket->push_back(in);
                delivered = true;
            }
        }
        for (auto& w : waiters_) {
            if (w->result) continue;
            if (w->type == in.type && (!w->match || w->match(in))) {
                w->result = in;
                delivered = true;
                break;  // one waiter per message
            }
        }
        if (!delivered) ++stats_.undelivered;
    }
    cv_.notify_all();

    // Waehrend des Aufrufs gehalten: set_listener() kehrt erst zurueck, wenn
    // kein Aufruf mehr laeuft (Lebensdauer der Captures beim Aufrufer).
    std::lock_guard dl{dispatch_mutex_};
    Listener listener;
    {
        std::lock_guard lk{callback_mutex_};
        listener = listener_;
    }
    if (listener) {
        try {
            listener(in);
        } catch (const std::exception& e) {
            log::error("listener threw: {}", e.what());
        }
    }
}

std::string Session::announced_version() const {
    if (options_.cortex_control_version != "auto") return options_.cortex_control_version;
    const auto z = device_version().zenos_git_hash;
    const bool looks_like_version = !z.empty() && z.size() <= 12 &&
                                    std::all_of(z.begin(), z.end(), [](char c) { return std::isdigit(static_cast<unsigned char>(c)) || c == '.'; });
    return looks_like_version ? z : options_.fallback_cortex_control_version;
}

void Session::auto_reply_version(const Inbound& in) {
    auto v = in.parse<pa::VersionMessage>();
    if (!v) return;
    if (v->action() == pa::MessageAction::READ && !v->has_app_fw_version()) {
        // The device asks the host for its version; Cortex Control answers with
        // its own version string, which opens the push gate.
        pa::VersionMessage reply;
        reply.set_action(pa::MessageAction::UPDATE);
        reply.set_cortex_control_version(announced_version());
        (void)send(MessageType::Version, reply);
        return;
    }
    if (v->has_app_fw_version()) {
        DeviceVersion dv;
        dv.app_fw_version = v->app_fw_version();
        dv.zenos_git_hash = v->zenos_git_hash();
        dv.linux_kernel_version = v->linux_kernel_version();
        dv.uboot_version = v->uboot_version();
        dv.bootloader_fw_version = v->bootloader_fw_version();
        dv.serial_number = v->device_serial_number();
        dv.mac_address = v->mac_address();
        dv.custom_name = v->custom_name();
        if (v->has_comms_version() && !v->comms_version().empty()) {
            const auto& blob = v->comms_version();
            dv.comms_version_hex = framing::hex(std::span<const std::uint8_t>(reinterpret_cast<const std::uint8_t*>(blob.data()), blob.size()));  // unsafe{}
        }
        std::lock_guard lk{mutex_};
        version_ = dv;
    }
}

void Session::mark_lost(std::string reason) {
    {
        std::lock_guard lk{mutex_};
        lost_reason_ = reason;
    }
    lost_ = true;
    connected_ = false;
    cv_.notify_all();
    log::warn("device lost: {}", reason);
    std::lock_guard dl{dispatch_mutex_};
    LostHandler handler;
    {
        std::lock_guard lk{callback_mutex_};
        handler = lost_handler_;
    }
    if (handler) handler(reason);
}

// ── handshake ───────────────────────────────────────────────────────────────

std::expected<void, std::string> Session::handshake_once() {
    const auto timeout = options_.handshake_timeout;

    // 1. ResetCommsBuffers with a fresh session id; the device echoes it.
    pa::ResetCommsBuffersMessage reset;
    const std::string session_id = random_hex(16);
    reset.set_session_id(session_id);
    auto echo = await(
        MessageType::ResetCommsBuffers, [&] { (void)send(MessageType::ResetCommsBuffers, reset); }, timeout,
        [&](const Inbound& i) {
            auto m = i.parse<pa::ResetCommsBuffersMessage>();
            return m && m->session_id() == session_id;
        });
    if (!echo) return std::unexpected("ResetCommsBuffers: " + echo.error());

    // 2. Version READ -> the device's version blob.
    auto version = await(
        MessageType::Version, [&] { (void)send_read(MessageType::Version); }, timeout,
        [](const Inbound& i) {
            auto m = i.parse<pa::VersionMessage>();
            return m && m->has_app_fw_version();
        });
    if (!version) return std::unexpected("Version: " + version.error());
    // auto_reply_version() already stored version_ on the RX thread.

    // 3. Announce our Cortex Control version (opens the push gate).
    pa::VersionMessage announce;
    announce.set_action(pa::MessageAction::UPDATE);
    announce.set_cortex_control_version(announced_version());
    if (auto r = send(MessageType::Version, announce); !r) return r;

    // 4. ModelRepo READ (readiness gate; the payload is kept for the catalog).
    if (auto r = send_read(MessageType::ModelRepo); !r) return r;

    // 5. Connection{connected: true}.
    pa::ConnectionMessage conn;
    conn.set_connected(true);
    if (auto r = send(MessageType::Connection, conn); !r) return r;

    // 6. Subscription burst.
    if (options_.subscribe_state) {
        for (auto t : kSubscribeTypes) {
            if (auto r = send_read(t); !r) return r;
        }
    }
    return {};
}

std::expected<void, std::string> Session::connect() {
    if (!started_.load()) start();
    if (connected_.load()) return {};
    if (lost_.load()) return std::unexpected("device lost: " + lost_reason());

    const auto deadline = std::chrono::steady_clock::now() + options_.handshake_patience;
    int attempt = 0;
    for (;;) {
        ++attempt;
        auto r = handshake_once();
        if (r) break;
        if (lost_.load() || std::chrono::steady_clock::now() >= deadline) {
            return std::unexpected(std::format(
                "handshake failed after {} attempt(s): {} (the device can stay silent 10-20 s after boot; "
                "quit Cortex Control if it is running)",
                attempt, r.error()));
        }
        log::info("handshake attempt {} failed ({}); retrying", attempt, r.error());
    }
    std::this_thread::sleep_for(options_.settle);
    connected_ = true;
    keepalive_thread_ = std::jthread([this](std::stop_token st) { keepalive_loop(st); });
    log::info("connected to {} (fw {}, serial {})", transport_->description(), device_version().app_fw_version,
              device_version().serial_number);
    return {};
}

void Session::disconnect() {
    if (keepalive_thread_.joinable()) {
        keepalive_thread_.request_stop();
        cv_.notify_all();
        keepalive_thread_.join();
    }
    if (connected_.exchange(false) && !lost_.load()) {
        pa::ConnectionMessage conn;
        conn.set_connected(false);
        (void)send(MessageType::Connection, conn);
    }
}

void Session::keepalive_loop(std::stop_token st) {
    std::mutex local;
    std::unique_lock lk{local};
    std::condition_variable_any cv;
    while (!st.stop_requested()) {
        if (cv.wait_for(lk, st, options_.keepalive_interval, [&] { return st.stop_requested(); })) return;
        if (lost_.load() || !connected_.load()) continue;
        pa::KeepAliveMessage ka;
        ka.set_action(pa::MessageAction::UPDATE);
        ka.set_is_online(true);
        if (auto r = send(MessageType::KeepAlive, ka); !r) log::debug("keepalive: {}", r.error());
    }
}

}  // namespace qc
