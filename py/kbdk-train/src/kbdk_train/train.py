"""Fine-tune a torchvision backbone on an ImageFolder dataset; export TorchScript.

Training normalization MUST match the board runner and the convert step:
(x - 127.5) * 0.0078125 == [-1, 1] scaling, i.e. Normalize(0.5, 0.5) on [0,1]
tensors. Progress goes to stdout as JSON-lines for the Rust host.
"""

import json
from pathlib import Path

import torch
import torch.nn as nn
from torch.utils.data import DataLoader
from torchvision import datasets, models, transforms


def emit(event, **kw):
    print(json.dumps({"event": event, **kw}), flush=True)


def make_transforms(size: int):
    return transforms.Compose(
        [
            transforms.Resize(int(size * 256 / 224)),
            transforms.CenterCrop(size),
            transforms.RandomHorizontalFlip(),
            transforms.ToTensor(),                  # [0,1]
            transforms.Normalize([0.5] * 3, [0.5] * 3),  # -> [-1,1]
        ]
    )


def make_model(backbone: str, n_classes: int) -> nn.Module:
    if backbone == "mobilenet_v2":
        m = models.mobilenet_v2(weights=models.MobileNet_V2_Weights.IMAGENET1K_V1)
        m.classifier[1] = nn.Linear(m.last_channel, n_classes)
    elif backbone == "resnet18":
        m = models.resnet18(weights=models.ResNet18_Weights.IMAGENET1K_V1)
        m.fc = nn.Linear(m.fc.in_features, n_classes)
    else:
        raise ValueError(f"unknown backbone {backbone} (mobilenet_v2 | resnet18)")
    return m


def train(
    data_dir: Path,
    backbone: str,
    epochs: int,
    lr: float,
    out: Path,
    size: int = 224,
    batch_size: int = 32,
    device_str: str | None = None,
) -> list[str]:
    device = torch.device(
        device_str or ("mps" if torch.backends.mps.is_available() else "cpu")
    )
    ds = datasets.ImageFolder(str(data_dir), make_transforms(size))
    n_val = max(1, len(ds) // 10)
    tr, va = torch.utils.data.random_split(
        ds, [len(ds) - n_val, n_val], generator=torch.Generator().manual_seed(0)
    )
    dl = DataLoader(tr, batch_size=batch_size, shuffle=True)
    dv = DataLoader(va, batch_size=batch_size)
    model = make_model(backbone, len(ds.classes)).to(device)
    opt = torch.optim.AdamW(model.parameters(), lr=lr)
    lossf = nn.CrossEntropyLoss()
    emit("start", classes=ds.classes, n_train=len(tr), n_val=len(va), device=str(device))
    for ep in range(epochs):
        model.train()
        tot = 0.0
        for x, y in dl:
            x, y = x.to(device), y.to(device)
            opt.zero_grad()
            loss = lossf(model(x), y)
            loss.backward()
            opt.step()
            tot += loss.item() * len(y)
        model.eval()
        correct = 0
        with torch.no_grad():
            for x, y in dv:
                correct += (model(x.to(device)).argmax(1).cpu() == y).sum().item()
        emit("epoch", n=ep + 1, loss=tot / len(tr), val_acc=correct / len(va))
    model.eval().cpu()
    ts = torch.jit.trace(model, torch.rand(1, 3, size, size))
    ts.save(str(out))
    emit("saved", path=str(out), classes=ds.classes)
    return ds.classes
