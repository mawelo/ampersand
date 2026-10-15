#!/usr/bin/env python3
"""qc_probe_run.py - Sondenversuch am Geraet fahren: Aufbau, Aufnahme, Auswertung.

Wer:      Martin.Lonkwitz
Was:      Richtet das laufende Preset als Messstrecke ein (Zeile 3: Eingang 1
          durch einen Neural-Capture-Block auf Multi Out, Zeile 1 und 2
          stillgelegt), nimmt danach trockenes und bearbeitetes Signal
          gleichzeitig auf und stellt die Aufnahme per `qcctl capture-match`
          gegen alle Kandidaten. Zum Schluss wird der Messaufbau verworfen.
Wann:     2026-09-11
Warum:    Der Kennlinien-Lauf (docs/evidence/90) lief als lose Kette von
          Kommandos. Der zweite Durchgang - die Gewichtsanordnung mit der
          Sonde `layout` - faehrt dieselbe Strecke; ab da gehoert sie in ein
          Werkzeug, damit Dritte sie nachfahren koennen und die Rohdaten
          vollstaendig liegenbleiben.
Projekt:  Ampersand
Abhaengig: gebautes qcctl, arecord/aplay (ALSA), sox, Geraet per USB,
          Instrument an Eingang 1. Die Sonde muss in der Bibliothek des
          Geraets liegen (docs/evidence/75-probe-install).
Nutzung:  tools/qc_probe_run.py setup
          # am Geraet die Sonde im Capture-Block waehlen
          tools/qc_probe_run.py record --into docs/evidence/91-... \
              --serial QCXXXXXXXXX --cns <sonde>.cns
          tools/qc_probe_run.py restore --preset 1E
Historie: git log
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

# Modell-Kennung des Neural-Capture-Blocks aus dem Katalog des Geraets.
MODEL_NEURAL_CAPTURE = 14000

# Port-Nummern, wie `qcctl route` sie erwartet: 0 = leer, 1 = Input 1,
# 19 = Multi Out. Zeile 3 traegt die Messstrecke, weil die Zeilen 1 und 2
# stillgelegt werden und so nichts anderes mitklingt.
PORT_LEER = 0
PORT_INPUT1 = 1
PORT_MULTI_OUT = 19
MESSZEILE = 3
MESSSPALTE = 1

# Kanalbelegung des USB-Eingangs, gemessen in docs/evidence/85: Kanal 1/2
# fuehren immer das trockene Eingangssignal, Kanal 3/4 das bearbeitete. Die
# Sonde sitzt dazwischen, deshalb ist Kanal 1 die Referenz und Kanal 3 die
# Antwort des Geraets.
KANAL_DRY = 1
KANAL_WET = 3

SAY = Path.home() / ".local/bin/say"


def ansage(text: str, vorlauf: float = 0.0) -> None:
    """Sprachausgabe fuer den Spielenden; `vorlauf` wartet danach.

    Fuenf Sekunden sind das Mindeste vor einer Messung - so lange braucht man,
    um das Instrument in die Hand zu nehmen. Das Praefix macht hoerbar, dass
    die Entwicklungssitzung ruft und nicht irgendetwas anderes spricht."""
    if SAY.is_file():
        try:
            subprocess.run([str(SAY), f"QC-DEV: {text}"], capture_output=True, timeout=90)
        except subprocess.TimeoutExpired:
            pass
    if vorlauf > 0:
        time.sleep(vorlauf)


def lauf(cmd: list[str], timeout: int = 120) -> tuple[int, str]:
    """Fuehrt aus und gibt (Rueckgabewert, Ausgabe) zurueck - stdout und stderr zusammen."""
    try:
        p = subprocess.run(cmd, capture_output=True, text=True, timeout=timeout)
        return p.returncode, (p.stdout or "") + (p.stderr or "")
    except subprocess.TimeoutExpired:
        return 124, "timeout"
    except OSError as e:
        return 127, str(e)


def pegel(wav: Path, kanal: int) -> tuple[float, float]:
    """Spitzenwert und Effektivwert eines Kanals, beide in dBFS.

    Liest blockweise, damit auch eine halbe Minute Achtkanalaufnahme (rund
    46 MB) nicht als Ganzes im Speicher liegt."""
    with wave.open(str(wav), "rb") as w:
        kanaele = w.getnchannels()
        if w.getsampwidth() != 4:
            raise ValueError(f"{wav}: erwartet 32 Bit, gefunden {w.getsampwidth() * 8}")
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


def aufbau(a: argparse.Namespace) -> int:
    """Messstrecke im laufenden Preset einrichten. Nichts davon wird gespeichert -
    `restore` holt das Preset zurueck und verwirft damit alles."""
    schritte = [
        (["route", str(MESSZEILE), str(PORT_INPUT1), str(PORT_MULTI_OUT)],
         "Messzeile auf Input 1 / Multi Out"),
        (["block", str(MESSZEILE), str(MESSSPALTE), str(MODEL_NEURAL_CAPTURE)],
         "Neural-Capture-Block setzen"),
        (["route", "1", str(PORT_LEER), str(PORT_MULTI_OUT)], "Zeile 1 stilllegen"),
        (["route", "2", str(PORT_LEER), str(PORT_MULTI_OUT)], "Zeile 2 stilllegen"),
    ]
    for args, was in schritte:
        rc, out = lauf([a.qcctl, *args])
        letzte = out.strip().splitlines()[-1] if out.strip() else ""
        print(f"  {was}: rc={rc} {letzte}")
        # Der Block meldet beim Setzen mitunter einen Fehler, landet aber
        # trotzdem: das Geraet baut seine DSP-Kette neu auf und antwortet erst
        # danach (docs/evidence/79). Deshalb bricht ein Fehler hier nicht ab.
    print("\nJetzt am Geraet den Capture-Block antippen und die Sonde waehlen.")
    ansage("Messaufbau steht. Bitte am Geraet den Capture Block antippen und die Sonde waehlen.")
    return 0


def aufnahme(a: argparse.Namespace) -> int:
    for werkzeug in ("arecord", "aplay", "sox"):
        if shutil.which(werkzeug) is None:
            print(f"fehlt: {werkzeug}", file=sys.stderr)
            return 2
    if not Path(a.cns).is_file():
        print(f"Sondendatei fehlt: {a.cns}", file=sys.stderr)
        return 2

    roh = Path(a.into) / "rohdaten"
    roh.mkdir(parents=True, exist_ok=True)

    # Zustand vor der Aufnahme festhalten - er belegt, dass die Sonde im Block
    # sitzt und die Nachbarzeilen leer sind.
    rc, vorher = lauf([a.qcctl, "current"])
    (roh / "grid-vor-aufnahme.txt").write_text(vorher)
    print(f"  Grid festgehalten: rc={rc}")

    acht = roh / "aufnahme-8-kanal.wav"
    ansage(
        f"Aufnahme startet in fuenf Sekunden und laeuft {a.seconds} Sekunden. "
        "Bitte gehaltene Toene spielen und ausklingen lassen, damit die Kennlinie "
        "von laut bis leise durchlaufen wird.",
        vorlauf=5.0,
    )

    # Ohne gleichzeitige Wiedergabe liefert das Geraet digitale Stille: der
    # Aufnahme-Endpunkt ist zugleich der Taktgeber der Wiedergabe (Implicit
    # Feedback, docs/evidence/85). aplay auf /dev/zero haelt den Takt.
    spieler = subprocess.Popen(
        ["aplay", "-D", f"hw:{a.card}", "-f", "S32_LE", "-r", "48000", "-c", "8", "/dev/zero"],
        stdout=subprocess.DEVNULL,
        stderr=subprocess.DEVNULL,
    )
    time.sleep(1.0)
    rc, out = lauf(
        ["arecord", "-D", f"hw:{a.card}", "-f", "S32_LE", "-r", "48000", "-c", "8",
         "-d", str(a.seconds), str(acht)],
        timeout=a.seconds + 40,
    )
    spieler.terminate()
    spieler.wait(timeout=10)
    ansage("Stop. Danke, du kannst aufhoeren.")
    if rc != 0 or not acht.is_file():
        print(f"Aufnahme fehlgeschlagen: rc={rc}\n{out}", file=sys.stderr)
        return 1

    sp_d, rms_d = pegel(acht, KANAL_DRY)
    sp_w, rms_w = pegel(acht, KANAL_WET)
    print(f"  Kanal {KANAL_DRY} (trocken):  Spitze {sp_d:.1f} dBFS, RMS {rms_d:.1f} dBFS")
    print(f"  Kanal {KANAL_WET} (Sonde):    Spitze {sp_w:.1f} dBFS, RMS {rms_w:.1f} dBFS")
    if rms_d < -60.0:
        print("  Der trockene Kanal ist praktisch still - wurde gespielt?", file=sys.stderr)

    # capture-match erwartet zwei einkanalige Float-WAVs.
    dry = roh / "dry-kanal1.wav"
    wet = roh / "wet-durch-sonde-kanal3.wav"
    for quelle, kanal in ((dry, KANAL_DRY), (wet, KANAL_WET)):
        rc, out = lauf(["sox", str(acht), "-e", "float", "-b", "32", str(quelle),
                        "remix", str(kanal)], timeout=120)
        if rc != 0:
            print(f"sox fehlgeschlagen: {out}", file=sys.stderr)
            return 1

    rc, out = lauf([a.qcctl, "capture-match", a.serial, a.cns, str(dry), str(wet)], timeout=900)
    (roh / "capture-match.txt").write_text(out)
    print("\n" + out)
    return 0 if rc == 0 else 1


def zurueck(a: argparse.Namespace) -> int:
    """Messaufbau verwerfen: das Preset neu laden. Gespeichert wurde nichts,
    deshalb genuegt ein Recall."""
    rc, out = lauf([a.qcctl, "recall", a.preset])
    print(out.strip())
    return 0 if rc == 0 else 1


def main() -> int:
    p = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    p.add_argument("--qcctl", default="build/default/qcctl")
    p.add_argument("--card", default="Cortex")
    sub = p.add_subparsers(dest="phase", required=True)

    s = sub.add_parser("setup", help="Messstrecke im laufenden Preset einrichten")
    s.set_defaults(func=aufbau)

    r = sub.add_parser("record", help="aufnehmen, trennen und gegen alle Kandidaten rechnen")
    r.add_argument("--into", required=True, help="Belegordner; Rohdaten landen darin unter rohdaten/")
    r.add_argument("--serial", required=True, help="Seriennummer, mit der die Sonde verschluesselt ist")
    r.add_argument("--cns", required=True, help="Sondendatei (.cns), passend zur am Geraet gewaehlten")
    r.add_argument("--seconds", type=int, default=22)
    r.set_defaults(func=aufnahme)

    z = sub.add_parser("restore", help="Messaufbau verwerfen")
    z.add_argument("--preset", required=True, help="Slot oder Name, z. B. 1E")
    z.set_defaults(func=zurueck)

    a = p.parse_args()
    return a.func(a)


if __name__ == "__main__":
    sys.exit(main())
