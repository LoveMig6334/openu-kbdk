#!/usr/bin/env python3
"""NVDLA milestone (a): single-conv parity vs the int-exact host reference.

Generates a random (seeded) conv+SDP job, runs it on the V831 NPU via
bin/nna_runner (push over adb), pulls the output back and compares it
byte-exactly against kbdk_convert.nvdla.ref_conv_sdp under both rounding
hypotheses (the SDP out-converter's rounding behaviour is what this harness
pins down).

Run:  uv run --project py python scripts/nvdla_parity.py [options]
Needs: board on adb, bin/nna_runner built (make nna-runner) and pushed
(--push does it for you).
"""

import argparse
import subprocess
import sys
import tempfile
from pathlib import Path

import numpy as np

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "py" / "kbdk-convert" / "src"))
from kbdk_convert import nvdla  # noqa: E402

REPO = Path(__file__).resolve().parents[1]


def sh(*cmd: str) -> str:
    r = subprocess.run(cmd, capture_output=True, text=True)
    if r.returncode != 0:
        raise RuntimeError(f"{' '.join(cmd)} rc={r.returncode}: {r.stdout} {r.stderr}")
    return r.stdout


def align(n: int, a: int = 4096) -> int:
    return (n + a - 1) // a * a


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--in-c", type=int, default=8)
    ap.add_argument("--k", type=int, default=16)
    ap.add_argument("--size", type=int, default=16, help="square input W=H")
    ap.add_argument("--kdim", type=int, default=3)
    ap.add_argument("--stride", type=int, default=1)
    ap.add_argument("--pad", type=int, default=1)
    ap.add_argument("--relu", action="store_true")
    ap.add_argument("--scale", type=int, default=23)
    ap.add_argument("--truncate", type=int, default=12)
    ap.add_argument("--bias-lshift", type=int, default=2)
    ap.add_argument("--pool", type=int, default=0, help="fused PDP maxpool kernel (0=off)")
    ap.add_argument("--pool-stride", type=int, default=2)
    ap.add_argument("--seed", type=int, default=0)
    ap.add_argument("--repeat", type=int, default=1)
    ap.add_argument("--push", action="store_true", help="push bin/nna_runner first")
    a = ap.parse_args()

    rng = np.random.default_rng(a.seed)
    x = rng.integers(-128, 128, size=(a.in_c, a.size, a.size), dtype=np.int8)
    wt = rng.integers(-128, 128, size=(a.k, a.in_c, a.kdim, a.kdim), dtype=np.int8)
    bias = rng.integers(-3000, 3000, size=a.k, dtype=np.int16)

    conv_out = (a.size - a.kdim + 2 * a.pad) // a.stride + 1
    if a.pool:
        out_w = out_h = (conv_out - a.pool) // a.pool_stride + 1
    else:
        out_w = out_h = conv_out

    in_blob = nvdla.pack_feature(x)
    wt_blob = nvdla.pack_weights(wt)
    bias_blob = bias.astype("<i2").tobytes()

    src_off = 0
    wt_off = align(len(in_blob))
    bias_off = align(wt_off + len(wt_blob))
    dst_off = align(bias_off + len(bias_blob))
    out_size = nvdla.feature_size(a.k, out_h, out_w)
    ion_size = align(dst_off + out_size) + 4096

    layer = nvdla.ConvLayer(
        in_w=a.size, in_h=a.size, in_c=a.in_c, out_c=a.k,
        kw=a.kdim, kh=a.kdim, stride=a.stride, pad=a.pad,
        bias_lshift=a.bias_lshift, relu=a.relu,
        out_scale=a.scale, out_truncate=a.truncate,
        src_offset=src_off, wt_offset=wt_off, bias_offset=bias_off, dst_offset=dst_off,
        has_pdp=bool(a.pool), pool_w=a.pool, pool_h=a.pool,
        pool_stride=a.pool_stride if a.pool else 0, pool_pad=0,
        pool_out_w=out_w if a.pool else 0, pool_out_h=out_h if a.pool else 0,
    )
    job = nvdla.emit_job(
        [layer],
        [(src_off, in_blob), (wt_off, wt_blob), (bias_off, bias_blob)],
        ion_size=ion_size, out_offset=dst_off, out_size=out_size,
    )

    with tempfile.TemporaryDirectory() as td:
        jp = Path(td) / "job.nvj"
        jp.write_bytes(job)
        if a.push:
            sh("adb", "push", str(REPO / "bin/nna_runner"), "/tmp/nna_runner")
            sh("adb", "shell", "chmod +x /tmp/nna_runner")
        sh("adb", "push", str(jp), "/tmp/job.nvj")
        out = sh(str(REPO / "target/debug/kbdk"), "exec",
                 f"/tmp/nna_runner /tmp/job.nvj - /tmp/out.bin {a.repeat}")
        print(out.strip())
        op = Path(td) / "out.bin"
        sh("adb", "pull", "/tmp/out.bin", str(op))
        got = nvdla.unpack_feature(op.read_bytes(), a.k, out_h, out_w)

    verdicts = []
    for rounding in (False, True):
        want = nvdla.ref_conv_sdp(
            x, wt, bias, stride=a.stride, pad=a.pad, bias_lshift=a.bias_lshift,
            scale=a.scale, truncate=a.truncate, relu=a.relu, rounding=rounding)
        if a.pool:
            want = nvdla.ref_maxpool(want, pool=a.pool, stride=a.pool_stride, pad=0)
        diff = np.abs(got.astype(np.int32) - want.astype(np.int32))
        verdicts.append((rounding, int(diff.max()), int((diff > 0).sum()), diff.size))
        name = "round-half-up" if rounding else "truncate"
        print(f"vs {name:>14}: max|diff|={diff.max()}  mismatches={int((diff>0).sum())}/{diff.size}")
        if diff.max() > 0 and diff.max() <= 4:
            idx = np.argwhere(diff > 0)[:5]
            for c, hh, ww in idx:
                print(f"   ({c},{hh},{ww}) got={got[c,hh,ww]} want={want[c,hh,ww]}")

    exact = [r for r, mx, _, _ in verdicts if mx == 0]
    if exact:
        print(f"PARITY: byte-exact under {'rounding' if exact[0] else 'truncation'} semantics")
        return 0
    print("PARITY FAILED under both hypotheses")
    return 1


if __name__ == "__main__":
    sys.exit(main())
