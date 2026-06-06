# kidbright-uai

Low-level C toolkit for the **KidBright µAI** board (Allwinner V831, single-core
ARM Cortex-A7, armv7l hard-float NEON/VFPv4, Linux 4.9 BusyBox/Tina, musl libc).
Cross-compile C on the Mac, drive the board over its serial console, and talk to
the hardware **directly** (syscall / ioctl / mmap, or the vendor MPP API where the
kernel forces it) — instead of going through the board's CodeBlock / MaixPy stack.
The goal is full control over the screen, audio, camera (and eventually GPIO/NPU)
for building AIoT projects without the vendor library's limits.

## What works

| Capability | Tool | How |
| --- | --- | --- |
| **Screen** | `fbtest` | `/dev/fb0` (240×240 RGB888) via raw `ioctl`+`mmap` |
| **Audio** | `audio` | raw `SNDRV_PCM_IOCTL_*` on `/dev/snd` — no alsa-lib (`probe`/`tone`/`play`/`rec`) |
| **Camera capture** | `cammpp` | Allwinner **MPP** `AW_MPI_VI`+ISP → NV21M frame (standard V4L2 streaming is not exposed on this BSP) |
| **Live camera preview** | `campreview` | `AW_MPI_SYS_Bind(VI→VO)` — camera straight to the LCD, zero-copy in hardware |
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
│   ├── cammpp.c                   # MPP camera capture (one NV21M frame, for processing)
│   └── campreview.c               # MPP live preview on the panel (VI→VO bind)
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
make cammpp     # camera capture | make campreview  # live preview
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

# live camera preview on the 240x240 panel (VI->VO hardware bind)
make preview-start                        # camera goes live, stays until stopped
make preview-stop                         # clean shutdown + unblank the panel
```

## Notes / limitations
- **musl mismatch:** the board is musl **1.1.16** but the cross toolchain is musl
  1.2+ (time64). Time64-redirected libc symbols (e.g. `dlsym`→`__dlsym_time64`,
  `select`) don't exist on the board — bind the plain symbol (`__asm__("dlsym")`)
  and prefer `poll` over `select`.
- **Camera needs the vendor MPP libs** (`libmedia_mpp`/`libmpp_vi`/`libmpp_isp`/
  `libmpp_vo`), which are on the board; we `dlopen` them rather than link. Camera
  *colour* is ISP-3A-limited (mildly green/flat) — the tuning lives in `libisp.so`.
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
