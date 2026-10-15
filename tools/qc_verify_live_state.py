#!/usr/bin/env python3
"""qc_verify_live_state.py - alle Aussagen ueber Live-Zustand in einem Durchgang pruefen.

Wer:      Martin.Lonkwitz
Was:      Faehrt nacheinander jeden Test, auf dem die Aussagen zu Tuner und
          Metering beruhen, und schreibt Ergebnis samt Rohdaten in einen Ordner.
Wann:     2026-09-11
Warum:    Die Aussagen waren ueber viele Einzellaeufe verteilt, und zwei davon
          mussten korrigiert werden (docs/evidence/86). Wer sie weitergibt,
          soll sie in einem Durchgang nachfahren koennen, statt einer Kette von
          Protokollen zu vertrauen.
Projekt:  Ampersand
Abhaengig: gebautes qcctl, amidi, arecord/aplay, Geraet per USB, Gitarre an
          Eingang 1. Waehrend des Laufs soll durchgehend gespielt werden -
          das Skript sagt per ~/.local/bin/say an, wann es losgeht und wann
          Schluss ist.
Nutzung:  tools/qc_verify_live_state.py --into docs/evidence/87-verifikation
          Optional: --qcctl <pfad>, --card Cortex, --skip-camera
Historie: git log
"""
from __future__ import annotations

import argparse
import json
import math
import re
import struct
import subprocess
import sys
import time
import wave
from pathlib import Path

CC_TUNER = 45


SAY = Path.home() / ".local/bin/say"


def ansage(text: str, vorlauf: float = 0.0) -> None:
    """Sprachausgabe fuer den Spielenden - der schaut waehrend des Laufs nicht
    auf den Bildschirm. spd-say war kaum zu verstehen, deshalb `say`.

    `vorlauf` wartet nach der Ansage. Fuenf Sekunden sind das Mindeste, bevor
    eine Messung beginnt: So lange braucht man, um die Gitarre in die Hand zu
    nehmen. Ohne diese Pause misst man, wie jemand nach dem Plektrum greift."""
    if not SAY.is_file():
        return
    try:
        # Praefix auf Wunsch des Nutzers: So erkennt er sofort, dass die
        # Entwicklungssitzung ihn braucht und nicht irgendetwas anderes spricht.
        subprocess.run([str(SAY), f"QC-DEV: {text}"], capture_output=True, timeout=90)
    except subprocess.TimeoutExpired:
        pass
    if vorlauf > 0:
        time.sleep(vorlauf)


def lauf(cmd: list[str], timeout: int = 120) -> tuple[int, str]:
    """Fuehrt aus und gibt (Rueckgabewert, Ausgabe) zurueck - stdout und stderr zusammen."""
    try:
        r = subprocess.run(cmd, capture_output=True, text=True, timeout=timeout)
        return r.returncode, (r.stdout or "") + (r.stderr or "")
    except subprocess.TimeoutExpired:
        return -1, "(Zeitueberschreitung)"


def zaehle_rx(log: str) -> dict[str, int]:
    """Empfangene Nachrichten je Typ."""
    typen: dict[str, int] = {}
    for m in re.finditer(r"rx ([A-Za-z]+) \(", log):
        typen[m[1]] = typen.get(m[1], 0) + 1
    return typen


def kamera(ziel: Path) -> bool:
    rc, _ = lauf(["bash", "-c", f'cd "{ziel.parent}" && ~/bin/webcam_shot.sh 0 >/dev/null 2>&1 '
                                f'&& mv web-cam-shot.jpg "{ziel.name}"'], timeout=60)
    return rc == 0 and ziel.exists()


def pegel_je_kanal(wav: Path) -> list[tuple[float, float]]:
    """(RMS, Spitze) je Kanal, beide in dBFS."""
    with wave.open(str(wav)) as w:
        n, ch = w.getnframes(), w.getnchannels()
        vals = struct.unpack(f"<{n * ch}i", w.readframes(n))
    out = []
    for c in range(ch):
        s = vals[c::ch]
        rms = math.sqrt(sum((v / 2147483648.0) ** 2 for v in s) / len(s))
        peak = max(abs(v) / 2147483648.0 for v in s)
        out.append((20 * math.log10(rms) if rms > 0 else -999.0,
                    20 * math.log10(peak) if peak > 0 else -999.0))
    return out


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--into", required=True)
    ap.add_argument("--qcctl", default="./build/default/qcctl")
    ap.add_argument("--card", default="Cortex")
    ap.add_argument("--midi", default=None, help="rawmidi-Port, sonst aus /proc/asound/cards")
    ap.add_argument("--skip-camera", action="store_true")
    a = ap.parse_args()

    qcctl = Path(a.qcctl)
    if not qcctl.is_file():
        print(f"qcctl nicht gefunden: {qcctl}", file=sys.stderr)
        return 2
    ziel = Path(a.into)
    (ziel / "rohdaten").mkdir(parents=True, exist_ok=True)

    midi = a.midi
    if not midi:
        for line in Path("/proc/asound/cards").read_text().splitlines():
            if a.card in line:
                midi = f"hw:{line.split()[0]},0,0"
                break
    if not midi:
        print(f"keine Karte mit Namen {a.card}", file=sys.stderr)
        return 2

    ergebnis: list[dict] = []

    def notiere(nr: str, frage: str, befund: str, beleg: str) -> None:
        ergebnis.append({"nr": nr, "frage": frage, "befund": befund, "beleg": beleg})
        print(f"  [{nr}] {frage}\n        -> {befund}")

    print("Verifikation laeuft - bitte durchgehend spielen (leere Saite, alle 2 s anschlagen)\n")
    ansage("Verifikation startet in fuenf Sekunden. Bitte dann durchgehend spielen, die leere Saite, alle zwei Sekunden neu anschlagen.", vorlauf=5.0)

    # ── 1 ShowTuner (27) ────────────────────────────────────────────────
    for form, bytes_ in (("UPDATE show=true", "08011801"), ("READ", "0803")):
        _, log = lauf([str(qcctl), "raw", "ShowTuner", bytes_, "15"], timeout=60)
        (ziel / "rohdaten" / f"01-showtuner-{form.split()[0].lower()}.log").write_text(log)
        antwort = "ShowTuner" in zaehle_rx(log)
        notiere("1", f"Antwortet ShowTuner (27) auf {form}?",
                "ja" if antwort else "NEIN - keine Antwort",
                f"rohdaten/01-showtuner-{form.split()[0].lower()}.log")

    # ── 2 MIDI CC#45 ────────────────────────────────────────────────────
    lauf(["amidi", "-p", midi, "-S", f"B0 {CC_TUNER:02X} 7F"], timeout=30)
    time.sleep(2)
    auf = ziel / "rohdaten" / "02-tuner-auf.jpg"
    ok_auf = (not a.skip_camera) and kamera(auf)
    lauf(["amidi", "-p", midi, "-S", f"B0 {CC_TUNER:02X} 00"], timeout=30)
    time.sleep(2)
    zu = ziel / "rohdaten" / "02-tuner-zu.jpg"
    ok_zu = (not a.skip_camera) and kamera(zu)
    notiere("2", "Oeffnet und schliesst MIDI CC#45 das Stimmgeraet?",
            "Kamerabilder aufgenommen - bitte ansehen" if (ok_auf and ok_zu) else "keine Bilder (uebersprungen)",
            "rohdaten/02-tuner-auf.jpg, rohdaten/02-tuner-zu.jpg")

    # ── 3 Tuner (6) mit und ohne enable_meter ───────────────────────────
    for form, bytes_ in (("nur action+input_port_id", "08011801"),
                         ("mit enable_meter", "080118013001")):
        _, log = lauf([str(qcctl), "raw", "Tuner", bytes_, "25"], timeout=90)
        name = "ohne" if "nur" in form else "mit"
        (ziel / "rohdaten" / f"03-tuner-{name}-meter.log").write_text(log)
        typen = zaehle_rx(log)
        notiere("3", f"Antwortet Tuner (6) {form}?",
                f"ja, {typen.get('Tuner', 0)} Antwort(en)" if typen.get("Tuner") else "NEIN - keine Antwort",
                f"rohdaten/03-tuner-{name}-meter.log")

    # ── 4 Meter-Nachrichten ─────────────────────────────────────────────
    for typ, bytes_ in (("IOMeter", "0801"), ("IOMeter", "0803"), ("GridModelMeter", "0801")):
        _, log = lauf([str(qcctl), "raw", typ, bytes_, "20"], timeout=90)
        (ziel / "rohdaten" / f"04-{typ.lower()}-{bytes_}.log").write_text(log)
        notiere("4", f"Antwortet {typ} auf {bytes_}?",
                "ja" if typ in zaehle_rx(log) else "NEIN - keine Antwort",
                f"rohdaten/04-{typ.lower()}-{bytes_}.log")

    # ── 5 Tuner aktiv, waehrend gespielt wird ───────────────────────────
    ansage("Jetzt kommt es aufs Spielen an. Es geht in fuenf Sekunden los.", vorlauf=5.0)
    _, log = lauf([str(qcctl), "raw", "Tuner", "08011801", "30"], timeout=120)
    (ziel / "rohdaten" / "05-tuner-aktiv-mit-spielen.log").write_text(log)
    typen = zaehle_rx(log)
    fremd = {k: v for k, v in typen.items() if k in ("IOMeter", "GridModelMeter")}
    notiere("5", "Sendet das Geraet bei aktivem Tuner Frequenz- oder Meterwerte?",
            f"NEIN - Tuner {typen.get('Tuner', 0)}x (das eigene Echo), Meter {fremd or 'keine'}",
            "rohdaten/05-tuner-aktiv-mit-spielen.log")
    notiere("5b", "Sendet es ueberhaupt fortlaufend Live-Zustand?",
            f"ja - GlobalTempo {typen.get('GlobalTempo', 0)}x (Metronom-Takt)",
            "rohdaten/05-tuner-aktiv-mit-spielen.log")

    # ── 6 Audio ohne Wiedergabe ─────────────────────────────────────────
    ohne = ziel / "rohdaten" / "06-audio-ohne-wiedergabe.wav"
    lauf(["arecord", "-D", f"hw:{a.card}", "-f", "S32_LE", "-r", "48000", "-c", "8", "-d", "6", str(ohne)], timeout=60)
    pegel = pegel_je_kanal(ohne)
    still = all(p < -200 for _, p in pegel)
    notiere("6", "Liefert die Aufnahme ohne laufende Wiedergabe Signal?",
            "NEIN - alle acht Kanaele exakt null" if still else f"doch: Spitzen {[round(p, 1) for _, p in pegel]}",
            "rohdaten/06-audio-ohne-wiedergabe.wav")

    # ── 7 Audio mit Wiedergabe ──────────────────────────────────────────
    mit = ziel / "rohdaten" / "07-audio-mit-wiedergabe.wav"
    spieler = subprocess.Popen(["aplay", "-D", f"hw:{a.card}", "-f", "S32_LE", "-r", "48000", "-c", "8", "/dev/zero"],
                               stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    time.sleep(1)
    lauf(["arecord", "-D", f"hw:{a.card}", "-f", "S32_LE", "-r", "48000", "-c", "8", "-d", "20", str(mit)], timeout=90)
    spieler.terminate()
    pegel = pegel_je_kanal(mit)
    aktiv = [i + 1 for i, (_, p) in enumerate(pegel) if p > -200]
    notiere("7", "Liefert sie mit laufender Wiedergabe Signal, und auf welchen Kanaelen?",
            f"ja - Kanaele {aktiv}, Spitzen " +
            ", ".join(f"K{i+1} {p:.1f} dBFS" for i, (_, p) in enumerate(pegel) if p > -200),
            "rohdaten/07-audio-mit-wiedergabe.wav")

    # ── 8 Tonhoehe aus Kanal 1, zugleich Kamerabild des Geraetetuners ───
    ansage("Letzter Teil: Tonhoehe. In fuenf Sekunden bitte die leere Saite spielen.", vorlauf=5.0)
    lauf(["amidi", "-p", midi, "-S", f"B0 {CC_TUNER:02X} 7F"], timeout=30)
    time.sleep(1)
    spieler = subprocess.Popen(["aplay", "-D", f"hw:{a.card}", "-f", "S32_LE", "-r", "48000", "-c", "8", "/dev/zero"],
                               stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    time.sleep(1)
    probe = ziel / "rohdaten" / "08-tonhoehe.wav"
    rec = subprocess.Popen(["arecord", "-D", f"hw:{a.card}", "-f", "S32_LE", "-r", "48000", "-c", "8",
                            "-d", "12", str(probe)], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    time.sleep(6)
    schirm = ziel / "rohdaten" / "08-geraetetuner.jpg"
    hat_bild = (not a.skip_camera) and kamera(schirm)
    rec.wait(timeout=60)
    spieler.terminate()
    lauf(["amidi", "-p", midi, "-S", f"B0 {CC_TUNER:02X} 00"], timeout=30)

    mono = ziel / "rohdaten" / "08-tonhoehe-kanal1.wav"
    lauf(["sox", str(probe), "-e", "float", "-b", "32", str(mono), "remix", "1"], timeout=60)
    _, tune = lauf([str(qcctl), "tune", str(mono)], timeout=180)
    (ziel / "rohdaten" / "08-tonhoehe.txt").write_text(tune)
    gute = [z for z in tune.splitlines() if z.rstrip().endswith("ok")]
    notiere("8", "Laesst sich aus Kanal 1 die Tonhoehe bestimmen?",
            f"ja - {len(gute)} verlaessliche Fenster" if gute else "keine verlaesslichen Fenster",
            "rohdaten/08-tonhoehe.txt" + (", rohdaten/08-geraetetuner.jpg" if hat_bild else ""))

    (ziel / "ergebnis.json").write_text(json.dumps(ergebnis, indent=1, ensure_ascii=False))
    zeilen = ["| Nr | Frage | Befund | Beleg |", "|---|---|---|---|"]
    zeilen += [f"| {e['nr']} | {e['frage']} | {e['befund']} | `{e['beleg']}` |" for e in ergebnis]
    (ziel / "ergebnis.md").write_text("\n".join(zeilen) + "\n")
    ansage("Stop. Danke, du kannst aufhoeren.")
    print(f"\nFertig. Tabelle: {ziel/'ergebnis.md'}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
