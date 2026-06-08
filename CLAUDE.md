# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## What this is

A host-side toolkit for the **KidBright µAI** board (Allwinner V831 — single-core
ARM Cortex-A7, armv7l hard-float NEON/VFPv4, Linux 4.9 BusyBox/Tina, musl libc).
There is no network link to the board: everything happens over the serial console.
Two kinds of code live here, compiled by two different compilers:

- **Host tools** (`src/uai.c`) — built native for the macOS arm64 host with `cc`.
- **Board binaries** (`src/hello.c`, `src/fbtest.c`, `src/v4l2cap.c`,
  `src/v4l2probe.c`) — cross-compiled for the V831, then pushed and run on the board.

The central workflow is always: cross-compile on the Mac → transfer over serial via
`uai` → execute on the board → read stdout/exit-code back over the same serial line.

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
| **NPU** (the "µAI") | ⚠️ CNN inference works **on CPU**; NPU silicon not driveable on this image | The CNN runtime is **not** a gap: the board ships `libmaix_nn.so` (the **AWNN** engine — a quantized-**ncnn** fork; models are `.param`+`.bin` with the `7767517` ncnn magic). `nncls.c` dlopen's it (no link, like `cammpp.c`), loads `fe_res18_117` (128×128×3 → 256-float embedding) and runs forward (~31 ms/inf). **But the NPU hardware has no driver on this rootfs**: `open(/dev/nna)` fails, no `nna`/`npu` `.ko` exists, nothing in `/proc/devices`. AWNN falls back to **CPU int8 (NEON)**, so inference is functional but not accelerated. Driving the ~0.2-TOPS NPU needs the V831 `nna` kernel module (absent here) — the real remaining research item. See `docs/superpowers/specs/2026-06-08-npu-cnn-runtime-design.md`. |

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
  - **NPU vs CPU**: `open(/dev/nna)` fails because this rootfs has **no NPU driver**, so
    AWNN runs on **CPU int8 (NEON)** — ~31 ms/inf, ~9 MB RSS for ResNet18@128². Driving
    the actual NPU needs the V831 `nna` kernel module (not present). The next step (M2) is
    live VOC-20 YOLOv2 on the LCD (camera→forward→CPU yolo2 decode→fb0).
