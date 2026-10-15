# Protokoll-Notizen (Quad Cortex USB-HID / Protobuf)

Kurzfassung dessen, was diese Implementierung nutzt. Die zugrunde liegende
Protokollkenntnis stammt aus zwei quelloffenen Projekten:
[pyquadcortex](https://github.com/stokes-audio/pyquadcortex) (stokes-audio, MIT,
Stand CorOS 4.0.1 / d14e) und
[CortexUSB](https://github.com/VanIseghemThomas/CortexUSB) (VanIseghemThomas,
MIT). "Bestaetigt hier" = gegen eine eigene Einheit (CorOS 4.1.0, `app_fw`
d14e) nachgemessen.

## Geraet

- USB `152a:880a`, HID-Interface 5, Usage Page 0x01/Usage 0x00, nur ein
  Interrupt-IN-Endpoint → Output-Reports gehen als `SET_REPORT` ueber EP0.
- **Jeder Host-Write wird vom Geraet mit STALL im Status-Stage quittiert** und
  trotzdem verarbeitet. Linux hidraw meldet EPIPE; `HidrawTransport::write`
  zaehlt das als zugestellt. Bestaetigt hier: `qcctl --verbose version` liefert
  die Antwort trotz EPIPE.
- Kein unaufgeforderter Traffic vor dem Handshake.

## Report-Framing (framing.hpp)

129 Bytes: `[id][len][flags][126 Daten]`, id 0x02 Host→Geraet, 0x01 zurueck;
`flags` 0x40 FIRST, 0x80 LAST, 0xC0 komplett. Kein Gesamtlaengenfeld;
Reassemblierung rein flag-gesteuert. Body = protobuf ++ Trailer(8):
`[type u16 LE][0 0][encrypted][compressed][0 0]`. Ein FIRST|LAST-Report waehrend
einer laufenden Mehrfach-Uebertragung ist eine eigenstaendige Nachricht
(Interleaving, CortexUSB) — Test `test_framing.cpp`.

## Handshake (session.cpp), bestaetigt hier

1. `ResetCommsBuffers{session_id: 32 hex}` → Echo.
2. `Version{READ}` → Version-Blob (fw, Seriennummer, MAC, `comms_version`).
3. `comms_version` (Teil des Version-Blobs) wird nur roh als Hex festgehalten
   (`comms_version_hex`). Verschluesselte Nachrichten (Trailer-Flag
   `encrypted`) werden nicht unterstuetzt: sie bleiben roh und tragen
   `decrypt_failed`.
4. `Version{UPDATE, cortex_control_version: <zenos-Version des Geraets>}`
   (oeffnet das Push-Gate); das Geraet fragt seinerseits `Version{READ}` und
   wird ebenso beantwortet. Bestaetigt hier: "4.0.1" gegen CorOS 4.1.0 ergibt
   `GenericError{VERSION_IS_OLDER}`, "4.1.0" ergibt
   `Version{cortex_control_version_valid: true}` → Default "auto".
5. `ModelRepo{READ}` (Bereitschafts-Gate; Payload = gzip(tar(ModelRepo.xml)),
   hier 59 KB → 633 Modelle).
6. `Connection{connected: true}` + READ-Burst fuer 22 Zustandstypen
   (Subscriptions), 2 s settle, dann `KeepAlive{UPDATE, is_online}` je Sekunde.
7. Seed-Pushes (`RecallPreset`, `SetlistPosition`, `Scene`, …) kommen ~9–11 s
   nach dem Connect (hier gemessen: 11 s).

## Operationen (device.cpp) und ihr Beleg

| Operation | Wire-Shape | Beleg |
|---|---|---|
| Recall | `SetlistPosition{UPDATE, folder_key, position, is_factory}` | Capture (pyquadcortex); Factory-Pfad mit Slash `/opt/neuraldsp/Factory Library/` |
| Live-Grid lesen | `RecallPreset{READ, request_id}` → Push mit gleicher id | Hardware-Readback (pyquadcortex); bestaetigt hier (`qcctl current`) |
| Preset lesen | Recall mit `request_id`, Push echot sie | pyquadcortex |
| Szene | `Scene{UPDATE, selected_scene}`; `Scene{READ, request_id}` → `selected_scene` | Capture; bestaetigt hier |
| Parameter | `Grid{UPDATE, chains{row, models{column, params{index, param_values{float}}}}}` | Capture (Knob-Drag); Index positional |
| Per-Szene | `scene_mode` ALLEIN senden, Szene wechseln, dann Wert (`param_values[0]` = aktive Szene) | Hardware-Readback |
| Bypass | `Grid{UPDATE, bypass{row, colBypass{column, sceneBypass[0]{bypass}}}}` ohne `chains`-Geschwister | Hardware-A/B |
| Block setzen | `Grid{UPDATE, chains{row, models{column, hash}}}`; Echo bestaetigt, Ausbleiben = kein DSP-Platz | Readback + Echo-Messung |
| Block loeschen | gleiche Form mit `action: DELETE` | Geraete-Broadcast beim Loeschen am Touchscreen |
| Routing | `chains{row, in_portid}` / `out_portid` (19 = Multi Out) | Readback |
| Split | `chains{row, split_control_points{split, mix}}`, nur Zeile 0/2 | Readback |
| Lane-Output | `chains{row, output_control{hash 23000, params{…}}}` | Readback |
| Tempo | `tempoProgramData{hash 25000, params{index 0, (bpm-40)/200}}` | Readback |
| Save As | `File{CREATE, type 0, folder{key, is_factory false, files{index, name, instrument}}}`, ohne Payload (Grid-Snapshot) | Capture + Readback |
| Delete/Move | `File{DELETE/MOVE, …, files{key: "<folder>/<name>.pb"}}` | Capture |
| Master-Volume | `MasterVolume{UPDATE, volume}` (nie `calibrate`) | Readback + Gehoer |
| Listen | `File{READ}` → Flut von `File{folder{key, files[]}}` (256 Slots je Setlist) | Capture; bestaetigt hier (20 Presets in "My Presets") |

## Ports

Eingaenge (`in_portid`): 0 Empty, 1 Input 1, 2 Input 2, 3 Input 1/2, 4 Return 1,
5 Return 2, 6 Return 1/2, 7 Prev. Row, 8–11 USB 5–8, 12 USB 5/6, 13 USB 7/8,
14 Sidechain. Ausgaenge (`out_portid`): 1 Out 1/2, 2 Out 3/4, 3 Send 1/2,
4–7 Einzelausgaenge, 8/9 Sends, 10–15 USB, 16–18 Zeilen-intern, 19 Multi Out,
20–22 USB 3/4. Bestaetigt hier: Zeile 1 = "Input 2" → "USB 3/4".

## Offen / nicht verifiziert

- Verhalten bei CorOS > 4.1 (Protokoll ist unversioniert).
- IR-Import ueber USB (`ir_payload`): Format unbekannt (pyquadcortex).
- Master-Volume liest 0.0 mit `engaged: true`, auch bei Knopf auf 4 — das Feld
  spiegelt nicht die Knopfstellung.
- Verschluesselte Pushes (`License`, `CloudLogin`, `LogsForward`): werden nicht
  unterstuetzt. Die Session markiert sie mit `decrypt_failed` und zaehlt sie
  (`Stats::decrypt_failures`); ausgehend wird nur im Klartext gesendet.
  Steuerfunktionen sind unbetroffen (Klartext).
- Geraete-Logs ueber USB: `Logs{READ/CREATE}` bleibt unbeantwortet;
  `LogsForward` ist ein Cloud-Kanal ueber den Host (nicht unterstuetzt).
