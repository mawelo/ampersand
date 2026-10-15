#!/usr/bin/env python3
"""qc_block_params.py - die Parameterliste von Blockmodellen am Geraet abfragen.

Wer:      Martin.Lonkwitz
Was:      Setzt ein Modell nacheinander in eine freie Gitterzelle, liest die
          Parameternamen samt Anzeigewerten aus `qcctl current` und raeumt die
          Zelle wieder ab. Ergebnis ist eine Tabelle Modell -> Parameter.
Wann:     2026-09-12
Warum:    Der Katalog des Geraets nennt nur Modellnamen, nicht ihre Parameter.
          Fuer die Uebersetzung einer Logic-Spur in eine Block-Kette
          (docs/evidence/97, 98) braucht es aber genau diese Namen - sonst
          raet man, welcher Regler welcher ist.
Projekt:  Ampersand
Abhaengig: gebautes qcctl, Geraet per USB
Nutzung:  tools/qc_block_params.py 1164 12052 4006 --into docs/evidence/98-.../
          Optional: --zelle 4 1 (Vorgabe), --qcctl <pfad>
Historie: git log

VORSICHT: Das Werkzeug VERAENDERT das laufende Preset - es setzt und entfernt
Bloecke. Es speichert nichts; ein `qcctl recall <slot>` stellt den Ausgangs-
zustand wieder her. Die Zelle sollte in einer Zeile ohne Eingang liegen, dann
ist der Vorgang auch lautlos.
"""
from __future__ import annotations

import argparse
import json
import re
import subprocess
import sys
from pathlib import Path


def lauf(cmd: list[str], timeout: int = 120) -> tuple[int, str]:
    try:
        p = subprocess.run(cmd, capture_output=True, text=True, timeout=timeout)
        return p.returncode, (p.stdout or "") + (p.stderr or "")
    except subprocess.TimeoutExpired:
        return 124, "timeout"


def zelle_lesen(qcctl: str, zeile: int, spalte: int) -> tuple[str, list[tuple[int, str, str]]]:
    """Name des Blocks und seine Parameter aus `qcctl current`."""
    _, out = lauf([qcctl, "current"])
    im_block = False
    name = ""
    params: list[tuple[int, str, str]] = []
    for z in out.splitlines():
        m = re.match(rf"^Row {zeile}: ", z)
        if m:
            im_block = "suchen"
            continue
        if im_block == "suchen":
            m = re.match(rf"^  \[{spalte}\] (.+?) \(", z)
            if m:
                name = m.group(1)
                im_block = True
                continue
            if re.match(r"^Row \d", z):
                break
            continue
        if im_block is True:
            # Die Einrueckung schrumpft, sobald der Index zweistellig wird -
            # eine feste Zahl Leerzeichen bricht die Liste bei 10 ab.
            m = re.match(r"^\s{5,9}(\d+) (.{1,22}?)\s{2,}(.*)$", z)
            if m:
                params.append((int(m.group(1)), m.group(2).strip(), m.group(3).strip()))
                continue
            break
    return name, params


def main() -> int:
    p = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    p.add_argument("modelle", nargs="+", type=int)
    p.add_argument("--qcctl", default="build/default/qcctl")
    p.add_argument("--zelle", nargs=2, type=int, default=[4, 1], metavar=("ZEILE", "SPALTE"))
    p.add_argument("--into", type=Path)
    a = p.parse_args()
    zeile, spalte = a.zelle

    ergebnis: dict[str, dict] = {}
    for modell in a.modelle:
        lauf([a.qcctl, "remove", str(zeile), str(spalte)])
        rc, out = lauf([a.qcctl, "block", str(zeile), str(spalte), str(modell)])
        name, params = zelle_lesen(a.qcctl, zeile, spalte)
        if not name:
            print(f"  {modell:>6}  -- konnte nicht gelesen werden (rc={rc})", file=sys.stderr)
            continue
        ergebnis[str(modell)] = {"name": name, "params": [
            {"idx": i, "name": n, "wert": w} for i, n, w in params]}
        print(f"  {modell:>6}  {name}")
        for i, n, w in params:
            print(f"          {i:>2}  {n:<22} {w}")
    lauf([a.qcctl, "remove", str(zeile), str(spalte)])

    if a.into:
        a.into.mkdir(parents=True, exist_ok=True)
        ziel = a.into / "blockparameter.json"
        ziel.write_text(json.dumps(ergebnis, indent=2, ensure_ascii=False))
        print(f"\n{ziel}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
