// test_backup_store.cpp — BackupStore: nur Dateinamen, nur ein Verzeichnis,
// kein Ueberschreiben, keine Symlinks, nur regulaere Dateien bis zur Grenze.
//
// Wer:      Martin.Lonkwitz
// Wann:     2026-10-15
// Warum:    Review 2026-10-15, H1/M1 (qc_backup_save/-restore)
// Projekt:  Ampersand
// Historie: git log
#include "../src/mcp/backup_store.hpp"

#include <catch2/catch_test_macros.hpp>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>

#include <sys/stat.h>
#include <unistd.h>

namespace fs = std::filesystem;
using qcmcp::BackupStore;

namespace {

/// Eigenes Temp-Verzeichnis je Test, wird am Ende entfernt.
struct TempDir {
    fs::path path;
    TempDir() {
        std::string tmpl = (fs::temp_directory_path() / "qc-backup-test-XXXXXX").string();
        REQUIRE(::mkdtemp(tmpl.data()) != nullptr);
        path = tmpl;
    }
    TempDir(const TempDir&) = delete;
    TempDir& operator=(const TempDir&) = delete;
    ~TempDir() {
        std::error_code ec;
        fs::remove_all(path, ec);
    }
};

std::string slurp(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

mode_t mode_of(const fs::path& p) {
    struct stat st{};
    REQUIRE(::lstat(p.c_str(), &st) == 0);
    return st.st_mode & 07777;
}

}  // namespace

TEST_CASE("check_name accepts plain file names only", "[backup_store]") {
    CHECK(BackupStore::check_name("qc-2026-10-15.json"));
    CHECK(BackupStore::check_name("a"));
    CHECK(BackupStore::check_name(std::string(128, 'x')));
    for (const char* bad : {"", ".", "..", ".hidden", "../x", "a/b", "/etc/passwd", "~/.bashrc", "a b", "-rf", "x\n"})
        CHECK_FALSE(BackupStore::check_name(bad));
    CHECK_FALSE(BackupStore::check_name(std::string(129, 'x')));
}

TEST_CASE("save creates a private directory and a 0600 file; load reads it back", "[backup_store]") {
    TempDir t;
    const BackupStore s{t.path / "nested/backups"};
    auto p = s.save("b.json", R"({"k":1})");
    REQUIRE(p);
    CHECK(*p == t.path / "nested/backups/b.json");
    CHECK(mode_of(t.path / "nested/backups") == 0700);
    CHECK(mode_of(*p) == 0600);
    auto d = s.load("b.json");
    REQUIRE(d);
    CHECK(*d == R"({"k":1})");
}

TEST_CASE("save never overwrites an existing file", "[backup_store]") {
    TempDir t;
    const BackupStore s{t.path / "b"};
    REQUIRE(s.save("x.json", "first"));
    auto again = s.save("x.json", "second");
    REQUIRE_FALSE(again);
    CHECK(again.error().find("already exists") != std::string::npos);
    CHECK(slurp(t.path / "b/x.json") == "first");
}

TEST_CASE("save and load do not follow a symlink at the file name", "[backup_store]") {
    TempDir t;
    const BackupStore s{t.path / "b"};
    REQUIRE(s.save("seed.json", "{}"));
    const fs::path victim = t.path / "victim";
    { std::ofstream(victim) << "keep"; }
    fs::create_symlink(victim, t.path / "b/link.json");
    CHECK_FALSE(s.save("link.json", "evil"));
    CHECK(slurp(victim) == "keep");
    CHECK_FALSE(s.load("link.json"));
}

TEST_CASE("the backup directory itself must not be a symlink or writable by others", "[backup_store]") {
    TempDir t;
    fs::create_directory(t.path / "real");
    fs::create_directory_symlink(t.path / "real", t.path / "alias");
    CHECK_FALSE(BackupStore{t.path / "alias"}.save("x.json", "{}"));
    CHECK_FALSE(fs::exists(t.path / "real/x.json"));

    fs::create_directory(t.path / "open");
    REQUIRE(::chmod((t.path / "open").c_str(), 0777) == 0);
    auto r = BackupStore{t.path / "open"}.save("x.json", "{}");
    REQUIRE_FALSE(r);
    CHECK(r.error().find("writable by others") != std::string::npos);
}

TEST_CASE("load rejects non-regular files and files over the size limit", "[backup_store]") {
    TempDir t;
    const BackupStore s{t.path / "b", 16};
    REQUIRE(s.save("ok.json", "0123456789abcdef"));  // genau 16 Bytes
    CHECK(s.load("ok.json"));
    REQUIRE(s.save("big.json", "0123456789abcdefX"));  // 17 Bytes
    auto big = s.load("big.json");
    REQUIRE_FALSE(big);
    CHECK(big.error().find("larger than") != std::string::npos);

    REQUIRE(::mkfifo((t.path / "b/fifo").c_str(), 0600) == 0);
    auto fifo = s.load("fifo");  // blockiert nicht (O_NONBLOCK), scheitert an S_ISREG
    REQUIRE_FALSE(fifo);
    CHECK(fifo.error().find("not a regular file") != std::string::npos);

    fs::create_directory(t.path / "b/sub");
    CHECK_FALSE(s.load("sub"));
    CHECK_FALSE(s.load("missing.json"));
}
