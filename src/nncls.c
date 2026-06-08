/* nncls.c - V831 NPU bring-up smoke test via libmaix_nn (AWNN).
 *
 * Loads fe_res18_117 (128x128x3 -> 256-float face embedding) and runs forward on the
 * NPU. dlopen's the board's libmaix_nn.so (we never link it). No camera/screen: input
 * is a raw RGB888 file (128*128*3 = 49152 bytes), so this isolates the NPU path.
 *
 * Build:  make nncls          (needs -Wl,--export-dynamic; see the stubs below)
 * Run:    LD_LIBRARY_PATH=/usr/lib/eyesee-mpp:/usr/lib /tmp/nncls IN.rgb [IN2.rgb]
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include <dlfcn.h>

#include "libmaix_nn.h"

/* board musl 1.1.16 has plain dlsym, not the cross toolchain's __dlsym_time64 */
extern void *aw_dlsym(void *handle, const char *symbol) __asm__("dlsym");

/* libmaix_nn.so carries undefined back-references to retinaface decoder hooks that
 * normally live in the _maix_nn Python extension. musl always binds immediately
 * (RTLD_LAZY is a no-op), so they must resolve at load. We never use that decoder, so
 * export harmless stubs from this executable (built with -Wl,--export-dynamic) for the
 * loader to bind against. */
void retinaface_get_priorboxes(void){}
void retinaface_decode(void){}

#define NN_SO        "/usr/lib/python3.8/site-packages/maix/libmaix_nn.so"
#define IN_W         128
#define IN_H         128
#define IN_C         3
#define FEAT_LEN     256
#define MODEL_PARAM  "/home/model/face_recognize/fe_res18_117.param"
#define MODEL_BIN    "/home/model/face_recognize/fe_res18_117.bin"

typedef libmaix_err_t (*fn_module_init)(void);
typedef libmaix_nn_t* (*fn_create)(void);
typedef void          (*fn_destroy)(libmaix_nn_t**);
typedef float         (*fn_cmp)(float*, float*, int);

static fn_module_init nn_module_init;
static fn_create      nn_create;
static fn_destroy     nn_destroy;
static fn_cmp         nn_cmp;

/* wall clock without any time64-redirected libc call (see CLAUDE.md musl note) */
static double uptime_now(void){
    FILE *f = fopen("/proc/uptime", "r");
    double t = 0;
    if(f){ if(fscanf(f, "%lf", &t) != 1) t = 0; fclose(f); }
    return t;
}
static long rss_kb(void){
    FILE *f = fopen("/proc/self/status", "r");
    if(!f) return -1;
    char line[128]; long kb = -1;
    while(fgets(line, sizeof line, f))
        if(sscanf(line, "VmRSS: %ld kB", &kb) == 1) break;
    fclose(f);
    return kb;
}
static uint8_t *read_file(const char *path, size_t want){
    FILE *f = fopen(path, "rb");
    if(!f){ perror(path); return NULL; }
    uint8_t *buf = malloc(want);
    size_t n = fread(buf, 1, want, f);
    fclose(f);
    if(n != want){
        fprintf(stderr, "%s: read %zu bytes, want %zu\n", path, n, want);
        free(buf); return NULL;
    }
    return buf;
}
static int run_forward(libmaix_nn_t *nn, uint8_t *rgb, uint8_t *qbuf, float *out_feat){
    libmaix_nn_layer_t in = {
        .w = IN_W, .h = IN_H, .c = IN_C,
        .dtype = LIBMAIX_NN_DTYPE_UINT8, .layout = LIBMAIX_NN_LAYOUT_HWC,
        .need_quantization = true, .data = rgb, .buff_quantization = qbuf,
    };
    libmaix_nn_layer_t out = {
        .w = 1, .h = 1, .c = FEAT_LEN,
        .dtype = LIBMAIX_NN_DTYPE_FLOAT, .layout = LIBMAIX_NN_LAYOUT_HWC,
        .need_quantization = false, .data = out_feat, .buff_quantization = NULL,
    };
    libmaix_err_t e = nn->forward(nn, &in, &out);
    if(e != LIBMAIX_ERR_NONE){ fprintf(stderr, "forward err %d\n", e); return -1; }
    return 0;
}

int main(int argc, char **argv){
    if(argc < 2){
        fprintf(stderr, "usage: %s IN.rgb [IN2.rgb]   (IN = %dx%dx%d raw RGB, %d bytes)\n",
                argv[0], IN_W, IN_H, IN_C, IN_W*IN_H*IN_C);
        return 2;
    }
    size_t insz = (size_t)IN_W * IN_H * IN_C;

    /* 1. load runtime + resolve symbols */
    if(!dlopen(NN_SO, RTLD_NOW|RTLD_GLOBAL)){
        fprintf(stderr, "dlopen %s: %s\n", NN_SO, dlerror()); return 1;
    }
    #define SYM(v,name) do{ *(void**)(&v) = aw_dlsym(RTLD_DEFAULT, name); \
        if(!v){ fprintf(stderr, "dlsym %s failed\n", name); return 1; } }while(0)
    SYM(nn_module_init, "libmaix_nn_module_init");
    SYM(nn_create,      "libmaix_nn_create");
    SYM(nn_destroy,     "libmaix_nn_destroy");
    SYM(nn_cmp,         "libmaix_nn_feature_compare_float");
    #undef SYM

    /* 2. create + load model */
    if(nn_module_init() != LIBMAIX_ERR_NONE){ fprintf(stderr, "module_init failed\n"); return 1; }
    libmaix_nn_t *nn = nn_create();
    if(!nn){ fprintf(stderr, "nn_create failed\n"); return 1; }
    if(nn->init(nn) != LIBMAIX_ERR_NONE){ fprintf(stderr, "nn init failed\n"); return 1; }

    char *in_names[]  = { "inputs_blob" };  /* blob name, not the layer name "inputs" */
    char *out_names[] = { "FC_blob"     };
    libmaix_nn_model_path_t mp; memset(&mp, 0, sizeof mp);
    mp.awnn.param_path = MODEL_PARAM;
    mp.awnn.bin_path   = MODEL_BIN;
    libmaix_nn_opt_param_t opt; memset(&opt, 0, sizeof opt);
    opt.awnn.input_names  = in_names;
    opt.awnn.output_names = out_names;
    opt.awnn.input_num    = 1;
    opt.awnn.output_num   = 1;
    opt.awnn.mean[0] = opt.awnn.mean[1] = opt.awnn.mean[2] = 127.5f;
    opt.awnn.norm[0] = opt.awnn.norm[1] = opt.awnn.norm[2] = 0.0078125f;
    opt.awnn.encrypt = false;
    if(nn->load(nn, &mp, &opt) != LIBMAIX_ERR_NONE){ fprintf(stderr, "nn load failed\n"); return 1; }
    fprintf(stderr, "[mem] after load: RSS=%ld kB\n", rss_kb());

    /* 3. read input A */
    uint8_t *imgA = read_file(argv[1], insz);
    if(!imgA){ return 1; }
    uint8_t *qbuf = malloc(insz);
    float featA[FEAT_LEN], featA2[FEAT_LEN];

    /* 4. warmup + timed loop on the same image -> latency + determinism */
    if(run_forward(nn, imgA, qbuf, featA)){ return 1; }
    int N = 10;
    double t0 = uptime_now();
    for(int i = 0; i < N; i++)
        if(run_forward(nn, imgA, qbuf, featA2)){ return 1; }
    double t1 = uptime_now();
    int det = (memcmp(featA, featA2, sizeof featA) == 0);
    printf("forward OK  latency=%.1f ms/inf (avg of %d)  det(A==A)=%s\n",
           1000.0 * (t1 - t0) / N, N, det ? "YES" : "NO");
    printf("featA[0..7]=");
    for(int i = 0; i < 8; i++) printf(" % .4f", featA[i]);
    printf("\n");
    printf("self-compare(A,A)=%.4f\n", nn_cmp(featA, featA, FEAT_LEN));

    /* 5. optional second image -> variance + cross-compare */
    if(argc >= 3){
        uint8_t *imgB = read_file(argv[2], insz);
        if(!imgB){ return 1; }
        float featB[FEAT_LEN];
        if(run_forward(nn, imgB, qbuf, featB)){ return 1; }
        int differ = (memcmp(featA, featB, sizeof featA) != 0);
        printf("img B: differ(A!=B)=%s  compare(A,B)=%.4f\n",
               differ ? "YES" : "NO", nn_cmp(featA, featB, FEAT_LEN));
        free(imgB);
    }
    fprintf(stderr, "[mem] peak: RSS=%ld kB\n", rss_kb());

    free(imgA);
    free(qbuf);
    nn_destroy(&nn);
    return 0;
}
