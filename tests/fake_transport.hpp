// fake_transport.hpp — Test-Double fuer qc::Transport (simuliert das Geraet)
//
// Wer:      Martin.Lonkwitz
// Was:      Nimmt Host-Reports entgegen, reassembliert sie zu Nachrichten und
//           erlaubt dem Test, Antworten des "Geraets" einzuspeisen — auch
//           gzip-komprimiert oder mit gesetztem Verschluesselungs-Flag.
// Wann:     2026-09-04
// Projekt:  Ampersand (tests)
#pragma once

#include "qc/framing.hpp"
#include "qc/messages.hpp"
#include "qc/transport.hpp"

#include <google/protobuf/message_lite.h>

#include <condition_variable>
#include <deque>
#include <functional>
#include <mutex>
#include <optional>
#include <vector>

namespace qc::test {

class FakeTransport final : public Transport {
public:
    struct HostMessage {
        MessageType type;
        std::vector<std::uint8_t> payload;
        template <class M>
        std::optional<M> parse() const {
            M m;
            if (!m.ParseFromArray(payload.data(), static_cast<int>(payload.size()))) return std::nullopt;
            return m;
        }
    };
    /// Called (on the caller's thread of write()) for every complete host message.
    using Responder = std::function<void(const HostMessage&)>;

    ReadResult read(std::span<std::uint8_t> buf, std::chrono::milliseconds timeout) override {
        std::unique_lock lk{mutex_};
        cv_.wait_for(lk, timeout, [&] { return !to_host_.empty() || lost_; });
        if (lost_) return {ReadStatus::Lost, 0, "fake: device unplugged"};
        if (to_host_.empty()) return {};
        auto r = std::move(to_host_.front());
        to_host_.pop_front();
        std::copy(r.begin(), r.end(), buf.begin());
        return {ReadStatus::Data, r.size(), {}};
    }

    bool write(std::span<const std::uint8_t> report) override {
        {
            std::lock_guard lk{mutex_};
            ++writes_;
            auto m = reassembler_.push(report);
            if (!m || !*m) return true;
            auto env = framing::decode_body(**m);
            if (!env) return true;
            host_messages_.push_back({message_type_from_raw(env->type), env->payload});
        }
        Responder r;
        HostMessage last;
        {
            std::lock_guard lk{mutex_};
            r = responder_;
            last = host_messages_.back();
        }
        if (r) r(last);
        return true;
    }

    std::string description() const override { return "fake"; }

    // ── test controls ───────────────────────────────────────────────────
    void set_responder(Responder r) {
        std::lock_guard lk{mutex_};
        responder_ = std::move(r);
    }

    /// Queue a device -> host message (framed into INPUT reports).
    void push_raw(MessageType type, std::span<const std::uint8_t> payload, bool encrypted = false,
                  bool compressed = false) {
        auto reports = framing::encode(raw(type), payload, encrypted, compressed);
        std::lock_guard lk{mutex_};
        for (auto& r : reports) {
            r.at(0) = framing::kInReportId;
            to_host_.emplace_back(r.begin(), r.end());
        }
        cv_.notify_all();
    }

    void push(MessageType type, const google::protobuf::MessageLite& msg) {
        std::string bytes = msg.SerializeAsString();
        push_raw(type, std::span<const std::uint8_t>(reinterpret_cast<const std::uint8_t*>(bytes.data()), bytes.size()));
    }

    /// Push one raw INPUT report verbatim (for hand-built frames).
    void push_report(std::span<const std::uint8_t> report) {
        std::lock_guard lk{mutex_};
        to_host_.emplace_back(report.begin(), report.end());
        cv_.notify_all();
    }

    void unplug() {
        std::lock_guard lk{mutex_};
        lost_ = true;
        cv_.notify_all();
    }

    std::vector<HostMessage> host_messages() const {
        std::lock_guard lk{mutex_};
        return host_messages_;
    }

    std::optional<HostMessage> last_of(MessageType t) const {
        std::lock_guard lk{mutex_};
        for (auto it = host_messages_.rbegin(); it != host_messages_.rend(); ++it) {
            if (it->type == t) return *it;
        }
        return std::nullopt;
    }

    std::size_t count_of(MessageType t) const {
        std::lock_guard lk{mutex_};
        std::size_t n = 0;
        for (const auto& m : host_messages_) n += (m.type == t);
        return n;
    }

    std::uint64_t writes() const {
        std::lock_guard lk{mutex_};
        return writes_;
    }

private:
    mutable std::mutex mutex_;
    std::condition_variable cv_;
    std::deque<std::vector<std::uint8_t>> to_host_;
    std::vector<HostMessage> host_messages_;
    framing::Reassembler reassembler_;
    Responder responder_;
    bool lost_{false};
    std::uint64_t writes_{0};
};

}  // namespace qc::test
