# kbdk Detection (Phase 7) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Object detection matching the official KidBright µAI feature: train YOLOv2-slim on the Mac, deploy as an int8 ncnn pack, board draws live bounding boxes on the panel, UI overlays them on the camera preview.

**Architecture:** MobileNetV2 features (proven 466 ms int8 on the board) + YOLOv2 head (Conv 1280→256→A·(5+C) @ 7×7 grid, 5 anchors). Raw conv map is exported (trace-friendly, pure convs for pnnx/ncnn/int8); sigmoid/softmax/anchor decode + NMS run on the CPU both board-side (kbrun) and host-side (python, for training eval + int8 parity). Dataset format = YOLO/Darknet (`images/`, `labels/*.txt` with `cls cx cy w h` normalized, `classes.txt`) — what labelImg/Roboflow export.

**Key geometric fact:** the fb preview and the net input are the *same centre square* of the capture, so normalized box coords map straight onto the 240×240 panel and the UI preview — no remapping.

**Anchors:** k-means (IoU distance) over the dataset's box w/h in grid units, computed at train time. Train writes a sidecar `model.meta.json` {task, anchors, grid, classes, size}; convert reads it into the manifest. Pack manifest gains a `detection` object {anchors, grid, conf_threshold 0.5, nms_threshold 0.45}.

### Task 1: toy detection dataset generator
`examples/make_toy_detection.py` — 150 synthetic 256² images, 1–3 colored shapes (red/green/blue) with exact YOLO labels, gradients/noise/blur like the classification toy set.

### Task 2: kbdk-train detection
`py/kbdk-train/src/kbdk_train/detect.py`: YoloDataset (resize to S·32, h-flip aug), `kmeans_anchors`, `YoloV2Slim` (mobilenet_v2 features + head), YOLOv2 loss (responsible cell + best-anchor assignment; MSE coords ×5, BCE conf w/ λ_noobj 0.5, CE class), `decode_boxes` + `nms` (shared by eval/parity), train loop emitting JSON-lines, val metric = detection rate (best-IoU>0.5 & class match), TorchScript trace export + meta sidecar. CLI: `--task detection`. pytest: 2-epoch CPU run on a 24-image toy set → loss drops, model+meta saved.

### Task 3: convert detection packs
`kbdk-convert`: `--task detection` reads meta sidecar (or flags), parity = decoded-box agreement fp32 vs int8 (same top box class + IoU>0.5 on N val images), manifest gains detection fields + labels from classes. kbdk-convert depends on kbdk-train (workspace dep) for decode/nms.

### Task 4: manifest plumbing
pack.rs `DetectionSpec` (serde default None), manifest.h parses anchors (variable-length float list), grid + thresholds.

### Task 5: kbrun detection
Detection path in the worker: forward → decode (sigmoid/exp/anchors) → NMS → JSON `{"event":"result","ms":..,"boxes":[{label,index,conf,x,y,w,h}]}` (x,y = top-left, normalized) → fb overlay (outline rects + class-colored labels via fb_rect/fb_text, normalized→240×240). `--image` mode prints boxes for parity. Verify on hardware vs host decode on the same raw image.

### Task 6: UI box overlay
deploy_tab: parse `boxes`, paint rect_stroke + label over the preview texture rect; result card lists boxes. Verify with live run screenshot.

### Task 7: docs + merge
CLAUDE.md (detection format, decode notes, measured latency), README quickstart for detection, merge to main.
