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

rm -f /tmp/mt_cmd /tmp/mt_recent /tmp/mt_recent_key
"$DIR/minitracker" "$TRACKER_DB" "$SNAP" "/mnt/SDCARD/Roms"

# If the user picked a game to launch, Mini Tracker wrote the launch line (in
# OnionOS cmd_to_run format) to /tmp/mt_cmd. Hand off through OnionOS's own game
# pipeline so it registers in recents / GameSwitcher / play-activity: move it to
# cmd_to_run.sh and touch quick_switch so runtime.sh's loop launches it.
if [ -f /tmp/mt_cmd ]; then
    cp -f /tmp/mt_cmd /mnt/SDCARD/.tmp_update/cmd_to_run.sh
    rm -f /tmp/mt_cmd

    # MainUI (which we bypass) is what normally writes the recent list that
    # GameSwitcher reads. Merge the game's recent entry ourselves so it gets a
    # GameSwitcher card + resume screenshot, mirroring a native launch. The
    # newest entry is line 1; we dedupe the game's old entry, prepend, cap, and
    # write atomically via a temp file. Strictly additive: any failure here
    # still leaves the game launching below.
    if [ -f /tmp/mt_recent ] && [ -s /tmp/mt_recent_key ]; then
        if [ -f /mnt/SDCARD/.tmp_update/config/.showRecents ]; then
            RL=/mnt/SDCARD/Roms/recentlist.json
        else
            RL=/mnt/SDCARD/Roms/recentlist-hidden.json
        fi
        KEY="$(cat /tmp/mt_recent_key)"
        TMP=/tmp/mt_recent_merge.json
        {
            cat /tmp/mt_recent
            # drop any prior entry for this exact rompath (KEY is non-empty: guarded above)
            [ -f "$RL" ] && grep -vF "$KEY" "$RL"
        } | head -n 40 > "$TMP"
        # only commit if we produced a non-empty file
        [ -s "$TMP" ] && mv -f "$TMP" "$RL"
        rm -f "$TMP"
        sync
    fi
    rm -f /tmp/mt_recent /tmp/mt_recent_key

    touch /tmp/quick_switch
fi
exit 0
