/* keytest.c — guided button-mapping tool. Prompts for each physical button,
 * captures the exact /dev/input/event0 keycode, shows it, and logs it.
 * Gives ground-truth mapping so input.c is correct (no guessing). */
#include "gfx.h"
#include <linux/input.h>
#include <fcntl.h>
#include <unistd.h>
#include <poll.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

static const char *LABELS[] = {
    "UP", "DOWN", "LEFT", "RIGHT",
    "A", "B", "X", "Y",
    "L1", "R1", "L2", "R2",
    "START", "SELECT"
};
#define N ((int)(sizeof LABELS / sizeof LABELS[0]))

static void drain(int fd, int ms) {  /* flush pending events (e.g. the release) */
    time_t t0 = time(0);
    while (time(0) - t0 < (ms / 1000) + 1) {
        struct pollfd p = { fd, POLLIN, 0 };
        if (poll(&p, 1, 150) <= 0) return;
        struct input_event ev;
        if (read(fd, &ev, sizeof ev) != (int)sizeof ev) return;
    }
}

int main(int argc, char **argv) {
    char logp[512];
    snprintf(logp, sizeof logp, "%s/keytest_log.txt", argc > 1 ? argv[1] : "/mnt/SDCARD/App/KeyTest");
    FILE *log = fopen(logp, "w");

    Gfx g;
    if (gfx_init(&g) != 0) { if (log) { fprintf(log, "gfx_init failed\n"); fclose(log); } return 1; }
    int in = open("/dev/input/event0", O_RDONLY);

    for (int i = 0; i < N; i++) {
        gfx_clear(&g, 0xFF0D0D14u);
        gfx_text(&g, &font_body, 30, 80, "Button mapping test", 0xFF8A8CA0u);
        gfx_text(&g, &font_title, 30, 150, "Press:", 0xFF2DD4BFu);
        gfx_text(&g, &font_title, 200, 150, LABELS[i], 0xFFEDEDF2u);
        char hint[64];
        snprintf(hint, sizeof hint, "%d / %d   (if a button is missing, just wait ~15s)", i + 1, N);
        gfx_text(&g, &font_body, 30, 230, hint, 0xFF8A8CA0u);
        gfx_present(&g);

        int code = -1;
        time_t t0 = time(0);
        while (time(0) - t0 < 15) {
            struct pollfd p = { in, POLLIN, 0 };
            if (poll(&p, 1, 300) <= 0) continue;
            struct input_event ev;
            if (read(in, &ev, sizeof ev) != (int)sizeof ev) continue;
            if (ev.type == EV_KEY && ev.value == 1) { code = ev.code; break; }
        }
        if (log) { fprintf(log, "%s=%d\n", LABELS[i], code); fflush(log); }

        gfx_clear(&g, 0xFF0D0D14u);
        char msg[64];
        if (code >= 0) snprintf(msg, sizeof msg, "%s = code %d", LABELS[i], code);
        else           snprintf(msg, sizeof msg, "%s = (skipped)", LABELS[i]);
        gfx_text(&g, &font_title, 30, 150, msg, 0xFF2DD4BFu);
        gfx_present(&g);
        drain(in, 800);
    }

    gfx_clear(&g, 0xFF0D0D14u);
    gfx_text(&g, &font_title, 30, 150, "Done!  Exiting...", 0xFF2DD4BFu);
    gfx_present(&g);
    if (log) fclose(log);
    if (in >= 0) close(in);
    gfx_quit(&g);
    return 0;
}
