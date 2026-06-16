#!/bin/sh
# Game Tracker — runs inside the terminal.
#  1. Finds the OnionOS play-activity DB (under whichever profile exists).
#  2. Copies it READ-ONLY to a snapshot (we never open the live DB).
#  3. Refreshes our library from the snapshot, PRESERVING your status marks.
#  4. Opens the interactive menu.
#
# SAFETY: this script only ever READS OnionOS data and WRITES inside its own
# GameTracker data folder. It never modifies firmware, ROMs, saves, or the
# OnionOS play-activity database. Uninstall = delete the App/GameTracker folder.

DIR="$(cd "$(dirname "$0")" && pwd)"
BIN="$DIR/tracker"
chmod +x "$BIN" 2>/dev/null

# Locate the play-activity DB under any profile (CurrentProfile, GuestProfile, ...)
ONION_DB="$(ls /mnt/SDCARD/Saves/*/play_activity/play_activity_db.sqlite 2>/dev/null | head -1)"

if [ -n "$ONION_DB" ]; then
    PROFILE_DIR="$(dirname "$(dirname "$ONION_DB")")"
    DATADIR="$PROFILE_DIR/GameTracker"
else
    DATADIR="/mnt/SDCARD/Saves/CurrentProfile/GameTracker"
fi
mkdir -p "$DATADIR"
TRACKER_DB="$DATADIR/tracker.sqlite"
SNAP="$DATADIR/onion_snapshot.sqlite"

echo "Game Tracker"
echo "============"
if [ -n "$ONION_DB" ]; then
    cp -f "$ONION_DB" "$SNAP" 2>/dev/null      # read a copy, never the live DB
    "$BIN" import "$SNAP" "$TRACKER_DB"
else
    echo "(!) OnionOS play-activity data not found yet."
    echo "    Play a game or two, then reopen Game Tracker."
fi

# Non-interactive report mode (used when no terminal is available).
if [ "$1" = "--report" ]; then
    "$BIN" stats "$TRACKER_DB"
    exit 0
fi

"$BIN" menu "$TRACKER_DB"

echo ""
echo "Press SELECT to exit."
