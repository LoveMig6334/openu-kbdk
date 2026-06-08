/* nnacam.cpp - live camera -> NPU image classification on the KidBright uAI (V831).
 *
 * Captures NV21 frames via Allwinner MPP (VI+ISP, same path as cammpp.c), centre-crops
 * + downscales each frame to 32x32 RGB, and classifies it on the NPU (NVDLA) with the
 * vendored CIFAR-10 network (third_party/v831-npu). Prints the predicted label per frame.
 *
 * MPP symbols are dlopen'd from the board libs at runtime; the NPU is driven straight
 * via /dev/mem + /dev/ion + /dev/cedar_dev (no kernel driver). Both subsystems share the
 * one process -- the point of this tool is to prove camera + NPU coexist in ~60 MB.
 *
 * Build: make nnacam      Run: LD_LIBRARY_PATH=/usr/lib/eyesee-mpp:/usr/lib /tmp/nnacam [WxH] [nframes]
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
#include <linux/videodev2.h>

/* board musl 1.1.16 has plain dlsym, not the cross toolchain's __dlsym_time64 */
extern void *aw_dlsym(void *handle, const char *symbol) __asm__("dlsym");

#include "media/mm_common.h"
#include "media/mm_comm_sys.h"
#include "media/mm_comm_video.h"
#include "media/mm_comm_vi.h"

#include "nna_hw.h"   /* NPU control: nna_configure/on/off, xreg_open/close, nna_reset */

/* classifier hooks exported by the vendored nna_cifar10.cpp (built with -DNNACAM) */
extern void nna_set_input_rgb(const unsigned char *rgb32x32);
extern void cifar10(void);
extern int  nna_pred;
extern int  nna_scores[10];
extern const char *nna_label(int i);

/* ---- AW_MPI entry points resolved at runtime (subset of cammpp.c) ---------- */
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
static int (*VI_SetVirChnAttr)(int, int, void*);
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
    SYM(VI_CreateVirChn,"AW_MPI_VI_CreateVirChn"); SYM(VI_SetVirChnAttr,"AW_MPI_VI_SetVirChnAttr");
    SYM(VI_EnableVirChn,"AW_MPI_VI_EnableVirChn"); SYM(VI_DisableVirChn,"AW_MPI_VI_DisableVirChn");
    SYM(VI_DestoryVirChn,"AW_MPI_VI_DestoryVirChn");
    SYM(VI_GetFrame,"AW_MPI_VI_GetFrame"); SYM(VI_ReleaseFrame,"AW_MPI_VI_ReleaseFrame");
    #undef SYM
    return 0;
}

#define CLAMP8(x) ((x)<0?0:((x)>255?255:(x)))

static int g_isp = 0, g_stop = 0;
static void *isp_thread(void *arg){ (void)arg; ISP_Run(g_isp); return NULL; }
static void on_stop(int s){ (void)s; g_stop = 1; }
static void on_alarm(int s){ (void)s; const char m[]="\n[watchdog] timed out\n"; write(2,m,sizeof m-1); _exit(4); }

/* centre-square downscale of an NV21 frame to 32x32 interleaved RGB (BT.601) */
static void nv21_to_rgb32(const uint8_t *Y, const uint8_t *VU, int W, int H, unsigned char *rgb){
    int side = (W < H) ? W : H;
    int xoff = (W - side)/2, yoff = (H - side)/2;
    for (int oy=0; oy<32; oy++){
        int sy = yoff + oy*side/32;
        const uint8_t *yl = Y  + (size_t)sy*W;
        const uint8_t *cl = VU + (size_t)(sy>>1)*W;     /* VU plane: W bytes per chroma row */
        for (int ox=0; ox<32; ox++){
            int sx = xoff + ox*side/32;
            int yv = yl[sx];
            int ci = (sx>>1)<<1;
            int cv = cl[ci]   - 128;                    /* V (Cr) */
            int cu = cl[ci+1] - 128;                    /* U (Cb) */
            int r = yv + ((359*cv)>>8);                 /* 1.402 */
            int g = yv - ((88*cu + 183*cv)>>8);         /* 0.344, 0.714 */
            int b = yv + ((454*cu)>>8);                 /* 1.772 */
            unsigned char *p = rgb + ((size_t)oy*32 + ox)*3;
            p[0]=CLAMP8(r); p[1]=CLAMP8(g); p[2]=CLAMP8(b);
        }
    }
}

int main(int argc, char **argv){
    setvbuf(stdout, NULL, _IONBF, 0);
    int W=320, H=240, dev=0, chn=0;
    if (argc>1) sscanf(argv[1], "%dx%d", &W, &H);
    int nframes = (argc>2) ? atoi(argv[2]) : 0;         /* 0 = run until SIGTERM */
    signal(SIGTERM,on_stop); signal(SIGINT,on_stop);
    signal(SIGALRM,on_alarm); alarm(25);                /* guards setup */

    if (resolve() < 0) return 2;

    /* ---- NPU up once (clock 400MHz, power on, map regs) ---- */
    nna_configure(nna_cmd_clk, 400);
    nna_on();
    if (!xreg_open()){ fprintf(stderr,"xreg_open (/dev/mem) failed\n"); return 2; }
    nna_reset();
    printf("NPU up (NVDLA via /dev/mem)\n");

    /* ---- MPP camera up (VI + ISP) ---- */
    MPP_SYS_CONF_S sys; memset(&sys,0,sizeof sys); sys.nAlignWidth = 32;
    SYS_SetConf(&sys);
    if (SYS_Init() != SUCCESS){ fprintf(stderr,"SYS_Init failed\n"); return 2; }
    ISP_Init();
    pthread_t t; pthread_create(&t,NULL,isp_thread,NULL);
    usleep(200000);

    VI_ATTR_S attr; memset(&attr,0,sizeof attr);
    attr.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
    attr.memtype = V4L2_MEMORY_MMAP;
    attr.format.pixelformat = V4L2_PIX_FMT_NV21M;
    attr.format.field = V4L2_FIELD_NONE;
    attr.format.width = W; attr.format.height = H;
    attr.nbufs = 5; attr.nplanes = 2; attr.fps = 30;
    attr.capturemode = V4L2_MODE_VIDEO;

    int rc=2, vipp=0, vch=0;
    if (VI_CreateVipp(dev)!=SUCCESS){ fprintf(stderr,"CreateVipp failed\n"); goto out; } vipp=1;
    VI_SetVippAttr(dev,&attr);
    if (VI_EnableVipp(dev)!=SUCCESS){ fprintf(stderr,"EnableVipp failed\n"); goto out; }
    if (VI_CreateVirChn(dev,chn,NULL)!=SUCCESS){ fprintf(stderr,"CreateVirChn failed\n"); goto out; }
    if (VI_EnableVirChn(dev,chn)!=SUCCESS){ fprintf(stderr,"EnableVirChn failed\n"); goto out; } vch=1;

    alarm(0);
    printf("LIVE camera -> NPU classify (NV21 %dx%d, centre-crop -> 32x32). SIGTERM to stop.\n", W, H);

    {
    unsigned char rgb[32*32*3];
    unsigned long frames=0;
    for (int k=0; (nframes<=0 || k<nframes) && !g_stop; k++){
        VIDEO_FRAME_INFO_S fi; memset(&fi,0,sizeof fi);
        alarm(8);                                       /* a stalled GetFrame must not hang forever */
        if (VI_GetFrame(dev,chn,&fi,2000)!=SUCCESS) continue;
        VIDEO_FRAME_S *fr=&fi.VFrame;
        uint8_t *Y=(uint8_t*)fr->mpVirAddr[0], *VU=(uint8_t*)fr->mpVirAddr[1];
        if (!Y||!VU){ VI_ReleaseFrame(dev,chn,&fi); continue; }

        /* skip black/garbage frames the sensor occasionally emits */
        unsigned long s=0,c=0; for (unsigned i=0;i<(unsigned)W*H;i+=16){ s+=Y[i]; c++; }
        unsigned long ay = c?s/c:0;
        if (ay<20 || ay>240){ VI_ReleaseFrame(dev,chn,&fi); continue; }

        nv21_to_rgb32(Y, VU, fr->mWidth, fr->mHeight, rgb);
        VI_ReleaseFrame(dev,chn,&fi);

        nna_set_input_rgb(rgb);
        cifar10();                                      /* runs on the NPU, fills nna_pred/nna_scores */

        /* second-best for a rough confidence margin */
        int s1=-1000,s2=-1000;
        for (int i=0;i<10;i++){ if(nna_scores[i]>s1){ s2=s1; s1=nna_scores[i]; } else if(nna_scores[i]>s2) s2=nna_scores[i]; }
        printf("frame %3lu  avgY=%3lu  -> %-11s (score %d, margin %d)\n",
               frames, ay, nna_label(nna_pred), s1, s1-s2);
        frames++; rc=0;
    }
    alarm(25);
    printf("stopping after %lu classified frames\n", frames);
    }

out:
    if (vch)  { VI_DisableVirChn(dev,chn); VI_DestoryVirChn(dev,chn); }
    if (vipp) { VI_DisableVipp(dev); VI_DestoryVipp(dev); }
    ISP_Stop(g_isp); pthread_join(t,NULL); ISP_Exit();
    SYS_Exit();
    xreg_close(); nna_off();
    printf(rc==0 ? "OK\n" : "FAILED\n");
    return rc;
}
