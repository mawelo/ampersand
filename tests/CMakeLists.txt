# tests/CMakeLists.txt — Catch2-Testsuite (offline, ohne Geraet)
find_package(Catch2 3 REQUIRED)
include(Catch)

add_executable(qc_tests
    test_framing.cpp
    test_compression.cpp
    test_catalog.cpp
    test_preset_view.cpp
    test_session.cpp
    test_device.cpp
    test_midi.cpp
    test_pitch.cpp
    test_ir.cpp
    test_wav.cpp
    test_io_meter.cpp
    test_backup_store.cpp
    ../src/mcp/backup_store.cpp
    fake_transport.hpp)
target_link_libraries(qc_tests PRIVATE qccore Catch2::Catch2WithMain qc_warnings)
catch_discover_tests(qc_tests)
