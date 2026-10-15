// transport.hpp — abstrakte Report-Schnittstelle (HID oder Test-Double)
//
// Wer:      Martin.Lonkwitz
// Was:      Minimale Schnittstelle, ueber die Session 129-Byte-Reports liest
//           und schreibt. Implementierungen: HidrawTransport (echtes Geraet)
//           und FakeTransport (Tests).
// Wann:     2026-09-04
// Warum:    Trennt Protokolllogik von Linux-hidraw; macht die Session ohne
//           Hardware testbar.
// Projekt:  Ampersand
// Abhaengig: keine
// Historie: git log
#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>

namespace qc {

class Transport {
public:
    enum class ReadStatus : std::uint8_t { Data, Timeout, Lost };
    struct ReadResult {
        ReadStatus status{ReadStatus::Timeout};
        std::size_t size{0};
        std::string error;
    };

    Transport() = default;
    Transport(const Transport&) = delete;
    Transport& operator=(const Transport&) = delete;
    Transport(Transport&&) = delete;
    Transport& operator=(Transport&&) = delete;
    virtual ~Transport() = default;

    /// Read one INPUT report into `buf` (>= 129 bytes). Returns Timeout when
    /// nothing arrived within `timeout`, Lost when the device is gone.
    virtual ReadResult read(std::span<std::uint8_t> buf, std::chrono::milliseconds timeout) = 0;

    /// Write one 129-byte OUTPUT report. Returns true when the report was
    /// delivered. NOTE: the Quad Cortex STALLs the status stage of every
    /// SET_REPORT after consuming the data; implementations MUST report that
    /// as delivered (see docs/protocol.md "benign write STALL").
    virtual bool write(std::span<const std::uint8_t> report) = 0;

    [[nodiscard]] virtual std::string description() const = 0;
};

}  // namespace qc
