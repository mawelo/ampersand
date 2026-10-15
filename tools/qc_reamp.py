#!/usr/bin/env python3
"""qc_reamp.py - ein Trockensignal vom Rechner durch das Geraet schicken.

Wer:      Martin.Lonkwitz
Was:      Legt den Eingang der gewaehlten Gitterzeilen auf einen USB-Eingang,
          spielt eine einkanalige Datei dorthin und nimmt gleichzeitig auf, was
          das Geraet daraus macht. Danach wird die Verdrahtung zurueckgestellt.
Wann:     2026-09-12
Warum:    Klangvergleiche brauchen Wiederholungen - Regler aendern, hoeren,
          erneut vergleichen. Wenn dafuer jedes Mal gespielt werden muss, ist
          jeder Durchgang eine andere Darbietung und die Messung wertlos. Mit
          dem Trockensignal aus docs/evidence/99 laesst sich beliebig oft
          derselbe Eingang durch das Geraet schicken.
Projekt:  Ampersand
Abhaengig: gebautes qcctl, arecord/aplay (ALSA), sox, Geraet per USB
Nutzung:  tools/qc_reamp.py trocken.wav --out ergebnis.wav --zeilen 1 2
Historie: git log

DIE PORTNUMMERN, am Geraet abgetastet (2026-09-12)

     1 Input 1      3 Input 1/2     7 Prev. Row    10 USB 7
     2 Input 2      4 Return 1      8 USB 5        11 USB 8
    19 Multi Out    5 Return 2      9 USB 6        12 USB 5/6
                    6 Return 1/2

VORSICHT: Waehrend des Laufs haengen die genannten Zeilen NICHT mehr an der
Gitarre. Das Werkzeug stellt sie danach zurueck; bei einem Abbruch mitten im
Lauf muss man es von Hand tun (`qcctl route <zeile> 1 19`).
"""
from __future__ import annotations

import argparse
import shutil
import subprocess
import sys
import time
from pathlib import Path

PORT_USB5 = 8
PORT_INPUT1 = 1
PORT_MULTI_OUT = 19
KANAL_ERGEBNIS = 3      # USB-Kanal 3 traegt das bearbeitete Signal
KANAL_WIEDERGABE = 5    # Wiedergabekanal 5 des Rechners = "USB 5" am Geraet


def lauf(cmd: list[str], timeout: int = 300) -> tuple[int, str]:
    try:
        p = subprocess.run(cmd, capture_output=True, text=True, timeout=timeout)
        return p.returncode, (p.stdout or "") + (p.stderr or "")
    except subprocess.TimeoutExpired:
        return 124, "timeout"


def dauer(datei: Path) -> float:
    rc, out = lauf(["soxi", "-D", str(datei)])
    return float(out.strip()) if rc == 0 else 0.0


def main() -> int:
    p = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    p.add_argument("trocken", type=Path)
    p.add_argument("--out", type=Path, required=True)
    p.add_argument("--zeilen", nargs="+", type=int, default=[1],
                   help="welche Gitterzeilen auf den USB-Eingang gelegt werden")
    p.add_argument("--qcctl", default="build/default/qcctl")
    p.add_argument("--card", default="Cortex")
    a = p.parse_args()

    for werkzeug in ("arecord", "aplay", "sox", "soxi"):
        if shutil.which(werkzeug) is None:
            print(f"fehlt: {werkzeug}", file=sys.stderr)
            return 2
    if not a.trocken.is_file():
        print(f"nicht gefunden: {a.trocken}", file=sys.stderr)
        return 2

    sek = dauer(a.trocken)
    print(f"  Eingang: {a.trocken.name}, {sek:.2f} s")

    # Achtkanaliges Wiedergabesignal bauen: nur Kanal 5 traegt etwas.
    acht = a.out.parent / (a.out.stem + "-wiedergabe-8-kanal.wav")
    a.out.parent.mkdir(parents=True, exist_ok=True)
    remix = ["remix"] + ["0"] * 8
    remix[KANAL_WIEDERGABE] = "1"          # remix[1] ist Kanal 1, also Index = Kanalnummer
    rc, out = lauf(["sox", str(a.trocken), "-b", "32", "-e", "signed-integer",
                    "-c", "8", str(acht)] + remix)
    if rc != 0:
        print(f"sox: {out}", file=sys.stderr)
        return 1

    for z in a.zeilen:
        lauf([a.qcctl, "route", str(z), str(PORT_USB5), str(PORT_MULTI_OUT)])
        time.sleep(2)
    rc, belegung = lauf([a.qcctl, "current"])
    for zeile in belegung.splitlines():
        if zeile.startswith("Row "):
            print(f"  {zeile}")

    mitschnitt = a.out.parent / (a.out.stem + "-8-kanal.wav")
    dauer_s = int(sek) + 2
    print(f"\n  Reamp laeuft, {dauer_s} s ...")
    rec = subprocess.Popen(["arecord", "-D", f"hw:{a.card}", "-f", "S32_LE", "-r", "48000",
                            "-c", "8", "-d", str(dauer_s), str(mitschnitt)],
                           stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    time.sleep(0.4)
    lauf(["aplay", "-D", f"hw:{a.card}", "-f", "S32_LE", "-r", "48000", "-c", "8", str(acht)],
         timeout=dauer_s + 60)
    rec.wait(timeout=dauer_s + 60)

    for z in a.zeilen:
        lauf([a.qcctl, "route", str(z), str(PORT_INPUT1), str(PORT_MULTI_OUT)])
        time.sleep(2)
    print("  Verdrahtung zurueckgestellt (Eingang 1).")

    if not mitschnitt.is_file():
        print("Aufnahme fehlgeschlagen", file=sys.stderr)
        return 1
    rc, out = lauf(["sox", str(mitschnitt), "-b", "24", str(a.out),
                    "remix", str(KANAL_ERGEBNIS)])
    if rc != 0:
        print(f"sox: {out}", file=sys.stderr)
        return 1
    mitschnitt.unlink()
    acht.unlink()

    rc, stats = lauf(["sox", str(a.out), "-n", "stats"])
    for z in stats.splitlines():
        if z.startswith(("Pk lev", "RMS lev")):
            print(f"  {z}")
    print(f"\n  {a.out}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
