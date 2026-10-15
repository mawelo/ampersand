// session.hpp — Protokoll-Session: Reports <-> korrelierte Nachrichten
//
// Wer:      Martin.Lonkwitz
// Was:      Besitzt den Transport, den RX-Thread (Reassemblierung,
//           Dekompression, Zustellung an Warter/Sammler/Listener), den
//           Keepalive-Thread und den Verbindungs-Handshake. Kennt Protobuf nur
//           als serialisierte Bytes plus die wenigen Handshake-Nachrichten.
// Wann:     2026-09-04
// Warum:    Das Geraet antwortet nicht "auf eine Anfrage", sondern pusht
//           Nachrichten nach Typ; Korrelation ist typ-basiert mit request_id
//           als Plausibilitaetspruefung (docs/protocol.md, Abschnitt 5).
// Projekt:  Ampersand
// Abhaengig: framing, compression, transport, generierte Protobuf-Klassen
// Nutzung:  qc::Session s(std::move(transport)); s.start(); auto ok = s.connect();
//           auto reply = s.request(MessageType::Version, VersionMessage{...}, 5s);
// Historie: git log
#pragma once

#include "qc/framing.hpp"
#include "qc/messages.hpp"
#include "qc/transport.hpp"

#include <google/protobuf/message_lite.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <expected>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

namespace qc {

/// One decoded inbound message.
struct Inbound {
    MessageType type{MessageType::Undefined};
    std::vector<std::uint8_t> payload;  ///< protobuf bytes (decompressed when possible)
    bool encrypted{false};
    bool compressed{false};
    bool decrypt_failed{false};         ///< encrypted: not supported in this build, payload is raw
    std::chrono::steady_clock::time_point at{};

    /// Parse the payload into a generated message type.
    template <class M>
    [[nodiscard]] std::optional<M> parse() const {
        M m;
        if (!m.ParseFromArray(payload.data(), static_cast<int>(payload.size()))) return std::nullopt;
        return m;
    }
};

struct DeviceVersion {
    std::string app_fw_version;
    std::string zenos_git_hash;
    std::string linux_kernel_version;
    std::string uboot_version;
    std::string bootloader_fw_version;
    std::string serial_number;
    std::string mac_address;
    std::string custom_name;
    std::string comms_version_hex;  ///< raw comms_version blob as sent (for offline analysis)
};

struct SessionOptions {
    std::chrono::milliseconds read_timeout{200};
    std::chrono::milliseconds keepalive_interval{1000};
    std::chrono::milliseconds handshake_timeout{5000};
    std::chrono::milliseconds handshake_patience{30000};  ///< retry budget while the device is openable but silent
    std::chrono::milliseconds settle{2000};
    /// Hard cap for await() when the device never falls silent. The cap grows
    /// with the call's silence budget (eight times it) but never exceeds this:
    /// a caller that allows only a short silence wants to fail fast, one that
    /// allows a long one is prepared to sit through a seed push.
    std::chrono::milliseconds await_limit{60000};
    /// Version announced as Cortex Control. "auto" = the device's own zenos
    /// version (the unit answers VERSION_IS_OLDER to anything below it, and
    /// `cortex_control_version_valid: true` to a match).
    std::string cortex_control_version{"auto"};
    /// Greift nur, wenn die Zenos-Angabe des Geraets keine reine Versionszahl
    /// ist. ZenUI 4.1.1 vergleicht die angekuendigte Fassung gegen die
    /// Konstante "4.1.0" und protokolliert alles darunter als
    /// "Cortex-Control version is incompatible" (Beleg 144: Vergleichsroutine
    /// bei 0x6f4ec8, Konstante bei 0xa28358). Der fruehere Rueckfallwert
    /// "4.0.1" lag darunter. Gemessen aendert die angekuendigte Fassung am
    /// IOMeter-Verhalten nichts - der Wert ist Vorsorge, keine Loesung.
    std::string fallback_cortex_control_version{"4.1.0"};
    bool subscribe_state{true};  ///< send the READ burst Cortex Control sends
};

class Session {
public:
    using Options = SessionOptions;

    struct Stats {
        std::uint64_t sent_messages{0};
        std::uint64_t sent_reports{0};
        std::uint64_t received_messages{0};
        std::uint64_t received_reports{0};
        std::uint64_t decode_errors{0};
        std::uint64_t decrypt_failures{0};
        std::uint64_t undelivered{0};
    };

    using Predicate = std::function<bool(const Inbound&)>;
    using Listener = std::function<void(const Inbound&)>;
    using LostHandler = std::function<void(const std::string&)>;

    Session(std::unique_ptr<Transport> transport, Options options = {});
    ~Session();
    Session(const Session&) = delete;
    Session& operator=(const Session&) = delete;

    /// Start the RX thread. Idempotent.
    void start();
    /// Stop RX and keepalive threads. Sends nothing.
    void stop();

    /// The connect handshake Cortex Control performs (see docs/protocol.md §4).
    /// Starts the keepalive on success.
    [[nodiscard]] std::expected<void, std::string> connect();
    /// Announce departure (Connection{connected:false}) and stop the keepalive.
    void disconnect();

    [[nodiscard]] bool connected() const noexcept { return connected_.load(); }
    [[nodiscard]] bool lost() const noexcept { return lost_.load(); }
    [[nodiscard]] std::string lost_reason() const;
    [[nodiscard]] DeviceVersion device_version() const;
    [[nodiscard]] Stats stats() const;
    [[nodiscard]] std::string transport_description() const { return transport_->description(); }

    [[nodiscard]] std::uint64_t next_request_id() noexcept { return ++request_ids_; }

    // ── outbound ────────────────────────────────────────────────────────
    /// Outbound messages are always sent unencrypted; encrypted messages are
    /// not supported in this build.
    [[nodiscard]] std::expected<void, std::string> send(MessageType type, const google::protobuf::MessageLite& msg);
    [[nodiscard]] std::expected<void, std::string> send_raw(MessageType type, std::span<const std::uint8_t> payload);
    /// `{action: READ}` for any message type whose field 1 is the action.
    [[nodiscard]] std::expected<void, std::string> send_read(MessageType type);

    // ── correlation ─────────────────────────────────────────────────────
    /// Register a waiter for `type`, run `trigger`, block for the first match.
    ///
    /// `quiet` is a budget of SILENCE, not of total time: while the unit keeps
    /// sending, it is busy rather than gone, and the budget starts over. It
    /// answers a request only after working through its seed push, which takes
    /// 12 to 16 s, while the gap to its own last message is a steady 60 ms
    /// (measured over ten runs, docs/evidence/78-antwortzeiten). Options::
    /// await_limit caps the total wait.
    [[nodiscard]] std::expected<Inbound, std::string> await(MessageType type, const std::function<void()>& trigger,
                                                           std::chrono::milliseconds quiet, Predicate match = {});

    /// Run `trigger` and gather every matching message for `duration`.
    [[nodiscard]] std::vector<Inbound> collect(MessageType type, const std::function<void()>& trigger,
                                               std::chrono::milliseconds duration, Predicate match = {});

    /// Send `msg` (tagged with a fresh request_id) and wait for a same-type
    /// reply whose request_id, if present, echoes it.
    template <class M>
    [[nodiscard]] std::expected<M, std::string> request(MessageType type, M msg, std::chrono::milliseconds timeout) {
        const auto rid = next_request_id();
        msg.set_request_id(rid);
        auto in = await(
            type, [&] { (void)send(type, msg); }, timeout,
            [rid](const Inbound& i) {
                auto parsed = i.template parse<M>();
                return parsed && (!parsed->has_request_id() || parsed->request_id() == rid);
            });
        if (!in) return std::unexpected(in.error());
        auto parsed = in->template parse<M>();
        if (!parsed) return std::unexpected("reply did not parse");
        return *parsed;
    }

    // ── observation ─────────────────────────────────────────────────────
    /// Called on the RX thread for EVERY decoded inbound message.
    ///
    /// When set_listener()/set_lost_handler() return, the PREVIOUS callback is
    /// no longer running and will not be called again. A callback may therefore
    /// capture locals by reference, provided the caller clears it before those
    /// locals die. Both may be called from inside a callback (the lock is
    /// recursive). A callback must not block on a thread that is waiting in
    /// one of these setters (docs/code-befunde/2026-10-02-listener-lebensdauer.json).
    void set_listener(Listener listener);
    void set_lost_handler(LostHandler handler);

    /// The ModelRepo payload (gzip tar) captured during the handshake, if any.
    [[nodiscard]] std::optional<std::vector<std::uint8_t>> model_repo_payload() const;

    /// Der MIDI-Kanal des Geraets, aufgeschnappt aus den GeneralSettings, die
    /// ohnehin im Handshake kommen. 0-basiert, wie MIDI ihn auf der Leitung
    /// fuehrt (das Geraet meldet ihn 1-basiert). Fehlt die Angabe, ist nichts
    /// gesetzt — dann bleibt dem Aufrufer nur die Vorgabe.
    [[nodiscard]] std::optional<int> midi_channel() const;

private:
    struct Waiter {
        MessageType type;
        Predicate match;
        std::optional<Inbound> result;
    };
    struct Collector {
        MessageType type;
        Predicate match;
        std::vector<Inbound>* bucket;
    };

    void rx_loop(std::stop_token st);
    void keepalive_loop(std::stop_token st);
    void handle_body(std::vector<std::uint8_t> body);
    void dispatch(Inbound in);
    void auto_reply_version(const Inbound& in);
    void mark_lost(std::string reason);
    [[nodiscard]] std::expected<void, std::string> handshake_once();
    [[nodiscard]] std::string announced_version() const;
    [[nodiscard]] std::expected<void, std::string> write_reports(const std::vector<framing::Report>& reports);

    std::unique_ptr<Transport> transport_;
    Options options_;
    std::chrono::steady_clock::time_point last_rx_{};  ///< when the device last sent anything

    std::jthread rx_thread_;
    std::jthread keepalive_thread_;
    std::atomic<bool> started_{false};
    std::atomic<bool> connected_{false};
    std::atomic<bool> lost_{false};
    std::atomic<std::uint64_t> request_ids_{0};

    mutable std::mutex mutex_;            ///< waiters_, collectors_, lost_reason_, version_, stats_, model_repo_
    std::condition_variable cv_;
    std::vector<std::shared_ptr<Waiter>> waiters_;
    std::vector<Collector*> collectors_;
    std::string lost_reason_;
    DeviceVersion version_;
    Stats stats_;
    std::optional<std::vector<std::uint8_t>> model_repo_;
    std::optional<int> midi_channel_;

    std::mutex write_mutex_;              ///< one logical message's reports are written atomically
    std::mutex callback_mutex_;           ///< listener_/lost_handler_
    std::recursive_mutex dispatch_mutex_; ///< held while a callback runs; the setters take it too
    Listener listener_;
    LostHandler lost_handler_;
    framing::Reassembler reassembler_;    ///< RX thread only
};

}  // namespace qc
