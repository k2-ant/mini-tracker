/* fbtest.c — framebuffer + input smoke test for the Miyoo Mini Plus.
 *
 * Purpose: prove the GUI foundation before building it.
 *   1. mmap /dev/fb0 and paint an orientation test pattern:
 *        - 4 colored corners: RED=top-left, GREEN=top-right,
 *          BLUE=bottom-left, WHITE=bottom-right (as the code intends them).
 *        - a magenta horizontal bar and cyan vertical bar through the middle.
 *      Whatever the USER sees tells us the real orientation/rotation.
 *   2. Read /dev/input/event0 and log every key code/value to a file, and
 *      flash a small block on screen so input is visibly working.
 *   3. Auto-exit after ~25s (no way to get stuck), restoring nothing — the
 *      OS repaints when it resumes.
 *
 * Writes a report to: <dir-of-argv0>/fbtest_log.txt  (pulled over SSH after).
 * Fully static musl build — no libraries needed on the device.
 */
#include <linux/fb.h>
#include <linux/input.h>
#include <sys/mman.h>
#include <sys/ioctl.h>
#include <fcntl.h>
#include <unistd.h>
#include <poll.h>
#include <time.h>
#include <stdio.h>
#include <stdint.h>
#include <string.h>

static struct fb_var_screeninfo vinfo;
static struct fb_fix_screeninfo finfo;
static uint8_t *fb;        /* mmap base */
static uint8_t *draw;      /* visible buffer start (accounts for yoffset) */

static uint32_t pack(int r, int g, int b) {
    return ((uint32_t)(r) << vinfo.red.offset) |
           ((uint32_t)(g) << vinfo.green.offset) |
           ((uint32_t)(b) << vinfo.blue.offset) |
           (vinfo.transp.length ? (0xFFu << vinfo.transp.offset) : 0);
}

static void put(int x, int y, uint32_t px) {
    if (x < 0 || y < 0 || x >= (int)vinfo.xres || y >= (int)vinfo.yres) return;
    *(uint32_t *)(draw + y * finfo.line_length + x * 4) = px;
}

static void fill(int x0, int y0, int w, int h, uint32_t px) {
    for (int y = y0; y < y0 + h; y++)
        for (int x = x0; x < x0 + w; x++)
            put(x, y, px);
}

int main(int argc, char **argv) {
    char logpath[512];
    snprintf(logpath, sizeof logpath, "%s/fbtest_log.txt",
             argc > 1 ? argv[1] : "/mnt/SDCARD/App/FBTest");
    FILE *log = fopen(logpath, "w");

    int fd = open("/dev/fb0", O_RDWR);
    if (fd < 0) { if (log) fprintf(log, "ERR: cannot open /dev/fb0\n"), fclose(log); return 1; }
    ioctl(fd, FBIOGET_VSCREENINFO, &vinfo);
    ioctl(fd, FBIOGET_FSCREENINFO, &finfo);

    if (log) {
        fprintf(log, "xres=%u yres=%u bpp=%u\n", vinfo.xres, vinfo.yres, vinfo.bits_per_pixel);
        fprintf(log, "xres_virtual=%u yres_virtual=%u xoffset=%u yoffset=%u\n",
                vinfo.xres_virtual, vinfo.yres_virtual, vinfo.xoffset, vinfo.yoffset);
        fprintf(log, "line_length=%u smem_len=%u\n", finfo.line_length, finfo.smem_len);
        fprintf(log, "RGBA offsets: r=%u g=%u b=%u a=%u (lengths %u/%u/%u/%u)\n",
                vinfo.red.offset, vinfo.green.offset, vinfo.blue.offset, vinfo.transp.offset,
                vinfo.red.length, vinfo.green.length, vinfo.blue.length, vinfo.transp.length);
        fflush(log);
    }

    size_t maplen = finfo.smem_len ? finfo.smem_len : finfo.line_length * vinfo.yres_virtual;
    fb = mmap(0, maplen, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (fb == MAP_FAILED) { if (log) fprintf(log, "ERR: mmap failed\n"), fclose(log); close(fd); return 1; }
    draw = fb + vinfo.yoffset * finfo.line_length + vinfo.xoffset * 4;

    int W = vinfo.xres, H = vinfo.yres, C = 80;
    fill(0, 0, W, H, pack(30, 30, 40));               /* dark bg */
    fill(0, 0, C, C, pack(220, 40, 40));              /* TL red   */
    fill(W - C, 0, C, C, pack(40, 200, 60));          /* TR green */
    fill(0, H - C, C, C, pack(50, 90, 230));          /* BL blue  */
    fill(W - C, H - C, C, C, pack(240, 240, 240));    /* BR white */
    fill(0, H / 2 - 6, W, 12, pack(220, 40, 220));    /* H magenta bar */
    fill(W / 2 - 6, 0, 12, H, pack(40, 220, 220));    /* V cyan bar    */
    msync(fb, maplen, MS_SYNC);

    /* read input ~25s, log keycodes, flash a marker per press */
    int in = open("/dev/input/event0", O_RDONLY);
    if (log) fprintf(log, "input fd=%d\n-- key events (type,code,value) --\n", in), fflush(log);
    struct pollfd pfd = { .fd = in, .events = POLLIN };
    time_t start = time(0);
    int flip = 0;
    while (time(0) - start < 25 && in >= 0) {
        if (poll(&pfd, 1, 500) > 0) {
            struct input_event ev;
            if (read(in, &ev, sizeof ev) == (int)sizeof ev) {
                if (ev.type == EV_KEY) {
                    if (log) fprintf(log, "KEY code=%u value=%d\n", ev.code, ev.value), fflush(log);
                    if (ev.value == 1) {   /* press: flash a moving block */
                        fill(W / 2 - 40 + (flip % 5) * 18, H / 2 + 30, 14, 14, pack(255, 220, 0));
                        flip++;
                        msync(fb, maplen, MS_SYNC);
                    }
                }
            }
        }
    }
    if (log) fprintf(log, "-- done --\n"), fclose(log);
    if (in >= 0) close(in);
    munmap(fb, maplen);
    close(fd);
    return 0;
}
