/* tracker.c — see tracker.h for the contract. */
#define _DEFAULT_SOURCE 1   /* declare strdup/localtime_r under any -std */
#include "tracker.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <ctype.h>
#include <dirent.h>
#include <sys/stat.h>

/* tracker.sqlite schema. game = imported facts; status = the data we own. */
static const char *SCHEMA_SQL =
    "PRAGMA foreign_keys=ON;"
    "CREATE TABLE IF NOT EXISTS meta ("
    "  key TEXT PRIMARY KEY, value TEXT);"
    "CREATE TABLE IF NOT EXISTS game ("
    "  rom_path     TEXT PRIMARY KEY,"   /* relative file_path, e.g. 'GBA/Advance Wars.gba' */
    "  console      TEXT NOT NULL,"      /* first path segment, e.g. 'GBA' */
    "  name         TEXT NOT NULL,"
    "  image_path   TEXT,"
    "  play_secs    INTEGER NOT NULL DEFAULT 0,"
    "  play_count   INTEGER NOT NULL DEFAULT 0,"
    "  first_played INTEGER,"            /* epoch; NULL if unknown/filtered */
    "  last_played  INTEGER,"
    "  imported_at  INTEGER);"
    "CREATE TABLE IF NOT EXISTS status ("
    "  rom_path   TEXT PRIMARY KEY REFERENCES game(rom_path) ON DELETE CASCADE,"
    "  status     TEXT NOT NULL DEFAULT 'backlog',"   /* backlog|playing|beaten|abandoned */
    "  rating     INTEGER,"                            /* tier 1-6 (F..S), NULL = unset */
    "  beaten_at  INTEGER,"
    "  hidden     INTEGER NOT NULL DEFAULT 0,"         /* 1 = hidden from all views */
    "  updated_at INTEGER);"
    "INSERT OR IGNORE INTO meta(key,value) VALUES('schema_version','1');";

static int exec_or_warn(sqlite3 *db, const char *sql) {
    char *err = NULL;
    if (sqlite3_exec(db, sql, NULL, NULL, &err) != SQLITE_OK) {
        fprintf(stderr, "sqlite error: %s\n", err ? err : "(unknown)");
        sqlite3_free(err);
        return -1;
    }
    return 0;
}

int trk_open(Tracker *t, const char *tracker_db_path) {
    t->db = NULL;
    if (sqlite3_open_v2(tracker_db_path, &t->db,
                        SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE, NULL) != SQLITE_OK) {
        fprintf(stderr, "cannot open tracker db '%s': %s\n",
                tracker_db_path, t->db ? sqlite3_errmsg(t->db) : "(nomem)");
        return -1;
    }
    if (exec_or_warn(t->db, SCHEMA_SQL) != 0) return -1;
    /* Migration: add 'hidden' to status tables created before it existed.
     * Fails harmlessly (duplicate column) on already-migrated DBs. */
    sqlite3_exec(t->db, "ALTER TABLE status ADD COLUMN hidden INTEGER NOT NULL DEFAULT 0;",
                 NULL, NULL, NULL);
    return 0;
}

void trk_close(Tracker *t) {
    if (t && t->db) { sqlite3_close(t->db); t->db = NULL; }
}

/* console = substring of rom_path before the first '/'. */
static void derive_console(const char *rom_path, char *out, size_t outsz) {
    const char *slash = strchr(rom_path, '/');
    size_t n = slash ? (size_t)(slash - rom_path) : strlen(rom_path);
    if (n >= outsz) n = outsz - 1;
    memcpy(out, rom_path, n);
    out[n] = '\0';
    if (n == 0) snprintf(out, outsz, "UNKNOWN");
}

static int denied_ext(const char *fn);   /* fwd: non-game extension filter */

int trk_import_onion(Tracker *t, const char *onion_db_path,
                     const char *roms_dir, int *out_count) {
    sqlite3 *onion = NULL;
    if (sqlite3_open_v2(onion_db_path, &onion, SQLITE_OPEN_READONLY, NULL) != SQLITE_OK) {
        fprintf(stderr, "cannot open OnionOS db '%s' read-only: %s\n",
                onion_db_path, onion ? sqlite3_errmsg(onion) : "(nomem)");
        return -1;
    }

    /* Aggregate playtime per rom. play_secs/count use ALL sessions; first/last
     * use only plausibly-dated sessions so a junk 1970 row can't skew dates. */
    const char *agg_sql =
        "SELECT r.file_path, r.name, r.image_path,"
        "       COALESCE(SUM(pa.play_time),0),"
        "       COUNT(pa.rom_id),"
        "       MIN(CASE WHEN pa.created_at > ?1 THEN pa.created_at END),"
        "       MAX(CASE WHEN pa.created_at > ?1 THEN pa.created_at END)"
        " FROM rom r LEFT JOIN play_activity pa ON pa.rom_id = r.id"
        " WHERE r.file_path IS NOT NULL AND r.file_path <> ''"
        " GROUP BY r.id";

    const char *upsert_sql =
        "INSERT INTO game(rom_path,console,name,image_path,play_secs,play_count,"
        "                 first_played,last_played,imported_at)"
        " VALUES(?1,?2,?3,?4,?5,?6,?7,?8,?9)"
        " ON CONFLICT(rom_path) DO UPDATE SET"
        /* keep the scan/gamelist name; only fill image if the scan found none */
        "   console=excluded.console,"
        "   image_path=CASE WHEN game.image_path IS NULL OR game.image_path=''"
        "                   THEN excluded.image_path ELSE game.image_path END,"
        "   play_secs=excluded.play_secs, play_count=excluded.play_count,"
        "   first_played=excluded.first_played, last_played=excluded.last_played,"
        "   imported_at=excluded.imported_at";

    sqlite3_stmt *sel = NULL, *up = NULL;
    int rc = -1, count = 0;
    long now = (long)time(NULL);

    if (sqlite3_prepare_v2(onion, agg_sql, -1, &sel, NULL) != SQLITE_OK) {
        fprintf(stderr, "prepare select: %s\n", sqlite3_errmsg(onion)); goto done;
    }
    if (sqlite3_prepare_v2(t->db, upsert_sql, -1, &up, NULL) != SQLITE_OK) {
        fprintf(stderr, "prepare upsert: %s\n", sqlite3_errmsg(t->db)); goto done;
    }
    sqlite3_bind_int64(sel, 1, (sqlite3_int64)TRK_MIN_VALID_EPOCH);

    exec_or_warn(t->db, "BEGIN");
    while (sqlite3_step(sel) == SQLITE_ROW) {
        const char *file_path  = (const char *)sqlite3_column_text(sel, 0);
        const char *name       = (const char *)sqlite3_column_text(sel, 1);
        const char *image_path = (const char *)sqlite3_column_text(sel, 2);
        sqlite3_int64 secs  = sqlite3_column_int64(sel, 3);
        sqlite3_int64 plays = sqlite3_column_int64(sel, 4);
        int has_first = sqlite3_column_type(sel, 5) != SQLITE_NULL;
        int has_last  = sqlite3_column_type(sel, 6) != SQLITE_NULL;
        sqlite3_int64 first = sqlite3_column_int64(sel, 5);
        sqlite3_int64 last  = sqlite3_column_int64(sel, 6);
        char console[64];
        if (!file_path) continue;
        {   /* same non-game filter as the dir scan (BIOS/.bin/.cfg/saves/etc.) */
            const char *bn = strrchr(file_path, '/'); bn = bn ? bn + 1 : file_path;
            if (denied_ext(bn)) continue;
        }
        /* Skip fossil rows whose ROM no longer exists on the card. file_path is
         * relative to the live Roms dir, e.g. "GB/Foo.gbc". */
        if (roms_dir && *roms_dir) {
            char abs[2400];
            snprintf(abs, sizeof abs, "%s/%s", roms_dir, file_path);
            struct stat sb;
            if (stat(abs, &sb) != 0) continue;
        }
        derive_console(file_path, console, sizeof console);

        sqlite3_reset(up);
        sqlite3_bind_text (up, 1, file_path, -1, SQLITE_TRANSIENT);
        sqlite3_bind_text (up, 2, console,   -1, SQLITE_TRANSIENT);
        sqlite3_bind_text (up, 3, name ? name : file_path, -1, SQLITE_TRANSIENT);
        if (image_path) sqlite3_bind_text(up, 4, image_path, -1, SQLITE_TRANSIENT);
        else            sqlite3_bind_null(up, 4);
        sqlite3_bind_int64(up, 5, secs);
        sqlite3_bind_int64(up, 6, plays);
        if (has_first) sqlite3_bind_int64(up, 7, first); else sqlite3_bind_null(up, 7);
        if (has_last)  sqlite3_bind_int64(up, 8, last);  else sqlite3_bind_null(up, 8);
        sqlite3_bind_int64(up, 9, (sqlite3_int64)now);

        if (sqlite3_step(up) != SQLITE_DONE) {
            fprintf(stderr, "upsert failed for '%s': %s\n", file_path, sqlite3_errmsg(t->db));
            exec_or_warn(t->db, "ROLLBACK");
            goto done;
        }
        count++;
    }
    exec_or_warn(t->db, "COMMIT");
    if (out_count) *out_count = count;
    rc = 0;

done:
    if (sel) sqlite3_finalize(sel);
    if (up)  sqlite3_finalize(up);
    sqlite3_close(onion);
    return rc;
}

/* Delete game rows whose ROM file no longer exists under roms_dir (status/rating
 * cascade via the FK). SAFETY: only runs if roms_dir is itself a readable
 * directory, so an unmounted/missing card can never wipe the whole library.
 * Returns the number pruned, or 0/-1 if skipped/failed. */
int trk_prune_missing(Tracker *t, const char *roms_dir) {
    if (!roms_dir || !*roms_dir) return 0;
    struct stat rb;
    if (stat(roms_dir, &rb) != 0 || !S_ISDIR(rb.st_mode)) return 0;   /* card gone -> skip */

    sqlite3_stmt *sel = NULL;
    if (sqlite3_prepare_v2(t->db, "SELECT rom_path FROM game", -1, &sel, NULL) != SQLITE_OK)
        return -1;
    char **miss = NULL; int n = 0, cap = 0, total = 0;
    while (sqlite3_step(sel) == SQLITE_ROW) {
        const char *rp = (const char *)sqlite3_column_text(sel, 0);
        if (!rp) continue;
        total++;
        char abs[2400]; snprintf(abs, sizeof abs, "%s/%s", roms_dir, rp);
        struct stat sb;
        if (stat(abs, &sb) != 0) {
            if (n == cap) { cap = cap ? cap * 2 : 64;
                            char **m2 = realloc(miss, (size_t)cap * sizeof *miss);
                            if (!m2) { for (int i = 0; i < n; i++) free(miss[i]); free(miss);
                                       sqlite3_finalize(sel); return -1; }
                            miss = m2; }
            miss[n++] = strdup(rp);
        }
    }
    sqlite3_finalize(sel);
    /* Safety: if EVERY game looks missing, the card is almost certainly half-
     * mounted (empty stub dir) — never wipe the whole library on that. */
    int do_delete = (n > 0 && !(total > 0 && n >= total));
    if (do_delete) {
        sqlite3_stmt *del = NULL;
        if (sqlite3_prepare_v2(t->db, "DELETE FROM game WHERE rom_path=?1", -1, &del, NULL) == SQLITE_OK) {
            exec_or_warn(t->db, "BEGIN");
            for (int i = 0; i < n; i++) {
                sqlite3_reset(del);
                sqlite3_bind_text(del, 1, miss[i], -1, SQLITE_TRANSIENT);
                sqlite3_step(del);
            }
            exec_or_warn(t->db, "COMMIT");
            sqlite3_finalize(del);
        }
    }
    for (int i = 0; i < n; i++) free(miss[i]);
    free(miss);
    return do_delete ? n : 0;
}

/* Extensions that are NOT games (data tracks, art, saves, metadata).
 * Notably 'bin' is denied: PS1 .bin are data for the .cue (the real entry). */
static int denied_ext(const char *fn) {
    const char *dot = strrchr(fn, '.');
    if (!dot) return 0;                       /* no extension -> keep */
    char e[16]; int i = 0;
    for (const char *p = dot + 1; *p && i < 15; p++) e[i++] = (char)tolower((unsigned char)*p);
    e[i] = 0;
    static const char *deny[] = {
        "bin","xml","db","txt","log","png","jpg","jpeg","gif","bmp","srm","sav",
        "state","sta","dat","nv","nvram","cfg","opt","bak","ips","bps","ups","ppf", NULL
    };
    for (int k = 0; deny[k]; k++) if (!strcmp(e, deny[k])) return 1;
    return 0;
}

/* Subdirectories that are NOT games (art, manuals, saves, etc.). Everything
 * else that's a directory IS a game (e.g. ScummVM stores each game as a folder). */
static int is_util_dir(const char *n) {
    static const char *u[] = {
        "Imgs","imgs","Manuals","Manual","Media","Save","Saves","Cores","System",
        "BIOS","bios","Cheats","Overlays","Box","Snaps","Videos","Themes",
        "Shortcuts","Games", NULL
    };
    for (int i = 0; u[i]; i++) if (!strcmp(n, u[i])) return 1;
    return 0;
}

/* ---- miyoogamelist.xml support (OnionOS's curated names + image paths) ---- */
/* Extract the inner text of <tag>...</tag> from a block. Returns 1 if found. */
static int xml_inner(const char *block, const char *tag, char *out, int n) {
    char open[24], close[24];
    snprintf(open, sizeof open, "<%s>", tag);
    snprintf(close, sizeof close, "</%s>", tag);
    const char *s = strstr(block, open);
    if (!s) { out[0] = 0; return 0; }
    s += strlen(open);
    const char *e = strstr(s, close);
    if (!e) { out[0] = 0; return 0; }
    int len = (int)(e - s); if (len >= n) len = n - 1; if (len < 0) len = 0;
    memcpy(out, s, len); out[len] = 0;
    return 1;
}
/* Decode the handful of XML entities gamelists use, in place. */
static void xml_decode(char *s) {
    char *d = s, *p = s;
    while (*p) {
        if (*p == '&') {
            if (!strncmp(p, "&amp;", 5))  { *d++ = '&';  p += 5; continue; }
            if (!strncmp(p, "&lt;", 4))   { *d++ = '<';  p += 4; continue; }
            if (!strncmp(p, "&gt;", 4))   { *d++ = '>';  p += 4; continue; }
            if (!strncmp(p, "&apos;", 6)) { *d++ = '\''; p += 6; continue; }
            if (!strncmp(p, "&#39;", 5))  { *d++ = '\''; p += 5; continue; }
            if (!strncmp(p, "&quot;", 6)) { *d++ = '"';  p += 6; continue; }
        }
        *d++ = *p++;
    }
    *d = 0;
}
static const char *strip_dotslash(const char *p) {
    if (p[0] == '.' && p[1] == '/') return p + 2;
    return p;
}
/* Parse <sysdir>/miyoogamelist.xml and upsert each listed game (curated name +
 * image). Returns the number of games upserted (0 if absent/empty/unparsable,
 * so the caller can fall back to a plain directory scan). */
static int scan_gamelist(sqlite3_stmt *up, const char *console,
                         const char *sysdir, long now) {
    char path[1300];
    snprintf(path, sizeof path, "%s/miyoogamelist.xml", sysdir);
    FILE *f = fopen(path, "r");
    if (!f) return 0;
    fseek(f, 0, SEEK_END); long sz = ftell(f); fseek(f, 0, SEEK_SET);
    if (sz <= 0 || sz > 16 * 1024 * 1024) { fclose(f); return 0; }
    char *buf = malloc((size_t)sz + 1);
    if (!buf) { fclose(f); return 0; }
    size_t rd = fread(buf, 1, (size_t)sz, f); buf[rd] = 0; fclose(f);

    int count = 0;
    char *p = buf;
    while ((p = strstr(p, "<game>")) != NULL) {
        char *gend = strstr(p, "</game>");
        if (!gend) break;
        char saved = *gend; *gend = 0;                  /* limit tag search to this block */
        char rel[420] = {0}, name[300] = {0}, img[420] = {0};
        xml_inner(p, "path", rel, sizeof rel);
        xml_inner(p, "name", name, sizeof name);
        xml_inner(p, "image", img, sizeof img);
        *gend = saved; p = gend + 7;
        if (!rel[0]) continue;
        xml_decode(rel); xml_decode(name); xml_decode(img);

        const char *r = strip_dotslash(rel);
        char abs[1700];
        snprintf(abs, sizeof abs, "%s/%s", sysdir, r);
        struct stat sb;
        if (stat(abs, &sb) != 0) continue;              /* skip entries whose file is gone */

        char rom_path[520];
        snprintf(rom_path, sizeof rom_path, "%s/%s", console, r);
        char imgfull[1700] = {0};
        if (img[0]) {                                   /* explicit <image> in the gamelist */
            snprintf(imgfull, sizeof imgfull, "%s/%s", sysdir, strip_dotslash(img));
            struct stat ib; if (stat(imgfull, &ib) != 0) imgfull[0] = 0;
        }
        if (!imgfull[0]) {                              /* fallback: Imgs/<rom-base>.png */
            const char *slash = strrchr(r, '/');
            char base[420]; snprintf(base, sizeof base, "%s", slash ? slash + 1 : r);
            char *dot = strrchr(base, '.'); if (dot) *dot = 0;
            char cand[1700]; snprintf(cand, sizeof cand, "%s/Imgs/%s.png", sysdir, base);
            struct stat ib; if (stat(cand, &ib) == 0) snprintf(imgfull, sizeof imgfull, "%s", cand);
        }
        const char *disp = name[0] ? name : r;
        sqlite3_reset(up);
        sqlite3_bind_text (up, 1, rom_path, -1, SQLITE_TRANSIENT);
        sqlite3_bind_text (up, 2, console,  -1, SQLITE_TRANSIENT);
        sqlite3_bind_text (up, 3, disp,     -1, SQLITE_TRANSIENT);
        sqlite3_bind_text (up, 4, imgfull,  -1, SQLITE_TRANSIENT);
        sqlite3_bind_int64(up, 5, (sqlite3_int64)now);
        if (sqlite3_step(up) == SQLITE_DONE) count++;
    }
    free(buf);
    return count;
}

/* Scan /Roms/<SYSTEM>/<files> and upsert a game row per ROM (metadata only,
 * preserving any existing play_secs). This populates the FULL library,
 * including never-played games; trk_import_onion then overlays playtime.
 * If a system has a miyoogamelist.xml, it is used as the source of truth. */
int trk_scan_roms(Tracker *t, const char *roms_dir, int *out_count) {
    DIR *d = opendir(roms_dir);
    if (!d) return -1;
    const char *up =
        "INSERT INTO game(rom_path,console,name,image_path,play_secs,play_count,imported_at)"
        " VALUES(?1,?2,?3,?4,0,0,?5)"
        " ON CONFLICT(rom_path) DO UPDATE SET"
        "   console=excluded.console, name=excluded.name,"
        "   image_path=CASE WHEN excluded.image_path<>'' THEN excluded.image_path ELSE game.image_path END,"
        "   imported_at=excluded.imported_at";
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(t->db, up, -1, &st, NULL) != SQLITE_OK) { closedir(d); return -1; }
    long now = (long)time(NULL);
    int count = 0;
    exec_or_warn(t->db, "BEGIN");
    struct dirent *e;
    while ((e = readdir(d)) != NULL) {
        if (e->d_name[0] == '.') continue;
        char sysdir[1200];
        snprintf(sysdir, sizeof sysdir, "%s/%s", roms_dir, e->d_name);
        struct stat sb;
        if (stat(sysdir, &sb) != 0 || !S_ISDIR(sb.st_mode)) continue;
        const char *console = e->d_name;
        /* Prefer the curated gamelist (matches what MainUI shows). */
        int got = scan_gamelist(st, console, sysdir, now);
        if (got > 0) { count += got; continue; }
        DIR *sd = opendir(sysdir);
        if (!sd) continue;
        struct dirent *f;
        while ((f = readdir(sd)) != NULL) {
            if (f->d_name[0] == '.') continue;
            char fpath[2400];
            snprintf(fpath, sizeof fpath, "%s/%s", sysdir, f->d_name);
            struct stat fb;
            if (stat(fpath, &fb) != 0) continue;
            char name[200];
            if (S_ISDIR(fb.st_mode)) {                  /* subdir = a game (ScummVM etc.) */
                if (is_util_dir(f->d_name)) continue;
                snprintf(name, sizeof name, "%s", f->d_name);
            } else {                                    /* file = a game (normal ROMs) */
                if (denied_ext(f->d_name)) continue;
                snprintf(name, sizeof name, "%s", f->d_name);
                char *dot = strrchr(name, '.'); if (dot) *dot = 0;
            }
            char rom_path[420];
            snprintf(rom_path, sizeof rom_path, "%s/%s", console, f->d_name);
            char img[2600];
            snprintf(img, sizeof img, "%s/%s/Imgs/%s.png", roms_dir, console, name);
            struct stat ib;
            const char *imgp = (stat(img, &ib) == 0) ? img : "";
            sqlite3_reset(st);
            sqlite3_bind_text (st, 1, rom_path, -1, SQLITE_TRANSIENT);
            sqlite3_bind_text (st, 2, console,  -1, SQLITE_TRANSIENT);
            sqlite3_bind_text (st, 3, name,     -1, SQLITE_TRANSIENT);
            sqlite3_bind_text (st, 4, imgp,     -1, SQLITE_TRANSIENT);
            sqlite3_bind_int64(st, 5, (sqlite3_int64)now);
            if (sqlite3_step(st) == SQLITE_DONE) count++;
        }
        closedir(sd);
    }
    exec_or_warn(t->db, "COMMIT");
    sqlite3_finalize(st);
    closedir(d);
    if (out_count) *out_count = count;
    return 0;
}

int trk_set_status(Tracker *t, const char *rom_path, const char *status, long now_epoch) {
    const char *sql =
        "INSERT INTO status(rom_path,status,beaten_at,updated_at) VALUES(?1,?2,?3,?4)"
        " ON CONFLICT(rom_path) DO UPDATE SET"
        "   status=excluded.status,"
        "   beaten_at=CASE WHEN excluded.status='beaten' AND status.beaten_at IS NULL"
        "                  THEN ?3 ELSE status.beaten_at END,"
        "   updated_at=excluded.updated_at";
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(t->db, sql, -1, &st, NULL) != SQLITE_OK) return -1;
    sqlite3_bind_text (st, 1, rom_path, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text (st, 2, status,   -1, SQLITE_TRANSIENT);
    if (strcmp(status, TRK_BEATEN) == 0) sqlite3_bind_int64(st, 3, (sqlite3_int64)now_epoch);
    else                                  sqlite3_bind_null (st, 3);
    sqlite3_bind_int64(st, 4, (sqlite3_int64)now_epoch);
    int ok = sqlite3_step(st) == SQLITE_DONE;
    sqlite3_finalize(st);
    return ok ? 0 : -1;
}

int trk_set_hidden(Tracker *t, const char *rom_path, int hidden, long now_epoch) {
    const char *sql =
        "INSERT INTO status(rom_path,hidden,updated_at) VALUES(?1,?2,?3)"
        " ON CONFLICT(rom_path) DO UPDATE SET hidden=excluded.hidden,"
        "   updated_at=excluded.updated_at";
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(t->db, sql, -1, &st, NULL) != SQLITE_OK) return -1;
    sqlite3_bind_text (st, 1, rom_path, -1, SQLITE_TRANSIENT);
    sqlite3_bind_int  (st, 2, hidden ? 1 : 0);
    sqlite3_bind_int64(st, 3, (sqlite3_int64)now_epoch);
    int ok = sqlite3_step(st) == SQLITE_DONE;
    sqlite3_finalize(st);
    return ok ? 0 : -1;
}

int trk_get_setting(Tracker *t, const char *key, int def) {
    sqlite3_stmt *st = NULL;
    int v = def;
    if (sqlite3_prepare_v2(t->db, "SELECT value FROM meta WHERE key=?1", -1, &st, NULL) != SQLITE_OK)
        return def;
    sqlite3_bind_text(st, 1, key, -1, SQLITE_TRANSIENT);
    if (sqlite3_step(st) == SQLITE_ROW) {
        const char *s = (const char *)sqlite3_column_text(st, 0);
        if (s) v = atoi(s);
    }
    sqlite3_finalize(st);
    return v;
}

void trk_set_setting(Tracker *t, const char *key, int value) {
    sqlite3_stmt *st = NULL;
    char buf[16];
    snprintf(buf, sizeof buf, "%d", value);
    if (sqlite3_prepare_v2(t->db,
            "INSERT INTO meta(key,value) VALUES(?1,?2)"
            " ON CONFLICT(key) DO UPDATE SET value=excluded.value", -1, &st, NULL) != SQLITE_OK)
        return;
    sqlite3_bind_text(st, 1, key, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st, 2, buf, -1, SQLITE_TRANSIENT);
    sqlite3_step(st);
    sqlite3_finalize(st);
}

int trk_set_rating(Tracker *t, const char *rom_path, int rating, long now_epoch) {
    const char *sql =
        "INSERT INTO status(rom_path,rating,updated_at) VALUES(?1,?2,?3)"
        " ON CONFLICT(rom_path) DO UPDATE SET rating=excluded.rating, updated_at=excluded.updated_at";
    sqlite3_stmt *st = NULL;
    if (rating < 1 || rating > 6) return -1;   /* 6 tiers: 1=F .. 6=S */
    if (sqlite3_prepare_v2(t->db, sql, -1, &st, NULL) != SQLITE_OK) return -1;
    sqlite3_bind_text (st, 1, rom_path, -1, SQLITE_TRANSIENT);
    sqlite3_bind_int  (st, 2, rating);
    sqlite3_bind_int64(st, 3, (sqlite3_int64)now_epoch);
    int ok = sqlite3_step(st) == SQLITE_DONE;
    sqlite3_finalize(st);
    return ok ? 0 : -1;
}

int trk_print_stats(Tracker *t) {
    /* Overall line. */
    const char *overall_sql =
        "SELECT COUNT(*),"
        "  SUM(CASE WHEN COALESCE(s.status,'backlog')='beaten'    THEN 1 ELSE 0 END),"
        "  SUM(CASE WHEN COALESCE(s.status,'backlog')='playing'   THEN 1 ELSE 0 END),"
        "  SUM(CASE WHEN COALESCE(s.status,'backlog')='abandoned' THEN 1 ELSE 0 END),"
        "  ROUND(SUM(g.play_secs)/3600.0,1)"
        " FROM game g LEFT JOIN status s ON s.rom_path=g.rom_path";
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(t->db, overall_sql, -1, &st, NULL) != SQLITE_OK) return -1;
    if (sqlite3_step(st) == SQLITE_ROW) {
        int total = sqlite3_column_int(st, 0);
        int beaten = sqlite3_column_int(st, 1);
        int playing = sqlite3_column_int(st, 2);
        int abandoned = sqlite3_column_int(st, 3);
        double hours = sqlite3_column_double(st, 4);
        int backlog = total - beaten - playing - abandoned;
        double pct = total ? (100.0 * beaten / total) : 0.0;
        printf("\n=== Library ===\n");
        printf("%d games  |  %d beaten (%.0f%%)  |  %d playing  |  %d backlog  |  %d abandoned\n",
               total, beaten, pct, playing, backlog, abandoned);
        printf("Total tracked playtime: %.1f h\n", hours);
    }
    sqlite3_finalize(st);

    /* Per-console table. */
    const char *per_sql =
        "SELECT g.console, COUNT(*),"
        "  SUM(CASE WHEN COALESCE(s.status,'backlog')='beaten' THEN 1 ELSE 0 END),"
        "  ROUND(SUM(g.play_secs)/3600.0,1)"
        " FROM game g LEFT JOIN status s ON s.rom_path=g.rom_path"
        " GROUP BY g.console ORDER BY 3 DESC, 2 DESC";
    if (sqlite3_prepare_v2(t->db, per_sql, -1, &st, NULL) != SQLITE_OK) return -1;
    printf("\n=== Beaten per console ===\n");
    printf("%-10s %6s %7s %7s %8s\n", "Console", "Games", "Beaten", "Done%", "Hours");
    printf("%-10s %6s %7s %7s %8s\n", "-------", "-----", "------", "-----", "-----");
    while (sqlite3_step(st) == SQLITE_ROW) {
        const char *console = (const char *)sqlite3_column_text(st, 0);
        int games  = sqlite3_column_int(st, 1);
        int beaten = sqlite3_column_int(st, 2);
        double hours = sqlite3_column_double(st, 3);
        double pct = games ? (100.0 * beaten / games) : 0.0;
        printf("%-10s %6d %7d %6.0f%% %8.1f\n", console, games, beaten, pct, hours);
    }
    sqlite3_finalize(st);
    printf("\n");
    return 0;
}

int trk_print_list(Tracker *t, const char *console_or_null) {
    char sql[512];
    snprintf(sql, sizeof sql,
        "SELECT g.console, g.name, COALESCE(s.status,'backlog'),"
        "       ROUND(g.play_secs/3600.0,1), g.rom_path"
        " FROM game g LEFT JOIN status s ON s.rom_path=g.rom_path %s"
        " ORDER BY g.console, g.name",
        console_or_null ? "WHERE g.console = ?1" : "");
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(t->db, sql, -1, &st, NULL) != SQLITE_OK) return -1;
    if (console_or_null) sqlite3_bind_text(st, 1, console_or_null, -1, SQLITE_TRANSIENT);
    printf("%-8s %-9s %5s  %s\n", "Console", "Status", "Hrs", "Game");
    while (sqlite3_step(st) == SQLITE_ROW) {
        printf("%-8s %-9s %5.1f  %s\n",
               (const char *)sqlite3_column_text(st, 0),
               (const char *)sqlite3_column_text(st, 2),
               sqlite3_column_double(st, 3),
               (const char *)sqlite3_column_text(st, 1));
    }
    sqlite3_finalize(st);
    return 0;
}
