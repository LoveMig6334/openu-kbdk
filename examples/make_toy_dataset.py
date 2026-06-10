"""Generate a 3-class toy ImageFolder dataset (red/green/blue objects).

Procedural "photos": a colored blob with gradient lighting, random size and
position, on a varied gray background with noise — enough variation that the
classifier must actually look at color + shape, not memorize pixels.

Usage: uv run --with pillow --with numpy python examples/make_toy_dataset.py [out_dir]
"""

import sys
from pathlib import Path

import numpy as np
from PIL import Image, ImageDraw, ImageFilter

OUT = Path(sys.argv[1] if len(sys.argv) > 1 else "examples/toy-dataset")
N_PER_CLASS = 40
SIZE = 256
COLORS = {
    "red": (200, 35, 35),
    "green": (40, 180, 50),
    "blue": (40, 70, 200),
}

rng = np.random.default_rng(42)

for name, (r, g, b) in COLORS.items():
    d = OUT / name
    d.mkdir(parents=True, exist_ok=True)
    for i in range(N_PER_CLASS):
        bg = int(rng.integers(60, 200))
        img = Image.new("RGB", (SIZE, SIZE), (bg, bg, bg))
        draw = ImageDraw.Draw(img)
        # blob: ellipse or rounded rect, random geometry + per-image color jitter
        cx, cy = rng.integers(70, SIZE - 70, 2)
        rx, ry = rng.integers(40, 90, 2)
        jit = rng.integers(-30, 30, 3)
        col = tuple(int(np.clip(c + j, 0, 255)) for c, j in zip((r, g, b), jit))
        box = (cx - rx, cy - ry, cx + rx, cy + ry)
        if rng.random() < 0.5:
            draw.ellipse(box, fill=col)
        else:
            draw.rounded_rectangle(box, radius=int(rng.integers(8, 30)), fill=col)
        # gradient "lighting" + blur + sensor-ish noise
        arr = np.asarray(img).astype(np.int16)
        gx = np.linspace(-1, 1, SIZE)[None, :] * rng.integers(-40, 40)
        gy = np.linspace(-1, 1, SIZE)[:, None] * rng.integers(-40, 40)
        arr = arr + (gx + gy)[..., None]
        arr = arr + rng.integers(-12, 12, arr.shape)
        img = Image.fromarray(arr.clip(0, 255).astype("uint8"))
        img = img.filter(ImageFilter.GaussianBlur(radius=float(rng.random() * 1.5)))
        img.save(d / f"{name}_{i:03d}.png")

print(f"wrote {N_PER_CLASS} images x {len(COLORS)} classes under {OUT}")
