#!/usr/bin/env python3
"""qc_record.py - trockenes und bearbeitetes Signal gleichzeitig mitschneiden.

Wer:      Martin.Lonkwitz
Was:      Nimmt den USB-Audiostrom des Quad Cortex auf und trennt ihn in das
          trockene Eingangssignal (Kanal 1) und das bearbeitete (Kanal 3).
          Nennt die Pegel und warnt, wenn etwas uebersteuert oder still ist.
Wann:     2026-09-12
Warum:    Fuer jeden Vergleich "klingt das Geraet wie die Vorlage?" braucht es
          BEIDE Seiten aus EINEM Durchgang: Das Trockensignal ist der Eingang,
          mit dem die Gegenseite gefuettert wird, das bearbeitete die Antwort
          des Geraets. Wer zweimal spielt, vergleicht Darbietungen statt
          Ketten.
Projekt:  Ampersand
Abhaengig: arecord/aplay (ALSA), sox, Geraet per USB
Nutzung:  tools/qc_record.py --into <verzeichnis> --seconds 30
Historie: git log

WARUM AUCH ABGESPIELT WIRD: Ohne gleichzeitige Wiedergabe liefert das Geraet
digitale Stille - der Aufnahme-Endpunkt ist zugleich der Taktgeber der
Wiedergabe (Implicit Feedback, docs/evidence/85). `aplay` auf /dev/zero haelt
den Takt.
"""
from __future__ import annotations

import argparse
import array
import math
import shutil
import subprocess
import sys
import time
import wave
from pathlib import Path

KANAL_DRY = 1
KANAL_WET = 3
SAY = Path.home() / ".local/bin/say"


def ansage(text: str, vorlauf: float = 0.0) -> None:
    if SAY.is_file():
        try:
            subprocess.run([str(SAY), f"QC-DEV: {text}"], capture_output=True, timeout=90)
        except subprocess.TimeoutExpired:
            pass
    if vorlauf > 0:
        time.sleep(vorlauf)


def pegel(wav: Path, kanal: int) -> tuple[float, float]:
    """Spitze und Effektivwert eines Kanals in dBFS, blockweise gelesen."""
    with wave.open(str(wav), "rb") as w:
        kanaele = w.getnchannels()
        if w.getsampwidth() != 4:
            raise ValueError(f"{wav}: erwartet 32 Bit")
        spitze = 0
        summe = 0.0
        anzahl = 0
        while True:
            roh = w.readframes(48000)
            if not roh:
                break
            werte = array.array("i")
            werte.frombytes(roh)
            for v in werte[kanal - 1 :: kanaele]:
                spitze = max(spitze, abs(v))
                summe += float(v) * float(v)
                anzahl += 1
    if anzahl == 0:
        return (-200.0, -200.0)
    voll = float(2**31)
    in_db = lambda x: 20.0 * math.log10(x) if x > 0 else -200.0
    return (in_db(spitze / voll), in_db(math.sqrt(summe / anzahl) / voll))


def main() -> int:
    p = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    p.add_argument("--into", type=Path, required=True)
    p.add_argument("--seconds", type=int, default=30)
    p.add_argument("--card", default="Cortex")
    p.add_argument("--name", default="lauf", help="Vorsilbe der Dateinamen")
    a = p.parse_args()

    for werkzeug in ("arecord", "aplay", "sox"):
        if shutil.which(werkzeug) is None:
            print(f"fehlt: {werkzeug}", file=sys.stderr)
            return 2
    a.into.mkdir(parents=True, exist_ok=True)
    acht = a.into / f"{a.name}-8-kanal.wav"

    ansage(f"Aufnahme startet in fuenf Sekunden und laeuft {a.seconds} Sekunden. "
           "Bitte durchgehend spielen.", vorlauf=5.0)

    spieler = subprocess.Popen(
        ["aplay", "-D", f"hw:{a.card}", "-f", "S32_LE", "-r", "48000", "-c", "8", "/dev/zero"],
        stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    time.sleep(1.0)
    p2 = subprocess.run(["arecord", "-D", f"hw:{a.card}", "-f", "S32_LE", "-r", "48000",
                         "-c", "8", "-d", str(a.seconds), str(acht)],
                        capture_output=True, text=True, timeout=a.seconds + 40)
    spieler.terminate()
    spieler.wait(timeout=10)
    ansage("Stop. Danke.")
    if p2.returncode != 0 or not acht.is_file():
        print(f"Aufnahme fehlgeschlagen:\n{p2.stderr}", file=sys.stderr)
        return 1

    dry = a.into / f"{a.name}-trocken.wav"
    wet = a.into / f"{a.name}-geraet.wav"
    for ziel, kanal in ((dry, KANAL_DRY), (wet, KANAL_WET)):
        # 24 Bit statt Float: Logic und jede DAW nehmen das ohne Murren,
        # und der Rauschabstand reicht bei Weitem.
        r = subprocess.run(["sox", str(acht), "-b", "24", str(ziel), "remix", str(kanal)],
                           capture_output=True, text=True, timeout=180)
        if r.returncode != 0:
            print(f"sox: {r.stderr}", file=sys.stderr)
            return 1

    print(f"\n  {'Datei':<34} {'Spitze':>9} {'RMS':>9}")
    for ziel, kanal, was in ((dry, KANAL_DRY, "trocken, Kanal 1"), (wet, KANAL_WET, "Geraet, Kanal 3")):
        sp, rms = pegel(acht, kanal)
        print(f"  {ziel.name:<34} {sp:>8.1f} {rms:>8.1f}   ({was})")
        if sp > -0.2:
            print("      ACHTUNG: an der Vollaussteuerung - der Vergleich wird unbrauchbar")
        elif rms < -60.0:
            print("      ACHTUNG: praktisch still - wurde gespielt?")
    acht.unlink()
    print(f"\n  Achtkanalfassung verworfen; nur Kanal 1 und 3 tragen Signal (docs/evidence/85).")
    return 0


if __name__ == "__main__":
    sys.exit(main())
