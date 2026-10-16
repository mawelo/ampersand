# `qc::io::write_private_atomic` — privat und atomar schreiben

**Einheit:** `include/qc/file_write.hpp` (header-only) ·
**Nutzer:** `qcctl backup-save`, GUI-Sicherung (`Controller::saveBackup`)

## Zweck

Bytes in eine frei gewählte Datei schreiben, nur für den Besitzer lesbar
(0600), ohne eine schon vorhandene Datei bei einem Schreibfehler zu verlieren.
Single Source of Truth für das Sichern wählbarer Pfade (CLI und GUI nutzen
denselben Code).

## Ablauf-Logik

1. In eine Nachbardatei `<ziel>.tmp.<pid>.<n>` schreiben, geöffnet mit
   `O_CREAT|O_EXCL|O_NOFOLLOW|O_CLOEXEC`, Modus 0600.
2. `fchmod(0600)` — umask-unabhängig den Modus festlegen.
3. Nutzlast schreiben (EINTR wird wiederholt), dann `fsync`.
4. `rename(tmp, ziel)` — ersetzt das Ziel atomar.
5. Bei jedem Fehler: `unlink(tmp)` und ein `std::unexpected` mit Begründung;
   das Ziel bleibt unberührt.

## Korrektheit

- **Atomarität:** `rename` innerhalb desselben Verzeichnisses ist atomar; ein
  Leser sieht entweder die alte oder die vollständige neue Datei, nie eine
  halb geschriebene.
- **Verlustfreiheit:** Das Ziel wird erst im letzten Schritt angefasst. Bricht
  Schritt 1–3 ab, bleibt die alte Sicherung erhalten (Gegenbeleg zum früheren
  `O_TRUNC`, das sie sofort zerstörte — Re-Review 2026-10-15).
- **Eindeutigkeit des Temp-Namens:** `pid` + prozesslokaler Zähler; `O_EXCL`
  bricht ab, falls der Name wider Erwarten existiert.

## Sicherheit

- **0600:** Nutzerdaten (Presets/Captures/IRs in einem LocalBackup) sind nur
  für den Besitzer lesbar.
- **`O_NOFOLLOW` auf der Temp-Datei:** kein Folgen eines untergeschobenen
  Symlinks am Temp-Namen.
- Das Ziel selbst darf ein Symlink sein; `rename` ersetzt den Namen durch eine
  reguläre Datei — gewolltes Verhalten für eine Sicherung.

## Tests

`tests/test_file_write.cpp`: Rundlauf + Modus 0600, atomares Ersetzen ohne
Temp-Reste, und — als Nicht-Root — Erhalt des Originals, wenn die Temp-Datei
nicht angelegt werden kann (nur-lesbares Verzeichnis). Teil der `ctest`-Suite.
