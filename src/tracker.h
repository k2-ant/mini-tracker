/* tracker.h — portable data/stats core for the Game Tracker app.
 *
 * UI-agnostic: the same code backs the shell MVP and the SDL2 GUI.
 * Reads OnionOS's play-activity DB (READ-ONLY) and owns a separate,
 * writable tracker.sqlite for status/ratings. Never writes OnionOS data.
 */
#ifndef TRACKER_H
#define TRACKER_H

#include <sqlite3.h>

/* Sessions older than this epoch are treated as junk timestamps
 * (Kyle's data has a 1970 row). 2010-01-01 UTC. */
#define TRK_MIN_VALID_EPOCH 1262304000

/* Status values stored in tracker.sqlite. Absence of a row == backlog. */
#define TRK_BACKLOG   "backlog"
#define TRK_PLAYING   "playing"
#define TRK_BEATEN    "beaten"
#define TRK_ABANDONED "abandoned"

typedef struct {
    sqlite3 *db;        /* our writable tracker.sqlite */
} Tracker;

/* Open (creating + migrating schema if needed) our tracker DB. Returns 0 on success. */
int  trk_open(Tracker *t, const char *tracker_db_path);
void trk_close(Tracker *t);

/* Scan /Roms/<SYSTEM>/ folders and upsert a game row per ROM file (full
 * library incl. never-played games). Metadata only; preserves play_secs.
 * *out_count = number of ROM files seen. Returns 0 on success. */
int  trk_scan_roms(Tracker *t, const char *roms_dir, int *out_count);

/* Import/refresh game rows from an OnionOS play-activity DB (opened READ-ONLY).
 * Upserts game metadata + aggregated playtime; never touches the status table.
 * Rows whose ROM file is missing under roms_dir (fossils from renamed/removed
 * games) are skipped. roms_dir may be NULL to disable the existence check.
 * On success, *out_count = number of games imported. Returns 0 on success. */
int  trk_import_onion(Tracker *t, const char *onion_db_path,
                      const char *roms_dir, int *out_count);

/* Set status / rating for a game (by relative rom_path, e.g. "GBA/Advance Wars.gba"). */
int  trk_set_status(Tracker *t, const char *rom_path, const char *status, long now_epoch);
int  trk_set_rating(Tracker *t, const char *rom_path, int rating /*tier 1-6 (F..S)*/, long now_epoch);

/* Hide/unhide a game (1 = hidden from all views). */
int  trk_set_hidden(Tracker *t, const char *rom_path, int hidden, long now_epoch);

/* Delete game rows whose ROM file is gone (skipped if roms_dir isn't a live dir). */
int  trk_prune_missing(Tracker *t, const char *roms_dir);

/* Persisted app settings (stored in the meta table). */
int  trk_get_setting(Tracker *t, const char *key, int def);
void trk_set_setting(Tracker *t, const char *key, int value);

/* Print a stats dashboard (overall + per-console) to stdout. */
int  trk_print_stats(Tracker *t);

/* Print the game list (optionally filtered by console) to stdout. */
int  trk_print_list(Tracker *t, const char *console_or_null);

/* Run the interactive line-based menu (Phase 2 on-device UI). */
int  trk_run_menu(Tracker *t);

#endif /* TRACKER_H */
