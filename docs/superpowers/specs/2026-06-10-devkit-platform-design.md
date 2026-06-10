# KidBright µAI dev-kit platform (kbdk) — design

Date: 2026-06-10. Status: approved direction from the project owner (architecture was
specified up front); hardware survey results below adjusted three decisions, marked **[survey]**.

## Goal

Evolve this repo from a board-probe toolkit into a **dev-kit platform**: fine-tune an AI
model on the Mac, convert it, push it to the KidBright µAI (Allwinner V831), and run it
live against the camera with on-screen results — matching the official KidBright µAI
feature set (image classification + object detection) without the CodeBlock/MaixPy stack.

- Host: **Rust** (CLI + egui UI, Catppuccin Mocha) orchestrating a **Python** train/convert
  pipeline via `uv run` subprocesses streaming JSON-lines progress events.
- Board: C/C++ runner reusing the existing MPP camera path + a vanilla **ncnn** runtime.
- Existing C tools and Makefile keep working; `third_party/v831-npu` (GPLv3) stays an
  isolated binary; the platform is MIT. NPU compiler (ONNX→NVDLA) is out of scope —
  CPU ncnn first.

## Hardware survey results (2026-06-10, live board)

| Question | Answer |
| --- | --- |
| OTG gadget | Board runs `/bin/adbd` (configfs `ffs.adb` + `mass_storage.usb0`→mmcblk0p4) on UDC `5100000.udc-controller`. Mac enumerates VID 0x18d1/PID 0x0002 "MaixPy3". |
| CDC-ACM/RNDIS | **Not in this kernel** (configfs `mkdir functions/acm.x` → ENOENT; no /proc/config.gz). No /dev/ttyGS* possible. |
| ADB | `adb shell` = root, no auth. `adb push` ≈ **5–6 MB/s** (md5-verified). No shell_v2: exit codes NOT propagated, stderr merged. `adb forward tcp:` works. |
| AWNN vs vanilla ncnn | `libmaix_nn.so` loads vanilla ncnn .param/.bin **without error but forwards to saturated int8 garbage** (−128…127 vs ground truth ±0.1) — it requires AWNN's proprietary quantize keys; the ncnn→AWNN converter is MaixHub-online-only (see docs/research/2026-06-08-v831-maixhub-models.md). Proven with `src/nnload.c` + host ncnn ground truth. |
| Vanilla ncnn on board | Cross-compiles clean (musl-armhf static, NEON, no vulkan/omp/threads); 2.9 MB runner; output matches host bit-for-nearly (−0.1473 vs −0.1470). |
| Perf/RAM @224, 1×A7 | ResNet18 fp32: **OOM-killed** (47 MB fp32 weights vs ~34 MB free). ResNet18 int8: runs, 12 MB file, but **4.6–6.1 s/inf** (ncnn armv7 int8 requantize path is pathological). MobileNetV2 int8: **466 ms/inf, 15 MB RSS**. MobileNetV2 fp16-storage: 603 ms, 32 MB RSS. |
| Storage | `/mnt/UDISK` = 57.8 GB vfat (mmcblk0p4, also mounted /root) for models — **flaky** (I/O errors, FSCK*.REC artifacts) → every push md5-verified with retry. `/tmp` = 29 MB tmpfs. |
| Host | rustc/cargo 1.96, uv 0.11.19, torch 2.12 with **MPS available**, pnnx (PyPI) works, ncnn host tools (ncnn2table/ncnn2int8) build natively, adb installed. |

## Decisions forced by the survey **[survey]**

1. **Transport = ADB over USB-OTG** (preferred path confirmed). uai/serial stays as console
   + fallback. No on-board raw decoder / raised baud needed.
2. **Deploy format = vanilla ncnn int8** (.param/.bin) run by **our own statically-linked
   ncnn runner**. AWNN is unusable for self-trained models (silent garbage + closed converter).
3. **Default classification backbone = MobileNetV2** (466 ms int8 @224; usable). ResNet-18
   stays a supported option to match official KidBright branding, with a documented
   slow-path warning (~5–6 s/inf int8; fp32 impossible in RAM). Detection = YOLOv2-slim
   (MobileNet-ish backbone) as planned. Input size is configurable (224 default; 128–160
   for faster UX).

## Architecture

```
kidbright-uai/
├── Makefile, src/, vendor/, third_party/   # existing, unchanged behaviour
├── Cargo.toml                  # workspace
├── crates/
│   ├── kbdk-core/              # transport + protocol library (no UI deps)
│   ├── kbdk-cli/               # `kbdk` binary
│   └── kbdk-ui/                # egui/eframe app, Catppuccin Mocha
├── py/
│   ├── pyproject.toml          # uv workspace root (python 3.12)
│   ├── kbdk-train/             # PyTorch (MPS): ImageFolder → backbone → TorchScript
│   └── kbdk-convert/           # pnnx → ncnn → int8 quantize → model pack
├── board/
│   ├── runner/                 # kbrun.cpp: MPP camera → ncnn → fb0 overlay (label/bbox)
│   └── ncnn/                   # toolchain file + build script for libncnn.a (musl-armhf)
└── models/                     # packs land here (gitignored except samples)
```

### kbdk-core (Rust lib)

- **Transport trait** with two impls: `AdbTransport` (spawns `adb` subprocess; push/pull/
  exec) and `SerialTransport` (port of src/uai.c: sentinel exec `'UAI''_BEGIN'` quote-split,
  octal-printf push, md5+size verify).
- Exit codes over both transports via sentinel wrapping (`echo 'KB''_END_'$?`) because
  adbd lacks shell_v2 — same constraint as serial: commands must not end in bare `&`.
- **Device discovery**: adb by VID/PID 0x18d1:0x0002 + serial `20080411`; serial console by
  `/dev/cu.usbserial-*`. Auto-pick ADB when present, else serial; both can be open at once.
- **Verified push**: md5 after write (UDISK vfat is flaky), retry ×3; big files → /mnt/UDISK,
  small/exec → /tmp.
- **Model pack**: a directory/tar with `manifest.json` (name, task type cls/det, backbone,
  input WxH, mean/norm, labels, anchors for det, int8/fp16, file md5s) + .param + .bin + labels.txt.

### kbdk-cli / kbdk-ui

- CLI verbs: `kbdk devices`, `exec`, `push`, `pull`, `deploy <pack>`, `run <pack>`,
  `train`, `convert`, `monitor`. Train/convert shell out to `uv run` and re-emit/render
  the JSON-lines progress.
- UI (egui/eframe, Catppuccin Mocha): dataset picker (ImageFolder), train tab (live loss/
  acc curves from JSON-lines), convert tab, deploy tab (device status, push progress,
  start/stop runner, last classification results read back over exec).

### Python side (uv workspace, Python 3.12)

- **kbdk-train**: `ImageFolder` dataset → fine-tune MobileNetV2 (default) or ResNet-18
  (transfer learning, MPS) → TorchScript export. Later: YOLOv2-slim detection head.
  Emits `{"event":"epoch","loss":…,"acc":…}` JSON-lines on stdout.
- **kbdk-convert**: TorchScript → pnnx → ncnn (.param/.bin, fp16 storage) → calibration
  (ncnn2table, real dataset images) → ncnn2int8 → pack with manifest.json. Bundles/builds
  the host ncnn tools (ncnn2table/ncnn2int8 cross of the same pinned ncnn release as the
  board lib).
- Pinned single ncnn release for host tools, host python verification, and board lib.

### Board runner (`board/runner/kbrun.cpp`)

- One process: MPP NV21 capture (dlopen, as `cammpp.c`/`nnacam.cpp`) → gray-world AWB →
  fb0 preview (vsync-gated, as `camcc.c`) → centre-crop/scale to net input → **ncnn**
  forward on a worker thread (latest-wins handoff, as `nnacam.cpp`) → overlay label+conf
  (cls) or boxes (det, YOLOv2 decode on CPU) with the existing 8×8 font.
- Statically linked against `libncnn.a` (musl-armhf): no LD_LIBRARY_PATH, immune to the
  musl-1.1.16 time64 symbol traps. MPP still dlopen'd (needs vendor .so at runtime).
- Reads the model pack from /mnt/UDISK/kbdk/<pack>/ via manifest.json; prints
  `{"label":…,"conf":…}` JSON-lines to stdout for the host to read over exec/monitor.
- RAM budget ≤ ~35 MB peak (mbv2-int8 measured 15 MB + preview buffers ≈ 22 MB total).

## Error handling

- Transport: every push verified (size+md5), 3 retries, then fail loudly naming the flaky
  partition. Exec timeouts kill the board-side process group (runner started via
  `(cmd) & true` with pidfile).
- Pipeline: subprocess JSON-lines include `{"event":"error"}`; Rust surfaces stderr tail on
  non-zero exit. Convert verifies the int8 model on host (python ncnn) against N dataset
  images before packing (top-1 agreement fp32-vs-int8 threshold).
- Runner: model-load failure prints a JSON error and exits non-zero (picked up via sentinel rc).

## Testing

- kbdk-core: unit tests for sentinel parsing/octal encode against recorded transcripts;
  integration tests gated on hardware presence (`KBDK_HW=1`).
- Python: pytest on a 3-class toy ImageFolder (generated); convert smoke test asserts
  host-ncnn output parity fp32 vs int8.
- Board: each phase ends with a hardware verification step over adb (documented command +
  expected output), same style as the existing probes.

## Phases (verify on hardware after each)

1. **Workspace scaffolding + kbdk-core ADB transport** — `kbdk devices/exec/push` works
   against the live board (md5-verified UDISK push).
2. **kbdk-core serial transport** (uai.c port) + auto-detection; parity tests.
3. **Board ncnn runtime**: pinned ncnn cross-build script + `kbrun` classification on a
   static test image; verify output parity with host.
4. **kbdk-convert**: pnnx→ncnn→int8→pack; deploy the pack; board output matches host int8.
5. **kbdk-train**: MobileNetV2 fine-tune on toy dataset (MPS) → through convert → board
   runs it live on camera with label overlay. ← end-to-end milestone
6. **kbdk-cli polish + kbdk-ui** (egui, Mocha): train/convert/deploy from the UI.
7. **YOLOv2-slim detection**: train head, anchors in manifest, bbox overlay on fb0.
8. Docs/publish pass (portability: Linux hosts, licence split).

## Out of scope (now)

NPU/NVDLA compiler, RNDIS/network gadget, rootfs modification, audio in the runner,
multi-board fleet support.
