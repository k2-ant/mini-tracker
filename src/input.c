#include "input.h"
#include <linux/input.h>
#include <fcntl.h>
#include <unistd.h>
#include <poll.h>

int input_open(void) {
    /* non-blocking so input_get can drain the queue to EAGAIN each wake */
    return open("/dev/input/event0", O_RDONLY | O_NONBLOCK);
}
void input_close(int fd) {
    if (fd >= 0) close(fd);
}

static int map(int code) {
    switch (code) {
        case 103: return PAD_UP;
        case 108: return PAD_DOWN;
        case 105: return PAD_LEFT;
        case 106: return PAD_RIGHT;
        case 57:  return PAD_A;       /* SPACE */
        case 29:  return PAD_B;       /* LEFTCTRL */
        case 42:  return PAD_X;       /* LEFTSHIFT */
        case 56:  return PAD_Y;       /* LEFTALT */
        case 18:  return PAD_L1;      /* E */
        case 20:  return PAD_R1;      /* T */
        case 15:  return PAD_L2;      /* TAB */
        case 14:  return PAD_R2;      /* BACKSPACE */
        case 28:  return PAD_START;   /* ENTER */
        case 97:  return PAD_SELECT;  /* RIGHTCTRL */
        default:  return PAD_NONE;
    }
}

int input_press_code = -1;  /* DEBUG: keycode of last PRESS this call (-1 if none) */
int input_press_btn = -1;   /* DEBUG: mapped PAD_* of that press */
FILE *input_log = NULL;      /* DEBUG: if set, every raw event is logged */

int input_get(int fd, int timeout_ms) {
    input_press_code = -1; input_press_btn = -1;
    struct pollfd pfd = { .fd = fd, .events = POLLIN };
    if (poll(&pfd, 1, timeout_ms) <= 0) return PAD_NONE;

    /* Drain ALL queued events this wake (prevents desync under slow redraws).
     * Return the first mapped key-press encountered; keep draining the rest. */
    struct input_event ev;
    int result = PAD_NONE;
    while (read(fd, &ev, sizeof ev) == (int)sizeof ev) {
        int press = (ev.type == EV_KEY && ev.value == 1);
        int b = press ? map(ev.code) : PAD_NONE;
        if (press && input_press_code < 0) { input_press_code = ev.code; input_press_btn = b; }
        if (input_log) {
            fprintf(input_log, "t=%d c=%d v=%d b=%d\n", ev.type, ev.code, ev.value, b);
            fflush(input_log);
        }
        if (b != PAD_NONE && result == PAD_NONE) result = b;
    }
    return result;
}
