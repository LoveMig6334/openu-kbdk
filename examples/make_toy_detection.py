"""Generate a toy YOLO-format detection dataset (red/green/blue shapes).

Layout (what labelImg/Roboflow "YOLO" exports look like):
    out/
      classes.txt          one class name per line
      images/img_NNN.png
      labels/img_NNN.txt   lines: <cls> <cx> <cy> <w> <h>   (normalized 0..1)

1-3 shapes per image with exact ground-truth boxes; gradient lighting, noise
and blur so the detector must generalize.

Usage: uv run --with pillow --with numpy python examples/make_toy_detection.py [out_dir]
"""

import sys
from pathlib import Path

import numpy as np
from PIL import Image, ImageDraw, ImageFilter

OUT = Path(sys.argv[1] if len(sys.argv) > 1 else "examples/toy-detection")
N_IMAGES = 150
SIZE = 256
CLASSES = {
    "red": (200, 35, 35),
    "green": (40, 180, 50),
    "blue": (40, 70, 200),
}

rng = np.random.default_rng(1234)
(OUT / "images").mkdir(parents=True, exist_ok=True)
(OUT / "labels").mkdir(parents=True, exist_ok=True)
(OUT / "classes.txt").write_text("\n".join(CLASSES))

names = list(CLASSES)
for i in range(N_IMAGES):
    bg = int(rng.integers(60, 200))
    img = Image.new("RGB", (SIZE, SIZE), (bg, bg, bg))
    draw = ImageDraw.Draw(img)
    lines = []
    for _ in range(int(rng.integers(1, 4))):
        cls = int(rng.integers(0, len(names)))
        r, g, b = CLASSES[names[cls]]
        rx, ry = rng.integers(22, 60, 2)
        cx = int(rng.integers(rx, SIZE - rx))
        cy = int(rng.integers(ry, SIZE - ry))
        jit = rng.integers(-30, 30, 3)
        col = tuple(int(np.clip(c + j, 0, 255)) for c, j in zip((r, g, b), jit))
        box = (cx - rx, cy - ry, cx + rx, cy + ry)
        if rng.random() < 0.5:
            draw.ellipse(box, fill=col)
        else:
            draw.rounded_rectangle(box, radius=int(rng.integers(6, 20)), fill=col)
        lines.append(
            f"{cls} {cx / SIZE:.6f} {cy / SIZE:.6f} {2 * rx / SIZE:.6f} {2 * ry / SIZE:.6f}"
        )
    arr = np.asarray(img).astype(np.int16)
    gx = np.linspace(-1, 1, SIZE)[None, :] * rng.integers(-35, 35)
    gy = np.linspace(-1, 1, SIZE)[:, None] * rng.integers(-35, 35)
    arr = arr + (gx + gy)[..., None] + rng.integers(-10, 10, arr.shape)
    img = Image.fromarray(arr.clip(0, 255).astype("uint8"))
    img = img.filter(ImageFilter.GaussianBlur(radius=float(rng.random())))
    img.save(OUT / "images" / f"img_{i:03d}.png")
    (OUT / "labels" / f"img_{i:03d}.txt").write_text("\n".join(lines))

print(f"wrote {N_IMAGES} images + labels under {OUT}")
