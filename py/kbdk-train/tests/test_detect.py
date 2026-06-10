"""Detection training smoke test: tiny synthetic YOLO dataset, 2 CPU epochs."""

import json

import numpy as np
import pytest
from PIL import Image, ImageDraw

from kbdk_train.detect import (
    YoloDataset,
    box_iou,
    decode_boxes,
    kmeans_anchors,
    nms,
    train_detection,
)


@pytest.fixture
def toy_det(tmp_path):
    rng = np.random.default_rng(0)
    colors = {"red": (200, 40, 40), "blue": (40, 70, 200)}
    (tmp_path / "images").mkdir()
    (tmp_path / "labels").mkdir()
    (tmp_path / "classes.txt").write_text("\n".join(colors))
    names = list(colors)
    for i in range(24):
        img = Image.new("RGB", (128, 128), (120, 120, 120))
        d = ImageDraw.Draw(img)
        cls = int(rng.integers(0, 2))
        r = int(rng.integers(20, 40))
        cx, cy = (int(v) for v in rng.integers(r, 128 - r, 2))
        d.ellipse((cx - r, cy - r, cx + r, cy + r), fill=colors[names[cls]])
        img.save(tmp_path / "images" / f"i{i:02d}.png")
        (tmp_path / "labels" / f"i{i:02d}.txt").write_text(
            f"{cls} {cx / 128:.4f} {cy / 128:.4f} {2 * r / 128:.4f} {2 * r / 128:.4f}"
        )
    return tmp_path


def test_dataset_and_anchors(toy_det):
    ds = YoloDataset(toy_det, size=224)
    assert len(ds) == 24 and ds.classes == ["red", "blue"]
    x, boxes = ds[0]
    assert x.shape == (3, 224, 224) and boxes.shape[1] == 5
    anchors = kmeans_anchors(ds)
    assert len(anchors) == 10 and all(a > 0 for a in anchors)


def test_box_iou_and_nms():
    assert box_iou((0.5, 0.5, 0.2, 0.2), (0.5, 0.5, 0.2, 0.2)) == pytest.approx(1.0)
    assert box_iou((0.2, 0.2, 0.1, 0.1), (0.8, 0.8, 0.1, 0.1)) == 0.0
    dets = [(0, 0.9, 0.5, 0.5, 0.2, 0.2), (0, 0.5, 0.51, 0.5, 0.2, 0.2), (1, 0.8, 0.5, 0.5, 0.2, 0.2)]
    kept = nms(dets, 0.45)
    assert len(kept) == 2  # duplicate same-class box suppressed; other class kept


def test_train_two_epochs(tmp_path, toy_det):
    out = tmp_path / "det.pt"
    meta = train_detection(
        toy_det, epochs=2, lr=1e-3, out=out, size=224, batch_size=8,
        device_str="cpu", pretrained=False,
    )
    assert out.exists()
    sidecar = json.loads((out.parent / "det.pt.meta.json").read_text())
    assert sidecar["classes"] == ["red", "blue"]
    assert sidecar["grid"] == 7 and len(sidecar["anchors"]) == 10
    assert meta["task"] == "detection"
    # decode runs on the traced model's output shape
    import torch

    m = torch.jit.load(str(out))
    raw = m(torch.rand(1, 3, 224, 224)).detach().numpy()[0]
    assert raw.shape == (5 * (5 + 2), 7, 7)
    decode_boxes(raw, sidecar["anchors"], 2, conf_thresh=0.99)  # shape-sane, no crash


def test_npu_det_backbone_shape():
    """npu_det: conv-only YOLOv2 head for the NVDLA path — 112x112 -> 7x7 grid,
    A*(5+C) channels, traceable, no depthwise/linear."""
    import torch
    from kbdk_train.detect import npu_det

    m = npu_det(n_classes=3, n_anchors=5)
    out = m(torch.rand(2, 3, 112, 112))
    assert out.shape == (2, 5 * (5 + 3), 7, 7)
    torch.jit.trace(m.eval(), torch.rand(1, 3, 112, 112))
