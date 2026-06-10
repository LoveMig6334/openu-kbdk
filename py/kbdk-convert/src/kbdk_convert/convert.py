"""TorchScript -> pnnx -> ncnn (fp16 storage) -> int8 quantize -> kbdk model pack.

Pipeline facts learned on hardware (see docs/superpowers/specs/2026-06-10-*.md):
- The board's AWNN cannot run vanilla ncnn models; the pack targets our own
  statically-linked ncnn runner (board/runner/kbrun.cpp).
- fp32 ResNet18 OOMs the 64 MB board; packs are int8 (ncnn2table/ncnn2int8).
- mean/norm must match training: (x-127.5)*0.0078125 == [-1,1] scaling.
Every step emits a JSON-line on stdout for the Rust host to stream.
"""

import hashlib
import json
import shutil
import subprocess
from pathlib import Path

REPO = Path(__file__).resolve().parents[4]  # py/kbdk-convert/src/kbdk_convert -> repo root
NCNN_TOOLS = REPO / "board/ncnn/dist/host"


def emit(event: str, **kw):
    print(json.dumps({"event": event, **kw}), flush=True)


def md5(p: Path) -> str:
    return hashlib.md5(p.read_bytes()).hexdigest()


def parse_blob_names(param: Path) -> tuple[str, str]:
    """First Input layer's output blob and the last layer's last output blob.

    ncnn .param line: <type> <name> <#in> <#out> <in blobs...> <out blobs...> <k=v...>
    """
    in_blob = out_blob = None
    for line in param.read_text().splitlines()[2:]:
        f = line.split()
        if len(f) < 4:
            continue
        ltype, _name, nin, nout = f[0], f[1], int(f[2]), int(f[3])
        blobs = f[4 : 4 + nin + nout]
        outs = blobs[nin:]
        if ltype == "Input" and in_blob is None and outs:
            in_blob = outs[0]
        if outs:
            out_blob = outs[-1]
    if not in_blob or not out_blob:
        raise RuntimeError(f"could not parse blob names from {param}")
    return in_blob, out_blob


def pnnx_export(ts_model: Path, w: int, h: int) -> tuple[Path, Path]:
    import pnnx  # noqa: deferred heavy import
    import torch

    emit("step", name="pnnx", model=str(ts_model))
    pnnx.convert(str(ts_model), [torch.rand(1, 3, h, w)])
    stem = str(ts_model)
    if stem.endswith(".pt"):
        stem = stem[: -len(".pt")]
    param, binf = Path(f"{stem}.ncnn.param"), Path(f"{stem}.ncnn.bin")
    if not param.exists() or not binf.exists():
        raise RuntimeError("pnnx did not produce .ncnn.param/.bin")
    return param, binf


def quantize(
    param: Path,
    binf: Path,
    calib_images: list[Path],
    w: int,
    h: int,
    mean: list[float],
    norm: list[float],
    workdir: Path,
) -> tuple[Path, Path]:
    emit("step", name="quantize", images=len(calib_images))
    if not (NCNN_TOOLS / "ncnn2table").exists():
        raise RuntimeError(f"{NCNN_TOOLS}/ncnn2table missing — run board/ncnn/build.sh first")
    lst = workdir / "imagelist.txt"
    lst.write_text("\n".join(str(p.resolve()) for p in calib_images))
    table = workdir / "model.table"
    subprocess.run(
        [
            str(NCNN_TOOLS / "ncnn2table"),
            str(param),
            str(binf),
            str(lst),
            str(table),
            f"mean=[{','.join(map(str, mean))}]",
            f"norm=[{','.join(map(str, norm))}]",
            f"shape=[{w},{h},3]",
            "pixel=RGB",
            "thread=4",
            "method=kl",
        ],
        check=True,
        capture_output=True,
    )
    qparam, qbin = workdir / "model.param", workdir / "model.bin"
    subprocess.run(
        [str(NCNN_TOOLS / "ncnn2int8"), str(param), str(binf), str(qparam), str(qbin), str(table)],
        check=True,
        capture_output=True,
    )
    return qparam, qbin


def host_infer(param: Path, binf: Path, img_rgb, w, h, mean, norm, in_blob, out_blob):
    import ncnn
    import numpy as np

    net = ncnn.Net()
    net.opt.num_threads = 1
    net.load_param(str(param))
    net.load_model(str(binf))
    m = ncnn.Mat.from_pixels(img_rgb, ncnn.Mat.PixelType.PIXEL_RGB, w, h)
    m.substract_mean_normalize(mean, norm)
    ex = net.create_extractor()
    ex.input(in_blob, m)
    _, out = ex.extract(out_blob)
    return np.array(out).flatten()


def verify_parity(
    p32, b32, p8, b8, images, w, h, mean, norm, in_blob, out_blob, min_agree=0.8
) -> float:
    import numpy as np
    from PIL import Image

    agree = 0
    for ip in images:
        img = np.asarray(Image.open(ip).convert("RGB").resize((w, h)), dtype=np.uint8)
        a = host_infer(p32, b32, img, w, h, mean, norm, in_blob, out_blob)
        b = host_infer(p8, b8, img, w, h, mean, norm, in_blob, out_blob)
        agree += int(a.argmax() == b.argmax())
    frac = agree / len(images)
    emit("step", name="parity", top1_agreement=frac)
    if frac < min_agree:
        raise RuntimeError(f"int8 parity too low: {frac:.2f} < {min_agree}")
    return frac


def build_pack(
    name, task, backbone, qparam, qbin, labels, w, h, mean, norm, in_blob, out_blob, out_dir: Path
) -> Path:
    pack = out_dir / name
    pack.mkdir(parents=True, exist_ok=True)
    shutil.copy(qparam, pack / "model.param")
    shutil.copy(qbin, pack / "model.bin")
    (pack / "labels.txt").write_text("\n".join(labels))
    manifest = {
        "name": name,
        "task": task,
        "backbone": backbone,
        "input": {"width": w, "height": h, "mean": mean, "norm": norm},
        "quant": "int8",
        "blobs": {"in_blob": in_blob, "out_blob": out_blob},
        "files": {"param": "model.param", "bin": "model.bin", "labels_file": "labels.txt"},
        "md5": {"param": md5(pack / "model.param"), "bin": md5(pack / "model.bin")},
        "labels": labels,
    }
    (pack / "manifest.json").write_text(json.dumps(manifest, indent=2))
    emit("done", pack=str(pack))
    return pack
