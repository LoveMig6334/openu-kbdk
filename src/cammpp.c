/* cammpp.c - minimal Allwinner MPP camera probe for the KidBright uAI (V831).
 *
 * Standard V4L2 streaming is a dead end on this BSP (QBUF/DQBUF are ENOTTY -- see
 * camdiag.c); the real capture path is Allwinner's MPP middleware. This grabs ONE
 * NV21M frame through AW_MPI VI+ISP and prints its geometry/addresses, to prove
 * the vendored sun8iw19p1 header ABI + init sequence work on hardware before we
 * build the live preview on top.
 *
 * The host can't pull the board's .so to link against (uai only pushes), so we
 * dlopen() them on the board and dlsym the AW_MPI_* entry points -- the vendored
 * eyesee-mpp headers are used only for the struct ABI. Symbols live in:
 *   libmedia_mpp.so (SYS)  libmpp_isp.so (ISP)  libmpp_vi.so (VI)
 *
 * HANG-SAFE: ISP run-loop on its own thread, GetFrame 5s timeout, alarm(12)
 * watchdog that _exit()s so a stuck MPP call can't hold the serial port forever.
 *
 * Run with: LD_LIBRARY_PATH=/usr/lib/eyesee-mpp:/usr/lib /tmp/cammpp
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

/* The board runs musl 1.1.16 (no time64); the cross toolchain (musl 1.2+)
 * redirects dlsym -> __dlsym_time64, which the board's libc doesn't export.
 * Bind to the plain "dlsym" symbol the board actually has. (dlopen/dlerror
 * are not redirected, so they're fine.) */
extern void *aw_dlsym(void *handle, const char *symbol) __asm__("dlsym");

#include "media/mm_common.h"
#include "media/mm_comm_sys.h"
#include "media/mm_comm_video.h"
#include "media/mm_comm_vi.h"

/* ---- AW_MPI entry points we resolve at runtime ---------------------------- */
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
    /* RTLD_GLOBAL so the libs' cross-references resolve; then dlsym globally. */
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

static int g_isp = 0;
static void *isp_thread(void *arg){ (void)arg; ISP_Run(g_isp); return NULL; }
static void watchdog(int s){ (void)s; const char m[]="\n[watchdog] timed out, exiting\n"; write(2,m,sizeof m-1); _exit(4); }

int main(int argc, char **argv){
    setvbuf(stdout, NULL, _IONBF, 0);
    int W = 320, H = 240, dev = 0, chn = 0;
    if (argc>1) sscanf(argv[1], "%dx%d", &W, &H);
    signal(SIGALRM, watchdog); alarm(25);

    if (resolve() < 0) return 2;

    MPP_SYS_CONF_S sys; memset(&sys,0,sizeof sys); sys.nAlignWidth = 32;
    SYS_SetConf(&sys);
    if (SYS_Init() != SUCCESS){ fprintf(stderr,"AW_MPI_SYS_Init failed\n"); return 2; }
    printf("SYS_Init ok\n");

    ISP_Init();
    pthread_t t; pthread_create(&t, NULL, isp_thread, NULL);
    usleep(200000);                       /* let the ISP 3A loop spin up */
    printf("ISP started (dev %d)\n", g_isp);

    VI_ATTR_S attr; memset(&attr,0,sizeof attr);
    attr.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
    attr.memtype = V4L2_MEMORY_MMAP;
    attr.format.pixelformat = V4L2_PIX_FMT_NV21M;
    attr.format.field = V4L2_FIELD_NONE;
    attr.format.width = W; attr.format.height = H;
    attr.nbufs = 5; attr.nplanes = 2; attr.fps = 30;
    attr.capturemode = V4L2_MODE_VIDEO;

    int rc = 1;
    if (VI_CreateVipp(dev) != SUCCESS){ fprintf(stderr,"CreateVipp failed\n"); goto sys_out; }
    if (VI_SetVippAttr(dev,&attr) != SUCCESS){ fprintf(stderr,"SetVippAttr failed\n"); goto vipp_out; }
    if (VI_EnableVipp(dev) != SUCCESS){ fprintf(stderr,"EnableVipp failed\n"); goto vipp_out; }
    VI_CreateVirChn(dev, chn, NULL);
    VI_SetVirChnAttr(dev, chn, NULL);
    if (VI_EnableVirChn(dev, chn) != SUCCESS){ fprintf(stderr,"EnableVirChn failed\n"); goto chn_out; }
    printf("VIPP %d + VirChn %d enabled, NV21M %dx%d; waiting for a frame...\n", dev, chn, W, H);

    /* grab a stream so AE/AWB converge; report avg Y/U/V every ~15 frames so we
     * see the steady state the live preview actually shows (NV21 VU = V,U). */
    int nframes = (argc>2) ? atoi(argv[2]) : 90;
    const char *dumpfile = (argc>3) ? argv[3] : NULL;  /* dump final NV21 frame here */
    for (int k=0; k<nframes; k++){
        VIDEO_FRAME_INFO_S fi; memset(&fi,0,sizeof fi);
        if (VI_GetFrame(dev, chn, &fi, 5000) != SUCCESS){ fprintf(stderr,"GetFrame: no frame\n"); goto stream_out; }
        VIDEO_FRAME_S *f = &fi.VFrame;
        uint8_t *y = f->mpVirAddr[0], *vu = f->mpVirAddr[1];
        if ((k%15==0 || k==nframes-1) && y && vu){
            unsigned fw=f->mWidth, fh=f->mHeight;
            unsigned long sy=0; for (unsigned i=0;i<fw*fh;i++) sy+=y[i];
            unsigned long sv=0,su=0,n=0; for (unsigned i=0;i+1<fw*fh/2;i+=2){ sv+=vu[i]; su+=vu[i+1]; n++; }
            printf("frame %2d: avg Y=%-3lu  U/Cb=%-3lu  V/Cr=%-3lu  (neutral 128)\n",
                   k, sy/(fw*fh), n?su/n:0, n?sv/n:0);
        }
        /* Write a contiguous NV21 file so the host can pull it back and inspect the
         * real color. Sensor emits occasional black frames, so only dump well-exposed
         * ones (sampled avg Y in 30..220) and overwrite -- the last good frame wins. */
        int good = 0;
        if (dumpfile && y){
            unsigned fw=f->mWidth, fh=f->mHeight; unsigned long s=0,c=0;
            for (unsigned i=0;i<fw*fh;i+=16){ s+=y[i]; c++; }
            unsigned long ay = c?s/c:0; good = (ay>30 && ay<220);
        }
        if (dumpfile && good && y && vu){
            unsigned fw=f->mWidth, fh=f->mHeight;
            FILE *fp = fopen(dumpfile, "wb");
            if (!fp){ perror("dump fopen"); }
            else {
                fwrite(y,  1, (size_t)fw*fh,   fp);   /* Y plane, contiguous */
                fwrite(vu, 1, (size_t)fw*fh/2, fp);   /* VU plane (NV21), contiguous */
                fclose(fp);
                if (k%30==0 || k==nframes-1)
                    printf("dumped NV21 %ux%u (%lu bytes) -> %s\n", fw,fh,(unsigned long)(fw*fh*3/2),dumpfile);
            }
        }
        VI_ReleaseFrame(dev, chn, &fi);
        rc = 0;
    }

stream_out:  VI_DisableVirChn(dev, chn); VI_DestoryVirChn(dev, chn);
chn_out:     VI_DisableVipp(dev);
vipp_out:    VI_DestoryVipp(dev);
sys_out:
    ISP_Stop(g_isp);
    pthread_join(t, NULL);
    ISP_Exit();
    SYS_Exit();
    printf(rc==0 ? "OK\n" : "FAILED\n");
    return rc;
}
