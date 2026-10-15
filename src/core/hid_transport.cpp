// hid_transport.cpp — Linux-hidraw-Transport (siehe hid_transport.hpp)
//
// Wer:      Martin.Lonkwitz
// Wann:     2026-09-04
// Projekt:  Ampersand
// Historie: git log
//
// UNSAFE ZONE (Rule 9.2): diese Datei ist die einzige FFI-Grenze zu den
// POSIX-/Linux-Systemaufrufen open/read/write/poll/close.
#include "qc/hid_transport.hpp"

#include <fcntl.h>
#include <poll.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>
#include <filesystem>
#include <format>
#include <fstream>
#include <string_view>

namespace qc::hid {

namespace {

std::string errno_text(int err) {
    // unsafe{ strerror_r GNU variant returns a pointer into buf or a static string.
    char buf[256]{};
    const char* msg = ::strerror_r(err, buf, sizeof buf);
    return std::string(msg ? msg : "?");
    // }unsafe
}

std::optional<std::uint16_t> parse_hex16(std::string_view s) {
    if (s.empty() || s.size() > 8) return std::nullopt;
    std::uint32_t v = 0;
    for (char c : s) {
        v <<= 4;
        if (c >= '0' && c <= '9') v |= static_cast<std::uint32_t>(c - '0');
        else if (c >= 'a' && c <= 'f') v |= static_cast<std::uint32_t>(c - 'a' + 10);
        else if (c >= 'A' && c <= 'F') v |= static_cast<std::uint32_t>(c - 'A' + 10);
        else return std::nullopt;
    }
    return static_cast<std::uint16_t>(v & 0xFFFF);
}

}  // namespace

std::vector<DeviceInfo> enumerate(std::uint16_t vendor, std::uint16_t product) {
    std::vector<DeviceInfo> out;
    std::error_code ec;
    const std::filesystem::path root{"/sys/class/hidraw"};
    if (!std::filesystem::exists(root, ec)) return out;
    for (const auto& entry : std::filesystem::directory_iterator(root, ec)) {
        const auto uevent = entry.path() / "device" / "uevent";
        std::ifstream in(uevent);
        if (!in) continue;
        DeviceInfo info;
        info.path = "/dev/" + entry.path().filename().string();
        std::string line;
        while (std::getline(in, line)) {
            const auto eq = line.find('=');
            if (eq == std::string::npos) continue;
            const std::string_view key(line.data(), eq);
            const std::string_view val(line.data() + eq + 1, line.size() - eq - 1);
            if (key == "HID_ID") {
                // Format: 0003:0000152A:0000880A (bus:vendor:product)
                const auto c1 = val.find(':');
                const auto c2 = (c1 == std::string_view::npos) ? c1 : val.find(':', c1 + 1);
                if (c1 == std::string_view::npos || c2 == std::string_view::npos) continue;
                info.vendor = parse_hex16(val.substr(c1 + 1, c2 - c1 - 1)).value_or(0);
                info.product = parse_hex16(val.substr(c2 + 1)).value_or(0);
            } else if (key == "HID_NAME") {
                info.name = std::string(val);
            } else if (key == "HID_PHYS") {
                info.phys = std::string(val);
            }
        }
        if (info.vendor != vendor || info.product != product) continue;
        // unsafe{ access(2) on the device node.
        info.accessible = ::access(info.path.c_str(), R_OK | W_OK) == 0;
        // }unsafe
        out.push_back(std::move(info));
    }
    std::sort(out.begin(), out.end(), [](const DeviceInfo& a, const DeviceInfo& b) { return a.path < b.path; });
    return out;
}

std::expected<std::unique_ptr<HidrawTransport>, std::string> HidrawTransport::open(const std::string& path) {
    // unsafe{ open(2); the fd is owned by the returned object and closed in ~HidrawTransport.
    const int fd = ::open(path.c_str(), O_RDWR | O_CLOEXEC | O_NONBLOCK);
    // }unsafe
    if (fd < 0) {
        const int err = errno;
        std::string hint;
        if (err == EACCES) {
            hint = " (install udev/99-neural-quad-cortex.rules and re-plug the device, or add yourself to group plugdev)";
        }
        return std::unexpected(std::format("open {}: {}{}", path, errno_text(err), hint));
    }
    return std::unique_ptr<HidrawTransport>(new HidrawTransport(fd, path));
}

std::expected<std::unique_ptr<HidrawTransport>, std::string> HidrawTransport::open_first() {
    const auto devices = enumerate();
    if (devices.empty()) {
        return std::unexpected("no Quad Cortex found on USB (vendor 152a, product 880a). Is it plugged in and booted?");
    }
    std::string last_error;
    for (const auto& d : devices) {
        auto t = open(d.path);
        if (t) return t;
        last_error = t.error();
    }
    return std::unexpected(last_error);
}

HidrawTransport::~HidrawTransport() {
    // unsafe{
    if (fd_ >= 0) ::close(fd_);
    // }unsafe
}

Transport::ReadResult HidrawTransport::read(std::span<std::uint8_t> buf, std::chrono::milliseconds timeout) {
    ReadResult r;
    // unsafe{ poll(2) + read(2) on the hidraw fd.
    pollfd pfd{};
    pfd.fd = fd_;
    pfd.events = POLLIN;
    const int prc = ::poll(&pfd, 1, static_cast<int>(timeout.count()));
    if (prc == 0) return r;  // Timeout
    if (prc < 0) {
        if (errno == EINTR) return r;
        r.status = ReadStatus::Lost;
        r.error = std::format("poll: {}", errno_text(errno));
        return r;
    }
    if (pfd.revents & (POLLERR | POLLHUP | POLLNVAL)) {
        r.status = ReadStatus::Lost;
        r.error = "device node reported error/hangup (unplugged?)";
        return r;
    }
    const ssize_t n = ::read(fd_, buf.data(), buf.size());
    // }unsafe
    if (n < 0) {
        const int err = errno;
        if (err == EAGAIN || err == EINTR) return r;  // spurious wakeup
        r.status = ReadStatus::Lost;
        r.error = std::format("read: {}", errno_text(err));
        return r;
    }
    ++stats_.reads;
    r.status = ReadStatus::Data;
    r.size = static_cast<std::size_t>(n);
    return r;
}

bool HidrawTransport::write(std::span<const std::uint8_t> report) {
    ++stats_.writes;
    // unsafe{ write(2). hidraw sends an OUTPUT report via SET_REPORT on the
    // control pipe when the interface has no interrupt OUT endpoint. The Quad
    // Cortex consumes the data stage and then STALLs the status stage, which
    // the kernel surfaces as EPIPE. That is the documented benign behaviour:
    // the report WAS delivered.
    const ssize_t n = ::write(fd_, report.data(), report.size());
    const int err = errno;
    // }unsafe
    if (n == static_cast<ssize_t>(report.size())) return true;
    if (n < 0 && (err == EPIPE || err == EPROTO)) {
        ++stats_.write_stalls;
        return true;
    }
    ++stats_.write_errors;
    return false;
}

}  // namespace qc::hid
