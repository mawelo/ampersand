# Architektur

Schichten von unten nach oben; jede Schicht kennt nur die direkt darunter.

```
  cortex-control (Qt)     qcctl (CLI)      qc-mcp (MCP/stdio)
        │                     │                  │
        └──────────────┬──────┴──────────────────┘
                       ▼
             qc::QuadCortex  (device.hpp)      fachliche API, Events
                       │
             qc::Session     (session.hpp)     RX-Thread, Korrelation, Handshake,
                       │                       Keepalive, gunzip
        ┌──────────────┴──────────────┐
        ▼                             ▼
   framing.hpp                  compression.hpp  reine Codecs, kein I/O
        │
   transport.hpp  ◄── hid_transport.hpp (Linux hidraw)  ◄── tests/fake_transport.hpp
```

Querschnitt: `catalog.hpp` (ModelRepo → Namen/Parameter), `preset_view.hpp`
(BinaryPreset → anzeigbare Sicht, JSON), `messages.hpp` (Typ-Enums), `log.hpp`,
`locked.hpp`, `version.hpp`.

## Datenfluss

Host → Gerät:

```
QuadCortex::switch_scene(1)
  → SceneMessage{action: UPDATE, selected_scene: 1}   (Protobuf)
  → Session::send(MessageType::Scene, msg)
  → framing::encode(13, bytes) → 1..n Reports [02][len][flags][data…][trailer]
  → HidrawTransport::write() je Report (EPIPE = STALL = zugestellt)
```

Gerät → Host (RX-Thread der Session):

```
HidrawTransport::read() → Reassembler::push() → decode_body()
  → [encrypted? nicht unterstuetzt: Nutzlast bleibt roh, decrypt_failed]
  → [gzip-Magic? inflate]
  → Inbound{type, payload}
  → Warter (await/request, erster Treffer) + Sammler (collect, alle) + Listener
  → QuadCortex::on_inbound → Event-Variante → EventSink (GUI/CLI)
```

## Threads

| Thread | Besitzer | Aufgabe |
|---|---|---|
| RX (`std::jthread`) | Session | liest Reports, reassembliert, entpackt, stellt zu. Stirbt nie an einer kaputten Nachricht. |
| Keepalive (`std::jthread`) | Session | `KeepAlive{UPDATE, is_online}` jede Sekunde |
| Aufrufer | Anwendung | blockierende `await/request/collect` |
| `qc-worker` (QThread) | GUI | `Controller` lebt hier; alle Geraetezugriffe; Signale (queued) zur GUI |

Geteilter Zustand nur hinter `std::mutex` (Session) bzw. `qc::Locked<T>`
(Logging). Callbacks (Listener/EventSink) laufen auf dem RX-Thread und
duerfen nicht blockieren; die GUI marshallt sie per `QMetaObject::invokeMethod`.

## Korrelation (warum nicht "Antwort auf Anfrage")

Das Geraet pusht nach Typ. Regeln (Belege in `protocol.md`):

- Antwort auf einen READ traegt oft **keine** `request_id` → erster Warter
  desselben Typs gewinnt.
- Zustandsaenderungen loesen eine Kaskade anderer Typen aus, die alle die
  `request_id` echoen → Korrelation zuerst nach Typ, `request_id` als Filter.
- Der `RecallPreset`-Push eines Host-Recalls echot die id; der Seed-Push nach
  dem Connect nicht → `read_preset` akzeptiert nur den Push mit seiner id.
- Ordnerlisten kommen als Flut (~400 Pushes auf einen `File{READ}`) → `collect`.

## Korrektheit und Sicherheit

- Jede Wire-Shape in `device.cpp` ist durch die Offline-Tests (`tests/test_device.cpp`)
  byte-genau festgeschrieben und in `protocol.md` mit dem Beleg des jeweiligen
  Referenzprojekts (Capture bzw. Hardware-Readback) versehen.
- Speicher: keine rohen `new`/`delete` (zlib-Kontexte per `unique_ptr`
  mit Deleter), `std::expected` statt Exceptions, `unsafe{}`-Marker an allen
  FFI-Stellen (`hid_transport.cpp`, `compression.cpp`, `catalog.cpp`).
- Grenzen: Reassembly-Puffer 1 MiB, Inflate 64 MiB, jede Nachricht wird
  defensiv geparst; Timeouts statt Vertrauen in Write-Fehler (der STALL).
- Kommunikation: ausschliesslich USB (lokal); kein Netzzugriff.
- MCP-Server standardmaessig nur lesend; schreibende Tools erst mit
  `--allow-writes`. Backups nur in einem festen Verzeichnis, Namen statt
  Pfade (siehe `docs/mcp-backup-store.md`).
