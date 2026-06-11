#!/usr/bin/env python3
"""NPU capacity probe: build a synthetic multi-layer conv net (random weights)
as an NVJ1 job, run it on the V831 NPU, report end-to-end latency + ION use.

Used for the 2026-06-11 bigger-models research (docs/research/). Layers must
stay inside the verified envelope (input <= ~112^2, channels <= 128) — check
new shapes with scripts/nvdla_parity.py first.

Run:  uv run --project py python scripts/nvdla_capacity.py [--repeat 30]
"""

import argparse
import subprocess
import sys
from pathlib import Path

import numpy as np

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "py" / "kbdk-convert" / "src"))
from kbdk_convert import nvdla, nvdla_compile  # noqa: E402

REPO = Path(__file__).resolve().parents[1]


def sh(*cmd: str) -> str:
    r = subprocess.run(cmd, capture_output=True, text=True)
    return r.stdout + r.stderr


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--repeat", type=int, default=30)
    ap.add_argument("--size", type=int, default=112)
    a = ap.parse_args()

    rng = np.random.default_rng(0)

    def layer(cin, cout, k=3, pad=1, pool=0, relu=True):
        return nvdla_compile.QuantLayer(
            wq=rng.integers(-128, 128, size=(cout, cin, k, k), dtype=np.int8),
            b16=rng.integers(-100, 100, size=cout, dtype=np.int16),
            bias_lshift=0, scale=200, truncate=16, relu=relu, stride=1, pad=pad,
            pool=pool, pool_stride=2 if pool else 0)

    layers = [
        layer(3, 32, pool=2),      # 112 -> 56
        layer(32, 64, pool=2),     # 56 -> 28
        layer(64, 96),             # 28
        layer(96, 128, pool=2),    # 28 -> 14
        layer(128, 128), layer(128, 128), layer(128, 128), layer(128, 128),
        layer(128, 128, pool=2),   # 14 -> 7
        layer(128, 128), layer(128, 128), layer(128, 128),
        layer(128, 1000, k=1, pad=0, relu=False),  # 1000-class head @7
    ]
    wbytes = sum(l.wq.size for l in layers)
    job, info = nvdla_compile.build_job(layers, a.size, a.size)
    print(f"layers={len(layers)} weights={wbytes / 1e6:.2f} MB "
          f"ion={info['ion_size'] / 1e6:.2f} MB")

    Path("/tmp/capnet.nvj").write_bytes(job)
    Path("/tmp/capnet_in.bin").write_bytes(nvdla.pack_feature(
        rng.integers(-128, 128, size=(3, a.size, a.size), dtype=np.int8)))
    sh("adb", "push", "/tmp/capnet.nvj", "/tmp/capnet.nvj")
    sh("adb", "push", "/tmp/capnet_in.bin", "/tmp/capnet_in.bin")
    out = sh(str(REPO / "target/debug/kbdk"), "exec",
             f"/tmp/nna_runner /tmp/capnet.nvj /tmp/capnet_in.bin /tmp/capnet_out.bin {a.repeat}")
    for line in out.splitlines():
        if '"done"' in line or '"error"' in line:
            print(line.strip())
    return 0


if __name__ == "__main__":
    sys.exit(main())
