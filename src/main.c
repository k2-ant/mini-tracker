/* main.c — CLI driver for testing the tracker core on the Mac.
 * The SDL2 GUI will link tracker.c directly and NOT use this file.
 *
 * Usage:
 *   tracker import <onion_db> <tracker_db>      refresh games from OnionOS data
 *   tracker stats  <tracker_db>                 print the dashboard
 *   tracker list   <tracker_db> [console]       list games (optionally one console)
 *   tracker set    <tracker_db> <rom_path> <backlog|playing|beaten|abandoned>
 *   tracker rate   <tracker_db> <rom_path> <1-5>
 */
#include "tracker.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static int usage(void) {
    fprintf(stderr,
        "usage:\n"
        "  tracker scan   <roms_dir> <tracker_db>\n"
        "  tracker import <onion_db> <tracker_db> [roms_dir]\n"
        "  tracker stats  <tracker_db>\n"
        "  tracker list   <tracker_db> [console]\n"
        "  tracker set    <tracker_db> <rom_path> <status>\n"
        "  tracker rate   <tracker_db> <rom_path> <1-5>\n"
        "  tracker menu   <tracker_db>\n");
    return 2;
}

int main(int argc, char **argv) {
    if (argc < 3) return usage();
    const char *cmd = argv[1];
    long now = (long)time(NULL);

    if (strcmp(cmd, "import") == 0) {
        if (argc < 4) return usage();
        Tracker t;
        if (trk_open(&t, argv[3]) != 0) return 1;
        int n = 0;
        const char *roms = (argc >= 5) ? argv[4] : "/mnt/SDCARD/Roms";
        int rc = trk_import_onion(&t, argv[2], roms, &n);
        if (rc == 0) printf("Imported/refreshed %d games into %s\n", n, argv[3]);
        trk_close(&t);
        return rc == 0 ? 0 : 1;
    }
    if (strcmp(cmd, "scan") == 0) {
        if (argc != 4) return usage();
        Tracker t;
        if (trk_open(&t, argv[3]) != 0) return 1;
        int n = 0;
        int rc = trk_scan_roms(&t, argv[2], &n);
        if (rc == 0) printf("Scanned %d ROM files from %s into %s\n", n, argv[2], argv[3]);
        trk_close(&t);
        return rc == 0 ? 0 : 1;
    }
    if (strcmp(cmd, "stats") == 0) {
        Tracker t;
        if (trk_open(&t, argv[2]) != 0) return 1;
        int rc = trk_print_stats(&t);
        trk_close(&t);
        return rc == 0 ? 0 : 1;
    }
    if (strcmp(cmd, "menu") == 0) {
        Tracker t;
        if (trk_open(&t, argv[2]) != 0) return 1;
        int rc = trk_run_menu(&t);
        trk_close(&t);
        return rc == 0 ? 0 : 1;
    }
    if (strcmp(cmd, "list") == 0) {
        Tracker t;
        if (trk_open(&t, argv[2]) != 0) return 1;
        int rc = trk_print_list(&t, argc >= 4 ? argv[3] : NULL);
        trk_close(&t);
        return rc == 0 ? 0 : 1;
    }
    if (strcmp(cmd, "set") == 0) {
        if (argc != 5) return usage();
        Tracker t;
        if (trk_open(&t, argv[2]) != 0) return 1;
        int rc = trk_set_status(&t, argv[3], argv[4], now);
        if (rc == 0) printf("Set %s -> %s\n", argv[3], argv[4]);
        else fprintf(stderr, "failed to set status\n");
        trk_close(&t);
        return rc == 0 ? 0 : 1;
    }
    if (strcmp(cmd, "rate") == 0) {
        if (argc != 5) return usage();
        Tracker t;
        if (trk_open(&t, argv[2]) != 0) return 1;
        int rc = trk_set_rating(&t, argv[3], atoi(argv[4]), now);
        if (rc == 0) printf("Rated %s -> %s/5\n", argv[3], argv[4]);
        else fprintf(stderr, "failed to set rating (must be 1-5)\n");
        trk_close(&t);
        return rc == 0 ? 0 : 1;
    }
    return usage();
}
