// test_file_write.cpp — qc::io::write_private_atomic: Rundlauf, Modus, Atomaritaet
//
// Offline, nur mit Temp-Dateien. Prueft, dass das Schreiben privat (0600) ist,
// eine vorhandene Datei atomar ersetzt und bei einem Fehler das Ziel
// unberuehrt laesst (Re-Review 2026-10-15: der O_TRUNC-Vorlaeufer tat das nicht).
#include "qc/file_write.hpp"

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <sys/stat.h>
#include <unistd.h>

namespace {

namespace fs = std::filesystem;

std::string read_all(const fs::path& p) {
    std::ifstream f(p, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
}

mode_t mode_of(const fs::path& p) {
    struct stat st{};
    REQUIRE(::stat(p.c_str(), &st) == 0);
    return st.st_mode & 07777;
}

/// Eigenes, nach Gebrauch wieder entferntes Temp-Verzeichnis.
struct TmpDir {
    fs::path path;
    TmpDir() {
        path = fs::temp_directory_path() / ("ampersand-fw-" + std::to_string(::getpid()) + "-" +
                                            std::to_string(reinterpret_cast<std::uintptr_t>(this)));
        fs::create_directories(path);
    }
    ~TmpDir() {
        std::error_code ec;
        fs::permissions(path, fs::perms::owner_all, fs::perm_options::add, ec);
        fs::remove_all(path, ec);
    }
};

std::span<const std::uint8_t> bytes_of(std::string_view s) {
    return {reinterpret_cast<const std::uint8_t*>(s.data()), s.size()};
}

}  // namespace

TEST_CASE("write_private_atomic creates a 0600 file with the exact content") {
    TmpDir d;
    const fs::path target = d.path / "backup.json";
    const std::string content = R"({"hello":"world"})";

    auto r = qc::io::write_private_atomic(target.string(), bytes_of(content));
    REQUIRE(r.has_value());
    REQUIRE(fs::exists(target));
    CHECK(read_all(target) == content);
    CHECK(mode_of(target) == 0600);
}

TEST_CASE("write_private_atomic replaces an existing file and leaves no temp behind") {
    TmpDir d;
    const fs::path target = d.path / "backup.json";
    {
        std::ofstream(target) << "OLD";
    }
    ::chmod(target.c_str(), 0644);

    const std::string content = "NEW-and-longer-content";
    auto r = qc::io::write_private_atomic(target.string(), bytes_of(content));
    REQUIRE(r.has_value());
    CHECK(read_all(target) == content);
    CHECK(mode_of(target) == 0600);

    // Keine .tmp.*-Reste im Verzeichnis.
    int leftovers = 0;
    for (const auto& e : fs::directory_iterator(d.path))
        if (e.path().filename().string().find(".tmp.") != std::string::npos) ++leftovers;
    CHECK(leftovers == 0);
}

TEST_CASE("write_private_atomic leaves the original intact when the write cannot be placed") {
    if (::geteuid() == 0) {
        SUCCEED("skipped as root: permission bits do not block writes");
        return;
    }
    TmpDir d;
    const fs::path target = d.path / "backup.json";
    {
        std::ofstream(target) << "ORIGINAL";
    }
    // Verzeichnis nur lesbar/ausfuehrbar: die Temp-Datei laesst sich nicht anlegen.
    ::chmod(d.path.c_str(), 0500);

    auto r = qc::io::write_private_atomic(target.string(), bytes_of("REPLACEMENT"));
    ::chmod(d.path.c_str(), 0700);  // fuer das Aufraeumen wieder schreibbar

    CHECK_FALSE(r.has_value());
    CHECK(read_all(target) == "ORIGINAL");
}
