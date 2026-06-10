/* nnload.c - generic AWNN (libmaix_nn) model-load probe.
 *
 * Answers one question on real hardware: which .param/.bin files will the board's
 * libmaix_nn.so actually load and forward? Point it at any model + blob names + dims;
 * it reports each stage (dlopen / module_init / load / forward) separately so a
 * vanilla-ncnn file (no AWNN quantize keys) fails with a diagnosis, not a mystery.
 *
 * Build:  make nnload
 * Run:    LD_LIBRARY_PATH=/usr/lib/eyesee-mpp:/usr/lib \
 *           /tmp/nnload PARAM BIN IN_BLOB OUT_BLOB W H C OUT_C
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

/* stubs for libmaix_nn.so's undefined retinaface back-refs (see nncls.c) */
void retinaface_get_priorboxes(void){}
void retinaface_decode(void){}

#define NN_SO "/usr/lib/python3.8/site-packages/maix/libmaix_nn.so"

typedef libmaix_err_t (*fn_module_init)(void);
typedef libmaix_nn_t* (*fn_create)(void);
typedef void          (*fn_destroy)(libmaix_nn_t**);

int main(int argc, char **argv){
    if(argc != 9){
        fprintf(stderr, "usage: %s PARAM BIN IN_BLOB OUT_BLOB W H C OUT_C\n", argv[0]);
        return 2;
    }
    const char *param = argv[1], *bin = argv[2];
    char *in_blob = argv[3], *out_blob = argv[4];
    int w = atoi(argv[5]), h = atoi(argv[6]), c = atoi(argv[7]), outc = atoi(argv[8]);

    if(!dlopen(NN_SO, RTLD_NOW|RTLD_GLOBAL)){
        printf("STAGE dlopen: FAIL (%s)\n", dlerror()); return 1;
    }
    printf("STAGE dlopen: OK\n");

    fn_module_init nn_module_init; fn_create nn_create; fn_destroy nn_destroy;
    #define SYM(v,name) do{ *(void**)(&v) = aw_dlsym(RTLD_DEFAULT, name); \
        if(!v){ printf("STAGE dlsym %s: FAIL\n", name); return 1; } }while(0)
    SYM(nn_module_init, "libmaix_nn_module_init");
    SYM(nn_create,      "libmaix_nn_create");
    SYM(nn_destroy,     "libmaix_nn_destroy");
    #undef SYM

    if(nn_module_init() != LIBMAIX_ERR_NONE){ printf("STAGE module_init: FAIL\n"); return 1; }
    libmaix_nn_t *nn = nn_create();
    if(!nn || nn->init(nn) != LIBMAIX_ERR_NONE){ printf("STAGE create/init: FAIL\n"); return 1; }
    printf("STAGE create/init: OK\n");

    char *in_names[]  = { in_blob };
    char *out_names[] = { out_blob };
    libmaix_nn_model_path_t mp; memset(&mp, 0, sizeof mp);
    mp.awnn.param_path = param;
    mp.awnn.bin_path   = bin;
    libmaix_nn_opt_param_t opt; memset(&opt, 0, sizeof opt);
    opt.awnn.input_names  = in_names;
    opt.awnn.output_names = out_names;
    opt.awnn.input_num    = 1;
    opt.awnn.output_num   = 1;
    opt.awnn.mean[0] = opt.awnn.mean[1] = opt.awnn.mean[2] = 127.5f;
    opt.awnn.norm[0] = opt.awnn.norm[1] = opt.awnn.norm[2] = 0.0078125f;
    opt.awnn.encrypt = false;
    fflush(stdout);                      /* flush before a possible crash in load() */
    libmaix_err_t e = nn->load(nn, &mp, &opt);
    if(e != LIBMAIX_ERR_NONE){ printf("STAGE load: FAIL (err %d)\n", e); return 1; }
    printf("STAGE load: OK\n");

    size_t insz = (size_t)w * h * c;
    uint8_t *img  = malloc(insz);
    uint8_t *qbuf = malloc(insz);
    float   *out  = malloc(sizeof(float) * outc);
    for(size_t i = 0; i < insz; i++) img[i] = (uint8_t)(i * 31u);

    libmaix_nn_layer_t in = {
        .w = w, .h = h, .c = c,
        .dtype = LIBMAIX_NN_DTYPE_UINT8, .layout = LIBMAIX_NN_LAYOUT_HWC,
        .need_quantization = true, .data = img, .buff_quantization = qbuf,
    };
    libmaix_nn_layer_t outl = {
        .w = 1, .h = 1, .c = outc,
        .dtype = LIBMAIX_NN_DTYPE_FLOAT, .layout = LIBMAIX_NN_LAYOUT_HWC,
        .need_quantization = false, .data = out, .buff_quantization = NULL,
    };
    fflush(stdout);
    e = nn->forward(nn, &in, &outl);
    if(e != LIBMAIX_ERR_NONE){ printf("STAGE forward: FAIL (err %d)\n", e); return 1; }
    printf("STAGE forward: OK  out[0..3] = %.4f %.4f %.4f %.4f\n",
           out[0], outc > 1 ? out[1] : 0, outc > 2 ? out[2] : 0, outc > 3 ? out[3] : 0);

    nn_destroy(&nn);
    return 0;
}
