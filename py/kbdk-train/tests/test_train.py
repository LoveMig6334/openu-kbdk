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
