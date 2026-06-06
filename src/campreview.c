/* campreview.c - LIVE camera preview on the KidBright uAI panel via Allwinner MPP.
 *
 * Builds on cammpp.c (VI+ISP capture, proven working) and adds the display half:
 * brings up VO (the LCD video layer) and AW_MPI_SYS_Bind()s VI->VO so the
 * hardware pipes camera -> ISP -> display with zero CPU per frame (max perf).
 * This is the path the vendor sample_virvi2vo uses on this exact SoC (sun8iw19p1).
 *
 * Standard V4L2 can't capture here (QBUF ENOTTY, see camdiag.c), and a plain
 * /dev/fb0 blit would be occluded by the MPP video layer -- so VO is both the
 * correct and the fastest way onto the panel.
 *
 * Runs for SECONDS then tears down (also stops cleanly on SIGTERM/SIGINT), so it
 * is safe to launch backgrounded over the slow serial link:
 *   LD_LIBRARY_PATH=/usr/lib/eyesee-mpp:/usr/lib /tmp/campreview 320x240 30 &
 *
 * Symbols are dlopen/dlsym'd from the board's libs (host can't link them):
 *   libmedia_mpp(SYS)  libmpp_isp(ISP)  libmpp_vi(VI)  libmpp_vo(VO)
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
#include <linux/videodev2.h>
#include <linux/fb.h>

#include "media/mm_common.h"
#include "media/mm_comm_sys.h"
#include "media/mm_comm_video.h"
#include "media/mm_comm_vi.h"
#include "media/mm_comm_vo.h"

/* board musl 1.1.16 has plain dlsym, not the cross toolchain's __dlsym_time64 */
extern void *aw_dlsym(void *handle, const char *symbol) __asm__("dlsym");

#define HLAY(chn,lyl) ((chn)*4+(lyl))      /* from vo/hwdisplay.h */

/* ---- resolved AW_MPI entry points ----------------------------------------- */
static int (*SYS_SetConf)(const MPP_SYS_CONF_S*);
static int (*SYS_Init)(void);
static int (*SYS_Exit)(void);
static int (*SYS_Bind)(MPP_CHN_S*, MPP_CHN_S*);
static int (*SYS_UnBind)(MPP_CHN_S*, MPP_CHN_S*);
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
static int (*VO_Enable)(int);
static int (*VO_Disable)(int);
static int (*VO_AddOutsideVideoLayer)(int);
static int (*VO_RemoveOutsideVideoLayer)(int);
static int (*VO_OpenVideoLayer)(int);
static int (*VO_CloseVideoLayer)(int);
static int (*VO_GetPubAttr)(int, VO_PUB_ATTR_S*);
static int (*VO_SetPubAttr)(int, const VO_PUB_ATTR_S*);
static int (*VO_EnableVideoLayer)(int);
static int (*VO_DisableVideoLayer)(int);
static int (*VO_GetVideoLayerAttr)(int, VO_VIDEO_LAYER_ATTR_S*);
static int (*VO_SetVideoLayerAttr)(int, const VO_VIDEO_LAYER_ATTR_S*);
static int (*VO_EnableChn)(int, int);
static int (*VO_DisableChn)(int, int);
static int (*VO_SetChnDispBufNum)(int, int, unsigned int);
static int (*VO_StartChn)(int, int);
static int (*VO_StopChn)(int, int);

static int resolve(void){
    const char *libs[] = { "/usr/lib/eyesee-mpp/libmedia_mpp.so",
                           "/usr/lib/eyesee-mpp/libmpp_isp.so",
                           "/usr/lib/eyesee-mpp/libmpp_vi.so",
                           "/usr/lib/eyesee-mpp/libmpp_vo.so" };
    for (size_t i=0;i<sizeof libs/sizeof libs[0];i++)
        if (!dlopen(libs[i], RTLD_NOW|RTLD_GLOBAL)){ fprintf(stderr,"dlopen %s: %s\n",libs[i],dlerror()); return -1; }
    #define SYM(v,name) do{ *(void**)(&v)=aw_dlsym(RTLD_DEFAULT,name); \
        if(!v){ fprintf(stderr,"dlsym %s failed\n",name); return -1; } }while(0)
    SYM(SYS_SetConf,"AW_MPI_SYS_SetConf"); SYM(SYS_Init,"AW_MPI_SYS_Init"); SYM(SYS_Exit,"AW_MPI_SYS_Exit");
    SYM(SYS_Bind,"AW_MPI_SYS_Bind"); SYM(SYS_UnBind,"AW_MPI_SYS_UnBind");
    SYM(ISP_Init,"AW_MPI_ISP_Init"); SYM(ISP_Run,"AW_MPI_ISP_Run");
    SYM(ISP_Stop,"AW_MPI_ISP_Stop"); SYM(ISP_Exit,"AW_MPI_ISP_Exit");
    SYM(VI_CreateVipp,"AW_MPI_VI_CreateVipp"); SYM(VI_SetVippAttr,"AW_MPI_VI_SetVippAttr");
    SYM(VI_EnableVipp,"AW_MPI_VI_EnableVipp"); SYM(VI_DisableVipp,"AW_MPI_VI_DisableVipp");
    SYM(VI_DestoryVipp,"AW_MPI_VI_DestoryVipp");
    SYM(VI_CreateVirChn,"AW_MPI_VI_CreateVirChn"); SYM(VI_EnableVirChn,"AW_MPI_VI_EnableVirChn");
    SYM(VI_DisableVirChn,"AW_MPI_VI_DisableVirChn"); SYM(VI_DestoryVirChn,"AW_MPI_VI_DestoryVirChn");
    SYM(VO_Enable,"AW_MPI_VO_Enable"); SYM(VO_Disable,"AW_MPI_VO_Disable");
    SYM(VO_AddOutsideVideoLayer,"AW_MPI_VO_AddOutsideVideoLayer");
    SYM(VO_RemoveOutsideVideoLayer,"AW_MPI_VO_RemoveOutsideVideoLayer");
    SYM(VO_OpenVideoLayer,"AW_MPI_VO_OpenVideoLayer");
    SYM(VO_CloseVideoLayer,"AW_MPI_VO_CloseVideoLayer");
    SYM(VO_GetPubAttr,"AW_MPI_VO_GetPubAttr"); SYM(VO_SetPubAttr,"AW_MPI_VO_SetPubAttr");
    SYM(VO_EnableVideoLayer,"AW_MPI_VO_EnableVideoLayer"); SYM(VO_DisableVideoLayer,"AW_MPI_VO_DisableVideoLayer");
    SYM(VO_GetVideoLayerAttr,"AW_MPI_VO_GetVideoLayerAttr"); SYM(VO_SetVideoLayerAttr,"AW_MPI_VO_SetVideoLayerAttr");
    SYM(VO_EnableChn,"AW_MPI_VO_EnableChn"); SYM(VO_DisableChn,"AW_MPI_VO_DisableChn");
    SYM(VO_SetChnDispBufNum,"AW_MPI_VO_SetChnDispBufNum");
    SYM(VO_StartChn,"AW_MPI_VO_StartChn"); SYM(VO_StopChn,"AW_MPI_VO_StopChn");
    #undef SYM
    return 0;
}

static int g_isp = 0;
static void *isp_thread(void *a){ (void)a; ISP_Run(g_isp); return NULL; }
static volatile sig_atomic_t g_stop = 0;
static void on_stop(int s){ (void)s; g_stop = 1; }
static void on_alarm(int s){ (void)s; const char m[]="\n[watchdog] stalled, exiting\n"; write(2,m,sizeof m-1); _exit(4); }
static void fb_unblank(void){      /* MPP SYS_Exit resets disp; turn the panel back on */
    int fd = open("/dev/fb0", O_RDWR);
    if (fd>=0){ ioctl(fd, FBIOBLANK, FB_BLANK_UNBLANK); close(fd); }
}

int main(int argc, char **argv){
    setvbuf(stdout, NULL, _IONBF, 0);
    int W=320, H=240, dev=0, chn=0, secs=0;   /* secs<=0 = run until SIGTERM */
    if (argc>1) sscanf(argv[1], "%dx%d", &W, &H);
    if (argc>2) secs = atoi(argv[2]);
    int uiLayer = HLAY(2,0);                      /* fb/UI layer to hide = 8 */

    signal(SIGTERM, on_stop); signal(SIGINT, on_stop);
    signal(SIGALRM, on_alarm); alarm(20);         /* guards setup only */

    if (resolve() < 0) return 2;

    MPP_SYS_CONF_S sys; memset(&sys,0,sizeof sys); sys.nAlignWidth = 32;
    SYS_SetConf(&sys);
    if (SYS_Init() != SUCCESS){ fprintf(stderr,"SYS_Init failed\n"); return 2; }

    ISP_Init();
    pthread_t t; pthread_create(&t, NULL, isp_thread, NULL);
    usleep(200000);
    printf("SYS+ISP up\n");

    /* progress flags so cleanup undoes exactly what was set up, in reverse */
    int vipp_created=0, vipp_enabled=0, virchn_created=0, virchn_enabled=0;
    int layer=-1, vochn=-1, vo_enabled=0, bound=0, ui_managed=0;
    MPP_CHN_S viCh = { MOD_ID_VIU, dev, chn };
    MPP_CHN_S voCh = { MOD_ID_VOU, 0, 0 };

    /* ---- VI (camera) ---- */
    VI_ATTR_S attr; memset(&attr,0,sizeof attr);
    attr.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
    attr.memtype = V4L2_MEMORY_MMAP;
    attr.format.pixelformat = V4L2_PIX_FMT_NV21M;
    attr.format.field = V4L2_FIELD_NONE;
    attr.format.width = W; attr.format.height = H;
    attr.nbufs = 5; attr.nplanes = 2; attr.fps = 30;
    attr.capturemode = V4L2_MODE_VIDEO;
    VI_CreateVipp(dev); vipp_created=1;
    VI_SetVippAttr(dev, &attr);
    VI_EnableVipp(dev); vipp_enabled=1;
    VI_CreateVirChn(dev, chn, NULL); virchn_created=1;   /* enabled later, after bind */

    /* ---- VO (display) ---- */
    VO_Enable(0); vo_enabled=1;
    VO_AddOutsideVideoLayer(uiLayer);
    VO_CloseVideoLayer(uiLayer); ui_managed=1;    /* hide the UI/fb layer */
    VO_PUB_ATTR_S pub; memset(&pub,0,sizeof pub);
    VO_GetPubAttr(0, &pub);
    pub.enIntfType = VO_INTF_LCD;
    pub.enIntfSync = VO_OUTPUT_NTSC;              /* placeholder; panel timing from disp drv */
    VO_SetPubAttr(0, &pub);

    for (int hwch=0; hwch<4; hwch++){             /* find first enable-able video layer */
        int hl = HLAY(hwch,0);
        if (VO_EnableVideoLayer(hl) == SUCCESS){ layer = hl; break; }
    }
    if (layer < 0){ fprintf(stderr,"no VO video layer\n"); goto cleanup; }

    VO_VIDEO_LAYER_ATTR_S la; memset(&la,0,sizeof la);
    VO_GetVideoLayerAttr(layer, &la);
    la.stDispRect.X = 0; la.stDispRect.Y = 0;
    la.stDispRect.Width = 240; la.stDispRect.Height = 240;   /* fill the 240x240 panel */
    VO_SetVideoLayerAttr(layer, &la);

    for (int c=0; c<4; c++){ if (VO_EnableChn(layer, c) == SUCCESS){ vochn = c; break; } }
    if (vochn < 0){ fprintf(stderr,"no VO channel\n"); goto cleanup; }
    VO_SetChnDispBufNum(layer, vochn, 2);

    /* ---- connect camera -> display, then start ---- */
    voCh.mDevId = layer; voCh.mChnId = vochn;
    if (SYS_Bind(&viCh, &voCh) != SUCCESS){ fprintf(stderr,"SYS_Bind VI->VO failed\n"); goto cleanup; }
    bound=1;
    VI_EnableVirChn(dev, chn); virchn_enabled=1;
    VO_StartChn(layer, vochn);

    alarm(0);                                     /* preview may run unbounded */
    printf("LIVE: VIPP%d/chn%d -> VO layer%d/chn%d, %dx%d on panel%s (SIGTERM to stop)\n",
           dev, chn, layer, vochn, W, H, secs>0 ? "" : ", until stopped");
    for (int i=0; (secs<=0 || i<secs) && !g_stop; i++) sleep(1);
    printf("stopping\n");
    alarm(25);                                    /* re-arm: guard teardown deadlocks */

cleanup:
    /* reverse order; VirChn MUST be destroyed before VIPP is disabled, else the
     * VIN capture thread spins "Virvi Com not exit ... wait" forever. */
    if (vochn >= 0)       VO_StopChn(layer, vochn);
    if (virchn_enabled)   VI_DisableVirChn(dev, chn);   /* stop source before unbind */
    if (bound)            SYS_UnBind(&viCh, &voCh);
    if (virchn_created)   VI_DestoryVirChn(dev, chn);   /* destroy chn before disabling vipp */
    if (vochn >= 0)       VO_DisableChn(layer, vochn);
    if (layer >= 0)       VO_DisableVideoLayer(layer);
    if (ui_managed){ VO_OpenVideoLayer(uiLayer);          /* re-show the UI/fb layer */
                     VO_RemoveOutsideVideoLayer(uiLayer); }
    if (vo_enabled)       VO_Disable(0);
    if (vipp_enabled)     VI_DisableVipp(dev);
    if (vipp_created)     VI_DestoryVipp(dev);
    ISP_Stop(g_isp); pthread_join(t, NULL); ISP_Exit();
    SYS_Exit();
    fb_unblank();                                 /* leave the panel on, not black */
    printf("done\n");
    return 0;
}
