"""YOLOv2-slim detection: MobileNetV2 features + single-scale YOLOv2 head.

Dataset = YOLO/Darknet layout (images/, labels/*.txt `cls cx cy w h` normalized,
classes.txt). The exported TorchScript model returns the RAW head map
(B, A*(5+C), S, S) — pure convs, so pnnx/ncnn/int8 handle it; sigmoid/exp/
softmax decode + NMS run on the CPU (here for eval/parity, in kbrun on the
board). A sidecar model.meta.json carries anchors/grid/classes to the
converter. Normalization matches the rest of kbdk: (x-127.5)*0.0078125.
"""

import json
from pathlib import Path

import numpy as np
import torch
import torch.nn as nn
import torch.nn.functional as F
from PIL import Image
from torch.utils.data import DataLoader, Dataset
from torchvision import models

from .train import emit

GRID = 7
N_ANCHORS = 5


# ---------------------------------------------------------------- dataset ----
class YoloDataset(Dataset):
    def __init__(self, root: Path, size: int = 224, augment: bool = True):
        self.root = Path(root)
        self.size = size
        self.augment = augment
        self.classes = (self.root / "classes.txt").read_text().split()
        self.images = sorted((self.root / "images").iterdir())

    def __len__(self):
        return len(self.images)

    def boxes_for(self, idx: int) -> list[list[float]]:
        lbl = self.root / "labels" / (self.images[idx].stem + ".txt")
        out = []
        if lbl.exists():
            for line in lbl.read_text().splitlines():
                f = line.split()
                if len(f) == 5:
                    out.append([float(x) for x in f])  # cls cx cy w h
        return out

    def __getitem__(self, idx):
        img = Image.open(self.images[idx]).convert("RGB").resize((self.size, self.size))
        boxes = self.boxes_for(idx)
        arr = np.asarray(img, dtype=np.float32)
        if self.augment and np.random.rand() < 0.5:  # horizontal flip
            arr = arr[:, ::-1].copy()
            boxes = [[c, 1.0 - cx, cy, w, h] for c, cx, cy, w, h in boxes]
        x = torch.from_numpy((arr - 127.5) * 0.0078125).permute(2, 0, 1)
        return x, torch.tensor(boxes, dtype=torch.float32).reshape(-1, 5)


def collate(batch):
    xs, bs = zip(*batch)
    return torch.stack(xs), list(bs)


# ---------------------------------------------------------------- anchors ----
def kmeans_anchors(dataset: YoloDataset, k: int = N_ANCHORS, grid: int = GRID) -> list[float]:
    """k-means on box (w, h) in grid units with 1-IoU distance (YOLOv2 paper)."""
    wh = np.array(
        [[b[3] * grid, b[4] * grid] for i in range(len(dataset)) for b in dataset.boxes_for(i)]
    )
    if len(wh) < k:
        return [1.0, 1.0, 2.0, 2.0, 3.0, 3.0, 4.0, 4.0, 5.0, 5.0][: 2 * k]

    def iou(box, centers):
        inter = np.minimum(box[0], centers[:, 0]) * np.minimum(box[1], centers[:, 1])
        return inter / (box[0] * box[1] + centers[:, 0] * centers[:, 1] - inter)

    rng = np.random.default_rng(0)
    centers = wh[rng.choice(len(wh), k, replace=False)]
    for _ in range(30):
        assign = np.array([np.argmax(iou(b, centers)) for b in wh])
        new = np.array(
            [wh[assign == i].mean(axis=0) if (assign == i).any() else centers[i] for i in range(k)]
        )
        if np.allclose(new, centers, atol=1e-4):
            break
        centers = new
    centers = centers[np.argsort(centers[:, 0] * centers[:, 1])]
    return [round(float(v), 4) for v in centers.flatten()]


# ------------------------------------------------------------------ model ----
def npu_det(n_classes: int, n_anchors: int = N_ANCHORS) -> nn.Sequential:
    """Conv-only YOLOv2 detector for the V831 NPU (NVDLA nv_small): plain convs
    + BN (compile-time folded) + ReLU + maxpool, 1x1-conv head to the raw
    (A*(5+C))xSxS map. 112x112 input -> 7x7 grid (4 pools). No depthwise (the
    hardware has none) — this is the NPU stand-in for the MobileNetV2 backbone.
    Flat named Sequential so kbdk_convert.nvdla_compile can walk it."""
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
        block(1, 3, 16)        # 112 -> 56
        + block(2, 16, 32)     # 56 -> 28
        + block(3, 32, 48)     # 28 -> 14
        + block(4, 48, 64)     # 14 -> 7
        + block(5, 64, 64, pool=False)
        + [("head", nn.Conv2d(64, n_anchors * (5 + n_classes), 1))]
    ))


class YoloV2Slim(nn.Module):
    def __init__(self, n_classes: int, n_anchors: int = N_ANCHORS, pretrained: bool = True):
        super().__init__()
        w = models.MobileNet_V2_Weights.IMAGENET1K_V1 if pretrained else None
        self.backbone = models.mobilenet_v2(weights=w).features  # (B,1280,S,S)
        self.head = nn.Sequential(
            nn.Conv2d(1280, 256, 3, padding=1),
            nn.ReLU(inplace=True),
            nn.Conv2d(256, n_anchors * (5 + n_classes), 1),
        )

    def forward(self, x):
        return self.head(self.backbone(x))  # raw map


# ------------------------------------------------------------------- loss ----
def yolo_loss(pred, target_boxes, anchors, n_classes, device):
    """YOLOv2-style loss. pred: (B, A*(5+C), S, S); target_boxes: list of (N,5)."""
    B, _, S, _ = pred.shape
    A = len(anchors) // 2
    anc = torch.tensor(anchors, device=device).view(A, 2)
    p = pred.view(B, A, 5 + n_classes, S, S)
    tx, ty, tw, th, tconf = p[:, :, 0], p[:, :, 1], p[:, :, 2], p[:, :, 3], p[:, :, 4]
    tcls = p[:, :, 5:]  # (B,A,C,S,S)

    obj_mask = torch.zeros(B, A, S, S, device=device, dtype=torch.bool)
    txy_t = torch.zeros(B, A, 2, S, S, device=device)
    twh_t = torch.zeros(B, A, 2, S, S, device=device)
    cls_t = torch.zeros(B, A, S, S, device=device, dtype=torch.long)

    for b, boxes in enumerate(target_boxes):
        for cls, cx, cy, w, h in boxes.tolist():
            gj = min(int(cx * S), S - 1)
            gi = min(int(cy * S), S - 1)
            gw, gh = w * S, h * S
            inter = torch.minimum(anc[:, 0], torch.tensor(gw, device=device)) * torch.minimum(
                anc[:, 1], torch.tensor(gh, device=device)
            )
            ious = inter / (anc[:, 0] * anc[:, 1] + gw * gh - inter)
            a = int(ious.argmax())
            obj_mask[b, a, gi, gj] = True
            txy_t[b, a, 0, gi, gj] = cx * S - gj
            txy_t[b, a, 1, gi, gj] = cy * S - gi
            twh_t[b, a, 0, gi, gj] = float(np.log(max(gw, 1e-4) / float(anc[a, 0])))
            twh_t[b, a, 1, gi, gj] = float(np.log(max(gh, 1e-4) / float(anc[a, 1])))
            cls_t[b, a, gi, gj] = int(cls)

    n_obj = obj_mask.sum().clamp(min=1).float()
    loss_xy = F.mse_loss(torch.sigmoid(tx)[obj_mask], txy_t[:, :, 0][obj_mask], reduction="sum")
    loss_xy += F.mse_loss(torch.sigmoid(ty)[obj_mask], txy_t[:, :, 1][obj_mask], reduction="sum")
    loss_wh = F.mse_loss(tw[obj_mask], twh_t[:, :, 0][obj_mask], reduction="sum")
    loss_wh += F.mse_loss(th[obj_mask], twh_t[:, :, 1][obj_mask], reduction="sum")
    loss_obj = F.binary_cross_entropy_with_logits(
        tconf[obj_mask], torch.ones_like(tconf[obj_mask]), reduction="sum"
    )
    loss_noobj = F.binary_cross_entropy_with_logits(
        tconf[~obj_mask], torch.zeros_like(tconf[~obj_mask]), reduction="sum"
    )
    loss_cls = F.cross_entropy(
        tcls.permute(0, 1, 3, 4, 2)[obj_mask], cls_t[obj_mask], reduction="sum"
    )
    return (5.0 * (loss_xy + loss_wh) + loss_obj + 0.5 * loss_noobj + loss_cls) / n_obj


# ----------------------------------------------------------------- decode ----
def decode_boxes(raw, anchors, n_classes, conf_thresh=0.5):
    """raw: numpy (A*(5+C), S, S) -> [(cls, conf, cx, cy, w, h)] normalized."""
    A = len(anchors) // 2
    S = raw.shape[-1]
    p = raw.reshape(A, 5 + n_classes, S, S)
    out = []
    for a in range(A):
        aw, ah = anchors[2 * a], anchors[2 * a + 1]
        for gi in range(S):
            for gj in range(S):
                v = p[a, :, gi, gj]
                obj = 1.0 / (1.0 + np.exp(-v[4]))
                cl = v[5:] - v[5:].max()
                sm = np.exp(cl)
                sm /= sm.sum()
                cls = int(sm.argmax())
                conf = float(obj * sm[cls])
                if conf < conf_thresh:
                    continue
                cx = (gj + 1.0 / (1.0 + np.exp(-v[0]))) / S
                cy = (gi + 1.0 / (1.0 + np.exp(-v[1]))) / S
                w = aw * float(np.exp(min(v[2], 8))) / S
                h = ah * float(np.exp(min(v[3], 8))) / S
                out.append((cls, conf, cx, cy, w, h))
    return out


def box_iou(b1, b2):
    """(cx,cy,w,h) IoU"""
    x1a, y1a = b1[0] - b1[2] / 2, b1[1] - b1[3] / 2
    x2a, y2a = b1[0] + b1[2] / 2, b1[1] + b1[3] / 2
    x1b, y1b = b2[0] - b2[2] / 2, b2[1] - b2[3] / 2
    x2b, y2b = b2[0] + b2[2] / 2, b2[1] + b2[3] / 2
    iw = max(0.0, min(x2a, x2b) - max(x1a, x1b))
    ih = max(0.0, min(y2a, y2b) - max(y1a, y1b))
    inter = iw * ih
    union = b1[2] * b1[3] + b2[2] * b2[3] - inter
    return inter / union if union > 0 else 0.0


def nms(dets, iou_thresh=0.45):
    """dets: [(cls, conf, cx, cy, w, h)] -> kept, per-class NMS."""
    dets = sorted(dets, key=lambda d: -d[1])
    kept = []
    for d in dets:
        if all(d[0] != k[0] or box_iou(d[2:], k[2:]) < iou_thresh for k in kept):
            kept.append(d)
    return kept


# ------------------------------------------------------------------ train ----
def eval_detection_rate(model, ds, anchors, n_classes, device, n=24):
    """Fraction of GT boxes matched by a prediction (same class, IoU>0.5)."""
    model.eval()
    hit = tot = 0
    with torch.no_grad():
        for i in range(min(n, len(ds))):
            x, gts = ds[i]
            raw = model(x.unsqueeze(0).to(device)).cpu().numpy()[0]
            dets = nms(decode_boxes(raw, anchors, n_classes, 0.3))
            for g in gts.tolist():
                tot += 1
                if any(
                    int(g[0]) == d[0] and box_iou(g[1:], d[2:]) > 0.5 for d in dets
                ):
                    hit += 1
    return hit / max(tot, 1)


def train_detection(
    data_dir: Path,
    epochs: int,
    lr: float,
    out: Path,
    size: int = 224,
    batch_size: int = 16,
    device_str: str | None = None,
    pretrained: bool = True,
    backbone: str = "mobilenet_v2",
) -> dict:
    device = torch.device(
        device_str or ("mps" if torch.backends.mps.is_available() else "cpu")
    )
    npu = backbone == "npu_slim"
    if npu and size != 16 * GRID:
        raise ValueError(f"npu_slim detection wants --size {16 * GRID} (4 pools -> {GRID}x{GRID} grid)")
    ds = YoloDataset(data_dir, size=size, augment=True)
    val = YoloDataset(data_dir, size=size, augment=False)
    n_classes = len(ds.classes)
    anchors = kmeans_anchors(ds)
    dl = DataLoader(ds, batch_size=batch_size, shuffle=True, collate_fn=collate)
    model = (npu_det(n_classes) if npu
             else YoloV2Slim(n_classes, pretrained=pretrained)).to(device)
    opt = torch.optim.AdamW(model.parameters(), lr=lr)
    emit("start", classes=ds.classes, n_train=len(ds), anchors=anchors, device=str(device))
    for ep in range(epochs):
        model.train()
        tot = 0.0
        for x, boxes in dl:
            x = x.to(device)
            opt.zero_grad()
            loss = yolo_loss(model(x), boxes, anchors, n_classes, device)
            loss.backward()
            opt.step()
            tot += loss.item() * len(x)
        det_rate = eval_detection_rate(model, val, anchors, n_classes, device)
        emit("epoch", n=ep + 1, loss=tot / len(ds), det_rate=det_rate)
    model.eval().cpu()
    ts = torch.jit.trace(model, torch.rand(1, 3, size, size))
    ts.save(str(out))
    meta = {
        "task": "detection",
        "backbone": "npu-det" if npu else "yolov2-slim-mbv2",
        "size": size,
        "grid": GRID,
        "anchors": anchors,
        "classes": ds.classes,
        "conf_threshold": 0.5,
        "nms_threshold": 0.45,
    }
    Path(str(out) + ".meta.json").write_text(json.dumps(meta, indent=2))
    emit("saved", path=str(out), meta=meta)
    return meta
