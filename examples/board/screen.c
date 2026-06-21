/* kbdk-example
 * name: Screen colour bars
 * desc: mmap /dev/fb0 and paint colour bars + a white border (panel test)
 * extra_args:
 */
/* Derived from src/fbtest.c. Talks to the 240x240 LCD directly through the
 * framebuffer device — no vendor library, just FBIO* ioctls + mmap.
 *
 * The one idea to learn here: never hardcode the pixel format. We ask the kernel
 * for the panel geometry and the R/G/B bit positions (FBIOGET_VSCREENINFO), then
 * pack each pixel from those bitfields. That makes the same code correct for any
 * RGB/BGR order or 16/24/32 bpp — on this board it's 240x240 RGB888. */
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <linux/fb.h>

/* Build one pixel word from 8-bit r/g/b using the panel's own bit layout. */
static uint32_t pack(struct fb_var_screeninfo *v, uint8_t r, uint8_t g, uint8_t b){
    uint32_t rv = (uint32_t)(r >> (8 - v->red.length))   << v->red.offset;
    uint32_t gv = (uint32_t)(g >> (8 - v->green.length)) << v->green.offset;
    uint32_t bv = (uint32_t)(b >> (8 - v->blue.length))  << v->blue.offset;
    return rv | gv | bv;
}

int main(void){
    int fd = open("/dev/fb0", O_RDWR);
    if (fd < 0){ fprintf(stderr, "open /dev/fb0: %s\n", strerror(errno)); return 1; }

    /* Query geometry (var) and stride/size (fix) at runtime. */
    struct fb_var_screeninfo v;
    struct fb_fix_screeninfo f;
    if (ioctl(fd, FBIOGET_VSCREENINFO, &v) < 0){ perror("VSCREENINFO"); return 1; }
    if (ioctl(fd, FBIOGET_FSCREENINFO, &f) < 0){ perror("FSCREENINFO"); return 1; }
    int Bpp = v.bits_per_pixel / 8;
    printf("/dev/fb0: %ux%u, %u bpp, line_length=%u\n",
           v.xres, v.yres, v.bits_per_pixel, f.line_length);

    /* mmap the whole framebuffer so we can write pixels as plain memory. */
    size_t maplen = f.smem_len ? f.smem_len : (size_t)f.line_length * v.yres;
    uint8_t *fb = mmap(NULL, maplen, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (fb == MAP_FAILED){ perror("mmap"); return 1; }

    ioctl(fd, FBIOBLANK, FB_BLANK_UNBLANK);   /* make sure the panel is on */

    static const uint8_t bar[8][3] = {
        {255,0,0},{0,255,0},{0,0,255},{255,255,0},
        {0,255,255},{255,0,255},{255,255,255},{0,0,0},
    };
    for (unsigned y = 0; y < v.yres; y++){
        uint8_t *row = fb + (size_t)y * f.line_length;
        for (unsigned x = 0; x < v.xres; x++){
            unsigned bi = x * 8 / v.xres;             /* which of the 8 bars */
            uint8_t r = bar[bi][0], g = bar[bi][1], b = bar[bi][2];
            int border = (x < 2 || x >= v.xres-2 || y < 2 || y >= v.yres-2);
            if (border){ r = g = b = 255; }           /* white frame around the edge */

            uint32_t px = pack(&v, r, g, b);
            uint8_t *p = row + (size_t)x * Bpp;
            for (int k = 0; k < Bpp; k++) p[k] = (px >> (8*k)) & 0xff;  /* little-endian */
        }
    }
    printf("painted %ux%u colour bars + border\n", v.xres, v.yres);

    munmap(fb, maplen);
    close(fd);
    return 0;
}
