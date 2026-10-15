# Ampersand

Ampersand is an independent Linux control application for the **Neural DSP Quad
Cortex**. It speaks the unit's own USB-HID/Protobuf protocol directly, so it
needs no vendor software and no network. Written in C++26.

> **Unofficial.** Ampersand is not affiliated with, endorsed by, or connected
> to Neural DSP. "Neural DSP", "Quad Cortex" and "Cortex Control" are
> trademarks of their respective owner and are used here only to state which
> device this software talks to. No warranty of any kind — use at your own
> risk.

## What it is

| Component | Purpose |
|---|---|
| `ampersand` | Qt Widgets GUI: the signal grid (4 rows x 8 columns), block parameter editor, scenes A–H, preset library, Save As, master volume, live sync with the unit. |
| `qcctl` | Command-line tool for everything the library can do: version, folders, presets, recall, live-grid dump (text/JSON), parameters, bypass, blocks, save, catalog search, traffic monitor. |
| `qc-mcp` | MCP server (stdio JSON-RPC) exposing `qc_*` tools for USB control of the unit. |
| `libqccore` | The library behind all three (`include/qc/`). |

## Build

Ampersand uses CMake presets. Requirements: `clang++-22` (C++26), Qt 6
(Widgets), protobuf, zlib, tinyxml2, nlohmann_json, Catch2 v3 and ALSA.

```bash
cmake --preset default            # GUI + CLI + MCP + tests
cmake --build --preset default
ctest --test-dir build/default --output-on-failure
cmake --install build/default     # -> ~/.local/bin/{ampersand,qcctl,qc-mcp}
```

Presets: `default` (everything), `nogui` (core, CLI, MCP, tests — no Qt),
`asan` (Debug with AddressSanitizer/UBSan).

Debian/Ubuntu packages:

```
protobuf-compiler libprotobuf-dev zlib1g-dev libtinyxml2-dev \
nlohmann-json3-dev catch2 libasound2-dev
```

plus Qt 6 Widgets. No hidapi is needed — the transport uses Linux `hidraw`
directly.

A portable AppImage that bundles all three programs and Qt can be built with
`tools/build_appimage.py`; a pre-built archive is published under `releases/`.
It runs on its own — no SSH, no network, nothing is installed on the unit.

## Device access (once)

Raw access to `/dev/hidrawN` belongs to root by default. Install the udev rule
once:

```bash
sudo install -m 644 udev/99-neural-quad-cortex.rules /etc/udev/rules.d/
sudo udevadm control --reload-rules && sudo udevadm trigger --subsystem-match=hidraw
qcctl devices        # must say "rw"
```

Then unplug and replug the unit. Quit Cortex Control or any WebHID page holding
the device first — only one program can own the HID connection.

## Quick start

Ampersand talks to the unit over USB only, through the unit's own protocol
messages. Commands that save, delete or restore presets
change the unit's library, so save a backup before the first writing
command:

```bash
qcctl backup-save my-backup.json   # the unit builds a local backup (~90 s); keep the file
```

`qcctl backup-restore` exists but has not yet been tried on a real unit.

```bash
qcctl version                 # read the version, no handshake
qcctl info                    # handshake: position, scene, mode, volume
qcctl current                 # the live grid with catalog names
qcctl presets --factory       # list the factory library
qcctl recall 28C --factory    # recall a preset (interrupts audio)
qcctl scene B
qcctl param 1 1 0 0.5         # row 1, column 1, parameter 0 := 0.5 (normalized)
qcctl save 30A "My Preset"    # Save As into My Presets
qcctl catalog darkglass       # search the unit's block catalog
ampersand                     # launch the GUI
```

## MCP server

`qc-mcp` is a stdio MCP server. Point any MCP client at it:

```bash
mcp add qc -- ~/.local/bin/qc-mcp
```

It offers `qc_*` tools for USB control. The server is **read-only by
default**; pass `--allow-writes` to enable the tools that change the unit or
write files:

```bash
mcp add qc -- ~/.local/bin/qc-mcp --allow-writes
```

`qc_backup_save` and `qc_backup_restore` take a plain file name, not a path.
Backups live only in `~/.local/share/ampersand/backups` (or
`$XDG_DATA_HOME/ampersand/backups`; override with `--backup-dir <absolute
dir>`). A save never overwrites an existing file; files are created with mode
0600. A restore reads only a regular file of at most 64 MiB.

## Layout

```
include/qc/    public headers (libqccore API)
src/core/      library: framing, compression, session handshake, hidraw
               transport, catalog, preset_view, device API, audio, metering
src/cli/       qcctl
src/mcp/       qc-mcp (+ reusable mcp_stdio_server.hpp)
src/gui/       the Qt Widgets GUI (ampersand)
proto/         the reconstructed protobuf schema
tests/         Catch2, offline, a fake transport simulates the unit
docs/          architecture and protocol notes
tools/         helper scripts (build, measurement, diagnostics)
udev/          hidraw access rule
```

## Provenance

The protocol knowledge builds on two open-source projects whose wire captures
and hardware read-backs are cited in `docs/protocol.md`:
[pyquadcortex](https://github.com/stokes-audio/pyquadcortex) (MIT) and
[CortexUSB](https://github.com/VanIseghemThomas/CortexUSB) (MIT).

## License

Apache License 2.0. See [`LICENSE`](LICENSE). The protobuf schemas under
`proto/` are taken unchanged from pyquadcortex (MIT,
[`proto/LICENSE.pyquadcortex`](proto/LICENSE.pyquadcortex)); see
`proto/README.md`. The portable build bundles Qt (LGPL-3.0) and other
libraries; see [`packaging/THIRD-PARTY-NOTICES.md`](packaging/THIRD-PARTY-NOTICES.md).
