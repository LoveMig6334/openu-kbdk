"""kbdk-convert CLI: TorchScript model + dataset -> deployable int8 ncnn pack."""

import argparse
import json
import sys
import tempfile
import traceback
from pathlib import Path

from . import convert as C

IMG_EXTS = {".jpg", ".jpeg", ".png", ".bmp"}


def collect_images(data_dir: Path, limit: int) -> list[Path]:
    imgs = sorted(p for p in data_dir.rglob("*") if p.suffix.lower() in IMG_EXTS)
    step = max(1, len(imgs) // limit)
    return imgs[::step][:limit]


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(description="TorchScript -> int8 ncnn kbdk pack")
    ap.add_argument("--model", required=True, type=Path, help="TorchScript .pt")
    ap.add_argument("--data", required=True, type=Path, help="ImageFolder dataset (calibration + labels)")
    ap.add_argument("--name", required=True)
    ap.add_argument("--out", required=True, type=Path, help="output dir; pack lands at OUT/NAME/")
    ap.add_argument("--width", type=int, default=224)
    ap.add_argument("--height", type=int, default=224)
    ap.add_argument("--backbone", default="mobilenet_v2")
    ap.add_argument("--task", default="classification")
    ap.add_argument("--labels", type=Path, default=None,
                    help="labels.txt (one per line); default: class dirs of --data, sorted")
    ap.add_argument("--calib-images", type=int, default=64)
    ap.add_argument("--min-parity", type=float, default=0.8)
    args = ap.parse_args(argv)

    mean = [127.5, 127.5, 127.5]
    norm = [0.0078125, 0.0078125, 0.0078125]

    try:
        # detection models carry a meta sidecar written by kbdk-train
        meta = None
        meta_path = Path(str(args.model) + ".meta.json")
        if args.task == "detection" or meta_path.exists():
            if not meta_path.exists():
                raise RuntimeError(f"detection needs the meta sidecar: {meta_path}")
            meta = json.loads(meta_path.read_text())
            args.task = "detection"
            args.width = args.height = meta["size"]
            args.backbone = meta.get("backbone", args.backbone)

        if meta:
            labels = meta["classes"]
        elif args.labels:
            labels = args.labels.read_text().splitlines()
        else:
            labels = sorted(d.name for d in args.data.iterdir() if d.is_dir())
        if not labels:
            raise RuntimeError(f"no labels: {args.data} has no class subdirectories")

        param, binf = C.pnnx_export(args.model, args.width, args.height)
        in_blob, out_blob = C.parse_blob_names(param)
        C.emit("step", name="blobs", in_blob=in_blob, out_blob=out_blob)

        calib = collect_images(args.data, args.calib_images)
        if not calib:
            raise RuntimeError(f"no calibration images under {args.data}")

        with tempfile.TemporaryDirectory() as td:
            wd = Path(td)
            qparam, qbin = C.quantize(
                param, binf, calib, args.width, args.height, mean, norm, wd
            )
            if meta:
                C.verify_parity_detection(
                    param, binf, qparam, qbin, calib[:16],
                    args.width, args.height, mean, norm, in_blob, out_blob, meta,
                    min_agree=args.min_parity,
                )
            else:
                C.verify_parity(
                    param, binf, qparam, qbin, calib[:16],
                    args.width, args.height, mean, norm, in_blob, out_blob,
                    min_agree=args.min_parity,
                )
            C.build_pack(
                args.name, args.task, args.backbone, qparam, qbin, labels,
                args.width, args.height, mean, norm, in_blob, out_blob, args.out,
                detection=meta,
            )
        return 0
    except Exception as e:  # noqa: BLE001 - single reporting point for the host
        C.emit("error", msg=str(e), trace=traceback.format_exc(limit=3))
        return 1


if __name__ == "__main__":
    sys.exit(main())
