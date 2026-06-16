/* gfx.h — minimal framebuffer graphics for the Miyoo Mini Plus.
 * Draws into an offscreen ARGB buffer, then gfx_present() rotate-blits 180°
 * to /dev/fb0 (the panel is mounted upside-down — see miyoo-display-input-facts). */
#ifndef GFX_H
#define GFX_H
#include <stdint.h>
#include "font.h"

typedef struct {
    int w, h;            /* logical screen (640x480) */
    uint32_t *back;      /* offscreen buffer, w*h ARGB (0xAARRGGBB) */
    uint8_t  *fbmem;     /* mmap'd framebuffer */
    int fbfd;
    unsigned long maplen;
    int line_bytes;      /* fb stride */
} Gfx;

int  gfx_init(Gfx *g);
void gfx_quit(Gfx *g);

void gfx_clear(Gfx *g, uint32_t color);
void gfx_rect(Gfx *g, int x, int y, int w, int h, uint32_t color);
void gfx_hline(Gfx *g, int x, int y, int w, uint32_t color);
/* blend `color` over the back buffer at `alpha` (0-255) */
void gfx_rect_blend(Gfx *g, int x, int y, int w, int h, uint32_t color, int alpha);

/* Draw text; returns x advanced to. align: text is left-anchored at x,y (top-left). */
int  gfx_text(Gfx *g, const Font *f, int x, int y, const char *s, uint32_t color);
int  gfx_text_w(const Font *f, const char *s);          /* pixel width of s */
int  gfx_text_scaled(Gfx *g, const Font *f, int x, int y, const char *s, uint32_t color, int sc);
void gfx_text_right(Gfx *g, const Font *f, int xr, int y, const char *s, uint32_t color);

void gfx_present(Gfx *g);

#endif
