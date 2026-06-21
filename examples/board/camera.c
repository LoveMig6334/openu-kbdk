/* kbdk-example
 * name: Camera preview
 * desc: Live colour-corrected camera on the LCD (MPP capture -> fb0)
 * extra_args: -DAWCHIP=0x1817 -Ivendor/eyesee-mpp/sun8iw19p1/include -Ivendor/eyesee-mpp/sun8iw19p1/include/media -Ivendor/eyesee-mpp/sun8iw19p1/include/utils -ldl -lpthread
 * mpp_libs: true
 */
/* Derived from src/camcc.c. Live camera preview on the 240x240 panel via the
 * Allwinner MPP middleware (the OV2685 is NOT reachable through standard V4L2 on
 * this BSP — see CLAUDE.md). It captures NV21 frames, applies a fixed
 * white-balance + saturation correction, and blits RGB to /dev/fb0.
 *
 * Board facts baked in here (each one bites if you skip it):
 *  - VENDOR LIBS ARE dlopen'd, NOT LINKED. The AW_MPI_* symbols come from the
 *    board's /usr/lib/eyesee-mpp/*.so at runtime, so we link only -ldl -lpthread.
 *    RUN WITH:  LD_LIBRARY_PATH=/usr/lib/eyesee-mpp:/usr/lib /tmp/camera
 *  - MUSL TIME64 TRAP. The cross toolchain (musl 1.2) redirects dlsym ->
 *    __dlsym_time64, which the board's musl 1.1.16 lacks. Bind the plain symbol
 *    with the aw_dlsym alias below (same trick as the production camcc.c).
 *  - ISP RUNS ON ITS OWN THREAD. AW_MPI_ISP_Run() hosts the 3A loop and blocks,
 *    so it must live on a separate pthread or nothing else runs.
 *  - NO VO LAYER, SO fb0 SHOWS THROUGH. We bring up VI+ISP capture but no video
 *    output layer, so MPP leaves the UI/fb layer visible and our /dev/fb0 writes
 *    appear on screen. Gate each frame on FBIO_WAITFORVSYNC and never pan the
 *    virtual page, or the image tears and rolls.
 *  - The build needs the vendored MPP headers — that's what extra_args carries
 *    (the -I paths are relative to the repo root, the Edit-tab compile cwd).
 *
 * Runs until SIGTERM/SIGINT.
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>
#include <signal.h>
#include <pthread.h>
#include <dlfcn.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <linux/videodev2.h>
#include <linux/fb.h>

#include "media/mm_common.h"
#include "media/mm_comm_sys.h"
#include "media/mm_comm_video.h"
#include "media/mm_comm_vi.h"

/* board musl 1.1.16 has plain dlsym, not the cross toolchain's __dlsym_time64 */
extern void *aw_dlsym(void *handle, const char *symbol) __asm__("dlsym");

/* ---- AW_MPI entry points resolved at runtime (capture only; no VO) -------- */
static int (*SYS_SetConf)(const MPP_SYS_CONF_S*);
static int (*SYS_Init)(void);
static int (*SYS_Exit)(void);
static int (*ISP_Init)(void);
static int (*ISP_Run)(int);
static int (*ISP_Stop)(int);
static int (*ISP_Exit)(void);
static int (*VI_CreateVipp)(int);
static int (*VI_SetVippAttr)(int, VI_ATTR_S*);
static int (*VI_EnableVipp)(int);
static int (*VI_DisableVipp)(int);
static int (*VI_DestoryVipp)(int);
static int (*VI_CreateVirChn)(int, int, void*);
static int (*VI_EnableVirChn)(int, int);
static int (*VI_DisableVirChn)(int, int);
static int (*VI_DestoryVirChn)(int, int);
static int (*VI_GetFrame)(int, int, VIDEO_FRAME_INFO_S*, int);
static int (*VI_ReleaseFrame)(int, int, VIDEO_FRAME_INFO_S*);

static int resolve(void){
    const char *libs[] = { "/usr/lib/eyesee-mpp/libmedia_mpp.so",
                           "/usr/lib/eyesee-mpp/libmpp_isp.so",
                           "/usr/lib/eyesee-mpp/libmpp_vi.so" };
    for (size_t i = 0; i < sizeof libs/sizeof libs[0]; i++)
        if (!dlopen(libs[i], RTLD_NOW|RTLD_GLOBAL)){
            fprintf(stderr, "dlopen %s: %s\n", libs[i], dlerror()); return -1; }
    #define SYM(v,name) do{ *(void**)(&v)=aw_dlsym(RTLD_DEFAULT,name); \
        if(!v){ fprintf(stderr,"dlsym %s failed\n",name); return -1; } }while(0)
    SYM(SYS_SetConf,"AW_MPI_SYS_SetConf"); SYM(SYS_Init,"AW_MPI_SYS_Init"); SYM(SYS_Exit,"AW_MPI_SYS_Exit");
    SYM(ISP_Init,"AW_MPI_ISP_Init"); SYM(ISP_Run,"AW_MPI_ISP_Run");
    SYM(ISP_Stop,"AW_MPI_ISP_Stop"); SYM(ISP_Exit,"AW_MPI_ISP_Exit");
    SYM(VI_CreateVipp,"AW_MPI_VI_CreateVipp"); SYM(VI_SetVippAttr,"AW_MPI_VI_SetVippAttr");
    SYM(VI_EnableVipp,"AW_MPI_VI_EnableVipp"); SYM(VI_DisableVipp,"AW_MPI_VI_DisableVipp");
    SYM(VI_DestoryVipp,"AW_MPI_VI_DestoryVipp");
    SYM(VI_CreateVirChn,"AW_MPI_VI_CreateVirChn"); SYM(VI_EnableVirChn,"AW_MPI_VI_EnableVirChn");
    SYM(VI_DisableVirChn,"AW_MPI_VI_DisableVirChn"); SYM(VI_DestoryVirChn,"AW_MPI_VI_DestoryVirChn");
    SYM(VI_GetFrame,"AW_MPI_VI_GetFrame"); SYM(VI_ReleaseFrame,"AW_MPI_VI_ReleaseFrame");
    #undef SYM
    return 0;
}

static int g_isp = 0;
static void *isp_thread(void *a){ (void)a; ISP_Run(g_isp); return NULL; }   /* 3A loop */
static volatile sig_atomic_t g_stop = 0;
static void on_stop(int s){ (void)s; g_stop = 1; }

#define CLAMP8(x) ((x)<0?0:(x)>255?255:(x))
#ifndef FBIO_WAITFORVSYNC
#define FBIO_WAITFORVSYNC _IOW('F', 0x20, unsigned int)   /* sunxi-disp supports this */
#endif

int main(void){
    setvbuf(stdout, NULL, _IONBF, 0);
    const int W = 320, H = 240, dev = 0, chn = 0;     /* capture size; cropped to panel */

    /* Fixed colour correction (BT.601 full-range): neutralise the ISP's green
     * cast (U+9 / V-11) then boost chroma x1.6, folded into <<8 fixed-point. */
    const int SH = 8;
    const double sat = 1.6;
    const int RV=(int)(1.402*sat*256+0.5), GU=(int)(0.344*sat*256+0.5),
              GV=(int)(0.714*sat*256+0.5), BU=(int)(1.772*sat*256+0.5);
    const int UO = 9, VO_ = -11;

    signal(SIGTERM, on_stop); signal(SIGINT, on_stop);

    /* ---- framebuffer ---- */
    int fbfd = open("/dev/fb0", O_RDWR);
    if (fbfd < 0){ perror("open /dev/fb0"); return 2; }
    struct fb_var_screeninfo v; struct fb_fix_screeninfo f;
    if (ioctl(fbfd,FBIOGET_VSCREENINFO,&v)<0 || ioctl(fbfd,FBIOGET_FSCREENINFO,&f)<0){ perror("fb info"); return 2; }
    int FW=v.xres, FH=v.yres, Bpp=v.bits_per_pixel/8, LL=f.line_length;
    int rO=v.red.offset/8, gO=v.green.offset/8, bO=v.blue.offset/8;   /* byte index per channel */
    size_t maplen = f.smem_len ? f.smem_len : (size_t)LL*v.yres_virtual;
    uint8_t *fb = mmap(NULL,maplen,PROT_READ|PROT_WRITE,MAP_SHARED,fbfd,0);
    if (fb==MAP_FAILED){ perror("mmap fb"); return 2; }
    v.xoffset = 0; v.yoffset = 0; v.activate = FB_ACTIVATE_NOW;        /* pin page 0, never pan */
    ioctl(fbfd, FBIOPAN_DISPLAY, &v);
    int vsync_ok = 1; unsigned vzero = 0;

    /* ---- MPP bring-up: SYS -> ISP (own thread) -> VI ---- */
    if (resolve() < 0) return 2;
    MPP_SYS_CONF_S sys; memset(&sys,0,sizeof sys); sys.nAlignWidth = 32;
    SYS_SetConf(&sys);
    if (SYS_Init() != SUCCESS){ fprintf(stderr,"SYS_Init failed\n"); return 2; }
    ISP_Init();
    pthread_t t; pthread_create(&t,NULL,isp_thread,NULL);
    usleep(200000);                              /* let the 3A loop settle */

    VI_ATTR_S attr; memset(&attr,0,sizeof attr);
    attr.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
    attr.memtype = V4L2_MEMORY_MMAP;
    attr.format.pixelformat = V4L2_PIX_FMT_NV21M;     /* Y plane + interleaved VU plane */
    attr.format.field = V4L2_FIELD_NONE;
    attr.format.width = W; attr.format.height = H;
    attr.nbufs = 5; attr.nplanes = 2; attr.fps = 30;
    attr.capturemode = V4L2_MODE_VIDEO;
    if (VI_CreateVipp(dev)!=SUCCESS){ fprintf(stderr,"CreateVipp failed\n"); return 2; }
    VI_SetVippAttr(dev,&attr);
    if (VI_EnableVipp(dev)!=SUCCESS){ fprintf(stderr,"EnableVipp failed\n"); return 2; }
    if (VI_CreateVirChn(dev,chn,NULL)!=SUCCESS){ fprintf(stderr,"CreateVirChn failed\n"); return 2; }
    if (VI_EnableVirChn(dev,chn)!=SUCCESS){ fprintf(stderr,"EnableVirChn failed\n"); return 2; }

    int xoff = (W-FW)/2; if (xoff<0) xoff=0;          /* centre-crop WxH down to the panel */
    int yoff = (H-FH)/2; if (yoff<0) yoff=0;
    printf("LIVE: corrected preview on panel (%dx%d crop of %dx%d), SIGTERM to stop\n",FW,FH,W,H);

    unsigned long frames = 0;
    while (!g_stop){
        VIDEO_FRAME_INFO_S fi; memset(&fi,0,sizeof fi);
        if (VI_GetFrame(dev,chn,&fi,2000)!=SUCCESS) continue;
        VIDEO_FRAME_S *fr = &fi.VFrame;
        uint8_t *Y = fr->mpVirAddr[0], *VU = fr->mpVirAddr[1];     /* NV21M: Y + VU planes */
        if (!Y || !VU){ VI_ReleaseFrame(dev,chn,&fi); continue; }

        /* Wait for vblank, then write the whole frame into the one visible page
         * so scan-out never catches a half-drawn image. */
        if (vsync_ok && ioctl(fbfd,FBIO_WAITFORVSYNC,&vzero)<0) vsync_ok=0;
        for (int dy=0; dy<FH; dy++){
            int sy = yoff + dy;
            uint8_t *row = fb + (size_t)dy*LL;
            const uint8_t *yl = Y  + (size_t)sy*W;
            const uint8_t *cl = VU + (size_t)(sy>>1)*W;       /* one chroma row per 2 lines */
            for (int dx=0; dx<FW; dx++){
                int sx = xoff + dx;
                int yv = yl[sx];
                int ci = (sx>>1)<<1;
                int cv = cl[ci]   - 128 + VO_;     /* V (Cr) */
                int cu = cl[ci+1] - 128 + UO;      /* U (Cb) */
                int r = yv + ((cv*RV)>>SH);
                int g = yv - ((cu*GU + cv*GV)>>SH);
                int b = yv + ((cu*BU)>>SH);
                uint8_t *p = row + (size_t)dx*Bpp;
                p[rO]=CLAMP8(r); p[gO]=CLAMP8(g); p[bO]=CLAMP8(b);
            }
        }
        VI_ReleaseFrame(dev,chn,&fi);
        if (frames == 0) ioctl(fbfd,FBIOBLANK,FB_BLANK_UNBLANK);
        frames++;
    }
    printf("stopping after %lu frames\n", frames);

    /* ---- teardown ---- */
    VI_DisableVirChn(dev,chn); VI_DestoryVirChn(dev,chn);
    VI_DisableVipp(dev);       VI_DestoryVipp(dev);
    ISP_Stop(g_isp); pthread_join(t,NULL); ISP_Exit();
    SYS_Exit();
    munmap(fb,maplen); close(fbfd);
    return 0;
}
