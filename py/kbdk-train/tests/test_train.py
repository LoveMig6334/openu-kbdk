"""Train on a trivially-separable toy dataset (solid colors + noise) on CPU."""

import numpy as np
import pytest
from PIL import Image

from kbdk_train.train import train


@pytest.fixture
def toy_dataset(tmp_path):
    rng = np.random.default_rng(0)
    colors = {"blue": (40, 40, 200), "green": (40, 200, 40), "red": (200, 40, 40)}
    for name, rgb in colors.items():
        d = tmp_path / "data" / name
        d.mkdir(parents=True)
        for i in range(12):
            arr = np.full((64, 64, 3), rgb, dtype=np.int16)
            arr = arr + rng.integers(-20, 20, arr.shape)
            Image.fromarray(arr.clip(0, 255).astype("uint8")).save(d / f"{i}.png")
    return tmp_path / "data"


def test_train_toy(tmp_path, toy_dataset):
    out = tmp_path / "model.pt"
    classes = train(
        toy_dataset, "mobilenet_v2", epochs=2, lr=1e-3, out=out,
        size=64, batch_size=8, device_str="cpu",
    )
    assert out.exists()
    assert classes == ["blue", "green", "red"]  # ImageFolder alphabetical


def test_npu_slim_backbone_shape():
    """npu_slim: NPU-compatible conv-only net (no depthwise/linear/BN at runtime),
    64x64 in -> [B, n_classes] logits out, traceable."""
    import torch
    from kbdk_train.train import make_model

    m = make_model("npu_slim", 3)
    out = m(torch.rand(2, 3, 64, 64))
    assert out.shape == (2, 3)
    torch.jit.trace(m.eval(), torch.rand(1, 3, 64, 64))


def test_npu_mid_backbone_shape():
    """npu_mid: the wider NPU classifier (112x112, channels to 128, 7x7-conv
    head) — stays inside the verified NVDLA envelope (<=128 ch, <=112^2)."""
    import torch
    from kbdk_train.train import make_model

    m = make_model("npu_mid", 5)
    out = m(torch.rand(2, 3, 112, 112))
    assert out.shape == (2, 5)
    torch.jit.trace(m.eval(), torch.rand(1, 3, 112, 112))


def test_npu_repvgg_backbone_shape():
    """npu_repvgg: truncated RepVGG-B0 flattened to a plain conv+relu stack
    (no BN — reparam already folded it), 112x112 -> [B, n_classes]."""
    import torch
    from kbdk_train.train import make_model, npu_repvgg

    m = npu_repvgg(4)  # no pretrained download in tests
    out = m(torch.rand(2, 3, 112, 112))
    assert out.shape == (2, 4)
    torch.jit.trace(m.eval(), torch.rand(1, 3, 112, 112))


def test_input_norm_fold():
    """Folding ImageNet mean/std into the stem makes the conv accept our
    [-1,1] inputs: W'.y + b' == W.((y+1)/2 - m)/s + b exactly."""
    import torch
    import torch.nn as nn
    from kbdk_train.train import fold_input_norm

    torch.manual_seed(0)
    conv = nn.Conv2d(3, 8, 3, 2, 1, bias=True)
    mean = (0.485, 0.456, 0.406)
    std = (0.229, 0.224, 0.225)
    y = torch.rand(1, 3, 16, 16) * 2 - 1
    x = ((y + 1) / 2 - torch.tensor(mean).view(1, 3, 1, 1)) / torch.tensor(std).view(1, 3, 1, 1)
    want = conv(x)
    folded = fold_input_norm(conv, mean, std)
    got = folded(y)
    # exact on the interior; the 1-px border differs (padding zeros mean
    # p=mean in x-space but p=0.5 in y-space) and is absorbed by fine-tuning
    assert torch.allclose(want[:, :, 1:-1, 1:-1], got[:, :, 1:-1, 1:-1], atol=1e-5)
    assert not torch.allclose(want, got, atol=1e-5)
