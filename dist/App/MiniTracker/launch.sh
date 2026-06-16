#!/bin/sh
# Mini Tracker — GUI launcher. OnionOS suspends MainUI when this runs from the
# Apps menu, so the framebuffer + input are ours. Snapshots the OnionOS play
# data READ-ONLY, refreshes the library (preserving status), then opens the GUI.
DIR="$(cd "$(dirname "$0")" && pwd)"
chmod +x "$DIR/minitracker" 2>/dev/null

ONION_DB="$(ls /mnt/SDCARD/Saves/*/play_activity/play_activity_db.sqlite 2>/dev/null | head -1)"
if [ -n "$ONION_DB" ]; then
    PROFILE_DIR="$(dirname "$(dirname "$ONION_DB")")"
    DATADIR="$PROFILE_DIR/MiniTracker"
else
    DATADIR="/mnt/SDCARD/Saves/CurrentProfile/MiniTracker"
fi
mkdir -p "$DATADIR"
SNAP="$DATADIR/onion_snapshot.sqlite"
TRACKER_DB="$DATADIR/tracker.sqlite"
[ -n "$ONION_DB" ] && cp -f "$ONION_DB" "$SNAP" 2>/dev/null

rm -f /tmp/mt_cmd
"$DIR/minitracker" "$TRACKER_DB" "$SNAP" "/mnt/SDCARD/Roms"

# If the user picked a game to launch, Mini Tracker wrote the launch line (in
# OnionOS cmd_to_run format) to /tmp/mt_cmd. Hand off through OnionOS's own game
# pipeline so it registers in recents / GameSwitcher / play-activity: move it to
# cmd_to_run.sh and touch quick_switch so runtime.sh's loop launches it.
if [ -f /tmp/mt_cmd ]; then
    cp -f /tmp/mt_cmd /mnt/SDCARD/.tmp_update/cmd_to_run.sh
    rm -f /tmp/mt_cmd
    touch /tmp/quick_switch
fi
exit 0
