#include "gfx.h"
#include <stdio.h>
#include <linux/fb.h>
#include <sys/mman.h>
#include <sys/ioctl.h>
#include <fcntl.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>

int gfx_init(Gfx *g) {
    memset(g, 0, sizeof *g);
    g->fbfd = open("/dev/fb0", O_RDWR);
    if (g->fbfd < 0) return -1;
    struct fb_var_screeninfo vinfo;
    struct fb_fix_screeninfo finfo;
    if (ioctl(g->fbfd, FBIOGET_VSCREENINFO, &vinfo) < 0) return -1;
    if (ioctl(g->fbfd, FBIOGET_FSCREENINFO, &finfo) < 0) return -1;
    if (vinfo.bits_per_pixel != 32) {          /* we only render 32-bit ARGB8888 */
        fprintf(stderr, "gfx: unsupported framebuffer depth %ubpp (need 32)\n",
                vinfo.bits_per_pixel);
        close(g->fbfd); g->fbfd = -1; return -1;
    }
    g->w = vinfo.xres;
    g->h = vinfo.yres;
    g->line_bytes = finfo.line_length;
    g->maplen = finfo.smem_len ? finfo.smem_len : (unsigned long)finfo.line_length * vinfo.yres_virtual;
    g->fbmem = mmap(0, g->maplen, PROT_READ | PROT_WRITE, MAP_SHARED, g->fbfd, 0);
    if (g->fbmem == MAP_FAILED) { g->fbmem = 0; return -1; }
    g->back = calloc((size_t)g->w * g->h, sizeof(uint32_t));
    if (!g->back) return -1;
    return 0;
}

void gfx_quit(Gfx *g) {
    if (g->back) free(g->back);
    if (g->fbmem) munmap(g->fbmem, g->maplen);
    if (g->fbfd >= 0) close(g->fbfd);
    memset(g, 0, sizeof *g);
}

void gfx_clear(Gfx *g, uint32_t color) {
    int n = g->w * g->h;
    for (int i = 0; i < n; i++) g->back[i] = color;
}

void gfx_rect(Gfx *g, int x, int y, int w, int h, uint32_t color) {
    int x1 = x + w, y1 = y + h;
    if (x < 0) x = 0; if (y < 0) y = 0;
    if (x1 > g->w) x1 = g->w; if (y1 > g->h) y1 = g->h;
    for (int yy = y; yy < y1; yy++) {
        uint32_t *row = g->back + (size_t)yy * g->w;
        for (int xx = x; xx < x1; xx++) row[xx] = color;
    }
}

void gfx_hline(Gfx *g, int x, int y, int w, uint32_t color) {
    gfx_rect(g, x, y, w, 1, color);
}

void gfx_rect_blend(Gfx *g, int x, int y, int w, int h, uint32_t color, int alpha) {
    if (alpha <= 0) return;
    if (alpha > 255) alpha = 255;
    int x1 = x + w, y1 = y + h;
    if (x < 0) x = 0; if (y < 0) y = 0;
    if (x1 > g->w) x1 = g->w; if (y1 > g->h) y1 = g->h;
    unsigned a = (unsigned)alpha, ia = 255 - a;
    unsigned cr = (color >> 16) & 0xFF, cg = (color >> 8) & 0xFF, cb = color & 0xFF;
    for (int yy = y; yy < y1; yy++) {
        uint32_t *row = g->back + (size_t)yy * g->w;
        for (int xx = x; xx < x1; xx++) {
            uint32_t bg = row[xx];
            unsigned r  = (cr * a + ((bg >> 16) & 0xFF) * ia) / 255;
            unsigned gg = (cg * a + ((bg >> 8) & 0xFF) * ia) / 255;
            unsigned b  = (cb * a + (bg & 0xFF) * ia) / 255;
            row[xx] = 0xFF000000u | (r << 16) | (gg << 8) | b;
        }
    }
}

static inline void blend(uint32_t *dst, uint32_t fg, unsigned a) {
    if (a == 0) return;
    if (a >= 255) { *dst = fg; return; }
    uint32_t bg = *dst;
    unsigned ia = 255 - a;
    unsigned r = (((fg >> 16) & 0xFF) * a + ((bg >> 16) & 0xFF) * ia) / 255;
    unsigned gg = (((fg >> 8) & 0xFF) * a + ((bg >> 8) & 0xFF) * ia) / 255;
    unsigned b = (((fg) & 0xFF) * a + ((bg) & 0xFF) * ia) / 255;
    *dst = 0xFF000000u | (r << 16) | (gg << 8) | b;
}

int gfx_text(Gfx *g, const Font *f, int x, int y, const char *s, uint32_t color) {
    for (; *s; s++) {
        unsigned char c = (unsigned char)*s;
        if (c < f->first || c >= f->first + f->count) { x += f->w; continue; }
        const unsigned char *gl = f->data + (size_t)(c - f->first) * f->w * f->h;
        for (int gy = 0; gy < f->h; gy++) {
            int py = y + gy;
            if (py < 0 || py >= g->h) continue;
            uint32_t *row = g->back + (size_t)py * g->w;
            const unsigned char *grow = gl + (size_t)gy * f->w;
            for (int gx = 0; gx < f->w; gx++) {
                int px = x + gx;
                if (px < 0 || px >= g->w) continue;
                blend(&row[px], color, grow[gx]);
            }
        }
        x += f->w;
    }
    return x;
}

int gfx_text_w(const Font *f, const char *s) {
    int n = 0;
    while (*s++) n++;
    return n * f->w;
}

/* Integer-scaled text (each glyph pixel drawn as sc*sc block) for big headlines. */
int gfx_text_scaled(Gfx *g, const Font *f, int x, int y, const char *s, uint32_t color, int sc) {
    if (sc < 1) sc = 1;
    for (; *s; s++) {
        unsigned char c = (unsigned char)*s;
        if (c < f->first || c >= f->first + f->count) { x += f->w * sc; continue; }
        const unsigned char *gl = f->data + (size_t)(c - f->first) * f->w * f->h;
        for (int gy = 0; gy < f->h; gy++) {
            const unsigned char *grow = gl + (size_t)gy * f->w;
            for (int gx = 0; gx < f->w; gx++) {
                unsigned a = grow[gx];
                if (!a) continue;
                for (int yy = 0; yy < sc; yy++) {
                    int py = y + gy * sc + yy;
                    if (py < 0 || py >= g->h) continue;
                    uint32_t *row = g->back + (size_t)py * g->w;
                    for (int xx = 0; xx < sc; xx++) {
                        int px = x + gx * sc + xx;
                        if (px < 0 || px >= g->w) continue;
                        blend(&row[px], color, a);
                    }
                }
            }
        }
        x += f->w * sc;
    }
    return x;
}

void gfx_text_right(Gfx *g, const Font *f, int xr, int y, const char *s, uint32_t color) {
    gfx_text(g, f, xr - gfx_text_w(f, s), y, s, color);
}

/* Present: panel is rotated 180°, stride == w*4 (no padding), so the visible
 * framebuffer is contiguous and a 180° rotation is a full reversal. */
void gfx_present(Gfx *g) {
    uint32_t *fb = (uint32_t *)g->fbmem;
    int n = g->w * g->h;
    if (g->line_bytes == g->w * 4) {
        for (int i = 0; i < n; i++) fb[i] = g->back[n - 1 - i];
    } else {
        /* fallback for padded stride */
        for (int y = 0; y < g->h; y++)
            for (int x = 0; x < g->w; x++) {
                int dx = g->w - 1 - x, dy = g->h - 1 - y;
                *(uint32_t *)(g->fbmem + (size_t)dy * g->line_bytes + dx * 4) =
                    g->back[(size_t)y * g->w + x];
            }
    }
}
