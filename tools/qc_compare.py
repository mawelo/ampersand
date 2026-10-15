#!/usr/bin/env python3
"""qc_compare.py - zwei Aufnahmen derselben Quelle sachlich vergleichen.

Wer:      Martin.Lonkwitz
Was:      Bringt zwei Audiodateien auf gemeinsame Abtastrate und Kanalzahl,
          sucht den Zeitversatz per Kreuzkorrelation, passt den Pegel nach
          kleinsten Quadraten an und nennt dann erst den Restfehler - dazu den
          Unterschied ueber Terzbaender.
Wann:     2026-09-12
Warum:    "Klingt aehnlich" ist keine Aussage. Fuer den Vergleich der
          nachgebauten Gojira-Kette gegen den Logic-Bounce (docs/evidence/99)
          braucht es Zahlen, und sie muessen um die beiden Stoergroessen
          bereinigt sein, die jeden Hoervergleich verfaelschen: Zeitversatz und
          Lautstaerkeunterschied. Erst danach sagt ein Restfehler etwas.
Projekt:  Ampersand
Abhaengig: numpy, sox
Nutzung:  tools/qc_compare.py referenz.wav kandidat.aif
          Optional: --rate 48000, --band-grenze 40 (unterste Terzmitte in Hz)
Historie: git log

WAS DIE ZAHLEN BEDEUTEN

  Versatz      Abtastwerte, um die der Kandidat gegen die Referenz verschoben
               ist. Kommt von unterschiedlicher Latenz der Ketten.
  Pegelunterschied  Reiner Lautstaerkeunterschied, ueber den Effektivwert.
  Aehnlichkeit      Korrelation der Wellenformen: 1 gleich, 0 nichts gemeinsam.
  Restfehler        Rest nach Anpassung kleinster Quadrate. NUR aussagekraeftig,
                    wenn die Aehnlichkeit hoch ist - sonst misst er, dass die
                    Wellenformen verschieden sind, und sagt nichts ueber Klang.
  Terzbaender  WO es auseinandergeht. Eine gleichmaessige Abweichung ueber alle
               Baender ist ein Pegelunterschied; eine in wenigen Baendern ist
               ein Klangunterschied.

VORSICHT BEI DER DEUTUNG: Zwei verschiedene Amp-Modelle ergeben auch bei
gleicher Einstellung einen erheblichen Restfehler - die Nichtlinearitaet macht
aus kleinen Unterschieden grosse. Der Restfehler taugt zum Vergleich MEHRERER
Kandidaten gegeneinander, nicht als absolutes Guetemass.
"""
from __future__ import annotations

import argparse
import subprocess
import sys
import tempfile
from pathlib import Path

import numpy as np


def als_mono(datei: Path, rate: int) -> np.ndarray:
    """Ueber sox auf mono, gewuenschte Rate, 32-bit float - sox rechnet die
    Abtastrate sauber um, das ist keine Aufgabe fuer eigene Schleifen."""
    # Rohformat statt WAV: Pythons `wave` kann keine Float-WAVs lesen
    # (Formatkennung 3), und ein Umweg ueber Ganzzahlen kostet Genauigkeit.
    with tempfile.NamedTemporaryFile(suffix=".raw", delete=False) as tmp:
        ziel = Path(tmp.name)
    r = subprocess.run(["sox", str(datei), "-t", "raw", "-b", "32", "-e", "float",
                        "-r", str(rate), "-c", "1", str(ziel)],
                       capture_output=True, text=True, timeout=300)
    if r.returncode != 0:
        print(f"sox ({datei}): {r.stderr.strip()}", file=sys.stderr)
        raise SystemExit(1)
    daten = np.fromfile(ziel, dtype="<f4").astype(np.float64)
    ziel.unlink()
    return daten


def versatz(a: np.ndarray, b: np.ndarray) -> int:
    """Zeitversatz per Kreuzkorrelation im Frequenzbereich.

    ACHTUNG, VORZEICHEN: NEGATIV heisst, b liegt SPAETER als a. Der Wert ist
    also der Versatz von a gegen b, nicht umgekehrt. Bis 2026-09-15 stand hier
    das Gegenteil; qc_compare selbst war nie betroffen, weil main() ohnehin
    beide Richtungen probiert (siehe `beschneiden`). Ein anderes Werkzeug ist
    der falschen Zeile gefolgt und hat den Versatz verdoppelt statt
    ausgeglichen. Wer sich hier verlaesst, probiert besser beide Vorzeichen."""
    n = 1 << int(np.ceil(np.log2(len(a) + len(b))))
    fa = np.fft.rfft(a - a.mean(), n)
    fb = np.fft.rfft(b - b.mean(), n)
    korr = np.fft.irfft(fa * np.conj(fb), n)
    korr = np.concatenate((korr[-(len(b) - 1):], korr[: len(a)]))
    return int(np.argmax(np.abs(korr)) - (len(b) - 1))


def terzbaender(x: np.ndarray, rate: int, unterste: float) -> list[tuple[float, float]]:
    """Effektivwert je Terzband, in dB. Fensterlos ueber das ganze Stueck - fuer
    einen Klangvergleich reicht das Mittel."""
    spektrum = np.abs(np.fft.rfft(x * np.hanning(len(x))))
    freq = np.fft.rfftfreq(len(x), 1.0 / rate)
    aus: list[tuple[float, float]] = []
    mitte = unterste
    while mitte < rate / 2.2:
        unten, oben = mitte / 2 ** (1 / 6), mitte * 2 ** (1 / 6)
        maske = (freq >= unten) & (freq < oben)
        if maske.any():
            energie = float(np.sqrt(np.mean(spektrum[maske] ** 2)))
            aus.append((mitte, 20 * np.log10(energie) if energie > 0 else -200.0))
        mitte *= 2 ** (1 / 3)
    return aus


def main() -> int:
    p = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    p.add_argument("referenz", type=Path)
    p.add_argument("kandidat", type=Path)
    p.add_argument("--rate", type=int, default=48000)
    p.add_argument("--band-grenze", type=float, default=40.0)
    a = p.parse_args()

    ref = als_mono(a.referenz, a.rate)
    kan = als_mono(a.kandidat, a.rate)
    print(f"  Referenz : {a.referenz.name:<34} {len(ref)/a.rate:7.2f} s")
    print(f"  Kandidat : {a.kandidat.name:<34} {len(kan)/a.rate:7.2f} s")

    v = versatz(ref, kan)
    print()

    # Auf den gemeinsamen Bereich beschneiden. Das Vorzeichen des Versatzes
    # falsch zu deuten kostet die ganze Auswertung: Bei 11 Abtastwerten faellt
    # es nicht auf, bei 20000 ist die Aehnlichkeit dann null. Deshalb werden
    # beide Richtungen probiert und die bessere behalten - das ist billig und
    # prueft sich selbst.
    def beschneiden(schnitt: int) -> tuple[np.ndarray, np.ndarray]:
        if schnitt > 0:
            r, k = ref, kan[schnitt:]
        else:
            r, k = ref[-schnitt:], kan
        m = min(len(r), len(k))
        return r[:m], k[:m]

    beste = None
    for kandidat in (v, -v):
        r, k = beschneiden(kandidat)
        if len(r) < a.rate:
            continue
        nr, nk = np.linalg.norm(r), np.linalg.norm(k)
        g = abs(float(r @ k) / (nr * nk)) if nr > 0 and nk > 0 else 0.0
        if beste is None or g > beste[0]:
            beste = (g, kandidat, r, k)
    if beste is None:
        print("  kein gemeinsamer Bereich", file=sys.stderr)
        return 1
    _, v, ref_a, kan_a = beste
    n = len(ref_a)
    print(f"  Versatz  : {v:+d} Abtastwerte ({v/a.rate*1000:+.1f} ms), Richtung geprueft")
    print(f"  Vergleich: {n/a.rate:.2f} s gemeinsamer Bereich")

    # ZWEI VERSCHIEDENE MASSSTAEBE, und sie nicht zu verwechseln ist der Kern:
    #
    #   Kleinste Quadrate      beantwortet "ist es dieselbe Wellenform?".
    #   Effektivwert           beantwortet "ist es derselbe Klang?".
    #
    # Bei schwacher Aehnlichkeit liefert die Anpassung nach kleinsten Quadraten
    # NICHT den Lautstaerkeausgleich, sondern Korrelation mal Pegelverhaeltnis -
    # der Faktor faellt dann viel zu klein aus, und ein Bandvergleich damit
    # zeigt ueberall dieselbe falsche Absenkung. Fuer den Klangvergleich wird
    # deshalb ueber den Effektivwert angeglichen.
    rms_ref = float(np.sqrt(np.mean(ref_a**2)))
    rms_kan = float(np.sqrt(np.mean(kan_a**2)))
    kq_nenner = float(kan_a @ kan_a)
    kq_faktor = float(ref_a @ kan_a) / kq_nenner if kq_nenner > 0 else 0.0
    korr = (float(ref_a @ kan_a) / (np.linalg.norm(ref_a) * np.linalg.norm(kan_a))
            if rms_ref > 0 and rms_kan > 0 else 0.0)
    pegel_faktor = rms_ref / rms_kan if rms_kan > 0 else 0.0
    rest = ref_a - kq_faktor * kan_a
    fehler = float(np.sqrt(np.mean(rest**2))) / rms_ref if rms_ref > 0 else 1.0

    print(f"  Pegelunterschied : {20*np.log10(pegel_faktor):+.1f} dB (Effektivwert)")
    print(f"  Aehnlichkeit     : {korr:+.3f} (1 = gleiche Wellenform, 0 = keine)")
    print(f"  Restfehler       : {fehler:.4f} nach Anpassung kleinster Quadrate")
    if abs(korr) < 0.8:
        print("     Hinweis: Bei dieser Aehnlichkeit ist der Restfehler kein Klangmass -")
        print("     er sagt nur, dass die Wellenformen verschieden sind. Die Terzbaender")
        print("     unten sind ueber den Effektivwert angeglichen und deshalb aussagekraeftig.")

    print(f"\n  Terzbaender - Kandidat gegen Referenz, auf gleichen Effektivwert gebracht:")
    br = terzbaender(ref_a, a.rate, a.band_grenze)
    bk = terzbaender(pegel_faktor * kan_a, a.rate, a.band_grenze)
    print(f"    {'Hz':>7}  {'Referenz':>9}  {'Kandidat':>9}  {'Differenz':>9}")
    for (f, dr), (_, dk) in zip(br, bk):
        d = dk - dr
        balken = ("+" if d > 0 else "-") * min(30, int(abs(d)))
        print(f"    {f:7.0f}  {dr:9.1f}  {dk:9.1f}  {d:+9.1f}  {balken}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
