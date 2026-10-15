// midi.cpp — rawmidi-Ausgang finden und bespielen (siehe include/qc/midi.hpp)
//
// Wer:      Martin.Lonkwitz
// Was:      Umsetzung von qc::midi: Karte in /proc/asound/cards suchen,
//           /dev/snd/midiC<N>D0 oeffnen, Control-Change-Nachrichten schreiben.
// Wann:     2026-09-11
// Warum:    siehe Kopf von include/qc/midi.hpp
// Projekt:  Ampersand
// Abhaengig: Linux, ALSA-rawmidi-Geraeteknoten
// Nutzung:  ueber qc::midi::Port
// Historie: git log
#include "qc/midi.hpp"

#include <fcntl.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>
#include <format>
#include <fstream>
#include <string>

namespace qc::midi {

namespace {

/// Eine Zeile aus /proc/asound/cards sieht so aus:
///   " 3 [Cortex         ]: USB-Audio - Quad Cortex"
/// Gesucht ist die Nummer der ersten Karte, deren Zeile `name` enthaelt.
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
            continue;  // Fortsetzungszeile ohne Nummer — die naechste pruefen
        }
    }
    return std::unexpected(std::format("no sound card whose name contains \"{}\" — is the unit connected?", name));
}

std::string errno_text(std::string_view what, std::string_view path) {
    return std::format("{} {}: {}", what, path, std::strerror(errno));
}

}  // namespace

Port::~Port() {
    if (fd_ >= 0) ::close(fd_);
}

Port& Port::operator=(Port&& other) noexcept {
    if (this != &other) {
        if (fd_ >= 0) ::close(fd_);
        fd_ = std::exchange(other.fd_, -1);
        path_ = std::move(other.path_);
    }
    return *this;
}

std::expected<std::string, std::string> find_device(std::string_view card_name) {
    auto index = card_index(card_name);
    if (!index) return std::unexpected(index.error());
    return std::format("/dev/snd/midiC{}D0", *index);
}

std::expected<Port, std::string> Port::open(std::string_view device_path) {
    const std::string path(device_path);
    const int fd = ::open(path.c_str(), O_WRONLY | O_CLOEXEC);
    if (fd < 0) return std::unexpected(errno_text("cannot open", path));
    return Port(fd, path);
}

std::expected<Port, std::string> Port::open_quad_cortex(std::string_view card_name) {
    auto path = find_device(card_name);
    if (!path) return std::unexpected(path.error());
    return open(*path);
}

std::expected<void, std::string> Port::control_change(int cc, int value, int channel) {
    if (fd_ < 0) return std::unexpected("MIDI port is not open");
    if (channel < 0 || channel > 15) return std::unexpected(std::format("MIDI channel {} out of range 0..15", channel));
    if (cc < 0 || cc > 127) return std::unexpected(std::format("control change number {} out of range 0..127", cc));
    if (value < 0 || value > 127) return std::unexpected(std::format("control change value {} out of range 0..127", value));

    // Status 0xB0 | Kanal, dann Nummer und Wert — drei Bytes, eine Nachricht.
    const unsigned char message[3]{static_cast<unsigned char>(0xB0 | channel), static_cast<unsigned char>(cc),
                                   static_cast<unsigned char>(value)};
    std::size_t written = 0;
    while (written < sizeof(message)) {
        const auto n = ::write(fd_, message + written, sizeof(message) - written);  // unsafe{} Zeigerarithmetik auf lokalem Array
        if (n < 0) {
            if (errno == EINTR) continue;
            return std::unexpected(errno_text("cannot write to", path_));
        }
        written += static_cast<std::size_t>(n);
    }
    return {};
}

}  // namespace qc::midi
