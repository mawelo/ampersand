#!/bin/bash
# qc_referenz_bauen.sh - docs/ampersand/reference/commands.md aus der Quelle bauen.
#
# WER   Projekt Ampersand (Quad-Cortex).
# WAS   Erzeugt das Referenz-Dokument aus der --help-Ausgabe von qcctl und
#       ampersand, plus den handgepflegten Teilen (Portnummern, Werkzeugtabelle).
# WARUM Projektregel fuer Dokumentation: "Reference aus der Quelle - aus --help,
#       Code oder Schema erzeugen." Referenzdokumente veralten sonst am
#       schnellsten; es stehen dann Optionen darin, die es nicht mehr gibt.
# NUTZUNG  tools/qc_referenz_bauen.sh          (nach jeder Aenderung an Befehlen)
#          QC_BUILD=build/nogui tools/qc_referenz_bauen.sh
#
# ABWEICHUNG VON rules/code.md: Shell statt Perl, trotz ueber 20 Zeilen. Das
# Skript ist eine Aneinanderreihung von Textbloecken ohne Ablauflogik; in Perl
# waere es dasselbe mit mehr Rahmen.
set -eu
cd "$(dirname "$0")/.."
BAU=${QC_BUILD:-build/default}
ZIEL=docs/ampersand/reference/commands.md
[ -x "$BAU/qcctl" ] || { echo "nicht gebaut: $BAU/qcctl" >&2; exit 1; }
[ -x "$BAU/src/gui/ampersand" ] || { echo "nicht gebaut: $BAU/src/gui/ampersand" >&2; exit 1; }

# Die handgepflegten Teile stehen in einer eigenen Datei, damit sie ein Lauf
# nicht ueberschreibt.
TEIL=docs/ampersand/reference/commands-handteil.md
[ -f "$TEIL" ] || { echo "fehlt: $TEIL" >&2; exit 1; }

{
    sed -n '/^## Port numbers/,$p' "$TEIL" > /dev/null   # Vorhandensein pruefen
    sed -n '1,/^## qcctl$/p' "$TEIL"
    echo '```'
    "$BAU/qcctl" --help 2>&1
    printf '```\n\n## ampersand\n\n```\n'
    "$BAU/src/gui/ampersand" --help 2>&1
    echo '```'
    echo
    sed -n '/^## Port numbers/,$p' "$TEIL"
} > "$ZIEL"

echo "$ZIEL: $(wc -l < "$ZIEL") Zeilen aus $BAU erzeugt"
