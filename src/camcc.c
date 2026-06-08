/* camcc.c - COLOR-CORRECTED live camera preview on the KidBright uAI panel.
 *
 * The zero-copy path (campreview.c: AW_MPI_SYS_Bind VI->VO) shows the camera with
 * no CPU touch, but you get the ISP's raw color -- which on this board is green/grey
 * and washed out (neutrals land at U~119/V~139 instead of 128/128, saturation ~16).
 * This variant takes the CPU path instead: capture NV21 via MPP (same as cammpp.c),
 * apply a white-balance + saturation correction per pixel, and blit RGB to /dev/fb0.
 *
 * We do NOT bring up a VO video layer, so MPP's display stack leaves the UI/fb layer
 * (HLAY(2,0)) visible and our fb0 writes show through. Cost: ~1 frame of CPU on the
 * single A7 vs zero-copy, but full control of the color.
 *
 * Correction (defaults from host-side analysis, see captures/nv21.py):
 *   neutralize: U += UOFF(+9), V += VOFF(-11)   then  saturate: chroma *= SAT(1.6)
 * All three are runtime args so you can tune live without recompiling.
 *
 *   LD_LIBRARY_PATH=/usr/lib/eyesee-mpp:/usr/lib /tmp/camcc [WxH] [secs] [uoff voff sat] [flip]
 *   e.g.  /tmp/camcc 320x240 0 9 -11 1.6 0       (run until SIGTERM, default correction)
 *
 * Symbols dlopen/dlsym'd from the board libs (host can't link them):
 *   libmedia_mpp(SYS)  libmpp_isp(ISP)  libmpp_vi(VI)
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

/* ---- resolved AW_MPI entry points (capture only; no VO) -------------------- */
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
    for (size_t i=0;i<sizeof libs/sizeof libs[0];i++)
        if (!dlopen(libs[i], RTLD_NOW|RTLD_GLOBAL)){ fprintf(stderr,"dlopen %s: %s\n",libs[i],dlerror()); return -1; }
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
static void *isp_thread(void *a){ (void)a; ISP_Run(g_isp); return NULL; }
static volatile sig_atomic_t g_stop = 0;
static void on_stop(int s){ (void)s; g_stop = 1; }
static void on_alarm(int s){ (void)s; const char m[]="\n[watchdog] stalled, exiting\n"; write(2,m,sizeof m-1); _exit(4); }

#define CLAMP8(x) ((x)<0?0:(x)>255?255:(x))
#ifndef FBIO_WAITFORVSYNC
#define FBIO_WAITFORVSYNC _IOW('F', 0x20, unsigned int)   /* sunxi-disp supports this */
#endif

int main(int argc, char **argv){
    setvbuf(stdout, NULL, _IONBF, 0);
    int W=320, H=240, dev=0, chn=0, secs=0;
    if (argc>1) sscanf(argv[1], "%dx%d", &W, &H);
    if (argc>2) secs = atoi(argv[2]);
    double uoff = (argc>3) ? atof(argv[3]) :  9.0;   /* white-balance trim, chroma units */
    double voff = (argc>4) ? atof(argv[4]) : -11.0;
    double sat  = (argc>5) ? atof(argv[5]) :  1.6;   /* saturation multiplier */
    int    flip = (argc>6) ? atoi(argv[6]) :  0;     /* raw sensor orientation is already upright */

    /* fold saturation into the BT.601 coefficients (fixed-point, <<8) */
    const int SH=8;
    int RV=(int)(1.402*sat*256+0.5), GU=(int)(0.344*sat*256+0.5),
        GV=(int)(0.714*sat*256+0.5), BU=(int)(1.772*sat*256+0.5);
    int UO=(int)(uoff+ (uoff<0?-0.5:0.5)), VO_=(int)(voff+(voff<0?-0.5:0.5));

    signal(SIGTERM, on_stop); signal(SIGINT, on_stop);
    signal(SIGALRM, on_alarm); alarm(20);            /* guards setup */

    /* ---- framebuffer ---- */
    int fbfd = open("/dev/fb0", O_RDWR);
    if (fbfd<0){ perror("open /dev/fb0"); return 2; }
    struct fb_var_screeninfo v; struct fb_fix_screeninfo f;
    if (ioctl(fbfd,FBIOGET_VSCREENINFO,&v)<0||ioctl(fbfd,FBIOGET_FSCREENINFO,&f)<0){ perror("fb info"); return 2; }
    int FW=v.xres, FH=v.yres, Bpp=v.bits_per_pixel/8, LL=f.line_length;
    int rO=v.red.offset/8, gO=v.green.offset/8, bO=v.blue.offset/8;   /* byte index per channel */
    size_t maplen = f.smem_len ? f.smem_len : (size_t)LL*v.yres_virtual;
    uint8_t *fb = mmap(NULL,maplen,PROT_READ|PROT_WRITE,MAP_SHARED,fbfd,0);
    if (fb==MAP_FAILED){ perror("mmap fb"); return 2; }
    /* Pin the visible page to offset 0 and never pan. Panning between the two
     * virtual pages was switching the displayed page mid-scan -> the image tore
     * and rolled down the panel. Drawing to one fixed page, gated on vsync, is
     * stable (the write of a 240x240 frame is fast enough to finish in a blank). */
    v.xoffset = 0; v.yoffset = 0; v.activate = FB_ACTIVATE_NOW;
    ioctl(fbfd, FBIOPAN_DISPLAY, &v);
    int vsync_ok = 1; unsigned vzero = 0;
    printf("fb %dx%d %dbpp LL=%d  correction U+%d V%+d sat=%.2f flip=%d\n",
           FW,FH,v.bits_per_pixel,LL,UO,VO_,sat,flip);

    if (resolve() < 0) return 2;

    MPP_SYS_CONF_S sys; memset(&sys,0,sizeof sys); sys.nAlignWidth = 32;
    SYS_SetConf(&sys);
    if (SYS_Init() != SUCCESS){ fprintf(stderr,"SYS_Init failed\n"); return 2; }
    ISP_Init();
    pthread_t t; pthread_create(&t,NULL,isp_thread,NULL);
    usleep(200000);
    printf("SYS+ISP up\n");

    int vipp_created=0,vipp_enabled=0,virchn_created=0,virchn_enabled=0, rc=2;
    VI_ATTR_S attr; memset(&attr,0,sizeof attr);
    attr.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
    attr.memtype = V4L2_MEMORY_MMAP;
    attr.format.pixelformat = V4L2_PIX_FMT_NV21M;
    attr.format.field = V4L2_FIELD_NONE;
    attr.format.width = W; attr.format.height = H;
    attr.nbufs = 5; attr.nplanes = 2; attr.fps = 30;
    attr.capturemode = V4L2_MODE_VIDEO;
    if (VI_CreateVipp(dev)!=SUCCESS){ fprintf(stderr,"CreateVipp failed\n"); goto out; } vipp_created=1;
    VI_SetVippAttr(dev,&attr);
    if (VI_EnableVipp(dev)!=SUCCESS){ fprintf(stderr,"EnableVipp failed\n"); goto out; } vipp_enabled=1;
    if (VI_CreateVirChn(dev,chn,NULL)!=SUCCESS){ fprintf(stderr,"CreateVirChn failed\n"); goto out; } virchn_created=1;
    if (VI_EnableVirChn(dev,chn)!=SUCCESS){ fprintf(stderr,"EnableVirChn failed\n"); goto out; } virchn_enabled=1;

    /* center-crop the WxH capture down to the FWxFH panel */
    int xoff = (W-FW)/2; if (xoff<0) xoff=0;
    int yoff = (H-FH)/2; if (yoff<0) yoff=0;

    alarm(0);
    printf("LIVE: corrected preview on panel, %dx%d crop of %dx%d (SIGTERM to stop)\n",FW,FH,W,H);
    unsigned long frames=0;
    for (unsigned i=0; (secs<=0 || (int)(frames/30)<secs) && !g_stop; i++){
        VIDEO_FRAME_INFO_S fi; memset(&fi,0,sizeof fi);
        alarm(5);                                  /* a stalled GetFrame must not hang forever */
        if (VI_GetFrame(dev,chn,&fi,2000)!=SUCCESS) continue;
        VIDEO_FRAME_S *fr=&fi.VFrame;
        uint8_t *Y=fr->mpVirAddr[0], *VU=fr->mpVirAddr[1];
        if (!Y||!VU){ VI_ReleaseFrame(dev,chn,&fi); continue; }

        /* wait for the panel's vblank, then write the whole frame into the one
         * visible page so the scan-out never catches a half-updated image */
        if (vsync_ok && ioctl(fbfd,FBIO_WAITFORVSYNC,&vzero)<0) vsync_ok=0;
        uint8_t *base = fb;                         /* fixed visible page (yoffset 0) */
        for (int dy=0; dy<FH; dy++){
            int sy = yoff + dy;
            uint8_t *row = base + (size_t)(flip? (FH-1-dy):dy)*LL;
            const uint8_t *yl = Y + (size_t)sy*W;
            const uint8_t *cl = VU + (size_t)(sy>>1)*W;     /* VU plane: W bytes/chroma row */
            for (int dx=0; dx<FW; dx++){
                int sx = xoff + dx;
                int yv = yl[sx];
                int ci = (sx>>1)<<1;                 /* byte offset of this chroma pair */
                int cv = cl[ci]   - 128 + VO_;       /* V (Cr) */
                int cu = cl[ci+1] - 128 + UO;        /* U (Cb) */
                int r = yv + ((cv*RV)>>SH);
                int g = yv - ((cu*GU + cv*GV)>>SH);
                int b = yv + ((cu*BU)>>SH);
                uint8_t *p = row + (size_t)(flip? (FW-1-dx):dx)*Bpp;
                p[rO]=CLAMP8(r); p[gO]=CLAMP8(g); p[bO]=CLAMP8(b);
            }
        }
        VI_ReleaseFrame(dev,chn,&fi);
        frames++; rc=0;
        if (frames==1) ioctl(fbfd,FBIOBLANK,FB_BLANK_UNBLANK);
    }
    alarm(25);
    printf("stopping after %lu frames (vsync %s)\n", frames, vsync_ok?"on":"UNSUPPORTED-tear-possible");

out:
    if (virchn_enabled) VI_DisableVirChn(dev,chn);
    if (virchn_created) VI_DestoryVirChn(dev,chn);
    if (vipp_enabled)   VI_DisableVipp(dev);
    if (vipp_created)   VI_DestoryVipp(dev);
    ISP_Stop(g_isp); pthread_join(t,NULL); ISP_Exit();
    SYS_Exit();
    ioctl(fbfd,FBIOBLANK,FB_BLANK_UNBLANK);         /* leave panel on */
    munmap(fb,maplen); close(fbfd);
    printf(rc==0?"done\n":"FAILED\n");
    return rc;
}
