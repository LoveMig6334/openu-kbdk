# Research: the board has ~20 MB free RAM — what bigger/better models fit?

Question (owner, 2026-06-11): the perf monitor shows ~20 MB free while running —
can we use it for a better, larger model? All numbers below measured on the live
board this session unless marked otherwise.

## TL;DR

RAM is **not** the binding constraint on either inference path:

- **CPU (ncnn/AWNN)**: latency-bound, not RAM-bound. MobileNetV2 int8 already
  costs 466–950 ms/inf; a model 2× bigger would be 2× slower. More RAM buys
  nothing useful here.
- **NPU (NVDLA)**: massively underused. The constraint is the per-layer CBUF
  programming envelope, not memory. Within today's verified envelope a **~20×
  bigger network than npu_slim runs in 8.7 ms** — still 4× faster than the
  camera delivers frames. The right move is a wider backbone (`npu_mid`-style),
  not chasing RAM.

## Measured memory ledger (60 MB total)

| Item | Measured |
| --- | --- |
| MemAvailable, idle | ~37 MB |
| MemAvailable, AWNN resnet18 running | ~22 MB |
| kbrun RSS: AWNN resnet18 @224 | 22.3 MB |
| kbrun RSS: ncnn MobileNetV2 @224 | ~15 MB |
| kbrun RSS: nvdla engine | 5.3 MB (+2.3 MB nna_runner; weights live in ION) |
| /tmp tmpfs (logs, frames, NPU job files) | 30 MB carve, ~7 MB used |
| ION allocation ceiling (idle, single buffer) | **24 MB OK, 32 MB ENOMEM** (CmaTotal=0 — ION system heap) |

So the NPU path could hold ~24 MB of weights — far more than the CBUF envelope
lets a single-pass runner execute. Weights are nowhere near the limit.

## NPU envelope, hardware-verified (scripts/nvdla_parity.py)

| Layer shape | CBUF banks (data+wt) | Result |
| --- | --- | --- |
| 56×56, 64→64, 3×3 | 13+3 | byte-exact, 1.4 ms |
| 28×28, 128→128, 3×3 | 7+10 | byte-exact, 1.4 ms |
| 112×112×3 stem (npu_det L1) | 7+1 | byte-exact (milestone d) |
| 224×224×3→16, s2 | 25+1 | **FAILS** (gross mismatch — needs partial-fetch/H-split programming) |
| 14×14, 192→192, 3×3 | 4+21 | **FAILS** (gross mismatch — >128ch needs multi-pass weight handling) |

**Verified envelope today: input ≤ ~112², channels ≤ 128, kernels 1×1–5×5,
maxpool 2–3.** Anything outside must go through the parity harness before use.

End-to-end synthetic test (scripts/nvdla_capacity.py): 13 conv layers @112²,
channels 32→128, 1000-class 1×1 head, 1.49 MB int8 weights, 1.71 MB ION —
**8.7 ms/inference** (30-run avg). npu_slim is 0.07 MB / 1.9 ms for scale.

## Recommendations, in order

1. **Now, no new code: a wider `npu_mid` backbone** (e.g. 112² input,
   32→64→96→128 channels, a few extra 128-ch stages, same conv/BN/relu/pool
   recipe). ~10–20× npu_slim capacity at ~5–9 ms/inf, trains and converts with
   the existing pipeline. This is where real accuracy on harder datasets comes
   from. Same idea applies to detection (npu_det with 64→128-ch stages).
2. **Medium: pretrained RepVGG-style transfer.** RepVGG reparameterizes at
   deploy time into a *plain 3×3-conv + ReLU stack* — exactly what nv_small
   runs (no depthwise, no residual at inference). A timm-pretrained RepVGG-A0
   (~9 M params ≈ 9 MB int8, ~72% ImageNet top-1, fits ION easily) would need
   either 112² fine-tuning or item 3 for 224².
3. **Compiler work to widen the envelope** (each gated by nvdla_parity):
   H-split/partial-fetch for ≥224² inputs and >128-ch layers; SDP elementwise
   add (X2 path) for residual nets; per-channel quant for deeper-net accuracy.
4. **Don't** grow CPU-path models: 20 MB of headroom fits the weights but the
   single A7 doesn't fit the FLOPs. AWNN resnet18 (22 MB RSS) is already near
   the comfortable ceiling on a 60 MB board anyway.

## Caveats

- **The NPU is single-tenant** (found after this report): concurrent nna_runner
  instances — e.g. probe scripts while a kbrun nvdla pack serves — silently
  corrupt each other's results and inflate latencies. The numbers in this doc
  were measured with kbrun stopped; nna_runner now enforces this (rc 5).

- The ION probe was a single contiguous buffer on an idle board; with kbrun +
  camera running, budget ~16–20 MB to be safe.
- Synthetic-net latency used random weights; real packs add CPU-side softmax /
  decode (sub-ms) and the serve-mode file shuffle (~1–2 ms, already included
  in measured pack numbers).
- 8.7 ms includes per-inference input upload; layer_ms granularity is 10 ms
  (/proc/uptime), hence the zeros.
