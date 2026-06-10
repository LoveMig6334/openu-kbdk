// kbrun.cpp — kbdk model-pack runner for the V831 (KidBright µAI).
//
// Two modes, one binary:
//   kbrun PACK_DIR --image RAW.rgb            one-shot classification of a raw
//                                             RGB888 file at the manifest's WxH
//   kbrun PACK_DIR [WxH] [nframes] [sat] [flip]
//                                             live camera -> ncnn classify ->
//                                             fb0 preview + label overlay
//
// The pack is a directory with manifest.json + ncnn .param/.bin + labels.txt
// (see crates/kbdk-core/src/pack.rs). Inference is vanilla ncnn, statically
// linked (the board's AWNN cannot run vanilla ncnn models — proven by nnload.c);
// the camera is Allwinner MPP, dlopen'd at runtime exactly like src/nnacam.cpp,
// whose preview/AWB/worker-thread structure this transplants.
//
// JSON-lines on stdout: {"event":"result","ms":...,"top":[{label,index,conf}...]}
// per inference, so the host streams results over exec/adb.
//
// Build: make kbrun   (needs board/ncnn/build.sh run once)
// Run:   LD_LIBRARY_PATH=/usr/lib/eyesee-mpp:/usr/lib /tmp/kbrun PACK [320x240] [0]
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
#include <sys/socket.h>
#include <netinet/in.h>
#include <poll.h>

#include <algorithm>
#include <string>
#include <vector>

#include "net.h" // ncnn (static)
#include "manifest.h"

/* board musl 1.1.16 has plain dlsym, not the cross toolchain's __dlsym_time64 */
extern void *aw_dlsym(void *handle, const char *symbol) __asm__("dlsym");

/* musl-1.2 cross headers redirect fstat -> __fstat_time64, which the board's musl
 * 1.1.16 lacks (load would fail: musl binds eagerly). ncnn's file mapping
 * (platform.h ScopedFile) only consumes st_size, and musl 1.2's arm struct stat
 * is the 1.1 layout with 64-bit timespecs appended at the end — same st_size
 * offset — so zero-fill + the plain legacy fstat satisfies the caller. */
#include <sys/stat.h>
extern "C" int kb_fstat_legacy(int fd, struct stat *st) __asm__("fstat");
extern "C" int __fstat_time64(int fd, struct stat *st){
    memset(st, 0, sizeof *st);
    return kb_fstat_legacy(fd, st);
}

/* static libstdc++ references these; the board's musl 1.1.16 predates them */
extern "C" char *secure_getenv(const char *name){ return getenv(name); }
extern "C" int getentropy(void *buf, size_t len){
    FILE *f = fopen("/dev/urandom", "rb");
    if(!f) return -1;
    size_t n = fread(buf, 1, len, f);
    fclose(f);
    return n == len ? 0 : -1;
}

#include "media/mm_common.h"
#include "media/mm_comm_sys.h"
#include "media/mm_comm_video.h"
#include "media/mm_comm_vi.h"

/* AWNN engine (the board's vendor runtime) for the stock /home/model/* packs —
 * vanilla ncnn can't run those (proprietary quantize keys) and AWNN can't run
 * ours; the manifest's "runtime" field picks the engine. dlopen'd lazily so
 * ncnn packs need none of the vendor stack. Same wiring as src/nnacam.cpp. */
extern "C" {
#include "libmaix_nn.h"
/* libmaix_nn.so has undefined back-refs to retinaface decoder hooks (normally in
 * the python ext); musl binds immediately, so export stubs (-Wl,--export-dynamic). */
void retinaface_get_priorboxes(void){}
void retinaface_decode(void){}
}
#define NN_SO "/usr/lib/python3.8/site-packages/maix/libmaix_nn.so"

/* ---- wall clock without any time64-redirected libc call (musl 1.1.16 board) -- */
static double now_ms(void){
    FILE *f = fopen("/proc/uptime", "r");
    double t = 0;
    if(f){ if(fscanf(f, "%lf", &t) != 1) t = 0; fclose(f); }
    return t * 1000.0;
}

/* ---- AW_MPI entry points resolved at runtime (same set as nnacam.cpp) -------- */
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

#define CLAMP8(x) ((x)<0?0:((x)>255?255:(x)))
#ifndef FBIO_WAITFORVSYNC
#define FBIO_WAITFORVSYNC _IOW('F', 0x20, unsigned int)
#endif

static int g_isp = 0;
static volatile sig_atomic_t g_stop = 0;
static void *isp_thread(void *arg){ (void)arg; ISP_Run(g_isp); return NULL; }
static void on_stop(int s){ (void)s; g_stop = 1; }
static void on_alarm(int s){ (void)s; const char m[]="\n[watchdog] timed out\n"; write(2,m,sizeof m-1); _exit(4); }

/* ---- tiny 5x7-in-8x8 bitmap font (same table as nnacam.cpp) ----------------- */
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

/* gray-world AUTO white balance — per-channel gains <<8, EMA-smoothed (camcc note:
 * fixed U/V offsets can't neutralise this ISP's green cast). */
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

/* centre-square downscale of an NV21 frame to in_w x in_h interleaved RGB, balanced */
static void nv21_to_rgb_in(const uint8_t *Y, const uint8_t *VU, int W, int H,
                           unsigned char *rgb, int in_w, int in_h){
    int side = (W < H) ? W : H;
    int xoff = (W - side)/2, yoff = (H - side)/2;
    for (int oy=0; oy<in_h; oy++){
        int sy = yoff + oy*side/in_h;
        const uint8_t *yl = Y  + (size_t)sy*W;
        const uint8_t *cl = VU + (size_t)(sy>>1)*W;
        for (int ox=0; ox<in_w; ox++){
            int sx = xoff + ox*side/in_w;
            int yv = yl[sx]; int ci = (sx>>1)<<1;
            int cv = cl[ci]   - 128;
            int cu = cl[ci+1] - 128;
            int r = (yv + ((cv*RV)>>SH)) * gR >> 8;
            int g = (yv - ((cu*GU + cv*GV)>>SH)) * gG >> 8;
            int b = (yv + ((cu*BU)>>SH)) * gB >> 8;
            unsigned char *p = rgb + ((size_t)oy*in_w + ox)*3;
            p[0]=CLAMP8(r); p[1]=CLAMP8(g); p[2]=CLAMP8(b);
        }
    }
}

/* ---- shared classifier state -------------------------------------------------- */
static KbManifest g_m;
static ncnn::Net g_net;
static bool g_awnn_engine = false;
static bool g_nvdla_engine = false;
static libmaix_nn_t *g_awnn = NULL;
static unsigned char *g_awnn_qbuf = NULL;
static float *g_awnn_out = NULL;
static int g_nclass = 0;

/* manifest model paths may be absolute (stock board models) or pack-relative */
static std::string model_path(const std::string& dir, const std::string& p){
    return p.size() && p[0] == '/' ? p : dir + "/" + p;
}

static bool awnn_init(const std::string& dir){
    typedef libmaix_err_t (*fn_module_init)(void);
    typedef libmaix_nn_t* (*fn_create)(void);
    if(!dlopen(NN_SO, RTLD_NOW|RTLD_GLOBAL)){
        fprintf(stderr, "dlopen %s: %s\n", NN_SO, dlerror()); return false;
    }
    fn_module_init nn_module_init; fn_create nn_create;
    #define SYM(v,name) do{ *(void**)(&v)=aw_dlsym(RTLD_DEFAULT,name); \
        if(!v){ fprintf(stderr,"dlsym %s failed\n",name); return false; } }while(0)
    SYM(nn_module_init, "libmaix_nn_module_init");
    SYM(nn_create,      "libmaix_nn_create");
    #undef SYM
    if(nn_module_init() != LIBMAIX_ERR_NONE) return false;
    g_awnn = nn_create();
    if(!g_awnn || g_awnn->init(g_awnn) != LIBMAIX_ERR_NONE) return false;

    static std::string param = model_path(dir, g_m.param);
    static std::string bin   = model_path(dir, g_m.bin);
    static char *in_names[2]  = { (char*)g_m.in_blob.c_str(),  NULL };
    static char *out_names[2] = { (char*)g_m.out_blob.c_str(), NULL };
    libmaix_nn_model_path_t mp; memset(&mp, 0, sizeof mp);
    mp.awnn.param_path = (char*)param.c_str();
    mp.awnn.bin_path   = (char*)bin.c_str();
    libmaix_nn_opt_param_t opt; memset(&opt, 0, sizeof opt);
    opt.awnn.input_names = in_names; opt.awnn.output_names = out_names;
    opt.awnn.input_num = 1; opt.awnn.output_num = 1;
    for(int i = 0; i < 3; i++){ opt.awnn.mean[i] = g_m.mean[i]; }
    /* AWNN's norm is the raw scale; the manifest's norm already is too */
    for(int i = 0; i < 3; i++){ opt.awnn.norm[i] = g_m.norm[i]; }
    opt.awnn.encrypt = false;
    if(g_awnn->load(g_awnn, &mp, &opt) != LIBMAIX_ERR_NONE){
        fprintf(stderr, "awnn load failed (%s)\n", param.c_str()); return false;
    }
    g_awnn_qbuf = (unsigned char*)malloc((size_t)g_m.w * g_m.h * 3);
    g_awnn_out  = (float*)malloc(sizeof(float) * g_nclass);
    return true;
}

/* ---- NVDLA engine: the GPL nna_runner as a child process ---------------------- */
/* kbrun stays MIT; everything that links third_party/v831-npu lives in the
 * separate /tmp/nna_runner executable (GPLv3). The two talk only through a
 * pipe pair ("go\n" per frame / one JSON line back) and two tmpfs files
 * (packed int8 input cube in, raw logit cube out). The child keeps the NPU
 * mapped and the weights resident, so per-frame cost is just input+forward. */
#define NV_RUNNER "/tmp/nna_runner"
#define NV_IN  "/tmp/kbrun_nv_in.bin"
#define NV_OUT "/tmp/kbrun_nv_out.bin"
static FILE *g_nv_to = NULL, *g_nv_from = NULL;
static unsigned char *g_nv_inbuf = NULL;   /* 8ch-padded feature cube, w*h*8 */

static bool nvdla_init(const std::string& dir){
    std::string job = model_path(dir, g_m.param);
    if((size_t)g_m.nv_in_size != (size_t)g_m.w * g_m.h * 8){
        fprintf(stderr, "nvdla: nv_in_size %d != w*h*8\n", g_m.nv_in_size);
        return false;
    }
    int to[2], from[2];
    if(pipe(to) || pipe(from)) return false;
    pid_t p = fork();
    if(p < 0) return false;
    if(p == 0){
        dup2(to[0], 0); dup2(from[1], 1);
        close(to[0]); close(to[1]); close(from[0]); close(from[1]);
        execl(NV_RUNNER, NV_RUNNER, job.c_str(), NV_IN, NV_OUT, "serve", (char*)NULL);
        _exit(127);
    }
    close(to[0]); close(from[1]);
    g_nv_to = fdopen(to[1], "w");
    g_nv_from = fdopen(from[0], "r");
    if(!g_nv_to || !g_nv_from) return false;
    setvbuf(g_nv_to, NULL, _IONBF, 0);
    char line[256];
    while(fgets(line, sizeof line, g_nv_from)){
        if(strstr(line, "\"loaded\"")){
            g_nv_inbuf = (unsigned char*)calloc((size_t)g_m.w * g_m.h, 8);
            return g_nv_inbuf != NULL;
        }
        if(strstr(line, "\"error\"")){ fprintf(stderr, "nna_runner: %s", line); return false; }
    }
    fprintf(stderr, "nna_runner exited during init (pushed? chmod +x?)\n");
    return false;
}

/* one forward on interleaved RGB via the NPU child; fills dequantized logits */
static bool nvdla_forward(const unsigned char *rgb, std::vector<float>& logits){
    size_t npix = (size_t)g_m.w * g_m.h;
    for(size_t i = 0; i < npix; i++){
        unsigned char *d = g_nv_inbuf + i * 8;   /* ch 3..7 stay zero (calloc) */
        d[0] = (unsigned char)((int)rgb[i*3+0] - 128);
        d[1] = (unsigned char)((int)rgb[i*3+1] - 128);
        d[2] = (unsigned char)((int)rgb[i*3+2] - 128);
    }
    FILE *f = fopen(NV_IN, "wb");
    if(!f) return false;
    fwrite(g_nv_inbuf, 1, npix * 8, f);
    fclose(f);
    fputs("go\n", g_nv_to);
    char line[256];
    while(fgets(line, sizeof line, g_nv_from)){
        if(strstr(line, "\"infer\"")) break;
        if(strstr(line, "\"error\"")) return false;
    }
    if(feof(g_nv_from)) return false;            /* child died */
    f = fopen(NV_OUT, "rb");
    if(!f) return false;
    /* logits are 1x1 spatial: channel c sits at byte c (8-padded surfaces are
     * contiguous when w==h==1) */
    signed char q[256];
    int n = (int)fread(q, 1, sizeof q, f);
    fclose(f);
    if(n < g_m.nv_out_c) return false;
    logits.resize(g_m.nv_out_c);
    for(int i = 0; i < g_m.nv_out_c; i++) logits[i] = (float)q[i] / g_m.logit_scale;
    return true;
}

static void softmax_in_place(std::vector<float>& v){
    float mx = *std::max_element(v.begin(), v.end()); float sum = 0;
    for(auto& x : v){ x = expf(x - mx); sum += x; }
    for(auto& x : v) x /= sum;
}

/* run one forward on interleaved RGB (in_w*in_h*3), return softmax probs */
static bool classify(const unsigned char *rgb, std::vector<float>& probs, double *ms){
    double t0 = now_ms();
    if(g_nvdla_engine){
        if(!nvdla_forward(rgb, probs)) return false;
        if(ms) *ms = now_ms() - t0;
        softmax_in_place(probs);
        return true;
    }
    if(g_awnn_engine){
        libmaix_nn_layer_t in; memset(&in, 0, sizeof in);
        in.w = g_m.w; in.h = g_m.h; in.c = 3;
        in.dtype = LIBMAIX_NN_DTYPE_UINT8; in.layout = LIBMAIX_NN_LAYOUT_HWC;
        in.need_quantization = true;
        in.data = (void*)rgb; in.buff_quantization = g_awnn_qbuf;
        libmaix_nn_layer_t o; memset(&o, 0, sizeof o);
        o.w = 1; o.h = 1; o.c = g_nclass;
        o.dtype = LIBMAIX_NN_DTYPE_FLOAT; o.layout = LIBMAIX_NN_LAYOUT_HWC;
        o.data = g_awnn_out;
        if(g_awnn->forward(g_awnn, &in, &o) != LIBMAIX_ERR_NONE) return false;
        if(ms) *ms = now_ms() - t0;
        probs.assign(g_awnn_out, g_awnn_out + g_nclass);
        softmax_in_place(probs);
        return true;
    }
    ncnn::Mat in = ncnn::Mat::from_pixels(rgb, ncnn::Mat::PIXEL_RGB, g_m.w, g_m.h);
    in.substract_mean_normalize(g_m.mean, g_m.norm);
    ncnn::Mat out;
    ncnn::Extractor ex = g_net.create_extractor();
    ex.input(g_m.in_blob.c_str(), in);
    if(ex.extract(g_m.out_blob.c_str(), out)) return false;
    if(ms) *ms = now_ms() - t0;
    probs.resize(out.c > 1 ? out.c : out.w);
    if(out.c > 1) for(int i = 0; i < out.c; i++) probs[i] = out.channel(i)[0];
    else for(int i = 0; i < out.w; i++) probs[i] = ((const float*)out)[i];
    softmax_in_place(probs);
    return true;
}

static void print_result_json(const std::vector<float>& v, double ms){
    std::vector<int> idx(v.size());
    for(size_t i = 0; i < idx.size(); i++) idx[i] = (int)i;
    size_t k5 = std::min<size_t>(5, idx.size());
    std::partial_sort(idx.begin(), idx.begin() + k5, idx.end(),
                      [&](int a, int b){ return v[a] > v[b]; });
    printf("{\"event\":\"result\",\"ms\":%.1f,\"top\":[", ms);
    for(size_t k = 0; k < k5; k++){
        int i = idx[k];
        printf("%s{\"label\":\"%s\",\"index\":%d,\"conf\":%.4f}", k ? "," : "",
               i < (int)g_m.labels.size() ? g_m.labels[i].c_str() : "?", i, v[i]);
    }
    printf("]}\n");
    fflush(stdout);
}

/* ---- detection (YOLOv2 head decode + NMS, mirrors kbdk_train.detect) ---------- */
struct Det { int cls; float conf, cx, cy, w, h; };

static inline float sigmoidf_(float x){ return 1.0f / (1.0f + expf(-x)); }

static float det_iou(const Det& a, const Det& b){
    float x1a=a.cx-a.w/2, y1a=a.cy-a.h/2, x2a=a.cx+a.w/2, y2a=a.cy+a.h/2;
    float x1b=b.cx-b.w/2, y1b=b.cy-b.h/2, x2b=b.cx+b.w/2, y2b=b.cy+b.h/2;
    float iw = std::min(x2a,x2b) - std::max(x1a,x1b);
    float ih = std::min(y2a,y2b) - std::max(y1a,y1b);
    if(iw <= 0 || ih <= 0) return 0;
    float inter = iw*ih, uni = a.w*a.h + b.w*b.h - inter;
    return uni > 0 ? inter/uni : 0;
}

/* out: ncnn Mat (c=A*(5+C), h=S, w=S) -> NMS'd detections, normalized coords */
static void decode_dets(const ncnn::Mat& out, std::vector<Det>& dets){
    int A = (int)g_m.anchors.size() / 2, S = g_m.grid;
    int C = g_nclass;
    std::vector<Det> cand;
    std::vector<float> cl(C);
    for(int a = 0; a < A; a++){
        float aw = g_m.anchors[2*a], ah = g_m.anchors[2*a+1];
        for(int gi = 0; gi < S; gi++){
            for(int gj = 0; gj < S; gj++){
                float obj = sigmoidf_(out.channel(a*(5+C)+4).row(gi)[gj]);
                if(obj < g_m.conf_threshold * 0.5f) continue;   /* cheap pre-gate */
                float mx = -1e30f;
                for(int c = 0; c < C; c++){ cl[c] = out.channel(a*(5+C)+5+c).row(gi)[gj]; mx = std::max(mx, cl[c]); }
                float sum = 0;
                for(int c = 0; c < C; c++){ cl[c] = expf(cl[c]-mx); sum += cl[c]; }
                int best = 0;
                for(int c = 1; c < C; c++) if(cl[c] > cl[best]) best = c;
                float conf = obj * cl[best] / sum;
                if(conf < g_m.conf_threshold) continue;
                Det d;
                d.cls = best; d.conf = conf;
                d.cx = (gj + sigmoidf_(out.channel(a*(5+C)+0).row(gi)[gj])) / S;
                d.cy = (gi + sigmoidf_(out.channel(a*(5+C)+1).row(gi)[gj])) / S;
                d.w  = aw * expf(std::min(out.channel(a*(5+C)+2).row(gi)[gj], 8.0f)) / S;
                d.h  = ah * expf(std::min(out.channel(a*(5+C)+3).row(gi)[gj], 8.0f)) / S;
                cand.push_back(d);
            }
        }
    }
    std::sort(cand.begin(), cand.end(), [](const Det&a, const Det&b){ return a.conf > b.conf; });
    dets.clear();
    for(const Det& d : cand){
        bool keep = true;
        for(const Det& k : dets)
            if(k.cls == d.cls && det_iou(d, k) >= g_m.nms_threshold){ keep = false; break; }
        if(keep) dets.push_back(d);
        if(dets.size() >= 16) break;
    }
}

/* forward + decode on interleaved RGB (ncnn engine only) */
static bool detect_forward(const unsigned char *rgb, std::vector<Det>& dets, double *ms){
    double t0 = now_ms();
    ncnn::Mat in = ncnn::Mat::from_pixels(rgb, ncnn::Mat::PIXEL_RGB, g_m.w, g_m.h);
    in.substract_mean_normalize(g_m.mean, g_m.norm);
    ncnn::Mat out;
    ncnn::Extractor ex = g_net.create_extractor();
    ex.input(g_m.in_blob.c_str(), in);
    if(ex.extract(g_m.out_blob.c_str(), out)) return false;
    if(ms) *ms = now_ms() - t0;
    decode_dets(out, dets);
    return true;
}

static void print_dets_json(const std::vector<Det>& dets, double ms){
    printf("{\"event\":\"result\",\"ms\":%.1f,\"boxes\":[", ms);
    for(size_t i = 0; i < dets.size(); i++){
        const Det& d = dets[i];
        printf("%s{\"label\":\"%s\",\"index\":%d,\"conf\":%.3f,"
               "\"x\":%.4f,\"y\":%.4f,\"w\":%.4f,\"h\":%.4f}",
               i ? "," : "",
               d.cls < (int)g_m.labels.size() ? g_m.labels[d.cls].c_str() : "?",
               d.cls, d.conf, d.cx - d.w/2, d.cy - d.h/2, d.w, d.h);
    }
    printf("]}\n");
    fflush(stdout);
}

/* per-class overlay palette (RGB) */
static const int DET_COL[6][3] = {
    {166,227,161}, {243,139,168}, {137,180,250}, {249,226,175}, {203,166,247}, {148,226,213},
};

static void fb_box_outline(float nx, float ny, float nw, float nh, const int col[3]){
    int x = (int)(nx * FBW), y = (int)(ny * FBH);
    int w = (int)(nw * FBW), h = (int)(nh * FBH);
    const int t = 2;
    fb_rect(x, y, w, t, col[0], col[1], col[2]);
    fb_rect(x, y + h - t, w, t, col[0], col[1], col[2]);
    fb_rect(x, y, t, h, col[0], col[1], col[2]);
    fb_rect(x + w - t, y, t, h, col[0], col[1], col[2]);
}

/* ---- inference worker (latest-wins handoff, same scheme as nnacam.cpp) -------- */
static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t  g_cond = PTHREAD_COND_INITIALIZER;
static unsigned char *g_in = NULL;            /* m.w*m.h*3, latest balanced frame */
static int  g_in_ready = 0;
static char g_label[96] = "...";
static unsigned long g_infers = 0;
static std::vector<Det> g_dets;               /* latest detections (under g_lock) */

static void *infer_thread(void *arg){
    (void)arg;
    size_t insz = (size_t)g_m.w * g_m.h * 3;
    unsigned char *local = (unsigned char*)malloc(insz);
    std::vector<float> probs;
    while (!g_stop){
        pthread_mutex_lock(&g_lock);
        while (!g_in_ready && !g_stop) pthread_cond_wait(&g_cond,&g_lock);
        if (g_stop){ pthread_mutex_unlock(&g_lock); break; }
        memcpy(local, g_in, insz); g_in_ready = 0;
        pthread_mutex_unlock(&g_lock);

        double ms = 0;
        char buf[96];
        std::vector<Det> dets;
        if(g_m.task == "detection"){
            if(!detect_forward(local, dets, &ms)) continue;
            if(dets.empty())
                snprintf(buf, sizeof buf, "no obj");
            else
                snprintf(buf, sizeof buf, "%s %d%%%s",
                         dets[0].cls < (int)g_m.labels.size() ? g_m.labels[dets[0].cls].c_str() : "?",
                         (int)(dets[0].conf * 100.0f + 0.5f),
                         dets.size() > 1 ? " +" : "");
            print_dets_json(dets, ms);
        } else {
            if(!classify(local, probs, &ms)) continue;
            int best = (int)(std::max_element(probs.begin(), probs.end()) - probs.begin());
            int pct = (int)(probs[best] * 100.0f + 0.5f);
            const char *nm = best < (int)g_m.labels.size() ? g_m.labels[best].c_str() : "?";
            snprintf(buf,sizeof buf,"%s %d%%", nm, pct);
            print_result_json(probs, ms);
        }

        pthread_mutex_lock(&g_lock);
        strncpy(g_label, buf, sizeof g_label-1); g_label[sizeof g_label-1]=0;
        g_dets = dets;
        g_infers++;
        pthread_mutex_unlock(&g_lock);
    }
    free(local);
    return NULL;
}

/* ---- preview frame export (host UI shows the camera) -------------------------- */
/* The balanced 240x240 RGB preview goes two ways: into an in-memory buffer the
 * TCP frame server (below) serves over adb-forward (the fast path, ~10-15 fps),
 * and every few frames into a tmpfs file the host can adb-pull (the fallback for
 * hosts that predate the server). The write-then-rename keeps pulls from seeing
 * a half-written frame. */
#define PREV_W 240
#define PREV_H 240
#define PREV_PATH "/tmp/kbrun_frame.rgb"
#define PREV_TMP  "/tmp/.kbrun_frame.tmp"
#define FRAME_PORT 18902

static pthread_mutex_t g_prev_lock = PTHREAD_MUTEX_INITIALIZER;
static unsigned char g_prev[PREV_W * PREV_H * 3];   /* latest preview, under g_prev_lock */
static unsigned long g_prev_seq = 0;                /* 0 = no frame yet */

static void write_preview(const uint8_t *Y, const uint8_t *VU, int W, int H, int to_file){
    static unsigned char buf[PREV_W * PREV_H * 3];
    nv21_to_rgb_in(Y, VU, W, H, buf, PREV_W, PREV_H);
    pthread_mutex_lock(&g_prev_lock);
    memcpy(g_prev, buf, sizeof buf);
    g_prev_seq++;
    pthread_mutex_unlock(&g_prev_lock);
    if(!to_file) return;
    FILE *f = fopen(PREV_TMP, "wb");
    if(!f) return;
    unsigned char hdr[8] = {'K','B','F','1',
        PREV_W & 0xFF, PREV_W >> 8, PREV_H & 0xFF, PREV_H >> 8};
    fwrite(hdr, 1, sizeof hdr, f);
    fwrite(buf, 1, sizeof buf, f);
    fclose(f);
    rename(PREV_TMP, PREV_PATH);
}

/* TCP frame server: one-shot per connection — wait (briefly) for a preview frame
 * newer than the last one served, send "KBF1" + dims + RGB, close. The wait is
 * what paces a tight host fetch loop at the camera's preview cadence. No
 * pthread_cond_timedwait here: the musl-1.2 cross headers redirect it to
 * __pthread_cond_timedwait_time64, which the board's musl 1.1.16 lacks — a
 * usleep poll avoids the trap (same reason the accept gate is poll(), not
 * select()). */
static void *frame_server(void *arg){
    (void)arg;
    int ls = socket(AF_INET, SOCK_STREAM, 0);
    if(ls < 0) return NULL;
    int one = 1;
    setsockopt(ls, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    struct sockaddr_in a; memset(&a, 0, sizeof a);
    a.sin_family = AF_INET;
    a.sin_port = htons(FRAME_PORT);
    a.sin_addr.s_addr = htonl(INADDR_ANY);
    if(bind(ls, (struct sockaddr*)&a, sizeof a) < 0 || listen(ls, 2) < 0){
        fprintf(stderr, "frame server: bind/listen :%d failed\n", FRAME_PORT);
        close(ls);
        return NULL;
    }
    static unsigned char out[8 + sizeof g_prev];
    memcpy(out, "KBF1", 4);
    out[4] = PREV_W & 0xFF; out[5] = PREV_W >> 8;
    out[6] = PREV_H & 0xFF; out[7] = PREV_H >> 8;
    unsigned long sent_seq = 0;
    while(!g_stop){
        struct pollfd pf = { ls, POLLIN, 0 };
        if(poll(&pf, 1, 500) <= 0) continue;
        int c = accept(ls, NULL, NULL);
        if(c < 0) continue;
        for(int waited = 0; waited < 250 && !g_stop; waited += 10){
            pthread_mutex_lock(&g_prev_lock);
            unsigned long s = g_prev_seq;
            pthread_mutex_unlock(&g_prev_lock);
            if(s && s != sent_seq) break;
            usleep(10000);
        }
        pthread_mutex_lock(&g_prev_lock);
        unsigned long s = g_prev_seq;
        if(s) memcpy(out + 8, g_prev, sizeof g_prev);
        pthread_mutex_unlock(&g_prev_lock);
        if(s){
            sent_seq = s;
            size_t off = 0;
            while(off < sizeof out){
                ssize_t n = send(c, out + off, sizeof out - off, MSG_NOSIGNAL);
                if(n <= 0) break;
                off += (size_t)n;
            }
        }
        close(c);
    }
    close(ls);
    return NULL;
}

/* ---- one-shot file mode (host parity verification) ---------------------------- */
static int run_image_mode(const std::string& dir, const char *img_path){
    size_t need = (size_t)g_m.w * g_m.h * 3;
    std::vector<unsigned char> rgb(need);
    FILE* f = fopen(img_path, "rb");
    if(!f || fread(rgb.data(), 1, need, f) != need){
        printf("{\"event\":\"error\",\"msg\":\"image read failed (want %zu bytes)\"}\n", need);
        return 1;
    }
    fclose(f);
    double ms = 0;
    (void)dir;
    if(g_m.task == "detection"){
        std::vector<Det> dets;
        if(!detect_forward(rgb.data(), dets, &ms)){
            printf("{\"event\":\"error\",\"msg\":\"extract failed\"}\n"); return 1;
        }
        print_dets_json(dets, ms);
        fflush(NULL);
        _exit(0);
    }
    std::vector<float> probs;
    if(!classify(rgb.data(), probs, &ms)){
        printf("{\"event\":\"error\",\"msg\":\"extract failed\"}\n"); return 1;
    }
    print_result_json(probs, ms);
    /* skip libc atexit teardown: AWNN's exit handlers segfault in ion frees
     * (phy2vir spam) once a model was loaded; our output is already flushed */
    fflush(NULL);
    _exit(0);
}

int main(int argc, char **argv){
    setvbuf(stdout, NULL, _IONBF, 0);

    /* KBRUN_DAEMON=1: detach into a new session FIRST (before any heavy work).
     * adbd kills the session's whole process group when its shell closes (this
     * BusyBox has no setsid/nohup), so backgrounding from the shell can't
     * survive. The pipe makes the parent wait until the child has setsid()'d —
     * without it the group kill races the child between fork and setsid. */
    if(getenv("KBRUN_DAEMON")){
        int pfd[2];
        if(pipe(pfd) == 0){
            pid_t p = fork();
            if(p > 0){ char c; (void)!read(pfd[0], &c, 1); _exit(0); }
            setsid();
            (void)!write(pfd[1], "x", 1);
            close(pfd[0]); close(pfd[1]);
            FILE *pf = fopen("/tmp/kbrun.pid", "w");
            if(pf){ fprintf(pf, "%d\n", getpid()); fclose(pf); }
        }
    }

    if(argc < 2){
        fprintf(stderr, "usage: %s PACK_DIR [--image RAW.rgb | [WxH] [nframes] [sat] [flip]]\n", argv[0]);
        return 2;
    }
    std::string dir = argv[1];
    if(!mf_load((dir + "/manifest.json").c_str(), g_m)){
        printf("{\"event\":\"error\",\"msg\":\"manifest load failed\"}\n"); return 1;
    }
    /* labels: inline JSON array, or one-per-line labels_file (1000 ImageNet
     * labels don't belong inline) */
    if(g_m.labels.empty() && !g_m.labels_file.empty()){
        FILE *lf = fopen(model_path(dir, g_m.labels_file).c_str(), "r");
        char line[160];
        while(lf && fgets(line, sizeof line, lf)){
            size_t n = strlen(line);
            while(n && (line[n-1] == '\n' || line[n-1] == '\r')) line[--n] = 0;
            if(n) g_m.labels.push_back(line);
        }
        if(lf) fclose(lf);
    }
    g_nclass = (int)g_m.labels.size();

    g_awnn_engine = (g_m.runtime == "awnn");
    g_nvdla_engine = (g_m.runtime == "nvdla");
    if(g_nvdla_engine){
        if(g_m.task == "detection"){
            printf("{\"event\":\"error\",\"msg\":\"nvdla detection not supported yet\"}\n");
            return 1;
        }
        if(!nvdla_init(dir)){
            printf("{\"event\":\"error\",\"msg\":\"nvdla engine init failed (nna_runner pushed?)\"}\n");
            return 1;
        }
    } else if(g_awnn_engine){
        if(g_nclass == 0){
            printf("{\"event\":\"error\",\"msg\":\"awnn pack needs labels (class count)\"}\n");
            return 1;
        }
        if(!awnn_init(dir)){
            printf("{\"event\":\"error\",\"msg\":\"awnn engine init failed (LD_LIBRARY_PATH?)\"}\n");
            return 1;
        }
    } else {
        g_net.opt.num_threads = 1;
        if(g_net.load_param(model_path(dir, g_m.param).c_str()) ||
           g_net.load_model(model_path(dir, g_m.bin).c_str())){
            printf("{\"event\":\"error\",\"msg\":\"model load failed\"}\n"); return 1;
        }
    }
    printf("{\"event\":\"loaded\",\"name\":\"%s\",\"runtime\":\"%s\",\"input\":\"%dx%d\",\"classes\":%zu}\n",
           g_m.name.c_str(), g_m.runtime.c_str(), g_m.w, g_m.h, g_m.labels.size());

    if(argc >= 4 && std::string(argv[2]) == "--image")
        return run_image_mode(dir, argv[3]);

    /* ---------------- camera mode ---------------- */
    int W=320, H=240, dev=0, chn=0;
    if (argc>2) sscanf(argv[2], "%dx%d", &W, &H);
    int nframes = (argc>3) ? atoi(argv[3]) : 0;         /* 0 = run until SIGTERM */
    double sat  = (argc>4) ? atof(argv[4]) :  1.3;
    int    flip = (argc>5) ? atoi(argv[5]) :  0;

    RV=(int)(1.402*sat*256+0.5); GU=(int)(0.344*sat*256+0.5);
    GV=(int)(0.714*sat*256+0.5); BU=(int)(1.772*sat*256+0.5);

    signal(SIGTERM,on_stop); signal(SIGINT,on_stop);
    signal(SIGHUP,SIG_IGN);                             /* adbd HUPs on session close */
    signal(SIGPIPE,SIG_IGN);                            /* frame client may vanish mid-send */
    signal(SIGALRM,on_alarm); alarm(30);                /* guards setup */

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

    if (resolve_mpp() < 0) return 2;

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
    size_t insz = (size_t)g_m.w * g_m.h * 3;
    unsigned char *rgb = (unsigned char*)malloc(insz);
    g_in = (unsigned char*)malloc(insz);
    pthread_t it, ft;
    int xoff, yoff;
    unsigned long frames=0, skipped=0;

    if (VI_CreateVipp(dev)!=SUCCESS){ fprintf(stderr,"CreateVipp failed\n"); goto out; } vipp=1;
    VI_SetVippAttr(dev,&attr);
    if (VI_EnableVipp(dev)!=SUCCESS){ fprintf(stderr,"EnableVipp failed\n"); goto out; }
    if (VI_CreateVirChn(dev,chn,NULL)!=SUCCESS){ fprintf(stderr,"CreateVirChn failed\n"); goto out; }
    if (VI_EnableVirChn(dev,chn)!=SUCCESS){ fprintf(stderr,"EnableVirChn failed\n"); goto out; } vch=1;

    /* centre-crop the WxH capture down to the FBWxFBH panel */
    xoff = (W-FBW)/2; if (xoff<0) xoff=0;
    yoff = (H-FBH)/2; if (yoff<0) yoff=0;

    pthread_create(&it,NULL,infer_thread,NULL);
    pthread_create(&ft,NULL,frame_server,NULL);   /* after daemonize + camera up */

    alarm(0);
    fprintf(stderr, "LIVE: camera on panel + ncnn '%s' overlay (%dx%d). SIGTERM to stop.\n",
            g_m.name.c_str(), W, H);

    for (int k=0; (nframes<=0 || k<nframes) && !g_stop; k++){
        VIDEO_FRAME_INFO_S fi; memset(&fi,0,sizeof fi);
        alarm(8);
        if (VI_GetFrame(dev,chn,&fi,2000)!=SUCCESS) continue;
        VIDEO_FRAME_S *fr=&fi.VFrame;
        uint8_t *Y=(uint8_t*)fr->mpVirAddr[0], *VU=(uint8_t*)fr->mpVirAddr[1];
        if (!Y||!VU){ VI_ReleaseFrame(dev,chn,&fi); continue; }

        unsigned long s=0,c=0; for (unsigned i=0;i<(unsigned)W*H;i+=16){ s+=Y[i]; c++; }
        unsigned long ay = c?s/c:0;
        /* gate only the sensor's all-black glitch frames (avgY ~ 0); a dim room
         * legitimately sits in the teens and must still classify */
        if (ay<8 || ay>240){
            skipped++;
            if (skipped%30==1) fprintf(stderr, "skip avgY=%lu\n", ay);
            VI_ReleaseFrame(dev,chn,&fi);
            continue;
        }

        update_awb(Y, VU, fr->mWidth, fr->mHeight);

        /* TCP buffer every 3rd frame (~10 fps at the camera's 30 — more starves
         * the inference thread: at 15 fps the adbd transfer alone roughly doubles
         * forward() wall time on the single A7); tmpfs fallback file every 6th
         * (the pull path only samples ~2.5/s anyway) */
        if (k % 3 == 0) write_preview(Y, VU, fr->mWidth, fr->mHeight, (k % 6) == 0);

        /* hand the latest frame to the inference worker (non-blocking; latest wins) */
        nv21_to_rgb_in(Y, VU, fr->mWidth, fr->mHeight, rgb, g_m.w, g_m.h);
        if (pthread_mutex_trylock(&g_lock)==0){
            memcpy(g_in, rgb, insz); g_in_ready=1;
            pthread_cond_signal(&g_cond);
            pthread_mutex_unlock(&g_lock);
        }

        /* render the live frame to the panel, gated on vblank */
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
        {
            char line[96];
            std::vector<Det> dets;
            pthread_mutex_lock(&g_lock);
            strncpy(line,g_label,sizeof line-1); line[sizeof line-1]=0;
            dets = g_dets;
            pthread_mutex_unlock(&g_lock);
            /* detection: bounding boxes — the panel shows the same centre square
             * the net sees, so normalized coords map straight to fb pixels */
            for(size_t di = 0; di < dets.size(); di++){
                const Det& d = dets[di];
                const int *col = DET_COL[d.cls % 6];
                fb_box_outline(d.cx - d.w/2, d.cy - d.h/2, d.w, d.h, col);
                char bl[48];
                snprintf(bl, sizeof bl, "%s %d%%",
                         d.cls < (int)g_m.labels.size() ? g_m.labels[d.cls].c_str() : "?",
                         (int)(d.conf * 100.0f + 0.5f));
                int bx = (int)((d.cx - d.w/2) * FBW) + 3;
                int by = (int)((d.cy - d.h/2) * FBH) + 3;
                fb_text(bx, by, bl, 1, col[0], col[1], col[2]);
            }
            fb_text(4, 4, line, 2, 0,255,0);
        }

        VI_ReleaseFrame(dev,chn,&fi);
        if (frames==0) ioctl(fbfd,FBIOBLANK,FB_BLANK_UNBLANK);
        if (k%30==0)
            fprintf(stderr, "frame %3lu avgY=%3lu awb[%.2f %.2f %.2f] infers=%lu\n",
                    frames, ay, gR/256.0, gG/256.0, gB/256.0, g_infers);
        frames++; rc=0;
    }
    alarm(30);
    fprintf(stderr, "stopping after %lu frames (%lu dark-skipped), %lu classifications (vsync %s)\n",
            frames, skipped, g_infers, vsync_ok?"on":"UNSUPPORTED");

    /* wake + join the inference worker (g_stop makes it exit; the counted-frames
     * path otherwise leaves it blocked on the condvar and join hangs forever) */
    g_stop = 1;
    pthread_mutex_lock(&g_lock); pthread_cond_broadcast(&g_cond); pthread_mutex_unlock(&g_lock);
    pthread_join(it,NULL);
    pthread_join(ft,NULL);   /* poll() gate notices g_stop within 500 ms */

out:
    if (vch)  { VI_DisableVirChn(dev,chn); VI_DestoryVirChn(dev,chn); }
    if (vipp) { VI_DisableVipp(dev); VI_DestoryVipp(dev); }
    ISP_Stop(g_isp); pthread_join(t,NULL); ISP_Exit();
    SYS_Exit();
    ioctl(fbfd,FBIOBLANK,FB_BLANK_UNBLANK);
    munmap(FB,maplen); close(fbfd);
    fprintf(stderr, rc==0 ? "OK\n" : "FAILED\n");
    /* skip libc atexit teardown (see run_image_mode): MPP is already shut down
     * cleanly above; AWNN's exit handlers segfault in ion frees */
    fflush(NULL);
    _exit(rc);
}
