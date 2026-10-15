# Ampersand — User Guide

**Ampersand** is an independent Linux client for the Neural DSP Quad Cortex.
It talks to the device over USB HID. Everything is in one file: the graphical app, the
command-line tool `qcctl` and the MCP server `qc-mcp`.

> **Not affiliated with Neural DSP.** Ampersand is an independent project, not
> endorsed by or connected to Neural DSP. "Neural DSP", "Quad Cortex" and
> "Cortex Control" are their trademarks and appear here only to say which
> device this works with.
> No warranty of any kind. Use at your own risk. File operations can void
> your warranty or leave the device unusable.

## 1. Getting started

```
chmod +x Ampersand-x86_64.AppImage
sudo ./install-udev.sh          # once: lets you reach /dev/hidrawN without root
./Ampersand-x86_64.AppImage
```

Unplug and replug the Quad Cortex after installing the udev rule.

Requirements: x86_64 Linux, X11 or XWayland for the graphical app, FUSE 2 for
the AppImage format. If FUSE is missing, run it as
`./Ampersand-x86_64.AppImage --appimage-extract-and-run`.

Nothing else needs installing. Qt and every other library are inside the file.

## 2. The command line

The same file carries `qcctl`. Pass its name first, or make a symlink:

```
./Ampersand-x86_64.AppImage qcctl version
ln -s Ampersand-x86_64.AppImage qcctl && ./qcctl version
```

Useful commands:

| Command | What it does |
|---|---|
| `qcctl version` | firmware, serial, CorOS version, MAC |
| `qcctl info` | version plus current preset position and scene |
| `qcctl current` | the live grid, including unsaved edits |
| `qcctl export out.json` | the whole signal chain with every setting |
| `qcctl presets` / `recall <slot>` | list and load presets |
| `qcctl scene A` | switch scene |
| `qcctl param <row> <col> <idx> <v>` | set one block parameter, 0..1 |
| `qcctl block <row> <col> <model>` | place a block; `qcctl catalog <text>` finds model numbers |
| `qcctl volume` / `tempo` / `tuner on` | master volume, tempo, tuner |
| `qcctl backup-save f.json` | read the device backup |

`--json` gives machine-readable output where it makes sense. `qcctl` with no
arguments prints the full list.

## 3. What Ampersand can and cannot do, next to Cortex Control

Legend: yes · partly · no · **plus** = Ampersand has it and Cortex Control
does not.

### Everyday control — complete and verified on hardware

| | Cortex Control | Ampersand |
|---|---|---|
| View and edit the live grid, blocks, parameters, bypass | yes | yes |
| Routing, split and merge, lane outputs | yes | yes |
| Place, remove and drag blocks | yes | yes |
| Scenes A–H: switch, rename, colour, copy | yes | yes |
| Presets and setlists: folders, recall, save as, delete, move | yes | yes |
| Search the model catalogue | yes | yes |
| Master volume, tempo, mode | yes | yes |
| Tuner on and off | yes | yes |
| Reorder items inside a library folder | yes | partly |

### Backup and device information

| | Cortex Control | Ampersand |
|---|---|---|
| Record a device backup | yes | yes |
| Restore a backup | yes | partly |
| Show the installed CorOS version | yes | yes |

### Things Cortex Control does and Ampersand does not

| | |
|---|---|
| Cortex Cloud login | no |
| Collect the device's internal logs | no |
| Firmware update | no |
| Download presets, captures or IRs from the cloud | no |
| Create a Neural Capture (reamping) | no, that runs on the device |
| Upload custom IRs over USB | no |
| Gig View, Looper, undo and redo | no |
| Settings screens for global EQ, I/O and MIDI | partly, messages exist, no interface |

### Things Ampersand does and Cortex Control does not

| | |
|---|---|
| Runs on Linux | **plus** |
| Export the whole signal chain as JSON | **plus** |
| Drive everything from a script or an AI assistant over MCP | **plus** |

## 4. One caveat

**The graphical app runs on X11 and on Wayland.** Qt picks the platform of the
session; `QT_QPA_PLATFORM=wayland` or `QT_QPA_PLATFORM=xcb` forces one.

## 5. Privacy

Ampersand talks to the device over USB and to nothing else. It contacts no
server.

## 6. If something goes wrong

- **The device does not answer.** Quit Neural's Cortex Control if it is
  running; only one program can hold the HID connection. After a reboot the
  device can stay silent for ten to twenty seconds.
- **Permission denied on /dev/hidrawN.** The udev rule is not installed or the
  device was not replugged afterwards.
- **The AppImage will not start.** FUSE 2 is missing; use
  `--appimage-extract-and-run`.

The binaries keep their symbols and debug information, so a crash produces a
usable stack trace.
