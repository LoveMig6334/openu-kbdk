#ifndef __LIBMAIX_NN_H
#define __LIBMAIX_NN_H

#include <stdint.h>
#include <stdbool.h>
#include "libmaix_err.h"

/* Vendored from sipeed/libmaix (components/libmaix/include/libmaix_nn.h).
 * Trimmed to the AWNN path used by this toolkit. */

typedef enum {
    LIBMAIX_NN_DTYPE_UINT8 = 0,
    LIBMAIX_NN_DTYPE_INT8  = 1,
    LIBMAIX_NN_DTYPE_FLOAT = 2,
    LIBMAIX_NN_DTYPE_MAX
} libmaix_nn_dtype_t;

typedef enum {
    LIBMAIX_NN_LAYOUT_HWC = 0,
    LIBMAIX_NN_LAYOUT_CHW = 1,
    LIBMAIX_NN_LAYOUT_MAX
} libmaix_nn_layout_t;

typedef struct {
    int w;
    int h;
    int c;
    libmaix_nn_dtype_t  dtype;
    libmaix_nn_layout_t layout;
    bool  need_quantization;
    void* data;
    void* buff_quantization;
} libmaix_nn_layer_t;

typedef union {
    struct { char* param_path; char* bin_path;  } awnn;
    struct { char* model_path; char* reserved;  } aipu;
} libmaix_nn_model_path_t;

typedef union {
    struct {
        char**  input_names;
        char**  output_names;
        uint8_t input_num;
        uint8_t output_num;
        float   mean[3];
        float   norm[3];
        int*    input_ids;
        int*    output_ids;
        bool    encrypt;
    } awnn;
    struct {
        char**  input_names;
        char**  output_names;
        uint8_t input_num;
        uint8_t output_num;
        float   mean[3];
        float   norm[3];
        int*    input_ids;
        int*    output_ids;
        bool    encrypt;
        float   scale[5];
    } aipu;
} libmaix_nn_opt_param_t;

typedef struct libmaix_nn {
    void* config;
    libmaix_err_t (*init)(struct libmaix_nn* obj);
    libmaix_err_t (*deinit)(struct libmaix_nn* obj);
    libmaix_err_t (*load)(struct libmaix_nn* obj,
                          const libmaix_nn_model_path_t* path,
                          libmaix_nn_opt_param_t* opt);
    libmaix_err_t (*forward)(struct libmaix_nn* obj,
                             libmaix_nn_layer_t* inputs,
                             libmaix_nn_layer_t* outputs);
} libmaix_nn_t;

libmaix_err_t libmaix_nn_module_init(void);
libmaix_err_t libmaix_nn_module_deinit(void);
libmaix_nn_t* libmaix_nn_create(void);
void          libmaix_nn_destroy(libmaix_nn_t** obj);
float         libmaix_nn_feature_compare_float(float* a, float* b, int len);

#endif
