#!/usr/bin/env python3
"""qc_usbmon_decode.py - einen usbmon-Mitschnitt in Nachrichtentypen uebersetzen.

Wer:      Martin.Lonkwitz
Was:      Liest einen pcap-Mitschnitt des Quad-Cortex-USB-Verkehrs, setzt die
          129-Byte-HID-Reports nach ihren Flags wieder zu Nachrichten zusammen
          und zaehlt aus, welche Nachrichtentypen in welche Richtung liefen.
          Dazu die Aufteilung des Mitschnitts nach Transferart, damit ein
          Audiostrom nicht mit dem Steuerkanal verwechselt wird.
Wann:     2026-09-12
Warum:    Fuer die Referenzmessung gegen das Original (Cortex Control im
          Windows-Gast) braucht es eine Antwort auf genau eine Frage: Kommen
          IOMeter (5) oder GridModelMeter (37) ueber den HID-Kanal? Von Hand
          ist das aus 200 MB Mitschnitt nicht zu beantworten.
Projekt:  Ampersand
Abhaengig: tshark; Mitschnitt von `tools/qc_usb_capture.pl` oder tcpdump auf
          usbmon<N>.
Nutzung:  tools/qc_usbmon_decode.py mitschnitt.pcap
          tools/qc_usbmon_decode.py mitschnitt.pcap --typ 5 --typ 37
Historie: git log

ACHTUNG ZUM DATEIORT: tshark liest hier nur Dateien, die ausserhalb der
Projektablage liegen (unser docs/evidence-Baum steht auf 0700, und tshark
laeuft unter einem Profil, das daran scheitert - Meldung "You don't have
permission to read the file"). Im Zweifel den Mitschnitt vorher nach /tmp
kopieren; dieses Skript sagt es, wenn es danach aussieht.

DAS FRAMING (docs/protocol.md, framing.hpp)

  129 Bytes je Report: [id][len][flags][126 Daten]
      id    0x02 Host -> Geraet, 0x01 Geraet -> Host
      flags 0x40 FIRST, 0x80 LAST, 0xC0 vollstaendig in einem Report
  Der zusammengesetzte Koerper endet auf einen Trailer von 8 Bytes:
      [type u16 LE][0 0][encrypted][compressed][0 0]
  Der Typ steht also am ENDE der Nachricht, nicht am Anfang - wer die ersten
  Bytes liest, liest Protobuf-Nutzlast.
"""
from __future__ import annotations

import argparse
import subprocess
import sys
from collections import Counter
from pathlib import Path

TYPEN = {
    0: "Undefined", 1: "Grid", 2: "SetlistPosition", 3: "IOSettings", 4: "File",
    5: "IOMeter", 6: "Tuner", 7: "Diagnostics", 8: "MIDISettings", 9: "GeneralSettings",
    10: "Version", 11: "ProductionAutomationMode", 12: "GridMove", 13: "Scene", 14: "Mode",
    15: "RecallPreset", 16: "EnableCaptureOut", 17: "MasterVolume", 18: "CloudLogin",
    19: "DefaultParameters", 20: "RecentsFavorites", 21: "UndoRedo", 22: "SceneCopy",
    23: "SceneLabel", 24: "ShowGigView", 25: "Screenshot", 26: "CPULoad", 27: "ShowTuner",
    28: "Looper", 29: "ProductForward", 30: "BackupsForward", 31: "LogsForward",
    32: "KeepAlive", 33: "GlobalTempo", 34: "PresetDirty", 35: "ModuleStats",
    36: "NeuralCapture", 37: "GridModelMeter", 38: "GlobalEQ", 39: "RecentSearches",
    40: "LocalBackup", 41: "CloudBackup", 42: "CompilerInhibitedModules",
    43: "SystemTimeSync", 44: "Logs", 45: "ProcessDownloadsQueue", 46: "CloudProduct",
    47: "Confirmation", 48: "SceneColor", 49: "Connection", 50: "NewModels", 51: "ModelRepo",
    52: "ResetCommsBuffers", 53: "SuspendConnection", 54: "PinnedModels", 55: "GigViewButton",
    56: "GenericError", 57: "BulkOperation", 58: "License", 59: "PresetSpeedTest",
    60: "Updater", 61: "UpdaterForward", 62: "GainCalibration", 63: "NeuralCapture2",
    64: "Serialization", 65: "TestFarm", 66: "ProductionTest", 67: "LoadAutomatedTestPreset",
    68: "SetTestPresetInputOutputPorts", 69: "SetTestPresetSplitMixPoints",
    70: "GenerateTestPreset",
}

TRANSFER = {"0x00": "isochron", "0x01": "interrupt", "0x02": "control", "0x03": "bulk"}

FIRST, LAST = 0x40, 0x80


def tshark(pcap: Path, felder: list[str], filter_: str = "") -> list[list[str]]:
    cmd = ["tshark", "-r", str(pcap), "-T", "fields"]
    for f in felder:
        cmd += ["-e", f]
    if filter_:
        cmd += ["-Y", filter_]
    p = subprocess.run(cmd, capture_output=True, text=True)
    if p.returncode != 0:
        hinweis = ""
        if "permission" in p.stderr.lower():
            hinweis = ("\nHinweis: tshark kommt an die Datei nicht heran. Kopiere den "
                       "Mitschnitt nach /tmp und wiederhole es dort - siehe Kopf dieses Skripts.")
        print(f"tshark schlug fehl:\n{p.stderr.strip()}{hinweis}", file=sys.stderr)
        raise SystemExit(2)
    return [z.split("\t") for z in p.stdout.splitlines() if z.strip()]


def uebersicht(pcap: Path) -> None:
    """Wie verteilt sich der Mitschnitt? Trennt Steuerkanal von Nutzdatenstrom."""
    zeilen = tshark(pcap, ["usb.transfer_type", "usb.endpoint_address", "usb.data_len"])
    eimer: dict[tuple[str, str], list[int]] = {}
    for z in zeilen:
        if len(z) < 3:
            continue
        art, ep, laenge = z[0], z[1], z[2]
        try:
            n = int(laenge) if laenge else 0
        except ValueError:
            n = 0
        eimer.setdefault((art, ep), []).append(n)
    print("Aufteilung des Mitschnitts")
    print(f"  {'Transferart':<12} {'Endpunkt':<10} {'Pakete':>8} {'Bytes':>14}")
    for (art, ep), werte in sorted(eimer.items(), key=lambda kv: -sum(kv[1])):
        print(f"  {TRANSFER.get(art, art):<12} {ep:<10} {len(werte):>8} {sum(werte):>14}")
    print()


def nachrichten(pcap: Path) -> list[tuple[float, str, int, int, bool]]:
    """Setzt die HID-Reports zu Nachrichten zusammen.

    Rueckgabe je Nachricht: (Zeit, Richtung, Typ, Koerperlaenge, verschluesselt).
    Ein FIRST|LAST-Report waehrend einer laufenden Uebertragung ist eine
    eigenstaendige Nachricht (Interleaving) - deshalb wird je Richtung ein
    eigener Puffer gefuehrt und ein vollstaendiger Report sofort abgeschlossen,
    ohne den laufenden Puffer anzutasten."""
    # Drei Feldnamen, weil tshark die Nutzlast je nach Transferart anders
    # ablegt: `usbhid.data` bei den Interrupt-Reports vom Geraet,
    # `usb.data_fragment` bei den SET_REPORT-Schreibvorgaengen des Hosts ueber
    # EP0. Wer nur eines abfragt, sieht genau eine Richtung - und haelt den
    # Steuerkanal faelschlich fuer einseitig.
    zeilen = tshark(pcap, ["frame.time_relative", "usbhid.data", "usb.data_fragment",
                           "usb.capdata"],
                    "usb.transfer_type == 0x01 || usb.transfer_type == 0x02")
    puffer: dict[str, bytearray] = {}
    aus: list[tuple[float, str, int, int, bool]] = []
    for z in zeilen:
        hexdaten = next((f for f in z[1:] if f), "")
        if not hexdaten:
            continue
        try:
            zeit = float(z[0])
        except ValueError:
            continue
        try:
            roh = bytes.fromhex(hexdaten.replace(":", ""))
        except ValueError:
            continue
        if len(roh) < 3:
            continue
        kennung, laenge, flags = roh[0], roh[1], roh[2]
        richtung = {0x01: "Geraet->Host", 0x02: "Host->Geraet"}.get(kennung)
        if richtung is None:
            continue
        daten = roh[3 : 3 + laenge]

        if flags & FIRST and flags & LAST:
            koerper = daten                      # vollstaendig in einem Report
        elif flags & FIRST:
            puffer[richtung] = bytearray(daten)
            continue
        else:
            if richtung not in puffer:
                continue                          # Anfang fehlt (Mitschnitt begann mittendrin)
            puffer[richtung] += daten
            if not flags & LAST:
                continue
            koerper = bytes(puffer.pop(richtung))

        if len(koerper) < 8:
            continue
        trailer = koerper[-8:]
        typ = trailer[0] | (trailer[1] << 8)
        aus.append((zeit, richtung, typ, len(koerper) - 8, bool(trailer[4])))
    return aus


def main() -> int:
    p = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    p.add_argument("pcap", type=Path)
    p.add_argument("--typ", type=int, action="append", default=[],
                   help="diese Typnummer einzeln auflisten (mehrfach moeglich)")
    a = p.parse_args()
    if not a.pcap.is_file():
        print(f"nicht gefunden: {a.pcap}", file=sys.stderr)
        return 2

    uebersicht(a.pcap)
    msgs = nachrichten(a.pcap)
    if not msgs:
        print("Keine zusammengesetzten HID-Nachrichten im Mitschnitt.")
        return 0

    dauer = msgs[-1][0] - msgs[0][0]
    print(f"HID-Nachrichten: {len(msgs)} ueber {dauer:.1f} s")
    print(f"  {'Typ':>4}  {'Name':<24} {'Richtung':<14} {'Anzahl':>7} {'je s':>7}")
    zaehler = Counter((typ, richtung) for _, richtung, typ, _, _ in msgs)
    for (typ, richtung), n in sorted(zaehler.items(), key=lambda kv: -kv[1]):
        rate = n / dauer if dauer > 0 else 0.0
        print(f"  {typ:>4}  {TYPEN.get(typ, '?'):<24} {richtung:<14} {n:>7} {rate:>7.1f}")

    for gesucht in a.typ:
        treffer = [m for m in msgs if m[2] == gesucht]
        name = TYPEN.get(gesucht, "?")
        print(f"\nTyp {gesucht} ({name}): {len(treffer)} Nachrichten")
        for zeit, richtung, _, laenge, enc in treffer[:20]:
            print(f"  {zeit:8.3f}s  {richtung:<14} {laenge:>6} Bytes{'  verschluesselt' if enc else ''}")
        if len(treffer) > 20:
            print(f"  ... und {len(treffer) - 20} weitere")
    return 0


if __name__ == "__main__":
    sys.exit(main())
