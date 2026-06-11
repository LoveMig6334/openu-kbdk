#!/usr/bin/env python3
"""Evaluate a kbdk TorchScript classifier on an ImageFolder dir (top-1).

Used for the 10-shot transfer comparison (scratch npu_mid vs pretrained
npu_repvgg) on imagenette val. Normalization matches the platform:
(x-127.5)*0.0078125.

Run: uv run --project py python scripts/eval_classifier.py MODEL.pt DATA_DIR [--size 112]
"""

import argparse
from pathlib import Path

import torch
from torch.utils.data import DataLoader
from torchvision import datasets, transforms


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("model", type=Path)
    ap.add_argument("data", type=Path)
    ap.add_argument("--size", type=int, default=112)
    a = ap.parse_args()

    tf = transforms.Compose([
        transforms.Resize(int(a.size * 256 / 224)),
        transforms.CenterCrop(a.size),
        transforms.ToTensor(),
        transforms.Normalize([0.5] * 3, [0.5] * 3),
    ])
    ds = datasets.ImageFolder(str(a.data), tf)
    dl = DataLoader(ds, batch_size=64, num_workers=4)
    dev = torch.device("mps" if torch.backends.mps.is_available() else "cpu")
    m = torch.jit.load(str(a.model), map_location="cpu").eval().to(dev)
    correct = total = 0
    with torch.no_grad():
        for x, y in dl:
            pred = m(x.to(dev)).argmax(1).cpu()
            correct += (pred == y).sum().item()
            total += len(y)
    print(f"{a.model.name}: top-1 {correct / total:.3f} ({correct}/{total})")
    return 0


if __name__ == "__main__":
    main()
