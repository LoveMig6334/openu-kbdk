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


def npu_slim(n_classes: int) -> nn.Sequential:
    """NPU-compatible tiny classifier (V831 NVDLA nv_small): plain 3x3 convs +
    BN (folded into the conv at compile time) + ReLU + 2x2 maxpool, head is a
    4x4 conv to 1x1 logits. No depthwise, no Linear, every conv's in_c <= 32
    (the verified NVDLA weight-layout limit). Input is fixed 64x64."""
    from collections import OrderedDict

    def block(i: int, cin: int, cout: int) -> list[tuple[str, nn.Module]]:
        # named entries: pnnx's generated python chokes on bare-Sequential
        # numeric attribute names (`self.2 = ...`)
        return [
            (f"conv{i}", nn.Conv2d(cin, cout, 3, 1, 1, bias=False)),
            (f"bn{i}", nn.BatchNorm2d(cout)),
            (f"relu{i}", nn.ReLU(inplace=True)),
            (f"pool{i}", nn.MaxPool2d(2, 2)),
        ]
    return nn.Sequential(OrderedDict(
        block(1, 3, 16)      # 64 -> 32
        + block(2, 16, 32)   # 32 -> 16
        + block(3, 32, 32)   # 16 -> 8
        + block(4, 32, 32)   # 8 -> 4
        + [("head", nn.Conv2d(32, n_classes, 4)),  # 4x4 -> 1x1 logits
           ("flatten", nn.Flatten())]
    ))


def npu_mid(n_classes: int) -> nn.Sequential:
    """Wider NPU classifier (112x112): channels 32->64->96->128, an extra
    128-ch stage at 7x7, 7x7-conv head to 1x1 logits. Everything inside the
    hardware-verified NVDLA envelope (channels <= 128, input <= 112^2, 7x7
    kernel parity-checked). ~350 KB int8, ~5 ms/inf on the NPU — the "use the
    headroom" backbone from docs/research/2026-06-11-bigger-npu-models.md."""
    from collections import OrderedDict

    def block(i: int, cin: int, cout: int, pool: bool = True):
        mods = [
            (f"conv{i}", nn.Conv2d(cin, cout, 3, 1, 1, bias=False)),
            (f"bn{i}", nn.BatchNorm2d(cout)),
            (f"relu{i}", nn.ReLU(inplace=True)),
        ]
        if pool:
            mods.append((f"pool{i}", nn.MaxPool2d(2, 2)))
        return mods

    return nn.Sequential(OrderedDict(
        block(1, 3, 32)        # 112 -> 56
        + block(2, 32, 64)     # 56 -> 28
        + block(3, 64, 96)     # 28 -> 14
        + block(4, 96, 128)    # 14 -> 7
        + block(5, 128, 128, pool=False)
        + [("head", nn.Conv2d(128, n_classes, 7)),  # 7x7 -> 1x1 logits
           ("flatten", nn.Flatten())]
    ))


def fold_input_norm(conv: nn.Conv2d, mean, std) -> nn.Conv2d:
    """Fold an ImageNet (p-mean)/std preprocessing into a stem conv so the
    folded conv accepts kbdk's [-1,1] inputs directly. With y = 2p-1:
    x = ((y+1)/2 - m)/s = y/(2s) + (0.5-m)/s, and conv(x) = conv'(y) where
    W'[:,c] = W[:,c]/(2 s_c) and b' = b + sum_c sum(W[:,c]) * (0.5-m_c)/s_c.
    Exact for interior pixels; the 1-px padded border deviates (padding zeros
    sit at p=mean in x-space but p=0.5 in y-space) — absorbed by fine-tuning."""
    import torch

    m = torch.tensor(mean, dtype=conv.weight.dtype)
    s = torch.tensor(std, dtype=conv.weight.dtype)
    out = nn.Conv2d(conv.in_channels, conv.out_channels, conv.kernel_size,
                    conv.stride, conv.padding, bias=True)
    with torch.no_grad():
        out.weight.copy_(conv.weight / (2 * s).view(1, -1, 1, 1))
        b = conv.bias.detach().clone() if conv.bias is not None \
            else torch.zeros(conv.out_channels)
        b += (conv.weight.sum(dim=(2, 3)) * ((0.5 - m) / s).view(1, -1)).sum(dim=1)
        out.bias.copy_(b)
    return out


def npu_repvgg(n_classes: int, pretrained: bool = False) -> nn.Sequential:
    """Truncated RepVGG-B0 transfer backbone for the V831 NPU: stem + stages
    0-1 (11 plain 3x3 convs after reparameterization, channels 64->128 — the
    deeper 256/1280-ch stages exceed the verified NVDLA envelope) + a fresh
    conv/pool/7x7 head. No BN at this point (reparam folded it), so fine-tune
    gently (lr ~2e-4). pretrained=True downloads timm repvgg_b0, fuses the
    branches, copies the weights in, and folds the ImageNet mean/std into the
    stem so the board's (x-128) input quantization stays unchanged.
    Input fixed 112x112."""
    from collections import OrderedDict

    def cr(name: str, cin: int, cout: int, stride: int):
        return [(f"{name}", nn.Conv2d(cin, cout, 3, stride, 1, bias=True)),
                (f"{name}_relu", nn.ReLU(inplace=True))]

    mods = cr("conv1", 3, 64, 2)          # 112 -> 56   (stem)
    mods += cr("conv2", 64, 64, 2)        # 56 -> 28    (stage0)
    for i in range(3, 6):
        mods += cr(f"conv{i}", 64, 64, 1)
    mods += cr("conv6", 64, 128, 2)       # 28 -> 14    (stage1)
    for i in range(7, 12):
        mods += cr(f"conv{i}", 128, 128, 1)
    # fresh head: conv + maxpool beats a stride-2 conv by ~5 pts on imagenette
    # few-shot, and the fused PDP pool costs only ~0.7 ms (an earlier "6 ms"
    # reading was NPU contention from a concurrently-serving runner)
    mods += cr("conv12", 128, 128, 1)
    mods.append(("pool", nn.MaxPool2d(2, 2)))  # 14 -> 7
    mods.append(("head", nn.Conv2d(128, n_classes, 7)))  # 7x7 -> 1x1 logits
    mods.append(("flatten", nn.Flatten()))
    model = nn.Sequential(OrderedDict(mods))

    if pretrained:
        import timm
        from timm.utils.model import reparameterize_model

        src = reparameterize_model(timm.create_model("repvgg_b0", pretrained=True).eval())
        cfg = src.pretrained_cfg
        blocks = [src.stem] + list(src.stages[0]) + list(src.stages[1])
        import torch
        with torch.no_grad():
            for i, blk in enumerate(blocks, start=1):
                dst: nn.Conv2d = getattr(model, f"conv{i}")
                dst.weight.copy_(blk.reparam_conv.weight)
                dst.bias.copy_(blk.reparam_conv.bias)
        folded = fold_input_norm(getattr(model, "conv1"), cfg["mean"], cfg["std"])
        setattr(model, "conv1", folded)
    return model


def make_model(backbone: str, n_classes: int) -> nn.Module:
    if backbone == "npu_slim":
        return npu_slim(n_classes)
    if backbone == "npu_mid":
        return npu_mid(n_classes)
    if backbone == "npu_repvgg":
        return npu_repvgg(n_classes, pretrained=True)
    if backbone == "mobilenet_v2":
        m = models.mobilenet_v2(weights=models.MobileNet_V2_Weights.IMAGENET1K_V1)
        m.classifier[1] = nn.Linear(m.last_channel, n_classes)
    elif backbone == "resnet18":
        m = models.resnet18(weights=models.ResNet18_Weights.IMAGENET1K_V1)
        m.fc = nn.Linear(m.fc.in_features, n_classes)
    else:
        raise ValueError(
            f"unknown backbone {backbone} "
            "(mobilenet_v2 | resnet18 | npu_slim | npu_mid | npu_repvgg)")
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
