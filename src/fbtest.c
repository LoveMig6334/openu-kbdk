/* fbtest.c - framebuffer probe + draw demo for the KidBright µAI (Allwinner V831).
 *
 * Queries /dev/fb0 geometry at runtime (FBIOGET_VSCREENINFO/FSCREENINFO), prints
 * it, then mmaps and paints an unambiguous test pattern so we can confirm the
 * panel on the physical screen: 8 vertical colour bars, a white border, and a
 * diagonal so orientation is obvious. Pixel packing is derived from the var
 * bitfields, so it is correct regardless of RGB/BGR byte order or 16/24/32 bpp.
 *
 * Usage: fbtest [/dev/fbN]   (default /dev/fb0)
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <linux/fb.h>

static uint32_t pack(struct fb_var_screeninfo *v, uint8_t r, uint8_t g, uint8_t b){
    uint32_t rv = (uint32_t)(r >> (8 - v->red.length))   << v->red.offset;
    uint32_t gv = (uint32_t)(g >> (8 - v->green.length)) << v->green.offset;
    uint32_t bv = (uint32_t)(b >> (8 - v->blue.length))  << v->blue.offset;
    return rv | gv | bv;
}

int main(int argc, char **argv){
    const char *dev = argc > 1 ? argv[1] : "/dev/fb0";
    int fd = open(dev, O_RDWR);
    if (fd < 0){ fprintf(stderr, "open %s: %s\n", dev, strerror(errno)); return 1; }

    struct fb_var_screeninfo v;
    struct fb_fix_screeninfo f;
    if (ioctl(fd, FBIOGET_VSCREENINFO, &v) < 0){ perror("VSCREENINFO"); return 1; }
    if (ioctl(fd, FBIOGET_FSCREENINFO, &f) < 0){ perror("FSCREENINFO"); return 1; }

    int Bpp = v.bits_per_pixel / 8;
    printf("%s: %ux%u visible, %ux%u virtual, %u bpp, line_length=%u, smem_len=%u\n",
           dev, v.xres, v.yres, v.xres_virtual, v.yres_virtual,
           v.bits_per_pixel, f.line_length, f.smem_len);
    printf("  R off=%u len=%u  G off=%u len=%u  B off=%u len=%u  yoffset=%u\n",
           v.red.offset, v.red.length, v.green.offset, v.green.length,
           v.blue.offset, v.blue.length, v.yoffset);

    size_t maplen = f.smem_len ? f.smem_len : (size_t)f.line_length * v.yres_virtual;
    uint8_t *fb = mmap(NULL, maplen, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (fb == MAP_FAILED){ perror("mmap"); return 1; }

    /* Make sure the panel is on and we are showing page 0. */
    ioctl(fd, FBIOBLANK, FB_BLANK_UNBLANK);
    v.xoffset = 0; v.yoffset = 0;
    ioctl(fd, FBIOPAN_DISPLAY, &v);

    /* 8 classic colour bars. */
    static const uint8_t bar[8][3] = {
        {255,0,0},{0,255,0},{0,0,255},{255,255,0},
        {0,255,255},{255,0,255},{255,255,255},{0,0,0},
    };

    unsigned vh = v.yres_virtual ? v.yres_virtual : v.yres; /* fill all pages */
    for (unsigned y = 0; y < vh; y++){
        unsigned yv = y % v.yres;                 /* pattern row within a page  */
        uint8_t *row = fb + (size_t)y * f.line_length;
        for (unsigned x = 0; x < v.xres; x++){
            unsigned bi = x * 8 / v.xres;         /* which colour bar           */
            uint8_t r = bar[bi][0], g = bar[bi][1], b = bar[bi][2];

            int border = (x < 2 || x >= v.xres-2 || yv < 2 || yv >= v.yres-2);
            int diag   = (x * v.yres == yv * v.xres); /* TL->BR diagonal        */
            if (border || diag){ r = g = b = 255; }

            uint32_t px = pack(&v, r, g, b);
            uint8_t *p = row + (size_t)x * Bpp;
            for (int k = 0; k < Bpp; k++) p[k] = (px >> (8*k)) & 0xff; /* LE */
        }
    }

    printf("painted %ux%u colour bars + border + diagonal to %s\n",
           v.xres, v.yres, dev);
    munmap(fb, maplen);
    close(fd);
    return 0;
}
