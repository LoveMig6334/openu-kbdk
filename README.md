# kidbright-uai

Low-level C toolkit **and AI dev-kit platform (kbdk)** for the **KidBright µAI**
board (Allwinner V831, single-core ARM Cortex-A7, armv7l hard-float NEON/VFPv4,
Linux 4.9 BusyBox/Tina, musl libc).
Cross-compile C on the Mac, drive the board over USB (ADB gadget) or its serial
console, and talk to the hardware **directly** (syscall / ioctl / mmap, or the
vendor MPP API where the kernel forces it) — instead of going through the board's
CodeBlock / MaixPy stack. The goal is full control over the screen, audio, camera
(and eventually GPIO/NPU) for building AIoT projects without the vendor library's
limits.

## kbdk: train on the Mac → run live on the board

The `kbdk` platform (Rust CLI + Python pipeline + C++ board runner) fine-tunes an
image classifier and deploys it to the board's camera + LCD:

```sh
cargo build && (cd py && uv sync)         # host tools (Rust + Python via uv)
sh board/ncnn/build.sh && make kbrun      # pinned ncnn runtime + board runner

uv run --with pillow --with numpy python examples/make_toy_dataset.py
./target/debug/kbdk train   --data examples/toy-dataset --out models/toy3/model.pt
./target/debug/kbdk convert --model models/toy3/model.pt --data examples/toy-dataset --name toy3
./target/debug/kbdk deploy packs/toy3
./target/debug/kbdk run toy3              # live camera + label overlay on the panel
./target/debug/kbdk log && ./target/debug/kbdk stop
```

Prefer a GUI? `cargo run -p kbdk-ui` opens the desktop app (egui, Catppuccin
Mocha): pick a dataset, watch live loss/accuracy curves while training on MPS,
convert with an int8-parity check, then deploy and watch the board's live
camera + classifications stream in. The Deploy & Run tab also lists the
board's stock models (ImageNet ResNet-18 via the vendor AWNN runtime) and can
**capture frames from the board camera into ImageFolder datasets** (single
shot or burst) — point the camera at your objects, collect a dataset, train,
deploy, all without leaving the app.

Train = PyTorch MPS — classification (MobileNetV2/ResNet18 transfer learning,
ImageFolder datasets) **and object detection** (`--task detection`: YOLOv2-slim =
MobileNetV2 + YOLO head, YOLO/Darknet-format datasets as exported by
labelImg/Roboflow; anchors auto-k-means'd). Convert = pnnx → ncnn → int8
(calibrated on your dataset, host-verified parity — box-level for detection).
Detection runs live on the board with bounding boxes + labels drawn on the
panel and overlaid on the UI's camera preview.
Deploy = md5-verified push over the board's **ADB USB gadget** (~6 MB/s; serial
console fallback built in). Run = `kbrun`, statically-linked vanilla **ncnn**
(the vendor's AWNN runtime cannot run self-converted models — see CLAUDE.md),
MPP camera capture, gray-world AWB, fb0 preview + label overlay, JSON-lines
results over `kbdk log`. Measured: MobileNetV2-int8 @224 ≈ 470 ms/inf, preview
~27 fps concurrently.

## What works

| Capability | Tool | How |
| --- | --- | --- |
| **Screen** | `fbtest` | `/dev/fb0` (240×240 RGB888) via raw `ioctl`+`mmap` |
| **Audio** | `audio` | raw `SNDRV_PCM_IOCTL_*` on `/dev/snd` — no alsa-lib (`probe`/`tone`/`play`/`rec`) |
| **Camera capture** | `cammpp` | Allwinner **MPP** `AW_MPI_VI`+ISP → NV21M frame (standard V4L2 streaming is not exposed on this BSP); optional raw-frame dump for host inspection |
| **Live camera preview** | `campreview` | `AW_MPI_SYS_Bind(VI→VO)` — camera straight to the LCD, zero-copy in hardware (raw ISP colour) |
| **Colour-corrected preview** | `camcc` | MPP capture → CPU white-balance + saturation → `/dev/fb0`; fixes the green/flat ISP colour, ~30 fps, vsync-synced |
| GPIO / I²C / SPI | — | UAPI headers present; not yet written |
| NPU | — | needs Allwinner's proprietary runtime; research item |

## Board facts
- SoC **Allwinner V831** (`sun8iw19p1`), Cortex-A7 armv7l hard-float NEON/VFPv4,
  ~64 MB RAM, Linux **4.9** (Tina/BusyBox), **musl 1.1.16**
- Target triple `arm-linux-musleabihf` · loader `/lib/ld-musl-armhf.so.1`
- Serial console `/dev/cu.usbserial-210` @ **115200 8N1**, lands at a `root@sipeed` shell (no login)
- Panel `/dev/fb0` = **240×240 RGB888**; camera = **OV2685** (2 MP, MIPI) behind the sunxi-VIN pipeline
- busybox has **no `base64`** applet → file transfer uses octal `printf`

## Layout
```
kidbright-uai/
├── Makefile
├── CLAUDE.md                      # deep notes for the codebase (hardware, gotchas)
├── src/
│   ├── uai.c                      # host serial toolkit (native macOS arm64)
│   ├── hello.c                    # board smoke-test (printf + sqrt → hard-float)
│   ├── fbtest.c                   # framebuffer probe + test pattern (screen)
│   ├── audio.c                    # raw-ioctl ALSA: probe / tone / play / rec
│   ├── v4l2probe.c  v4l2cap.c     # V4L2 recon (proved standard streaming is unavailable)
│   ├── camdiag.c    camread.c     # V4L2 buffer-ABI + read() diagnostics
│   ├── cammpp.c                   # MPP camera capture (one NV21M frame; can dump raw to a file)
│   ├── campreview.c               # MPP live preview on the panel (VI→VO bind, raw colour)
│   └── camcc.c                    # colour-corrected live preview (MPP capture → CPU WB/sat → fb0)
├── captures/nv21.py               # host tool: decode dumped NV21 → PPM + colour stats (stdlib only)
├── vendor/eyesee-mpp/sun8iw19p1/  # vendored Allwinner MPP headers (V831 ABI) for the camera
├── scripts/serial_transfer_run.py # original pyserial transfer (reference/fallback)
└── bin/                           # build output (gitignored)
```

## Prerequisites
```sh
# cross toolchain for the board (one-time)
brew tap messense/macos-cross-toolchains
brew install arm-unknown-linux-musleabihf
```
`make` needs `/opt/homebrew/bin` on PATH so it can find the cross compiler.

## Build
```sh
make            # bin/uai (host) + bin/hello + bin/fbtest + bin/audio (board)
make uai        # just the host serial tool
make fbtest     # screen test    | make audio  # audio tool
make cammpp     # camera capture | make campreview  # live preview (raw colour)
make camcc      # colour-corrected live preview
make clean
```

## `uai` — the serial toolkit
One native tool, usable interactively **and** scriptably (the `exec`/`run`/`push`
commands print clean stdout and exit with the board command's return code).

```
uai [-p PORT] [-b BAUD] <command> [args]
  monitor [-t]         read-only console (-t prefixes timestamps)
  term                 interactive terminal (type + read); Ctrl-] to quit
  exec "CMD"           run a shell command on the board, print output, exit rc
  push LOCAL REMOTE    upload a file (octal-printf, size + md5 verified)
  run REMOTE [args]    execute REMOTE on the board, print output, exit rc
  deploy LOCAL REMOTE  push + chmod +x + run
```
Defaults: `-p /dev/cu.usbserial-210 -b 115200`, overridable with `-p`/`-b` or
env `UAI_PORT` / `UAI_BAUD`.

### Examples
```sh
./bin/uai exec "uname -a"                 # → Linux sipeed 4.9.118 ... armv7l
make deploy                               # build hello and run it on the V831
./bin/uai monitor -t                      # watch boot/runtime logs live
```

## Screen
```sh
make deploy-fb                            # build fbtest, push to the board
./bin/uai run /tmp/fbtest                 # paint colour bars + border + diagonal
```

## Audio
Raw kernel-ALSA (`SNDRV_PCM_IOCTL_*`); no alsa-lib. Codec: playback 1–2 ch,
capture mono, S16_LE, 8k–192k Hz.
```sh
make deploy-audio
./bin/uai exec "/tmp/audio probe p"       # show codec capabilities
./bin/uai exec "/tmp/audio tone 440 2"    # 2s sine on the speaker
./bin/uai exec "/tmp/audio rec /tmp/x.wav 3 && /tmp/audio play /tmp/x.wav"
```

## Camera
Standard V4L2 streaming is **not available** on this BSP — `/dev/video0` accepts
`S_FMT`/`REQBUFS` but `QBUF`/`DQBUF`/`QUERYBUF` return `ENOTTY`. Capture goes
through Allwinner's **MPP** middleware instead (`AW_MPI_VI`/ISP/VO). The vendored
`vendor/eyesee-mpp/` headers give the struct ABI; the actual symbols are
`dlopen`/`dlsym`'d from the board's `/usr/lib/eyesee-mpp/*.so` at runtime.

```sh
# grab one NV21M frame (Y+UV addresses, for processing) and print its stats
make deploy-cammpp
./bin/uai exec "LD_LIBRARY_PATH=/usr/lib/eyesee-mpp:/usr/lib /tmp/cammpp 320x240"

# live camera preview on the 240x240 panel (VI->VO hardware bind, raw ISP colour)
make preview-start                        # camera goes live, stays until stopped
make preview-stop                         # clean shutdown + unblank the panel
```

### Colour-corrected preview (`camcc`)
The hardware VI→VO path is zero-copy but shows the raw ISP colour, which on this
board is green/grey and washed out (neutrals land at **U≈119 / V≈139** instead of
128/128; saturation ≈16). `camcc` takes the CPU path instead — captures NV21 via
MPP, applies a white-balance + saturation correction per pixel, and blits BGR to
`/dev/fb0`. It brings up **no VO video layer**, so the fb/UI layer stays visible and
the writes show through; each frame is written into a single fixed page gated on
`FBIO_WAITFORVSYNC`, so the image is steady (no tearing/roll). ~30 fps on the single A7.

```sh
make camcc-start                          # corrected camera live, until stopped
make camcc-stop
```
Correction is tunable live without recompiling — args are `WxH secs uoff voff sat flip`:
```sh
# defaults: uoff=+9 voff=-11 sat=1.6 flip=0 (raw sensor orientation is upright)
./bin/uai exec "(LD_LIBRARY_PATH=/usr/lib/eyesee-mpp:/usr/lib /tmp/camcc 320x240 0 9 -11 2.0 0 &); echo ok"
```
The defaults came from host-side analysis: dump a raw frame, pull it over serial,
and measure it with `captures/nv21.py` (pure stdlib; NV21→PPM + colour stats):
```sh
# on the board: dump one converged NV21 frame, gzip it
./bin/uai exec "LD_LIBRARY_PATH=/usr/lib/eyesee-mpp:/usr/lib /tmp/cammpp 320x240 90 /tmp/f.nv21; gzip -f /tmp/f.nv21"
# pull it (chunked hexdump keeps each exec under uai's 15s timeout), then:
python3 captures/nv21.py f.nv21.gz out.ppm 320 240 9 -11 1.6   # uoff voff sat
```

## Notes / limitations
- **musl mismatch:** the board is musl **1.1.16** but the cross toolchain is musl
  1.2+ (time64). Time64-redirected libc symbols (e.g. `dlsym`→`__dlsym_time64`,
  `select`) don't exist on the board — bind the plain symbol (`__asm__("dlsym")`)
  and prefer `poll` over `select`.
- **Camera needs the vendor MPP libs** (`libmedia_mpp`/`libmpp_vi`/`libmpp_isp`/
  `libmpp_vo`), which are on the board; we `dlopen` them rather than link. Camera
  *colour* is ISP-3A-limited (mildly green/flat) — the proper fix lives in `libisp.so`,
  but `camcc` corrects it cheaply in software (white-balance + saturation on the CPU).
- **Display restore:** fully shutting MPP down (`SYS_Exit`) resets the display
  engine; the preview unblanks `/dev/fb0` on exit, but a clean console may need a
  reboot (the vendor's own sample has the same property).
- `uai push` recreates the file without the execute bit — `chmod +x` after pushing
  (the `deploy*`/`preview-start` targets do this for you).
- `exec`/`run` wrap the command to capture its exit code, so a command must not
  **end** in a bare `&`; to background, use `"(your cmd) & true"`. Long-running
  jobs should redirect output to a file or the MPP logs flood the serial line.
- Transfer is octal-printf (~4 wire bytes per payload byte): fine for these small
  binaries, slow for large ones.

## Why C instead of pyserial / CodeBlock
No python/pyserial dependency on the host, a single fast binary that works for both
human debugging (`term`/`monitor`) and automation, and — on the board side — direct
register/ioctl/MPP access so projects aren't boxed in by what the vendor's blocks
expose. The original `scripts/serial_transfer_run.py` is kept as a reference/fallback.
