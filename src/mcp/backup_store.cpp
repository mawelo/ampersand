// backup_store.cpp — fester Ablageort fuer LocalBackups (siehe backup_store.hpp)
//
// Wer:      Martin.Lonkwitz
// Wann:     2026-10-15
// Projekt:  Ampersand
// Historie: git log
#include "backup_store.hpp"

#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <format>
#include <system_error>
#include <utility>

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

namespace qcmcp {

namespace {

/// Besitzt einen Dateideskriptor und schliesst ihn genau einmal.
class Fd {
public:
    explicit Fd(int fd) noexcept : fd_(fd) {}
    Fd(const Fd&) = delete;
    Fd& operator=(const Fd&) = delete;
    Fd(Fd&& o) noexcept : fd_(std::exchange(o.fd_, -1)) {}
    Fd& operator=(Fd&&) = delete;
    ~Fd() {
        if (fd_ >= 0) ::close(fd_);
    }
    [[nodiscard]] int get() const noexcept { return fd_; }
    [[nodiscard]] bool ok() const noexcept { return fd_ >= 0; }
    /// Schliesst explizit und meldet den Fehler von close() (wichtig nach write).
    [[nodiscard]] int close() noexcept { return ::close(std::exchange(fd_, -1)); }

private:
    int fd_;
};

std::string err(std::string_view what, const std::filesystem::path& p, int e) {
    return std::format("{} {}: {}", what, p.string(), std::strerror(e));
}

/// Oeffnet das Ablageverzeichnis ohne einem Symlink zu folgen und verlangt,
/// dass es dem aufrufenden Nutzer gehoert und fuer niemanden sonst
/// beschreibbar ist.
std::expected<Fd, std::string> open_dir(const std::filesystem::path& dir) {
    Fd d{::open(dir.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC)};
    if (!d.ok()) return std::unexpected(err("cannot open backup directory", dir, errno));
    struct stat st{};
    if (::fstat(d.get(), &st) != 0) return std::unexpected(err("cannot stat", dir, errno));
    if (st.st_uid != ::geteuid()) return std::unexpected(std::format("backup directory {} is not owned by the current user", dir.string()));
    if ((st.st_mode & (S_IWGRP | S_IWOTH)) != 0)
        return std::unexpected(std::format("backup directory {} is writable by others", dir.string()));
    return d;
}

}  // namespace

std::filesystem::path BackupStore::default_dir() {
    if (const char* x = std::getenv("XDG_DATA_HOME"); x != nullptr && x[0] == '/')
        return std::filesystem::path{x} / "ampersand/backups";
    const char* home = std::getenv("HOME");
    return std::filesystem::path{home != nullptr ? home : "."} / ".local/share/ampersand/backups";
}

std::expected<void, std::string> BackupStore::check_name(std::string_view name) {
    constexpr std::size_t kMaxName = 128;
    auto alnum = [](char c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9'); };
    if (name.empty() || name.size() > kMaxName)
        return std::unexpected(std::format("backup name must be 1..{} characters", kMaxName));
    if (!alnum(name.front()))
        return std::unexpected("backup name must start with a letter or digit (a plain file name, not a path)");
    for (const char c : name)
        if (!alnum(c) && c != '.' && c != '_' && c != '-')
            return std::unexpected("backup name may only contain A-Z a-z 0-9 . _ - (a plain file name, not a path)");
    return {};
}

std::expected<std::filesystem::path, std::string> BackupStore::save(std::string_view name, std::string_view data) const {
    if (auto c = check_name(name); !c) return std::unexpected(c.error());
    std::error_code ec;
    std::filesystem::create_directories(dir_.parent_path(), ec);
    if (ec) return std::unexpected(std::format("cannot create {}: {}", dir_.parent_path().string(), ec.message()));
    if (::mkdir(dir_.c_str(), 0700) != 0 && errno != EEXIST) return std::unexpected(err("cannot create", dir_, errno));
    auto d = open_dir(dir_);
    if (!d) return std::unexpected(d.error());

    const std::string fname{name};
    const auto path = dir_ / fname;
    Fd f{::openat(d->get(), fname.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600)};
    if (!f.ok()) {
        const int e = errno;
        if (e == EEXIST) return std::unexpected(std::format("{} already exists; choose another name", path.string()));
        return std::unexpected(err("cannot create", path, e));
    }
    auto fail = [&](int e) -> std::expected<std::filesystem::path, std::string> {
        ::unlinkat(d->get(), fname.c_str(), 0);
        return std::unexpected(err("cannot write", path, e));
    };
    std::size_t done = 0;
    while (done < data.size()) {
        const ssize_t n = ::write(f.get(), data.data() + done, data.size() - done);
        if (n < 0) {
            if (errno == EINTR) continue;
            return fail(errno);
        }
        done += static_cast<std::size_t>(n);
    }
    if (::fsync(f.get()) != 0) return fail(errno);
    if (f.close() != 0) return fail(errno);
    return path;
}

std::expected<std::string, std::string> BackupStore::load(std::string_view name) const {
    if (auto c = check_name(name); !c) return std::unexpected(c.error());
    auto d = open_dir(dir_);
    if (!d) return std::unexpected(d.error());

    const std::string fname{name};
    const auto path = dir_ / fname;
    // O_NONBLOCK: eine FIFO blockiert sonst schon beim Oeffnen; sie scheitert
    // danach an der S_ISREG-Pruefung.
    Fd f{::openat(d->get(), fname.c_str(), O_RDONLY | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC)};
    if (!f.ok()) return std::unexpected(err("cannot open", path, errno));
    struct stat st{};
    if (::fstat(f.get(), &st) != 0) return std::unexpected(err("cannot stat", path, errno));
    if (!S_ISREG(st.st_mode)) return std::unexpected(std::format("{} is not a regular file", path.string()));
    if (st.st_size < 0 || static_cast<std::size_t>(st.st_size) > max_bytes_)
        return std::unexpected(std::format("{} is larger than {} bytes", path.string(), max_bytes_));

    // Stueckweise lesen und die Grenze bei jedem Stueck pruefen: waechst die
    // Datei zwischen fstat und read, greift sie trotzdem.
    std::string out;
    out.reserve(static_cast<std::size_t>(st.st_size));
    char buf[65536];
    for (;;) {
        const ssize_t n = ::read(f.get(), buf, sizeof buf);
        if (n < 0) {
            if (errno == EINTR) continue;
            return std::unexpected(err("cannot read", path, errno));
        }
        if (n == 0) break;
        if (out.size() + static_cast<std::size_t>(n) > max_bytes_)
            return std::unexpected(std::format("{} is larger than {} bytes", path.string(), max_bytes_));
        out.append(buf, static_cast<std::size_t>(n));
    }
    return out;
}

}  // namespace qcmcp
