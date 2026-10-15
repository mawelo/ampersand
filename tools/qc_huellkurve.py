#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""Vergleicht die Kurzzeit-Huellkurven zweier Aufnahmen.

WER    Projekt Ampersand (Quad-Cortex).
WAS    Rechnet fuer zwei Aufnahmen den Kurzzeit-RMS ueber gleich lange Fenster
       und gibt aus, wie weit sie auseinanderliegen: Mittel, Streuung, Spanne
       und Korrelation, getrennt nach einem waehlbaren Zeitschnitt (gespielter
       Teil gegen Ausklang). Gleicht vorher Zeitversatz und Pegel aus.
WANN   2026-09-15.
WARUM  Der Nutzer sah in Audacity unterschiedliche Huellkurven zweier Presets
       und fragte, ob das die Presets sind. Ohne Vergleichsmassstab ist die
       Frage nicht zu beantworten: Delay (TAPE MOD) und Hall laufen bei jedem
       Durchgang anders (Beleg 103). Dieses Werkzeug misst denselben Wert
       einmal fuer zwei verschiedene Presets und einmal fuer zwei Laeufe
       DERSELBEN Einstellung - die Differenz der beiden Zahlen ist die
       Antwort. Belegordner: docs/evidence/109-hoerurteil-3f-3h/.
PROJEKT Ampersand, Teil der Beleg-Werkzeuge.
ABHAENGIGKEITEN
       Python >= 3.11, numpy, sox (ueber tools/qc_compare.als_mono).
       Liest ausschliesslich; fasst kein Geraet an.
NUTZUNG
       tools/qc_huellkurve.py A.wav B.wav [--fenster-ms 50] [--schnitt-s 28]
HISTORIE
       Siehe git log dieser Datei.

KORREKTHEIT: Verglichen wird erst nach Zeitausgleich (Kreuzkorrelation) und
Pegelausgleich (die lautere Aufnahme wird abgesenkt). Ohne beides misst man
Versatz und Lautstaerke statt der Huellkurve. Fenster unterhalb der
Stilleschwelle bleiben aussen vor, sonst bestimmt das Grundrauschen das
Ergebnis.
"""

from __future__ import annotations

import argparse
import math
import sys
from pathlib import Path

import numpy as np

sys.path.insert(0, str(Path(__file__).resolve().parent))
from qc_compare import als_mono, versatz  # noqa: E402

RATE = 48000
STILLE = 1e-4  # unterhalb davon gilt ein Fenster als still


def ausgleichen(a: np.ndarray, b: np.ndarray) -> tuple[np.ndarray, np.ndarray, int]:
    """Zeitversatz herausrechnen und die lautere Aufnahme absenken.

    Das Vorzeichen von `versatz` wird NICHT geglaubt, sondern geprueft: beide
    Richtungen werden beschnitten und die mit der hoeheren Aehnlichkeit
    behalten. Genau dieser Punkt ging am 2026-09-15 schief - die Doku von
    `versatz` war verkehrt, der Versatz wurde dadurch verdoppelt."""
    roh = versatz(a, b)

    def beschneiden(s: int) -> tuple[np.ndarray, np.ndarray]:
        if s > 0:
            x, y = a[: len(b) - s] if len(b) > s else a[:0], b[s:]
        elif s < 0:
            x, y = a[-s:], b[: len(a) + s]
        else:
            x, y = a, b
        m = min(len(x), len(y))
        return x[:m], y[:m]

    beste = None
    for kandidat in (roh, -roh):
        x, y = beschneiden(kandidat)
        if len(x) < RATE:
            continue
        nx, ny = np.linalg.norm(x), np.linalg.norm(y)
        g = abs(float(x @ y) / (nx * ny)) if nx > 0 and ny > 0 else 0.0
        if beste is None or g > beste[0]:
            beste = (g, kandidat, x, y)
    if beste is None:
        raise SystemExit("kein gemeinsamer Bereich")
    _, v, a, b = beste
    a, b = a.copy(), b.copy()
    ra = math.sqrt(float(np.mean(a**2)))
    rb = math.sqrt(float(np.mean(b**2)))
    if ra > rb:
        a *= rb / ra
    else:
        b *= ra / rb
    return a, b, v


def huellkurve(x: np.ndarray, fenster: int) -> np.ndarray:
    m = len(x) // fenster
    return np.sqrt(np.mean(x[: m * fenster].reshape(m, fenster) ** 2, axis=1))


def bericht(name: str, d: np.ndarray) -> str:
    if len(d) == 0:
        return f"{name:<22} (keine Fenster ueber der Stilleschwelle)"
    return (f"{name:<22} n={len(d):4d}  Mittel {d.mean():+6.2f} dB  "
            f"Streuung {d.std():5.2f} dB  Spanne {d.min():+6.2f}..{d.max():+6.2f}")


def main() -> int:
    p = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    p.add_argument("a", type=Path)
    p.add_argument("b", type=Path)
    p.add_argument("--fenster-ms", type=float, default=50.0)
    p.add_argument("--schnitt-s", type=float, default=None,
                   help="trennt den Bericht in vor/nach diesem Zeitpunkt")
    args = p.parse_args()

    a = als_mono(args.a, RATE)
    b = als_mono(args.b, RATE)
    a, b, v = ausgleichen(a, b)
    print(f"{args.a.name}  gegen  {args.b.name}")
    print(f"  Zeitversatz herausgerechnet: {v} Abtastwerte ({v/RATE*1000:.1f} ms)")

    w = int(args.fenster_ms / 1000 * RATE)
    ha, hb = huellkurve(a, w), huellkurve(b, w)
    d = 20 * np.log10((ha + 1e-12) / (hb + 1e-12))
    gueltig = (ha > STILLE) & (hb > STILLE)
    t = np.arange(len(d)) * w / RATE

    print(f"  Fenster {args.fenster_ms:.0f} ms, {int(gueltig.sum())} von {len(d)} ueber der Stilleschwelle")
    print("  " + bericht("gesamt", d[gueltig]))
    if args.schnitt_s is not None:
        print("  " + bericht(f"bis {args.schnitt_s:.0f} s", d[gueltig & (t < args.schnitt_s)]))
        print("  " + bericht(f"ab {args.schnitt_s:.0f} s", d[gueltig & (t >= args.schnitt_s)]))
    k = float(np.corrcoef(ha[gueltig], hb[gueltig])[0, 1])
    print(f"  Korrelation der Huellkurven: {k:.4f}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
