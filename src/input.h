/* input.h — Miyoo Mini Plus button input via /dev/input/event0.
 * Keycodes confirmed on hardware (see miyoo-display-input-facts). */
#ifndef INPUT_H
#define INPUT_H
#include <stdio.h>

enum {
    PAD_NONE = 0,
    PAD_UP, PAD_DOWN, PAD_LEFT, PAD_RIGHT,
    PAD_A, PAD_B, PAD_X, PAD_Y,
    PAD_L1, PAD_R1, PAD_L2, PAD_R2,
    PAD_START, PAD_SELECT
};

int input_open(void);                 /* returns fd, or -1 */
void input_close(int fd);

/* DEBUG diagnostics, set by input_get for the on-screen readout / log. */
extern int input_press_code;  /* keycode of last PRESS this call (-1 if none) */
extern int input_press_btn;   /* mapped PAD_* of that press */
extern FILE *input_log;       /* if non-NULL, every raw event is logged here */
/* Wait up to timeout_ms for a button event. On a press, returns the PAD_* code;
 * returns PAD_NONE on timeout or non-press events. */
int input_get(int fd, int timeout_ms);

#endif
