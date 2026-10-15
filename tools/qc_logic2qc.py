#!/usr/bin/env python3
"""qc_logic2qc.py - eine Neural-Plugin-Spur aus Logic als Block-Kette aufs Geraet.

Wer:      Martin.Lonkwitz
Was:      Liest den Zustand eines Neural-Plugins (das XML, das Logic im Projekt
          ablegt) und setzt die entsprechenden Werte in die Bloecke des Quad
          Cortex. Kann die Kette auch nur vorschlagen, ohne etwas zu senden.
Wann:     2026-09-12
Warum:    Der Nutzer will seine Logic-Spur "Archetype Gojira X Clean" auf dem
          Geraet nachbauen. Beide Seiten sind bekannt - der Plugin-Zustand
          liegt im Klartext vor (docs/evidence/97), und ein "Plugin" auf dem
          Geraet ist selbst nur ein Preset (evidence/93). Es fehlte allein die
          Zuordnung Attribut -> Blockparameter, und die steht hier.
Projekt:  Ampersand
Abhaengig: gebautes qcctl, Geraet per USB
Nutzung:  tools/qc_logic2qc.py zustand.xml --zeile 1            # nur zeigen
          tools/qc_logic2qc.py zustand.xml --zeile 1 --setzen   # anwenden
Historie: git log

VORSICHT: `--setzen` VERAENDERT das laufende Preset. Es speichert nichts; ein
`qcctl recall <slot>` stellt den Ausgangszustand wieder her. Vorher sichern.

STAND DER ZUORDNUNG (am Geraet abgelesen, docs/evidence/98)

  vollstaendig   Amp (Gojira CLN) und der 9-Band-EQ
  weitgehend     Delay
  teilweise      Reverb - fuer den Mix-Anteil ist kein benannter Parameter da
  weitgehend     Cab - die Mikrofonierung ist seit 2026-09-13 gemessen
                 (docs/evidence/101): Typ, POSITION, DISTANCE, LEVEL, PAN,
                 PHASE je Mikrofon, dazu ROOM L/R und OUTPUT VOLUME

Was nicht zugeordnet ist, wird nicht gesetzt und beim Lauf genannt. Lieber eine
Luecke, die man sieht, als ein geratener Wert.

MASSSTAEBE, gemessen statt vermutet

  Regler 0..10      Der XML-Wert IST der Parameterwert. 0.605 -> Anzeige 6.05.
  EQ-Baender        +/-12 dB linear, also (dB + 12) / 24. Gemessen an fuenf
                    Punkten: param 0 -> -12 dB, 0.5 -> 0 dB, 1 -> +12 dB.
  Schalter          false -> 0, true -> 1.
  Cab POSITION      1:1, der XML-Wert ist der Parameterwert (Anzeige 0.30 bei 0.3).
  Cab DISTANCE      1:1, ebenso.
  Cab PAN           linear, 0 -> 50 L, 0,5 -> C, 1 -> 50 R. An fuenf Punkten
                    gemessen; das XML nennt 0 fuer die Mitte.
  Cab LEVEL         NICHT linear und keine einfache Potenz - siehe
                    LEVEL_KENNLINIE, sechzehn am Display abgelesene Punkte
                    von -40 dB bis +6 dB. 0 dB = 0,5 und -12 dB = 0,085 sind
                    am Geraet nachgemessen, nicht interpoliert.
"""
from __future__ import annotations

import argparse
import re
import subprocess
import sys
from pathlib import Path

# Kette, die das Buendel selbst benutzt (aus Default.pb, evidence/93). Die
# Spalte ist die Stelle im Grid, das Modell die Katalognummer.
KETTE = [
    (1, 1162, "Gojira CLN"),
    (2, 12050, "Gojira Cab 1 (M)"),
    (3, 4006, "Plugin Graphic-9"),
    (4, 6019, "Gojira DLY"),
    (5, 8016, "Gojira REV"),
]


def db_zu_norm(db: str | float) -> float:
    """EQ-Baender: +/-12 dB linear auf 0..1. Nimmt auch die Zeichenkette aus
    dem XML - die Umrechner werden alle gleich aufgerufen."""
    return max(0.0, min(1.0, (float(db) + 12.0) / 24.0))


def schalter(v: str) -> float:
    return 1.0 if str(v).lower() == "true" else 0.0


def schalter_invers(v: str) -> float:
    """Fuer Parameter 8: 0 heisst AN, 1 heisst AUS. Am Geraet gemessen -
    Parameter 8 auf 1 kippt den Schalter des zweiten Mikrofons im UI-Baum von
    `toggle_iconOn.png` auf `toggle_iconOff.png` (docs/evidence/101)."""
    return 0.0 if str(v).lower() == "true" else 1.0


# Kennlinie des LEVEL-Reglers im Cab-Block, am Display abgelesen
# (docs/evidence/101/rohdaten/skala). Sie ist NICHT linear und auch keine
# einfache Potenz; deshalb wird die Messung selbst benutzt und nicht eine
# Formel, die ungefaehr passt. Dieselbe Kennlinie gilt fuer ROOM L/R LEVEL -
# an zwei Punkten gegengeprueft (0.223 -> -6.0 dB, 0.85 -> 4.5 dB).
LEVEL_KENNLINIE = [
    (0.000, -40.0), (0.050, -14.9), (0.085, -12.0), (0.100, -11.1),
    (0.150, -8.6), (0.200, -6.7), (0.250, -5.2), (0.300, -3.9),
    (0.350, -2.8), (0.400, -1.8), (0.450, -0.8), (0.500, 0.0),
    (0.600, 1.5), (0.750, 3.4), (0.900, 5.0), (1.000, 6.0),
]


def pegel_zu_norm(db: str | float) -> float:
    """dB -> Parameterwert, stueckweise linear ueber die gemessenen Punkte.
    Unterhalb von -40 dB und oberhalb von +6 dB wird begrenzt - das ist der
    Bereich, den der Regler hergibt."""
    z = float(db)
    if z <= LEVEL_KENNLINIE[0][1]:
        return LEVEL_KENNLINIE[0][0]
    if z >= LEVEL_KENNLINIE[-1][1]:
        return LEVEL_KENNLINIE[-1][0]
    for (v0, d0), (v1, d1) in zip(LEVEL_KENNLINIE, LEVEL_KENNLINIE[1:]):
        if d0 <= z <= d1:
            return v0 + (v1 - v0) * (z - d0) / (d1 - d0)
    raise ValueError(f"{db} dB liegt ausserhalb der Kennlinie")


def pan_zu_norm(p: str | float) -> float:
    """PAN ist linear: 0 -> 50 L, 0,5 -> C, 1 -> 50 R. Am Geraet an fuenf
    Punkten gemessen. Das XML nennt 0 fuer die Mitte, also Mitte + p/100."""
    return max(0.0, min(1.0, 0.5 + float(p) / 100.0))


# Zuordnung: (Spalte, Parameterindex, XML-Attribut, Umrechnung, Bemerkung)
ZUORDNUNG = [
    (1, 0, "cleanAmpGain",   float,      "Regler 0..10"),
    (1, 1, "cleanAmpBright", schalter,   "Schalter"),
    (1, 2, "cleanAmpBass",   float,      "Regler 0..10"),
    (1, 3, "cleanAmpMid",    float,      "Regler 0..10"),
    (1, 4, "cleanAmpTreble", float,      "Regler 0..10"),
    (3, 1, "cleanEQBand1",   db_zu_norm, "65 Hz"),
    (3, 2, "cleanEQBand2",   db_zu_norm, "125 Hz"),
    (3, 3, "cleanEQBand3",   db_zu_norm, "250 Hz"),
    (3, 4, "cleanEQBand4",   db_zu_norm, "500 Hz"),
    (3, 5, "cleanEQBand5",   db_zu_norm, "1 kHz"),
    (3, 6, "cleanEQBand6",   db_zu_norm, "2 kHz"),
    (3, 7, "cleanEQBand7",   db_zu_norm, "4 kHz"),
    (3, 8, "cleanEQBand8",   db_zu_norm, "8 kHz"),
    (3, 9, "cleanEQBand9",   db_zu_norm, "16 kHz"),
    (4, 1, "delayMix",       float,      "MIX"),
    (4, 2, "delayFeedback",  float,      "FEEDBACK"),
    (4, 5, "delayPingPong",  schalter,   "PING PONG"),
    (4, 10, "delayTapeMod",  float,      "TAPE MOD"),
    (4, 11, "delayTapeSat",  float,      "TAPE SAT"),
    (5, 0, "reverbDecay",    float,      "DECAY"),
    (5, 4, "reverbLowCut",   float,      "LOW CUT"),
    (5, 5, "reverbHighCut",  float,      "HIGH CUT"),
    # Cab-Mikrofonierung. Alle Indizes am Geraet gemessen, nicht geraten:
    # jeder Parameter einzeln verstellt und am Display nachgesehen
    # (tools/qc_param_kalibrieren.py, docs/evidence/101). Das linke Panel des
    # Geraets ist "left" im XML, das rechte "right".
    (2, 2,  "leftCabMicLevel",   pegel_zu_norm,   "LEVEL links"),
    (2, 3,  "leftCabPan",        pan_zu_norm,     "PAN links"),
    (2, 4,  "leftCabDistance",   float,           "DISTANCE links, 1:1"),
    (2, 5,  "leftCabPosition",   float,           "POSITION links, 1:1"),
    (2, 6,  "leftCabPhase",      schalter,        "PHASE links"),
    (2, 8,  "rightCabActive",    schalter_invers, "Mikrofon rechts an/aus (0 = an)"),
    (2, 10, "rightCabMicLevel",  pegel_zu_norm,   "LEVEL rechts"),
    (2, 11, "rightCabPan",       pan_zu_norm,     "PAN rechts"),
    (2, 12, "rightCabDistance",  float,           "DISTANCE rechts, 1:1"),
    (2, 13, "rightCabPosition",  float,           "POSITION rechts, 1:1"),
    (2, 14, "rightCabPhase",     schalter,        "PHASE rechts"),
    (2, 24, "leftRoomActive",    schalter,        "ROOM LEFT an/aus"),
    (2, 25, "leftRoomMicLevel",  pegel_zu_norm,   "ROOM L LEVEL"),
    (2, 26, "rightRoomActive",   schalter,        "ROOM RIGHT an/aus"),
    (2, 27, "rightRoomMicLevel", pegel_zu_norm,   "ROOM R LEVEL"),
]

# Reihenfolge der Mikrofonliste, am Geraet abgelesen: den Vorwaerts-Knopf des
# Selectors sechsmal getippt und jedes Mal den angezeigten Namen aus dem
# UI-Baum geholt (docs/evidence/101). Nach sechs Schritten war die Liste
# wieder am Anfang - sie hat also genau sechs Eintraege.
MIKROFONE = [
    "Dynamic 57",     # 0
    "Dynamic 421",    # 1
    "Condenser 414",  # 2
    "Condenser 184",  # 3
    "Ribbon 160",     # 4
    "Ribbon 121",     # 5
]

# Textparameter: (Spalte, Index, XML-Attribut, Vorsatz fuer den IR-Namen)
TEXT_ZUORDNUNG = [
    (2, 1, "leftCab0MicType",  "NG_112 Gojira Cab 1_"),
    (2, 9, "rightCab0MicType", "NG_112 Gojira Cab 1_"),
]

# Bewusst NICHT gesetzt, mit Grund - erscheint als Hinweis im Lauf.
OFFEN = {
    "cleanAmpLevel": ("der Level-Regler des Plugins hat einen eigenen Bereich; "
                      "OUTPUT am Geraet ist -60..+12 dB (gemessen). Ohne den "
                      "Plugin-Bereich waere jede Umrechnung geraten - OUTPUT "
                      "bleibt daher auf 0 dB"),
    "reverbMix": "kein benannter MIX-Parameter im Reverb-Block",
    "delayTime": "Bereich des Parameters DELAY TIME nicht kalibriert",
    "delaySync": "Zuordnung der Sync-Stufen offen",
    "delayTone": "kein passender benannter Parameter",
    "cleanEQHpf": "Hertz-Bereich von HPF nicht kalibriert",
    "cleanEQLpf": ("Hertz-Bereich von LPF nicht kalibriert. Der Regler ist "
                   "gefunden - Cab-Parameter 17, gemessen: 1.0 = OFF, 0.15 = "
                   "535 Hz -, aber zwei Punkte sind keine Kennlinie"),
    "leftCabActive": ("fuer das LINKE Mikrofon wurde kein Schalter gefunden; "
                      "nur Parameter 8 schaltet das rechte. Solange das offen "
                      "ist, bleibt das linke Mikrofon an"),
    "leftCabStereo": "kein zugeordneter Parameter gefunden",
    "rightCabStereo": "kein zugeordneter Parameter gefunden",
}


def attribute(xml: str) -> dict[str, str]:
    """Alle Attribute aus allen Elementen, flach. Die Namen sind im Neural-XML
    eindeutig genug (cleanAmpGain, reverbDecay …), eine Baumstruktur braucht es
    dafuer nicht."""
    aus: dict[str, str] = {}
    for m in re.finditer(r"<(\w+)([^>]*)>", xml):
        for k, v in re.findall(r'(\w+)="([^"]*)"', m.group(2)):
            aus.setdefault(k, v)
    return aus


def lauf(cmd: list[str]) -> tuple[int, str]:
    p = subprocess.run(cmd, capture_output=True, text=True, timeout=120)
    return p.returncode, (p.stdout or "") + (p.stderr or "")


def main() -> int:
    p = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    p.add_argument("xml", type=Path)
    p.add_argument("--zeile", type=int, default=1)
    p.add_argument("--setzen", action="store_true", help="wirklich senden")
    p.add_argument("--qcctl", default="build/default/qcctl")
    p.add_argument("--ohne-kette", action="store_true", dest="ohne_kette",
                   help="die Bloecke NICHT neu setzen - noetig, sobald das Preset schon "
                        "abgestimmt ist: ein `block` setzt den Block auf seine Vorgabe zurueck")
    p.add_argument("--spalten", default="",
                   help="nur diese Spalten anfassen, z. B. 2 fuer allein den Cab")
    a = p.parse_args()

    nur = {int(x) for x in a.spalten.split(",") if x.strip()} or None

    werte = attribute(a.xml.read_text(encoding="utf-8"))
    print(f"{a.xml}: {len(werte)} Attribute gelesen\n")

    print("Kette:" if not a.ohne_kette else "Kette: uebersprungen (--ohne-kette)")
    for spalte, modell, name in (() if a.ohne_kette else KETTE):
        if nur and spalte not in nur:
            continue
        print(f"  Zeile {a.zeile} Spalte {spalte}  {modell:>6}  {name}")
        if a.setzen:
            lauf([a.qcctl, "block", str(a.zeile), str(spalte), str(modell)])
            if modell == 1162:
                # OUTPUT auf 0 dB statt auf dem Blockstandard -24 dB: neutral
                # ist die ehrlichere Vorgabe, solange der Bereich des
                # Plugin-Reglers unbekannt ist. (0 dB = (0+60)/72)
                lauf([a.qcctl, "param", str(a.zeile), str(spalte), "5", "0.833333"])

    print("\nParameter:")
    gesetzt = 0
    for spalte, idx, attr, um, bem in ZUORDNUNG:
        if nur and spalte not in nur:
            continue
        if attr not in werte:
            print(f"  {attr:<18} fehlt im Zustand")
            continue
        roh = werte[attr]
        try:
            wert = um(roh)
        except ValueError:
            print(f"  {attr:<18} '{roh}' nicht umrechenbar")
            continue
        print(f"  {attr:<18} {roh:>10}  ->  Spalte {spalte} Parameter {idx} = {wert:.6f}   ({bem})")
        if a.setzen:
            rc, out = lauf([a.qcctl, "param", str(a.zeile), str(spalte), str(idx), f"{wert:.6f}"])
            if rc != 0:
                print(f"      Fehler: {out.strip().splitlines()[-1] if out.strip() else rc}")
            else:
                gesetzt += 1

    print("\nMikrofontypen:")
    for spalte, idx, attr, vorsatz in TEXT_ZUORDNUNG:
        if nur and spalte not in nur:
            continue
        if attr not in werte:
            print(f"  {attr:<18} fehlt im Zustand")
            continue
        roh = werte[attr]
        try:
            name = MIKROFONE[int(roh)]
        except (ValueError, IndexError):
            print(f"  {attr:<18} '{roh}' ist kein bekannter Listenplatz")
            continue
        voll = vorsatz + name
        print(f"  {attr:<18} {roh:>10}  ->  Spalte {spalte} Parameter {idx} = {name}")
        if a.setzen:
            rc, out = lauf([a.qcctl, "param-text", str(a.zeile), str(spalte), str(idx), voll])
            if rc != 0:
                print(f"      Fehler: {out.strip().splitlines()[-1] if out.strip() else rc}")
            else:
                gesetzt += 1
    print("  ANNAHME: Der Listenplatz im Plugin entspricht dem am Geraet. Die")
    print("  Reihenfolge am Geraet ist gemessen, die im Plugin nicht - sie")
    print("  laesst sich nur hoerend oder per Reamp-Vergleich bestaetigen.")

    print("\nNicht gesetzt, und warum:")
    for attr, grund in OFFEN.items():
        if attr in werte:
            print(f"  {attr:<18} {werte[attr]:>10}  -  {grund}")

    if a.setzen:
        print(f"\n{gesetzt} Parameter gesetzt. Nichts gespeichert - `qcctl recall <slot>` verwirft alles.")
    else:
        print("\nNur gezeigt. Mit --setzen anwenden.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
