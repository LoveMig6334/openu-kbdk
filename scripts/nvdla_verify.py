#!/usr/bin/env python3
"""NVDLA milestone (b): run an nvdla pack's images on the board NPU.

For each sampled dataset image: preprocess exactly like the pack, run the job
on the V831 NPU via bin/nna_runner, compare the returned logits byte-exactly
against the host int simulation, and check the predicted label. Finally report
NPU latency (--repeat averaged) for the full net.

Run:  uv run --project py python scripts/nvdla_verify.py \
        --pack packs/npu-toy3 --model models/npu_toy3.pt \
        --data examples/toy-dataset [--n 12] [--repeat 50] [--push]
"""

import argparse
import json
import subprocess
import sys
import tempfile
from pathlib import Path

import numpy as np
import torch

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "py" / "kbdk-convert" / "src"))
from kbdk_convert import nvdla, nvdla_compile  # noqa: E402

REPO = Path(__file__).resolve().parents[1]


def sh(*cmd: str) -> str:
    r = subprocess.run(cmd, capture_output=True, text=True)
    if r.returncode != 0:
        raise RuntimeError(f"{' '.join(cmd)} rc={r.returncode}: {r.stdout} {r.stderr}")
    return r.stdout


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--pack", type=Path, required=True)
    ap.add_argument("--model", type=Path, required=True)
    ap.add_argument("--data", type=Path, required=True)
    ap.add_argument("--n", type=int, default=12, help="images to run on the board")
    ap.add_argument("--repeat", type=int, default=50, help="latency averaging runs")
    ap.add_argument("--size", type=int, default=64)
    ap.add_argument("--push", action="store_true")
    a = ap.parse_args()

    manifest = json.loads((a.pack / "manifest.json").read_text())
    classes = manifest["labels"]
    imgs, _, labels = nvdla_compile.load_calib_images(a.data, a.size)

    from kbdk_train.train import npu_slim
    model = npu_slim(len(classes)).eval()
    model.load_state_dict(torch.jit.load(str(a.model), map_location="cpu").state_dict())
    qlayers, _ = nvdla_compile.quantize_slim(
        nvdla_compile.extract_slim_layers(model), imgs)

    if a.push:
        sh("adb", "push", str(REPO / "bin/nna_runner"), "/tmp/nna_runner")
        sh("adb", "shell", "chmod +x /tmp/nna_runner")
    sh("adb", "push", str(a.pack / "job.nvj"), "/tmp/job.nvj")

    rng = np.random.default_rng(0)
    pick = rng.choice(len(imgs), size=min(a.n, len(imgs)), replace=False)
    exact = correct = 0
    out_c = manifest["nvdla"]["nv_out_c"]
    with tempfile.TemporaryDirectory() as td:
        for i in pick:
            xq = nvdla_compile.preprocess(imgs[i])
            ip = Path(td) / "in.bin"
            ip.write_bytes(nvdla.pack_feature(xq))
            sh("adb", "push", str(ip), "/tmp/in.bin")
            sh(str(REPO / "target/debug/kbdk"), "exec",
               "/tmp/nna_runner /tmp/job.nvj /tmp/in.bin /tmp/out.bin 1")
            op = Path(td) / "out.bin"
            sh("adb", "pull", "/tmp/out.bin", str(op))
            got = nvdla.unpack_feature(op.read_bytes(), out_c, 1, 1).reshape(-1)
            want = nvdla_compile.simulate(qlayers, xq).reshape(-1)[:out_c]
            byte_ok = np.array_equal(got, want)
            pred = int(np.argmax(got))
            exact += int(byte_ok)
            correct += int(pred == labels[i])
            print(f"img {i:3d} [{classes[labels[i]]:>6}] -> {classes[pred]:>6} "
                  f"logits={got.tolist()} byte-exact={byte_ok}")

        # latency: repeat the full net on the last input
        out = sh(str(REPO / "target/debug/kbdk"), "exec",
                 f"/tmp/nna_runner /tmp/job.nvj /tmp/in.bin /tmp/out.bin {a.repeat}")
        for line in out.splitlines():
            if '"event":"done"' in line:
                print(line.strip())

    n = len(pick)
    print(f"\nboard-vs-host byte-exact: {exact}/{n}   label accuracy: {correct}/{n}")
    return 0 if exact == n else 1


if __name__ == "__main__":
    sys.exit(main())
