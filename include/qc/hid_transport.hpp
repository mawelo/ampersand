// hid_transport.hpp — Linux-hidraw-Transport (ohne hidapi)
//
// Wer:      Martin.Lonkwitz
// Was:      Geraetesuche ueber /sys/class/hidraw und Report-I/O ueber
//           /dev/hidrawN mit poll(2)-Timeouts. RAII-Dateideskriptor.
// Wann:     2026-09-04
// Warum:    Keine Abhaengigkeit von libhidapi-dev; der Kernel-hidraw-Treiber
//           liefert Input-Reports mit Report-ID und sendet Output-Reports als
//           SET_REPORT-Control-Transfer (das Geraet hat keinen OUT-Endpoint).
// Projekt:  Ampersand
// Abhaengig: Linux (<linux/hidraw.h>, poll, open/read/write)
// Nutzung:  auto devs = qc::hid::enumerate(); auto t = qc::hid::HidrawTransport::open(devs[0].path);
// Sicherheit: /dev/hidrawN ist standardmaessig root-only; udev/99-neural-quad-cortex.rules
//           gibt der Gruppe plugdev Zugriff.
// Historie: git log
#pragma once

#include "qc/transport.hpp"

#include <cstdint>
#include <expected>
#include <memory>
#include <string>
#include <vector>

namespace qc::hid {

inline constexpr std::uint16_t kVendorId = 0x152A;   ///< Neural DSP
inline constexpr std::uint16_t kProductId = 0x880A;  ///< Quad Cortex

struct DeviceInfo {
    std::string path;      ///< /dev/hidrawN
    std::string name;      ///< HID_NAME from sysfs ("Neural DSP Quad Cortex")
    std::string phys;      ///< HID_PHYS (usb topology)
    std::uint16_t vendor{0};
    std::uint16_t product{0};
    bool accessible{false};  ///< the current user may open it read/write
};

/// All hidraw nodes matching vendor/product (default: the Quad Cortex).
[[nodiscard]] std::vector<DeviceInfo> enumerate(std::uint16_t vendor = kVendorId,
                                                std::uint16_t product = kProductId);

class HidrawTransport final : public Transport {
public:
    [[nodiscard]] static std::expected<std::unique_ptr<HidrawTransport>, std::string>
    open(const std::string& path);

    /// Open the first accessible Quad Cortex.
    [[nodiscard]] static std::expected<std::unique_ptr<HidrawTransport>, std::string> open_first();

    ~HidrawTransport() override;

    ReadResult read(std::span<std::uint8_t> buf, std::chrono::milliseconds timeout) override;
    bool write(std::span<const std::uint8_t> report) override;
    [[nodiscard]] std::string description() const override { return path_; }

    struct Stats {
        std::uint64_t writes{0};
        std::uint64_t write_stalls{0};   ///< EPIPE: the benign status-stage STALL
        std::uint64_t write_errors{0};
        std::uint64_t reads{0};
    };
    [[nodiscard]] const Stats& stats() const noexcept { return stats_; }

private:
    HidrawTransport(int fd, std::string path) : fd_(fd), path_(std::move(path)) {}
    int fd_{-1};
    std::string path_;
    Stats stats_;
};

}  // namespace qc::hid
