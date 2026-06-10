"""kbdk-train CLI: ImageFolder dataset -> fine-tuned TorchScript model."""

import argparse
import sys
import traceback
from pathlib import Path

from .train import emit, train


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(description="fine-tune a backbone on an ImageFolder dataset")
    ap.add_argument("--data", required=True, type=Path)
    ap.add_argument("--out", required=True, type=Path, help="TorchScript output .pt")
    ap.add_argument("--task", default="classification", choices=["classification", "detection"])
    ap.add_argument("--backbone", default="mobilenet_v2")
    ap.add_argument("--epochs", type=int, default=5)
    ap.add_argument("--lr", type=float, default=1e-3)
    ap.add_argument("--size", type=int, default=224)
    ap.add_argument("--batch-size", type=int, default=32)
    ap.add_argument("--device", default=None, help="mps | cpu (default: mps if available)")
    ap.add_argument("--labels-out", type=Path, default=None)
    args = ap.parse_args(argv)

    try:
        args.out.parent.mkdir(parents=True, exist_ok=True)
        if args.task == "detection":
            from .detect import train_detection

            train_detection(
                args.data,
                args.epochs,
                args.lr,
                args.out,
                size=args.size,
                batch_size=args.batch_size,
                device_str=args.device,
            )
            return 0
        classes = train(
            args.data,
            args.backbone,
            args.epochs,
            args.lr,
            args.out,
            size=args.size,
            batch_size=args.batch_size,
            device_str=args.device,
        )
        if args.labels_out:
            args.labels_out.write_text("\n".join(classes))
        return 0
    except Exception as e:  # noqa: BLE001 - single reporting point for the host
        emit("error", msg=str(e), trace=traceback.format_exc(limit=3))
        return 1


if __name__ == "__main__":
    sys.exit(main())
