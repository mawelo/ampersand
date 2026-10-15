# Ampersand - portable build

Everything in one file: the GUI, the command-line tool `qcctl` and the MCP
server `qc-mcp`. Qt and all other libraries are embedded, so there is nothing
to install and no version to match. x86_64 only.

## Run the GUI

    chmod +x Ampersand-x86_64.AppImage
    ./Ampersand-x86_64.AppImage

Runs natively on Wayland and on X11; Qt picks the platform of the session.
To force one: `QT_QPA_PLATFORM=wayland` or `QT_QPA_PLATFORM=xcb`.

## Run the command-line tool

The same file carries `qcctl`. Either pass its name as the first argument:

    ./Ampersand-x86_64.AppImage qcctl version
    ./Ampersand-x86_64.AppImage qcctl presets

or make a symlink and call that, which is nicer for scripts:

    ln -s Ampersand-x86_64.AppImage qcctl
    ./qcctl version

`qc-mcp`, the MCP server, works the same way.

## One-time: allow USB access without root

Raw access to /dev/hidrawN belongs to root by default. Once:

    sudo ./install-udev.sh

Then unplug and replug the Quad Cortex.

## If it will not start

The AppImage format needs FUSE 2. Where that is missing:

    ./Ampersand-x86_64.AppImage --appimage-extract-and-run

## What this talks to

USB only. Nothing is sent anywhere unless you ask for it explicitly.
