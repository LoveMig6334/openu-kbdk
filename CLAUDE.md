# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## What this is

A host-side toolkit for the **KidBright µAI** board (Allwinner V831 — single-core
ARM Cortex-A7, armv7l hard-float NEON/VFPv4, Linux 4.9 BusyBox/Tina, musl libc).
No network link to the board; two host↔board channels exist:

- **ADB over USB-OTG** (fast, preferred): the board runs `adbd` (FunctionFS gadget,
  VID 0x18d1 PID 0x0002 "MaixPy3"). Root shell, no auth, `adb push` ≈ 5–6 MB/s,
  `adb forward tcp:` works. adbd is ancient (no shell_v2): **no exit codes, stderr
  merged** — kbdk wraps commands with sentinels to recover rc. CDC-ACM/RNDIS are
  NOT in this kernel (configfs refuses `acm.*`).
- **UART serial console** (115200 8N1, `/dev/cu.usbserial-210`): the fallback +
  boot-log channel, driven by `bin/uai` or kbdk's serial transport.

Code here is compiled by two different compilers:

- **Host tools** (`src/uai.c`, the `kbdk` Rust workspace) — native macOS arm64.
- **Board binaries** (`src/*.c`, `board/runner/kbrun.cpp`) — cross-compiled for the
  V831, pushed (adb or uai), run on the board.

## Project goal

The end goal is a **C-language development toolkit for building AIoT projects on the
KidBright µAI**, used *instead of* the board's own CodeBlock platform / MaixPy stack.
The motivation is full, direct control over the hardware — camera, audio, screen, GPIO,
and ideally the NPU — at the syscall/ioctl level, so projects are not boxed in by what
the vendor's library or the CodeBlock block modules expose. Each capability becomes a
small, self-contained C tool/lib (the existing `fbtest`/`v4l2cap` probes are the first
steps: screen and camera).

If it works, the intent is to **publish it on GitHub** as a low-level-control toolkit for
others working with this board. That implies a portability bar beyond "works on my Mac":
the cross-toolchain setup, board assumptions, and any vendor blobs need to be documented
and reproducible (see the toolchain notes below).

## kbdk — the dev-kit platform (Rust + Python + board runner)

The repo's second layer (branch `kbdk-platform`): fine-tune a model on the Mac,
convert it, push it over ADB, and run it live on the board's camera. Spec:
`docs/superpowers/specs/2026-06-10-devkit-platform-design.md`; plan:
`docs/superpowers/plans/2026-06-10-kbdk-platform-phase1-5.md`. **End-to-end verified
on hardware 2026-06-10**: toy 3-class dataset → MobileNetV2 fine-tune (MPS) →
int8 ncnn pack → `kbdk deploy/run` → 100% correct on held-out images on the board
(470 ms/inf @224), live camera + label overlay streaming JSON results.

```sh
cargo build                       # crates/kbdk-core (lib) + kbdk-cli (kbdk) + kbdk-ui (egui app)
sh board/ncnn/build.sh            # one-time: pinned ncnn -> board lib + host quantize tools
make kbrun                        # board runner (static libncnn + dlopen'd MPP camera)
make nna-runner                   # GPL NVDLA job executor (kbrun spawns it for nvdla packs)
(cd py && uv sync)                # Python workspace: kbdk-train + kbdk-convert

kbdk devices                      # adb serial + console port
kbdk exec "uname -a"              # sentinel-wrapped exec, real rc, adb or serial
kbdk train   --data DIR --out models/m.pt      # ImageFolder -> TorchScript (MPS)
kbdk convert --model models/m.pt --data DIR --name NAME   # -> packs/NAME (int8 ncnn)
kbdk deploy packs/NAME            # md5-verified push to /mnt/UDISK/kbdk/NAME + runner
kbdk run NAME [--frames N]        # live camera + overlay; kbdk log / kbdk stop

# NPU (NVDLA) path: conv-only backbone -> int8 job -> runs on the V831's NPU
kbdk train --data DIR --out models/m.pt --backbone npu_slim --size 64
uv run --project py python -m kbdk_convert.nvdla_compile \
    --model models/m.pt --data DIR --name NAME            # -> packs/NAME (runtime nvdla)
# then kbdk deploy/run as usual; scripts/nvdla_{parity,verify}.py = hardware checks
KBDK_HW=1 cargo test -p kbdk-core # hardware integration tests (board attached)
cargo run -p kbdk-ui              # desktop app: Train/Convert/Deploy tabs (egui, Catppuccin Mocha)
```

`kbdk-ui` notes: all blocking work (uv subprocesses via `kbdk-core::pipeline`,
board I/O, device polling) runs on worker threads reporting over mpsc; the UI
thread only renders. Field state persists via eframe Storage. The Deploy & Run
tab shows the **live board camera at ~10 fps over TCP**: kbrun serves the latest
240×240 KBF1 frame on port 18902 (one-shot per connection — wait for a *new* frame,
send, close — which paces the host loop), reached via `adb forward`
(`kbdk-core::frames::FrameStream`; forward removed on drop). kbrun also still drops
the KBF1 file into /tmp every 6th frame, and the UI poller falls back to adb-pulling
it (~2.5 fps) if the TCP fetch fails (older kbrun / no forward). Don't raise the TCP
cadence past every-3rd-frame: the adbd transfer competes with inference on the single
A7 (15 fps about doubles forward() wall time; at 10 fps it's ~640→950 ms, and only
while a client is fetching — idle cost is unchanged). The tab also has **dataset
capture**: 📷 Capture / burst save frames as PNGs into
`<dataset>/<class>/` ImageFolder layout for the Train tab. Verification hooks
(used by automated checks, harmless otherwise): `--screenshot PATH` (self-captures
the viewport — no macOS screen-recording permission needed), `--tab train|convert|deploy`,
`KBDK_SHOT_DELAY=secs`, `KBDK_AUTOTRAIN=1`, `KBDK_AUTOCONVERT=1`, `KBDK_POLL=1`
(start the board log poller as if Run was clicked). egui 0.34 gotchas already
handled: `App::ui` + `Panel::*::show_inside` (not `update`/`show`), egui_plot 0.35
pairs with egui 0.34, persistence needs the eframe `persistence` feature, and
`TextEdit` inside `Grid` collapses to minimum width (use horizontal rows +
`add_sized` labels instead — see convert_tab.rs).

Hard-won facts baked into kbdk (don't re-learn these):
- **AWNN can't run vanilla ncnn**: `libmaix_nn.so` loads a vanilla .param/.bin without
  error then forwards to saturated int8 garbage (needs proprietary quantize keys
  `20=/21=/22=/-23328/-23329`; the ncnn→AWNN converter is MaixHub-online-only).
  Proven with `make nnload`. Hence kbdk ships its own runner on **pinned vanilla ncnn**
  (`board/ncnn/build.sh`, commit b16501a, musl-armhf static, no vulkan/omp/threads).
- **Model packs must be int8**: fp32 ResNet18 (47 MB weights) is OOM-killed on the
  64 MB board. Measured @224: MobileNetV2 int8 466 ms / 15 MB RSS (the default);
  ResNet18 int8 *runs* but ncnn's armv7 int8 requantize path is pathological
  (4.6–6.1 s/inf) — don't default to ResNet. AWNN's own resnet18 does ~80 ms, so
  there's kernel headroom; possible future NVDLA/tuning work.
- **musl 1.1.16 shims in kbrun.cpp**: `__fstat_time64` (ncnn's file mapping reads only
  st_size; musl 1.2's arm stat = 1.1 layout with 64-bit times appended), plus
  `secure_getenv`/`getentropy` for static libstdc++. Check new board binaries with
  `nm -D -u` for `*_time64` leaks before shipping.
- **adbd kills the whole process group on session close** (no setsid/nohup applet):
  backgrounding via shell cannot survive. `kbrun` self-daemonizes (KBRUN_DAEMON=1 →
  fork + setsid synced via pipe + writes /tmp/kbrun.pid).
- **/mnt/UDISK (= mmcblk0p4 vfat, also /root) corrupts**: I/O errors flip it to
  read-only (`errors=remount-ro`). Recovery: `umount /root /mnt/UDISK; fsck.fat -a
  /dev/mmcblk0p4; mount` (both umounts succeed). kbdk md5-verifies every push ×3.
- Pack manifest keys are `in_blob`/`out_blob`/`labels_file` (not `input`/`labels`) so
  the board's flat JSON parser (`board/runner/manifest.h`) finds them by unique key.
- **kbrun has two engines**, picked by the manifest's `runtime` field: `ncnn` (static,
  for self-converted packs) and `awnn` (dlopen's the board's libmaix_nn — for the stock
  `/home/model/*` vendor models, which vanilla ncnn can't run and vice versa).
  `board-models/imagenet-resnet18/` is such a pack: manifest + labels only; `files.param`/
  `files.bin` are absolute board paths that deploy skips pushing. AWNN measured ~60–70 ms
  /inf @224 (ImageNet-1000). **AWNN's atexit handlers segfault** (ion phy2vir spam) once a
  model was loaded — kbrun ends with `_exit()` after its own MPP/fb cleanup to skip them.
  AWNN packs need labels for the class count (labels_file fallback is in kbrun + scan_packs).
- Training/convert/runner all share `(x−127.5)×0.0078125` ([−1,1]) normalization —
  changing one side silently breaks accuracy.
- **NVDLA runtime (`runtime: "nvdla"`) — models on the actual NPU.** kbrun stays MIT
  and spawns the GPLv3 `/tmp/nna_runner` (built from `board/nvdla/` + `third_party/
  v831-npu`, `make nna-runner`) in "serve" mode: `go\n` per frame over a pipe, packed
  int8 input/output cubes through tmpfs files, weights + NPU mapping stay resident.
  Compiler = `kbdk_convert.nvdla` (+ `nvdla_compile`): BN-fold → per-tensor int8 PTQ
  (calibrated on the dataset) → fused CONV→SDP[→PDP] layers in an NVJ1 job file.
  **Semantics are pinned byte-exact vs hardware** (scripts/nvdla_parity.py, 12+ configs):
  feature cubes = 8-ch surfaces ordered C′→W→H→surface; DC weights = kernel groups
  of 8, spatial-major, 1×1×C cubes (C≤32 — the >32 channel-group ordering is
  UNVERIFIED, so npu_slim keeps every conv's in_c ≤ 32); SDP out-cvt =
  `(acc + bias<<sh) * scale >> truncate` with **round half AWAY from zero**; PDP
  floor-mode pools need negative-pad clamping; >32-channel kernels = chunk-major
  channel groups of 32 (verified in_c 40/48/64); 1×1 kernels work. `npu_slim`
  (conv/BN/relu/maxpool, 64×64) measured **1.9 ms/inf on the NPU vs ~10 ms same-net
  int8 ncnn on CPU** — inference outruns the 30 fps camera (infers ≈ frames in
  kbrun). The toy pack is byte-exact host-sim↔board on 12/12 images. Depthwise is
  NOT supported by nv_small — MobileNet-style nets can't go on the NPU; that's why
  npu_slim exists.
- **Detection on the NPU**: `kbdk train --task detection --backbone npu_slim --size 112`
  trains `npu_det` (conv-only YOLOv2, 112×112 → 7×7 grid, 1×1-conv head to
  A·(5+C)); the same `nvdla_compile` invocation detects the `.meta.json` sidecar
  and emits a detection pack (manifest `detection` object + nvdla keys). kbrun's
  nvdla engine unpacks the raw map cube into an `ncnn::Mat` and reuses
  `decode_dets` — board boxes == host boxes. Measured **3.0 ms/inf** (vs ~560–720 ms
  for the mbv2 CPU detection); live detection keeps camera rate (~30 infers/s vs
  ~1.4). Verified end-to-end: byte-exact maps 12/12, identical boxes 15/15, and the
  synthetic-trained model boxing a real red ball live in the UI.
  `scripts/nvdla_verify_det.py` is the hardware check.
- The camera's all-black glitch frames are avgY≈0; kbrun gates `avgY<8` (a dim room
  sits in the teens and must still classify).
- **Detection (YOLOv2-slim)**: `kbdk train --task detection` takes a YOLO/Darknet
  dataset (`images/`, `labels/*.txt` `cls cx cy w h` normalized, `classes.txt` — the
  labelImg/Roboflow export format). Model = MobileNetV2 features + Conv head →
  raw (A·(5+C))×7×7 map (pure convs → pnnx/int8 clean); anchors are k-means'd from
  the dataset and travel in a `model.pt.meta.json` sidecar → manifest `detection`
  object {grid, anchors, conf_threshold, nms_threshold}. Decode (sigmoid/exp/softmax
  + per-class NMS) runs on CPU, implemented twice and kept in sync: python
  `kbdk_train.detect.decode_boxes`/`nms` (eval + int8 parity) and kbrun's
  `decode_dets`. Board↔host box parity verified near-exact; ~560 ms/inf standalone,
  ~720 ms with live preview. The panel and the net see the *same centre square*, so
  normalized boxes map 1:1 onto fb0 and the UI preview. Toy generator:
  `examples/make_toy_detection.py` (trained 60 epochs → det_rate 1.0; generalized to
  a real photographed red ball at 91%).

## Build & run

```sh
make                 # bin/uai (host) + bin/hello + bin/fbtest (cross-compiled)
make uai             # just the host serial tool
make hello           # cross-compile one board program (also: fbtest, v4l2cap)
make deploy          # build hello, push to /tmp/hello, chmod +x, run it
make deploy-fb       # same for fbtest
make monitor         # read-only serial console with timestamps
make term            # interactive serial shell (Ctrl-] to quit)
make clean
```

The cross toolchain (`arm-unknown-linux-musleabihf-gcc`) comes from
`brew install messense/macos-cross-toolchains/arm-unknown-linux-musleabihf` and needs
`/opt/homebrew/bin` on PATH. Board binaries **must** be built with
`-mcpu=cortex-a7 -mfpu=neon-vfpv4 -mfloat-abi=hard` (see `CROSSFLAGS`); the wrong
float ABI silently produces binaries that fault on the board.

`v4l2probe.c` has no Makefile target yet — add one mirroring `v4l2cap` if you need it.
There is also a stray root `test.c` (plain hello-world) that nothing builds.

## Toolchain & capability coverage

The Mac has the full cross toolchain to deliver the core goal: **GCC 15.2.0**
(`arm-unknown-linux-musleabihf-`, C/C++/Fortran), gdb, full binutils, and a musl sysroot
(`-print-sysroot`) with `libc`/`libstdc++`/`libgcc`/`libatomic`/`libgomp`. musl folds
`libm` into libc, so `-lm` works. The sysroot ships the kernel UAPI headers that matter
for direct hardware control: `linux/videodev2.h`, `linux/fb.h`, `linux/i2c-dev.h`,
`linux/spi/spidev.h`, `sound/asound.h`, `linux/input.h`.

Capability status against the goal (all "raw ioctl/mmap, no vendor lib" except where noted):

| Capability | Status | Path |
| --- | --- | --- |
| **Screen** (framebuffer) | ✅ done | `fbtest.c` — `FBIOGET_*` + mmap. Self-contained. |
| **Camera** | ✅ live preview on panel (raw + colour-corrected) | Standard V4L2 is unavailable here (`QBUF`/`DQBUF`/`QUERYBUF` ENOTTY — see `camdiag.c`); capture goes through Allwinner **MPP**. `cammpp.c` pulls NV21M frames (Y+VU phys/virt addrs, for processing); `campreview.c` shows the live camera on the 240×240 LCD via `AW_MPI_SYS_Bind(VI→VO)` (zero-copy hardware path); `camcc.c` is the colour-corrected CPU-path preview (MPP capture → per-pixel white-balance/saturation → `/dev/fb0`). All dlopen the board's `.so`; headers vendored under `vendor/eyesee-mpp/`. Raw colour is ISP-3A-limited (green/flat: near-white pixels converge to **U≈119 / V≈139** vs neutral 128, saturation ≈16); the proper fix is baked in `libisp.so`, but `camcc` neutralises it in software (U+9 / V−11, sat×1.6 — see the `camcc.c` note below). |
| **GPIO / I²C / SPI / buttons** | ✅ ready | UAPI headers present; same raw-ioctl approach. Not yet written. |
| **Audio** | ✅ working | `audio.c` — raw `SNDRV_PCM_IOCTL_*` on `/dev/snd/pcmC0D0{p,c}`, no alsa-lib. `probe`/`tone`/`play`/`rec`, all verified on hardware. Codec: playback 1–2 ch, capture mono-only, S16_LE/S24_LE, 8k–192k Hz. |
| **NPU** (the "µAI") | ✅ CNN inference runs **on the NPU** from userspace (no kernel driver) | The V831 NPU is a customised **NVIDIA NVDLA `nv_small`** core. **Two working paths.** (1) *CPU/AWNN*: the board ships `libmaix_nn.so` (AWNN, a quantized-**ncnn** fork; `.param`+`.bin`, `7767517` magic); `nncls.c` dlopen's it and runs `fe_res18` (~31 ms/inf) — but it `open(/dev/nna)`-fails and falls back to **CPU** because this rootfs's kernel was built **without `CONFIG_SUNXI_NNA`** (no `nna_sunxi.ko`; DT node `nna@02400000` *is* present + `okay`). (2) *NPU/userspace*: drive the NVDLA core directly via `/dev/mem` (regs `0x2400000`, CCU `0x3001000`) + `/dev/ion` + `/dev/cedar_dev` — **no kernel module needed**. `make nnaprobe` proves the regs respond; `make nna-cifar10` runs a 4-conv CNN on the NPU (classifies the test image "ship : 127", verified). Built from vendored **GPLv3** `third_party/v831-npu/` (mtx512). See `docs/superpowers/specs/2026-06-08-npu-cnn-runtime-design.md`. **kbdk now compiles trained models to the NPU** (`runtime: "nvdla"` packs — see the NVDLA runtime note in the kbdk facts list): npu_slim @64² = 1.9 ms/inf, byte-exact vs the host int8 simulation. |

**Portability gaps to fix before publishing** (currently macOS-host-specific):
- The toolchain is the third-party Homebrew tap `messense/macos-cross-toolchains` — Linux
  contributors need an equivalent musl-armhf cross compiler; document the alternative.
- `uai.c`'s `host_md5()` shells out to macOS `/sbin/md5`; Linux hosts have `md5sum` instead.
- The Makefile hardcodes the macOS cross-compiler triple.

## Running things on the board with `uai`

`./bin/uai [-p PORT] [-b BAUD] <cmd>` is the only channel to the board. It is both
human-interactive and scriptable — `exec`/`run`/`deploy` print clean stdout and exit
with the **board command's** return code, so they compose in shell pipelines.

```sh
./bin/uai exec "uname -a"            # run a shell command, propagate its rc
./bin/uai push LOCAL /tmp/x          # upload + verify (size + md5)
./bin/uai deploy bin/fbtest /tmp/fbtest   # push + chmod +x + run
./bin/uai monitor -t                 # watch boot/runtime logs live
```

Defaults `-p /dev/cu.usbserial-210 -b 115200`, overridable via `-p`/`-b` or env
`UAI_PORT`/`UAI_BAUD`. **Only one process can hold the serial port at a time** — you
cannot `monitor` and `exec` simultaneously, and a leftover `uai` holding the port shows
up as `exec` timeouts (kill it).

### Two design constraints worth knowing before editing `uai.c` or board code

- **No `base64` on the board.** BusyBox here lacks the applet, so `push` encodes each
  byte as an octal `\NNN` escape and appends it with `printf >> file` in ~45-byte
  chunks. This is ~4 wire bytes per payload byte and slow for anything large; it is
  fine for the small test binaries here. If you need speed, add `xxd`/`uudecode` to
  the rootfs and change the transfer path.
- **Exit-code capture via quote-split sentinels.** `board_exec` wraps every command as
  `echo 'UAI''_BEGIN'; <cmd>; echo 'UAI''_END_'$?`. The quote-split (`'UAI''_BEGIN'`)
  means the board's *command echo* never contains the literal marker token — only the
  real printed output does — so output parsing isn't fooled by the echoed command line.
  Consequence: a command must not **end** in a bare `&`; to background, write
  `"(cmd) & true"`. The same trick lives in `scripts/serial_transfer_run.py`, the
  original pyserial implementation kept only as reference/fallback.

## Hardware inventory

Read live off the board (`uname`, `/proc`, `i2cdetect`, sysfs) unless marked otherwise.
Re-probe with `./bin/uai exec "..."` rather than trusting this if a board revision differs.

| Subsystem | Detail |
| --- | --- |
| **SoC** | Allwinner **V831** (`sun8iw19`), single-core ARM **Cortex-A7** (impl `0x41` part `0xc07` rev 5), armv7l, hard-float NEON + VFPv3/VFPv4, IDIVA/IDIVT, LPAE |
| **NPU** | V831 has an on-die ~0.2-TOPS NPU (the "µAI" / MaixPy angle) — *chip spec, not separately probed* |
| **RAM** | ~64 MB (`MemTotal` 60048 kB, in-package DDR) |
| **Kernel** | Linux **4.9.118** PREEMPT, musl/BusyBox (Tina), hostname `sipeed` |
| **Storage** | `mmcblk0` ~58 GB (microSD/eMMC), GPT 4 parts: p1 256 KB boot, p2 4 MB, p3 416 MB rootfs, p4 57.8 GB data. No NAND/MTD. |
| **Display** | `/dev/fb0` = **240×240 RGB888** panel (mode `240x240p-126`), virtual 240×480 → double-buffered. fb1–fb7 are extra sunxi-disp planes (unused). `pwmchip0` for backlight/buzzer. |
| **Camera** | **OV2685** 2 MP, MIPI CSI via sunxi-VIN, driver `ov2685_mipi` bound at **I²C-1 0x3c**, node `/dev/video0` (`video1` = 2nd VIN node). Drivers `ov7251_mipi`/`ov9732_mipi`/`sp2305_mipi` also loaded but unconnected (BSP supports swapping the sensor). |
| **Wi-Fi** | **Realtek RTL8189FS** SDIO 802.11 b/g/n, driver `8189fs`, interfaces `wlan0` + `wlan1`. No Ethernet. |
| **Audio** | One ALSA card, playback `pcmC0D0p` + capture `pcmC0D0c` → built-in codec (speaker out + mic in). |
| **I²C** | `/dev/i2c-1`: camera @0x3c (bound) + unidentified devices at 0x48, 0x51, 0x62 (no driver attached — likely RTC + a sensor). `/dev/i2c-2`: present, empty. |
| **SPI** | `/dev/spidev1.0` |
| **Analog in** | `sunxi-gpadc0` GPADC, exposed as input `event0` (board buttons / light input) |
| **Console** | UART 115200 8N1 — the only host link (how `uai` reaches the board) |

The 0x48 / 0x51 / 0x62 I²C parts have no kernel driver bound, so their exact part numbers
are unconfirmed; a device-tree dump (`/proc/device-tree`, pipe through `tr -d '\0'` over serial)
would name them definitively.

## Board-side programs (hardware notes)

These are diagnostic probes for the V831's display and camera; they encode hardware
facts learned by trial:

- `fbtest.c` — derives pixel packing from the framebuffer's `var` bitfields at runtime,
  so it is correct for any RGB/BGR order or bpp. The panel is 240×240 RGB888.
- `audio.c` — talks to the codec through the raw kernel ALSA ioctl ABI (no alsa-lib).
  The fiddly part is `struct snd_pcm_hw_params`: it is a flat array of masks (access,
  format) + intervals (rate, channels, period/buffer), set to "any" then refined by
  `SNDRV_PCM_IOCTL_HW_PARAMS`, which collapses every interval to the chosen value
  (read back via `->min`). `pcm_setup` retries with the other channel count when the
  codec refuses one (capture is mono-only). Opened blocking, so `WRITEI`/`READI` pace
  themselves; `EPIPE` (xrun) is handled by re-`PREPARE`. Build needs `-lm` (sine gen).
- `v4l2probe.c` / `v4l2cap.c` — the camera is an OV2685 behind the Allwinner sunxi-vin
  pipeline on `/dev/video0` (a **multi-planar** node; `capabilities=0x85201000` =
  STREAMING|READWRITE|MPLANE, though per-node `device_caps` under-reports to `0x00200000`).
  **Capture via open V4L2 is a dead end on this BSP** (proven by `camdiag.c`/`camread.c`):
  `S_FMT`+`REQBUFS(MPLANE,MMAP)` succeed but `QUERYBUF`/`QBUF`/`DQBUF` are ENOTTY and
  `read()` returns no frame, so buffers can never be filled. Allwinner's MPP middleware
  (`libmpp_vi`, `libISP`, `libMemAdapter`/ION, `g2d`) owns the real capture path — see the
  camera row in the capability table. `camdiag.c` (buffer-ABI matrix) and `camread.c`
  (read() probe) are the diagnostics that established this; both stop before/around
  STREAMON and are hang-safe. Any future camera work goes through MPP, not `/dev/video0`.
- `cammpp.c` — the **working** camera capture, via Allwinner MPP. Sequence (from the
  sun8iw19p1 `sample_virvi`): `AW_MPI_SYS_SetConf`(nAlignWidth=32)`/SYS_Init` →
  `ISP_Init` + `ISP_Run(0)` **on its own thread** (Run hosts the 3A loop) →
  `VI_CreateVipp(0)`/`SetVippAttr`(NV21M, MPLANE, MMAP, nbufs=5, nplanes=2)/`EnableVipp`
  → `CreateVirChn(0,0,NULL)`/`EnableVirChn` → `GetFrame` (NV21M: `mpVirAddr[0]`=Y,
  `[1]`=VU, plus `mPhyAddr[*]` for zero-copy to g2d/VO) → `ReleaseFrame` → teardown +
  `SYS_Exit`. Build/run specifics that are easy to get wrong:
  - **Headers** are vendored under `vendor/eyesee-mpp/sun8iw19p1/include/` (the V831 =
    sun8iw19p1 SDK). Build needs `-DAWCHIP=0x1817` (cosmetic; satisfies `plat_defines.h`)
    and the three `-I` paths — see `MPPINC`/`MPPDEF` in the Makefile (`make cammpp`).
  - **No link against vendor libs**: symbols are `dlopen`/`dlsym`'d at runtime from
    `/usr/lib/eyesee-mpp/{libmedia_mpp,libmpp_isp,libmpp_vi}.so` (SYS/ISP/VI live in
    those respectively). So **run with** `LD_LIBRARY_PATH=/usr/lib/eyesee-mpp:/usr/lib`.
  - **musl time64 trap**: the board is musl **1.1.16**; the cross toolchain (musl 1.2+)
    redirects `dlsym`→`__dlsym_time64`, which the board lacks → "symbol not found" at
    load. Bind the plain symbol with `extern void *aw_dlsym(...) __asm__("dlsym");`.
    The same trap hits any time64-redirected libc call (e.g. `select`); prefer `poll`.
  - MPP needs the on-board sensor/ISP config (present because the vendor stack uses it);
    `SYS_Init` also spins up cedarx/VO/ALSA (verbose logs) but, run directly like this,
    does **not** SIGILL the way MaixPy's Python init does.
  - **Frame dump**: a 3rd arg (`cammpp WxH nframes DUMPFILE`) writes one well-exposed
    converged frame as contiguous NV21 (Y then VU, **not** stride-packed — the planes
    are contiguous and `mStride` here trips a bus error). The sensor emits occasional
    all-black frames (avg Y≈0), so the dump skips frames whose sampled avg Y is outside
    30..220 and overwrites, so the last good frame wins. Pull it host-side by gzipping on
    the board and `hexdump`-ing in ≤18 KB chunks (one full-frame hex stream overruns
    `uai exec`'s 15 s timeout); decode with `captures/nv21.py`.
- `camcc.c` — **colour-corrected live preview** (the CPU alternative to `campreview`'s
  zero-copy path). Captures NV21 via the same MPP VI+ISP sequence as `cammpp.c` (no VO),
  converts to RGB with a white-balance + saturation correction, and blits to `/dev/fb0`.
  Key facts learned building it:
  - **fb0 stays visible without VO.** `campreview` has to *hide* the UI/fb layer
    (`HLAY(2,0)`) to show its VO video layer; conversely, if you bring up **no** VO
    layer, MPP leaves the fb/UI layer visible and plain `/dev/fb0` writes show through.
  - **fb format**: 240×240, 24 bpp, `line_length=720`, byte order **B,G,R**
    (blue.offset=0, green=8, red=16); virtual 240×480 = two pages.
  - **No page-flipping.** Panning between the two virtual pages with `FBIOPAN_DISPLAY`
    every frame switched the displayed page mid-scan → the image **tore and rolled down**.
    Fix: pin `yoffset=0` and never pan; write each frame into that one page **after**
    blocking on `FBIO_WAITFORVSYNC` (the sunxi-disp driver supports it; reported "vsync on").
  - **Colour correction** (BT.601 full-range, saturation folded into the matrix coeffs,
    fixed-point `<<8`): neutralise the illuminant `U += +9`, `V += −11`, then `chroma ×1.6`.
    All of `uoff voff sat` plus a `flip` (180° rotate) are runtime args: `WxH secs uoff voff sat flip`.
  - **Orientation**: the OV2685 raw output is already **upright** for how the board is
    normally held → `flip=0` is the default (an earlier `flip=1` came from a mis-oriented
    test capture and is wrong). ~30 fps; integer conversion keeps up on the single A7.
  - **Verify colour/orientation remotely** by reading back the panel itself: while running,
    `dd if=/dev/fb0 bs=720 count=240 | gzip`, pull chunked, and decode the BGR with the
    same approach as `captures/nv21.py` (skip small/uniform grabs — they're black frames).
- `nncls.c` — **CNN inference** via the board's `libmaix_nn.so` (AWNN). dlopen's the lib
  (no link), loads `fe_res18_117` (128×128×3 → 256-float embedding), runs forward, and
  checks determinism/variance with `libmaix_nn_feature_compare_float`. `make nncls`,
  `make deploy-nncls`; run with `LD_LIBRARY_PATH=/usr/lib/eyesee-mpp:/usr/lib`. Input is a
  raw RGB888 file (`128*128*3` bytes); make one from a capture with `captures/ppm2raw.py`.
  Build/run facts that bite:
  - **Models are quantized ncnn** (`.param` text topology starting with `7767517` +
    `.bin` int8 weights). Pass the **blob** name, not the layer name — for `fe_res18` the
    input layer is `inputs` but its blob is `inputs_blob` (the layer-name guess segfaults
    in `awnn_quantize`/`getTensorScale`). Output blob is `FC_blob`.
  - **Preprocess** is HWC `uint8` with `mean=[127.5×3]`, `norm=[0.0078125×3]` and the
    input layer's `need_quantization=true` (AWNN quantizes from the `.param` scales). The
    NPU/engine runs the backbone; **decoders + argmax/softmax run on the CPU**.
  - **musl binds immediately** (RTLD_LAZY is a no-op), and `libmaix_nn.so` carries two
    undefined retinaface decoder back-refs (`retinaface_get_priorboxes`,
    `retinaface_decode`). We never use that decoder, so export harmless stubs from the
    binary via `-Wl,--export-dynamic` for the loader to bind.
  - **Timing** uses `/proc/uptime` (averaged over N) to dodge the musl time64 trap that
    hits `clock_gettime`/`gettimeofday`.
  - **NPU vs CPU**: `nncls`/AWNN runs on **CPU int8 (NEON)** — ~31 ms/inf, ~9 MB RSS for
    ResNet18@128² — because `open(/dev/nna)` fails (kernel built without `CONFIG_SUNXI_NNA`).
    The **NPU itself works via the userspace path below**; AWNN just can't reach it without
    its driver.
- `nnaprobe.c` — **read-only NPU bring-up probe** (clean-room, no driver). Maps `/dev/mem`,
  runs the documented CCU NNA clock/power sequence (`ccu[440]`=clk-select, `ccu[443]`=gate,
  base `0x03001000`), maps the NVDLA register window (`0x02400000`), and reads its config
  regs (SIGBUS-guarded so a wrong clock reports instead of hanging). Confirmed the regs are
  live (the DT's `assigned-clocks` power the NNA at boot). `make nnaprobe` / `make deploy-nnaprobe`.
- **`third_party/v831-npu/`** (GPLv3, mtx512) + `nna-cifar10` — **CNN inference on the NPU
  with no kernel driver**. The V831 NPU is an **NVIDIA NVDLA `nv_small`** core; this code
  drives it directly from userspace via `/dev/mem` (regs `0x2400000` + CCU `0x3001000`) +
  `/dev/ion` + `/dev/cedar_dev` (`AW_MEM_GET_IOMMU_ADDR` for the IOMMU/phys addr the NVDLA
  needs; cache flush via `ION_IOC_SUNXI_FLUSH_RANGE`). `make nna-cifar10` /
  `make deploy-nna-cifar10` cross-build the CIFAR-10 example for musl-armhf and it runs on
  the NPU (classifies the test image "ship : 127", verified). **No `LD_LIBRARY_PATH`**, no
  vendor `.so`. Gotchas: needs root (`/dev/mem`); buffers are cached so flush around every
  CPU↔buffer copy; all NPU address fields take the **physical/IOMMU** address from cedar,
  not the mmap'd virtual one. Note this is **GPLv3** (isolated in `third_party/`) — keep the
  licence in mind before folding it into a permissively-licensed publish. Open gap: no open
  compiler turns an arbitrary trained model into NVDLA descriptors (weights are hand-built /
  baked-in here).
- `nnacam.cpp` — **live camera → ImageNet classification, on the LCD**. Combines the MPP NV21
  capture (dlopen'd, same VI+ISP path as `cammpp.c`) with a **ResNet18 ImageNet-1000 classifier**
  run through the board's **AWNN** runtime (`libmaix_nn.so`, dlopen'd — same path as `nncls.c`),
  all in one process on the 240×240 panel: each frame is gray-world colour-corrected and blitted
  to `/dev/fb0` (live preview, vsync-gated like `camcc.c`), centre-cropped + scaled to 224×224 for
  the net, and the predicted **label + softmax% is drawn on the panel** via a built-in 8×8 bitmap
  font (no text lib on the board). `make nnacam` / `make deploy-nnacam`; run with
  `LD_LIBRARY_PATH=/usr/lib/eyesee-mpp:/usr/lib /tmp/nnacam [WxH] [nframes] [sat flip]`
  (nframes 0 = until SIGTERM). Measured: **~21.8 MB peak RSS (`VmHWM`)**, preview ~25–30 fps.
  Key facts:
  - **Inference on a background thread.** ResNet18@224 is ~80 ms on the single A7; running it inline
    would stall the preview. A worker thread classifies the *latest* frame (main hands it off via a
    mutex+cond, non-blocking `trylock`, latest-wins) and the preview loop just draws the most recent
    label → ~12 inf/s while the camera stays smooth. The worker copies the input under lock then runs
    `forward()` lock-free so the main loop never blocks on it.
  - **AWNN, on CPU here.** Model `/home/model/resnet18_1000_awnn.{param,bin}` (11.7 MB int8),
    blobs `input0`/`output0`, input 224×224×3 HWC UINT8 `need_quantization=true`, `mean=127.5`
    `norm=0.0078125`, output 1000 floats → CPU argmax+softmax. Runs on **CPU** because the NPU
    driver is absent (see NPU row); loading `nna_sunxi.ko` moves it onto the NPU with no code change.
    Needs `-Wl,--export-dynamic` for the retinaface back-ref stubs (same as `nncls`).
  - **Labels**: `models/imagenet1000_labels.txt` (PyTorch/standard ImageNet order, matches the
    model's training order — confirmed against libmaix `nn_resnet`'s baked list). **Pushed to
    `/tmp`, not `/home/model`**: writing the labels to the rootfs came back as all-NUL despite uai's
    md5 reporting OK (flaky rootfs storage — cf. the `/root` FSCK/FOUND.000 artifacts), so deploy
    targets tmpfs and the content is verified directly, not just by the reported md5.
  - **Colour: gray-world auto white balance** (`update_awb`), same approach as before — fixed `U/V`
    offsets can't neutralise the ISP's green cast (offsets only shift R via V / B via U; a `V−` trim
    crushes red → green-washed). Per-channel gains equalise the frame averages (gains `<<8`, clamped
    0.4×–2.5×, EMA ~8-frame); the **same gains feed the 224×224 net input** so the classifier sees
    the neutral colour it was trained on. Verify via the fb0-readback one-liner
    (`dd if=/dev/fb0 … | hexdump … | awk` averages).
  - ImageNet has 1000 fine-grained classes → point a well-lit, clear subject at the lens for
    meaningful labels (dim/cluttered scenes give low-confidence guesses). The earlier NVDLA/CIFAR-10
    version of nnacam is in git history; the NVDLA userspace path still ships as `make nna-cifar10`.
