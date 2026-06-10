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

/* run one ncnn forward on interleaved RGB (in_w*in_h*3), return softmax probs */
static bool classify(const unsigned char *rgb, std::vector<float>& probs, double *ms){
    ncnn::Mat in = ncnn::Mat::from_pixels(rgb, ncnn::Mat::PIXEL_RGB, g_m.w, g_m.h);
    in.substract_mean_normalize(g_m.mean, g_m.norm);
    ncnn::Mat out;
    double t0 = now_ms();
    ncnn::Extractor ex = g_net.create_extractor();
    ex.input(g_m.in_blob.c_str(), in);
    if(ex.extract(g_m.out_blob.c_str(), out)) return false;
    if(ms) *ms = now_ms() - t0;
    probs.resize(out.c > 1 ? out.c : out.w);
    if(out.c > 1) for(int i = 0; i < out.c; i++) probs[i] = out.channel(i)[0];
    else for(int i = 0; i < out.w; i++) probs[i] = ((const float*)out)[i];
    float mx = *std::max_element(probs.begin(), probs.end()); float sum = 0;
    for(auto& x : probs){ x = expf(x - mx); sum += x; }
    for(auto& x : probs) x /= sum;
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

/* ---- inference worker (latest-wins handoff, same scheme as nnacam.cpp) -------- */
static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t  g_cond = PTHREAD_COND_INITIALIZER;
static unsigned char *g_in = NULL;            /* m.w*m.h*3, latest balanced frame */
static int  g_in_ready = 0;
static char g_label[96] = "...";
static unsigned long g_infers = 0;

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
        if(!classify(local, probs, &ms)) continue;
        int best = (int)(std::max_element(probs.begin(), probs.end()) - probs.begin());
        int pct = (int)(probs[best] * 100.0f + 0.5f);
        const char *nm = best < (int)g_m.labels.size() ? g_m.labels[best].c_str() : "?";
        char buf[96]; snprintf(buf,sizeof buf,"%s %d%%", nm, pct);
        print_result_json(probs, ms);

        pthread_mutex_lock(&g_lock);
        strncpy(g_label, buf, sizeof g_label-1); g_label[sizeof g_label-1]=0;
        g_infers++;
        pthread_mutex_unlock(&g_lock);
    }
    free(local);
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
    std::vector<float> probs; double ms = 0;
    if(!classify(rgb.data(), probs, &ms)){
        printf("{\"event\":\"error\",\"msg\":\"extract failed\"}\n"); return 1;
    }
    (void)dir;
    print_result_json(probs, ms);
    return 0;
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
    g_net.opt.num_threads = 1;
    if(g_net.load_param((dir + "/" + g_m.param).c_str()) ||
       g_net.load_model((dir + "/" + g_m.bin).c_str())){
        printf("{\"event\":\"error\",\"msg\":\"model load failed\"}\n"); return 1;
    }
    printf("{\"event\":\"loaded\",\"name\":\"%s\",\"input\":\"%dx%d\",\"classes\":%zu}\n",
           g_m.name.c_str(), g_m.w, g_m.h, g_m.labels.size());

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
    pthread_t it;
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
        if (ay<20 || ay>240){
            skipped++;
            if (skipped%30==1) fprintf(stderr, "skip avgY=%lu\n", ay);
            VI_ReleaseFrame(dev,chn,&fi);
            continue;
        }

        update_awb(Y, VU, fr->mWidth, fr->mHeight);

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
            pthread_mutex_lock(&g_lock); strncpy(line,g_label,sizeof line-1); line[sizeof line-1]=0; pthread_mutex_unlock(&g_lock);
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

out:
    if (vch)  { VI_DisableVirChn(dev,chn); VI_DestoryVirChn(dev,chn); }
    if (vipp) { VI_DisableVipp(dev); VI_DestoryVipp(dev); }
    ISP_Stop(g_isp); pthread_join(t,NULL); ISP_Exit();
    SYS_Exit();
    ioctl(fbfd,FBIOBLANK,FB_BLANK_UNBLANK);
    munmap(FB,maplen); close(fbfd);
    fprintf(stderr, rc==0 ? "OK\n" : "FAILED\n");
    return rc;
}
