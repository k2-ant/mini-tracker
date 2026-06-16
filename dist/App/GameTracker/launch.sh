#!/bin/sh
# Game Tracker — entry point. OnionOS runs this when you open the app.
# Opens the OnionOS terminal (st) and runs the interactive menu inside it.
# If no terminal is found, falls back to writing a read-only stats report.

DIR="$(cd "$(dirname "$0")" && pwd)"
cd "$DIR"

if command -v st >/dev/null 2>&1; then
    st -q -e sh "$DIR/run.sh"
else
    sh "$DIR/run.sh" --report > "$DIR/STATS_REPORT.txt" 2>&1
fi
