#!/usr/bin/env bash
# ampersand.sh — Aufrufer der Ampersand-GUI
#
# Wer:      Martin.Lonkwitz
# Was:      Startet die GUI. Nimmt von den bekannten Ablagen die JUENGSTE
#           Datei, nicht die erste in einer festen Reihenfolge. Ohne Argument
#           wird mit --connect gestartet (Verbindung beim Start); weitere
#           Argumente werden durchgereicht.
#           Grund fuer die Aenderung: die feste Reihenfolge bevorzugte
#           ~/.local/bin und startete damit stillschweigend einen veralteten
#           Stand, waehrend im Build-Baum ein neuerer lag. Beim Start wird
#           jetzt ausserdem die Version gemeldet, damit so etwas auffaellt.
# Wann:     2026-09-04
# Projekt:  Ampersand (SUB-PROJECT FROM: Quad-Cortex)
# Nutzung:  ampersand.sh [weitere GUI-Optionen]
#           ampersand.sh --no-connect   # ohne Auto-Connect starten
# Historie: git log im Projekt-Repo
set -euo pipefail

BIN=""
newest=0
for c in \
    "$HOME/.local/bin/ampersand" \
    "$HOME/src/neural_quadcortex/cortex-control-linux/build/default/src/gui/ampersand"; do
    if [[ -x "$c" ]]; then
        m=$(stat -c %Y "$c")
        if (( m > newest )); then newest=$m; BIN="$c"; fi
    fi
done
if [[ -z "$BIN" ]]; then
    echo "ampersand.sh: GUI-Binary nicht gefunden (weder ~/.local/bin noch build/default)" >&2
    exit 1
fi

# Standard: mit Auto-Connect starten. --no-connect unterdrueckt das.
args=()
connect=1
for a in "$@"; do
    case "$a" in
        --no-connect) connect=0 ;;
        --connect)    connect=1 ;;   # explizit erlaubt, nicht doppelt setzen
        *)            args+=("$a") ;;
    esac
done
[[ $connect -eq 1 ]] && args=(--connect "${args[@]}")

# Beim Start Pfad und Version melden, damit ein veralteter Stand auffaellt.
# Nicht aber, wenn ohnehin danach gefragt wird - sonst stuende die Version
# zweimal da.
banner=1
for a in "$@"; do
    case "$a" in --version|-v|--help|-h) banner=0 ;; esac
done
if [[ $banner -eq 1 ]]; then
    echo "ampersand.sh: $BIN" >&2
    "$BIN" --version >&2 || true
fi

exec "$BIN" "${args[@]}"
