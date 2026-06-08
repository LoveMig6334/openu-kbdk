# NPU CNN Runtime — Milestone 1 (`nncls`) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Prove the V831 NPU runs CNN inference from a plain headless C process by loading the on-board `fe_res18_117` model (128×128×3 → 256-float embedding) through `libmaix_nn.so` (AWNN) and verifying the forward pass is fault-free, deterministic, and input-sensitive.

**Architecture:** A self-contained C tool, `src/nncls.c`, that `dlopen`s the board's `libmaix_nn.so` and calls the libmaix NN C API — same "raw C, dlopen the vendor `.so`" pattern as `cammpp.c`/`camcc.c` (including the musl-1.1.16 `aw_dlsym` workaround). No camera/screen in M1: input is a host-generated raw-RGB file pushed over serial, isolating NPU bring-up from ISP/RAM contention. libmaix public headers are vendored under `vendor/libmaix/`.

**Tech Stack:** C (cross-compiled `arm-unknown-linux-musleabihf-gcc`, `-mcpu=cortex-a7 -mfpu=neon-vfpv4 -mfloat-abi=hard`); `dlopen`/`dlsym`; board-side AWNN runtime in `libmaix_nn.so`; `./bin/uai` for transfer/exec; pure-stdlib Python host helper for test inputs.

**Scope note:** This plan is **Milestone 1 only** (runtime bring-up). Milestone 2 (live VOC-20 YOLOv2 detection on the LCD) gets its own plan once M1 confirms the runtime works headless and yields real latency/RAM numbers — its design depends on those facts. See the design spec `docs/superpowers/specs/2026-06-08-npu-cnn-runtime-design.md`.

**Reference facts (verified live on the board):**
- Runtime `.so`: `/usr/lib/python3.8/site-packages/maix/libmaix_nn.so`; run with `LD_LIBRARY_PATH=/usr/lib/eyesee-mpp:/usr/lib`.
- Exported symbols present: `libmaix_nn_module_init`, `libmaix_nn_create`, `libmaix_nn_destroy`, `libmaix_nn_feature_compare_float` (plus the struct's `init`/`load`/`forward` function pointers).
- Model: `/home/model/face_recognize/fe_res18_117.{param,bin}`; input blob `inputs` `0=128 1=128 2=3`; output blob `FC_blob` = `InnerProduct ... 0=256` (256-float).
- Success enum value: `LIBMAIX_ERR_NONE = 0`.

---

## File Structure

- `vendor/libmaix/libmaix_err.h` — vendored: `libmaix_err_t` enum (new)
- `vendor/libmaix/libmaix_nn.h` — vendored: NN structs/enums/function decls (new)
- `vendor/libmaix/README.md` — provenance + license note (new)
- `src/nncls.c` — the M1 tool: dlopen runtime, load model, forward, verify (new)
- `captures/ppm2raw.py` — host helper: P6 PPM → raw RGB888 with nearest-neighbor resize (new)
- `Makefile` — add `nncls` + `deploy-nncls` targets (modify)
- `CLAUDE.md` — flip the NPU row from ❌ gap to ✅ working via libmaix_nn/AWNN; note `nncls` (modify)

---

## Task 1: Vendor the libmaix headers

**Files:**
- Create: `vendor/libmaix/libmaix_err.h`
- Create: `vendor/libmaix/libmaix_nn.h`
- Create: `vendor/libmaix/README.md`

- [ ] **Step 1: Write `vendor/libmaix/libmaix_err.h`**

```c
#ifndef __LIBMAIX_ERR_H
#define __LIBMAIX_ERR_H

/* Vendored from sipeed/libmaix (components/libmaix/include/libmaix_err.h).
 * Only the success value (LIBMAIX_ERR_NONE == 0) is relied upon. */
typedef enum
{
    LIBMAIX_ERR_NONE          = 0,
    LIBMAIX_ERR_PARAM         = 1,
    LIBMAIX_ERR_NO_MEM        = 2,
    LIBMAIX_ERR_NOT_IMPLEMENT = 3,
    LIBMAIX_ERR_NOT_READY     = 4,
    LIBMAIX_ERR_NOT_INIT      = 5,
    LIBMAIX_ERR_NOT_PERMIT    = 6,
    LIBMAIX_ERR_NOT_EXEC      = 7,
    LIBMAIX_ERR_UNKNOWN,
} libmaix_err_t;

#endif
```

- [ ] **Step 2: Write `vendor/libmaix/libmaix_nn.h`**

```c
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
```

- [ ] **Step 3: Write `vendor/libmaix/README.md`**

```markdown
# Vendored libmaix headers (V831 NPU / AWNN)

Source: https://github.com/sipeed/libmaix — `components/libmaix/include/`
(`libmaix_nn.h`, `libmaix_err.h`). Trimmed to the AWNN code path this toolkit uses.

These are **header-only**: we never link `libmaix`. At runtime the tool `dlopen`s the
board's own `/usr/lib/python3.8/site-packages/maix/libmaix_nn.so` and `dlsym`s the
exported symbols (verified to match these declarations against the on-board `.so`).

License: libmaix is published by Sipeed; see the upstream repository for its terms. Same
treatment/rationale as `vendor/eyesee-mpp/` (headers vendored for the build only).
```

- [ ] **Step 4: Verify the headers compile standalone**

Run:
```bash
cc -fsyntax-only -xc -Ivendor/libmaix - <<'EOF'
#include "libmaix_nn.h"
int main(void){ libmaix_nn_layer_t l; (void)l; return LIBMAIX_ERR_NONE; }
EOF
echo "syntax rc=$?"
```
Expected: `syntax rc=0` (no errors/warnings).

- [ ] **Step 5: Commit**

```bash
git add vendor/libmaix/
git commit -m "Vendor libmaix NN headers (AWNN) for the V831 NPU runtime"
```

---

## Task 2: Makefile target + dlopen smoke stub

Prove the linkage path (cross-compile → push → dlopen the board's `libmaix_nn.so` with
the right `LD_LIBRARY_PATH`) **before** writing any NN logic. This de-risks the riskiest
part (does the runtime load at all from plain C) in isolation.

**Files:**
- Create: `src/nncls.c` (temporary stub; replaced in Task 4)
- Modify: `Makefile` (add `nncls` + `deploy-nncls` targets; extend `.PHONY` and `clean`)

- [ ] **Step 1: Write the stub `src/nncls.c`**

```c
/* nncls.c (stub) - prove we can dlopen the board's libmaix_nn.so from plain C. */
#include <stdio.h>
#include <dlfcn.h>

extern void *aw_dlsym(void *handle, const char *symbol) __asm__("dlsym");
#define NN_SO "/usr/lib/python3.8/site-packages/maix/libmaix_nn.so"

int main(void){
    void *h = dlopen(NN_SO, RTLD_NOW|RTLD_GLOBAL);
    if(!h){ fprintf(stderr,"dlopen %s: %s\n", NN_SO, dlerror()); return 1; }
    void *p = aw_dlsym(RTLD_DEFAULT, "libmaix_nn_create");
    printf("dlopen OK; libmaix_nn_create=%p\n", p);
    return p ? 0 : 1;
}
```

- [ ] **Step 2: Add Makefile targets**

After the `cammpp` block (around `Makefile:68`), add:
```make
nncls: bin/nncls
bin/nncls: src/nncls.c | bin
	$(CROSS) $(CROSSFLAGS) -Ivendor/libmaix -o $@ $< -ldl

deploy-nncls: nncls
	$(UAI) push bin/nncls /tmp/nncls && $(UAI) exec "chmod +x /tmp/nncls"
	@echo 'run: ./bin/uai exec "LD_LIBRARY_PATH=/usr/lib/eyesee-mpp:/usr/lib /tmp/nncls"'
```
Add `nncls` to the `.PHONY` line (`Makefile:28`) and `deploy-nncls` to it, and append
`bin/nncls` to the `clean` rule's file list (`Makefile:131`).

- [ ] **Step 3: Build**

Run: `make nncls`
Expected: compiles to `bin/nncls`, no errors.

- [ ] **Step 4: Deploy + run on the board**

Run:
```bash
make deploy-nncls
./bin/uai exec "LD_LIBRARY_PATH=/usr/lib/eyesee-mpp:/usr/lib /tmp/nncls"
```
Expected: `dlopen OK; libmaix_nn_create=0x...` and exit code 0.
If `dlopen` fails: check the `.so` path and that `LD_LIBRARY_PATH` includes
`/usr/lib/eyesee-mpp` (the NN lib's MPP deps live there).

- [ ] **Step 5: Commit**

```bash
git add Makefile src/nncls.c
git commit -m "Add nncls build/deploy targets + dlopen smoke stub for libmaix_nn"
```

---

## Task 3: Host helper to generate raw-RGB test inputs

**Files:**
- Create: `captures/ppm2raw.py`

- [ ] **Step 1: Write `captures/ppm2raw.py` (pure stdlib)**

```python
#!/usr/bin/env python3
"""ppm2raw.py SRC.ppm DST.rgb [W H]
Convert a binary P6 PPM to raw interleaved RGB888, nearest-neighbor resized.
Default size 128x128 (the fe_res18_117 NPU input). Pure stdlib, no deps."""
import sys

def main():
    if len(sys.argv) < 3:
        sys.exit("usage: ppm2raw.py SRC.ppm DST.rgb [W H]")
    src, dst = sys.argv[1], sys.argv[2]
    ow = int(sys.argv[3]) if len(sys.argv) > 3 else 128
    oh = int(sys.argv[4]) if len(sys.argv) > 4 else 128
    data = open(src, "rb").read()
    if data[:2] != b"P6":
        sys.exit("not a binary P6 PPM")
    idx, toks = 2, []
    while len(toks) < 3:                       # parse width, height, maxval
        while idx < len(data) and data[idx] in b" \t\r\n":
            idx += 1
        if data[idx:idx+1] == b"#":            # comment line
            while idx < len(data) and data[idx] not in b"\r\n":
                idx += 1
            continue
        start = idx
        while idx < len(data) and data[idx] not in b" \t\r\n":
            idx += 1
        toks.append(int(data[start:idx]))
    w, h, _maxv = toks
    idx += 1                                    # one whitespace byte after maxval
    pix = data[idx:idx + w*h*3]
    out = bytearray(ow*oh*3)
    for y in range(oh):
        sy = y * h // oh
        for x in range(ow):
            sx = x * w // ow
            si = (sy*w + sx) * 3
            di = (y*ow + x) * 3
            out[di:di+3] = pix[si:si+3]
    open(dst, "wb").write(out)
    print(f"wrote {dst}: {ow}x{oh}x3 = {len(out)} bytes (from {w}x{h})")

main()
```

- [ ] **Step 2: Generate two distinct test inputs from existing captures**

Run:
```bash
python3 captures/ppm2raw.py captures/now.ppm captures/inA_128.rgb 128 128
python3 captures/ppm2raw.py captures/frame.ppm captures/inB_128.rgb 128 128
ls -l captures/inA_128.rgb captures/inB_128.rgb
```
Expected: each file is exactly `49152` bytes (128*128*3). If `captures/frame.ppm` is
absent, substitute any second `captures/*.ppm` that differs from `now.ppm`.

- [ ] **Step 3: Verify the two inputs actually differ**

Run: `cmp -s captures/inA_128.rgb captures/inB_128.rgb; echo "same=$?"`
Expected: `same=1` (cmp returns non-zero → files differ), so the variance check in
Task 5 is meaningful.

- [ ] **Step 4: Commit**

```bash
git add captures/ppm2raw.py
git commit -m "Add pure-stdlib PPM->raw-RGB helper for NPU test inputs"
```
(The generated `*.rgb` files are throwaway test fixtures; do not commit them.)

---

## Task 4: Implement the full `nncls` runtime tool

**Files:**
- Modify: `src/nncls.c` (replace the stub with the full implementation)

- [ ] **Step 1: Replace `src/nncls.c` with the full tool**

```c
/* nncls.c - V831 NPU bring-up smoke test via libmaix_nn (AWNN).
 *
 * Loads fe_res18_117 (128x128x3 -> 256-float face embedding) and runs forward on the
 * NPU. dlopen's the board's libmaix_nn.so (we never link it). No camera/screen: input
 * is a raw RGB888 file (128*128*3 = 49152 bytes), so this isolates the NPU path.
 *
 * Build:  make nncls
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

    char *in_names[]  = { "inputs"  };
    char *out_names[] = { "FC_blob" };
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
```

- [ ] **Step 2: Build**

Run: `make nncls`
Expected: compiles cleanly to `bin/nncls`. Fix any header-field mismatch against
`vendor/libmaix/libmaix_nn.h` before proceeding.

- [ ] **Step 3: Deploy binary + model inputs to the board**

Run:
```bash
make deploy-nncls
./bin/uai push captures/inA_128.rgb /tmp/inA.rgb
./bin/uai push captures/inB_128.rgb /tmp/inB.rgb
```
Expected: all three pushes verify (size + md5) per `uai`.

- [ ] **Step 4: First real run (single image)**

Run:
```bash
./bin/uai exec "LD_LIBRARY_PATH=/usr/lib/eyesee-mpp:/usr/lib /tmp/nncls /tmp/inA.rgb"
```
Expected: a line `forward OK  latency=… ms/inf … det(A==A)=YES`, a `featA[0..7]=` line
with non-zero/non-identical floats, `self-compare(A,A)=1.0000`, and exit code 0. No
SIGILL/SIGBUS. The `[mem]` lines on stderr report RSS.
If `nn load failed`: re-check the blob names (`inputs`/`FC_blob`) and model paths.
If it faults: capture board output with `./bin/uai monitor -t` in a second step and
apply superpowers:systematic-debugging before changing code.

- [ ] **Step 5: Commit**

```bash
git add src/nncls.c
git commit -m "Implement nncls: fe_res18 forward on the V831 NPU via libmaix_nn"
```

---

## Task 5: Verify the M1 success criteria

This is the milestone gate. No new code unless a check fails.

- [ ] **Step 1: Determinism — run twice, outputs identical**

Run:
```bash
./bin/uai exec "LD_LIBRARY_PATH=/usr/lib/eyesee-mpp:/usr/lib /tmp/nncls /tmp/inA.rgb" | tee /tmp/run1.txt
./bin/uai exec "LD_LIBRARY_PATH=/usr/lib/eyesee-mpp:/usr/lib /tmp/nncls /tmp/inA.rgb" | tee /tmp/run2.txt
diff <(grep featA /tmp/run1.txt) <(grep featA /tmp/run2.txt); echo "feat-diff rc=$?"
```
Expected: `det(A==A)=YES` in both runs and `feat-diff rc=0` (identical `featA` line
across separate process invocations).

- [ ] **Step 2: Variance + embedding sanity — two different images**

Run:
```bash
./bin/uai exec "LD_LIBRARY_PATH=/usr/lib/eyesee-mpp:/usr/lib /tmp/nncls /tmp/inA.rgb /tmp/inB.rgb"
```
Expected: `differ(A!=B)=YES`, `self-compare(A,A)=1.0000`, and `compare(A,B)` strictly
less than 1.0 (a real embedding distinguishes the inputs).

- [ ] **Step 3: Record latency + RAM in the spec**

Read the `latency=… ms/inf` value and the `[mem] peak: RSS=… kB` from Step 2's run.
Append a short "M1 measured results" note (latency, peak RSS, board firmware date) to
`docs/superpowers/specs/2026-06-08-npu-cnn-runtime-design.md` under the M1 section.
These numbers are the input to the M2 plan's RAM budget.

- [ ] **Step 4: Commit the measured results**

```bash
git add docs/superpowers/specs/2026-06-08-npu-cnn-runtime-design.md
git commit -m "Record M1 NPU forward latency + RAM measurements"
```

---

## Task 6: Update CLAUDE.md (NPU now works)

**Files:**
- Modify: `CLAUDE.md` (capability table NPU row + a board-program note for `nncls`)

- [ ] **Step 1: Flip the NPU capability row**

In the capability table, replace the NPU row's `❌ gap` text with: ✅ working — CNN
inference runs on the V831 NPU from plain C via the board's `libmaix_nn.so` (AWNN, a
quantized-ncnn engine; models are `.param`+`.bin`). `nncls.c` dlopen's it (no link),
loads `fe_res18_117` (128×128×3 → 256-float embedding), runs forward. Run with
`LD_LIBRARY_PATH=/usr/lib/eyesee-mpp:/usr/lib`. Note that the earlier "needs reverse
engineering" assessment was wrong: the runtime ships on the board.

- [ ] **Step 2: Add an `nncls.c` entry to the board-side programs section**

Mirror the existing `cammpp.c` note style: model `.param`/`.bin` are quantized ncnn
(`7767517` magic); input is HWC uint8 with `mean=[127.5×3]`/`norm=[0.0078125×3]` and
`need_quantization=true`; the NPU runs the backbone, decoders/argmax run on the CPU;
timing uses `/proc/uptime` to dodge the musl time64 trap; `libmaix_nn_module_init` then
`create`/`init`/`load`/`forward`. Note M2 (live YOLOv2 on the LCD) is the next step.

- [ ] **Step 3: Commit**

```bash
git add CLAUDE.md
git commit -m "Document the V831 NPU CNN runtime (nncls) in CLAUDE.md"
```

---

## Next plan (Milestone 2 — to be written after M1)

Once M1 confirms the runtime and yields latency/RAM numbers, write
`docs/superpowers/plans/<date>-npu-cnn-runtime-m2.md` covering **`nndetect`**: live
VOC-20 YOLOv2 (`yolo2_20class_awnn`) — MPP VI+ISP capture (reuse `cammpp.c`) → resize to
224×224 RGB → NPU forward → CPU YOLOv2 decode (dlopen the board's exported
`libmaix_nn_decoder_yolo2_*`, or port `decoder.Yolo2`) → draw boxes/labels → blit to
`/dev/fb0` (reuse `camcc.c`'s vsync path). That plan must vendor the decoder header and
budget RAM for camera+ISP+model coexisting in ~60 MB.

---

## Self-Review (completed during planning)

- **Spec coverage:** M1 milestone (model choice, isolated static-frame test, success
  criteria, RAM logging, build/run) → Tasks 1–6. M2 explicitly deferred to its own plan
  per the spec's milestone split. Risk #1 (NPU reachable headless) → Task 2 stub + Task 4
  run. Risk #2 (C API shape) → Task 1 vendored headers verified vs on-board symbols.
- **Placeholders:** none — all code and commands are concrete. The two `*.rgb` fixtures
  are generated by Task 3's committed script, not placeholders.
- **Type consistency:** `libmaix_nn_t`, `libmaix_nn_layer_t`, field names
  (`need_quantization`, `buff_quantization`, `awnn.param_path`, `awnn.mean/norm`),
  `LIBMAIX_ERR_NONE`, blob names `inputs`/`FC_blob`, and `FEAT_LEN=256` match across the
  header (Task 1) and the tool (Task 4).
