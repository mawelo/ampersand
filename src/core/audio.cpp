// audio.cpp — ALSA-Aufnahme vom Geraet, mit Stille-Ausgeber als Taktgeber
//
// Wer:      Martin.Lonkwitz
// Was:      Umsetzung von qc::audio::Capture: Karte suchen, Aufnahme- und
//           Wiedergabestrom oeffnen, Rahmen lesen, Kanal herausloesen.
// Wann:     2026-09-11
// Warum:    siehe Kopf von include/qc/audio.hpp - insbesondere der Grund fuer
//           den Wiedergabestrom (Implicit Feedback).
// Projekt:  Ampersand
// Abhaengig: libasound
// Nutzung:  ueber qc::audio::Capture
// Historie: git log
#include "qc/audio.hpp"

#include "qc/log.hpp"

#include <alsa/asoundlib.h>

#include <array>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <cmath>
#include <fstream>
#include <format>
#include <stop_token>
#include <thread>
#include <vector>

namespace qc::audio {

namespace {

constexpr snd_pcm_format_t kFormat = SND_PCM_FORMAT_S32_LE;
constexpr unsigned kLatencyUs = 200000;   ///< 200 ms; groesser nimmt das Geraet nicht an
constexpr std::size_t kRingSeconds = 2;   ///< so weit zurueck laesst sich holen

/// Warum ueberhaupt ein eigener Lesefaden: Wer im Aufrufer einen halbsekuendigen
/// Block liest und darauf YIN rechnet, laesst den ALSA-Puffer waehrend der
/// Rechnung ueberlaufen. Der Overrun wird zwar behoben, aber das Signal hat dann
/// Luecken - live war die Tonhoehe damit unbrauchbar (Guete 0,2 bis 0,5),
/// waehrend dieselbe Rechnung auf einer Aufnahme 0,001 erreichte. Einen
/// groesseren ALSA-Puffer nimmt das Geraet nicht an (Input/output error), also
/// laeuft das Abholen nebenher und der Aufrufer bedient sich aus dem Ring.

/// Kartennummer aus /proc/asound/cards, wie in qc::midi — dieselbe Quelle,
/// damit MIDI- und Audiozugriff nicht auseinanderlaufen koennen.
std::expected<int, std::string> card_index(std::string_view name) {
    std::ifstream cards("/proc/asound/cards");
    if (!cards) return std::unexpected("cannot read /proc/asound/cards — is ALSA present?");
    std::string line;
    while (std::getline(cards, line)) {
        if (line.find(name) == std::string::npos) continue;
        const auto first = line.find_first_not_of(' ');
        if (first == std::string::npos) continue;
        const auto end = line.find(' ', first);
        try {
            return std::stoi(line.substr(first, end - first));
        } catch (const std::exception&) {
            continue;
        }
    }
    return std::unexpected(std::format("no sound card whose name contains \"{}\" — is the unit connected?", name));
}

std::expected<snd_pcm_t*, std::string> open_stream(const std::string& device, snd_pcm_stream_t direction) {
    snd_pcm_t* pcm = nullptr;
    if (const int err = snd_pcm_open(&pcm, device.c_str(), direction, 0); err < 0) {
        return std::unexpected(std::format("cannot open {} for {}: {}", device,
                                           direction == SND_PCM_STREAM_CAPTURE ? "capture" : "playback",
                                           snd_strerror(err)));
    }
    if (const int err = snd_pcm_set_params(pcm, kFormat, SND_PCM_ACCESS_RW_INTERLEAVED, kChannels, kSampleRate,
                                           1 /* resample allowed */, kLatencyUs);
        err < 0) {
        snd_pcm_close(pcm);
        return std::unexpected(std::format("cannot configure {}: {}", device, snd_strerror(err)));
    }
    return pcm;
}

}  // namespace

struct Capture::Impl {
    snd_pcm_t* capture{nullptr};
    snd_pcm_t* playback{nullptr};
    std::jthread silence;
    std::jthread reader;

    std::mutex mutex;
    std::condition_variable arrived;
    std::vector<std::int32_t> ring;   ///< verschraenkt, kRingSeconds lang
    std::size_t write_at{0};          ///< naechste Schreibstelle, in Rahmen
    std::uint64_t total_frames{0};    ///< seit dem Oeffnen eingetroffen
    std::uint64_t handed_out{0};      ///< Stand beim letzten Abholen
    std::uint64_t dropouts{0};

    std::array<double, kChannels> rms{};

    ~Impl() {
        reader.request_stop();
        if (reader.joinable()) reader.join();
        silence.request_stop();
        if (silence.joinable()) silence.join();
        if (capture) snd_pcm_close(capture);
        if (playback) snd_pcm_close(playback);
    }
};

Capture::Capture(std::unique_ptr<Impl> impl) noexcept : impl_(std::move(impl)) {}
Capture::Capture(Capture&&) noexcept = default;
Capture& Capture::operator=(Capture&&) noexcept = default;
Capture::~Capture() = default;

std::expected<Capture, std::string> Capture::open_quad_cortex(std::string_view card_name) {
    auto index = card_index(card_name);
    if (!index) return std::unexpected(index.error());
    const std::string device = std::format("hw:{},0", *index);

    auto impl = std::make_unique<Impl>();

    auto playback = open_stream(device, SND_PCM_STREAM_PLAYBACK);
    if (!playback) return std::unexpected(playback.error());
    impl->playback = *playback;

    auto capture = open_stream(device, SND_PCM_STREAM_CAPTURE);
    if (!capture) return std::unexpected(capture.error());
    impl->capture = *capture;

    // Beide Stroeme gemeinsam anlaufen lassen. Der Aufnahme-Endpunkt taktet die
    // Wiedergabe (Implicit Feedback), deshalb scheitert die Aufnahme mit EIO,
    // wenn sie zuerst startet. snd_pcm_link koppelt Start und Stopp; davor wird
    // der Wiedergabepuffer mit Stille vorgefuellt, damit beim Start etwas
    // auszugeben ist.
    {
        constexpr snd_pcm_uframes_t kPrefill = 1024;
        const std::vector<std::int32_t> quiet(kPrefill * kChannels, 0);
        if (const int err = snd_pcm_prepare(impl->playback); err < 0) {
            return std::unexpected(std::format("cannot prepare playback: {}", snd_strerror(err)));
        }
        if (const int err = snd_pcm_prepare(impl->capture); err < 0) {
            return std::unexpected(std::format("cannot prepare capture: {}", snd_strerror(err)));
        }
        if (const int err = snd_pcm_link(impl->playback, impl->capture); err < 0) {
            log::warn("audio: cannot link playback and capture ({}) — trying without", snd_strerror(err));
        }
        for (int i = 0; i < 2; ++i) {
            if (snd_pcm_writei(impl->playback, quiet.data(), kPrefill) < 0) break;
        }
        if (const int err = snd_pcm_start(impl->playback); err < 0 && err != -EBADFD) {
            log::warn("audio: cannot start playback ({})", snd_strerror(err));
        }
    }

    // Der Stille-Ausgeber ist kein Beiwerk: Ohne laufende Wiedergabe schickt das
    // Geraet auf allen Kanaelen exakt null (docs/evidence/85).
    impl->silence = std::jthread([pcm = impl->playback](const std::stop_token& stop) {
        constexpr snd_pcm_uframes_t kBlock = 1024;
        const std::vector<std::int32_t> quiet(kBlock * kChannels, 0);
        while (!stop.stop_requested()) {
            const auto written = snd_pcm_writei(pcm, quiet.data(), kBlock);
            if (written < 0) {
                const int err = snd_pcm_recover(pcm, static_cast<int>(written), 1 /* silent */);
                if (err < 0) {
                    // Nicht stumm aussteigen: Ohne diesen Strom kommt nichts an,
                    // und die Anzeige zeigte dann -120 dBFS ohne Begruendung.
                    log::warn("tuner: playback stream stopped ({}) — without it the unit sends digital silence",
                              snd_strerror(err));
                    return;
                }
            }
        }
    });

    // Der Lesefaden holt fortlaufend ab, egal wie lange der Aufrufer rechnet.
    constexpr snd_pcm_uframes_t kChunk = 2048;
    impl->ring.assign(static_cast<std::size_t>(kSampleRate) * kRingSeconds * kChannels, 0);
    Impl* raw = impl.get();
    impl->reader = std::jthread([raw](const std::stop_token& stop) {
        std::vector<std::int32_t> chunk(kChunk * kChannels);
        const std::size_t ring_frames = raw->ring.size() / kChannels;
        while (!stop.stop_requested()) {
            const auto got = snd_pcm_readi(raw->capture, chunk.data(), kChunk);
            if (got < 0) {
                {
                    std::lock_guard lk{raw->mutex};
                    ++raw->dropouts;
                }
                if (const int err = snd_pcm_recover(raw->capture, static_cast<int>(got), 1); err < 0) {
                    log::warn("tuner: capture stream stopped ({})", snd_strerror(err));
                    return;
                }
                continue;
            }
            const auto frames = static_cast<std::size_t>(got);
            std::lock_guard lk{raw->mutex};
            for (std::size_t f = 0; f < frames; ++f) {
                const std::size_t at = (raw->write_at + f) % ring_frames;
                for (int c = 0; c < kChannels; ++c) {
                    raw->ring[at * kChannels + static_cast<std::size_t>(c)] =
                        chunk[f * kChannels + static_cast<std::size_t>(c)];
                }
            }
            raw->write_at = (raw->write_at + frames) % ring_frames;
            raw->total_frames += frames;
            raw->arrived.notify_all();
        }
    });

    return Capture(std::move(impl));
}

std::expected<std::size_t, std::string> Capture::read_channel(int channel, std::span<float> out) {
    if (!impl_ || !impl_->capture) return std::unexpected("capture stream is not open");
    if (channel < 0 || channel >= kChannels) {
        return std::unexpected(std::format("channel {} out of range 0..{}", channel, kChannels - 1));
    }
    if (out.empty()) return std::size_t{0};

    const std::size_t ring_frames = impl_->ring.size() / kChannels;
    if (out.size() > ring_frames) {
        return std::unexpected(std::format("asked for {} frames, the ring holds {}", out.size(), ring_frames));
    }

    std::unique_lock lk{impl_->mutex};
    // Auf genug Rahmen warten - aber mit Frist. Ohne die haengt der Aufrufer
    // endlos, wenn nichts eintrifft, und genau das ist passiert: Steht der
    // Wiedergabestrom nicht, liefert das Geraet nichts, und der Lesefaden
    // fuellt den Ring nie.
    // Auf NEUE Rahmen warten, nicht auf irgendwelche: Wer nur prueft, ob seit dem
    // Oeffnen genug eingetroffen ist, bekommt nach dem ersten Mal immer dasselbe
    // Fenster zurueck - die Anzeige stand dann auf einem Wert fest.
    const std::uint64_t ziel = std::max(impl_->handed_out + out.size(), static_cast<std::uint64_t>(out.size()));
    const bool genug = impl_->arrived.wait_for(lk, std::chrono::seconds(5),
                                               [&] { return impl_->total_frames >= ziel; });
    if (!genug) {
        return std::unexpected("no audio arrived within 5 s — the unit only sends samples while a playback "
                               "stream runs; is another application holding the sound card?");
    }
    impl_->handed_out = impl_->total_frames;

    // Die jeweils letzten out.size() Rahmen aus dem Ring holen.
    const std::size_t start = (impl_->write_at + ring_frames - out.size()) % ring_frames;
    std::array<double, kChannels> energy{};
    constexpr double kScale = 1.0 / 2147483648.0;
    for (std::size_t f = 0; f < out.size(); ++f) {
        const std::size_t at = (start + f) % ring_frames;
        for (int c = 0; c < kChannels; ++c) {
            const double v = static_cast<double>(impl_->ring[at * kChannels + static_cast<std::size_t>(c)]) * kScale;
            energy[static_cast<std::size_t>(c)] += v * v;
            if (c == channel) out[f] = static_cast<float>(v);
        }
    }
    for (int c = 0; c < kChannels; ++c) {
        impl_->rms[static_cast<std::size_t>(c)] =
            std::sqrt(energy[static_cast<std::size_t>(c)] / static_cast<double>(out.size()));
    }
    return out.size();
}

std::uint64_t Capture::dropouts() const noexcept {
    if (!impl_) return 0;
    std::lock_guard lk{impl_->mutex};
    return impl_->dropouts;
}

double Capture::last_rms(int channel) const noexcept {
    if (!impl_ || channel < 0 || channel >= kChannels) return 0.0;
    std::lock_guard lk{impl_->mutex};
    return impl_->rms[static_cast<std::size_t>(channel)];
}

}  // namespace qc::audio
