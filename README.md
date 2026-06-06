# kidbright-uai

Host-side toolkit for the **KidBright µAI** board (Allwinner V831, single-core
ARM Cortex-A7, armv7l hard-float NEON/VFPv4, Linux 4.9 BusyBox/Tina, musl libc).
Cross-compile C on the Mac and drive the board over its serial console.

## Board facts
- Target triple: `arm-linux-musleabihf` · loader `/lib/ld-musl-armhf.so.1`
- Serial console: `/dev/cu.usbserial-210` @ **115200 8N1**, lands at a `root@sipeed` shell (no login)
- busybox has **no `base64`** applet → file transfer uses octal `printf`

## Layout
```
kidbright-uai/
├── Makefile
├── src/
│   ├── uai.c      # the host serial toolkit (compiled native, macOS arm64)
│   └── hello.c    # board smoke-test (printf + sqrt → checks hard-float)
├── scripts/
│   └── serial_transfer_run.py   # original pyserial transfer (kept as fallback/reference)
└── bin/           # build output (gitignored): uai (host), hello (ARM)
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
make            # builds bin/uai (host) + bin/hello (board, cross-compiled)
make uai        # just the host tool
make hello      # just the board binary
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
./bin/uai exec "ls -l /tmp"               # exits with the board command's rc
make deploy                               # build hello and run it on the V831
./bin/uai deploy bin/hello /tmp/hello     # → hello from KidBright uAI: sqrt(2)=1.41421
./bin/uai monitor -t                      # watch boot/runtime logs live
./bin/uai term                            # full interactive shell over serial
```

### Notes / limitations
- `exec`/`run` wrap the command with `; echo <marker>$?` to capture the exit
  code, so a command must not **end** in a bare `&`. To background, write
  `"(your cmd) & true"`.
- Transfer is ~162 chunks for a 7 KB binary; fine for small programs, slow for
  large ones (octal printf is ~4 bytes wire per payload byte). Add `xxd`/
  `uudecode` to the rootfs if you need faster transfers.
- Only one process can hold the serial port at a time (so you can't `monitor`
  and `exec` simultaneously).

## Why C instead of pyserial
No python/pyserial dependency, a single fast binary, and the same tool works for
quick human debugging (`term`/`monitor`) and for automation. The original
`scripts/serial_transfer_run.py` is kept as a reference/fallback.
