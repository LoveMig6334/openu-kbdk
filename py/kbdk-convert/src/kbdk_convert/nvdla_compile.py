"""Compile an npu_slim TorchScript model into an NVDLA pack (runtime "nvdla").

Pipeline: load the trained npu_slim weights -> fold BN into the convs ->
per-tensor int8 PTQ (activation ranges calibrated on the dataset) -> map each
conv[+relu][+maxpool] stage onto a fused NVDLA CONV->SDP[->PDP] layer -> emit a
pack directory: manifest.json (runtime "nvdla") + job.nvj + labels.txt.

Quantization model (matches the hardware byte-exactly, see nvdla.py):
  x_q = uint8 - 128                      (input scale s0 = 127.5)
  W_q = round(W * s_w),  s_w = 127/max|W|
  acc = conv(x_q, W_q)                   (int32 on the NPU)
  acc += b16 << bias_lshift,  b16<<l ~ b * s_in * s_w
  out = round_half_away((acc * scale) >> truncate),  scale/2^t ~ s_out/(s_in*s_w)

The CPU side dequantizes logits with meta["logit_scale"] before softmax.

Run:  uv run --project py python -m kbdk_convert.nvdla_compile \
        --model models/npu_toy3.pt --data examples/toy-dataset --name npu-toy3
"""

from __future__ import annotations

import argparse
import hashlib
import json
import math
import sys
from dataclasses import dataclass
from pathlib import Path

import numpy as np
import torch
import torch.nn as nn
import torch.nn.functional as F
from PIL import Image

from . import nvdla


# ---- BN folding + model walking ---------------------------------------------------

def fold_bn(w, b, gamma, beta, mean, var, eps):
    """conv W [K,C,kh,kw] (+ optional bias) followed by BN -> equivalent W', b'."""
    g = gamma / np.sqrt(var + eps)
    w2 = w * g[:, None, None, None]
    b0 = b if b is not None else np.zeros(w.shape[0], dtype=w.dtype)
    b2 = (b0 - mean) * g + beta
    return w2.astype(np.float32), b2.astype(np.float32)


@dataclass
class FloatLayer:
    w: np.ndarray          # [K,C,kh,kw] float32 (BN folded)
    b: np.ndarray          # [K] float32
    stride: int
    pad: int
    relu: bool
    pool: int              # 0 = no maxpool
    pool_stride: int


def extract_slim_layers(model: nn.Module) -> list[FloatLayer]:
    """Walk an npu_slim nn.Sequential (Conv2d [BN] [ReLU] [MaxPool2d] ...)."""
    layers: list[FloatLayer] = []
    cur: FloatLayer | None = None
    for m in model.children():
        if isinstance(m, nn.Conv2d):
            if cur is not None:
                layers.append(cur)
            w = m.weight.detach().numpy().astype(np.float32)
            b = m.bias.detach().numpy().astype(np.float32) if m.bias is not None \
                else np.zeros(w.shape[0], dtype=np.float32)
            cur = FloatLayer(w=w, b=b, stride=m.stride[0], pad=m.padding[0],
                             relu=False, pool=0, pool_stride=0)
        elif isinstance(m, nn.BatchNorm2d):
            assert cur is not None
            cur.w, cur.b = fold_bn(
                cur.w, cur.b, m.weight.detach().numpy(), m.bias.detach().numpy(),
                m.running_mean.numpy(), m.running_var.numpy(), m.eps)
        elif isinstance(m, nn.ReLU):
            assert cur is not None
            cur.relu = True
        elif isinstance(m, nn.MaxPool2d):
            assert cur is not None
            ks = m.kernel_size if isinstance(m.kernel_size, int) else m.kernel_size[0]
            st = m.stride if isinstance(m.stride, int) else m.stride[0]
            cur.pool, cur.pool_stride = ks, st
        elif isinstance(m, nn.Flatten):
            pass
        else:
            raise ValueError(f"npu_slim walk: unsupported module {type(m).__name__}")
    if cur is not None:
        layers.append(cur)
    return layers


# ---- quantization ------------------------------------------------------------------

def requant_params(m: float) -> tuple[int, int]:
    """multiplier M -> (int16 scale, truncate) with scale/2^t ~ M, max precision."""
    assert m > 0
    t = min(31, int(math.floor(math.log2(32767.0 / m))))
    t = max(0, t)
    scale = int(round(m * (1 << t)))
    scale = max(1, min(32767, scale))
    return scale, t


def bias_params(bq: np.ndarray) -> tuple[np.ndarray, int]:
    """float bias (in acc units) -> (int16 stored bias, lshift) with b16<<l ~ bq."""
    lshift = 0
    while np.abs(np.round(bq / (1 << lshift))).max() > 32767:
        lshift += 1
    return np.round(bq / (1 << lshift)).astype(np.int16), lshift


@dataclass
class QuantLayer:
    wq: np.ndarray         # int8 [K,C,kh,kw]
    b16: np.ndarray        # int16 [K]
    bias_lshift: int
    scale: int
    truncate: int
    relu: bool
    stride: int
    pad: int
    pool: int
    pool_stride: int


def preprocess(img: np.ndarray) -> np.ndarray:
    """uint8 HWC image -> int8 [C,H,W] in the pack's input quantization."""
    return (img.astype(np.int16) - 128).clip(-128, 127).astype(np.int8).transpose(2, 0, 1)


def _float_forward(layers: list[FloatLayer], x: torch.Tensor) -> list[torch.Tensor]:
    """float stage outputs (post relu, pre pool has same max; post pool returned)."""
    outs = []
    for fl in layers:
        x = F.conv2d(x, torch.from_numpy(fl.w), torch.from_numpy(fl.b),
                     stride=fl.stride, padding=fl.pad)
        if fl.relu:
            x = F.relu(x)
        if fl.pool:
            x = F.max_pool2d(x, fl.pool, fl.pool_stride)
        outs.append(x)
    return outs


def quantize_slim(layers: list[FloatLayer], calib_imgs: np.ndarray):
    """PTQ: per-tensor weight scales + calibrated activation scales.

    calib_imgs: uint8 [N,H,W,3]. Returns (qlayers, meta).
    """
    x = torch.from_numpy(
        ((calib_imgs.astype(np.float32) / 255.0) - 0.5) / 0.5).permute(0, 3, 1, 2)
    with torch.no_grad():
        stage_outs = _float_forward(layers, x)
    amax = [float(o.abs().max()) for o in stage_outs]

    s_act = [127.5]  # input scale
    for a in amax:
        s_act.append(127.0 / max(a, 1e-6))

    qlayers = []
    for i, fl in enumerate(layers):
        s_w = 127.0 / max(float(np.abs(fl.w).max()), 1e-9)
        wq = np.clip(np.round(fl.w * s_w), -128, 127).astype(np.int8)
        b16, lsh = bias_params(fl.b * s_act[i] * s_w)
        scale, trunc = requant_params(s_act[i + 1] / (s_act[i] * s_w))
        qlayers.append(QuantLayer(
            wq=wq, b16=b16, bias_lshift=lsh, scale=scale, truncate=trunc,
            relu=fl.relu, stride=fl.stride, pad=fl.pad,
            pool=fl.pool, pool_stride=fl.pool_stride))
    meta = {"act_scales": s_act, "logit_scale": s_act[-1]}
    return qlayers, meta


def simulate(qlayers: list[QuantLayer], xq: np.ndarray) -> np.ndarray:
    """Host int-exact simulation of the full NPU job (byte-exact vs hardware)."""
    for ql in qlayers:
        xq = nvdla.ref_conv_sdp(
            xq, ql.wq, ql.b16, stride=ql.stride, pad=ql.pad,
            bias_lshift=ql.bias_lshift, scale=ql.scale, truncate=ql.truncate,
            relu=ql.relu, rounding=True)
        if ql.pool:
            xq = nvdla.ref_maxpool(xq, pool=ql.pool, stride=ql.pool_stride, pad=0)
    return xq


# ---- job + pack emission ------------------------------------------------------------

def _align(n: int, a: int = 64) -> int:
    return (n + a - 1) // a * a


def build_job(qlayers: list[QuantLayer], in_w: int, in_h: int) -> tuple[bytes, dict]:
    """Assemble the NVJ1 job: weight/bias blobs + ping-pong activation buffers."""
    blobs: list[tuple[int, bytes]] = []
    off = 0
    wt_offs, bias_offs = [], []
    for ql in qlayers:
        wb = nvdla.pack_weights(ql.wq)
        wt_offs.append(off)
        blobs.append((off, wb))
        off = _align(off + len(wb))
        bb = ql.b16.astype("<i2").tobytes()
        bias_offs.append(off)
        blobs.append((off, bb))
        off = _align(off + len(bb))

    # activation sizes per stage
    dims = [(qlayers[0].wq.shape[1], in_h, in_w)]
    for ql in qlayers:
        c = ql.wq.shape[0]
        h = (dims[-1][1] - ql.wq.shape[2] + 2 * ql.pad) // ql.stride + 1
        w = (dims[-1][2] - ql.wq.shape[3] + 2 * ql.pad) // ql.stride + 1
        if ql.pool:
            h = (h - ql.pool) // ql.pool_stride + 1
            w = (w - ql.pool) // ql.pool_stride + 1
        dims.append((c, h, w))
    bufsz = max(nvdla.feature_size(c, h, w) for c, h, w in dims)
    buf_a = _align(off, 4096)
    buf_b = _align(buf_a + bufsz, 4096)
    ion_size = _align(buf_b + bufsz, 4096) + 4096

    layers = []
    src, dst = buf_a, buf_b
    for i, ql in enumerate(qlayers):
        cin, hin, win = dims[i]
        cout, hout, wout = dims[i + 1]
        layers.append(nvdla.ConvLayer(
            in_w=win, in_h=hin, in_c=cin, out_c=cout,
            kw=ql.wq.shape[3], kh=ql.wq.shape[2], stride=ql.stride, pad=ql.pad,
            bias_lshift=ql.bias_lshift, relu=ql.relu,
            out_scale=ql.scale, out_truncate=ql.truncate,
            src_offset=src, wt_offset=wt_offs[i], bias_offset=bias_offs[i],
            dst_offset=dst,
            has_pdp=bool(ql.pool), pool_w=ql.pool, pool_h=ql.pool,
            pool_stride=ql.pool_stride, pool_pad=0,
            pool_out_w=wout if ql.pool else 0, pool_out_h=hout if ql.pool else 0,
        ))
        src, dst = dst, src

    out_c, out_h, out_w = dims[-1]
    in_size = nvdla.feature_size(*dims[0])
    out_size = nvdla.feature_size(out_c, out_h, out_w)
    job = nvdla.emit_job(layers, blobs, ion_size=ion_size,
                         out_offset=layers[-1].dst_offset, out_size=out_size,
                         in_offset=buf_a, in_size=in_size)
    info = {"in_size": in_size, "out_size": out_size,
            "out_dims": [out_c, out_h, out_w], "ion_size": ion_size}
    return job, info


def load_calib_images(data_dir: Path, size: int) -> tuple[np.ndarray, list[str], list[int]]:
    """All dataset images resized to size x size; returns (imgs, classes, labels)."""
    classes = sorted(d.name for d in data_dir.iterdir() if d.is_dir())
    imgs, labels = [], []
    for ci, cls in enumerate(classes):
        for p in sorted((data_dir / cls).iterdir()):
            if p.suffix.lower() not in (".png", ".jpg", ".jpeg"):
                continue
            im = Image.open(p).convert("RGB")
            s = size * 256 // 224
            im = im.resize((s, s))
            left = (s - size) // 2
            im = im.crop((left, left, left + size, left + size))
            imgs.append(np.asarray(im, dtype=np.uint8))
            labels.append(ci)
    return np.stack(imgs), classes, labels


def load_yolo_images(data_dir: Path, size: int) -> tuple[np.ndarray, list[str]]:
    """YOLO/Darknet layout (images/, classes.txt) -> (imgs resized, classes)."""
    classes = (data_dir / "classes.txt").read_text().split()
    imgs = []
    for p in sorted((data_dir / "images").iterdir()):
        if p.suffix.lower() not in (".png", ".jpg", ".jpeg"):
            continue
        im = Image.open(p).convert("RGB").resize((size, size))
        imgs.append(np.asarray(im, dtype=np.uint8))
    return np.stack(imgs), classes


def _float_logits(model, img: np.ndarray) -> np.ndarray:
    x = torch.from_numpy(((img.astype(np.float32) / 255.0) - 0.5) / 0.5)
    return model(x.permute(2, 0, 1).unsqueeze(0)).detach().numpy()[0]


def _write_pack(out_dir: Path, name: str, task: str, backbone: str, size: int,
                classes: list[str], job: bytes, info: dict, meta: dict,
                detection: dict | None) -> None:
    out_dir.mkdir(parents=True, exist_ok=True)
    (out_dir / "job.nvj").write_bytes(job)
    (out_dir / "labels.txt").write_text("\n".join(classes) + "\n")
    md5 = hashlib.md5(job).hexdigest()
    manifest = {
        "name": name,
        "task": task,
        "backbone": backbone,
        "input": {"width": size, "height": size,
                  "mean": [127.5] * 3, "norm": [0.0078125] * 3},
        "quant": "int8",
        "runtime": "nvdla",
        "blobs": {"in_blob": "nvdla", "out_blob": "nvdla"},
        "files": {"param": "job.nvj", "bin": "job.nvj", "labels_file": "labels.txt"},
        "md5": {"param": md5, "bin": md5},
        "labels": classes,
        "nvdla": {"logit_scale": meta["logit_scale"],
                  "nv_in_size": info["in_size"], "nv_out_size": info["out_size"],
                  "nv_out_c": info["out_dims"][0]},
    }
    if detection:
        manifest["detection"] = detection
    (out_dir / "manifest.json").write_text(json.dumps(manifest, indent=2) + "\n")


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(description="npu_slim/npu_det TorchScript -> NVDLA pack")
    ap.add_argument("--model", required=True, type=Path)
    ap.add_argument("--data", required=True, type=Path)
    ap.add_argument("--name", required=True)
    ap.add_argument("--out-dir", type=Path, default=None)
    ap.add_argument("--size", type=int, default=None,
                    help="input size (default: the backbone's native, or the detection meta's)")
    ap.add_argument("--backbone", default="npu_slim",
                    help="classification model constructor: npu_slim | npu_mid")
    a = ap.parse_args(argv)
    out_dir = a.out_dir or Path("packs") / a.name

    # a model.pt.meta.json sidecar with task "detection" switches the flow
    meta_path = Path(str(a.model) + ".meta.json")
    det_meta = None
    if meta_path.exists():
        m = json.loads(meta_path.read_text())
        if m.get("task") == "detection":
            det_meta = m

    if det_meta:
        size = a.size or det_meta["size"]
        classes = det_meta["classes"]
        anchors = det_meta["anchors"]
        imgs, _ = load_yolo_images(a.data, size)

        from kbdk_train.detect import decode_boxes, nms, npu_det
        width = "mid" if det_meta.get("backbone") == "npu-det-mid" else "slim"
        model = npu_det(len(classes), len(anchors) // 2, width=width).eval()
        model.load_state_dict(torch.jit.load(str(a.model), map_location="cpu").state_dict())

        qlayers, meta = quantize_slim(extract_slim_layers(model), imgs)

        # box-level parity: decode the int8-sim map vs the float map
        hit = tot = 0
        for img in imgs:
            qmap = simulate(qlayers, preprocess(img)).astype(np.float32) / meta["logit_scale"]
            qdets = nms(decode_boxes(qmap, anchors, len(classes), 0.5))
            fdets = nms(decode_boxes(_float_logits(model, img), anchors, len(classes), 0.5))
            tot += len(fdets)
            from kbdk_train.detect import box_iou
            for fd in fdets:
                if any(fd[0] == qd[0] and box_iou(fd[2:], qd[2:]) > 0.5 for qd in qdets):
                    hit += 1
        print(json.dumps({"event": "parity", "int8_vs_float_boxes": hit / max(tot, 1),
                          "n_float_boxes": tot, "n_images": len(imgs)}))

        job, info = build_job(qlayers, size, size)
        detection = {"grid": det_meta["grid"], "anchors": anchors,
                     "conf_threshold": det_meta.get("conf_threshold", 0.5),
                     "nms_threshold": det_meta.get("nms_threshold", 0.45)}
        _write_pack(out_dir, a.name, "detection", det_meta.get("backbone", "npu-det"),
                    size, classes, job, info, meta, detection)
        print(json.dumps({"event": "saved", "pack": str(out_dir), **info}))
        return 0

    size = a.size or {"npu_slim": 64, "npu_mid": 112}[a.backbone]
    imgs, classes, labels = load_calib_images(a.data, size)

    from kbdk_train.train import npu_mid, npu_slim
    ctor = {"npu_slim": npu_slim, "npu_mid": npu_mid}[a.backbone]
    model = ctor(len(classes)).eval()
    model.load_state_dict(torch.jit.load(str(a.model), map_location="cpu").state_dict())

    qlayers, meta = quantize_slim(extract_slim_layers(model), imgs)

    # int8-sim vs float top-1 agreement over the dataset
    agree = correct = 0
    for img, lab in zip(imgs, labels):
        qlog = simulate(qlayers, preprocess(img)).reshape(-1)[:len(classes)]
        flog = _float_logits(model, img).reshape(-1)
        agree += int(np.argmax(qlog) == np.argmax(flog))
        correct += int(np.argmax(qlog) == lab)
    n = len(labels)
    print(json.dumps({"event": "parity", "int8_vs_float_top1": agree / n,
                      "int8_acc": correct / n, "n": n}))

    job, info = build_job(qlayers, size, size)
    _write_pack(out_dir, a.name, "classification", a.backbone, size, classes,
                job, info, meta, None)
    print(json.dumps({"event": "saved", "pack": str(out_dir), **info}))
    return 0


if __name__ == "__main__":
    sys.exit(main())
