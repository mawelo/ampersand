# BackupStore (`src/mcp/backup_store.{hpp,cpp}`)

## Zweck

Ablage fuer `qc_backup_save` und `qc_backup_restore` im MCP-Server. Ein
MCP-Client ist meist ein Sprachmodell, dessen Eingaben aus fremden Texten
stammen koennen (Prompt-Injection). Deshalb bestimmt das Tool-Argument nur
einen **Dateinamen**, nie einen Pfad.

## Ablauf

- Verzeichnis: `$XDG_DATA_HOME/ampersand/backups`, sonst
  `~/.local/share/ampersand/backups`; `qc-mcp --backup-dir <absolut>`
  uebersteuert.
- Name: 1 bis 128 Zeichen aus `A-Z a-z 0-9 . _ -`, erstes Zeichen
  alphanumerisch. Damit sind `..`, versteckte Dateien und Pfadtrenner
  ausgeschlossen.
- `save`: legt das Verzeichnis mit 0700 an, oeffnet es mit `O_NOFOLLOW`,
  verlangt Besitz durch den aufrufenden Nutzer und keine Schreibrechte fuer
  Gruppe/andere. Datei per `openat(..., O_CREAT|O_EXCL|O_NOFOLLOW, 0600)`;
  vorhandene Datei = Fehler. Nach Schreibfehler wird die Teildatei entfernt.
- `load`: gleiche Verzeichnispruefung, `openat(..., O_NOFOLLOW|O_NONBLOCK)`,
  nur regulaere Dateien, hoechstens 64 MiB (gemessenes Backup: 211 016
  Bytes); die Grenze wird auch beim Lesen geprueft.
- `qc_backup_restore` prueft zusaetzlich, dass der Inhalt gueltiges JSON ist.

## Korrektheit

`tests/test_backup_store.cpp` (6 Faelle): Namenspruefung, Rechte 0700/0600,
kein Ueberschreiben, Symlink an Dateiname und Verzeichnis, Verzeichnis mit
0777, FIFO, Unterverzeichnis, Groessengrenze (genau an und eins darueber).

## Sicherheit / Grenzen

- Beide Backup-Tools sind schreibend (`--allow-writes` noetig): `save`
  schreibt eine lokale Datei, `restore` ueberschreibt Geraeteinhalte.
- Nicht abgedeckt: Symlinks in den **uebergeordneten** Verzeichnissen des
  Ablageorts (der Nutzer waehlt den Ort selbst).
