#!/usr/bin/env python3
"""qc_latency_probe.py - misst, wie lange das Geraet auf Anfragen antwortet.

Wer:      Martin.Lonkwitz
Was:      Fuehrt qcctl mehrfach mit --verbose aus, hebt jeden Mitschnitt roh
          auf und wertet die Zeitstempel aus: wann ging die Anfrage hinaus,
          wann kam die Antwort, wie lange lief dazwischen der Seed-Push des
          Geraets.
Wann:     2026-09-10
Warum:    `qcctl info` meldete `position: ?`, weil das Geraet erst 12,3 s
          spaeter antwortete - nach seinem Seed-Push (evidence/77). Bevor
          Wartezeiten angehoben oder das Warten umgebaut wird, soll belegt
          sein, wie stark diese Verzoegerung schwankt. Eine einzelne Messung
          traegt eine solche Entscheidung nicht.
Projekt:  Ampersand (SUB-PROJECT FROM: Quad-Cortex)
Abhaengig: python3, ein gebautes qcctl, Geraet per USB angeschlossen.
          Rein lesend - es werden nur READ-Kommandos ausgefuehrt.
Nutzung:  qc_latency_probe.py --runs 5 --into <ablage>
          qc_latency_probe.py --runs 3 --commands info current
          Ergebnis: je Lauf ein roher .log, dazu messwerte.json und eine
          Tabelle auf der Standardausgabe (Median und Spanne).
Historie: git log
"""
from __future__ import annotations

import argparse
import json
import re
import statistics
import subprocess
import sys
from datetime import datetime, timedelta
from pathlib import Path

# "12:25:39.339 [debug] tx SetlistPosition (2 bytes): 08 03"
ZEILE = re.compile(r"^(?P<zeit>\d\d:\d\d:\d\d\.\d\d\d) \[\w+\] (?P<richtung>tx|rx) (?P<typ>\w+) \((?P<groesse>\d+) ")

# Welchen Nachrichtentyp misst welches Kommando?
GEMESSEN = {
    "info": "SetlistPosition",
    "current": "RecallPreset",
    "coros": "Version",
    "version": "Version",
}

# Eine Version-Antwort ist erst ab dieser Groesse die vollstaendige; kleinere
# sind Teilantworten ohne app_fw_version (siehe evidence/76).
VERSION_VOLLSTAENDIG_AB = 100


def uhr(text: str) -> datetime:
    """Zeitstempel einer Logzeile. Das Datum ist beliebig, nur Differenzen zaehlen."""
    return datetime.strptime(text, "%H:%M:%S.%f")


def sekunden(von: datetime | None, bis: datetime | None) -> float | None:
    if von is None or bis is None:
        return None
    d: timedelta = bis - von
    return round(d.total_seconds(), 3)


def auswerten(log: str, typ: str) -> dict:
    """Ein Mitschnitt -> Messwerte in Sekunden.

    Gemessen wird die ERSTE Anfrage des gesuchten Typs und die erste Antwort
    danach. Zusaetzlich der Seed-Push (die File-Flut), weil die Verzoegerung
    genau daran haengt.
    """
    beginn = anfrage = antwort = erste_datei = letzte_datei = None
    dateien = 0

    for zeile in log.splitlines():
        m = ZEILE.match(zeile)
        if not m:
            continue
        t, richtung, nachricht = uhr(m["zeit"]), m["richtung"], m["typ"]
        if beginn is None:
            beginn = t
        if nachricht == "File" and richtung == "rx":
            dateien += 1
            erste_datei = erste_datei or t
            letzte_datei = t
        if nachricht != typ:
            continue
        if richtung == "tx" and anfrage is None:
            anfrage = t
        elif richtung == "rx" and anfrage is not None and antwort is None:
            if typ != "Version" or int(m["groesse"]) > VERSION_VOLLSTAENDIG_AB:
                antwort = t

    return {
        "anfrage_nach_verbindung_s": sekunden(beginn, anfrage),
        "antwort_nach_anfrage_s": sekunden(anfrage, antwort),
        "seed_dauer_s": sekunden(erste_datei, letzte_datei),
        "antwort_nach_seed_s": sekunden(letzte_datei, antwort),
        "file_nachrichten": dateien,
        "antwort_gesehen": antwort is not None,
    }


def spalte(werte: list[float | None]) -> str:
    """Median und Spanne einer Messreihe; '-' wenn nichts messbar war."""
    da = [w for w in werte if w is not None]
    if not da:
        return "-"
    if len(da) == 1:
        return f"{da[0]:.2f}"
    return f"{statistics.median(da):.2f} ({min(da):.2f}-{max(da):.2f})"


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--qcctl", default="./build/default/qcctl")
    ap.add_argument("--runs", type=int, default=5)
    ap.add_argument("--commands", nargs="+", default=["info", "current"])
    ap.add_argument("--into", default=None, help="Ablage der Rohmitschnitte")
    a = ap.parse_args()

    qcctl = Path(a.qcctl).expanduser()
    if not qcctl.is_file():
        print(f"qcctl nicht gefunden: {qcctl}", file=sys.stderr)
        return 2
    unbekannt = [c for c in a.commands if c not in GEMESSEN]
    if unbekannt:
        print(f"kein Messpunkt hinterlegt fuer: {', '.join(unbekannt)}", file=sys.stderr)
        return 2

    ablage = Path(a.into).expanduser() if a.into else None
    if ablage:
        ablage.mkdir(parents=True, exist_ok=True)

    alles: dict[str, list[dict]] = {}
    for kommando in a.commands:
        reihe: list[dict] = []
        for i in range(1, a.runs + 1):
            print(f"  {kommando} Lauf {i}/{a.runs} ...", flush=True)
            r = subprocess.run([str(qcctl), "--verbose", kommando],
                               capture_output=True, text=True, timeout=180)
            log = r.stdout + r.stderr
            if ablage:
                (ablage / f"{kommando}-{i:02d}.log").write_text(log)
            werte = auswerten(log, GEMESSEN[kommando]) | {"lauf": i}
            reihe.append(werte)
        alles[kommando] = reihe

    if ablage:
        (ablage / "messwerte.json").write_text(json.dumps(alles, indent=1))

    print()
    kopf = (f"{'Kommando':10} {'gemessen':16} {'Antwort nach Anfrage':22} "
            f"{'nach Seed-Ende':18} {'Seed-Dauer':18} {'Dateien':16} gesehen")
    print(kopf)
    print("-" * len(kopf))
    for kommando, reihe in alles.items():
        print(f"{kommando:10} {GEMESSEN[kommando]:16} "
              f"{spalte([w['antwort_nach_anfrage_s'] for w in reihe]):22} "
              f"{spalte([w['antwort_nach_seed_s'] for w in reihe]):18} "
              f"{spalte([w['seed_dauer_s'] for w in reihe]):18} "
              f"{spalte([float(w['file_nachrichten']) for w in reihe]):16} "
              f"{sum(w['antwort_gesehen'] for w in reihe)}/{len(reihe)}")
    print("\nAngaben in Sekunden: Median (Spanne).")
    return 0


if __name__ == "__main__":
    sys.exit(main())
