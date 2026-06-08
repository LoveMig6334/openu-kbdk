/* nnacam.cpp - live camera + on-screen AI image classification (KidBright uAI, V831).
 *
 * Shows the live camera on the 240x240 LCD AND classifies it on the NPU, drawing the
 * predicted label straight onto the panel in real time. One process does it all:
 *
 *   capture NV21 (Allwinner MPP VI+ISP, dlopen'd, same path as cammpp.c/camcc.c)
 *     -> colour-correct + blit the full frame to /dev/fb0   (the live preview)
 *     -> centre-crop + downscale to 32x32 RGB
 *     -> classify on the NPU (NVDLA, vendored CIFAR-10 net, third_party/v831-npu)
 *     -> overlay "LABEL score" as text on the panel (built-in 8x8 bitmap font)
 *
 * No VO video layer is brought up, so MPP leaves the fb/UI layer visible and our
 * /dev/fb0 writes show through (see camcc.c note). MPP symbols are dlopen'd; the NPU
 * is driven via /dev/mem + /dev/ion + /dev/cedar_dev (no kernel driver).
 *
 * Colour is fixed with gray-world auto white balance (per-channel gain, adapts to the
 * lighting) -- the board ISP's green cast can't be neutralised by a fixed U/V offset.
 *
 * Build: make nnacam
 * Run:   LD_LIBRARY_PATH=/usr/lib/eyesee-mpp:/usr/lib /tmp/nnacam [WxH] [nframes] [sat flip]
 *        e.g.  /tmp/nnacam 320x240 0            (run until SIGTERM, auto white balance)
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

#define CLAMP8(x) ((x)<0?0:((x)>255?255:(x)))
#ifndef FBIO_WAITFORVSYNC
#define FBIO_WAITFORVSYNC _IOW('F', 0x20, unsigned int)   /* sunxi-disp supports this */
#endif

static int g_isp = 0;
static volatile sig_atomic_t g_stop = 0;
static void *isp_thread(void *arg){ (void)arg; ISP_Run(g_isp); return NULL; }
static void on_stop(int s){ (void)s; g_stop = 1; }
static void on_alarm(int s){ (void)s; const char m[]="\n[watchdog] timed out\n"; write(2,m,sizeof m-1); _exit(4); }

/* ---- tiny 5x7-in-8x8 bitmap font (uppercase, digits, few punct) ------------ */
/* each glyph is 8 rows; pixel set when bit (0x80>>col) is 1, cols 0..4 used. */
typedef struct { char c; uint8_t r[8]; } glyph_t;
static const glyph_t FONT[] = {
 {' ',{0,0,0,0,0,0,0,0}},
 {'-',{0x00,0x00,0x00,0x70,0x00,0x00,0x00,0x00}},
 {':',{0x00,0x20,0x20,0x00,0x20,0x20,0x00,0x00}},
 {'.',{0x00,0x00,0x00,0x00,0x00,0x60,0x60,0x00}},
 {'%',{0x88,0x90,0x20,0x40,0x88,0x08,0x00,0x00}},
 {'0',{0x70,0x88,0x98,0xA8,0xC8,0x88,0x70,0x00}},
 {'1',{0x20,0x60,0x20,0x20,0x20,0x20,0x70,0x00}},
 {'2',{0x70,0x88,0x08,0x10,0x20,0x40,0xF8,0x00}},
 {'3',{0x70,0x88,0x08,0x30,0x08,0x88,0x70,0x00}},
 {'4',{0x10,0x18,0x28,0x88,0xF8,0x10,0x10,0x00}},
 {'5',{0xF8,0x80,0x80,0xF0,0x08,0x88,0x70,0x00}},
 {'6',{0x70,0x80,0x80,0xF0,0x88,0x88,0x70,0x00}},
 {'7',{0xF8,0x08,0x10,0x20,0x40,0x40,0x40,0x00}},
 {'8',{0x70,0x88,0x88,0x70,0x88,0x88,0x70,0x00}},
 {'9',{0x70,0x88,0x88,0x78,0x08,0x08,0x70,0x00}},
 {'A',{0x70,0x88,0x88,0xF8,0x88,0x88,0x88,0x00}},
 {'B',{0xF0,0x88,0x88,0xF0,0x88,0x88,0xF0,0x00}},
 {'C',{0x70,0x88,0x80,0x80,0x80,0x88,0x70,0x00}},
 {'D',{0xE0,0x90,0x88,0x88,0x88,0x90,0xE0,0x00}},
 {'E',{0xF8,0x80,0x80,0xF0,0x80,0x80,0xF8,0x00}},
 {'F',{0xF8,0x80,0x80,0xF0,0x80,0x80,0x80,0x00}},
 {'G',{0x70,0x88,0x80,0xB8,0x88,0x88,0x70,0x00}},
 {'H',{0x88,0x88,0x88,0xF8,0x88,0x88,0x88,0x00}},
 {'I',{0x70,0x20,0x20,0x20,0x20,0x20,0x70,0x00}},
 {'J',{0x38,0x10,0x10,0x10,0x90,0x90,0x60,0x00}},
 {'K',{0x88,0x90,0xA0,0xC0,0xA0,0x90,0x88,0x00}},
 {'L',{0x80,0x80,0x80,0x80,0x80,0x80,0xF8,0x00}},
 {'M',{0x88,0xD8,0xA8,0xA8,0x88,0x88,0x88,0x00}},
 {'N',{0x88,0xC8,0xA8,0x98,0x88,0x88,0x88,0x00}},
 {'O',{0x70,0x88,0x88,0x88,0x88,0x88,0x70,0x00}},
 {'P',{0xF0,0x88,0x88,0xF0,0x80,0x80,0x80,0x00}},
 {'Q',{0x70,0x88,0x88,0x88,0xA8,0x90,0x68,0x00}},
 {'R',{0xF0,0x88,0x88,0xF0,0xA0,0x90,0x88,0x00}},
 {'S',{0x70,0x88,0x80,0x70,0x08,0x88,0x70,0x00}},
 {'T',{0xF8,0x20,0x20,0x20,0x20,0x20,0x20,0x00}},
 {'U',{0x88,0x88,0x88,0x88,0x88,0x88,0x70,0x00}},
 {'V',{0x88,0x88,0x88,0x88,0x88,0x50,0x20,0x00}},
 {'W',{0x88,0x88,0x88,0xA8,0xA8,0xD8,0x88,0x00}},
 {'X',{0x88,0x88,0x50,0x20,0x50,0x88,0x88,0x00}},
 {'Y',{0x88,0x88,0x50,0x20,0x20,0x20,0x20,0x00}},
 {'Z',{0xF8,0x08,0x10,0x20,0x40,0x80,0xF8,0x00}},
};
static const uint8_t *glyph_of(char c){
    if (c>='a' && c<='z') c -= 32;                 /* fold to uppercase */
    for (size_t i=0;i<sizeof FONT/sizeof FONT[0];i++) if (FONT[i].c==c) return FONT[i].r;
    return FONT[0].r;                              /* unknown -> blank */
}

/* framebuffer globals (set up once in main) */
static uint8_t *FB; static int FBW, FBH, FBLL, FBBpp, FBrO, FBgO, FBbO;

static void fb_rect(int x,int y,int w,int h,int r,int g,int b){
    for (int j=0;j<h;j++){ int yy=y+j; if(yy<0||yy>=FBH) continue;
        uint8_t *row = FB + (size_t)yy*FBLL;
        for (int i=0;i<w;i++){ int xx=x+i; if(xx<0||xx>=FBW) continue;
            uint8_t *p = row + (size_t)xx*FBBpp; p[FBrO]=r; p[FBgO]=g; p[FBbO]=b; } }
}
static void fb_char(int x,int y,char c,int sc,int r,int g,int b){
    const uint8_t *gl = glyph_of(c);
    for (int ry=0; ry<8; ry++){ uint8_t bits=gl[ry];
        for (int rx=0; rx<5; rx++) if (bits & (0x80>>rx)) fb_rect(x+rx*sc, y+ry*sc, sc, sc, r,g,b); }
}
/* draw text with a dark background bar behind it for contrast */
static void fb_text(int x,int y,const char *s,int sc,int r,int g,int b){
    int cw=(5+1)*sc, len=(int)strlen(s);
    fb_rect(x-2, y-2, len*cw+2, 8*sc+4, 0,0,0);
    int cx=x; for (; *s; s++){ fb_char(cx,y,*s,sc,r,g,b); cx += cw; }
}

/* BT.601 chroma coefficients (saturation folded in, fixed-point <<SH). */
static int RV,GU,GV,BU;
static const int SH=8;

/* Gray-world AUTO white balance: per-channel gains (<<8), EMA-smoothed across frames.
 * This board's ISP leaves a strong green cast that a fixed U/V offset CANNOT neutralize
 * (offsets only shift R via V and B via U; they can't pull green down relative to luma).
 * Instead we equalize the channel averages every frame -> adapts to any lighting with no
 * hardcoded trim. Shared by the panel blit AND the NPU input so both see the same neutral
 * colour (and the classifier isn't fooled by a colour cast). */
static int gR=256, gG=256, gB=256;

/* sample the centre square, convert to RGB, and update the gray-world gains so the
 * per-channel averages converge to a common grey (gains clamped 0.4x..2.5x). */
static void update_awb(const uint8_t *Y, const uint8_t *VU, int W, int H){
    int side = (W < H) ? W : H;
    int xoff = (W - side)/2, yoff = (H - side)/2;
    long sr=0,sg=0,sb=0; int n=0;
    for (int oy=0; oy<32; oy++){
        int sy = yoff + oy*side/32;
        const uint8_t *yl = Y  + (size_t)sy*W;
        const uint8_t *cl = VU + (size_t)(sy>>1)*W;
        for (int ox=0; ox<32; ox++){
            int sx = xoff + ox*side/32;
            int yv = yl[sx]; int ci = (sx>>1)<<1;
            int cv = cl[ci]-128, cu = cl[ci+1]-128;
            int r = yv + ((cv*RV)>>SH);
            int g = yv - ((cu*GU + cv*GV)>>SH);
            int b = yv + ((cu*BU)>>SH);
            sr += CLAMP8(r); sg += CLAMP8(g); sb += CLAMP8(b); n++;
        }
    }
    if (!n || sr<=0 || sg<=0 || sb<=0) return;
    long t = (sr+sg+sb)/3;
    int nR=(int)(t*256/sr), nG=(int)(t*256/sg), nB=(int)(t*256/sb);
    const int lo=102, hi=640;                            /* 0.40x .. 2.5x */
    if(nR<lo)nR=lo; if(nR>hi)nR=hi;
    if(nG<lo)nG=lo; if(nG>hi)nG=hi;
    if(nB<lo)nB=lo; if(nB>hi)nB=hi;
    gR=(gR*7+nR)>>3; gG=(gG*7+nG)>>3; gB=(gB*7+nB)>>3;  /* EMA, ~8-frame time constant */
}

/* centre-square downscale of an NV21 frame to 32x32 interleaved RGB, gray-world balanced */
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
            int r = (yv + ((cv*RV)>>SH)) * gR >> 8;     /* + gray-world gain */
            int g = (yv - ((cu*GU + cv*GV)>>SH)) * gG >> 8;
            int b = (yv + ((cu*BU)>>SH)) * gB >> 8;
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
    double sat  = (argc>3) ? atof(argv[3]) :  1.3;      /* saturation multiplier */
    int    flip = (argc>4) ? atoi(argv[4]) :  0;        /* raw sensor is already upright */

    /* fold saturation into the BT.601 coefficients (fixed-point, <<SH); used by BOTH the
     * panel blit and the NPU input. White balance is handled adaptively by update_awb(). */
    RV=(int)(1.402*sat*256+0.5); GU=(int)(0.344*sat*256+0.5);
    GV=(int)(0.714*sat*256+0.5); BU=(int)(1.772*sat*256+0.5);

    signal(SIGTERM,on_stop); signal(SIGINT,on_stop);
    signal(SIGALRM,on_alarm); alarm(25);                /* guards setup */

    /* ---- framebuffer (live preview target) ---- */
    int fbfd = open("/dev/fb0", O_RDWR);
    if (fbfd<0){ perror("open /dev/fb0"); return 2; }
    struct fb_var_screeninfo v; struct fb_fix_screeninfo f;
    if (ioctl(fbfd,FBIOGET_VSCREENINFO,&v)<0 || ioctl(fbfd,FBIOGET_FSCREENINFO,&f)<0){ perror("fb info"); return 2; }
    FBW=v.xres; FBH=v.yres; FBBpp=v.bits_per_pixel/8; FBLL=f.line_length;
    FBrO=v.red.offset/8; FBgO=v.green.offset/8; FBbO=v.blue.offset/8;
    size_t maplen = f.smem_len ? f.smem_len : (size_t)FBLL*v.yres_virtual;
    FB = (uint8_t*)mmap(NULL,maplen,PROT_READ|PROT_WRITE,MAP_SHARED,fbfd,0);
    if (FB==MAP_FAILED){ perror("mmap fb"); return 2; }
    v.xoffset=0; v.yoffset=0; v.activate=FB_ACTIVATE_NOW;    /* pin visible page, never pan */
    ioctl(fbfd, FBIOPAN_DISPLAY, &v);
    int vsync_ok=1; unsigned vzero=0;
    printf("fb %dx%d %dbpp LL=%d  colour: gray-world AWB, sat=%.2f flip=%d\n",
           FBW,FBH,v.bits_per_pixel,FBLL,sat,flip);

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

    /* centre-crop the WxH capture down to the FBWxFBH panel */
    int xoff = (W-FBW)/2; if (xoff<0) xoff=0;
    int yoff = (H-FBH)/2; if (yoff<0) yoff=0;

    int rc=2, vipp=0, vch=0;
    if (VI_CreateVipp(dev)!=SUCCESS){ fprintf(stderr,"CreateVipp failed\n"); goto out; } vipp=1;
    VI_SetVippAttr(dev,&attr);
    if (VI_EnableVipp(dev)!=SUCCESS){ fprintf(stderr,"EnableVipp failed\n"); goto out; }
    if (VI_CreateVirChn(dev,chn,NULL)!=SUCCESS){ fprintf(stderr,"CreateVirChn failed\n"); goto out; }
    if (VI_EnableVirChn(dev,chn)!=SUCCESS){ fprintf(stderr,"EnableVirChn failed\n"); goto out; } vch=1;

    alarm(0);
    printf("LIVE: camera on panel + NPU classify overlay (%dx%d). SIGTERM to stop.\n", W, H);

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

        /* adapt white balance to this frame, then classify on the NPU (centre 32x32) */
        update_awb(Y, VU, fr->mWidth, fr->mHeight);
        nv21_to_rgb32(Y, VU, fr->mWidth, fr->mHeight, rgb);
        nna_set_input_rgb(rgb);
        cifar10();                                      /* runs on the NPU, fills nna_pred/nna_scores */
        int s1=-1000,s2=-1000;
        for (int i=0;i<10;i++){ if(nna_scores[i]>s1){ s2=s1; s1=nna_scores[i]; } else if(nna_scores[i]>s2) s2=nna_scores[i]; }

        /* render the live frame to the panel (colour-corrected), gated on vblank */
        if (vsync_ok && ioctl(fbfd,FBIO_WAITFORVSYNC,&vzero)<0) vsync_ok=0;
        for (int dy=0; dy<FBH; dy++){
            int sy = yoff + dy;
            uint8_t *row = FB + (size_t)(flip?(FBH-1-dy):dy)*FBLL;
            const uint8_t *yl = Y  + (size_t)sy*(int)fr->mWidth;
            const uint8_t *cl = VU + (size_t)(sy>>1)*(int)fr->mWidth;
            for (int dx=0; dx<FBW; dx++){
                int sx = xoff + dx;
                int yv = yl[sx];
                int ci = (sx>>1)<<1;
                int cv = cl[ci]   - 128;                /* V (Cr) */
                int cu = cl[ci+1] - 128;                /* U (Cb) */
                int r = (yv + ((cv*RV)>>SH)) * gR >> 8; /* + gray-world gain */
                int g = (yv - ((cu*GU + cv*GV)>>SH)) * gG >> 8;
                int b = (yv + ((cu*BU)>>SH)) * gB >> 8;
                uint8_t *p = row + (size_t)(flip?(FBW-1-dx):dx)*FBBpp;
                p[FBrO]=CLAMP8(r); p[FBgO]=CLAMP8(g); p[FBbO]=CLAMP8(b);
            }
        }

        /* overlay the prediction as text on the panel */
        char line[40];
        snprintf(line,sizeof line,"%s %d", nna_label(nna_pred), s1);
        fb_text(4, 4, line, 3, 0,255,0);                /* green, scale 3 (~24px) */

        VI_ReleaseFrame(dev,chn,&fi);
        if (frames==0) ioctl(fbfd,FBIOBLANK,FB_BLANK_UNBLANK);

        if (k%15==0)
            printf("frame %3lu  avgY=%3lu  awb[R%.2f G%.2f B%.2f]  -> %-11s (score %d, margin %d)\n",
                   frames, ay, gR/256.0, gG/256.0, gB/256.0, nna_label(nna_pred), s1, s1-s2);
        frames++; rc=0;
    }
    alarm(25);
    printf("stopping after %lu classified frames (vsync %s)\n",
           frames, vsync_ok?"on":"UNSUPPORTED");
    }

out:
    if (vch)  { VI_DisableVirChn(dev,chn); VI_DestoryVirChn(dev,chn); }
    if (vipp) { VI_DisableVipp(dev); VI_DestoryVipp(dev); }
    ISP_Stop(g_isp); pthread_join(t,NULL); ISP_Exit();
    SYS_Exit();
    ioctl(fbfd,FBIOBLANK,FB_BLANK_UNBLANK);             /* leave panel on */
    munmap(FB,maplen); close(fbfd);
    xreg_close(); nna_off();
    printf(rc==0 ? "OK\n" : "FAILED\n");
    return rc;
}
