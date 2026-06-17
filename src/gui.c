/* gui.c — framebuffer GUI for Game Tracker (Phase 3).
 * Links the tracker core (tracker.c + sqlite) for data + persistence, and
 * gfx/input for the screen. Reads tracker.sqlite; optional arg 2 = an OnionOS
 * snapshot to import first. D-pad navigable; no SDL, fully static. */
#define _DEFAULT_SOURCE 1   /* declare usleep/localtime_r/strdup under any -std */
#include "gfx.h"
#include "input.h"
#include "tracker.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>

#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_PNG
#define STBI_ONLY_JPEG
#define STBI_NO_LINEAR
#define STBI_NO_HDR
#include "stb_image.h"

#include <linux/kd.h>
#include <sys/ioctl.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#include <signal.h>
#include <dirent.h>

#define MT_VERSION "1.0.1"
/* ---- persisted settings (meta keys; defaults are the public-safe values) ---- */
static int g_favsync = 0;   /* "fav_sync": sync Playing <-> console favourite.json (opt-in) */
static int g_rumble  = 2;   /* "rumble":   strength 0=Off 1=Light 2=Medium 3=Strong */
static int g_hero_on = 1;   /* "hero":     ambient box-art backdrop on home */
static int g_view_default = 0; /* "view": default list(0)/grid(1) when opening a source */
static int g_npsel   = -1;  /* Now Playing strip focus: -1 = console list, 0..g_nplaying-1 */
static int g_onbanner = 0;  /* home focus is on the Stats banner card */

/* Stop the VT console from acting on keys (Ctrl/Shift/Alt/Space/Enter) and
 * from drawing text over our framebuffer. Restored on exit. */
static int g_tty = -1;
static int g_saved_kbmode = -1;
static void console_quiet(void) {
    g_tty = open("/dev/tty0", O_RDWR);
    if (g_tty < 0) g_tty = open("/dev/tty1", O_RDWR);
    if (g_tty < 0) return;
    if (ioctl(g_tty, KDGKBMODE, &g_saved_kbmode) != 0) g_saved_kbmode = -1;
    ioctl(g_tty, KDSKBMODE, K_OFF);        /* console ignores keyboard */
    ioctl(g_tty, KDSETMODE, KD_GRAPHICS);  /* console stops drawing text */
}
static void console_restore(void) {
    if (g_tty < 0) return;
    if (g_saved_kbmode >= 0) ioctl(g_tty, KDSKBMODE, g_saved_kbmode);
    ioctl(g_tty, KDSETMODE, KD_TEXT);
    close(g_tty);
    g_tty = -1;
}
/* Restore the console on abnormal exit (crash / SIGTERM from MainUI) so we never
 * leave the tty in graphics mode with the keyboard disabled, then re-raise. */
static void on_fatal_signal(int sig) {
    console_restore();
    signal(sig, SIG_DFL);
    raise(sig);
}

/* Rumble motor = GPIO 48, active-low (0=on, 1=off) — same as OnionOS.
 * NOT the backlight (that's pwmchip0/pwm0), so this is safe. */
static void gpio_wr(const char *path, const char *val) {
    int fd = open(path, O_WRONLY);
    if (fd < 0) return;
    ssize_t n = write(fd, val, strlen(val)); (void)n;
    close(fd);
}
static int g_rumble_init = 0;
static void rumble_ms(int ms) {
    if (!g_rumble_init) {
        gpio_wr("/sys/class/gpio/export", "48");
        gpio_wr("/sys/class/gpio/gpio48/direction", "out");
        gpio_wr("/sys/class/gpio/gpio48/value", "1");      /* off */
        g_rumble_init = 1;
    }
    gpio_wr("/sys/class/gpio/gpio48/value", "0");          /* on */
    usleep(ms * 1000);
    gpio_wr("/sys/class/gpio/gpio48/value", "1");          /* off */
}
static const int RUMBLE_BASE[4] = { 0, 35, 60, 95 };   /* off, light, medium, strong */
static void buzz_beaten(void) {
    int m = RUMBLE_BASE[g_rumble & 3];
    if (!m) return;
    rumble_ms(m * 6 / 10); usleep(45000); rumble_ms(m);   /* celebratory double-pulse */
}
static void buzz_tap(void) {
    int m = RUMBLE_BASE[g_rumble & 3];
    if (m) rumble_ms(m * 5 / 10);
}

/* Run the rumble in a forked child so the UI never pauses on marking beaten. */
static void play_celebrate(void) {
    pid_t pid = fork();
    if (pid != 0) return;                 /* parent (or fork failed): carry on */
    buzz_beaten();
    _exit(0);
}

/* Mini Tracker palette — semantic colors fixed; everything else theme-driven
 * (see THEMES / apply_theme) so the look can be reskinned, dark or light. */
#define C_RED    0xFFE0533Cu   /* "dropped" stays red across themes (semantic) */
#define C_GRAY   0xFF6E7286u
#define C_YELLOW 0xFFF5B642u

/* Theme-swappable colors (set by apply_theme; defaults = "Teal Dusk"). */
static uint32_t C_TEAL   = 0xFF2DD4BFu;   /* primary accent / Beaten */
static uint32_t C_PURPLE = 0xFFA855F7u;   /* secondary accent */
static uint32_t C_BLUE   = 0xFF6AA8FFu;   /* Playing */
static uint32_t C_SEL    = 0xFF3A2E5Cu;   /* selection / panel headers */
static uint32_t C_TITLE  = 0xFF1C1C2Au;   /* title bars / chrome */
static uint32_t C_TEXT   = 0xFFEDEDF2u;   /* primary text */
static uint32_t C_DIM    = 0xFF8A8CA0u;   /* secondary text */
static uint32_t C_PANEL  = 0xFF16161Fu;   /* detail panel fill */
static uint32_t C_BARBG  = 0xFF24243Au;   /* empty status bar / no-art box */
static uint32_t g_bg_top = 0xFF0D0D14u, g_bg_bot = 0xFF181826u;  /* home gradient */
static int g_light = 0;                   /* 1 = light theme (affects hero wash) */

typedef struct {
    const char *name;
    int light;
    uint32_t teal, purple, blue, sel, title, bg_top, bg_bot, text, dim, panel, barbg;
} Theme;
static const Theme THEMES[] = {
    { "Teal Dusk", 0, 0xFF2DD4BFu, 0xFFA855F7u, 0xFF6AA8FFu, 0xFF3A2E5Cu, 0xFF1C1C2Au, 0xFF0D0D14u, 0xFF181826u, 0xFFEDEDF2u, 0xFF8A8CA0u, 0xFF16161Fu, 0xFF24243Au },
    { "Sunset",    0, 0xFFFF9F45u, 0xFFFF5E7Eu, 0xFFFFC15Eu, 0xFF4A2C3Au, 0xFF2A1822u, 0xFF1A0F14u, 0xFF2A1820u, 0xFFEDEDF2u, 0xFF8A8CA0u, 0xFF1E1118u, 0xFF3A2230u },
    { "Matrix",    0, 0xFF3DDC84u, 0xFF8AE234u, 0xFF5EE0B0u, 0xFF1E3A2Au, 0xFF14241Cu, 0xFF0A140Eu, 0xFF14241Au, 0xFFEDEDF2u, 0xFF8A8CA0u, 0xFF11201Au, 0xFF1E3A2Au },
    { "Ocean",     0, 0xFF38BDF8u, 0xFF818CF8u, 0xFF5EEAD4u, 0xFF25304Au, 0xFF161E2Eu, 0xFF0A1018u, 0xFF141E2Eu, 0xFFEDEDF2u, 0xFF8A8CA0u, 0xFF131A28u, 0xFF24304A },
    { "Lite",      1, 0xFF0E9488u, 0xFF7C3AEDu, 0xFF2563EBu, 0xFFCBD6F5u, 0xFFE3E6EEu, 0xFFF5F6F9u, 0xFFE4E7EFu, 0xFF1B1E28u, 0xFF5C6175u, 0xFFFFFFFFu, 0xFFD7DBE6u },
};
#define N_THEMES ((int)(sizeof THEMES / sizeof THEMES[0]))
static int g_theme = 0;
static void apply_theme(void) {
    const Theme *t = &THEMES[g_theme % N_THEMES];
    C_TEAL = t->teal; C_PURPLE = t->purple; C_BLUE = t->blue;
    C_SEL = t->sel;   C_TITLE = t->title;
    C_TEXT = t->text; C_DIM = t->dim; C_PANEL = t->panel; C_BARBG = t->barbg;
    g_bg_top = t->bg_top; g_bg_bot = t->bg_bot;
    g_light = t->light;
}

enum { ST_BACKLOG, ST_PLAYING, ST_BEATEN, ST_ABANDONED };
static const char *ST_NAME[] = { "Backlog", "Playing", "Beaten", "Abandoned" };
static const char *ST_DB[]   = { TRK_BACKLOG, TRK_PLAYING, TRK_BEATEN, TRK_ABANDONED };
/* tier rating (beaten games only): 0 = unrated, 1..6 = F,D,C,B,A,S (6=S=best) */
#define N_TIERS 6
static const char *TIER_LABEL[] = { "-", "F", "D", "C", "B", "A", "S" };  /* index = rating */
/* a game can hold a tier rank once you've Beaten or Abandoned it (a verdict) */
#define RANKABLE(s) ((s) == ST_BEATEN || (s) == ST_ABANDONED)
/* Status colors are FIXED (not theme-swapped) so the status icons mean the same
 * thing in every theme: gray=Backlog, blue=Playing, teal=Beaten, red=Dropped. */
#define S_BACKLOG  0xFF6E7286u
#define S_PLAYING  0xFF6AA8FFu
#define S_BEATEN   0xFF2DD4BFu
#define S_ABANDON  0xFFE0533Cu
static uint32_t st_color(int s) {
    switch (s) { case ST_PLAYING: return S_PLAYING; case ST_BEATEN: return S_BEATEN;
                 case ST_ABANDONED: return S_ABANDON; default: return S_BACKLOG; }
}
/* Status color for TEXT: deepened on light themes so it stays legible (chips,
 * which are filled + white-glyphed, keep the bright fixed colors via st_color). */
static uint32_t st_textcol(int s) {
    if (!g_light) return st_color(s);
    switch (s) { case ST_PLAYING: return 0xFF1D4ED8u; case ST_BEATEN: return 0xFF0F766Eu;
                 case ST_ABANDONED: return 0xFFB23A28u; default: return 0xFF565B6Eu; }
}
/* classic tier-list colors: S red, A orange, B yellow, C green, D blue, F gray */
static uint32_t tier_color(int r) {
    switch (r) { case 6: return 0xFFFF6B6Bu; case 5: return 0xFFFF9F45u;
                 case 4: return 0xFFF5D061u; case 3: return 0xFF7BD88Fu;
                 case 2: return 0xFF5AA9E6u; case 1: return 0xFF9AA0B0u;
                 default: return C_BARBG; }
}

#define MAXG 8192      /* game cap (huge No-Intro-style sets); silently truncates beyond */
#define MAXC 64
typedef struct { char path[320], name[160], img[320]; int cidx, status, favorite, hidden, rating; double hours; long last_played, beaten_at; } Game;
static long g_now = 0;   /* wall clock at startup, for "last played" relative time */
typedef struct { char name[32]; int first, count, beaten, playing, aband; } Console;

static Game games[MAXG]; static int ngames;
static Console cons[MAXC]; static int ncons;

/* ---- sorting: a view[] of game indices for the current console ---- */
enum { SORT_PLAYTIME, SORT_LASTPLAYED, SORT_TIER, SORT_STATUS, SORT_NAME, SORT_NMODES };
static const char *SORT_LABEL[] = { "Playtime", "Last played", "Tier", "Status", "Name" };
static int g_sort = SORT_PLAYTIME;
static int view[MAXG]; static int nview;

/* status grouping order: in-progress/unplayed first, done/dropped last */
static int status_order(int s) {
    switch (s) { case ST_PLAYING: return 0; case ST_BACKLOG: return 1;
                 case ST_BEATEN: return 2; case ST_ABANDONED: return 3; default: return 4; }
}
static int cmp_view(const void *pa, const void *pb) {
    const Game *a = &games[*(const int *)pa], *b = &games[*(const int *)pb];
    if (g_sort == SORT_NAME) return strcasecmp(a->name, b->name);
    if (g_sort == SORT_PLAYTIME) {
        if (a->hours < b->hours) return 1;
        if (a->hours > b->hours) return -1;
        return strcasecmp(a->name, b->name);
    }
    if (g_sort == SORT_LASTPLAYED) {                  /* most recent first; never-played last */
        long la = a->last_played < TRK_MIN_VALID_EPOCH ? 0 : a->last_played;
        long lb = b->last_played < TRK_MIN_VALID_EPOCH ? 0 : b->last_played;
        if (la != lb) return la < lb ? 1 : -1;
        return strcasecmp(a->name, b->name);
    }
    if (g_sort == SORT_TIER) {                        /* S first; unrated last */
        if (a->rating != b->rating) return a->rating < b->rating ? 1 : -1;
        return strcasecmp(a->name, b->name);
    }
    int so = status_order(a->status) - status_order(b->status);   /* SORT_STATUS */
    if (so) return so;
    if (a->hours < b->hours) return 1;
    if (a->hours > b->hours) return -1;
    return strcasecmp(a->name, b->name);
}
static int g_show_hidden = 0;   /* "show_hidden": include hidden games in lists */
/* src == -1 => all games (global view); src >= 0 => that console */
static void build_view(int src) {
    nview = 0;
    for (int i = 0; i < ngames; i++) {
        if (src >= 0 && games[i].cidx != src) continue;
        if (games[i].hidden && !g_show_hidden) continue;
        view[nview++] = i;
    }
    qsort(view, nview, sizeof(int), cmp_view);
}
static const char *src_title(int src) { return src < 0 ? "All Games" : cons[src].name; }

/* "3 days ago"-style relative time for a unix epoch (junk/0 => never). */
static void rel_time(long ep, char *out, int n) {
    if (ep < TRK_MIN_VALID_EPOCH) { snprintf(out, n, "Never played"); return; }
    long d = g_now - ep; if (d < 0) d = 0;
    if (d < 90)             snprintf(out, n, "Played just now");
    else if (d < 3600)      snprintf(out, n, "Played %ld min ago", d / 60);
    else if (d < 86400)     snprintf(out, n, "Played %ld hr ago", d / 3600);
    else if (d < 86400 * 2) snprintf(out, n, "Played yesterday");
    else if (d < 86400 * 30) snprintf(out, n, "Played %ld days ago", d / 86400);
    else if (d < 86400 * 365) snprintf(out, n, "Played %ld months ago", d / (86400 * 30));
    else                    snprintf(out, n, "Played %ld years ago", d / (86400L * 365));
}

/* calendar date "Jun 16, 2026" for a unix epoch (junk/0 => "-"). */
static void fmt_date(long ep, char *out, int n) {
    static const char *mon[] = { "Jan","Feb","Mar","Apr","May","Jun",
                                 "Jul","Aug","Sep","Oct","Nov","Dec" };
    if (ep < TRK_MIN_VALID_EPOCH) { snprintf(out, n, "-"); return; }
    time_t tt = (time_t)ep; struct tm tmv;
    localtime_r(&tt, &tmv);
    snprintf(out, n, "%s %d, %d", mon[tmv.tm_mon % 12], tmv.tm_mday, tmv.tm_year + 1900);
}

static int status_from(const char *s) {
    if (!s) return ST_BACKLOG;
    if (!strcmp(s, "playing")) return ST_PLAYING;
    if (!strcmp(s, "beaten")) return ST_BEATEN;
    if (!strcmp(s, "abandoned")) return ST_ABANDONED;
    return ST_BACKLOG;
}

/* Per-console counts, excluding hidden games (hidden = rogue/junk the user
 * filtered out; they never count toward stats). */
static void recompute(void) {
    for (int c = 0; c < ncons; c++)
        cons[c].count = cons[c].beaten = cons[c].playing = cons[c].aband = 0;
    for (int i = 0; i < ngames; i++) {
        if (games[i].hidden) continue;
        Console *c = &cons[games[i].cidx];
        c->count++;
        switch (games[i].status) {
            case ST_BEATEN: c->beaten++; break;
            case ST_PLAYING: c->playing++; break;
            case ST_ABANDONED: c->aband++; break;
            default: break;
        }
    }
}

/* stacked status bar: beaten+playing+abandoned; rest = backlog. Fixed colors. */
static void draw_status_bar(Gfx *g, int x, int y, int w, int h,
                            int n, int beaten, int playing, int aband) {
    gfx_rect(g, x, y, w, h, C_BARBG);
    if (n <= 0) return;
    int cx = x;
    int wb = w * beaten / n; if (wb) { gfx_rect(g, cx, y, wb, h, S_BEATEN);  cx += wb; }
    int wp = w * playing / n; if (wp) { gfx_rect(g, cx, y, wp, h, S_PLAYING); cx += wp; }
    int wa = w * aband / n;   if (wa) { gfx_rect(g, cx, y, wa, h, S_ABANDON); cx += wa; }
}

/* Read OnionOS's Roms/favourite.json (newline-delimited JSON) and flag matching
 * games as favorites. Read-only: we never modify the device's favorites. */
static int sync_favorites(const char *roms_dir) {
    if (!roms_dir || !roms_dir[0]) return 0;
    char path[420];
    snprintf(path, sizeof path, "%s/favourite.json", roms_dir);
    FILE *f = fopen(path, "r");
    if (!f) return 0;
    char line[2048];
    int matched = 0;
    while (fgets(line, sizeof line, f)) {
        char *rp = strstr(line, "\"rompath\"");
        if (!rp) continue;
        rp = strchr(rp, ':'); if (!rp) continue;
        rp = strchr(rp, '"'); if (!rp) continue; rp++;          /* value start */
        char *end = strchr(rp, '"'); if (!end) continue; *end = 0;
        char *roms = strstr(rp, "/Roms/");
        const char *rel = roms ? roms + 6 : rp;                  /* path after /Roms/ */
        const char *slash = strchr(rel, '/');
        if (!slash) continue;
        char console[40];
        size_t cl = (size_t)(slash - rel);
        if (cl >= sizeof console) cl = sizeof console - 1;
        memcpy(console, rel, cl); console[cl] = 0;
        const char *base = strrchr(rel, '/'); base = base ? base + 1 : rel;
        for (int i = 0; i < ngames; i++) {
            if (strcmp(cons[games[i].cidx].name, console) != 0) continue;
            const char *gb = strrchr(games[i].path, '/');
            gb = gb ? gb + 1 : games[i].path;
            if (strcmp(gb, base) == 0) { games[i].favorite = 1; matched++; break; }
        }
    }
    fclose(f);
    return matched;
}

/* ---- writing favourite.json (so marking Playing pins to console favorites) ---- */
static void build_thumbs(void);   /* fwd decl (defined with the thumbnail cache) */
static void build_hero(void);     /* fwd decl (defined with the hero backdrop) */
static char g_roms[460] = "", g_emu[460] = "";
#define MAXEMU 64
static struct { char console[40], emu[40]; } emu_map[MAXEMU];
static int n_emu_map = 0;

/* Map each Roms/<CONSOLE> to its Emu/<EMU> folder (e.g. PS -> PSX) by reading
 * each Emu/<E>/config.json's rompath (".../Roms/<CONSOLE>"). */
static void build_emu_map(const char *emu_dir) {
    n_emu_map = 0;
    DIR *d = opendir(emu_dir);
    if (!d) return;
    struct dirent *e;
    while ((e = readdir(d)) != NULL && n_emu_map < MAXEMU) {
        if (e->d_name[0] == '.') continue;
        char cfg[600];
        snprintf(cfg, sizeof cfg, "%s/%s/config.json", emu_dir, e->d_name);
        FILE *f = fopen(cfg, "r");
        if (!f) continue;
        char data[2048];
        size_t rd = fread(data, 1, sizeof data - 1, f);
        data[rd] = 0; fclose(f);
        char *rp = strstr(data, "\"rompath\"");
        if (!rp) continue;
        rp = strchr(rp, ':'); if (!rp) continue;
        rp = strchr(rp, '"'); if (!rp) continue; rp++;
        char *end = strchr(rp, '"'); if (!end) continue; *end = 0;
        char *base = strrchr(rp, '/'); base = base ? base + 1 : rp;   /* last seg = console */
        snprintf(emu_map[n_emu_map].console, 40, "%s", base);
        snprintf(emu_map[n_emu_map].emu, 40, "%s", e->d_name);
        n_emu_map++;
    }
    closedir(d);
}
static const char *emu_for(const char *console) {
    for (int i = 0; i < n_emu_map; i++)
        if (!strcmp(emu_map[i].console, console)) return emu_map[i].emu;
    return console;   /* fallback: emu folder == console name */
}

/* Does a favourite.json line refer to the game (console + rom basename)? */
static int fav_line_match(const char *line, const char *console, const char *base) {
    const char *rp = strstr(line, "\"rompath\"");
    if (!rp) return 0;
    rp = strchr(rp, ':'); if (!rp) return 0;
    rp = strchr(rp, '"'); if (!rp) return 0; rp++;
    const char *end = strchr(rp, '"'); if (!end) return 0;
    char val[512]; size_t L = (size_t)(end - rp);
    if (L >= sizeof val) L = sizeof val - 1;
    memcpy(val, rp, L); val[L] = 0;
    const char *roms = strstr(val, "/Roms/");
    const char *rel = roms ? roms + 6 : val;
    const char *slash = strchr(rel, '/'); if (!slash) return 0;
    char con[40]; size_t cl = (size_t)(slash - rel); if (cl >= 40) cl = 39;
    memcpy(con, rel, cl); con[cl] = 0;
    const char *bs = strrchr(rel, '/'); bs = bs ? bs + 1 : rel;
    return !strcmp(con, console) && !strcmp(bs, base);
}

/* escape a string for a JSON double-quoted value (", \, newlines, control chars) */
static void json_esc(const char *s, char *out, int n) {
    int o = 0;
    for (; *s && o < n - 3; s++) {
        unsigned char c = (unsigned char)*s;
        if (c == '"' || c == '\\') { out[o++] = '\\'; out[o++] = (char)c; }
        else if (c == '\n') { out[o++] = '\\'; out[o++] = 'n'; }
        else if (c >= 0x20) out[o++] = (char)c;   /* drop other control chars */
    }
    out[o] = 0;
}
/* escape a string to sit safely inside shell double-quotes ("...": \ " $ `) */
static void sh_dq_esc(const char *s, char *out, int n) {
    int o = 0;
    for (; *s && o < n - 2; s++) {
        char c = *s;
        if (c == '\\' || c == '"' || c == '$' || c == '`') out[o++] = '\\';
        out[o++] = c;
    }
    out[o] = 0;
}

/* Rewrite favourite.json adding/removing this game. Backs up to .bak first,
 * writes atomically (temp + rename), preserves trailing newline, de-dupes.
 * Matches OnionOS's exact entry format. Read existing favorites are preserved. */
static void write_favourite(Game *gm, int add) {
    if (!g_roms[0]) return;
    char path[480], tmp[500], bak[500];
    snprintf(path, sizeof path, "%s/favourite.json", g_roms);
    snprintf(tmp,  sizeof tmp,  "%s/favourite.json.tmp", g_roms);
    snprintf(bak,  sizeof bak,  "%s/favourite.json.bak", g_roms);
    const char *console = cons[gm->cidx].name;
    const char *base = strrchr(gm->path, '/'); base = base ? base + 1 : gm->path;
    const char *imgbase = gm->img[0] ? (strrchr(gm->img, '/') ? strrchr(gm->img, '/') + 1 : gm->img) : "";
    const char *emu = emu_for(console);

    FILE *out = fopen(tmp, "w");
    if (!out) return;
    FILE *in = fopen(path, "r");
    if (in) {
        FILE *bk = fopen(bak, "w");           /* safety backup */
        char line[1200];
        while (fgets(line, sizeof line, in)) {
            if (bk) fputs(line, bk);
            if (fav_line_match(line, console, base)) continue;   /* drop existing (remove/dedup) */
            fputs(line, out);
            size_t L = strlen(line);
            if (L && line[L - 1] != '\n') fputc('\n', out);
        }
        if (bk) fclose(bk);
        fclose(in);
    }
    if (add) {
        char lbl[320]; json_esc(gm->name, lbl, sizeof lbl);
        if (imgbase[0])
            fprintf(out,
                "{\"label\":\"%s\",\"launch\":\"/mnt/SDCARD/Emu/%s/launch.sh\",\"type\":5,"
                "\"imgpath\":\"/mnt/SDCARD/Emu/%s/../../Roms/%s/Imgs/%s\","
                "\"rompath\":\"/mnt/SDCARD/Emu/%s/../../Roms/%s/%s\"}\n",
                lbl, emu, emu, console, imgbase, emu, console, base);
        else
            fprintf(out,
                "{\"label\":\"%s\",\"launch\":\"/mnt/SDCARD/Emu/%s/launch.sh\",\"type\":5,"
                "\"rompath\":\"/mnt/SDCARD/Emu/%s/../../Roms/%s/%s\"}\n",
                lbl, emu, emu, console, base);
    }
    fclose(out);
    rename(tmp, path);
}

/* Request launching the actual game. We hand off through OnionOS's OWN game
 * pipeline so the runtime registers the game in play-activity, recents and
 * GameSwitcher: write the launch line in cmd_to_run.sh format to /tmp/mt_cmd,
 * and our App's launch.sh moves it to .tmp_update/cmd_to_run.sh + touches
 * /tmp/quick_switch (the runtime's "quick switch" hook) before exiting.
 * The rompath uses /mnt/SDCARD/Roms/... which runtime.sh's check_is_game
 * recognises. /tmp is tmpfs; if the emulator/rom is missing we do nothing.
 * Returns 1 if queued. */
#define MT_LAUNCH_FILE "/tmp/mt_cmd"
#define MT_RECENT_FILE "/tmp/mt_recent"      /* JSON recent-list line for the game */
#define MT_RECENT_KEY  "/tmp/mt_recent_key"  /* raw rompath, for launch.sh de-dupe */
static int request_launch(Game *gm) {
    if (!g_emu[0] || !g_roms[0]) return 0;
    const char *console = cons[gm->cidx].name;
    const char *base = strrchr(gm->path, '/'); base = base ? base + 1 : gm->path;
    const char *emu = emu_for(console);
    char script[600], rom[700];
    snprintf(script, sizeof script, "%s/%s/launch.sh", g_emu, emu);
    if (access(script, F_OK) != 0) return 0;             /* no emulator -> abort safely */
    snprintf(rom, sizeof rom, "%s/%s/%s", g_roms, console, base);
    if (access(rom, F_OK) != 0) return 0;                /* rom gone -> abort safely */
    FILE *f = fopen(MT_LAUNCH_FILE, "w");
    if (!f) return 0;
    /* exact OnionOS game-launch line: LD_PRELOAD audio + "launch" "rompath",
     * with paths escaped so names containing $, `, ", or \ can't break/inject. */
    char es[640], er[760];
    sh_dq_esc(script, es, sizeof es);
    sh_dq_esc(rom,    er, sizeof er);
    fprintf(f, "LD_PRELOAD=/mnt/SDCARD/miyoo/app/../lib/libpadsp.so \"%s\" \"%s\"\n", es, er);
    fclose(f);

    /* Register the game in OnionOS's recent list so GameSwitcher shows a card +
     * resume screenshot. MainUI normally writes recents, but our quick_switch
     * handoff bypasses it; launch.sh merges this line into the active
     * recentlist*.json. Use MainUI's exact Emu-relative rompath form so it
     * dedupes against native entries and reuses the same romScreens/<hash>.png.
     * Best-effort: if any of this fails the game still launches (no card).
     * FAT32 forbids " and \ in names, so the JSON-escaped rompath == the raw key. */
    char rrel[760], irel[820];
    snprintf(rrel, sizeof rrel, "%s/%s/../../Roms/%s/%s", g_emu, emu, console, base);
    if (gm->img[0]) {
        snprintf(irel, sizeof irel, "%s", gm->img);
    } else {                                             /* fall back to <rom>.png in Imgs/ */
        char nb[200]; snprintf(nb, sizeof nb, "%s", base);
        char *dot = strrchr(nb, '.'); if (dot) *dot = 0;
        snprintf(irel, sizeof irel, "%s/%s/../../Roms/%s/Imgs/%s.png", g_emu, emu, console, nb);
    }
    FILE *r = fopen(MT_RECENT_FILE, "w");
    if (r) {
        char jl[340], jr[820], ji[900], js[700];
        json_esc(gm->name, jl, sizeof jl);
        json_esc(rrel,     jr, sizeof jr);
        json_esc(irel,     ji, sizeof ji);
        json_esc(script,   js, sizeof js);
        fprintf(r, "{\"label\":\"%s\",\"rompath\":\"%s\",\"imgpath\":\"%s\",\"launch\":\"%s\",\"type\":5}\n",
                jl, jr, ji, js);
        fclose(r);
        FILE *k = fopen(MT_RECENT_KEY, "w");
        if (k) { fputs(rrel, k); fclose(k); }
    }
    return 1;
}

/* full-screen "Launching ..." card shown just before we hand off to the emulator */
static void draw_bg(Gfx *g);   /* fwd decl (defined with the background gradient) */
static void draw_launching(Gfx *g, const char *name) {
    draw_bg(g);
    int cy = g->h / 2;
    int w1 = gfx_text_w(&font_title, "Launching");
    gfx_text(g, &font_title, (g->w - w1) / 2, cy - 34, "Launching", C_TEAL);
    int w2 = gfx_text_w(&font_body, name);
    gfx_text(g, &font_body, (g->w - w2) / 2, cy + 6, name, C_TEXT);
    gfx_present(g);
}

/* Set a game's status; keep favourite.json in sync (Playing <=> favorite). */
static void apply_status(Tracker *t, Game *gm, int ns, long now) {
    int was_playing = (gm->status == ST_PLAYING);
    gm->status = ns;
    trk_set_status(t, gm->path, ST_DB[ns], now);
    int is_playing = (ns == ST_PLAYING);
    if (g_favsync) {                                       /* opt-in: edit console favorites */
        if (is_playing && !was_playing) { write_favourite(gm, 1); gm->favorite = 1; }
        else if (!is_playing && was_playing) { write_favourite(gm, 0); gm->favorite = 0; }
    }
    if (g_rumble) {
        if (ns == ST_BEATEN) play_celebrate();
        else if (is_playing && !was_playing) buzz_tap();
    }
    recompute();
    build_thumbs();
}

/* Reconcile our statuses with favourite.json (only when sync is ON). Invariant:
 * favourited <=> Playing. Crucially this NEVER changes a Beaten/Abandoned mark —
 * those are "stronger" than a favorite and are preserved (just unpinned from
 * favorites). Handles favorites added/removed in OnionOS while we were closed. */
static void favsync_reconcile(Tracker *t) {
    long now = (long)time(NULL);
    sync_favorites(g_roms);                          /* refresh favorite flags */
    for (int i = 0; i < ngames; i++) {
        Game *g = &games[i];
        if (g->favorite) {
            if (g->status == ST_BACKLOG) {           /* adopt a console favorite as Playing */
                g->status = ST_PLAYING; trk_set_status(t, g->path, ST_DB[ST_PLAYING], now);
            } else if (g->status == ST_BEATEN || g->status == ST_ABANDONED) {
                write_favourite(g, 0); g->favorite = 0;   /* done -> unpin (keep the mark) */
            }
        } else if (g->status == ST_PLAYING) {        /* unpinned in OnionOS -> back to Backlog */
            g->status = ST_BACKLOG; trk_set_status(t, g->path, ST_DB[ST_BACKLOG], now);
        }
    }
    recompute();
}

/* Turning sync ON: push current in-app "Playing" games into favourites first (so
 * they survive instead of being demoted), then reconcile. Non-destructive merge. */
static void favsync_enable(Tracker *t) {
    sync_favorites(g_roms);
    for (int i = 0; i < ngames; i++)
        if (games[i].status == ST_PLAYING && !games[i].favorite) {
            write_favourite(&games[i], 1); games[i].favorite = 1;
        }
    favsync_reconcile(t);
    build_thumbs();
}

static int load(Tracker *t) {
    const char *sql =
        "SELECT g.console, g.name, g.rom_path, COALESCE(s.status,'backlog'),"
        "       g.play_secs/3600.0, COALESCE(g.image_path,''), COALESCE(s.hidden,0),"
        "       COALESCE(g.last_played,0), COALESCE(s.rating,0), COALESCE(s.beaten_at,0)"
        " FROM game g LEFT JOIN status s ON s.rom_path=g.rom_path"
        " ORDER BY g.console, g.name";
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(t->db, sql, -1, &st, NULL) != SQLITE_OK) return -1;
    ngames = ncons = 0;
    while (sqlite3_step(st) == SQLITE_ROW && ngames < MAXG) {
        const char *cn = (const char *)sqlite3_column_text(st, 0);
        const char *nm = (const char *)sqlite3_column_text(st, 1);
        const char *pa = (const char *)sqlite3_column_text(st, 2);
        const char *stt = (const char *)sqlite3_column_text(st, 3);
        double hrs = sqlite3_column_double(st, 4);
        const char *im = (const char *)sqlite3_column_text(st, 5);
        Game *g = &games[ngames];
        snprintf(g->name, sizeof g->name, "%s", nm ? nm : "?");
        snprintf(g->path, sizeof g->path, "%s", pa ? pa : "");
        snprintf(g->img, sizeof g->img, "%s", im ? im : "");
        g->status = status_from(stt);
        g->hidden = sqlite3_column_int(st, 6);
        g->last_played = (long)sqlite3_column_int64(st, 7);
        g->rating = sqlite3_column_int(st, 8);
        g->beaten_at = (long)sqlite3_column_int64(st, 9);
        g->hours = hrs;
        if (ncons == 0 || strcmp(cons[ncons - 1].name, cn ? cn : "?") != 0) {
            if (ncons < MAXC) {
                snprintf(cons[ncons].name, sizeof cons[ncons].name, "%s", cn ? cn : "?");
                cons[ncons].first = ngames; cons[ncons].count = 0; cons[ncons].beaten = 0;
                ncons++;
            }
        }
        g->cidx = ncons - 1;
        cons[ncons - 1].count++;
        ngames++;
    }
    sqlite3_finalize(st);
    recompute();
    return 0;
}

/* ---- box art (one decoded+scaled image cached at a time) ---- */
#define ART_MAX 240
static char art_path[320];
static uint32_t art_px[ART_MAX * ART_MAX];
static int art_w, art_h, art_ok;
static uint32_t art_glow = 0xFF6AA8FFu;   /* dominant color of current art (for glow) */

/* rounded-rect membership for an w*h box with corner radius rad */
static int in_round(int x, int y, int w, int h, int rad) {
    int cx = -1, cy = -1;
    if (x < rad && y < rad) { cx = rad; cy = rad; }
    else if (x >= w - rad && y < rad) { cx = w - rad - 1; cy = rad; }
    else if (x < rad && y >= h - rad) { cx = rad; cy = h - rad - 1; }
    else if (x >= w - rad && y >= h - rad) { cx = w - rad - 1; cy = h - rad - 1; }
    if (cx < 0) return 1;
    int dx = x - cx, dy = y - cy;
    return dx * dx + dy * dy <= rad * rad;
}

/* squared distance from (px,py) to segment (ax,ay)-(bx,by) */
static double seg_d2(double px, double py, double ax, double ay, double bx, double by) {
    double dx = bx - ax, dy = by - ay, L = dx * dx + dy * dy;
    double t = L > 0 ? ((px - ax) * dx + (py - ay) * dy) / L : 0;
    if (t < 0) t = 0; if (t > 1) t = 1;
    double cx = ax + t * dx, cy = ay + t * dy;
    return (px - cx) * (px - cx) + (py - cy) * (py - cy);
}
/* Draw a status glyph (shape, not just color) in an s*s box at (x,y), so the
 * UI is readable without relying on color: check=Beaten, triangle=Playing,
 * X=Abandoned, dash=Backlog. */
static void draw_glyph(Gfx *g, int x, int y, int s, int status, uint32_t col) {
    double th2 = (s * 0.15) * (s * 0.15);
    for (int yy = 0; yy < s; yy++) {
        int dy = y + yy; if (dy < 0 || dy >= g->h) continue;
        uint32_t *row = g->back + (size_t)dy * g->w;
        for (int xx = 0; xx < s; xx++) {
            int dx = x + xx; if (dx < 0 || dx >= g->w) continue;
            double fx = xx, fy = yy; int on = 0;
            switch (status) {
                case ST_BEATEN:                 /* check mark */
                    if (seg_d2(fx, fy, s*0.18, s*0.52, s*0.40, s*0.74) < th2 ||
                        seg_d2(fx, fy, s*0.40, s*0.74, s*0.82, s*0.26) < th2) on = 1;
                    break;
                case ST_PLAYING: {              /* play triangle (filled) */
                    double t = (fx - s*0.30) / (s*0.45);
                    if (t >= 0 && t <= 1) {
                        double half = (1 - t) * s*0.30;
                        if (fy >= s*0.5 - half && fy <= s*0.5 + half) on = 1;
                    }
                    break;
                }
                case ST_ABANDONED:              /* X */
                    if (seg_d2(fx, fy, s*0.24, s*0.24, s*0.76, s*0.76) < th2 ||
                        seg_d2(fx, fy, s*0.76, s*0.24, s*0.24, s*0.76) < th2) on = 1;
                    break;
                default:                        /* backlog: dash */
                    if (fy >= s*0.43 && fy <= s*0.57 && fx >= s*0.22 && fx <= s*0.78) on = 1;
                    break;
            }
            if (on) row[dx] = col;
        }
    }
}
/* colored status chip with its glyph overlaid (the standard status indicator) */
static void draw_status_chip(Gfx *g, int x, int y, int s, int status) {
    gfx_rect(g, x, y, s, s, st_color(status));
    draw_glyph(g, x, y, s, status, 0xFFFFFFFFu);
}
/* tier badge: a colored rounded square with the tier letter centered. */
static void draw_tier_badge(Gfx *g, int x, int y, int s, int rating) {
    if (rating <= 0) return;
    gfx_rect(g, x, y, s, s, tier_color(rating));
    const Font *f = (s >= 32) ? &font_title : &font_body;
    gfx_text(g, f, x + (s - f->w) / 2, y + (s - f->h) / 2, TIER_LABEL[rating], 0xFF101018u);
}

static void ensure_art(const char *path) {
    if (strcmp(path, art_path) == 0) return;
    snprintf(art_path, sizeof art_path, "%s", path);
    art_ok = 0; art_w = art_h = 0;
    if (!path || !path[0]) return;
    int w, h, n;
    unsigned char *img = stbi_load(path, &w, &h, &n, 4);
    if (!img) return;
    int ow = w, oh = h;
    if (w > ART_MAX || h > ART_MAX) {
        if (w >= h) { ow = ART_MAX; oh = h * ART_MAX / w; }
        else        { oh = ART_MAX; ow = w * ART_MAX / h; }
    }
    if (ow < 1) ow = 1; if (oh < 1) oh = 1;
    unsigned long sr = 0, sg = 0, sb = 0, cnt = 0;
    for (int y = 0; y < oh; y++) {
        int sy = y * h / oh;
        for (int x = 0; x < ow; x++) {
            int sx = x * w / ow;
            unsigned char *p = img + ((size_t)sy * w + sx) * 4;
            art_px[y * ART_MAX + x] =
                ((uint32_t)p[3] << 24) | ((uint32_t)p[0] << 16) | ((uint32_t)p[1] << 8) | p[2];
            if (p[3] > 128) { sr += p[0]; sg += p[1]; sb += p[2]; cnt++; }
        }
    }
    if (cnt) {                                  /* dominant color, brightened for glow */
        unsigned r = sr / cnt * 13 / 10, gg = sg / cnt * 13 / 10, b = sb / cnt * 13 / 10;
        if (r > 255) r = 255; if (gg > 255) gg = 255; if (b > 255) b = 255;
        art_glow = 0xFF000000u | (r << 16) | (gg << 8) | b;
    } else art_glow = 0xFF6AA8FFu;
    art_w = ow; art_h = oh; art_ok = 1;
    stbi_image_free(img);
}

static void blit_art(Gfx *g, int px, int py) {
    for (int y = 0; y < art_h; y++) {
        int dy = py + y; if (dy < 0 || dy >= g->h) continue;
        uint32_t *row = g->back + (size_t)dy * g->w;
        for (int x = 0; x < art_w; x++) {
            int dx = px + x; if (dx < 0 || dx >= g->w) continue;
            if (!in_round(x, y, art_w, art_h, 10)) continue;   /* rounded corners */
            uint32_t s = art_px[y * ART_MAX + x];
            unsigned a = s >> 24;
            if (a == 0) continue;
            if (a >= 255) { row[dx] = 0xFF000000u | (s & 0xFFFFFF); continue; }
            uint32_t bg = row[dx]; unsigned ia = 255 - a;
            unsigned r  = ((((s >> 16) & 255) * a) + (((bg >> 16) & 255) * ia)) / 255;
            unsigned gg = ((((s >> 8) & 255) * a)  + (((bg >> 8) & 255) * ia)) / 255;
            unsigned b  = (((s & 255) * a)         + ((bg & 255) * ia)) / 255;
            row[dx] = 0xFF000000u | (r << 16) | (gg << 8) | b;
        }
    }
}
/* blit the cached art scaled to dw*dh (for the detail page; caps tall covers) */
static void blit_art_scaled(Gfx *g, int px, int py, int dw, int dh) {
    if (dw < 1 || dh < 1) return;
    for (int y = 0; y < dh; y++) {
        int dy = py + y; if (dy < 0 || dy >= g->h) continue;
        int sy = y * art_h / dh;
        uint32_t *row = g->back + (size_t)dy * g->w;
        for (int x = 0; x < dw; x++) {
            int dx = px + x; if (dx < 0 || dx >= g->w) continue;
            if (!in_round(x, y, dw, dh, 9)) continue;
            int sx = x * art_w / dw;
            uint32_t s = art_px[sy * ART_MAX + sx];
            unsigned a = s >> 24; if (a == 0) continue;
            row[dx] = 0xFF000000u | (s & 0xFFFFFF);
        }
    }
}

/* ---- "Now Playing" strip: a horizontally scrollable wall of ALL the games
 * currently marked Playing (sorted by playtime). NPVIS covers are visible at a
 * time; the strip scrolls so every in-progress game is reachable. ---- */
#define NPVIS 4            /* covers visible in the strip at once */
#define THUMB 110
#define MAXPLAY 1024
static uint32_t thumb_px[NPVIS][THUMB * THUMB];
static int thumb_w[NPVIS], thumb_h[NPVIS];
static int thumb_for[NPVIS] = { -1, -1, -1, -1 };  /* game idx loaded in each slot */
static int play_idx[MAXPLAY];   /* all Playing game indices, sorted by playtime desc */
static int g_nplaying = 0;       /* total Playing games */
static int np_off = 0;           /* scroll offset: first visible playing index */

static int cmp_playtime(const void *a, const void *b) {
    double ha = games[*(const int *)a].hours, hb = games[*(const int *)b].hours;
    if (ha < hb) return 1; if (ha > hb) return -1;
    return strcasecmp(games[*(const int *)a].name, games[*(const int *)b].name);
}

static void load_thumb(const char *path, int slot) {
    thumb_w[slot] = thumb_h[slot] = 0;
    if (!path || !path[0]) return;
    int w, h, n;
    unsigned char *img = stbi_load(path, &w, &h, &n, 4);
    if (!img) return;
    int ow = THUMB, oh = THUMB;
    if (w >= h) oh = h * THUMB / w; else ow = w * THUMB / h;
    if (ow < 1) ow = 1; if (oh < 1) oh = 1;
    for (int y = 0; y < oh; y++) {
        int sy = y * h / oh;
        for (int x = 0; x < ow; x++) {
            int sx = x * w / ow;
            unsigned char *p = img + ((size_t)sy * w + sx) * 4;
            thumb_px[slot][y * THUMB + x] =
                ((uint32_t)p[3] << 24) | ((uint32_t)p[0] << 16) | ((uint32_t)p[1] << 8) | p[2];
        }
    }
    thumb_w[slot] = ow; thumb_h[slot] = oh;
    stbi_image_free(img);
}
/* rebuild the sorted Playing list + hero; invalidate the visible-cover cache */
static void build_thumbs(void) {
    g_nplaying = 0;
    for (int i = 0; i < ngames && g_nplaying < MAXPLAY; i++)
        if (games[i].status == ST_PLAYING && !games[i].hidden) play_idx[g_nplaying++] = i;
    qsort(play_idx, g_nplaying, sizeof(int), cmp_playtime);
    for (int k = 0; k < NPVIS; k++) thumb_for[k] = -1;   /* force reload */
    if (np_off > g_nplaying - 1 || np_off < 0) np_off = 0;
    build_hero();
}
/* load covers for the currently visible window (play_idx[np_off..]) on demand */
static void load_strip_window(void) {
    for (int k = 0; k < NPVIS; k++) {
        int pi = np_off + k;
        int gi = (pi < g_nplaying) ? play_idx[pi] : -1;
        if (thumb_for[k] == gi) continue;                /* unchanged -> keep */
        thumb_for[k] = gi;
        if (gi >= 0) load_thumb(games[gi].img, k);
        else { thumb_w[k] = thumb_h[k] = 0; }
    }
}
static void blit_thumb(Gfx *g, int slot, int px, int py) {
    int w = thumb_w[slot], h = thumb_h[slot];
    for (int y = 0; y < h; y++) {
        int dy = py + y; if (dy < 0 || dy >= g->h) continue;
        uint32_t *row = g->back + (size_t)dy * g->w;
        for (int x = 0; x < w; x++) {
            int dx = px + x; if (dx < 0 || dx >= g->w) continue;
            if (!in_round(x, y, w, h, 9)) continue;        /* rounded corners */
            uint32_t s = thumb_px[slot][y * THUMB + x];
            unsigned a = s >> 24;
            if (a == 0) continue;
            row[dx] = 0xFF000000u | (s & 0xFFFFFF);
        }
    }
}

/* subtle vertical gradient background (navy -> faint purple) */
static void draw_bg(Gfx *g) {
    int r0 = (g_bg_top >> 16) & 255, g0 = (g_bg_top >> 8) & 255, b0 = g_bg_top & 255;
    int r1 = (g_bg_bot >> 16) & 255, g1 = (g_bg_bot >> 8) & 255, b1 = g_bg_bot & 255;
    for (int y = 0; y < g->h; y++) {
        int t = y * 256 / g->h;
        int r = r0 + (r1 - r0) * t / 256;
        int gg = g0 + (g1 - g0) * t / 256;
        int b = b0 + (b1 - b0) * t / 256;
        uint32_t c = 0xFF000000u | (r << 16) | (gg << 8) | b;
        uint32_t *row = g->back + (size_t)y * g->w;
        for (int x = 0; x < g->w; x++) row[x] = c;
    }
}

/* ---- hero ambient backdrop: the top-played in-progress game's art, ---- */
/* ---- scaled-to-fill and darkened, behind the Now Playing strip ---- */
#define HEROW_MAX 768          /* buffer fits the widest supported screen (752) */
#define HEROH 166
static uint32_t hero_px[HEROW_MAX * HEROH];
static int hero_ok = 0;
static int g_scrw = 640, g_scrh = 480;   /* actual screen, set after gfx_init */
static int g_hvis = 6;                    /* home rows that fit, set after gfx_init */
static void build_hero(void) {
    hero_ok = 0;
    int HW = g_scrw < HEROW_MAX ? g_scrw : HEROW_MAX;
    if (g_nplaying == 0) return;
    const char *path = games[play_idx[0]].img;
    if (!path || !path[0]) return;
    int w, h, n;
    unsigned char *img = stbi_load(path, &w, &h, &n, 4);
    if (!img) return;
    double sxr = (double)HW / w, syr = (double)HEROH / h;
    double sc = sxr > syr ? sxr : syr;
    int sw = (int)(w * sc), sh = (int)(h * sc);
    int ox = (sw - HW) / 2, oy = (sh - HEROH) / 2;
    for (int y = 0; y < HEROH; y++) {
        int sy = (int)((y + oy) / sc); if (sy < 0) sy = 0; if (sy >= h) sy = h - 1;
        for (int x = 0; x < HW; x++) {
            int sx = (int)((x + ox) / sc); if (sx < 0) sx = 0; if (sx >= w) sx = w - 1;
            unsigned char *p = img + ((size_t)sy * w + sx) * 4;
            unsigned r, gg, b;
            if (g_light) {                                  /* wash toward white */
                r = p[0] * 30 / 100 + 178; gg = p[1] * 30 / 100 + 178; b = p[2] * 30 / 100 + 178;
                if (r > 255) r = 255; if (gg > 255) gg = 255; if (b > 255) b = 255;
            } else {                                        /* darken toward black */
                r = p[0] * 24 / 100; gg = p[1] * 24 / 100; b = p[2] * 24 / 100;
            }
            hero_px[y * HEROW_MAX + x] = 0xFF000000u | (r << 16) | (gg << 8) | b;
        }
    }
    hero_ok = 1;
    stbi_image_free(img);
}
static void blit_hero(Gfx *g, int py) {
    for (int y = 0; y < HEROH; y++) {
        int dy = py + y; if (dy < 0 || dy >= g->h) continue;
        uint32_t *row = g->back + (size_t)dy * g->w;
        for (int x = 0; x < g->w && x < HEROW_MAX; x++) row[x] = hero_px[y * HEROW_MAX + x];
    }
}

/* one-shot launch splash: the full logo, centered, for ~1.1s */
static void splash(Gfx *g, const char *path) {
    int w, h, n;
    unsigned char *img = stbi_load(path, &w, &h, &n, 4);
    /* fill with the logo's own (near-black) background so it blends seamlessly */
    uint32_t bgc = 0xFF000000u;
    if (img) bgc = 0xFF000000u | (img[0] << 16) | (img[1] << 8) | img[2];
    gfx_clear(g, bgc);
    if (img) {
        int maxw = g->w - 36, maxh = g->h - 36, ow = w, oh = h;
        if (w > maxw || h > maxh) {
            double sx = (double)maxw / w, sy = (double)maxh / h, sc = sx < sy ? sx : sy;
            ow = (int)(w * sc); oh = (int)(h * sc);
        }
        if (ow < 1) ow = 1; if (oh < 1) oh = 1;
        int px = (g->w - ow) / 2, py = (g->h - oh) / 2;
        for (int y = 0; y < oh; y++) {
            int sy = y * h / oh, dy = py + y; if (dy < 0 || dy >= g->h) continue;
            uint32_t *row = g->back + (size_t)dy * g->w;
            for (int x = 0; x < ow; x++) {
                int sx = x * w / ow, dx = px + x; if (dx < 0 || dx >= g->w) continue;
                unsigned char *p = img + ((size_t)sy * w + sx) * 4;
                unsigned a = p[3]; if (a == 0) continue;
                if (a >= 255) { row[dx] = 0xFF000000u | (p[0] << 16) | (p[1] << 8) | p[2]; continue; }
                uint32_t bg = row[dx]; unsigned ia = 255 - a;
                unsigned r = (p[0]*a + ((bg>>16)&255)*ia)/255, gg = (p[1]*a + ((bg>>8)&255)*ia)/255, b = (p[2]*a + (bg&255)*ia)/255;
                row[dx] = 0xFF000000u | (r << 16) | (gg << 8) | b;
            }
        }
        stbi_image_free(img);
    }
    gfx_present(g);
    usleep(1100000);
}

/* ---- screens ---- */
#define ROW_H 32
#define LIST_TOP 80
#define LISTW 360
#define VIS 11

static void draw_list(Gfx *g, int src, int sel, int top) {
    char buf[256];
    draw_bg(g);
    /* title bar */
    gfx_rect(g, 0, 0, g->w, 44, C_TITLE);
    gfx_rect(g, 0, 44, g->w, 2, C_TEAL);                /* accent underline */
    gfx_text(g, &font_title, 14, 7, ncons ? src_title(src) : "Mini Tracker", C_TEAL);
    if (ncons) {
        snprintf(buf, sizeof buf, "%d/%d", sel + 1, nview);
        gfx_text_right(g, &font_body, LISTW - 12, 14, buf, C_DIM);
    }
    /* sub bar: beaten + playing counts for the current view */
    if (ncons) {
        int beaten = 0, playing = 0;
        for (int i = 0; i < nview; i++) {
            int s = games[view[i]].status;
            if (s == ST_BEATEN) beaten++;
            else if (s == ST_PLAYING) playing++;
        }
        int x = 14;
        snprintf(buf, sizeof buf, "%d beaten", beaten);
        x = gfx_text(g, &font_body, x, 50, buf, st_textcol(ST_BEATEN));
        x = gfx_text(g, &font_body, x, 50, "   ", C_DIM);
        snprintf(buf, sizeof buf, "%d playing", playing);
        x = gfx_text(g, &font_body, x, 50, buf, st_textcol(ST_PLAYING));
        snprintf(buf, sizeof buf, "   of %d", nview);
        gfx_text(g, &font_body, x, 50, buf, C_DIM);
    }
    gfx_hline(g, 0, 76, g->w, C_TITLE);

    if (!ncons)
        gfx_text(g, &font_body, 14, LIST_TOP + 10,
                 "No games found. Play a game, then refresh.", C_DIM);

    /* left pane: list */
    for (int i = 0; i < VIS && ncons; i++) {
        int gi = top + i;
        if (gi >= nview) break;
        Game *gm = &games[view[gi]];
        int ry = LIST_TOP + i * ROW_H;
        if (gi == sel) {
            gfx_rect(g, 4, ry, LISTW - 8, ROW_H - 2, C_SEL);
            gfx_rect(g, 4, ry, 3, ROW_H - 2, C_TEAL);            /* selected marker */
        }
        draw_status_chip(g, 12, ry + 8, 14, gm->status);         /* status chip + glyph */
        int rated = (RANKABLE(gm->status) && gm->rating);
        if (src < 0) {                          /* All Games: show console tag */
            char nm[40];
            snprintf(nm, sizeof nm, "%.22s", gm->name);
            gfx_text(g, &font_body, 34, ry + 6, nm, gi == sel ? C_TEXT : C_DIM);
            int tagx = LISTW - 10 - gfx_text_w(&font_body, cons[gm->cidx].name);
            gfx_text(g, &font_body, tagx, ry + 6, cons[gm->cidx].name, C_PURPLE);
            if (rated) draw_tier_badge(g, tagx - 30, ry + 4, 24, gm->rating);
        } else {
            char nm[48];
            snprintf(nm, sizeof nm, "%.28s", gm->name);
            gfx_text(g, &font_body, 34, ry + 6, nm, gi == sel ? C_TEXT : C_DIM);
            if (rated) draw_tier_badge(g, LISTW - 32, ry + 4, 24, gm->rating);
        }
    }

    /* right pane: box art + details for the selected game */
    if (ncons && nview) {
        Game *gm = &games[view[sel]];
        int px0 = LISTW + 6, pw = g->w - px0;
        gfx_rect(g, px0, LIST_TOP - 2, pw, VIS * ROW_H, C_PANEL);
        ensure_art(gm->img);
        int ay = LIST_TOP + 8;
        if (art_ok) {
            int ax = px0 + (pw - art_w) / 2;
            /* soft colored glow from the art's dominant color */
            gfx_rect_blend(g, ax - 14, ay - 14, art_w + 28, art_h + 28, art_glow, 26);
            gfx_rect_blend(g, ax - 8,  ay - 8,  art_w + 16, art_h + 16, art_glow, 34);
            gfx_rect_blend(g, ax + 5,  ay + 7,  art_w, art_h, 0x000000u, 130);   /* shadow */
            blit_art(g, ax, ay);
        } else {
            gfx_rect(g, px0 + 18, ay, pw - 36, 150, C_BARBG);
            gfx_text(g, &font_body, px0 + 30, ay + 66, "no box art", C_DIM);
        }
        int ty = ay + (art_ok ? art_h : 150) + 16;
        draw_status_chip(g, px0 + 16, ty + 2, 14, gm->status);
        gfx_text(g, &font_body, px0 + 38, ty, ST_NAME[gm->status], C_TEXT);
        if (gm->hours >= 0.05) {
            snprintf(buf, sizeof buf, "%.1f h played", gm->hours);
            gfx_text(g, &font_body, px0 + 16, ty + 26, buf, C_DIM);
        }
        rel_time(gm->last_played, buf, sizeof buf);     /* last-played line */
        gfx_text(g, &font_body, px0 + 16, ty + 48, buf, C_DIM);
    }

    /* footer hints */
    gfx_rect(g, 0, g->h - 26, g->w, 26, C_TITLE);
    snprintf(buf, sizeof buf, "A:open  X:sort[%s]  Y:grid  L/R:console  B:menu",
             SORT_LABEL[g_sort]);
    gfx_text(g, &font_body, 10, g->h - 23, buf, C_DIM);
    gfx_present(g);
}

/* ---- box-art grid view: a cover wall over the current view[] ---- */
#define GCOLS 4                 /* 4 across x ~3 rows: larger, readable covers */
#define GTHUMB 116
#define GCACHE 40                /* roomy enough for the tier-list page */
#define GRID_TOP 60
#define GRID_VGAP 14
static uint32_t gcache_px[GCACHE][GTHUMB * GTHUMB];
static int  gcache_w[GCACHE], gcache_h[GCACHE];
static char gcache_path[GCACHE][320];
static int  gcache_next = 0;
/* return a cache slot holding this cover (loading on demand), or -1 */
static int grid_thumb(const char *path) {
    if (!path || !path[0]) return -1;
    for (int i = 0; i < GCACHE; i++)
        if (gcache_path[i][0] && strcmp(gcache_path[i], path) == 0) return i;
    int slot = gcache_next; gcache_next = (gcache_next + 1) % GCACHE;
    snprintf(gcache_path[slot], sizeof gcache_path[slot], "%s", path);
    gcache_w[slot] = gcache_h[slot] = 0;
    int w, h, n;
    unsigned char *img = stbi_load(path, &w, &h, &n, 4);
    if (!img) { gcache_path[slot][0] = 0; return -1; }
    int ow = GTHUMB, oh = GTHUMB;
    if (w >= h) oh = h * GTHUMB / w; else ow = w * GTHUMB / h;
    if (ow < 1) ow = 1; if (oh < 1) oh = 1;
    for (int y = 0; y < oh; y++) {
        int sy = y * h / oh;
        for (int x = 0; x < ow; x++) {
            int sx = x * w / ow;
            unsigned char *p = img + ((size_t)sy * w + sx) * 4;
            gcache_px[slot][y * GTHUMB + x] =
                ((uint32_t)p[3] << 24) | ((uint32_t)p[0] << 16) | ((uint32_t)p[1] << 8) | p[2];
        }
    }
    gcache_w[slot] = ow; gcache_h[slot] = oh;
    stbi_image_free(img);
    return slot;
}
static void blit_gthumb(Gfx *g, int slot, int px, int py) {
    int w = gcache_w[slot], h = gcache_h[slot];
    for (int y = 0; y < h; y++) {
        int dy = py + y; if (dy < 0 || dy >= g->h) continue;
        uint32_t *row = g->back + (size_t)dy * g->w;
        for (int x = 0; x < w; x++) {
            int dx = px + x; if (dx < 0 || dx >= g->w) continue;
            if (!in_round(x, y, w, h, 8)) continue;
            uint32_t s = gcache_px[slot][y * GTHUMB + x];
            unsigned a = s >> 24; if (a == 0) continue;
            row[dx] = 0xFF000000u | (s & 0xFFFFFF);
        }
    }
}
/* blit a cached cover scaled to fit a box*box cell (centered, aspect-preserved) */
static void blit_gthumb_fit(Gfx *g, int slot, int x, int y, int box) {
    int sw = gcache_w[slot], sh = gcache_h[slot];
    if (sw < 1 || sh < 1) return;
    int dw = box, dh = box;
    if (sw >= sh) dh = sh * box / sw; else dw = sw * box / sh;
    if (dw < 1) dw = 1; if (dh < 1) dh = 1;
    int ox = x + (box - dw) / 2, oy = y + (box - dh) / 2;
    for (int yy = 0; yy < dh; yy++) {
        int dy = oy + yy; if (dy < 0 || dy >= g->h) continue;
        int sy = yy * sh / dh;
        uint32_t *row = g->back + (size_t)dy * g->w;
        for (int xx = 0; xx < dw; xx++) {
            int dx = ox + xx; if (dx < 0 || dx >= g->w) continue;
            if (!in_round(xx, yy, dw, dh, 5)) continue;
            uint32_t s = gcache_px[slot][(yy * sh / dh) * GTHUMB + xx * sw / dw];
            unsigned a = s >> 24; if (a == 0) continue;
            row[dx] = 0xFF000000u | (s & 0xFFFFFF);
        }
    }
}
/* rows that fit on screen, given cell pitch (shared by draw + nav clamp) */
static int grid_vis_rows(Gfx *g) {
    int pitch = g->w / GCOLS, cw = pitch - 16; if (cw > GTHUMB) cw = GTHUMB;
    int rowh = cw + GRID_VGAP, vis = (g->h - GRID_TOP - 28) / rowh;
    return vis < 1 ? 1 : vis;
}
static void draw_grid(Gfx *g, int src, int sel, int top) {
    char buf[80];
    draw_bg(g);
    gfx_rect(g, 0, 0, g->w, 44, C_TITLE);
    gfx_rect(g, 0, 44, g->w, 2, C_TEAL);
    gfx_text(g, &font_title, 14, 7, src_title(src), C_TEAL);
    snprintf(buf, sizeof buf, "%d/%d", nview ? sel + 1 : 0, nview);
    gfx_text_right(g, &font_body, g->w - 14, 14, buf, C_DIM);

    int pitch = g->w / GCOLS, cw = pitch - 16; if (cw > GTHUMB) cw = GTHUMB;
    int top_y = GRID_TOP, rowh = cw + GRID_VGAP, vis = grid_vis_rows(g);
    for (int r = 0; r < vis; r++) {
        for (int c = 0; c < GCOLS; c++) {
            int idx = (top + r) * GCOLS + c;
            if (idx >= nview) { r = vis; break; }
            Game *gm = &games[view[idx]];
            int cx = c * pitch + (pitch - cw) / 2, cy = top_y + r * rowh;
            if (idx == sel) gfx_rect(g, c * pitch + 4, cy - 6, pitch - 8, rowh - 4, C_SEL);
            int slot = grid_thumb(gm->img);
            int covx = cx, covy = cy, covw = cw, covh = cw;
            if (slot >= 0) {
                int tw = gcache_w[slot], th = gcache_h[slot];
                int tx = cx + (cw - tw) / 2, ty = cy + (cw - th) / 2;
                if (idx == sel) gfx_rect(g, tx - 2, ty - 2, tw + 4, th + 4, C_TEAL);
                gfx_rect_blend(g, tx + 3, ty + 4, tw, th, 0x000000u, 120);   /* shadow */
                blit_gthumb(g, slot, tx, ty);
                covx = tx; covy = ty; covw = tw; covh = th;
            } else {
                gfx_rect(g, cx, cy, cw, cw, C_BARBG);
                gfx_text(g, &font_body, cx + 8, cy + cw / 2 - 10, "no art", C_DIM);
            }
            draw_status_chip(g, covx + 5, covy + 5, 26, gm->status);        /* status badge + glyph */
            if (RANKABLE(gm->status) && gm->rating) {                       /* tier badge, on a dark plate */
                int hs = 34, bx2 = covx + covw - hs - 4, by2 = covy + covh - hs - 4;
                gfx_rect_blend(g, bx2 - 3, by2 - 3, hs + 6, hs + 6, 0x000000u, 150);
                draw_tier_badge(g, bx2, by2, hs, gm->rating);
            }
        }
    }
    /* bottom bar: selected game name + hints */
    gfx_rect(g, 0, g->h - 26, g->w, 26, C_TITLE);
    if (nview) {
        char nm[48]; snprintf(nm, sizeof nm, "%.18s", games[view[sel]].name);
        gfx_text(g, &font_body, 10, g->h - 23, nm, C_TEXT);
    }
    char hint[80];
    snprintf(hint, sizeof hint, "A:open  X:sort[%s]  Y:list  B:menu", SORT_LABEL[g_sort]);
    gfx_text_right(g, &font_body, g->w - 10, g->h - 23, hint, C_DIM);
    gfx_present(g);
}

/* prominent scope pill (top-right) with L/R chevrons; shared by both stats tabs */
static void draw_scope_pill(Gfx *g, int scope) {
    const char *scn = scope < 0 ? "All Games" : cons[scope].name;
    int tw = gfx_text_w(&font_title, scn), pw = tw + 50, px = g->w - pw - 8, py = 7, ph = 32;
    gfx_rect(g, px - 2, py - 2, pw + 4, ph + 4, C_TEAL);
    gfx_rect(g, px, py, pw, ph, C_SEL);
    gfx_text(g, &font_body, px + 8, py + 9, "<", C_TEAL);
    gfx_text(g, &font_title, px + 24, py + 1, scn, C_TEXT);
    gfx_text(g, &font_body, px + pw - 16, py + 9, ">", C_TEAL);
}

/* most-played first (used to order tier-row previews + drill-in) */
static int tier_cmp_play(const void *a, const void *b) {
    double ha = games[*(const int *)a].hours, hb = games[*(const int *)b].hours;
    if (ha < hb) return 1; if (ha > hb) return -1;
    return strcasecmp(games[*(const int *)a].name, games[*(const int *)b].name);
}
/* collect game indices in a tier (within scope), sorted most-played first */
static int tier_games(int scope, int tier, int *out) {
    int n = 0;
    for (int i = 0; i < ngames; i++) {
        Game *gm = &games[i];
        if (gm->hidden || !RANKABLE(gm->status) || gm->rating != tier) continue;
        if (scope >= 0 && gm->cidx != scope) continue;
        out[n++] = i;
    }
    qsort(out, n, sizeof(int), tier_cmp_play);
    return n;
}

/* ---- Stats page: aggregate numbers + a tier list of beaten/rated games.
 * scope = -1 for all games, else a console index (L/R switches).
 * tsel = selected tier row (0=S .. N_TIERS-1=F), highlighted for drill-in. ---- */
static void draw_stats(Gfx *g, int scope, int tsel) {
    draw_bg(g);
    gfx_rect(g, 0, 0, g->w, 44, C_TITLE);
    gfx_rect(g, 0, 44, g->w, 2, C_TEAL);
    gfx_text(g, &font_title, 14, 7, "Tier List", C_TEAL);
    draw_scope_pill(g, scope);

    long ystart;                                   /* Jan 1 of the current year */
    { time_t tt = (time_t)g_now; struct tm tmv; localtime_r(&tt, &tmv);
      tmv.tm_mon = 0; tmv.tm_mday = 1; tmv.tm_hour = 0; tmv.tm_min = 0; tmv.tm_sec = 0;
      ystart = (long)mktime(&tmv); }

    int tot = 0, beaten = 0, byear = 0; double hours = 0;
    for (int i = 0; i < ngames; i++) {
        Game *gm = &games[i];
        if (gm->hidden || (scope >= 0 && gm->cidx != scope)) continue;
        tot++; hours += gm->hours;
        if (gm->status == ST_BEATEN) { beaten++;
            if (gm->beaten_at >= ystart) byear++; }
    }
    char s[120];
    snprintf(s, sizeof s, "%d beaten   %.0f h played   %d beaten this year", beaten, hours, byear);
    gfx_text(g, &font_body, (g->w - gfx_text_w(&font_body, s)) / 2, 54, s, C_DIM);

    int top_y = 82, rowh = (g->h - top_y - 28) / N_TIERS, labw = 52;
    int box = rowh - 8; if (box > 54) box = 54;
    static int tg[MAXG];
    for (int ti = 0; ti < N_TIERS; ti++) {
        int rval = N_TIERS - ti;                   /* S(6) down to F(1) */
        int ry = top_y + ti * rowh, rh = rowh - 6;
        if (ti == tsel) gfx_rect(g, 4, ry - 3, g->w - 8, rh + 6, C_TEAL);   /* selection frame */
        gfx_rect(g, 8, ry, labw, rh, tier_color(rval));
        gfx_text(g, &font_title, 8 + (labw - font_title.w) / 2, ry + (rh - font_title.h) / 2,
                 TIER_LABEL[rval], 0xFF101018u);
        gfx_rect_blend(g, 8 + labw + 4, ry, g->w - (labw + 20), rh, 0xFFFFFFu, g_light ? 22 : 10);

        int count = tier_games(scope, rval, tg);
        int cx = 8 + labw + 10, cy = ry + (rh - box) / 2;
        int maxcov = (g->w - cx - 8) / (box + 4), drawn = count < maxcov ? count : maxcov;
        for (int k = 0; k < drawn; k++) {
            int slot = grid_thumb(games[tg[k]].img);
            if (slot >= 0) blit_gthumb_fit(g, slot, cx, cy, box);
            else gfx_rect(g, cx, cy, box, box, C_BARBG);
            cx += box + 4;
        }
        if (count == 0)
            gfx_text(g, &font_body, 8 + labw + 12, ry + (rh - font_body.h) / 2, "-", C_DIM);
        else if (count > drawn) {
            char m[16]; snprintf(m, sizeof m, "+%d", count - drawn);
            gfx_text(g, &font_body, cx + 2, cy + box / 2 - 8, m, C_TEXT);
        }
    }
    gfx_rect(g, 0, g->h - 26, g->w, 26, C_TITLE);
    gfx_text(g, &font_body, 10, g->h - 23,
             "Up/Dn: tier   A: open   L/R: console   Y: overview   B: back", C_DIM);
    gfx_present(g);
}

/* Stats screen loop: L/R scope, Y toggles Tier List / Overview, Up/Down picks a
 * tier and A drills into it, X screenshots, B exits. Returns 1 if a game was
 * launched from a drill-in (so the caller exits to launch.sh). */
static void draw_overview(Gfx *g, int scope);
static int  save_screenshot(Gfx *g);
static int  run_tier(Gfx *g, int in, Tracker *t, int scope, int tier);
static int run_stats(Gfx *g, int in, Tracker *t) {
    int sc = -1, tab = 0, tsel = 0;   /* tab 0 = overview (default), 1 = tier list */
    tab ? draw_stats(g, sc, tsel) : draw_overview(g, sc);
    for (;;) {
        int sb = input_get(in, 1000);
        if (sb == PAD_NONE) continue;
        if (sb == PAD_LEFT || sb == PAD_L1)       sc = (sc <= -1) ? ncons - 1 : sc - 1;
        else if (sb == PAD_RIGHT || sb == PAD_R1) sc = (sc >= ncons - 1) ? -1 : sc + 1;
        else if (sb == PAD_Y)                     tab = !tab;
        else if (sb == PAD_X) {                                   /* clean frame, then capture */
            tab ? draw_stats(g, sc, tsel) : draw_overview(g, sc);
            save_screenshot(g); continue;
        }
        else if (tab && sb == PAD_UP)   { if (tsel > 0) tsel--; }
        else if (tab && sb == PAD_DOWN) { if (tsel < N_TIERS - 1) tsel++; }
        else if (tab && sb == PAD_A) { if (run_tier(g, in, t, sc, N_TIERS - tsel)) return 1; }
        else if (sb == PAD_B || sb == PAD_SELECT) break;
        else continue;
        tab ? draw_stats(g, sc, tsel) : draw_overview(g, sc);
    }
    return 0;
}

/* ---- minimal PNG writer (stored/uncompressed deflate) for screenshots ---- */
static uint32_t s_crc_tab[256]; static int s_crc_done = 0;
static uint32_t crc_update(uint32_t crc, const unsigned char *p, size_t n) {
    if (!s_crc_done) { for (uint32_t i = 0; i < 256; i++) { uint32_t c = i;
        for (int k = 0; k < 8; k++) c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
        s_crc_tab[i] = c; } s_crc_done = 1; }
    for (size_t i = 0; i < n; i++) crc = s_crc_tab[(crc ^ p[i]) & 0xFF] ^ (crc >> 8);
    return crc;
}
static void put_be32(FILE *f, uint32_t v) {
    fputc((v >> 24) & 255, f); fputc((v >> 16) & 255, f);
    fputc((v >> 8) & 255, f);  fputc(v & 255, f);
}
static void png_chunk(FILE *f, const char *type, const unsigned char *data, size_t len) {
    put_be32(f, (uint32_t)len);
    fwrite(type, 1, 4, f);
    if (len) fwrite(data, 1, len, f);
    uint32_t c = crc_update(0xFFFFFFFFu, (const unsigned char *)type, 4);
    c = crc_update(c, data, len) ^ 0xFFFFFFFFu;
    put_be32(f, c);
}
static int png_write(const char *path, Gfx *g) {
    int w = g->w, h = g->h;
    size_t rawlen = (size_t)h * (1 + 3 * w);
    unsigned char *raw = malloc(rawlen);
    if (!raw) return 0;
    size_t p = 0;
    for (int y = 0; y < h; y++) {
        raw[p++] = 0;                                   /* filter: none */
        const uint32_t *row = g->back + (size_t)y * w;
        for (int x = 0; x < w; x++) {
            uint32_t px = row[x];
            raw[p++] = (px >> 16) & 255; raw[p++] = (px >> 8) & 255; raw[p++] = px & 255;
        }
    }
    /* zlib stream: header + stored deflate blocks + adler32 */
    size_t nblk = (rawlen + 65534) / 65535;
    size_t zlen = 2 + nblk * 5 + rawlen + 4;
    unsigned char *z = malloc(zlen);
    if (!z) { free(raw); return 0; }
    size_t zp = 0;
    z[zp++] = 0x78; z[zp++] = 0x01;                     /* zlib header */
    size_t off = 0;
    while (off < rawlen) {
        size_t blk = rawlen - off; if (blk > 65535) blk = 65535;
        int final = (off + blk >= rawlen);
        z[zp++] = final ? 1 : 0;
        z[zp++] = blk & 255; z[zp++] = (blk >> 8) & 255;
        z[zp++] = ~blk & 255; z[zp++] = (~blk >> 8) & 255;
        memcpy(z + zp, raw + off, blk); zp += blk; off += blk;
    }
    uint32_t a = 1, bb = 0;                             /* adler32 over raw */
    for (size_t i = 0; i < rawlen; i++) { a = (a + raw[i]) % 65521; bb = (bb + a) % 65521; }
    uint32_t adler = (bb << 16) | a;
    z[zp++] = (adler >> 24) & 255; z[zp++] = (adler >> 16) & 255;
    z[zp++] = (adler >> 8) & 255;  z[zp++] = adler & 255;
    free(raw);

    FILE *f = fopen(path, "wb");
    if (!f) { free(z); return 0; }
    static const unsigned char sig[8] = { 137, 80, 78, 71, 13, 10, 26, 10 };
    fwrite(sig, 1, 8, f);
    unsigned char ihdr[13];
    ihdr[0]=(w>>24)&255; ihdr[1]=(w>>16)&255; ihdr[2]=(w>>8)&255; ihdr[3]=w&255;
    ihdr[4]=(h>>24)&255; ihdr[5]=(h>>16)&255; ihdr[6]=(h>>8)&255; ihdr[7]=h&255;
    ihdr[8]=8; ihdr[9]=2; ihdr[10]=0; ihdr[11]=0; ihdr[12]=0;
    png_chunk(f, "IHDR", ihdr, 13);
    png_chunk(f, "IDAT", z, zp);
    png_chunk(f, "IEND", NULL, 0);
    fclose(f);
    free(z);
    return 1;
}
/* Save the current screen as a branded PNG under /mnt/SDCARD/Screenshots. */
static int save_screenshot(Gfx *g) {
    int bw = 168, bh = 28, bx = g->w - bw - 10, by = g->h - bh - 34;   /* branding badge */
    gfx_rect_blend(g, bx, by, bw, bh, 0x000000u, 180);
    gfx_rect(g, bx, by, 4, bh, C_TEAL);
    draw_glyph(g, bx + 10, by + 7, 14, ST_BEATEN, C_TEAL);
    gfx_text(g, &font_body, bx + 30, by + 6, "Mini Tracker", C_TEXT);

    mkdir("/mnt/SDCARD/Screenshots", 0777);
    char path[160]; snprintf(path, sizeof path, "/mnt/SDCARD/Screenshots/MiniTracker_%ld.png", (long)time(NULL));
    int ok = png_write(path, g);

    const char *msg = ok ? "Saved to /Screenshots" : "Screenshot failed";
    int mw = gfx_text_w(&font_body, msg) + 28;
    gfx_rect(g, (g->w - mw) / 2 - 2, g->h / 2 - 20, mw + 4, 40, C_TEAL);
    gfx_rect(g, (g->w - mw) / 2, g->h / 2 - 18, mw, 36, C_SEL);
    gfx_text(g, &font_body, (g->w - mw) / 2 + 14, g->h / 2 - 9, msg, C_TEXT);
    gfx_present(g);
    return ok;
}

/* ---- Stats "Overview" tab: completion, library mix, beaten-by-month, top played ---- */
static int ov_cmp_hours(const void *a, const void *b) {
    double ha = games[*(const int *)a].hours, hb = games[*(const int *)b].hours;
    if (ha < hb) return 1; if (ha > hb) return -1; return 0;
}
static void draw_overview(Gfx *g, int scope) {
    draw_bg(g);
    gfx_rect(g, 0, 0, g->w, 44, C_TITLE);
    gfx_rect(g, 0, 44, g->w, 2, C_TEAL);
    gfx_text(g, &font_title, 14, 7, "Stats", C_TEAL);
    draw_scope_pill(g, scope);

    int tot = 0, beaten = 0, playing = 0, aband = 0; double hours = 0;
    int mon[6]; for (int i = 0; i < 6; i++) mon[i] = 0;
    struct tm nowtm; { time_t tt = (time_t)g_now; localtime_r(&tt, &nowtm); }
    int curym = nowtm.tm_year * 12 + nowtm.tm_mon;
    static int idx[MAXG]; int nidx = 0;
    for (int i = 0; i < ngames; i++) {
        Game *gm = &games[i];
        if (gm->hidden || (scope >= 0 && gm->cidx != scope)) continue;
        tot++; hours += gm->hours; idx[nidx++] = i;
        if (gm->status == ST_BEATEN) { beaten++;
            if (gm->beaten_at >= TRK_MIN_VALID_EPOCH) {
                time_t bt = (time_t)gm->beaten_at; struct tm btm; localtime_r(&bt, &btm);
                int ma = curym - (btm.tm_year * 12 + btm.tm_mon);
                if (ma >= 0 && ma < 6) mon[5 - ma]++;
            }
        } else if (gm->status == ST_PLAYING) playing++;
        else if (gm->status == ST_ABANDONED) aband++;
    }
    int pct = tot ? 100 * beaten / tot : 0;
    char b[96];

    /* 1) completion bar */
    gfx_text(g, &font_body, 14, 56, "Completion", C_DIM);
    snprintf(b, sizeof b, "%d / %d beaten", beaten, tot);
    gfx_text_right(g, &font_body, g->w - 14, 56, b, C_DIM);
    gfx_rect(g, 14, 80, g->w - 28, 26, C_BARBG);
    int fillw = tot ? (g->w - 28) * beaten / tot : 0;
    if (fillw) gfx_rect(g, 14, 80, fillw, 26, S_BEATEN);
    snprintf(b, sizeof b, "%d%%", pct);
    gfx_text_scaled(g, &font_title, 22, 79, b, 0xFF101018u, 1);

    /* 2) library mix */
    gfx_text(g, &font_body, 14, 120, "Library", C_DIM);
    draw_status_bar(g, 14, 142, g->w - 28, 22, tot, beaten, playing, aband);
    int lx = 14, ly = 172;
    int backlog = tot - beaten - playing - aband;
    draw_status_chip(g, lx, ly, 14, ST_BEATEN); snprintf(b,sizeof b,"%d beaten", beaten);
    lx = gfx_text(g, &font_body, lx + 20, ly - 2, b, C_DIM) + 16;
    draw_status_chip(g, lx, ly, 14, ST_PLAYING); snprintf(b,sizeof b,"%d playing", playing);
    lx = gfx_text(g, &font_body, lx + 20, ly - 2, b, C_DIM) + 16;
    draw_status_chip(g, lx, ly, 14, ST_BACKLOG); snprintf(b,sizeof b,"%d backlog", backlog);
    lx = gfx_text(g, &font_body, lx + 20, ly - 2, b, C_DIM) + 16;
    if (aband) { draw_status_chip(g, lx, ly, 14, ST_ABANDONED); snprintf(b,sizeof b,"%d dropped", aband);
                 gfx_text(g, &font_body, lx + 20, ly - 2, b, C_DIM); }

    /* 3) beaten by month (last 6) */
    gfx_text(g, &font_body, 14, 200, "Beaten by month", C_DIM);
    static const char *MON[] = {"Jan","Feb","Mar","Apr","May","Jun","Jul","Aug","Sep","Oct","Nov","Dec"};
    int mmax = 1; for (int i = 0; i < 6; i++) if (mon[i] > mmax) mmax = mon[i];
    int cbw = (g->w - 28) / 6, baseY = 286, bmaxh = 60;
    for (int i = 0; i < 6; i++) {
        int mh = mon[i] * bmaxh / mmax;
        int bx2 = 14 + i * cbw, bw2 = cbw - 12;
        gfx_rect(g, bx2, baseY - mh, bw2, mh ? mh : 1, S_BEATEN);
        int mi = ((nowtm.tm_mon - (5 - i)) % 12 + 12) % 12;
        gfx_text(g, &font_body, bx2 + (bw2 - gfx_text_w(&font_body, MON[mi])) / 2, baseY + 4, MON[mi], C_DIM);
        if (mon[i]) { char c[8]; snprintf(c,sizeof c,"%d",mon[i]);
                      gfx_text(g, &font_body, bx2 + (bw2 - gfx_text_w(&font_body,c))/2, baseY - mh - 18, c, C_TEXT); }
    }

    /* 4) most played */
    gfx_text(g, &font_body, 14, 318, "Most played", C_DIM);
    qsort(idx, nidx, sizeof(int), ov_cmp_hours);
    int rows = nidx < 5 ? nidx : 5, ry = 340;
    double toph = nidx ? games[idx[0]].hours : 1; if (toph < 0.1) toph = 0.1;
    for (int i = 0; i < rows; i++) {
        Game *gm = &games[idx[i]];
        char nm[40]; snprintf(nm, sizeof nm, "%.24s", gm->name);
        gfx_text(g, &font_body, 18, ry + 2, nm, C_TEXT);
        snprintf(b, sizeof b, "%.1f h", gm->hours);
        gfx_text_right(g, &font_body, g->w - 14, ry + 2, b, C_DIM);  /* hours pinned right */
        int barx = 360, barright = g->w - 14 - gfx_text_w(&font_body, b) - 12;  /* end before hours */
        int barw = barright - barx; if (barw < 12) barw = 12;
        gfx_rect(g, barx, ry + 3, barw, 12, C_BARBG);
        int fw = (int)(barw * gm->hours / toph);
        if (fw > 0) gfx_rect(g, barx, ry + 3, fw, 12, C_BLUE);
        ry += 22;
    }
    if (rows == 0) gfx_text(g, &font_body, 18, ry + 2, "No playtime recorded yet.", C_DIM);

    gfx_rect(g, 0, g->h - 26, g->w, 26, C_TITLE);
    gfx_text(g, &font_body, 10, g->h - 23, "L/R: console   Y: tier list   X: screenshot   B: back", C_DIM);
    gfx_present(g);
}

/* Shared modal chrome: drop shadow + accent border + panel fill, so overlays
 * read as a distinct card on top of the background. */
static void overlay_frame(Gfx *g, int bx, int by, int bw, int bh) {
    gfx_rect_blend(g, bx + 6, by + 9, bw, bh, 0x000000u, 150);   /* drop shadow */
    gfx_rect(g, bx - 2, by - 2, bw + 4, bh + 4, C_TEAL);         /* accent border */
    gfx_rect(g, bx, by, bw, bh, C_TITLE);                        /* panel fill */
}

/* ---- unified game page: box art + metadata + status + rating + launch ---- */
enum { RK_STATUS, RK_RATING, RK_LAUNCH, RK_HIDE };
/* Build the page's action rows for a game (rating row only when beaten). */
static int detail_rows(Game *gm, int *kind, int *arg) {
    int n = 0;
    for (int i = 0; i < 4; i++) { kind[n] = RK_STATUS; arg[n] = i; n++; }
    if (RANKABLE(gm->status)) { kind[n] = RK_RATING; arg[n] = 0; n++; }
    kind[n] = RK_LAUNCH; arg[n] = 0; n++;
    kind[n] = RK_HIDE;   arg[n] = 0; n++;
    return n;
}
/* Full-screen game page: left = info card (art, console, stats, rating),
 * right = actions. Title bar carries a colored pill for the current status, and
 * the current status row is marked with its color + a check. */
static void draw_detail(Gfx *g, Game *gm, int sel) {
    int kind[8], arg[8], nr = detail_rows(gm, kind, arg);
    int beaten = (gm->status == ST_BEATEN);
    char b[80];
    draw_bg(g);

    /* title bar: name + current-status pill */
    gfx_rect(g, 0, 0, g->w, 46, C_TITLE);
    gfx_rect(g, 0, 46, g->w, 2, C_TEAL);
    char nm[32]; snprintf(nm, sizeof nm, "%.24s", gm->name);
    gfx_text(g, &font_title, 14, 8, nm, C_TEAL);
    { int s = gm->status;
      int pw = gfx_text_w(&font_body, ST_NAME[s]) + 46;
      int px = g->w - pw - 12, py = 9, ph = 28;
      gfx_rect(g, px, py, pw, ph, st_color(s));
      draw_glyph(g, px + 10, py + 6, 16, s, 0xFFFFFFFFu);
      gfx_text(g, &font_body, px + 32, py + 5, ST_NAME[s], 0xFFFFFFFFu);
    }

    /* ---- left column: info card ---- */
    int lcx = 150, ay = 74;
    ensure_art(gm->img);
    int dw = 160, dh = 150;                          /* placeholder box dims */
    if (art_ok) {
        dw = art_w; dh = art_h;
        if (dh > 160) { dw = dw * 160 / dh; dh = 160; }    /* cap tall covers */
        if (dw > 220) { dh = dh * 220 / dw; dw = 220; }
        int ax = lcx - dw / 2;
        gfx_rect_blend(g, ax - 12, ay - 12, dw + 24, dh + 24, art_glow, 26);
        gfx_rect_blend(g, ax - 7,  ay - 7,  dw + 14, dh + 14, art_glow, 34);
        gfx_rect_blend(g, ax + 6,  ay + 7,  dw, dh, 0x000000u, 130);
        blit_art_scaled(g, ax, ay, dw, dh);
    } else {
        gfx_rect(g, lcx - 80, ay, 160, 150, C_BARBG);
        gfx_text(g, &font_body, lcx - 28, ay + 66, "no art", C_DIM);
    }
    int ly = ay + dh + 16;
    gfx_text(g, &font_body, lcx - gfx_text_w(&font_body, cons[gm->cidx].name) / 2, ly,
             cons[gm->cidx].name, C_PURPLE);
    ly += 28;
    int lx = 20;
    if (gm->hours >= 0.05) snprintf(b, sizeof b, "%.1f hours played", gm->hours);
    else                   snprintf(b, sizeof b, "Not played yet");
    gfx_text(g, &font_body, lx, ly, b, C_TEXT); ly += 24;
    rel_time(gm->last_played, b, sizeof b);
    gfx_text(g, &font_body, lx, ly, b, C_DIM); ly += 24;
    if (RANKABLE(gm->status)) {
        if (gm->status == ST_BEATEN) { char dt[24]; fmt_date(gm->beaten_at, dt, sizeof dt);
                                       snprintf(b, sizeof b, "Beaten %s", dt); }
        else                         snprintf(b, sizeof b, "Abandoned");
        gfx_text(g, &font_body, lx, ly, b, C_DIM); ly += 28;
        if (gm->rating) {
            draw_tier_badge(g, lx, ly, 34, gm->rating);
            gfx_text(g, &font_body, lx + 44, ly + 8, "tier rank", C_DIM);
        } else {
            gfx_rect(g, lx, ly, 140, 30, C_SEL);
            gfx_text(g, &font_body, lx + 12, ly + 5, "Not ranked yet", C_DIM);
        }
    } else {
        /* not yet a verdict — make the path to a rank obvious */
        int cw2 = 280;
        gfx_rect(g, lx - 2, ly - 2, cw2 + 4, 50, C_TEAL);
        gfx_rect(g, lx, ly, cw2, 46, C_SEL);
        gfx_text(g, &font_body, lx + 12, ly + 6, "Not ranked yet", C_TEXT);
        gfx_text(g, &font_body, lx + 12, ly + 26, "Beat or abandon it to rank", C_DIM);
    }

    /* ---- right column: actions ---- */
    int rx = 312, fullw = g->w - rx - 12;
    gfx_text(g, &font_body, rx, 60, "STATUS", C_DIM);
    int sy0 = 84, srh = 33;
    for (int j = 0; j < 4; j++) {                    /* the 4 status rows */
        int ry = sy0 + j * srh, rh = srh - 3;
        int cur = (gm->status == j), s = (sel == j);
        if (cur) gfx_rect_blend(g, rx - 6, ry, fullw + 6, rh, st_color(j), 40);
        if (s) { gfx_rect(g, rx - 6, ry, fullw + 6, rh, C_SEL); gfx_rect(g, rx - 6, ry, 3, rh, C_TEAL); }
        draw_status_chip(g, rx + 2, ry + (rh - 16) / 2, 16, j);
        uint32_t tc = cur ? st_color(j) : (s ? C_TEXT : C_DIM);
        gfx_text(g, &font_body, rx + 26, ry + (rh - 18) / 2, ST_NAME[j], tc);
        if (cur) draw_glyph(g, rx + fullw - 22, ry + (rh - 16) / 2, 16, ST_BEATEN, st_color(j));
    }
    int dy = sy0 + 4 * srh + 4;
    gfx_hline(g, rx - 6, dy, fullw + 6, C_SEL);
    int ry2 = dy + 12, arh = 30;
    for (int i = 4; i < nr; i++) {                   /* rating / launch / hide */
        int ry = ry2 + (i - 4) * arh, s = (sel == i);
        if (s) { gfx_rect(g, rx - 6, ry, fullw + 6, arh - 2, C_SEL); gfx_rect(g, rx - 6, ry, 3, arh - 2, C_TEAL); }
        uint32_t tc = s ? C_TEXT : C_DIM;
        const char *lbl = kind[i] == RK_RATING ? "Change rating"
                        : kind[i] == RK_LAUNCH ? "Launch game"
                        : (gm->hidden ? "Unhide game" : "Hide (exclude from stats)");
        gfx_text(g, &font_body, rx + 6, ry + (arh - 18) / 2, lbl, tc);
    }

    gfx_rect(g, 0, g->h - 26, g->w, 26, C_TITLE);
    gfx_text(g, &font_body, 10, g->h - 23, "Up/Down: choose    A: select    B: back", C_DIM);
    gfx_present(g);
}

/* Full-screen tier-rank menu: pick S/A/B/C/D/F. Returns the chosen rating
 * (1..6), or 0 if the user backed out. */
static int rating_chooser(Gfx *g, int in, Game *gm, const char *headline) {
    int order[6] = { 6, 5, 4, 3, 2, 1 };       /* S A B C D F, left to right */
    int rsel = 2;                               /* default B */
    for (int i = 0; i < 6; i++) if (order[i] == gm->rating) rsel = i;
    input_get(in, 0);
    for (;;) {
        draw_bg(g);
        gfx_text(g, &font_title, (g->w - gfx_text_w(&font_title, headline)) / 2, 50, headline, C_TEAL);
        char nm[48]; snprintf(nm, sizeof nm, "%.40s", gm->name);
        gfx_text(g, &font_body, (g->w - gfx_text_w(&font_body, nm)) / 2, 96, nm, C_DIM);
        gfx_text(g, &font_body, (g->w - gfx_text_w(&font_body, "Rank this game")) / 2, 124, "Rank this game", C_DIM);

        int tw = 92, th = 120, gap = 6, total = tw * 6 + gap * 5, ox = (g->w - total) / 2, oy = 152;
        for (int i = 0; i < 6; i++) {
            int rval = order[i];
            if (i == rsel) gfx_rect(g, ox - 4, oy - 4, tw + 8, th + 8, C_TEAL);
            gfx_rect(g, ox, oy, tw, th, tier_color(rval));
            int sc = 3, lw = font_title.w * sc, lh = font_title.h * sc;
            gfx_text_scaled(g, &font_title, ox + (tw - lw) / 2, oy + (th - lh) / 2,
                            TIER_LABEL[rval], 0xFF101018u, sc);
            ox += tw + gap;
        }
        gfx_text(g, &font_body, (g->w - gfx_text_w(&font_body, "L/R: choose    A: confirm    B: skip")) / 2,
                 g->h - 24, "L/R: choose    A: confirm    B: skip", C_DIM);
        gfx_present(g);

        int b = input_get(in, 30000);
        if (b == PAD_LEFT || b == PAD_L1)       rsel = (rsel + 5) % 6;
        else if (b == PAD_RIGHT || b == PAD_R1) rsel = (rsel + 1) % 6;
        else if (b == PAD_A) return order[rsel];
        else if (b == PAD_B || b == PAD_SELECT) return 0;
    }
}

/* Animated full-screen "BEATEN!" celebration: confetti popper, a flash, a big
 * pulsing banner, the box art with an imploding glow, and a counting-up stat.
 * Drains input while it plays so it can't be skipped, then asks for a rating
 * via the rating menu. */
#define NCF 110
static void celebrate(Gfx *g, int in, Tracker *t, Game *gm) {
    int tot = 0, beaten = 0;
    for (int i = 0; i < ngames; i++) {
        if (games[i].hidden) continue;
        tot++;
        if (games[i].status == ST_BEATEN) beaten++;
    }
    int pct = tot ? (100 * beaten / tot) : 0;
    Console *c = &cons[gm->cidx];
    ensure_art(gm->img);

    /* confetti, bursting up+out from the art center (no trig: random vx/vy) */
    static float cx[NCF], cy[NCF], vx[NCF], vy[NCF];
    static uint32_t cc[NCF]; static int csz[NCF];
    uint32_t spark = g_light ? 0xFF1B1E28u : 0xFFFFFFFFu;   /* 6th confetti: visible on the bg */
    uint32_t pal[6] = { S_BEATEN, S_PLAYING, C_PURPLE, C_YELLOW, S_ABANDON, spark };
    int cxp = g->w / 2, cyp = 140;
    for (int i = 0; i < NCF; i++) {
        cx[i] = (float)cxp; cy[i] = (float)cyp;
        vx[i] = (float)((rand() % 200) - 100) / 11.0f;       /* -9 .. 9 */
        vy[i] = -1.5f - (float)(rand() % 150) / 14.0f;        /* upward burst */
        cc[i] = pal[rand() % 6]; csz[i] = 3 + rand() % 4;
    }

    char nm[64]; snprintf(nm, sizeof nm, "%.40s", gm->name);
    char s1[80], s2[80];
    int FR = 48;
    for (int f = 0; f < FR; f++) {
        draw_bg(g);
        int ay = 44, yb;
        if (art_ok) {
            int ax = (g->w - art_w) / 2;
            int ring = f < 10 ? (44 - f * 4) : 6;             /* glow implodes inward */
            gfx_rect_blend(g, ax - ring, ay - ring, art_w + 2 * ring, art_h + 2 * ring, S_BEATEN, 24);
            gfx_rect_blend(g, ax - 8, ay - 8, art_w + 16, art_h + 16, S_BEATEN, 40);
            gfx_rect_blend(g, ax + 6, ay + 8, art_w, art_h, 0x000000u, 130);
            blit_art(g, ax, ay);
            yb = ay + art_h;
        } else yb = ay + 150;

        /* confetti step + draw */
        for (int i = 0; i < NCF; i++) {
            vy[i] += 0.36f; cx[i] += vx[i]; cy[i] += vy[i];
            int px = (int)cx[i], py = (int)cy[i];
            if (py > -8 && py < g->h) gfx_rect(g, px, py, csz[i], csz[i] + 2, cc[i]);
        }

        /* big pulsing BEATEN! banner */
        int sc = 2, wB = gfx_text_w(&font_title, "BEATEN!") * sc;
        uint32_t pulseA = g_light ? C_PURPLE : 0xFFFFFFFFu;
        uint32_t bc = ((f / 3) % 2) ? pulseA : st_textcol(ST_BEATEN);
        gfx_rect_blend(g, 0, yb + 12, g->w, 66, S_BEATEN, g_light ? 40 : 22);
        gfx_text_scaled(g, &font_title, (g->w - wB) / 2, yb + 20, "BEATEN!", bc, sc);

        gfx_text(g, &font_body, (g->w - gfx_text_w(&font_body, nm)) / 2, yb + 92, nm, C_TEXT);

        int shown = (beaten * (f + 1)) / FR; if (shown > beaten) shown = beaten;  /* count-up */
        snprintf(s1, sizeof s1, "%d games beaten   %d%% complete", shown, pct);
        gfx_text(g, &font_body, (g->w - gfx_text_w(&font_body, s1)) / 2, yb + 120, s1, C_PURPLE);
        snprintf(s2, sizeof s2, "%s   %d / %d cleared", c->name, c->beaten, c->count);
        gfx_text(g, &font_body, (g->w - gfx_text_w(&font_body, s2)) / 2, yb + 146, s2, st_textcol(ST_PLAYING));

        if (f < 6) gfx_rect_blend(g, 0, 0, g->w, g->h, 0xFFFFFFu, (6 - f) * 32);   /* impact flash */

        gfx_present(g);
        input_get(in, 0);          /* drain & ignore presses so it can't be skipped */
        usleep(33000);
    }

    /* rating menu (icons) — manual exit; sets the rating if the user confirms */
    int r = rating_chooser(g, in, gm, "How was it?");
    if (r > 0) { gm->rating = r; trk_set_rating(t, gm->path, gm->rating, g_now); }
}

/* Quiet "DROPPED" moment for abandoning a game: desaturated art, gentle
 * falling rain, no confetti/rumble. Then asks for a tier rank. */
#define NRAIN 80
static void lament(Gfx *g, int in, Tracker *t, Game *gm) {
    ensure_art(gm->img);
    static float rnx[NRAIN], rny[NRAIN], rnv[NRAIN];
    for (int i = 0; i < NRAIN; i++) {
        rnx[i] = (float)(rand() % g->w);
        rny[i] = (float)(-(rand() % g->h));
        rnv[i] = 3.0f + (float)(rand() % 5);
    }
    char nm[64]; snprintf(nm, sizeof nm, "%.40s", gm->name);
    int FR = 42;
    for (int f = 0; f < FR; f++) {
        draw_bg(g);
        int ay = 50 + f / 3, yb;                       /* art slowly slumps */
        if (art_ok) {
            int ax = (g->w - art_w) / 2;
            gfx_rect_blend(g, ax + 6, ay + 8, art_w, art_h, 0x000000u, 130);
            blit_art(g, ax, ay);
            gfx_rect_blend(g, ax, ay, art_w, art_h, 0xFF6E7286u, 130);   /* drain the color */
            gfx_rect_blend(g, ax, ay, art_w, art_h, 0x000000u, 70);      /* dim */
            yb = ay + art_h;
        } else yb = ay + 150;
        for (int i = 0; i < NRAIN; i++) {              /* falling rain */
            rny[i] += rnv[i];
            if (rny[i] > g->h) { rny[i] = -6; rnx[i] = (float)(rand() % g->w); }
            gfx_rect(g, (int)rnx[i], (int)rny[i], 2, 9, 0xFF6A7C96u);
        }
        int sc = 2, wB = gfx_text_w(&font_title, "DROPPED") * sc;
        gfx_rect_blend(g, 0, yb + 12, g->w, 60, 0xFF6E7286u, 20);
        gfx_text_scaled(g, &font_title, (g->w - wB) / 2, yb + 18, "DROPPED", 0xFF9AA3B8u, sc);
        gfx_text(g, &font_body, (g->w - gfx_text_w(&font_body, nm)) / 2, yb + 86, nm, C_TEXT);
        gfx_text(g, &font_body, (g->w - gfx_text_w(&font_body, "Not every game is for you.")) / 2,
                 yb + 114, "Not every game is for you.", C_DIM);
        gfx_present(g);
        input_get(in, 0);
        usleep(33000);
    }
    int r = rating_chooser(g, in, gm, "How was it?");
    if (r > 0) { gm->rating = r; trk_set_rating(t, gm->path, gm->rating, g_now); }
}

/* Pick a random game still in the Backlog (uniform). -1 if none left. */
static int pick_random_backlog(void) {
    int n = 0;
    for (int i = 0; i < ngames; i++) if (games[i].status == ST_BACKLOG && !games[i].hidden) n++;
    if (n == 0) return -1;
    int k = rand() % n, j = 0;
    for (int i = 0; i < ngames; i++)
        if (games[i].status == ST_BACKLOG && !games[i].hidden && j++ == k) return i;
    return -1;
}

/* Count games still in the Backlog (the recommendation pool). */
static int backlog_count(void) {
    int n = 0;
    for (int i = 0; i < ngames; i++) if (games[i].status == ST_BACKLOG && !games[i].hidden) n++;
    return n;
}

/* "What should I play?" — a single random backlog suggestion you can launch,
 * mark Playing, or shuffle. gi < 0 means the backlog is empty. */
static void draw_surprise(Gfx *g, int gi) {
    draw_bg(g);
    gfx_rect(g, 0, 0, g->w, 44, C_TITLE);
    gfx_rect(g, 0, 44, g->w, 2, C_TEAL);
    gfx_text(g, &font_title, 16, 7, "Roll the Dice", C_TEAL);
    if (gi < 0) {
        const char *l1 = "Your backlog is empty.";
        const char *l2 = "Every game is in progress, beaten, or dropped — nice work!";
        gfx_text(g, &font_title, (g->w - gfx_text_w(&font_title, l1)) / 2, g->h / 2 - 40, l1, C_TEXT);
        gfx_text(g, &font_body,  (g->w - gfx_text_w(&font_body,  l2)) / 2, g->h / 2 + 4,  l2, C_DIM);
        gfx_rect(g, 0, g->h - 26, g->w, 26, C_TITLE);
        gfx_text(g, &font_body, 10, g->h - 23, "B: Back", C_DIM);
        gfx_present(g);
        return;
    }
    Game *gm = &games[gi];

    /* subtitle on its own centered line under the title bar (no overlap) */
    char sub[80];
    snprintf(sub, sizeof sub, "A random pick from your %d backlog games", backlog_count());
    gfx_text(g, &font_body, (g->w - gfx_text_w(&font_body, sub)) / 2, 52, sub, C_DIM);

    ensure_art(gm->img);
    int ay = 80;
    if (art_ok) {
        int ax = (g->w - art_w) / 2;
        gfx_rect_blend(g, ax - 14, ay - 14, art_w + 28, art_h + 28, art_glow, 30);
        gfx_rect_blend(g, ax - 8,  ay - 8,  art_w + 16, art_h + 16, art_glow, 34);
        gfx_rect_blend(g, ax + 6,  ay + 8,  art_w, art_h, 0x000000u, 130);   /* shadow */
        blit_art(g, ax, ay);
        ay += art_h;
    } else ay += 150;

    /* game name, prominent and centered */
    char nm[64]; snprintf(nm, sizeof nm, "%.32s", gm->name);
    gfx_text(g, &font_title, (g->w - gfx_text_w(&font_title, nm)) / 2, ay + 14, nm, C_TEXT);

    /* console + playtime, then last-played, each on its own centered line */
    char s[96];
    if (gm->hours >= 0.05) snprintf(s, sizeof s, "%s    %.1f h played", cons[gm->cidx].name, gm->hours);
    else                   snprintf(s, sizeof s, "%s", cons[gm->cidx].name);
    gfx_text(g, &font_body, (g->w - gfx_text_w(&font_body, s)) / 2, ay + 50, s, C_PURPLE);
    char lp[48]; rel_time(gm->last_played, lp, sizeof lp);
    gfx_text(g, &font_body, (g->w - gfx_text_w(&font_body, lp)) / 2, ay + 74, lp, C_DIM);

    gfx_rect(g, 0, g->h - 26, g->w, 26, C_TITLE);
    gfx_text(g, &font_body, 10, g->h - 23,
             "A: Play it    Y: Mark playing    X: Roll again    B: Back", C_DIM);
    gfx_present(g);
}

/* Unified game popup (list, grid, and Now Playing strip all open this). Lets
 * you set status, rate a beaten game, see metadata, launch, or hide. Returns 1
 * if the user chose Launch (caller should exit so launch.sh runs the game). */
static int open_detail(Gfx *g, int in, Tracker *t, Game *gm, int cc) {
    int kind[8], arg[8], nr = detail_rows(gm, kind, arg);
    int sel = gm->status; if (sel >= nr) sel = 0;          /* start on current status */
    draw_detail(g, gm, sel);
    for (;;) {
        int b = input_get(in, 1000);
        if (b == PAD_NONE) continue;
        if (b == PAD_UP)        { sel = (sel - 1 + nr) % nr; draw_detail(g, gm, sel); }
        else if (b == PAD_DOWN) { sel = (sel + 1) % nr; draw_detail(g, gm, sel); }
        else if (b == PAD_A) {
            int k = kind[sel];
            if (k == RK_STATUS) {
                int was = gm->status;
                apply_status(t, gm, arg[sel], g_now);
                if (arg[sel] == ST_BEATEN && was != ST_BEATEN) celebrate(g, in, t, gm);
                else if (arg[sel] == ST_ABANDONED && was != ST_ABANDONED)     /* drop -> sad moment + rank */
                    lament(g, in, t, gm);
                build_view(cc);
                nr = detail_rows(gm, kind, arg);            /* rating row may appear/vanish */
                if (sel >= nr) sel = nr - 1;
            } else if (k == RK_RATING) {
                int r = rating_chooser(g, in, gm, "Rate this game");   /* pick an icon */
                if (r > 0) { gm->rating = r; trk_set_rating(t, gm->path, gm->rating, g_now); }
            } else if (k == RK_LAUNCH) {
                if (request_launch(gm)) return 1;
            } else {                                         /* RK_HIDE */
                gm->hidden = !gm->hidden;
                trk_set_hidden(t, gm->path, gm->hidden, g_now);
                recompute(); build_thumbs(); build_view(cc);
            }
            draw_detail(g, gm, sel);
        } else if (b == PAD_B || b == PAD_SELECT) break;
    }
    return 0;
}

/* Drill-in: a full scrollable grid of every game in one tier (within scope).
 * A opens a game (returns 1 if it requested a launch), B returns to the list. */
static int run_tier(Gfx *g, int in, Tracker *t, int scope, int tier) {
    static int tv[MAXG];
    int n = tier_games(scope, tier, tv);
    if (n == 0) return 0;
    int sel = 0, top = 0;
    int pitch = g->w / GCOLS, cw = pitch - 16; if (cw > GTHUMB) cw = GTHUMB;
    int top_y = 60, rowh = cw + GRID_VGAP, vis = (g->h - top_y - 28) / rowh; if (vis < 1) vis = 1;
    for (;;) {
        draw_bg(g);
        gfx_rect(g, 0, 0, g->w, 44, C_TITLE);
        gfx_rect(g, 0, 44, g->w, 2, C_TEAL);
        draw_tier_badge(g, 12, 7, 30, tier);
        char ttl[64]; snprintf(ttl, sizeof ttl, "%s tier", TIER_LABEL[tier]);
        gfx_text(g, &font_title, 50, 7, ttl, C_TEAL);
        char cnt[64]; snprintf(cnt, sizeof cnt, "%d games  -  %s", n, scope < 0 ? "All Games" : cons[scope].name);
        gfx_text_right(g, &font_body, g->w - 14, 14, cnt, C_PURPLE);

        for (int r = 0; r < vis; r++) {
            for (int c = 0; c < GCOLS; c++) {
                int idx = (top + r) * GCOLS + c;
                if (idx >= n) { r = vis; break; }
                Game *gm = &games[tv[idx]];
                int gx = c * pitch + (pitch - cw) / 2, gy = top_y + r * rowh;
                if (idx == sel) gfx_rect(g, c * pitch + 4, gy - 6, pitch - 8, rowh - 4, C_SEL);
                int slot = grid_thumb(gm->img);
                int covx = gx, covy = gy, covw = cw, covh = cw;
                if (slot >= 0) {
                    int tw = gcache_w[slot], th = gcache_h[slot];
                    int tx = gx + (cw - tw) / 2, ty = gy + (cw - th) / 2;
                    if (idx == sel) gfx_rect(g, tx - 2, ty - 2, tw + 4, th + 4, C_TEAL);
                    gfx_rect_blend(g, tx + 3, ty + 4, tw, th, 0x000000u, 120);
                    blit_gthumb(g, slot, tx, ty);
                    covx = tx; covy = ty; covw = tw; covh = th;
                } else { gfx_rect(g, gx, gy, cw, cw, C_BARBG);
                         gfx_text(g, &font_body, gx + 8, gy + cw / 2 - 10, "no art", C_DIM); }
                draw_status_chip(g, covx + 5, covy + 5, 22, gm->status);
            }
        }
        gfx_rect(g, 0, g->h - 26, g->w, 26, C_TITLE);
        char nm[40]; snprintf(nm, sizeof nm, "%.22s", games[tv[sel]].name);
        gfx_text(g, &font_body, 10, g->h - 23, nm, C_TEXT);
        gfx_text_right(g, &font_body, g->w - 10, g->h - 23, "A: open   B: back", C_DIM);
        gfx_present(g);

        int b = input_get(in, 1000);
        if (b == PAD_NONE) continue;
        if (b == PAD_UP)         { if (sel - GCOLS >= 0) sel -= GCOLS; }
        else if (b == PAD_DOWN)  { if (sel + GCOLS < n) sel += GCOLS; else if (sel < n - 1) sel = n - 1; }
        else if (b == PAD_LEFT)  { if (sel > 0) sel--; }
        else if (b == PAD_RIGHT) { if (sel < n - 1) sel++; }
        else if (b == PAD_A) {
            if (open_detail(g, in, t, &games[tv[sel]], scope)) { draw_launching(g, games[tv[sel]].name); return 1; }
            n = tier_games(scope, tier, tv);       /* re-ranking may change the set */
            if (n == 0) return 0;
            if (sel >= n) sel = n - 1;
        }
        else if (b == PAD_B || b == PAD_SELECT) return 0;
        { int srow = sel / GCOLS;
          if (srow < top) top = srow;
          if (srow >= top + vis) top = srow - vis + 1; }
    }
}

#define N_SETTINGS 9
static const char *SETTINGS_LABEL[N_SETTINGS] = {
    "Sync Playing with console favorites",
    "Rumble",
    "Default sort",
    "Default view",
    "Show hidden games",
    "Hero backdrop",
    "Theme",
    "Help & controls",
    "About",
};
static const char *SETTINGS_DESC[N_SETTINGS] = {
    "Games marked as 'Playing' will be shown in your favorites folder.",
    "Vibration strength when you mark a game Beaten.",
    "Default order for lists (you can also press X in a list).",
    "Open games as a list or a box-art grid (toggle with Y).",
    "Reveal games you've hidden, so you can unhide them.",
    "Show the top game's box art behind the Now Playing strip.",
    "Color palette for the whole app.",
    "Buttons, status icons, and what each color means.",
    "Version, tagline and credits.",
};
static const char *RUMBLE_NAME[4] = { "Off", "Light", "Medium", "Strong" };
static void setting_value(int i, char *out, int n) {
    switch (i) {
        case 0: snprintf(out, n, "%s", g_favsync ? "ON" : "OFF"); break;
        case 1: snprintf(out, n, "%s", RUMBLE_NAME[g_rumble & 3]); break;
        case 2: snprintf(out, n, "%s", SORT_LABEL[g_sort % SORT_NMODES]); break;
        case 3: snprintf(out, n, "%s", g_view_default ? "Grid" : "List"); break;
        case 4: snprintf(out, n, "%s", g_show_hidden ? "ON" : "OFF"); break;
        case 5: snprintf(out, n, "%s", g_hero_on ? "ON" : "OFF"); break;
        case 6: snprintf(out, n, "%s", THEMES[g_theme % N_THEMES].name); break;
        default: out[0] = 0; break;     /* Help/About: no value */
    }
}
static void draw_settings(Gfx *g, int sel) {
    int bw = g->w - 40; if (bw > 600) bw = 600;
    int bh = 46 + N_SETTINGS * 38 + 44;
    int bx = (g->w - bw) / 2, by = (g->h - bh) / 2;
    overlay_frame(g, bx, by, bw, bh);
    gfx_rect(g, bx, by, bw, 40, C_SEL);
    gfx_text(g, &font_body, bx + 16, by + 9, "Settings", C_TEXT);
    gfx_text_right(g, &font_body, bx + bw - 16, by + 9, "A change   B close", C_DIM);
    for (int i = 0; i < N_SETTINGS; i++) {
        int ry = by + 46 + i * 38;
        if (i == sel) { gfx_rect(g, bx + 8, ry, bw - 16, 32, C_SEL); gfx_rect(g, bx + 8, ry, 3, 32, C_TEAL); }
        gfx_text(g, &font_body, bx + 18, ry + 5, SETTINGS_LABEL[i], i == sel ? C_TEXT : C_DIM);
        char v[24]; setting_value(i, v, sizeof v);
        uint32_t vc = C_TEAL;
        if ((i == 0 && !g_favsync) || (i == 4 && !g_show_hidden) ||
            (i == 5 && !g_hero_on) || (i == 1 && g_rumble == 0)) vc = C_GRAY;
        gfx_text_right(g, &font_body, bx + bw - 18, ry + 5, v, vc);
    }
    gfx_hline(g, bx + 12, by + bh - 38, bw - 24, C_SEL);
    gfx_text(g, &font_body, bx + 16, by + bh - 28, SETTINGS_DESC[sel], C_DIM);   /* inline help */
    gfx_present(g);
}
static void draw_help(Gfx *g) {
    int bw = g->w - 40; if (bw > 600) bw = 600;
    int bh = 360, bx = (g->w - bw) / 2, by = (g->h - bh) / 2;
    overlay_frame(g, bx, by, bw, bh);
    gfx_rect(g, bx, by, bw, 40, C_SEL);
    gfx_text(g, &font_body, bx + 16, by + 9, "Help & controls", C_TEXT);
    int yy = by + 52, lh = 26, lx = bx + 18;
    gfx_text(g, &font_body, lx, yy, "Home:  D-pad  A open  X roll  Y settings  SEL quit", C_TEXT); yy += lh;
    gfx_text(g, &font_body, lx, yy, "List:  A open  X sort  Y grid  L/R console  B back", C_TEXT); yy += lh + 8;
    gfx_text(g, &font_body, lx, yy, "Status:", C_DIM); yy += lh;
    draw_status_chip(g, lx, yy + 3, 14, ST_PLAYING); gfx_text(g, &font_body, lx + 22, yy, "Playing", C_TEXT);
    draw_status_chip(g, lx + 170, yy + 3, 14, ST_BEATEN); gfx_text(g, &font_body, lx + 192, yy, "Beaten", C_TEXT);
    yy += lh;
    draw_status_chip(g, lx, yy + 3, 14, ST_BACKLOG); gfx_text(g, &font_body, lx + 22, yy, "Backlog", C_TEXT);
    draw_status_chip(g, lx + 170, yy + 3, 14, ST_ABANDONED); gfx_text(g, &font_body, lx + 192, yy, "Abandoned", C_TEXT);
    yy += lh + 8;
    gfx_text(g, &font_body, lx, yy, "Now Playing = in-progress games, by playtime.", C_DIM); yy += lh;
    gfx_text(g, &font_body, lx, yy, "Press UP from the top row, L/R to pick, A to open.", C_DIM);
    gfx_text(g, &font_body, bx + 16, by + bh - 26, "B: back", C_DIM);
    gfx_present(g);
}
static void draw_about(Gfx *g) {
    int bw = 440, bh = 230, bx = (g->w - bw) / 2, by = (g->h - bh) / 2;
    overlay_frame(g, bx, by, bw, bh);
    gfx_rect(g, bx, by, bw, 40, C_SEL);
    gfx_text(g, &font_body, bx + 16, by + 9, "About", C_TEXT);
    gfx_text(g, &font_title, bx + 20, by + 58, "Mini Tracker", C_TEAL);
    gfx_text(g, &font_body, bx + 20, by + 100, "Track. Beat. Collect.", C_PURPLE);
    gfx_text(g, &font_body, bx + 20, by + 132, "Version " MT_VERSION, C_TEXT);
    gfx_text(g, &font_body, bx + 20, by + 160, "A backlog tracker for OnionOS", C_DIM);
    gfx_text(g, &font_body, bx + 16, by + bh - 24, "B: back", C_DIM);
    gfx_present(g);
}
/* confirmation shown before enabling favorites sync */
static void draw_favsync_confirm(Gfx *g, int adopt, int push) {
    int bw = g->w - 60; if (bw > 560) bw = 560;
    int bh = 220, bx = (g->w - bw) / 2, by = (g->h - bh) / 2;
    overlay_frame(g, bx, by, bw, bh);
    gfx_rect(g, bx, by, bw, 40, C_PURPLE);
    gfx_text(g, &font_body, bx + 16, by + 9, "Enable favorites sync?", C_TEXT);
    char l[96];
    snprintf(l, sizeof l, "%d console favorite(s) will become PLAYING", adopt);
    gfx_text(g, &font_body, bx + 18, by + 58, l, C_BLUE);
    snprintf(l, sizeof l, "%d Playing game(s) will be pinned as favorites", push);
    gfx_text(g, &font_body, bx + 18, by + 88, l, C_TEAL);
    gfx_text(g, &font_body, bx + 18, by + 126, "Beaten / Abandoned marks are never changed.", C_DIM);
    gfx_text(g, &font_body, bx + 16, by + bh - 28, "A: Enable        B: Cancel", C_TEXT);
    gfx_present(g);
}

/* home console list: entry 0 = "All Games", entries 1..ncons = consoles */
#define HOME_TOP 244
#define HROW 34
#define HVIS 6

static void draw_home(Gfx *g, int hsel, int htop) {
    char buf[256];
    draw_bg(g);
    if (hero_ok && g_hero_on) blit_hero(g, 48);        /* ambient backdrop behind Now Playing */
    /* title bar */
    gfx_rect(g, 0, 0, g->w, 46, C_TITLE);
    gfx_rect(g, 0, 46, g->w, 2, C_TEAL);                /* accent underline */
    gfx_text(g, &font_title, 16, 8, "Mini Tracker", C_TEAL);
    int tot = 0, beaten = 0, playing = 0, backlog = 0, aband = 0; double hours = 0;
    for (int i = 0; i < ngames; i++) {
        if (games[i].hidden) continue;             /* hidden games never count */
        tot++;
        hours += games[i].hours;
        switch (games[i].status) {
            case ST_BEATEN: beaten++; break;
            case ST_PLAYING: playing++; break;
            case ST_ABANDONED: aband++; break;
            default: backlog++; break;
        }
    }
    int pct = tot ? (100 * beaten / tot) : 0;
    snprintf(buf, sizeof buf, "%d games  %.1fh", tot, hours);
    gfx_text_right(g, &font_body, g->w - 16, 14, buf, C_DIM);

    /* ---- Now Playing strip (scrollable wall of all in-progress games) ---- */
    gfx_text(g, &font_body, 16, 58, "NOW PLAYING", C_BLUE);
    if (g_nplaying > 0) {
        if (g_npsel >= 0) snprintf(buf, sizeof buf, "%d of %d", g_npsel + 1, g_nplaying);
        else              snprintf(buf, sizeof buf, "%d in progress", g_nplaying);
        gfx_text_right(g, &font_body, g->w - 16, 58, buf, C_DIM);
    }
    int ty = 80;
    load_strip_window();
    if (g_nplaying == 0) {
        gfx_text(g, &font_body, 16, ty + 40, "Nothing in progress. Set a game to Playing to pin it here.", C_DIM);
    } else {
        int nvis = g_nplaying - np_off; if (nvis > NPVIS) nvis = NPVIS;
        int step = THUMB + 26;
        int total = nvis * THUMB + (nvis - 1) * 26;
        int sx = (g->w - total) / 2;                       /* center the visible window */
        for (int k = 0; k < nvis; k++) {
            int pi = np_off + k;
            int px = sx + k * step;
            if (pi == g_npsel) {                           /* focused cover: teal glow ring */
                for (int r = 7; r >= 2; r--)
                    gfx_rect_blend(g, px - r, ty - r, thumb_w[k] + 2 * r, thumb_h[k] + 2 * r,
                                   0x2DD4BFu, 30);
                gfx_rect(g, px - 2, ty - 2, thumb_w[k] + 4, thumb_h[k] + 4, C_TEAL);
            }
            gfx_rect_blend(g, px + 4, ty + 6, thumb_w[k], thumb_h[k], 0x000000u, 140); /* drop shadow */
            blit_thumb(g, k, px, ty);
            /* playtime badge: translucent bar across the bottom of the cover */
            double hrs = games[play_idx[pi]].hours;
            char hb[16];
            if (hrs >= 9.95) snprintf(hb, sizeof hb, "%.0fh", hrs);
            else if (hrs >= 0.05) snprintf(hb, sizeof hb, "%.1fh", hrs);
            else snprintf(hb, sizeof hb, "new");
            gfx_rect_blend(g, px + 4, ty + thumb_h[k] - 20, thumb_w[k] - 8, 18, 0x000000u, 170);
            gfx_text(g, &font_body, px + 9, ty + thumb_h[k] - 20, hb, C_TEAL);
        }
        /* scroll chevrons when there's more off-screen either side */
        if (np_off > 0)
            gfx_text(g, &font_title, 4, ty + THUMB / 2 - 14, "<", C_TEAL);
        if (np_off + nvis < g_nplaying)
            gfx_text_right(g, &font_title, g->w - 4, ty + THUMB / 2 - 14, ">", C_TEAL);
    }

    (void)backlog;
    /* ---- Stats & Tier List banner card (selectable) ---- */
    int bnY = 208, bnH = 30;
    if (g_onbanner) gfx_rect(g, 6, bnY - 2, g->w - 12, bnH + 4, C_TEAL);
    gfx_rect(g, 8, bnY, g->w - 16, bnH, g_onbanner ? C_SEL : C_TITLE);
    gfx_rect(g, 8, bnY, 4, bnH, C_TEAL);
    gfx_text(g, &font_body, 20, bnY + 5, "Stats & Tier List", g_onbanner ? C_TEXT : C_TEAL);
    snprintf(buf, sizeof buf, "%d beaten  %d%%  %d playing  >>", beaten, pct, playing);
    gfx_text_right(g, &font_body, g->w - 16, bnY + 5, buf, C_DIM);

    /* ---- console list: "All Games" first, then consoles ---- */
    for (int i = 0; i < g_hvis; i++) {
        int ci = htop + i;
        if (ci > ncons) break;                 /* entries: 0..ncons */
        int ry = HOME_TOP + i * HROW;
        int selrow = (ci == hsel && !g_onbanner && g_npsel < 0);
        if (selrow) {
            gfx_rect(g, 6, ry, g->w - 12, HROW - 2, C_SEL);
            gfx_rect(g, 6, ry, 4, HROW - 2, C_TEAL);       /* selected marker */
        }
        const char *nm; int n, bb, pp, aa;
        if (ci == 0) { nm = "All Games"; n = tot; bb = beaten; pp = playing; aa = aband; }
        else { int c = ci - 1; nm = cons[c].name; n = cons[c].count;
               bb = cons[c].beaten; pp = cons[c].playing; aa = cons[c].aband; }
        int pct = n ? (100 * bb / n) : 0;
        gfx_text(g, &font_body, 18, ry + 8, nm, selrow ? C_TEXT : C_DIM);
        snprintf(buf, sizeof buf, "%d/%d", bb, n);
        gfx_text(g, &font_body, 150, ry + 8, buf, C_DIM);
        int barx = 250, barw = g->w - barx - 64, barh = 16;
        draw_status_bar(g, barx, ry + 7, barw, barh, n, bb, pp, aa);
        snprintf(buf, sizeof buf, "%d%%", pct);
        gfx_text_right(g, &font_body, g->w - 14, ry + 8, buf, selrow ? C_TEXT : C_DIM);
    }
    if (ngames == 0)
        gfx_text(g, &font_body, 18, HOME_TOP + 8,
                 "No games found yet. Add ROMs to /Roms and reopen.", C_DIM);
    gfx_rect(g, 0, g->h - 26, g->w, 26, C_TITLE);
    if (g_npsel >= 0)
        gfx_text(g, &font_body, 10, g->h - 23, "A: open    L/R: pick    DOWN: back", C_DIM);
    else if (g_onbanner)
        gfx_text(g, &font_body, 10, g->h - 23, "A: open stats    UP/DOWN: move", C_DIM);
    else
        gfx_text(g, &font_body, 10, g->h - 23, "A: open   X: roll   Y: settings   SEL: quit", C_DIM);
    gfx_present(g);
}

/* page dots for the onboarding carousel */
static void onb_dots(Gfx *g, int n, int cur) {
    int dw = 10, gap = 8, total = n * dw + (n - 1) * gap, x = (g->w - total) / 2, y = g->h - 46;
    for (int i = 0; i < n; i++) { gfx_rect(g, x, y, dw, dw, i == cur ? C_TEAL : C_BARBG); x += dw + gap; }
}
#define ONB_CARDS 6
/* First-run onboarding: welcome, how-to, navigation, theme picker, favorites
 * sync (opt-in), done. Persists theme + favorites choice + the onboarded flag. */
static void run_onboarding(Gfx *g, int in, Tracker *t) {
    int card = 0, themecard = 3, favcard = 4, fav_choice = 0, cx = g->w / 2;
    #define CT(str, yy, col) gfx_text(g, &font_body, cx - gfx_text_w(&font_body, str) / 2, (yy), (str), (col))
    for (;;) {
        draw_bg(g);
        gfx_rect(g, 0, 0, g->w, 4, C_TEAL);
        if (card == 0) {
            int w = gfx_text_w(&font_title, "Mini Tracker") * 2;
            gfx_text_scaled(g, &font_title, cx - w / 2, 96, "Mini Tracker", C_TEAL, 2);
            CT("Track. Beat. Collect.", 168, C_PURPLE);
            CT("Your retro backlog, organized.", 222, C_TEXT);
            CT("Beat games, rank them, watch your stats grow.", 252, C_DIM);
        } else if (card == 1) {
            gfx_text(g, &font_title, cx - gfx_text_w(&font_title, "How it works") / 2, 58, "How it works", C_TEAL);
            CT("Press A on a game to set where it stands:", 116, C_TEXT);
            const char *L[4] = { "Backlog", "Playing", "Beaten", "Abandoned" };
            int ws[4], tot = 0, gap = 24;
            for (int i = 0; i < 4; i++) { ws[i] = 22 + gfx_text_w(&font_body, L[i]); tot += ws[i]; }
            tot += gap * 3;
            int x = cx - tot / 2, y = 160;
            for (int i = 0; i < 4; i++) { draw_status_chip(g, x, y, 16, i);
                gfx_text(g, &font_body, x + 22, y, L[i], C_TEXT); x += ws[i] + gap; }
            CT("Mark a game Beaten or Abandoned to give", 214, C_DIM);
            CT("it a tier rank (S to F) and build your tier list.", 240, C_DIM);
        } else if (card == 2) {
            gfx_text(g, &font_title, cx - gfx_text_w(&font_title, "Find your way") / 2, 58, "Find your way", C_TEAL);
            const char *key[4] = { "Now Playing", "Stats & Tier List", "X    Roll the dice", "Y    Settings" };
            const char *des[4] = { "Your in-progress games, pinned up top",
                                   "Open the banner on the home screen",
                                   "Pick a random game to play",
                                   "Themes, options and help" };
            int lx = 76, y = 112, lh = 46;
            for (int i = 0; i < 4; i++) {
                gfx_text(g, &font_body, lx, y, key[i], C_TEAL);
                gfx_text(g, &font_body, lx + 18, y + 21, des[i], C_DIM);
                y += lh;
            }
        } else if (card == themecard) {
            gfx_text(g, &font_title, cx - gfx_text_w(&font_title, "Pick a theme") / 2, 60, "Pick a theme", C_TEAL);
            int w = gfx_text_w(&font_title, THEMES[g_theme % N_THEMES].name) * 2;
            gfx_text_scaled(g, &font_title, cx - w / 2, 106, THEMES[g_theme % N_THEMES].name, C_TEAL, 2);
            int py = 176;
            gfx_rect(g, cx - 150, py, 300, 34, C_SEL);
            gfx_text(g, &font_body, cx - 138, py + 8, "Selected game row", C_TEXT);
            draw_status_chip(g, cx - 150, py + 48, 16, ST_BEATEN);
            draw_status_chip(g, cx - 122, py + 48, 16, ST_PLAYING);
            gfx_text(g, &font_body, cx - 92, py + 48, "accent", C_TEAL);
            gfx_text(g, &font_body, cx - 16, py + 48, "secondary", C_PURPLE);
            int dw = 18, gap = 12, total = N_THEMES * dw + (N_THEMES - 1) * gap, x = cx - total / 2, dy = py + 86;
            for (int i = 0; i < N_THEMES; i++) {
                if (i == g_theme) gfx_rect(g, x - 3, dy - 3, dw + 6, dw + 6, C_TEXT);
                gfx_rect(g, x, dy, dw, dw, THEMES[i].teal);
                x += dw + gap;
            }
            CT("L/R to preview", py + 120, C_DIM);
        } else if (card == favcard) {
            gfx_text(g, &font_title, cx - gfx_text_w(&font_title, "Favorites sync") / 2, 58, "Favorites sync", C_TEAL);
            CT("Games you mark Playing can appear in your", 114, C_TEXT);
            CT("OnionOS Favorites - a quick way to launch", 140, C_TEXT);
            CT("them from the system home screen.", 166, C_TEXT);
            CT("Your Beaten and Abandoned marks stay untouched.", 204, C_DIM);
            const char *opt[2] = { "Skip for now", "Enable sync" };
            int bw = 200, bh = 42, gap = 24, total = bw * 2 + gap, x = cx - total / 2, y = 248;
            for (int i = 0; i < 2; i++) {
                int s = (i == fav_choice);
                if (s) gfx_rect(g, x - 3, y - 3, bw + 6, bh + 6, C_TEAL);
                gfx_rect(g, x, y, bw, bh, s ? C_SEL : C_BARBG);
                gfx_text(g, &font_body, x + (bw - gfx_text_w(&font_body, opt[i])) / 2, y + 11, opt[i], s ? C_TEXT : C_DIM);
                x += bw + gap;
            }
        } else {
            int w = gfx_text_w(&font_title, "You're all set!") * 2;
            gfx_text_scaled(g, &font_title, cx - w / 2, 108, "You're all set!", C_TEAL, 2);
            CT("Press A on a game to begin.", 198, C_TEXT);
            CT("Change anything later in Settings (Y).", 230, C_DIM);
        }
        gfx_rect(g, 0, g->h - 26, g->w, 26, C_TITLE);
        const char *hint = (card == ONB_CARDS - 1) ? "A: start    B: back"
                         : (card == favcard)  ? "L/R: choose   A: confirm   B: back   START: skip"
                         : (card == themecard) ? "L/R: theme   A: next   B: back   START: skip"
                         : "A: next    B: back    START: skip";
        gfx_text(g, &font_body, 10, g->h - 23, hint, C_DIM);
        onb_dots(g, ONB_CARDS, card);
        gfx_present(g);

        int b = input_get(in, 1000);
        if (b == PAD_NONE) continue;
        if (b == PAD_START || b == PAD_SELECT) break;                 /* skip all */
        if (card == themecard && (b == PAD_LEFT || b == PAD_L1))  { g_theme = (g_theme + N_THEMES - 1) % N_THEMES; apply_theme(); continue; }
        if (card == themecard && (b == PAD_RIGHT || b == PAD_R1)) { g_theme = (g_theme + 1) % N_THEMES; apply_theme(); continue; }
        if (card == favcard && (b == PAD_LEFT || b == PAD_RIGHT || b == PAD_L1 || b == PAD_R1)) { fav_choice = !fav_choice; continue; }
        if (b == PAD_A) {
            if (card == favcard && fav_choice && !g_favsync) {        /* confirm sync, here only */
                g_favsync = 1; trk_set_setting(t, "fav_sync", 1); favsync_enable(t);
            }
            if (card < ONB_CARDS - 1) card++; else break;
        }
        else if (b == PAD_B) { if (card > 0) card--; }
    }
    trk_set_setting(t, "theme", g_theme);
    trk_set_setting(t, "onboarded", 1);
    #undef CT
}

int main(int argc, char **argv) {
    if (argc < 2) { fprintf(stderr, "usage: gui <tracker.sqlite> [onion_snapshot] [roms_dir]\n"); return 2; }
    Tracker t;
    srand((unsigned)(time(NULL) ^ getpid()));   /* for the random "what to play" pick */
    g_now = (long)time(NULL);                    /* for "last played" relative times */
    if (trk_open(&t, argv[1]) != 0) return 1;
    if (argc >= 4 && argv[3][0]) { int n = 0; trk_scan_roms(&t, argv[3], &n); }    /* full library */
    if (argc >= 3 && argv[2][0]) {                                                 /* playtime overlay */
        int n = 0; const char *roms = (argc >= 4) ? argv[3] : NULL;
        trk_import_onion(&t, argv[2], roms, &n);
    }
    if (argc >= 4 && argv[3][0]) trk_prune_missing(&t, argv[3]);  /* drop deleted ROMs */
    load(&t);
    g_favsync = trk_get_setting(&t, "fav_sync", 0);   /* default OFF: don't touch user favorites */
    g_rumble  = trk_get_setting(&t, "rumble", 2);
    g_hero_on = trk_get_setting(&t, "hero", 1);
    g_sort    = trk_get_setting(&t, "sort", SORT_PLAYTIME);
    if (g_sort < 0 || g_sort >= SORT_NMODES) g_sort = SORT_PLAYTIME;
    g_theme   = trk_get_setting(&t, "theme", 0);
    if (g_theme < 0 || g_theme >= N_THEMES) g_theme = 0;
    g_view_default = trk_get_setting(&t, "view", 1) ? 1 : 0;   /* default: grid */
    g_show_hidden  = trk_get_setting(&t, "show_hidden", 0) ? 1 : 0;
    int first_run = !trk_get_setting(&t, "onboarded", 0);
    if (first_run) {                                   /* adaptive view: grid only if art-rich */
        int withart = 0;
        for (int i = 0; i < ngames; i++) if (games[i].img[0]) withart++;
        g_view_default = (ngames > 0 && withart * 2 >= ngames) ? 1 : 0;
        trk_set_setting(&t, "view", g_view_default);
    }
    int show_intro = first_run;
    {   /* one-shot sentinel beside the binary forces the intro once (then self-deletes) */
        char sp[480]; const char *sl = strrchr(argv[0], '/');
        if (sl) { snprintf(sp, sizeof sp, "%.*s/.show_intro", (int)(sl - argv[0]), argv[0]);
                  if (access(sp, F_OK) == 0) { show_intro = 1; unlink(sp); } }
    }
    apply_theme();
    if (argc >= 4 && argv[3][0]) {
        snprintf(g_roms, sizeof g_roms, "%s", argv[3]);
        snprintf(g_emu, sizeof g_emu, "%s", argv[3]);     /* derive Emu dir: .../Roms -> .../Emu */
        char *r = strrchr(g_emu, '/');
        if (r && !strcmp(r, "/Roms")) strcpy(r, "/Emu");
        build_emu_map(g_emu);
        if (g_favsync) favsync_reconcile(&t);   /* opt-in: mirror Playing <-> favourite.json */
    }

    Gfx g;
    if (gfx_init(&g) != 0) { fprintf(stderr, "gfx_init failed\n"); trk_close(&t); return 1; }
    g_scrw = g.w; g_scrh = g.h;                         /* resolution-aware layout */
    g_hvis = (g.h - HOME_TOP - 26) / HROW; if (g_hvis < 1) g_hvis = 1;
    build_thumbs();                                     /* builds the hero at real width */
    {   /* launch splash (logo next to the binary) */
        char sp[440]; const char *sl = strrchr(argv[0], '/');
        if (sl) snprintf(sp, sizeof sp, "%.*s/splash.png", (int)(sl - argv[0]), argv[0]);
        else snprintf(sp, sizeof sp, "splash.png");
        splash(&g, sp);
    }
    int in = input_open();
    if (in < 0) {               /* no input device -> can't navigate; bail cleanly */
        fprintf(stderr, "no input device (/dev/input/event0)\n");
        gfx_quit(&g); trk_close(&t); return 1;
    }
    signal(SIGCHLD, SIG_IGN);   /* auto-reap forked rumble/chime children */

    int screen = 0;          /* 0 = home (console select), 1 = game list */
    int hsel = 0, htop = 0;  /* home selection */
    int cc = 0, sel = 0, top = 0;
    int gmode = g_view_default, gtop = 0; /* gmode: 0 = list view, 1 = box-art grid */
    int launched = 0;        /* set when the user picks a Now Playing game to launch */
    long now = (long)time(NULL);

    console_quiet();                                  /* keep console off the framebuffer/keys */
    atexit(console_restore);                          /* belt-and-suspenders on any exit */
    signal(SIGTERM, on_fatal_signal); signal(SIGINT, on_fatal_signal);
    signal(SIGHUP,  on_fatal_signal); signal(SIGSEGV, on_fatal_signal);
    signal(SIGABRT, on_fatal_signal);
    if (show_intro) {                                 /* first-time welcome + setup */
        run_onboarding(&g, in, &t);
        gmode = g_view_default;
        build_thumbs();                               /* re-tint hero for the chosen theme */
    }
    draw_home(&g, hsel, htop);
    for (;;) {
        int b = input_get(in, 1000);
        if (b == PAD_NONE) continue;
        if (b == PAD_SELECT) break;

        if (screen == 0) {                 /* ---- HOME (All Games + consoles) ---- */
            if (b == PAD_Y) {              /* settings overlay (works even with no games) */
                int ssel = 0;
                draw_settings(&g, ssel);
                for (;;) {
                    int sb = input_get(in, 1000);
                    if (sb == PAD_NONE) continue;
                    if (sb == PAD_UP)        { ssel = (ssel - 1 + N_SETTINGS) % N_SETTINGS; draw_settings(&g, ssel); }
                    else if (sb == PAD_DOWN) { ssel = (ssel + 1) % N_SETTINGS; draw_settings(&g, ssel); }
                    else if (sb == PAD_A) {
                        switch (ssel) {
                            case 0:
                                if (!g_favsync) {                 /* enabling: confirm first */
                                    sync_favorites(g_roms);
                                    int adopt = 0, push = 0;
                                    for (int i = 0; i < ngames; i++) {
                                        if (games[i].favorite && games[i].status == ST_BACKLOG) adopt++;
                                        if (games[i].status == ST_PLAYING && !games[i].favorite) push++;
                                    }
                                    draw_favsync_confirm(&g, adopt, push);
                                    int ok = 0;
                                    for (;;) { int cb = input_get(in, 1000);
                                               if (cb == PAD_A) { ok = 1; break; }
                                               if (cb == PAD_B || cb == PAD_SELECT) { ok = 0; break; } }
                                    if (ok) { g_favsync = 1; trk_set_setting(&t, "fav_sync", 1); favsync_enable(&t); }
                                } else {                          /* disabling: no data change, no confirm */
                                    g_favsync = 0; trk_set_setting(&t, "fav_sync", 0);
                                }
                                break;
                            case 1: g_rumble = (g_rumble + 1) & 3; trk_set_setting(&t, "rumble", g_rumble); buzz_tap(); break;
                            case 2: g_sort = (g_sort + 1) % SORT_NMODES; trk_set_setting(&t, "sort", g_sort); break;
                            case 3: g_view_default = !g_view_default; trk_set_setting(&t, "view", g_view_default);
                                    gmode = g_view_default; break;   /* also flip current view */
                            case 4: g_show_hidden = !g_show_hidden; trk_set_setting(&t, "show_hidden", g_show_hidden);
                                    break;                            /* takes effect next time a list opens */
                            case 5: g_hero_on = !g_hero_on; trk_set_setting(&t, "hero", g_hero_on); break;
                            case 6: g_theme = (g_theme + 1) % N_THEMES; trk_set_setting(&t, "theme", g_theme);
                                    apply_theme(); build_thumbs(); break;   /* re-tint + rebuild hero */
                            case 7: draw_help(&g);
                                    for (;;) { int ab = input_get(in, 1000);
                                               if (ab == PAD_B || ab == PAD_SELECT || ab == PAD_A) break; }
                                    break;
                            case 8: draw_about(&g);
                                    for (;;) { int ab = input_get(in, 1000);
                                               if (ab == PAD_B || ab == PAD_SELECT || ab == PAD_A) break; }
                                    break;
                        }
                        draw_settings(&g, ssel);
                    } else if (sb == PAD_B || sb == PAD_SELECT) break;
                }
                draw_home(&g, hsel, htop); continue;
            }
            if (!ncons) continue;
            if (b == PAD_X && ngames) {    /* "What should I play?" random suggestion */
                int gi = pick_random_backlog();
                draw_surprise(&g, gi);
                int done = 0;
                while (!done) {
                    int sb = input_get(in, 1000);
                    if (sb == PAD_NONE) continue;
                    if (sb == PAD_X) { gi = pick_random_backlog(); draw_surprise(&g, gi); }
                    else if (sb == PAD_Y && gi >= 0) {
                        apply_status(&t, &games[gi], ST_PLAYING, now); draw_surprise(&g, gi);
                    } else if (sb == PAD_A && gi >= 0) {
                        if (request_launch(&games[gi])) {
                            draw_launching(&g, games[gi].name); launched = 1; done = 1;
                        }
                    } else if (sb == PAD_B || sb == PAD_SELECT) done = 1;
                }
                if (launched) break;
                draw_home(&g, hsel, htop);
                continue;
            }
            if (g_npsel >= 0) {            /* ---- focus on the Now Playing strip ---- */
                if (b == PAD_LEFT || b == PAD_L1)       { if (g_npsel > 0) g_npsel--; }
                else if (b == PAD_RIGHT || b == PAD_R1) { if (g_npsel < g_nplaying - 1) g_npsel++; }
                else if (b == PAD_DOWN || b == PAD_B)   { g_npsel = -1; g_onbanner = 1; }  /* -> banner */
                else if (b == PAD_A) {                  /* open the game popup */
                    Game *lg = &games[play_idx[g_npsel]];
                    if (open_detail(&g, in, &t, lg, -1)) { draw_launching(&g, lg->name); launched = 1; break; }
                    if (g_nplaying == 0) { g_npsel = -1; g_onbanner = 1; }
                    else if (g_npsel >= g_nplaying) g_npsel = g_nplaying - 1;
                }
                if (g_npsel >= 0) {                      /* keep the focused cover on screen */
                    if (g_npsel < np_off) np_off = g_npsel;
                    if (g_npsel >= np_off + NPVIS) np_off = g_npsel - NPVIS + 1;
                }
                draw_home(&g, hsel, htop);
                continue;
            }
            if (g_onbanner) {              /* ---- focus on the Stats banner ---- */
                if (b == PAD_UP) { if (g_nplaying > 0) { g_onbanner = 0; g_npsel = 0; np_off = 0; } }
                else if (b == PAD_DOWN) { g_onbanner = 0; }       /* -> console list */
                else if (b == PAD_A) { if (run_stats(&g, in, &t)) { launched = 1; break; } }
                draw_home(&g, hsel, htop);
                continue;
            }
            if (b == PAD_UP) {
                if (hsel > 0) hsel--;
                else g_onbanner = 1;                              /* top of list -> banner */
            }
            else if (b == PAD_DOWN) { if (hsel < ncons) hsel++; }   /* 0..ncons */
            else if (b == PAD_A) {
                cc = (hsel == 0) ? -1 : hsel - 1;                    /* -1 = All Games */
                sel = 0; top = 0; gtop = 0; screen = 1;
                build_view(cc);
                if (gmode) draw_grid(&g, cc, sel, gtop);
                else       draw_list(&g, cc, sel, top);
                continue;
            }
            if (hsel < htop) htop = hsel;
            if (hsel >= htop + g_hvis) htop = hsel - g_hvis + 1;
            draw_home(&g, hsel, htop);
            continue;
        }

        /* ---- GAME LIST / GRID ---- */
        if (!ncons) { screen = 0; draw_home(&g, hsel, htop); continue; }

        if (b == PAD_Y) {                  /* toggle list <-> box-art grid */
            gmode = !gmode;
            if (gmode) {
                int gv = grid_vis_rows(&g), srow = sel / GCOLS;
                gtop = srow - gv + 1; if (gtop < 0) gtop = 0;
                if (srow < gtop) gtop = srow;
                draw_grid(&g, cc, sel, gtop);
            } else {
                if (sel < top) top = sel;
                if (sel >= top + VIS) top = sel - VIS + 1;
                draw_list(&g, cc, sel, top);
            }
            continue;
        }

        if (gmode == 1) {                  /* ---- GRID navigation ---- */
            switch (b) {
                case PAD_UP:    if (sel - GCOLS >= 0) sel -= GCOLS; break;
                case PAD_DOWN:  if (sel + GCOLS < nview) sel += GCOLS;
                                else if (sel < nview - 1) sel = nview - 1; break;
                case PAD_LEFT:  if (sel > 0) sel--; break;
                case PAD_RIGHT: if (sel < nview - 1) sel++; break;
                case PAD_L1:    cc = ((cc + 1 - 1 + (ncons + 1)) % (ncons + 1)) - 1;
                                sel = 0; gtop = 0; build_view(cc); break;
                case PAD_R1:    cc = ((cc + 1 + 1) % (ncons + 1)) - 1;
                                sel = 0; gtop = 0; build_view(cc); break;
                case PAD_X:     g_sort = (g_sort + 1) % SORT_NMODES; trk_set_setting(&t, "sort", g_sort);
                                sel = 0; gtop = 0; build_view(cc); break;
                case PAD_B: case PAD_START:
                    hsel = (cc < 0) ? 0 : cc + 1;
                    if (hsel < htop) htop = hsel;
                    if (hsel >= htop + g_hvis) htop = hsel - g_hvis + 1;
                    screen = 0; draw_home(&g, hsel, htop); continue;
                case PAD_A:
                    if (nview && open_detail(&g, in, &t, &games[view[sel]], cc)) {
                        draw_launching(&g, games[view[sel]].name); launched = 1;
                    }
                    break;
                default: break;
            }
            if (launched) break;
            if (sel >= nview) sel = nview > 0 ? nview - 1 : 0;   /* hiding may shrink view */
            { int gv = grid_vis_rows(&g), srow = sel / GCOLS;
              if (srow < gtop) gtop = srow;
              if (srow >= gtop + gv) gtop = srow - gv + 1; }
            draw_grid(&g, cc, sel, gtop);
            continue;
        }

        switch (b) {                       /* ---- LIST navigation ---- */
            case PAD_UP:    if (sel > 0) sel--; break;
            case PAD_DOWN:  if (sel < nview - 1) sel++; break;
            case PAD_LEFT: case PAD_L1:        /* cycle sources: All, then each console */
                cc = ((cc + 1 - 1 + (ncons + 1)) % (ncons + 1)) - 1; sel = 0; top = 0; build_view(cc); break;
            case PAD_RIGHT: case PAD_R1:
                cc = ((cc + 1 + 1) % (ncons + 1)) - 1; sel = 0; top = 0; build_view(cc); break;
            case PAD_X:                         /* cycle sort mode (persisted) */
                g_sort = (g_sort + 1) % SORT_NMODES; trk_set_setting(&t, "sort", g_sort);
                sel = 0; top = 0; build_view(cc); break;
            case PAD_B: case PAD_START:        /* back to home menu */
                hsel = (cc < 0) ? 0 : cc + 1;
                if (hsel < htop) htop = hsel;
                if (hsel >= htop + g_hvis) htop = hsel - g_hvis + 1;
                screen = 0; draw_home(&g, hsel, htop); continue;
            case PAD_A:
                if (nview && open_detail(&g, in, &t, &games[view[sel]], cc)) {
                    draw_launching(&g, games[view[sel]].name); launched = 1;
                }
                break;
            default: break;
        }
        if (launched) break;
        if (sel >= nview) sel = nview > 0 ? nview - 1 : 0;   /* hiding may shrink view */
        if (sel < top) top = sel;
        if (sel >= top + VIS) top = sel - VIS + 1;
        draw_list(&g, cc, sel, top);
    }

    console_restore();
    input_close(in);
    gfx_quit(&g);
    trk_close(&t);
    return launched ? 10 : 0;   /* 10 = our launch.sh should run /tmp/mt_cmd */
}
