#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""Die Selbstdiagnose des Quad Cortex lesen und entschluesseln.

WER    Projekt Ampersand (SUB-PROJECT FROM: Quad-Cortex).
WAS    Ruft `qcctl raw 7 CREATE` auf, zerlegt die zurueckkommenden
       `DiagnosticsMessage`-Antworten nach dem Protobuf-Drahtformat und gibt
       sie als Tabelle aus - je Kern die CPU-Last, und fuer die Bruecke
       SOC2-ARM die HID-Zaehler samt Verlusten.
WANN   2026-09-20.
WARUM  Die Frage war, wo das Nadeloehr des USB-Wegs liegt. Neurals eigene
       Firmware misst das bereits: `SOC2ARMCommsDiagnosticsMessage` fuehrt
       USBHIDInCount, USBHIDOutCount und - entscheidend - die beiden
       Verlustzaehler USBHIDInDroppedCount und USBHIDOutDroppedCount, dazu die
       Lese- und Schreibzeiger eines Ringpuffers. Dazu kommt
       `soc1_to_soc2_dropped_count` in der umschliessenden Nachricht. Wer das
       liest, muss nicht mehr raten.
       ACHTUNG, die Aktion ist CREATE, nicht READ: auf READ antwortet das
       Geraet gar nicht (gemessen 2026-09-20, wie schon bei backup-save).
PROJEKT Ampersand.
ABHAENGIGKEITEN
       qcctl im Pfad oder unter ~/.local/bin. Sonst nur die Standardbibliothek.
NUTZUNG
       tools/qc_diagnose.py                 # eine Messung
       tools/qc_diagnose.py --sekunden 20   # laenger mitschreiben
       tools/qc_diagnose.py --roh           # zusaetzlich die Rohbytes
HISTORIE
       Siehe git log dieser Datei.

DRAHTFORMAT, nur so viel wie noetig: jedes Feld beginnt mit einem Varint
`(nummer << 3) | typ`. Typ 0 = Varint, 2 = laengenbegrenzt, 5 = 32 Bit.
Die Feldnummern stammen aus proto/ProductionAutomation.proto.
"""

from __future__ import annotations

import argparse
import os
import re
import struct
import subprocess
import sys

# proto/ProductionAutomation.proto, message DSPCommsDiagnosticsMessage
DSP_FELDER = {
    1: "status1", 2: "status2", 3: "status3", 4: "status4",
    5: "process_cpu", 6: "internal_heap_usage", 7: "external_heap_usage",
    8: "internal_pm_heap_usage", 9: "process_cpu_max", 10: "ext_dma_usage",
    11: "error_flags",
}
# message SOC2ARMCommsDiagnosticsMessage
ARM_FELDER = {
    1: "USBConnected", 2: "inReadIdx", 3: "inWriteIdx", 4: "outReadIdx",
    5: "outWriteIdx", 6: "USBAudioRxFrame", 7: "USBAudioTxFrame",
    8: "USBAudioTxFrameSkipped", 9: "USBAudioTxFrameAborted",
    10: "USBAudioOutRdWrDistanceAverage", 11: "USBAudioRdWrMinNumSamples",
    12: "USBAudioRdWrMaxNumSamples", 13: "USBAudioPlusOneSampleCount",
    14: "USBAudioMinusOneSampleCount", 15: "USBAudioFeedbackState",
    16: "USBAudioStreamingEPEnabled", 17: "USBAudioStreamingEPDisabled",
    18: "USBMidiOutCount", 19: "USBMidiInCount",
    20: "USBHIDInCount", 21: "USBHIDOutCount",
    22: "USBHIDInDroppedCount", 23: "USBHIDOutDroppedCount",
}
# message DiagnosticsMessage
KERNE = {3: "SOC1 Core1", 4: "SOC1 Core2", 5: "SOC2 Core1", 6: "SOC2 Core2"}


def varint(daten: bytes, i: int) -> tuple[int, int]:
    wert = verschiebung = 0
    while True:
        b = daten[i]; i += 1
        wert |= (b & 0x7F) << verschiebung
        if not b & 0x80:
            return wert, i
        verschiebung += 7


def felder(daten: bytes):
    """Gibt (nummer, typ, wert) je Feld. Wert: int, bytes oder float."""
    i = 0
    while i < len(daten):
        kopf, i = varint(daten, i)
        nummer, typ = kopf >> 3, kopf & 7
        if typ == 0:
            wert, i = varint(daten, i)
        elif typ == 2:
            laenge, i = varint(daten, i)
            wert, i = daten[i:i + laenge], i + laenge
        elif typ == 5:
            wert, i = struct.unpack_from("<f", daten, i)[0], i + 4
        elif typ == 1:
            wert, i = struct.unpack_from("<Q", daten, i)[0], i + 8
        else:
            raise ValueError(f"unbekannter Drahttyp {typ} bei {i}")
        yield nummer, typ, wert


def benenne(daten: bytes, namen: dict[int, str]) -> dict[str, object]:
    return {namen.get(n, f"feld{n}"): w for n, _, w in felder(daten)}


def zerlege(roh: bytes) -> dict[str, object]:
    ergebnis: dict[str, object] = {}
    for n, _, w in felder(roh):
        if n in KERNE:
            ergebnis[KERNE[n]] = benenne(w, DSP_FELDER)
        elif n == 7:
            ergebnis["SOC2-ARM"] = benenne(w, ARM_FELDER)
        elif n == 8:
            ergebnis["soc1_to_soc2_dropped_count"] = w
    return ergebnis


def qcctl() -> str:
    for p in (os.path.expanduser("~/.local/bin/qcctl"), "qcctl"):
        if os.path.isfile(p) or subprocess.run(["which", p], capture_output=True).returncode == 0:
            return p
    raise SystemExit("qcctl nicht gefunden")


def hole(sekunden: int, verzug: int) -> list[bytes]:
    aus = subprocess.run(
        [qcctl(), "raw", "7", "CREATE", str(sekunden), "--delay", str(verzug)],
        capture_output=True, text=True).stdout
    treffer = []
    for zeile in aus.splitlines():
        m = re.search(r"rx Diagnostics \(\d+ bytes\): ([0-9a-f ]+)", zeile)
        if m and "..." not in zeile:
            treffer.append(bytes.fromhex(m.group(1).replace(" ", "")))
    return treffer


def main() -> int:
    a = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    a.add_argument("--sekunden", type=int, default=6)
    a.add_argument("--verzug", type=int, default=12000,
                   help="ms warten, bis die Startflut des Geraets durch ist")
    a.add_argument("--roh", action="store_true")
    args = a.parse_args()

    proben = hole(args.sekunden, args.verzug)
    if not proben:
        print("keine Diagnostics-Antwort - laeuft ZenUI? (auf p2 antwortet nichts)", file=sys.stderr)
        return 1
    print(f"{len(proben)} Antworten\n")

    erste, letzte = zerlege(proben[0]), zerlege(proben[-1])

    print(f"{'Kern':<12}{'CPU %':>9}{'CPU max %':>11}{'DMA %':>9}{'Heap int':>11}{'Heap ext':>11}")
    print("-" * 63)
    for k in KERNE.values():
        d = letzte.get(k)
        if not d:
            continue
        print(f"{k:<12}{d.get('process_cpu', 0):>9.1f}{d.get('process_cpu_max', 0):>11.1f}"
              f"{d.get('ext_dma_usage', 0):>9.1f}{d.get('internal_heap_usage', 0):>11.0f}"
              f"{d.get('external_heap_usage', 0):>11.0f}")

    arm_a, arm_b = erste.get("SOC2-ARM", {}), letzte.get("SOC2-ARM", {})
    print(f"\nBruecke SOC2-ARM  ({len(proben)} Proben, {args.sekunden}s)")
    print("-" * 63)
    for name in ARM_FELDER.values():
        b = arm_b.get(name, 0)
        d = b - arm_a.get(name, 0)
        if b == 0 and d == 0:
            continue
        print(f"  {name:<34}{b:>14}{('  (+%d)' % d) if d else ''}")
    for name in ("USBHIDInDroppedCount", "USBHIDOutDroppedCount"):
        if name not in arm_b:
            print(f"  {name:<34}{0:>14}   (Feld fehlt = 0)")
    print(f"  {'soc1_to_soc2_dropped_count':<34}{letzte.get('soc1_to_soc2_dropped_count', 0):>14}")

    if args.roh:
        print("\nRohbytes der letzten Probe:\n  " + proben[-1].hex(" "))
    return 0


if __name__ == "__main__":
    sys.exit(main())
