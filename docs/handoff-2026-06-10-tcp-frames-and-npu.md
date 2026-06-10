# Handoff: TCP camera feed → NVDLA/NPU inference (in that order)

State as of 2026-06-10: kbdk phases 1–7 done and hardware-verified (classification +
YOLOv2-slim detection, train→convert→deploy→live camera, UI with preview/boxes/
capture, board built-in AWNN packs). Phase 8 (publish/portability) is deferred by
the owner. Read `CLAUDE.md` (kbdk section) first — every board gotcha learned so
far is recorded there. Established workflow: feature branch → implement → verify on
the live board over adb → UI check via the self-screenshot hooks (`--screenshot`,
`--tab`, `KBDK_POLL`, `KBDK_SHOT_DELAY`) → docs touch → merge to main; the owner
pushes to GitHub themselves.

## Work item 1 — faster camera feed (TCP frame server)

**Now:** kbrun drops a 240×240 RGB frame (`KBF1` + u16le w,h header) into
`/tmp/kbrun_frame.rgb` every 3rd frame; the UI's poller `adb pull`s it every
~400 ms → ~2.5 fps. The ceiling is the adb-pull round trip, not the board.

**Goal:** ~10–15 fps in the UI. `adb forward tcp:PORT tcp:PORT` is verified
working on this adbd; raw 240×240×3 ≈ 173 KB/frame → 15 fps ≈ 2.6 MB/s, well
under the measured ~6 MB/s USB throughput. No JPEG needed (saves the single A7).

**Sketch:**
- kbrun: a tiny listener thread (port 18902, `SO_REUSEADDR`) serving the latest
  preview buffer per connection/request (one-shot "send KBF1 frame, close" is
  simplest and stateless; keep the tmpfs file as fallback). Guard the frame
  buffer with the existing g_lock or a dedicated mutex; write_preview already
  produces the bytes.
- kbdk-core: `frame_stream` helper that sets up `adb forward` (spawn `adb
  forward tcp:18902 tcp:18902`, remove on drop) and reads frames in a loop.
- kbdk-ui workers: poller prefers TCP, falls back to the adb-pull path if the
  connect fails (older kbrun on the board). Bump poll cadence accordingly;
  `ctx.request_repaint()` per frame.

**Board traps that WILL bite here** (all documented in CLAUDE.md):
- musl 1.1.16 + musl-1.2 cross headers: **no `select`** (time64 redirect — use
  `poll`), no `clock_gettime` in the dynamically-linked kbrun (use the existing
  `now_ms()` /proc/uptime helper). After linking, check
  `arm-unknown-linux-musleabihf-nm -D -u bin/kbrun | grep time64` — must be empty.
- kbrun self-daemonizes (fork+setsid); the listener thread must start after that.
- Commands over the sentinel exec must not end in a bare `&`.

## Work item 2 — NVDLA/NPU path for real speed

**Now:** all kbdk inference is CPU. ncnn int8 mbv2 ≈ 470 ms; the vendor AWNN
runtime does resnet18 in 60–80 ms on the same core (better kernels), but it's
closed and can't run our models. The V831's NPU is an **NVIDIA NVDLA `nv_small`**
core, and the userspace driver path is already proven on this hardware:
`third_party/v831-npu` (GPLv3, mtx512) drives it via `/dev/mem` (regs
0x2400000, CCU 0x3001000) + `/dev/ion` + `/dev/cedar_dev` with **no kernel
module** — `make nna-cifar10` runs a 4-conv CNN on the NPU (verified: "ship :
127"). `make nnaprobe` is the safe register probe. Background:
`docs/superpowers/specs/2026-06-08-npu-cnn-runtime-design.md` and the research
report `docs/research/2026-06-08-v831-maixhub-models.md`.

**The gap is the compiler:** nothing open turns a trained model into NVDLA
descriptors — the cifar10 example hand-builds its layer configs. That is the
actual work item.

**Suggested approach (decide after a fresh look):**
1. Study `third_party/v831-npu`'s descriptor/layer API (hw/*.cpp) — what ops the
   nv_small path supports (conv+bias+relu, pooling, FC; int8) and what the
   weight/feature memory layouts are (cache flush around every CPU↔ION copy;
   all NPU address fields take the cedar IOMMU/phys address).
2. Write a small "nvdla pack" compiler in Python (kbdk-convert side): take the
   ncnn .param/.bin (already int8-quantized with known scales) or the torch
   model, map each supported layer to an NVDLA descriptor blob, emit a pack the
   board runner executes layer-by-layer (CPU fallback for unsupported ops —
   e.g. YOLO head decode already runs on CPU).
3. Board side: a new runner or a third kbrun engine `"nvdla"` — but **GPLv3
   isolation matters**: third_party/v831-npu code must stay in a separate
   binary (like nna_cifar10), not linked into MIT kbrun. A small
   `kbrun-nvdla` executable speaking the same pack/manifest + JSON-lines
   protocol is the clean shape; kbdk's deploy/run can select the runner binary
   by manifest `runtime: "nvdla"`.
4. Milestone ladder: (a) single conv layer matches ncnn output bit-for-bit-ish
   on the NPU; (b) toy classifier (the conv-only tiny net) end-to-end;
   (c) MobileNetV2 — note depthwise conv support on nv_small needs checking
   early, it may force a different backbone (plain conv "slim" net) for the NPU
   path; (d) YOLOv2-slim backbone on NPU + CPU decode.
5. Keep expectations honest: this is research-grade; timebox stage (a) before
   committing to the rest. Even backbone-only on NPU should beat 100 ms.

**Hardware facts:** needs root (`/dev/mem` — adb shell is root); buffers are
cached (flush via `ION_IOC_SUNXI_FLUSH_RANGE`); the NNA clock comes up at boot
via DT `assigned-clocks` (nnaprobe confirmed regs respond); 60 MB RAM total —
budget ION allocations.

## Definition of done
1. UI camera preview ≥10 fps over TCP with automatic fallback to adb-pull.
2. An NVDLA-executed model (at minimum the tiny conv net, ideally a slim
   backbone) deployable through `kbdk deploy/run` with measured latency vs the
   CPU baseline, GPLv3 code isolated in its own binary.
