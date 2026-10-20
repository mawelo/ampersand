#!/usr/bin/env python3
"""build_appimage.py - Baut das portable Ampersand-Paket.

Wer:      Martin.Lonkwitz
Was:      Bündelt GUI, CLI und MCP-Server (build/default/{src/gui/cortex-control,
          qcctl,qc-mcp}) samt Qt und allen übrigen Abhängigkeiten zu EINER
          ausführbaren AppImage und legt ein Verteil-Paket unter dist/ an
          (AppImage + udev-Regel + Install-Skript + README + USER-GUIDE +
          Lizenzen) sowie
          ein Zip Ampersand_<version>.zip unter ~/Downloads
          (ZIP_DIR ueberschreibt das). Zum Testen ohne
          Dependency-/Versions-Stress: Datei laden, ausführbar machen, starten.
          Einziger Host-Eingriff: die udev-Regel (einmalig, für nicht-root-
          Zugriff auf /dev/hidrawN).

          Die drei Programme stecken in derselben Datei. Welches läuft,
          entscheidet der Aufrufname oder das erste Argument:
              ./Ampersand-x86_64.AppImage            -> GUI
              ./Ampersand-x86_64.AppImage qcctl ...  -> CLI
              ln -s ...AppImage qcctl && ./qcctl ...       -> CLI
Wann:     2026-09-04
Warum:    linuxdeploy zieht sonst eine ältere System-libQt6Core statt 6.11.1
          (fehlendes Qt_6.11-Symbol); das TIFF-Bildplugin braucht libtiff.so.5,
          das hier fehlt. Beides wird hier gezielt behandelt.
Projekt:  Ampersand (SUB-PROJECT FROM: Quad-Cortex)
Nutzung:  tools/build_appimage.py            # nutzt ~/Documents/Qt/6.11.1
          QT_DIR=/pfad/zu/Qt/6.x/gcc_64 tools/build_appimage.py
Abhängig: linuxdeploy + linuxdeploy-plugin-qt + appimagetool (werden bei Bedarf
          nach tools/appimage-tools/ geladen), Python3-PIL (Icon), FUSE2.
Historie: git log
"""
from __future__ import annotations

import hashlib
import os
import shutil
import subprocess
import sys
import tempfile
import urllib.request
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent
QT_DIR = Path(os.environ.get("QT_DIR", str(Path.home() / "Documents/Qt/6.11.1/gcc_64")))
BIN = REPO / "build/default/src/gui/ampersand"
TOOL_BINS = {"qcctl": REPO / "build/default/qcctl", "qc-mcp": REPO / "build/default/qc-mcp"}
TOOLS = REPO / "tools/appimage-tools"
WORK = REPO / "build/appimage"
DIST = REPO / "dist"
APP = "ampersand"

TOOL_URLS = {
    "linuxdeploy": "https://github.com/linuxdeploy/linuxdeploy/releases/download/continuous/linuxdeploy-x86_64.AppImage",
    "linuxdeploy-plugin-qt": "https://github.com/linuxdeploy/linuxdeploy-plugin-qt/releases/download/continuous/linuxdeploy-plugin-qt-x86_64.AppImage",
    "appimagetool": "https://github.com/AppImage/appimagetool/releases/download/continuous/appimagetool-x86_64.AppImage",
}
# Die continuous-URLs sind an eine feste SHA-256 gebunden (Review M7): ein
# abweichender Upstream-Build schlaegt fehl, statt ungeprueft zu laufen. Beim
# bewussten Anheben einer Version wird die Pruefsumme hier mit aktualisiert
# (sha256sum tools/appimage-tools/<name>).
TOOL_SHA256 = {
    "linuxdeploy": "8aea8da0f7f7039d2a2cecb14657d752a222a5e1d3825caeef186c82f751cdd1",
    "linuxdeploy-plugin-qt": "cfc1055b2b9dbc08412b579f20990b7b41a17b61beaa5847dc9477c96c9e9617",
    "appimagetool": "95cbe7cce9717fce90c484e34052ee7c7f1d7635b33c12525b4776826a7d29b6",
}


def _sha256(path: Path) -> str:
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def fetch_tools() -> None:
    TOOLS.mkdir(parents=True, exist_ok=True)
    for name, url in TOOL_URLS.items():
        dst = TOOLS / name
        want = TOOL_SHA256[name]
        if dst.exists():
            got = _sha256(dst)
            if got == want:
                continue
            raise SystemExit(
                f"{dst} does not match the pinned SHA-256.\n"
                f"  expected {want}\n  got      {got}\n"
                "Remove the file to re-fetch, or update TOOL_SHA256 on a deliberate bump.")
        print(f"downloading {name} ...")
        # Erst in eine Temporaerdatei neben dem Ziel, pruefen, dann atomar
        # umbenennen: ein abgebrochener Download bleibt nicht als 'fertiges'
        # Werkzeug liegen.
        fd, tmp_name = tempfile.mkstemp(dir=TOOLS, prefix=f".{name}.", suffix=".part")
        os.close(fd)
        tmp = Path(tmp_name)
        try:
            urllib.request.urlretrieve(url, tmp)  # noqa: S310 (GitHub release, SHA-256-checked next)
            got = _sha256(tmp)
            if got != want:
                raise SystemExit(
                    f"downloaded {name} does not match the pinned SHA-256.\n"
                    f"  expected {want}\n  got      {got}\n  url      {url}")
            tmp.chmod(0o755)
            os.replace(tmp, dst)
        finally:
            if tmp.exists():
                tmp.unlink()


def make_icon(path: Path) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    try:
        from PIL import Image, ImageDraw, ImageFont
    except ImportError:
        # 1x1 transparent PNG fallback; linuxdeploy only needs a valid file.
        path.write_bytes(bytes.fromhex(
            "89504e470d0a1a0a0000000d494844520000000100000001080600000"
            "01f15c4890000000d49444154789c6360000002000001e221bc330000"
            "0000049454e44ae426082"))
        return
    img = Image.new("RGBA", (256, 256), (24, 26, 30, 255))
    d = ImageDraw.Draw(img)
    d.rounded_rectangle([16, 16, 240, 240], radius=32, fill=(32, 120, 140, 255))
    try:
        f = ImageFont.truetype("/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf", 120)
    except OSError:
        f = ImageFont.load_default()
    d.text((128, 120), "QC", font=f, anchor="mm", fill=(240, 244, 248, 255))
    img.save(path)


def build_appdir() -> Path:
    appdir = WORK / "AppDir"
    if appdir.exists():
        shutil.rmtree(appdir)
    (appdir / "usr/bin").mkdir(parents=True)
    (appdir / "usr/share/applications").mkdir(parents=True)
    shutil.copy2(BIN, appdir / "usr/bin/ampersand")
    # CLI und MCP-Server wandern mit hinein. linuxdeploy sammelt die
    # Abhaengigkeiten ALLER Programme unter usr/bin ein, sie sind damit ebenso
    # abgeschlossen wie die GUI.
    for name, path in TOOL_BINS.items():
        if not path.exists():
            raise SystemExit(f"binary not found: {path}\nBuild first: cmake --build --preset default")
        shutil.copy2(path, appdir / "usr/bin" / name)
        (appdir / "usr/bin" / name).chmod(0o755)
    make_icon(appdir / f"usr/share/icons/hicolor/256x256/apps/{APP}.png")
    desktop = (
        "[Desktop Entry]\nType=Application\nName=Ampersand\n"
        "Comment=Control the Neural DSP Quad Cortex over USB\n"
        "Exec=ampersand\nIcon=ampersand\nCategories=AudioVideo;Audio;\n"
        "Terminal=false\n"
    )
    (appdir / f"{APP}.desktop").write_text(desktop)
    shutil.copy2(appdir / f"{APP}.desktop", appdir / "usr/share/applications" / f"{APP}.desktop")
    return appdir


APPRUN = """#!/bin/sh
# AppRun - waehlt das Programm nach Aufrufname oder erstem Argument.
#
# Die AppImage enthaelt drei Programme. Ohne Angabe startet die GUI; heisst der
# Aufruf qcctl oder qc-mcp (per Symlink oder ARGV0), laeuft dieses. Alternativ
# als erstes Argument: `... qcctl version`.
HERE="$(dirname "$(readlink -f "$0")")"
export PATH="$HERE/usr/bin:$PATH"

# Umgebungs-Haken von linuxdeploy (Qt-Plugins, LD_LIBRARY_PATH) uebernehmen.
for hook in "$HERE"/apprun-hooks/*.sh; do
    [ -e "$hook" ] && . "$hook"
done

name="$(basename "${ARGV0:-$0}")"
case "$name" in
    qcctl|qc-mcp) exec "$HERE/usr/bin/$name" "$@" ;;
esac
case "$1" in
    qcctl|qc-mcp) prog="$1"; shift; exec "$HERE/usr/bin/$prog" "$@" ;;
esac
exec "$HERE/usr/bin/ampersand" "$@"
"""


def install_apprun(appdir: Path) -> None:
    """Ersetzt das AppRun von linuxdeploy durch die Weiche.

    Die Umgebungs-Haken (apprun-hooks/) bleiben unangetastet und werden vom
    neuen AppRun eingelesen - sonst faende Qt seine Plugins nicht.
    """
    run = appdir / "AppRun"
    if run.exists() or run.is_symlink():
        run.unlink()
    run.write_text(APPRUN)
    run.chmod(0o755)


# Das TIFF-Bildplugin braucht libtiff.so.5, das hier fehlt; linuxdeploy bricht
# daran ab. Es wird deshalb waehrend des Laufs beiseitegelegt. Die Anwendung
# laedt nie TIFF-Bilder.
#
# WICHTIG: Die Ablage liegt NEBEN dem Original in der Qt-Installation, nicht im
# Build-Verzeichnis. Frueher lag sie unter build/appimage/ - wurde der Lauf
# abgebrochen, blieb die Datei dort liegen, und ein spaeteres `rm -rf build/`
# hat sie mitgenommen. Das Ergebnis war eine kaputte Qt-Installation, in der
# JEDES CMake-Projekt mit Qt6::Gui nicht mehr konfiguriert. Genau so passiert
# am 2026-09-09.
TIFF = QT_DIR / "plugins/imageformats/libqtiff.so"
# Die Ablage darf NICHT im Plugin-Verzeichnis liegen: linuxdeploy durchsucht
# das Verzeichnis vollstaendig und sammelt jede .so darin ein, egal wie sie
# heisst - dann scheitert es wieder an libtiff.so.5. Also ein eigenes
# Unterverzeichnis in der Qt-Installation, das linuxdeploy nicht anfasst und
# das kein `rm -rf build/` trifft.
TIFF_STASH = QT_DIR / ".appimage-stash/libqtiff.so"


def restore_tiff_plugin() -> None:
    """Holt das Plugin zurueck, falls ein frueherer Lauf abgebrochen wurde."""
    if TIFF_STASH.exists() and not TIFF.exists():
        shutil.move(str(TIFF_STASH), str(TIFF))
        print("restored the Qt TIFF plugin from an interrupted earlier run")


def check_qt_intact() -> None:
    if TIFF.exists() or TIFF_STASH.exists():
        return
    raise SystemExit(
        f"missing: {TIFF}\n"
        "Qt cannot be configured without it - every CMake project using Qt6::Gui\n"
        "will fail. Reinstall it with the Qt Maintenance Tool:\n"
        f"  {Path.home()}/Documents/Qt/MaintenanceTool\n"
        "  -> Qt 6.11.1 -> Additional Libraries -> Qt Image Formats -> reinstall")


# Lizenzen (Review 2026-10-15, M8). Die Qt-Texte stammen aus den LICENSES/-
# Ordnern der Qt-Quellen neben QT_DIR (Qt-Installer: <ver>/Src/<modul>/LICENSES).
# Welche Module noetig sind, ergibt sich aus den Bibliotheken, die linuxdeploy
# tatsaechlich eingesammelt hat - deshalb laeuft das NACH linuxdeploy und VOR
# dem Packen.
QT_SRC = QT_DIR.parent / "Src"
QT_MODULE_OF = {  # Praefix der Bibliothek -> Qt-Quellmodul
    "libQt6Pdf": "qtwebengine",
    "libQt6Qml": "qtdeclarative",
    "libQt6Quick": "qtdeclarative",
    "libQt6Svg": "qtsvg",
    "libQt6VirtualKeyboard": "qtvirtualkeyboard",
    "libQt6Wayland": "qtwayland",
    "libQt6WlShell": "qtwayland",
}
OWN_LICENSES = {"LICENSE": REPO / "LICENSE",
                "LICENSE.pyquadcortex": REPO / "proto/LICENSE.pyquadcortex",
                "THIRD-PARTY-NOTICES.md": REPO / "packaging/THIRD-PARTY-NOTICES.md"}


def qt_modules(appdir: Path) -> list[str]:
    mods = {"qtbase"}
    for lib in (appdir / "usr/lib").glob("libQt6*.so*"):
        for prefix, mod in QT_MODULE_OF.items():
            if lib.name.startswith(prefix):
                mods.add(mod)
    return sorted(mods)


def install_licenses(appdir: Path) -> None:
    doc = appdir / "usr/share/doc/ampersand"
    qt = doc / "qt"
    qt.mkdir(parents=True, exist_ok=True)
    for name, src in OWN_LICENSES.items():
        shutil.copy2(src, doc / name)
    for d in [QT_SRC / "LICENSES"] + [QT_SRC / m / "LICENSES" for m in qt_modules(appdir)]:
        if not d.is_dir():
            raise SystemExit(f"Qt license folder missing: {d}\n"
                             "Install the Qt sources (Maintenance Tool -> Qt 6.11.1 -> Sources).")
        for f in d.iterdir():
            if f.is_file() and not f.name.startswith("LicenseRef-Qt-Commercial"):
                shutil.copy2(f, qt / f.name)
    for needed in ("LGPL-3.0-only.txt", "GPL-3.0-only.txt", "Unicode-3.0.txt"):
        if not (qt / needed).exists():
            raise SystemExit(f"license text not found in the Qt sources: {needed}")


def run_linuxdeploy(appdir: Path) -> Path:
    tiff, stash = TIFF, TIFF_STASH
    if tiff.exists():
        stash.parent.mkdir(parents=True, exist_ok=True)
        shutil.move(str(tiff), str(stash))
    try:
        env = dict(os.environ)
        env["PATH"] = f"{TOOLS}:{env.get('PATH', '')}"
        # Force the resolver onto Qt 6.11.1 (a system libQt6Core lacks Qt_6.11).
        env["LD_LIBRARY_PATH"] = f"{QT_DIR/'lib'}:{env.get('LD_LIBRARY_PATH', '')}"
        env["QMAKE"] = str(QT_DIR / "bin/qmake6")
        # X11 UND Wayland (2026-10-02, Wunsch des Nutzers): Qt waehlt selbst -
        # in einer Wayland-Sitzung wayland, sonst xcb; QT_QPA_PLATFORM
        # uebersteuert. Qt 6.11.1 bringt libqwayland.so mit.
        env["EXTRA_PLATFORM_PLUGINS"] = "libqxcb.so;libqwayland.so"
        # Symbole und Debug-Informationen BLEIBEN drin. linuxdeploy strippt
        # sonst alles mit. Bewusste Entscheidung: das Paket soll sich
        # analysieren und im Fehlerfall mit einem Debugger untersuchen lassen -
        # ein Stacktrace aus einem gestrippten Binary ist wertlos. Kostet
        # Platz, nichts sonst.
        env["NO_STRIP"] = "1"
        env["OUTPUT"] = "Ampersand-x86_64.AppImage"
        env["VERSION"] = "0.1.0"
        cmd = [
            str(TOOLS / "linuxdeploy"), "--appdir", str(appdir),
            "-d", str(appdir / f"{APP}.desktop"),
            "-i", str(appdir / f"usr/share/icons/hicolor/256x256/apps/{APP}.png"),
            "--plugin", "qt",
        ]
        # Bewusst OHNE --output appimage: erst das AppDir fertigstellen, dann
        # das AppRun durch die Weiche ersetzen, erst danach packen.
        subprocess.run(cmd, cwd=WORK, env=env, check=True)
        install_apprun(appdir)
        install_licenses(appdir)
        subprocess.run([str(TOOLS / "appimagetool"), str(appdir), str(WORK / env["OUTPUT"])],
                       cwd=WORK, env=env, check=True)
    finally:
        if stash.exists():
            shutil.move(str(stash), str(tiff))
    out = WORK / "Ampersand-x86_64.AppImage"
    if not out.exists():
        raise SystemExit("AppImage not produced")
    return out


README_TEXT = """# Ampersand - portable build

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
"""


def make_zip(version: str) -> Path:
    """Packt dist/ als Ampersand_<version>.zip - ein Download.

    Die Datei landet AUSSERHALB des Repos: Binaerartefakte gehoeren nicht in
    die Versionsverwaltung (siehe rechtliches/, Punkt A3). Vorgabe ist
    ~/Downloads, ueberschreibbar mit ZIP_DIR.
    """
    import zipfile
    outdir = Path(os.environ.get("ZIP_DIR", str(Path.home() / "Downloads")))
    outdir.mkdir(parents=True, exist_ok=True)
    out = outdir / f"Ampersand_{version}.zip"
    if out.exists():
        out.unlink()
    with zipfile.ZipFile(out, "w", zipfile.ZIP_DEFLATED) as z:
        for f in sorted(DIST.iterdir()):
            info = zipfile.ZipInfo.from_file(f, f.name)
            info.external_attr = (0o755 if os.access(f, os.X_OK) else 0o644) << 16
            info.compress_type = zipfile.ZIP_DEFLATED
            with open(f, "rb") as fh:
                z.writestr(info, fh.read())
    return out


def assemble_dist(appimage: Path) -> None:
    if DIST.exists():
        shutil.rmtree(DIST)
    DIST.mkdir(parents=True)
    shutil.copy2(appimage, DIST / appimage.name)
    (DIST / appimage.name).chmod(0o755)
    shutil.copy2(REPO / "udev/99-neural-quad-cortex.rules", DIST / "99-neural-quad-cortex.rules")
    install = (
        "#!/usr/bin/env bash\n"
        "# Installs the udev rule so /dev/hidrawN for the Quad Cortex is usable\n"
        "# without root. Run once: sudo ./install-udev.sh\n"
        "set -euo pipefail\n"
        "d=$(cd \"$(dirname \"$0\")\" && pwd)\n"
        "install -m 0644 \"$d/99-neural-quad-cortex.rules\" /etc/udev/rules.d/99-neural-quad-cortex.rules\n"
        "udevadm control --reload-rules && udevadm trigger\n"
        "echo 'udev rule installed. Replug the Quad Cortex, then run the AppImage.'\n"
    )
    (DIST / "install-udev.sh").write_text(install)
    (DIST / "install-udev.sh").chmod(0o755)
    (DIST / "README.md").write_text(README_TEXT)
    # Der ausfuehrliche Guide liegt versioniert unter packaging/ und wird nur
    # kopiert - so ist er im Repo diff-bar und nicht in einem String vergraben.
    shutil.copy2(REPO / "packaging/USER-GUIDE.md", DIST / "USER-GUIDE.md")
    # Lizenzen auch neben der AppImage (Review 2026-10-15, M8): wer nur das
    # Zip auspackt, sieht sie ohne die AppImage zu entpacken.
    for name, src in OWN_LICENSES.items():
        shutil.copy2(src, DIST / name)
    doc_qt = WORK / "AppDir/usr/share/doc/ampersand/qt"
    for name in ("LGPL-3.0-only.txt", "GPL-3.0-only.txt"):
        shutil.copy2(doc_qt / name, DIST / f"Qt-{name}")


def main() -> int:
    if not BIN.exists():
        raise SystemExit(f"GUI binary not found: {BIN}\nBuild first: cmake --build --preset default --target cortex-control")
    if not (QT_DIR / "bin/qmake6").exists():
        raise SystemExit(f"qmake6 not found under {QT_DIR}; set QT_DIR")
    WORK.mkdir(parents=True, exist_ok=True)
    restore_tiff_plugin()
    check_qt_intact()
    fetch_tools()
    appdir = build_appdir()
    appimage = run_linuxdeploy(appdir)
    assemble_dist(appimage)
    version = subprocess.run([str(appimage), "qcctl", "--version"], capture_output=True,
                             text=True, check=False).stdout.split()
    version = version[1] if len(version) > 1 else "0.1.0"
    archive = make_zip(version)
    size_mb = appimage.stat().st_size / (1024 * 1024)
    print(f"\nOK: {DIST/appimage.name} ({size_mb:.1f} MB)")
    print(f"    GUI, qcctl and qc-mcp are all in that one file.")
    print(f"    dist/ also holds the udev rule, install-udev.sh and README.md")
    print(f"    {archive} ({archive.stat().st_size / (1024*1024):.1f} MB)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
