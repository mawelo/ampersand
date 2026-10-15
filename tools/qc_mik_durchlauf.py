#!/usr/bin/env python3
"""Die Mikrofonwahl eines Cab-Blocks messen statt annehmen.

Wer     : Ampersand
Was     : Steppt die Mikrofonliste eines Cab-Blocks durch, schickt bei jedem
          Schritt dasselbe Trockensignal per Reamp durch die Kette und stellt
          das Ergebnis gegen eine Vorlage. Sieger ist das Mikrofon mit der
          kleinsten mittleren BETRAGSabweichung der Terzbaender.
Wann    : 2026-09-13
Warum   : Beim Uebertragen einer Neural-Plugin-Spur nennt das Plugin sein
          Mikrofon nur als Listenplatz (`leftCab0MicType="4"`). Dass dieser
          Platz am Geraet dasselbe Mikrofon meint, war eine ANNAHME - und der
          Hoervergleich am 2026-09-13 hat sie erschuettert: die uebertragene
          Mikrofonierung lag WEITER von der Vorlage entfernt als der Stand
          davor (docs/evidence/102). Also wird sie jetzt gemessen.
Projekt : Quad-Cortex / Ampersand
Haengt  : qcctl (param-text, recall), tools/qc_reamp.py, tools/qc_compare.py.
          Das Geraet muss am USB haengen und Zeile 1+2 die zu pruefende Kette
          tragen.
Nutzung : tools/qc_mik_durchlauf.py \
              --preset 3H --seite links \
              --trocken docs/evidence/99-vergleich-logic-qc/gojira-clean-trocken.wav \
              --vorlage docs/evidence/99-vergleich-logic-qc/logic-bounce.aif \
              --into docs/evidence/103-mikrofon-durchlauf/links
Achtung : VERAENDERT das laufende Preset (Mikrofonwahl, waehrend des Reamps
          auch die Verdrahtung). Speichert NICHTS; am Ende wird das Preset neu
          geladen. Waehrend des Laufs liegt das Trockensignal auf dem Ausgang -
          wer Kopfhoerer traegt, hoert es bei jedem Schritt.
Historie: git
"""

from __future__ import annotations

import argparse
import json
import re
import subprocess
import sys
import time
from pathlib import Path

# Reihenfolge der Mikrofonliste, am Geraet abgelesen (docs/evidence/101):
# Selector sechsmal vorwaerts, jedes Mal den Namen aus dem UI-Baum geholt.
MIKROFONE = [
    "Dynamic 57",
    "Dynamic 421",
    "Condenser 414",
    "Condenser 184",
    "Ribbon 160",
    "Ribbon 121",
]

# Welcher Parameter welches Panel waehlt. Gemessen in docs/evidence/101.
SEITE_INDEX = {"links": 1, "rechts": 9}

# Die Baender, getrennt nach dem, was sie bedeuten. Der KERN entscheidet:
# dort wirken Amp, EQ und Mikrofonierung, und dort ist die Vorlage nicht
# durch Plugins verfaelscht, die es am Geraet gar nicht gibt.
BEREICHE = [
    ("Bass 40-63 Hz", 40, 63),
    ("Kern 80 Hz-8 kHz", 80, 8127),
    ("Hoehen 640 Hz-8 kHz", 640, 8127),
]
ENTSCHEIDET = "Kern 80 Hz-8 kHz"


def terzbaender(text: str) -> list[tuple[float, float]]:
    """(Frequenz, Differenz in dB) aus der Ausgabe von qc_compare.py."""
    aus: list[tuple[float, float]] = []
    for zeile in text.splitlines():
        m = re.match(r"\s*(\d+)\s+-?[\d.]+\s+-?[\d.]+\s+([+-][\d.]+)", zeile)
        if m:
            aus.append((float(m.group(1)), float(m.group(2))))
    return aus


def mittlere_abweichung(baender: list[tuple[float, float]], tief: float, hoch: float) -> float:
    """Mittlere BETRAGSabweichung im Band.

    Der Betrag ist nicht Geschmackssache: Ohne ihn heben sich +3 dB und -3 dB
    auf, und eine nachweislich falsche Kette kaeme rechnerisch auf null."""
    werte = [abs(d) for f, d in baender if tief <= f <= hoch]
    if not werte:
        raise ValueError(f"keine Terzbaender zwischen {tief} und {hoch} Hz")
    return sum(werte) / len(werte)


def alle_bereiche(baender: list[tuple[float, float]]) -> dict[str, float]:
    return {name: mittlere_abweichung(baender, t, h) for name, t, h in BEREICHE}


class Lauf:
    """Ein Durchgang: Mikrofon setzen, reampen, vergleichen."""

    def __init__(self, a: argparse.Namespace) -> None:
        self._a = a

    def _ruf(self, cmd: list[str], timeout: int = 300) -> str:
        p = subprocess.run(cmd, capture_output=True, text=True, timeout=timeout)
        if p.returncode != 0:
            raise RuntimeError(f"{cmd[0]} …: {(p.stderr or p.stdout).strip()[-400:]}")
        return p.stdout

    def mikrofon_setzen(self, name: str) -> None:
        idx = SEITE_INDEX[self._a.seite]
        self._ruf([self._a.qcctl, "param-text", str(self._a.zeile), str(self._a.spalte),
                   str(idx), self._a.vorsatz + name])

    def reampen(self, ziel: Path) -> None:
        self._ruf([sys.executable, "tools/qc_reamp.py", str(self._a.trocken),
                   "--out", str(ziel), "--zeilen", *[str(z) for z in self._a.zeilen],
                   "--qcctl", self._a.qcctl], timeout=300)

    def vergleichen(self, aufnahme: Path, bericht: Path) -> dict[str, float]:
        text = self._ruf([sys.executable, "tools/qc_compare.py", str(aufnahme),
                          str(self._a.vorlage)], timeout=600)
        bericht.write_text(text, encoding="utf-8")
        baender = terzbaender(text)
        if not baender:
            raise RuntimeError(f"qc_compare lieferte keine Terzbaender ({bericht})")
        return alle_bereiche(baender)


def main() -> int:
    t = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    t.add_argument("--seite", choices=sorted(SEITE_INDEX), required=True)
    t.add_argument("--trocken", type=Path, required=True)
    t.add_argument("--vorlage", type=Path, required=True)
    t.add_argument("--into", type=Path, required=True)
    t.add_argument("--preset", default="", help="Preset, das vor dem Lauf und am Ende geladen wird")
    t.add_argument("--zeile", type=int, default=1)
    t.add_argument("--spalte", type=int, default=2)
    t.add_argument("--zeilen", type=int, nargs="+", default=[1, 2],
                   help="Gitterzeilen, die der Reamp auf den USB-Eingang legt")
    t.add_argument("--vorsatz", default="NG_112 Gojira Cab 1_",
                   help="Vorsatz des IR-Namens; er nennt den Cab")
    t.add_argument("--nur", default="", help="nur diese Mikrofone, kommagetrennt")
    t.add_argument("--qcctl", default="qcctl")
    a = t.parse_args()

    a.into.mkdir(parents=True, exist_ok=True)
    lauf = Lauf(a)
    kandidaten = [m for m in MIKROFONE if not a.nur or m in a.nur.split(",")]

    if a.preset:
        lauf._ruf([a.qcctl, "recall", a.preset])
        time.sleep(2)

    ergebnis: list[dict] = []
    ablage = a.into / "ergebnis.json"
    print(f"{len(kandidaten)} Mikrofone, Seite {a.seite}, Vorlage {a.vorlage.name}\n")

    for i, name in enumerate(kandidaten, 1):
        sicher = name.replace(" ", "-")
        print(f"[{i}/{len(kandidaten)}] {name} …", flush=True)
        try:
            lauf.mikrofon_setzen(name)
            time.sleep(1)
            wav = a.into / f"{sicher}.wav"
            lauf.reampen(wav)
            werte = lauf.vergleichen(wav, a.into / f"vergleich-{sicher}.txt")
        except (RuntimeError, subprocess.TimeoutExpired) as fehler:
            # Ein Kandidat, der scheitert, kostet die anderen nichts - und
            # zwoelf Minuten Messung duerfen nicht an einem Namen haengen.
            print(f"      uebersprungen: {fehler}")
            continue
        ergebnis.append({"mikrofon": name, **werte})
        # Nach JEDEM Schritt schreiben: ein Abbruch soll nichts kosten.
        ablage.write_text(json.dumps(ergebnis, indent=2, ensure_ascii=False), encoding="utf-8")
        print("      " + "   ".join(f"{k}: {v:.2f}" for k, v in werte.items()))

    if a.preset:
        lauf._ruf([a.qcctl, "recall", a.preset])

    if not ergebnis:
        print("\nNichts gemessen.")
        return 1

    print(f"\n{'Mikrofon':<16}" + "".join(f"{n:>22}" for n, _, _ in BEREICHE))
    print("-" * (16 + 22 * len(BEREICHE)))
    for e in sorted(ergebnis, key=lambda x: x[ENTSCHEIDET]):
        print(f"{e['mikrofon']:<16}" + "".join(f"{e[n]:>22.2f}" for n, _, _ in BEREICHE))
    bester = min(ergebnis, key=lambda x: x[ENTSCHEIDET])
    print(f"\nKleinste Abweichung im entscheidenden Band ({ENTSCHEIDET}): "
          f"**{bester['mikrofon']}** mit {bester[ENTSCHEIDET]:.2f} dB.")
    print("Das Preset wurde neu geladen - nichts gespeichert.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
