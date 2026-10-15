# Third-party notices

Ampersand itself is licensed under the Apache License 2.0 (`LICENSE`).
The portable build (AppImage) bundles third-party software under the
licenses below. Inside the AppImage the full texts live in
`usr/share/doc/ampersand/`; list them with

    ./Ampersand-x86_64.AppImage --appimage-extract
    ls squashfs-root/usr/share/doc/ampersand/ squashfs-root/usr/share/doc/

## Qt 6.11.1 — LGPL-3.0-only

The Qt libraries (`usr/lib/libQt6*.so.6`) and Qt plugins (`usr/plugins/`)
are used under the GNU Lesser General Public License v3
(`qt/LGPL-3.0-only.txt`, which builds on `qt/GPL-3.0-only.txt`). They are
unmodified builds from the Qt online installer, linked dynamically.

- Source code: the matching Qt 6.11.1 sources are available from
  <https://download.qt.io/official_releases/qt/>. On request, the exact
  sources used for this build will be provided.
- Replacing Qt: extract the AppImage (see above), replace the libraries in
  `squashfs-root/usr/lib/` with your own build of the same major version and
  start `squashfs-root/AppRun`.
- Qt contains further third-party components; their license texts are in
  `qt/` next to the LGPL text (copied from the `LICENSES/` folders of the Qt
  modules that are bundled).

## ICU (shipped with Qt)

`libicu*.so.73` come with the Qt installation. Unicode license text:
`qt/Unicode-3.0.txt`.

## Protocol schemas — MIT

`proto/Preset.proto` and `proto/ProductionAutomation.proto` are taken
unchanged from pyquadcortex (Copyright (c) 2026 Stokes, MIT License,
`LICENSE.pyquadcortex`). The C++ code generated from them is part of all
three programs.

## System libraries

The remaining libraries in `usr/lib/` (protobuf, zstd, brotli, xcb,
xkbcommon, glib, dbus, krb5 and others) are copied from Debian/Ubuntu
packages. Their copyright and license files are in
`usr/share/doc/<package>/copyright`.
