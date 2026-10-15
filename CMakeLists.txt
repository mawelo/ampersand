# ampersand — Build-Beschreibung
#
# Wer:      Martin.Lonkwitz
# Was:      CMake-Projekt fuer die Linux-Steuerung des Neural DSP Quad Cortex
#           (Kernbibliothek, CLI, MCP-Server, Qt-GUI, Tests).
# Wann:     2026-09-04
# Warum:    Cortex Control existiert nur fuer macOS/Windows; dieses Projekt
#           spricht das USB-HID/Protobuf-Protokoll des Geraets unter Linux.
# Projekt:  Ampersand (Sub-Projekt von Quad-Cortex)
# Abhaengig: clang++-22, C++26, protobuf 3.21, zlib, tinyxml2,
#           nlohmann_json, Catch2 v3, Qt 6.11 (Widgets) aus ~/Documents/Qt
# Nutzung:  cmake --preset default && cmake --build --preset default
#           ctest --test-dir build/default --output-on-failure
# Historie: git log
cmake_minimum_required(VERSION 3.28)

project(ampersand
    VERSION 0.1.0
    DESCRIPTION "Linux control software for the Neural DSP Quad Cortex"
    LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 26)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

option(QC_BUILD_GUI   "Build the Qt Widgets GUI (ampersand)" ON)
option(QC_BUILD_TESTS "Build the Catch2 test suite" ON)
option(QC_SANITIZE    "Enable AddressSanitizer + UndefinedBehaviorSanitizer" OFF)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")

# ─── Dependencies ───────────────────────────────────────────────────────────
find_package(Protobuf REQUIRED)
find_package(ZLIB REQUIRED)
find_package(nlohmann_json 3.11 REQUIRED)
find_package(Threads REQUIRED)

find_package(tinyxml2 CONFIG QUIET)
if(NOT tinyxml2_FOUND)
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(TINYXML2 REQUIRED IMPORTED_TARGET tinyxml2)
    add_library(tinyxml2::tinyxml2 ALIAS PkgConfig::TINYXML2)
endif()

# Build paths out of binaries and debug info: the home directory of the
# machine that built a release must not travel with it.
if(DEFINED ENV{HOME})
    add_compile_options("-ffile-prefix-map=$ENV{HOME}=~")
endif()

# ─── Warning / hardening interface targets ──────────────────────────────────
add_library(qc_warnings INTERFACE)
target_compile_options(qc_warnings INTERFACE
    -Wall -Wextra -Wpedantic -Wshadow -Wconversion -Wsign-conversion
    -Wnon-virtual-dtor -Wold-style-cast -Wcast-align -Wunused
    -Woverloaded-virtual -Wnull-dereference -Wdouble-promotion -Wformat=2
    -Wno-c2y-extensions -Werror)
target_compile_definitions(qc_warnings INTERFACE _GLIBCXX_ASSERTIONS)

add_library(qc_hardening INTERFACE)
target_compile_options(qc_hardening INTERFACE
    -fstack-protector-strong -fstack-clash-protection -fcf-protection=full)
target_compile_definitions(qc_hardening INTERFACE
    $<$<NOT:$<CONFIG:Debug>>:_FORTIFY_SOURCE=3>)
target_link_options(qc_hardening INTERFACE -Wl,-z,relro,-z,now)

if(QC_SANITIZE)
    target_compile_options(qc_hardening INTERFACE -fsanitize=address,undefined -fno-omit-frame-pointer)
    target_link_options(qc_hardening INTERFACE -fsanitize=address,undefined)
endif()

# ─── Generated protobuf bindings (recovered schema, see proto/README.md) ────
protobuf_generate_cpp(QC_PROTO_SRCS QC_PROTO_HDRS
    proto/Preset.proto
    proto/ProductionAutomation.proto)
add_library(qc_proto STATIC ${QC_PROTO_SRCS} ${QC_PROTO_HDRS})
target_include_directories(qc_proto SYSTEM PUBLIC ${CMAKE_CURRENT_BINARY_DIR})
target_link_libraries(qc_proto PUBLIC protobuf::libprotobuf)
# Generated code is not ours to lint.
target_compile_options(qc_proto PRIVATE -w)

# ─── Version string with the commit id baked in at BUILD time ───────────────
set(QC_VERSION_CPP "${CMAKE_CURRENT_BINARY_DIR}/generated/qc_version.cpp")
add_custom_target(qc_version_gen
    COMMAND ${CMAKE_COMMAND}
        -DQC_OUT=${QC_VERSION_CPP}
        -DQC_SRC_DIR=${CMAKE_CURRENT_SOURCE_DIR}
        -DQC_PROJECT_VERSION=${PROJECT_VERSION}
        -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/gen_version.cmake
    BYPRODUCTS ${QC_VERSION_CPP}
    COMMENT "Generating version string (commit id)"
    VERBATIM)

# ─── Core library ───────────────────────────────────────────────────────────
add_library(qccore STATIC
    src/core/framing.cpp
    src/core/compression.cpp
    src/core/messages.cpp
    src/core/log.cpp
    src/core/hid_transport.cpp
    src/core/midi.cpp
    src/core/pitch.cpp
    src/core/audio.cpp
    src/core/session.cpp
    src/core/catalog.cpp
    src/core/preset_view.cpp
    src/core/device.cpp
    src/core/io_meter.cpp
    src/core/ir.cpp
    src/core/wav.cpp
    ${QC_VERSION_CPP})
add_dependencies(qccore qc_version_gen)
find_package(PkgConfig REQUIRED)
pkg_check_modules(ALSA REQUIRED IMPORTED_TARGET alsa)
target_link_libraries(qccore PUBLIC PkgConfig::ALSA)
target_include_directories(qccore PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(qccore
    PUBLIC qc_proto ZLIB::ZLIB tinyxml2::tinyxml2
           nlohmann_json::nlohmann_json Threads::Threads
    PRIVATE qc_warnings qc_hardening)

# ─── Command line tool ──────────────────────────────────────────────────────
add_executable(qcctl src/cli/main.cpp)
target_link_libraries(qcctl PRIVATE qccore qc_warnings qc_hardening)

# ─── MCP server (stdio JSON-RPC) ────────────────────────────────────────────
add_executable(qc-mcp src/mcp/main.cpp src/mcp/tools.cpp src/mcp/backup_store.cpp)
target_link_libraries(qc-mcp PRIVATE qccore qc_warnings qc_hardening)

# ─── Qt GUI ─────────────────────────────────────────────────────────────────
if(QC_BUILD_GUI)
    add_subdirectory(src/gui)
endif()

# ─── Tests ──────────────────────────────────────────────────────────────────
if(QC_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()

# ─── Install ────────────────────────────────────────────────────────────────
include(GNUInstallDirs)
install(TARGETS qcctl qc-mcp RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
install(FILES udev/99-neural-quad-cortex.rules
        DESTINATION ${CMAKE_INSTALL_LIBDIR}/udev/rules.d OPTIONAL)
