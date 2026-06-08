# CNN inference runtime for the KidBright µAI (V831 NPU)

**Date:** 2026-06-08
**Status:** Approved design — ready for implementation plan
**Scope:** Bring up on-board CNN inference on the Allwinner V831 NPU from a plain C
tool, in the same "raw C, dlopen the vendor `.so`" style as the rest of this toolkit.

## Background / why this is now tractable

CLAUDE.md previously listed the NPU as a ❌ gap "needing reverse engineering of vendor
blobs." A live probe of the board (`./bin/uai exec ...`) showed that is **wrong** — a
complete, working NN runtime already ships on the board:

- **Runtime:** `libmaix_nn.so` (the real copy is at
  `/usr/lib/python3.8/site-packages/maix/libmaix_nn.so`) embeds the **AWNN** engine
  (Allwinner NN — symbols `awnn_*`, `AWNNTensorDesc`, `libmaix_nn_hal_forward`). The
  AWNN engine is statically linked into `libmaix_nn.so`; there is **no** separate
  `libawnn.so`. The C API entry points `libmaix_nn_create` / `libmaix_nn_destroy`
  (plus `->load` / `->forward` on the returned struct) are exported.
- **Model format:** quantized **ncnn**. `.param` files begin with `7767517` (ncnn's
  magic number); AWNN is an ncnn fork with int8/NPU quantization extensions encoded as
  `-23328`/`-23329` per-layer scale arrays. A model is a `.param` (text topology) +
  `.bin` (int8 weights) pair.
- **Ready-made int8 models already on the board:**
  | Path | Type | Input | Notes |
  | --- | --- | --- | --- |
  | `/home/model/face_recognize/fe_res18_117.{param,bin}` | ResNet18 face embedding (single-output) | in `inputs` 128×128×3 → out `FC_blob` 256-float | **M1 model** — clean single forward, no decoder |
  | `/home/model/model_int8.{param,bin}` | **detector head**, not a plain classifier (multi-output: `output1` softmax + concat branches, `Conv 0=20`) | 224×224×3 | 5 MB; needs a decoder — *not* used for M1 |
  | `/home/model/yolo2_20class_awnn.{param,bin}` | YOLOv2 detector, VOC-20 | 224×224×3 | **M2 model** — known labels + working demo at `/home/yolo2_20class_awnn.py` |
  | `/home/model/face/yolo2_face_awnn.*` | YOLOv2 face | 224×224×3 | demo at `/home/retinaface.py` |
  | `/home/model/awnn_yolo2_mask_int8.*` | YOLOv2 mask | 224×224×3 | 11.8 MB (largest — RAM risk) |
- **Inference conventions** (from the working `/home/yolo2_20class_awnn.py`):
  - Input **224×224×3, HWC, uint8 bytes**.
  - Preprocess `mean=[127.5,127.5,127.5]`, `norm=[0.0078125,…]` (i.e. `(x-127.5)/128`).
  - `forward(bytes, quantize=true, layout="hwc")` — the runtime does the int8
    quantization internally from the `.param` scales; it returns a **float** output
    tensor.
  - The NPU runs only the conv backbone. **Decoders (YOLO anchors + NMS, softmax) run
    on the CPU** in userspace. This is why a single-output network (a feature embedding,
    no decoder) is the simplest possible first test.

### Hardware constraints (re-confirmed live)
- V831: single-core ARM Cortex-A7, armv7l hard-float NEON/VFPv4, Linux 4.9 musl/BusyBox.
- RAM: ~60 MB total (`MemTotal` ≈ 60048 kB).
- ~0.2-TOPS on-die NPU. **No `/dev/npu` node and no NPU module in `lsmod`** — AWNN
  reaches the silicon internally via `/dev/ion`, `/dev/mem`, `/dev/cedar_dev`. This is
  an assumption to validate in Milestone 1 (see Risks).

## Goal

A C-language CNN inference capability for the toolkit: load an AWNN `.param`/`.bin`
model, run a forward pass on the V831 NPU, and use the result — first as a bring-up
smoke test, then as a live on-screen detector — without MaixPy/CodeBlock.

Non-goal: writing an NPU runtime from scratch or reverse-engineering the NPU
register/ioctl interface beneath AWNN. We wrap the shipped, proven engine.

## Architecture

A new self-contained C tool (two milestones, possibly one source file with a mode arg,
or two small tools) that **dlopens `libmaix_nn.so`** and calls the libmaix NN C API.
Identical pattern to `cammpp.c`:

- Bind symbols at runtime with `dlopen`/`dlsym`. Use the musl-1.1.16 time64 workaround
  `extern void *aw_dlsym(...) __asm__("dlsym");` (the board lacks `__dlsym_time64`).
- **No linking against vendor libs.** Symbols resolved from
  `/usr/lib/python3.8/site-packages/maix/libmaix_nn.so` at runtime.
- libmaix public **headers vendored** under `vendor/libmaix/` (from Sipeed's
  open-source `libmaix` repo — same provenance/license treatment as
  `vendor/eyesee-mpp/`, with a README noting the source and license).
- Run with `LD_LIBRARY_PATH=/usr/lib/eyesee-mpp:/usr/lib` because `libmaix_nn.so`'s
  `ldd` pulls in the MPP/cedarx/ISP/ion libs.

### Components & boundaries
1. **NN wrapper** — thin layer over the dlopen'd libmaix NN API: `create → load(model
   config) → forward(input bytes) → output tensor → destroy`. Knows nothing about
   cameras or screens. This is the reusable core.
2. **Preprocess** — resize source image to 224×224, RGB, HWC uint8 with the
   mean/norm conventions. (M1: trivial, the input is already 224×224; M2: resize from
   the camera NV21 frame.)
3. **Postprocess/decoder** — M1: softmax + top-k argmax. M2: CPU YOLOv2 decoder
   (anchors + confidence threshold + NMS), ported from the Python `decoder.Yolo2`.
4. **I/O sources/sinks** — M1: read a raw image file (pushed from host). M2: MPP VI+ISP
   capture (reused from `cammpp.c`) as source, `/dev/fb0` vsync blit (reused from
   `camcc.c`) as sink.

## Milestones

### Milestone 1 — `nncls`: prove the NPU on a static frame (no camera)
- **Model: `fe_res18_117`** (face-embedding ResNet18). Chosen after inspecting the
  on-board params: `model_int8` turned out to be a *detector head* (multiple outputs,
  needs a decoder), whereas `fe_res18_117` is a clean **single-input → single-output**
  network (input `inputs` 128×128×3 → output `FC_blob`, a 256-float embedding). That is
  the simplest possible no-decoder NPU bring-up — exactly the intent of "classifier
  first" — and it enables a *meaningful* check via the exported
  `libmaix_nn_feature_compare_float`.
- Input: two host-supplied **128×128×3 raw RGB** files, uploaded via `uai push`,
  generated on the host from existing `captures/*.ppm` by a pure-stdlib resizer. This
  keeps NPU bring-up **isolated from camera/ISP/RAM contention** — the cleanest possible
  test of "does AWNN run from a plain headless C process."
- Flow: `libmaix_nn_module_init` → `libmaix_nn_create` → `->init` → `->load`
  (`{inputs:(128,128,3), output FC_blob, mean:[127.5×3], norm:[0.0078125×3]}`) →
  build input layer (`dtype=UINT8, layout=HWC, need_quantization=true`) → `->forward`
  → read 256-float output → print first few values, inference latency (ms), and a
  `free`/RSS snapshot.
- **Success criteria:**
  1. `forward` completes without SIGILL/SIGBUS/fault from a plain C process.
  2. Output is **bit-for-bit deterministic** for a fixed input across repeated runs.
  3. Output **differs** between two different input images (not constant).
  4. `feature_compare_float(out, out)` ≈ 1.0 and `compare(imgA, imgB)` < that — sanity
     that the vector is a real embedding, not garbage.
  5. Inference latency and peak RAM recorded.
- This is a runtime/latency/RAM smoke test (no labels needed), proving the NN path
  works headless from plain C before any camera/screen wiring.

#### M1 measured results (2026-06-08, board firmware Linux 4.9.118 #3849 2024-06-18)
**All success criteria PASS.** `nncls` loads `fe_res18_117` and runs forward from a plain
C process:
- **Latency:** ~31–32 ms/inference (avg of 10), single Cortex-A7.
- **Peak RSS:** ~9.0 MB (8992–9032 kB) — comfortable in the ~60 MB budget.
- **Determinism:** `featA[0..7]` byte-identical across separate process invocations.
- **Variance / embedding sanity:** `compare(A,A)=100.0`, `compare(A,B)=87.3` (two similar
  camera frames) — `feature_compare_float` returns a 0–100 similarity, 100 = identical.
- **Two corrections vs the original plan:** (1) the input tensor is the *blob* name
  `inputs_blob`, not the layer name `inputs` (the layer-name guess segfaulted in
  `awnn_quantize`/`getTensorScale`); (2) `libmaix_nn.so` needs exported stubs for two
  undefined retinaface decoder back-refs, and musl binds immediately (RTLD_LAZY is a
  no-op) so they must resolve at load (`-Wl,--export-dynamic`).

**Key finding — the NPU runs on CPU here, not silicon.** `open(/dev/nna)` fails
(`nna_fd=-1`): this board image has **no NPU kernel driver** — no `nna`/`npu` `.ko` in
`/lib/modules`, nothing in `/proc/devices`, no `/sys/class`, nothing in dmesg (consistent
with the memory note that `import maix` crashes). AWNN tolerates this and falls back to
**CPU int8 (NEON)** execution, so inference is fully functional but **not
NPU-accelerated**. Enabling the ~0.2-TOPS NPU requires obtaining/loading the V831 `nna`
kernel module (absent from this rootfs) — tracked as follow-on work, not an M1 blocker.

### Milestone 2 — `nndetect`: live labeled detection on the LCD
- Model: `yolo2_20class_awnn` (VOC-20, has known labels + a reference Python demo to
  validate against).
- Flow: MPP VI+ISP capture (NV21, reused from `cammpp.c`) → resize/convert to 224×224
  RGB HWC → `forward` on NPU → **CPU YOLOv2 decoder** (labels + anchors below) → draw
  boxes + label strings → blit to `/dev/fb0` via the vsync, no-page-flip path from
  `camcc.c`.
- Decoder params from the reference demo: output grid `7×7×((1+4+20)*5)`, 5 anchors
  `[5.4,5.38, 1.65,2.09, 0.8,1.83, 2.45,4.14, 0.46,0.8]`, `nms=0.3`,
  `threshold=0.3`, net in/out `(224,224)`/`(7,7)`.
- VOC labels: `aeroplane, bicycle, bird, boat, bottle, bus, car, cat, chair, cow,
  diningtable, dog, horse, motorbike, person, pottedplant, sheep, sofa, train,
  tvmonitor`.
- **Success criteria:** live bounding boxes + labels for VOC classes drawn on the
  240×240 panel at an interactive frame rate, verifiable remotely by reading back
  `/dev/fb0` (the gzip+hexdump technique already used for `camcc`).

## RAM strategy (~60 MB total)
- **Pure C, no Python/rpyc** — frees the most headroom vs. the MaixPy path.
- **M1 runs with no camera/ISP** brought up: just the ResNet18 model + small activation
  buffers (128×128×3 input ≈ 48 KB + a few MB of intermediates) → comfortable.
- **M2 adds MPP ISP+VI.** Start with small int8 models; avoid the 11.8 MB mask model
  until the budget is measured. Reuse a single fb page (no double-buffer growth).
- Log `free` / RSS at each stage (after `create`, after `load`, after first `forward`)
  so the real budget is visible rather than guessed.

## Build & run
- New `make nncls` / `make nndetect` targets mirroring `make cammpp`: same cross flags
  (`-mcpu=cortex-a7 -mfpu=neon-vfpv4 -mfloat-abi=hard`), same `MPPINC`/`MPPDEF` for M2's
  camera path, plus `-Ivendor/libmaix`.
- Deploy with the existing `uai deploy` flow; run with
  `LD_LIBRARY_PATH=/usr/lib/eyesee-mpp:/usr/lib`.

## Risks / things to verify early
1. **NPU reachability from a headless C process** (primary risk). No `/dev/npu` node;
   AWNN uses `ion`/`mem`/`cedar_dev` internally. M1 *is* the test — analogous to how
   `cammpp` proved MPP runs without MaixPy's crashing Python init. If `forward` faults,
   inspect what `libmaix_nn.so` opens (`strace`/`ltrace` if present, else `strings` for
   `/dev/...` paths) before escalating.
2. **libmaix NN C API exact shape.** The Python demo shows the *semantics*; the C struct
   (`libmaix_nn_t`, the `nn_config`/`nn_layer` types, `forward` signature) comes from the
   vendored libmaix headers. If headers and the on-board `.so` ABI disagree, the symbol
   list from the `.so` is the source of truth.
3. **Quantization path.** `quantize=true` means the runtime quantizes from the `.param`
   scales; confirm the C API exposes the same flag / input dtype expectation.
4. **RAM for M2** when camera + ISP + model coexist; mitigate by model choice and
   staged `free` logging.

## Out of scope (future / research)
- Custom model conversion (training → AWNN `.param`/`.bin`) — uses Allwinner's offline
  converter, not needed while reusing on-board models.
- Reverse-engineering the NPU below AWNN.
- Face recognition pipeline (`fe_res18_117` embeddings), mask detector — straightforward
  follow-ons once the runtime + YOLO decoder exist.
