"""Full convert pipeline on a tiny traced CNN: pnnx -> ncnn -> int8 -> pack."""

import json
from pathlib import Path

import numpy as np
import pytest
import torch
import torch.nn as nn
from PIL import Image

from kbdk_convert import cli, convert


class Tiny(nn.Module):
    def __init__(self):
        super().__init__()
        self.c1 = nn.Conv2d(3, 8, 3, 2, 1)
        self.r = nn.ReLU()
        self.c2 = nn.Conv2d(8, 4, 16)  # k=16 eats the 16x16 map -> 4x1x1, no flatten

    def forward(self, x):
        return self.c2(self.r(self.c1(x)))


@pytest.fixture
def dataset(tmp_path: Path) -> Path:
    data = tmp_path / "data"
    rng = np.random.default_rng(7)
    for cls in ["a", "b", "c", "d"]:
        d = data / cls
        d.mkdir(parents=True)
        for i in range(3):
            arr = (rng.random((32, 32, 3)) * 255).astype("uint8")
            Image.fromarray(arr).save(d / f"{i}.bmp")
    return data


def test_full_pipeline(tmp_path: Path, dataset: Path):
    ts = tmp_path / "tiny.pt"
    m = Tiny().eval()
    torch.jit.trace(m, torch.rand(1, 3, 32, 32)).save(str(ts))

    rc = cli.main(
        [
            "--model", str(ts),
            "--data", str(dataset),
            "--name", "tinytest",
            "--out", str(tmp_path / "packs"),
            "--width", "32",
            "--height", "32",
            "--backbone", "tiny",
            "--min-parity", "0.0",  # random weights: int8 argmax may diverge; plumbing is the test
        ]
    )
    assert rc == 0

    pack = tmp_path / "packs" / "tinytest"
    mani = json.loads((pack / "manifest.json").read_text())
    assert mani["blobs"]["in_blob"] == "in0"
    assert mani["blobs"]["out_blob"] == "out0"
    assert mani["labels"] == ["a", "b", "c", "d"]
    assert mani["files"]["labels_file"] == "labels.txt"
    for key, rel in [("param", "model.param"), ("bin", "model.bin")]:
        got = convert.md5(pack / rel)
        assert mani["md5"][key] == got


def test_parse_blob_names(tmp_path: Path):
    p = tmp_path / "x.param"
    p.write_text(
        "7767517\n2 2\n"
        "Input                    in0   0 1 in0\n"
        "Convolution              conv0 1 1 in0 out0 0=4 1=16\n"
    )
    assert convert.parse_blob_names(p) == ("in0", "out0")
