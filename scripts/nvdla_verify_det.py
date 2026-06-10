#!/usr/bin/env python3
"""NVDLA milestone (d): detection pack on the board NPU.

For sampled dataset images: run the detection job on the V831 NPU, compare the
raw output map byte-exactly against the host int simulation, decode boxes on
both sides, and report match. Then measure full-net NPU latency.

Run:  uv run --project py python scripts/nvdla_verify_det.py \
        --pack packs/npu-toydet --model models/npu_toydet.pt \
        --data examples/toy-detection [--n 12] [--repeat 100] [--push]
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
    ap.add_argument("--n", type=int, default=12)
    ap.add_argument("--repeat", type=int, default=100)
    ap.add_argument("--push", action="store_true")
    a = ap.parse_args()

    manifest = json.loads((a.pack / "manifest.json").read_text())
    classes = manifest["labels"]
    det = manifest["detection"]
    nv = manifest["nvdla"]
    size = manifest["input"]["width"]
    grid, anchors = det["grid"], det["anchors"]
    out_c = nv["nv_out_c"]
    logit_scale = nv["logit_scale"]

    imgs, _ = nvdla_compile.load_yolo_images(a.data, size)

    from kbdk_train.detect import box_iou, decode_boxes, nms, npu_det
    model = npu_det(len(classes), len(anchors) // 2).eval()
    model.load_state_dict(torch.jit.load(str(a.model), map_location="cpu").state_dict())
    qlayers, _ = nvdla_compile.quantize_slim(
        nvdla_compile.extract_slim_layers(model), imgs)

    if a.push:
        sh("adb", "push", str(REPO / "bin/nna_runner"), "/tmp/nna_runner")
        sh("adb", "shell", "chmod +x /tmp/nna_runner")
    sh("adb", "push", str(a.pack / "job.nvj"), "/tmp/job.nvj")

    rng = np.random.default_rng(0)
    pick = rng.choice(len(imgs), size=min(a.n, len(imgs)), replace=False)
    exact = box_match = box_tot = 0
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
            got = nvdla.unpack_feature(op.read_bytes(), out_c, grid, grid)
            want = nvdla_compile.simulate(qlayers, xq)
            byte_ok = np.array_equal(got, want)
            exact += int(byte_ok)
            bdets = nms(decode_boxes(got.astype(np.float32) / logit_scale,
                                     anchors, len(classes), det["conf_threshold"]))
            hdets = nms(decode_boxes(want.astype(np.float32) / logit_scale,
                                     anchors, len(classes), det["conf_threshold"]))
            box_tot += len(hdets)
            for hd in hdets:
                if any(hd[0] == bd[0] and box_iou(hd[2:], bd[2:]) > 0.99 for bd in bdets):
                    box_match += 1
            print(f"img {i:3d}: byte-exact={byte_ok}  boxes(board)="
                  f"{[(classes[d[0]], round(d[1], 2)) for d in bdets]}")

        out = sh(str(REPO / "target/debug/kbdk"), "exec",
                 f"/tmp/nna_runner /tmp/job.nvj /tmp/in.bin /tmp/out.bin {a.repeat}")
        for line in out.splitlines():
            if '"event":"done"' in line:
                print(line.strip())

    n = len(pick)
    print(f"\nbyte-exact maps: {exact}/{n}   board-vs-host boxes: {box_match}/{box_tot}")
    return 0 if exact == n else 1


if __name__ == "__main__":
    sys.exit(main())
