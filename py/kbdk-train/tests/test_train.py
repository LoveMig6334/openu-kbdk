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
