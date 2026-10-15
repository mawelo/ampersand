#!/usr/bin/env python3
"""Unbenannte Blockparameter am Geraet benennen, indem man sie verstellt und
dem Display zusieht.

Wer     : Ampersand
Was     : Setzt jeden Parameter eines Blocks nacheinander auf einen deutlich
          anderen Wert, holt je ein Bildschirmfoto und vergleicht es mit dem
          Ausgangsbild. Das veraenderte Rechteck sagt, WELCHES Bedienelement
          zu dem Index gehoert; der Name steht dann im Bild daneben.
Wann    : 2026-09-13
Warum   : Der Katalog des Geraets nennt viele Parameter nur "param 17". Beim
          Cab-Block sind es 28 von 32 - und das ist die hoerbarste Luecke bei
          der Uebersetzung aus einem Plugin (docs/evidence/98). Raten verbietet
          sich; das Display weiss es, also fragt man das Display.
Projekt : Quad-Cortex / Ampersand
Haengt  : qcctl (screenshot, current --json, param) - RemoteControl aus
          CorOS 4.1.0; Pillow, numpy. Das Bildschirmfoto ist deterministisch
          (docs/evidence/95), taugt also als Messmittel.
Nutzung : tools/qc_param_kalibrieren.py --zeile 1 --spalte 2 \
              --into docs/evidence/101-cab-mikrofonierung/rohdaten/cab-tab
          Der zu untersuchende Block muss am Geraet GEOEFFNET sein (antippen);
          das Werkzeug oeffnet ihn nicht selbst, weil die Bildpunkte je Preset
          anders liegen.
Achtung : Das Werkzeug VERAENDERT das laufende Preset. Es stellt jeden Wert
          sofort zurueck, speichert nichts - aber es hinterlaesst `dirty`.
          Auf einem entbehrlichen Preset arbeiten und danach `qcctl recall`.
Falle   : GENAU DARAUS entsteht eine Fehlmessung. Der erste Schreibvorgang
          macht das Preset `dirty`, und die KOPFZEILE des Displays aendert sich
          dadurch. Ist das Ausgangsbild noch "sauber" aufgenommen, steckt diese
          Aenderung in JEDEM Vergleich - am 2026-09-16 waren das 383 Punkte im
          Streifen (19, 74, 592, 85) bei ALLEN dreizehn Indizes, auch bei denen,
          die in einem zweiten Lauf null Punkte ergaben (Beleg 115).
          Erkennungszeichen: derselbe Kasten bei allen Indizes. Abhilfe: einen
          Wegwerf-Schreibvorgang vor dem Ausgangsbild, oder den Lauf
          wiederholen, wenn das Preset schon `dirty` ist.
Historie: git
"""

from __future__ import annotations

import argparse
import json
import shutil
import subprocess
import sys
import time
from dataclasses import dataclass
from pathlib import Path

import numpy as np
from PIL import Image, ImageDraw

# Das Display des Quad Cortex. Feste Groesse, siehe docs/evidence/94.
BREITE, HOEHE = 800, 480

# Wieviele Bildpunkte sich mindestens unterscheiden muessen, damit wir von
# einer Aenderung sprechen. Zwei Aufnahmen desselben Zustands sind identisch
# (der Screenshot ist deterministisch), die Schwelle faengt nur Einzelpunkte
# aus Kantenglaettung ab.
MINDEST_PUNKTE = 12

# Der Bereich des Blocknamens. Dort steht ein Ebenen-Symbol, SOLANGE der Block
# genau auf seiner Voreinstellung sitzt; beim Verstellen verschwindet es und
# beim Zuruecknehmen kommt es wieder. Es zeigt also nicht den Parameter an,
# sondern nur, dass wir ueberhaupt etwas angefasst haben - und faelschte
# vorher jeden Vergleich um konstant 1096 Bildpunkte (docs/evidence/101).
# Ausblenden statt herausrechnen: eine Maske ist ehrlicher als ein Abzug.
BLOCKNAME = (55, 25, 205, 66)  # links, oben, rechts, unten


@dataclass(frozen=True)
class Parameter:
    index: int
    name: str
    wert: float
    anzeige: str


@dataclass(frozen=True)
class Befund:
    parameter: Parameter
    testwert: float
    kasten: tuple[int, int, int, int] | None  # links, oben, rechts, unten
    punkte: int


class Geraet:
    """Duenne Huelle um qcctl. Jeder Aufruf ist ein eigener Handschlag -
    das ist langsam, aber es haelt das Werkzeug einfach und zustandsfrei."""

    def __init__(self, qcctl: Path, zeile: int, spalte: int) -> None:
        self._qcctl = qcctl
        self._zeile = zeile
        self._spalte = spalte

    def _lauf(self, *args: str) -> subprocess.CompletedProcess[str]:
        return subprocess.run([str(self._qcctl), *args], capture_output=True, text=True, timeout=120)

    def parameter(self) -> list[Parameter]:
        p = self._lauf("current", "--json")
        if p.returncode != 0:
            raise RuntimeError(f"qcctl current: {p.stderr.strip() or p.stdout.strip()}")
        daten = json.loads(p.stdout)
        # `current --json` zaehlt Zeilen und Spalten 1-basiert, wie das Geraet
        # sie beschriftet und wie `qcctl param` sie erwartet (am 2026-09-13
        # gegengeprueft: `param 1 2 5 0.8` traf den Cab-Block).
        for reihe in daten.get("rows", []):
            if reihe.get("row") != self._zeile:
                continue
            for block in reihe.get("blocks", []):
                if block.get("column") != self._spalte:
                    continue
                # Genau die Parameter, die sich mit `param` setzen lassen: die
                # mit einem Zahlenwert. `type` ist bei den UNBENANNTEN leer -
                # gerade die wollen wir - also taugt er nicht als Filter; nur
                # `string` (Namenslisten wie der Mikrofontyp) faellt raus, den
                # setzt `param-text`.
                return [
                    Parameter(q["index"], q.get("name", ""), float(q["value"]), str(q.get("display", "")))
                    for q in block.get("params", [])
                    if q.get("value") is not None and q.get("type") != "string"
                ]
        raise RuntimeError(f"kein Block in Zeile {self._zeile}, Spalte {self._spalte}")

    def setze(self, index: int, wert: float) -> None:
        # Voll ausschreiben, nicht runden. Mit vier Nachkommastellen wurde aus
        # 0.5595231652 beim Zuruecksetzen 0.5595 - und das Geraet zeigte
        # fortan "-0.0 dB" statt "0.0 dB". Ein Zurueckstellen, das den Wert
        # veraendert, ist keins, und der Rest des Laufs vergleicht dann gegen
        # einen verschobenen Zustand (docs/evidence/101).
        p = self._lauf("param", str(self._zeile), str(self._spalte), str(index), repr(float(wert)))
        if p.returncode == 0 and "ok" in p.stdout:
            return
        # Schalter rasten: `param 31 0.85` kommt als 1.0 zurueck, und qcctl
        # meldet das zu Recht als Abweichung. Der Wert IST angekommen - fuer
        # unseren Zweck (sehen, was sich am Display ruehrt) genuegt das. Nur
        # ein echter Fehlschlag soll den Lauf abbrechen.
        meldung = (p.stderr + p.stdout).strip()
        if "after writing" in meldung:
            return
        raise RuntimeError(f"param {index} := {wert}: {meldung}")

    def bild(self, ziel: Path) -> Image.Image:
        p = self._lauf("screenshot", str(ziel))
        if p.returncode != 0 or not ziel.exists():
            raise RuntimeError(f"screenshot: {p.stderr.strip() or p.stdout.strip()}")
        return Image.open(ziel).convert("RGB")


def testwert(wert: float, vorgabe: float | None = None) -> float:
    """Ein Wert, der sich deutlich vom jetzigen unterscheidet - damit die
    Aenderung sichtbar wird und nicht im Rundungsrauschen verschwindet.

    Mit `vorgabe` laesst sich ein fester Wert erzwingen. Das braucht man fuer
    Parameter, die bei 0,85 stumm bleiben: manche Regler zeigen erst am Rand
    ihres Bereichs etwas (ein Filter meldet bis dahin schlicht "OFF")."""
    if vorgabe is not None:
        return vorgabe
    return 0.85 if wert <= 0.5 else 0.15


def unterschied(a: Image.Image, b: Image.Image) -> tuple[tuple[int, int, int, int] | None, int]:
    """Das umschliessende Rechteck aller Bildpunkte, die sich unterscheiden."""
    da = np.asarray(a, dtype=np.int16)
    db = np.asarray(b, dtype=np.int16)
    maske = np.abs(da - db).sum(axis=2) > 24
    li, ob, re, un = BLOCKNAME
    maske[ob:un, li:re] = False
    punkte = int(maske.sum())
    if punkte < MINDEST_PUNKTE:
        return None, punkte
    zeilen = np.flatnonzero(maske.any(axis=1))
    spalten = np.flatnonzero(maske.any(axis=0))
    return (int(spalten[0]), int(zeilen[0]), int(spalten[-1]) + 1, int(zeilen[-1]) + 1), punkte


def ausschnitt(bild: Image.Image, kasten: tuple[int, int, int, int], rand: int = 14) -> Image.Image:
    """Der veraenderte Bereich, grosszuegig geweitet - die Beschriftung steht
    ueber dem Wert und muss mit aufs Bild, sonst nuetzt der Ausschnitt nichts."""
    li, ob, re, un = kasten
    li = max(0, li - rand)
    ob = max(0, ob - 3 * rand)  # nach oben mehr: dort steht der Name
    re = min(BREITE, re + rand)
    un = min(HOEHE, un + rand)
    return bild.crop((li, ob, re, un))


def kontaktbogen(befunde: list[Befund], bilder: dict[int, Image.Image], ziel: Path) -> None:
    """Alle Ausschnitte untereinander auf EIN Blatt, jeder mit seinem Index.
    Ein Blatt statt dreissig Dateien - so laesst sich die Zuordnung in einem
    Blick lesen und als ein Beleg ablegen."""
    sichtbar = [b for b in befunde if b.kasten is not None]
    if not sichtbar:
        return
    zeilenhoehe = [bilder[b.parameter.index].height for b in sichtbar]
    breite = max(bilder[b.parameter.index].width for b in sichtbar) + 120
    blatt = Image.new("RGB", (breite, sum(zeilenhoehe) + 8 * len(sichtbar)), (16, 16, 16))
    stift = ImageDraw.Draw(blatt)
    y = 0
    for b in sichtbar:
        bild = bilder[b.parameter.index]
        blatt.paste(bild, (116, y))
        stift.text((8, y + bild.height // 2 - 4), f"idx {b.parameter.index:>2}", fill=(255, 210, 60))
        y += bild.height + 8
    blatt.save(ziel)


def main() -> int:
    t = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    t.add_argument("--zeile", type=int, required=True, help="Zeile des Blocks, 1-basiert wie am Geraet")
    t.add_argument("--spalte", type=int, required=True, help="Spalte des Blocks, 1-basiert wie am Geraet")
    t.add_argument("--into", type=Path, required=True, help="Ablage fuer Bilder und Ergebnis")
    t.add_argument("--qcctl", type=Path, default=Path("build/default/qcctl"))
    t.add_argument("--index", type=str, default="", help="nur diese Indizes, z. B. 4,5,17-22")
    t.add_argument("--ruhe", type=float, default=0.6, help="Wartezeit nach dem Setzen, bis das Display steht")
    t.add_argument("--wert", type=float, default=None, help="fester Testwert statt 0,85/0,15 - fuer Regler, die erst am Rand ihres Bereichs etwas zeigen")
    a = t.parse_args()

    if not a.qcctl.exists() and shutil.which(str(a.qcctl)) is None:
        print(f"qcctl nicht gefunden: {a.qcctl}", file=sys.stderr)
        return 2
    a.into.mkdir(parents=True, exist_ok=True)

    gewuenscht: set[int] | None = None
    if a.index:
        gewuenscht = set()
        for teil in a.index.split(","):
            if "-" in teil:
                von, bis = teil.split("-", 1)
                gewuenscht.update(range(int(von), int(bis) + 1))
            else:
                gewuenscht.add(int(teil))

    geraet = Geraet(a.qcctl, a.zeile, a.spalte)
    parameter = geraet.parameter()
    if gewuenscht is not None:
        parameter = [p for p in parameter if p.index in gewuenscht]
    print(f"{len(parameter)} Zahlenparameter in Zeile {a.zeile}, Spalte {a.spalte}")

    grund = geraet.bild(a.into / "00-ausgang.png")
    befunde: list[Befund] = []
    ausschnitte: dict[int, Image.Image] = {}

    for p in parameter:
        neu = testwert(p.wert, a.wert)
        try:
            try:
                geraet.setze(p.index, neu)
                time.sleep(a.ruhe)
                bild = geraet.bild(a.into / f"idx-{p.index:02d}.png")
            finally:
                geraet.setze(p.index, p.wert)  # immer zurueckstellen, auch nach Fehlern
                time.sleep(a.ruhe / 2)
        except (RuntimeError, subprocess.TimeoutExpired) as fehler:
            # Ein Parameter, der sich straeubt, kostet die anderen nichts. Der
            # Fehlschlag gehoert ins Ergebnis, nicht in einen Abbruch - sonst
            # sind zwanzig Minuten Messung wegen des letzten Index verloren.
            print(f"  idx {p.index:>2}: uebersprungen - {fehler}")
            befunde.append(Befund(p, neu, None, -1))
            continue
        kasten, punkte = unterschied(grund, bild)
        befunde.append(Befund(p, neu, kasten, punkte))
        if kasten is not None:
            ausschnitte[p.index] = ausschnitt(bild, kasten)
            print(f"  idx {p.index:>2} ({p.wert} -> {neu}): Kasten {kasten}, {punkte} Punkte")
        else:
            print(f"  idx {p.index:>2} ({p.wert} -> {neu}): keine sichtbare Aenderung ({punkte} Punkte)")

    kontaktbogen(befunde, ausschnitte, a.into / "kontaktbogen.png")
    ergebnis = {
        "zeile": a.zeile,
        "spalte": a.spalte,
        "befunde": [
            {
                "index": b.parameter.index,
                "name_laut_geraet": b.parameter.name,
                "wert_vorher": b.parameter.wert,
                "anzeige_vorher": b.parameter.anzeige,
                "testwert": b.testwert,
                "kasten": b.kasten,
                "punkte": b.punkte,
            }
            for b in befunde
        ],
    }
    (a.into / "ergebnis.json").write_text(json.dumps(ergebnis, indent=2, ensure_ascii=False), encoding="utf-8")
    stumm = [b.parameter.index for b in befunde if b.kasten is None and b.punkte >= 0]
    misslungen = [b.parameter.index for b in befunde if b.punkte < 0]
    wirksam = len(befunde) - len(stumm) - len(misslungen)
    print(f"\n{wirksam} von {len(befunde)} zeigen eine Wirkung.")
    if stumm:
        print(f"Ohne sichtbare Wirkung auf dieser Seite: {stumm}")
        print("Das heisst NICHT wirkungslos - es kann auf einem anderen Reiter liegen.")
    if misslungen:
        print(f"Nicht gemessen (Fehler beim Setzen): {misslungen}")
    print(f"Kontaktbogen: {a.into / 'kontaktbogen.png'}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
