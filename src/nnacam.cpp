/* nnacam.cpp - live camera + on-screen AI image classification (KidBright uAI, V831).
 *
 * Shows the live camera on the 240x240 LCD AND classifies it, drawing the predicted
 * label straight onto the panel in real time. One process:
 *
 *   capture NV21 (Allwinner MPP VI+ISP, dlopen'd, same path as cammpp.c/camcc.c)
 *     -> gray-world auto white balance (update_awb) + blit to /dev/fb0 (the live preview)
 *     -> centre-crop + scale to 224x224 RGB
 *     -> classify with ResNet18 (ImageNet 1000-class) via the board's AWNN runtime
 *        (libmaix_nn.so, dlopen'd -- the .param/.bin format, magic 7767517)
 *     -> overlay "LABEL pct%" as text on the panel (built-in 8x8 bitmap font)
 *
 * Inference (ResNet18@224, ~100 ms on the single A7) runs on a BACKGROUND THREAD so the
 * camera preview stays smooth (~30 fps); the worker always classifies the most recent
 * frame and the main loop draws the latest label. AWNN runs on the CPU here because the
 * NPU kernel driver is absent (see CLAUDE.md); loading nna_sunxi.ko would move it onto
 * the NPU with no code change. (The earlier NVDLA/CIFAR-10 nnacam is in git history; the
 * NVDLA userspace path still ships as `make nna-cifar10`.)
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
#include <math.h>
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

extern "C" {
#include "libmaix_nn.h"
/* libmaix_nn.so has undefined back-refs to retinaface decoder hooks (normally in the
 * python ext); musl binds immediately, so export harmless stubs (-Wl,--export-dynamic). */
void retinaface_get_priorboxes(void){}
void retinaface_decode(void){}
}

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

static int resolve_mpp(void){
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

/* ---- AWNN (libmaix_nn) entry points resolved at runtime --------------------- */
#define NN_SO     "/usr/lib/python3.8/site-packages/maix/libmaix_nn.so"
#define MODEL_PARAM "/home/model/resnet18_1000_awnn.param"
#define MODEL_BIN   "/home/model/resnet18_1000_awnn.bin"
#define LABELS_PATH "/tmp/imagenet1000_labels.txt"   /* tmpfs: rootfs writes here proved flaky */
#define IN_W 224
#define IN_H 224
#define NCLASS 1000

typedef libmaix_err_t (*fn_module_init)(void);
typedef libmaix_nn_t* (*fn_create)(void);
typedef void          (*fn_destroy)(libmaix_nn_t**);
static fn_module_init nn_module_init;
static fn_create      nn_create;
static fn_destroy     nn_destroy;

static char *g_labels[NCLASS];
static int load_labels(const char *path){
    FILE *f = fopen(path,"r"); if(!f) return 0;
    char line[160]; int i=0;
    while(i<NCLASS && fgets(line,sizeof line,f)){
        size_t n=strlen(line); while(n && (line[n-1]=='\n'||line[n-1]=='\r')) line[--n]=0;
        g_labels[i++] = strdup(line);
    }
    fclose(f); return i;
}

#define CLAMP8(x) ((x)<0?0:((x)>255?255:(x)))
#ifndef FBIO_WAITFORVSYNC
#define FBIO_WAITFORVSYNC _IOW('F', 0x20, unsigned int)
#endif

static int g_isp = 0;
static volatile sig_atomic_t g_stop = 0;
static void *isp_thread(void *arg){ (void)arg; ISP_Run(g_isp); return NULL; }
static void on_stop(int s){ (void)s; g_stop = 1; }
static void on_alarm(int s){ (void)s; const char m[]="\n[watchdog] timed out\n"; write(2,m,sizeof m-1); _exit(4); }

/* ---- tiny 5x7-in-8x8 bitmap font (uppercase, digits, few punct) ------------ */
typedef struct { char c; uint8_t r[8]; } glyph_t;
static const glyph_t FONT[] = {
 {' ',{0,0,0,0,0,0,0,0}},
 {'-',{0x00,0x00,0x00,0x70,0x00,0x00,0x00,0x00}},
 {':',{0x00,0x20,0x20,0x00,0x20,0x20,0x00,0x00}},
 {'.',{0x00,0x00,0x00,0x00,0x00,0x60,0x60,0x00}},
 {',',{0x00,0x00,0x00,0x00,0x00,0x60,0x60,0x40}},
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
    if (c>='a' && c<='z') c -= 32;
    for (size_t i=0;i<sizeof FONT/sizeof FONT[0];i++) if (FONT[i].c==c) return FONT[i].r;
    return FONT[0].r;
}

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
static void fb_text(int x,int y,const char *s,int sc,int r,int g,int b){
    int cw=(5+1)*sc, len=(int)strlen(s);
    fb_rect(x-2, y-2, len*cw+2, 8*sc+4, 0,0,0);
    int cx=x; for (; *s; s++){ fb_char(cx,y,*s,sc,r,g,b); cx += cw; }
}

/* gray-world AUTO white balance (see git log / camcc note): per-channel gains <<8,
 * EMA-smoothed -- fixed U/V offsets can't neutralise the ISP's green cast. */
static int RV,GU,GV,BU;
static const int SH=8;
static int gR=256, gG=256, gB=256;

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
    const int lo=102, hi=640;
    if(nR<lo)nR=lo; if(nR>hi)nR=hi;
    if(nG<lo)nG=lo; if(nG>hi)nG=hi;
    if(nB<lo)nB=lo; if(nB>hi)nB=hi;
    gR=(gR*7+nR)>>3; gG=(gG*7+nG)>>3; gB=(gB*7+nB)>>3;
}

/* centre-square downscale of an NV21 frame to IN_W x IN_H interleaved RGB, gray-world balanced */
static void nv21_to_rgb_in(const uint8_t *Y, const uint8_t *VU, int W, int H, unsigned char *rgb){
    int side = (W < H) ? W : H;
    int xoff = (W - side)/2, yoff = (H - side)/2;
    for (int oy=0; oy<IN_H; oy++){
        int sy = yoff + oy*side/IN_H;
        const uint8_t *yl = Y  + (size_t)sy*W;
        const uint8_t *cl = VU + (size_t)(sy>>1)*W;
        for (int ox=0; ox<IN_W; ox++){
            int sx = xoff + ox*side/IN_W;
            int yv = yl[sx]; int ci = (sx>>1)<<1;
            int cv = cl[ci]   - 128;
            int cu = cl[ci+1] - 128;
            int r = (yv + ((cv*RV)>>SH)) * gR >> 8;
            int g = (yv - ((cu*GU + cv*GV)>>SH)) * gG >> 8;
            int b = (yv + ((cu*BU)>>SH)) * gB >> 8;
            unsigned char *p = rgb + ((size_t)oy*IN_W + ox)*3;
            p[0]=CLAMP8(r); p[1]=CLAMP8(g); p[2]=CLAMP8(b);
        }
    }
}

/* ---- inference worker (keeps the heavy ResNet18 forward off the preview loop) -- */
static libmaix_nn_t *g_nn = NULL;
static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t  g_cond = PTHREAD_COND_INITIALIZER;
static unsigned char g_in[IN_W*IN_H*3];
static int  g_in_ready = 0;
static char g_label[96] = "...";
static unsigned long g_infers = 0;

static void *infer_thread(void *arg){
    (void)arg;
    unsigned char *local = (unsigned char*)malloc(IN_W*IN_H*3);
    unsigned char *qbuf  = (unsigned char*)malloc(IN_W*IN_H*3);
    float *out = (float*)malloc(sizeof(float)*NCLASS);
    while (!g_stop){
        pthread_mutex_lock(&g_lock);
        while (!g_in_ready && !g_stop) pthread_cond_wait(&g_cond,&g_lock);
        if (g_stop){ pthread_mutex_unlock(&g_lock); break; }
        memcpy(local, g_in, IN_W*IN_H*3); g_in_ready = 0;
        pthread_mutex_unlock(&g_lock);

        libmaix_nn_layer_t in; memset(&in,0,sizeof in);
        in.w=IN_W; in.h=IN_H; in.c=3;
        in.dtype=LIBMAIX_NN_DTYPE_UINT8; in.layout=LIBMAIX_NN_LAYOUT_HWC;
        in.need_quantization=true; in.data=local; in.buff_quantization=qbuf;
        libmaix_nn_layer_t o; memset(&o,0,sizeof o);
        o.w=1; o.h=1; o.c=NCLASS;
        o.dtype=LIBMAIX_NN_DTYPE_FLOAT; o.layout=LIBMAIX_NN_LAYOUT_HWC;
        o.need_quantization=false; o.data=out; o.buff_quantization=NULL;
        if (g_nn->forward(g_nn, &in, &o) != LIBMAIX_ERR_NONE) continue;

        int best=0; float mx=out[0];
        for (int i=1;i<NCLASS;i++) if (out[i]>mx){ mx=out[i]; best=i; }
        double sum=0; for (int i=0;i<NCLASS;i++) sum += expf(out[i]-mx);
        int pct = (sum>0) ? (int)(100.0/sum + 0.5) : 0;     /* softmax prob of top-1 */
        const char *nm = g_labels[best] ? g_labels[best] : "?";
        char buf[96]; snprintf(buf,sizeof buf,"%s %d%%", nm, pct);

        pthread_mutex_lock(&g_lock);
        strncpy(g_label, buf, sizeof g_label-1); g_label[sizeof g_label-1]=0;
        g_infers++;
        pthread_mutex_unlock(&g_lock);
    }
    free(local); free(qbuf); free(out);
    return NULL;
}

int main(int argc, char **argv){
    setvbuf(stdout, NULL, _IONBF, 0);
    int W=320, H=240, dev=0, chn=0;
    if (argc>1) sscanf(argv[1], "%dx%d", &W, &H);
    int nframes = (argc>2) ? atoi(argv[2]) : 0;         /* 0 = run until SIGTERM */
    double sat  = (argc>3) ? atof(argv[3]) :  1.3;      /* saturation multiplier */
    int    flip = (argc>4) ? atoi(argv[4]) :  0;        /* raw sensor is already upright */

    RV=(int)(1.402*sat*256+0.5); GU=(int)(0.344*sat*256+0.5);
    GV=(int)(0.714*sat*256+0.5); BU=(int)(1.772*sat*256+0.5);

    signal(SIGTERM,on_stop); signal(SIGINT,on_stop);
    signal(SIGALRM,on_alarm); alarm(30);                /* guards setup (model load is slow) */

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
    v.xoffset=0; v.yoffset=0; v.activate=FB_ACTIVATE_NOW;
    ioctl(fbfd, FBIOPAN_DISPLAY, &v);
    int vsync_ok=1; unsigned vzero=0;
    printf("fb %dx%d %dbpp LL=%d  colour: gray-world AWB, sat=%.2f flip=%d\n",
           FBW,FBH,v.bits_per_pixel,FBLL,sat,flip);

    /* ---- AWNN classifier up (ResNet18 ImageNet-1000) ---- */
    int nl = load_labels(LABELS_PATH);
    printf("labels: %d loaded from %s%s\n", nl, LABELS_PATH, nl<NCLASS?"  (WARN: <1000)":"");
    if (!dlopen(NN_SO, RTLD_NOW|RTLD_GLOBAL)){ fprintf(stderr,"dlopen %s: %s\n",NN_SO,dlerror()); return 2; }
    #define SYM(v,name) do{ *(void**)(&v)=aw_dlsym(RTLD_DEFAULT,name); \
        if(!v){ fprintf(stderr,"dlsym %s failed\n",name); return 2; } }while(0)
    SYM(nn_module_init,"libmaix_nn_module_init");
    SYM(nn_create,"libmaix_nn_create");
    SYM(nn_destroy,"libmaix_nn_destroy");
    #undef SYM
    if (nn_module_init()!=LIBMAIX_ERR_NONE){ fprintf(stderr,"nn module_init failed\n"); return 2; }
    g_nn = nn_create();
    if (!g_nn || g_nn->init(g_nn)!=LIBMAIX_ERR_NONE){ fprintf(stderr,"nn create/init failed\n"); return 2; }
    {
        char *in_names[]={(char*)"input0"}, *out_names[]={(char*)"output0"};
        libmaix_nn_model_path_t mp; memset(&mp,0,sizeof mp);
        mp.awnn.param_path=(char*)MODEL_PARAM; mp.awnn.bin_path=(char*)MODEL_BIN;
        libmaix_nn_opt_param_t opt; memset(&opt,0,sizeof opt);
        opt.awnn.input_names=in_names; opt.awnn.output_names=out_names;
        opt.awnn.input_num=1; opt.awnn.output_num=1;
        opt.awnn.mean[0]=opt.awnn.mean[1]=opt.awnn.mean[2]=127.5f;
        opt.awnn.norm[0]=opt.awnn.norm[1]=opt.awnn.norm[2]=0.0078125f;
        opt.awnn.encrypt=false;
        if (g_nn->load(g_nn,&mp,&opt)!=LIBMAIX_ERR_NONE){ fprintf(stderr,"nn load failed\n"); return 2; }
    }
    printf("AWNN ResNet18-1000 loaded (input0 %dx%dx3 -> output0 %d)\n", IN_W, IN_H, NCLASS);

    if (resolve_mpp() < 0) return 2;

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

    /* centre-crop the WxH capture down to the FBWxFBH panel */
    int xoff; xoff = (W-FBW)/2; if (xoff<0) xoff=0;
    int yoff; yoff = (H-FBH)/2; if (yoff<0) yoff=0;

    pthread_t it; pthread_create(&it,NULL,infer_thread,NULL);

    alarm(0);
    printf("LIVE: camera on panel + AWNN ResNet18 overlay (%dx%d). SIGTERM to stop.\n", W, H);

    {
    unsigned char rgb[IN_W*IN_H*3];
    unsigned long frames=0;
    for (int k=0; (nframes<=0 || k<nframes) && !g_stop; k++){
        VIDEO_FRAME_INFO_S fi; memset(&fi,0,sizeof fi);
        alarm(8);
        if (VI_GetFrame(dev,chn,&fi,2000)!=SUCCESS) continue;
        VIDEO_FRAME_S *fr=&fi.VFrame;
        uint8_t *Y=(uint8_t*)fr->mpVirAddr[0], *VU=(uint8_t*)fr->mpVirAddr[1];
        if (!Y||!VU){ VI_ReleaseFrame(dev,chn,&fi); continue; }

        unsigned long s=0,c=0; for (unsigned i=0;i<(unsigned)W*H;i+=16){ s+=Y[i]; c++; }
        unsigned long ay = c?s/c:0;
        if (ay<20 || ay>240){ VI_ReleaseFrame(dev,chn,&fi); continue; }

        update_awb(Y, VU, fr->mWidth, fr->mHeight);

        /* hand the latest frame to the inference worker (non-blocking; latest wins) */
        nv21_to_rgb_in(Y, VU, fr->mWidth, fr->mHeight, rgb);
        if (pthread_mutex_trylock(&g_lock)==0){
            memcpy(g_in, rgb, sizeof rgb); g_in_ready=1;
            pthread_cond_signal(&g_cond);
            pthread_mutex_unlock(&g_lock);
        }

        /* render the live frame to the panel (gray-world balanced), gated on vblank */
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
                int cv = cl[ci]   - 128;
                int cu = cl[ci+1] - 128;
                int r = (yv + ((cv*RV)>>SH)) * gR >> 8;
                int g = (yv - ((cu*GU + cv*GV)>>SH)) * gG >> 8;
                int b = (yv + ((cu*BU)>>SH)) * gB >> 8;
                uint8_t *p = row + (size_t)(flip?(FBW-1-dx):dx)*FBBpp;
                p[FBrO]=CLAMP8(r); p[FBgO]=CLAMP8(g); p[FBbO]=CLAMP8(b);
            }
        }

        /* overlay the latest prediction (produced by the worker) */
        char line[96]; unsigned long ninf;
        pthread_mutex_lock(&g_lock); strncpy(line,g_label,sizeof line-1); line[sizeof line-1]=0; ninf=g_infers; pthread_mutex_unlock(&g_lock);
        fb_text(4, 4, line, 2, 0,255,0);

        VI_ReleaseFrame(dev,chn,&fi);
        if (frames==0) ioctl(fbfd,FBIOBLANK,FB_BLANK_UNBLANK);

        if (k%15==0)
            printf("frame %3lu  avgY=%3lu  awb[R%.2f G%.2f B%.2f]  infers=%lu  -> %s\n",
                   frames, ay, gR/256.0, gG/256.0, gB/256.0, ninf, line);
        frames++; rc=0;
    }
    alarm(30);
    printf("stopping after %lu frames, %lu classifications (vsync %s)\n",
           frames, g_infers, vsync_ok?"on":"UNSUPPORTED");
    }

    /* wake + join the inference worker */
    pthread_mutex_lock(&g_lock); pthread_cond_broadcast(&g_cond); pthread_mutex_unlock(&g_lock);
    pthread_join(it,NULL);

out:
    if (vch)  { VI_DisableVirChn(dev,chn); VI_DestoryVirChn(dev,chn); }
    if (vipp) { VI_DisableVipp(dev); VI_DestoryVipp(dev); }
    ISP_Stop(g_isp); pthread_join(t,NULL); ISP_Exit();
    SYS_Exit();
    ioctl(fbfd,FBIOBLANK,FB_BLANK_UNBLANK);
    munmap(FB,maplen); close(fbfd);
    if (g_nn) nn_destroy(&g_nn);
    printf(rc==0 ? "OK\n" : "FAILED\n");
    return rc;
}
