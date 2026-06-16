/* menu.c — interactive on-device menu for the shell-MVP (Phase 2).
 *
 * Deliberately line-based (printf + fgets): this is the one input model that
 * works reliably in the OnionOS `st` terminal via the on-screen keyboard,
 * with no dependency on arrow-key/raw-tty behavior we can't test off-device.
 * The SDL2 GUI (Phase 3) will replace this with D-pad navigation.
 */
#include "tracker.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define MAXN 2048
#define PATHLEN 320

static int prompt_line(const char *msg, char *buf, size_t n) {
    printf("%s", msg);
    fflush(stdout);
    if (!fgets(buf, (int)n, stdin)) return -1;   /* EOF / SELECT-exit */
    buf[strcspn(buf, "\r\n")] = '\0';
    return 0;
}

/* Returns parsed int, or -1 on empty/EOF (treated as "back"). */
static int prompt_int(const char *msg) {
    char b[64];
    if (prompt_line(msg, b, sizeof b) != 0) return -1;
    if (b[0] == '\0') return -1;
    return atoi(b);
}

/* Fill `out` with distinct consoles (with counts). Returns count. */
static int list_consoles(Tracker *t, char out[][32], int max) {
    const char *sql =
        "SELECT g.console, COUNT(*),"
        "  SUM(CASE WHEN COALESCE(s.status,'backlog')='beaten' THEN 1 ELSE 0 END)"
        " FROM game g LEFT JOIN status s ON s.rom_path=g.rom_path"
        " GROUP BY g.console ORDER BY g.console";
    sqlite3_stmt *st = NULL;
    int n = 0;
    if (sqlite3_prepare_v2(t->db, sql, -1, &st, NULL) != SQLITE_OK) return 0;
    while (sqlite3_step(st) == SQLITE_ROW && n < max) {
        const char *c = (const char *)sqlite3_column_text(st, 0);
        int games = sqlite3_column_int(st, 1), beaten = sqlite3_column_int(st, 2);
        snprintf(out[n], 32, "%s", c ? c : "?");
        printf("  %2d) %-10s  %d/%d beaten\n", n + 1, out[n], beaten, games);
        n++;
    }
    sqlite3_finalize(st);
    return n;
}

/* List games for a console; fill paths[]. Returns count. */
static int list_games(Tracker *t, const char *console, char (*paths)[PATHLEN], int max) {
    const char *sql =
        "SELECT g.rom_path, g.name, COALESCE(s.status,'backlog'),"
        "       ROUND(g.play_secs/3600.0,1)"
        " FROM game g LEFT JOIN status s ON s.rom_path=g.rom_path"
        " WHERE g.console=?1 ORDER BY g.name";
    sqlite3_stmt *st = NULL;
    int n = 0;
    if (sqlite3_prepare_v2(t->db, sql, -1, &st, NULL) != SQLITE_OK) return 0;
    sqlite3_bind_text(st, 1, console, -1, SQLITE_TRANSIENT);
    while (sqlite3_step(st) == SQLITE_ROW && n < max) {
        const char *path = (const char *)sqlite3_column_text(st, 0);
        const char *name = (const char *)sqlite3_column_text(st, 1);
        const char *stat = (const char *)sqlite3_column_text(st, 2);
        double hrs = sqlite3_column_double(st, 3);
        snprintf(paths[n], PATHLEN, "%s", path);
        printf("  %2d) [%-9s] %-28.28s %4.1fh\n", n + 1, stat, name, hrs);
        n++;
    }
    sqlite3_finalize(st);
    return n;
}

static void mark_flow(Tracker *t, long now) {
    char consoles[64][32];
    char (*paths)[PATHLEN] = malloc(sizeof(*paths) * MAXN);
    if (!paths) return;

    printf("\n-- Pick a console --\n");
    int nc = list_consoles(t, consoles, 64);
    int ci = prompt_int("Console # (blank = back): ");
    if (ci < 1 || ci > nc) { free(paths); return; }

    printf("\n-- %s games --\n", consoles[ci - 1]);
    int ng = list_games(t, consoles[ci - 1], paths, MAXN);
    int gi = prompt_int("Game # (blank = back): ");
    if (gi < 1 || gi > ng) { free(paths); return; }

    printf("\n  Set status for: %s\n", paths[gi - 1]);
    printf("    1) backlog   2) playing   3) beaten   4) abandoned\n");
    int si = prompt_int("Status # (blank = back): ");
    const char *status = NULL;
    switch (si) {
        case 1: status = TRK_BACKLOG;   break;
        case 2: status = TRK_PLAYING;   break;
        case 3: status = TRK_BEATEN;    break;
        case 4: status = TRK_ABANDONED; break;
        default: free(paths); return;
    }
    if (trk_set_status(t, paths[gi - 1], status, now) == 0)
        printf("  ✓ marked %s\n", status);
    else
        printf("  ! failed to save\n");
    free(paths);
}

int trk_run_menu(Tracker *t) {
    long now = (long)time(NULL);
    for (;;) {
        printf("\n========= GAME TRACKER =========\n");
        printf("  1) View stats\n");
        printf("  2) Mark a game (beaten / backlog / ...)\n");
        printf("  3) List a console\n");
        printf("  0) Quit\n");
        int c = prompt_int("Choose: ");
        if (c == 0 || c == -1) { printf("Bye!\n"); return 0; }
        if (c == 1) {
            trk_print_stats(t);
        } else if (c == 2) {
            mark_flow(t, now);
        } else if (c == 3) {
            char consoles[64][32];
            printf("\n-- Consoles --\n");
            int nc = list_consoles(t, consoles, 64);
            int ci = prompt_int("Console # (blank = back): ");
            if (ci >= 1 && ci <= nc) {
                printf("\n-- %s --\n", consoles[ci - 1]);
                trk_print_list(t, consoles[ci - 1]);
            }
        }
    }
}
