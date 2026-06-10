"""NVDLA nv_small (V831 NPU) model compiler primitives.

The V831's NPU is an NVIDIA NVDLA nv_small core, driven from userspace by the
GPLv3 runner (board/nvdla, built on third_party/v831-npu). This module is the
host side: pack int8 tensors into the memory layouts the hardware reads, model
the conv+SDP(+PDP) integer pipeline exactly, and serialize "NVJ1" job files the
board runner executes layer by layer.

Layouts (established by third_party/v831-npu's format test, verified on
hardware by scripts/nvdla_parity.py):

- feature cube [C,H,W]: channels split into surfaces of 8; within a surface,
  1x1x8 cubes ordered C' -> W -> H -> surface. Partial surfaces still occupy
  8 bytes per pixel (padded with zeros).
- DC int8 weights [K,C,KH,KW]: kernels in groups of 8; within a group, spatial
  positions row-major; per position, each kernel's 1x1xC cube back to back.
  The last group packs tight (no padding to 8). C <= 32 only — the >32
  channel-group ordering is not verified yet.

SDP math per layer (out_cvt semantics pinned down by the parity harness, byte-
exact on hardware): acc(int32) -> + bias<<bias_lshift -> *out_scale, then
>> out_truncate with ROUND HALF AWAY FROM ZERO -> relu -> sat8.
(Half-up matched 32766/32768 — the two misses were negative exact-halves.)
"""

from __future__ import annotations

import struct
from dataclasses import dataclass

import numpy as np

ATOM = 8  # NNA_ATOMIC_{C,K}_SIZE / NNA_MEMORY_ATOMIC_SIZE on nv_small


# ---- tensor layout packing -------------------------------------------------------

def pack_feature(x: np.ndarray) -> bytes:
    """int8 [C,H,W] -> packed feature-format bytes (surfaces of 8 channels)."""
    assert x.dtype == np.int8 and x.ndim == 3
    c, h, w = x.shape
    surfaces = (c + ATOM - 1) // ATOM
    padded = np.zeros((surfaces * ATOM, h, w), dtype=np.int8)
    padded[:c] = x
    # [S*8,H,W] -> [S,8,H,W] -> [S,H,W,8] -> flat
    return padded.reshape(surfaces, ATOM, h, w).transpose(0, 2, 3, 1).tobytes()


def unpack_feature(buf: bytes, c: int, h: int, w: int) -> np.ndarray:
    """packed feature-format bytes -> int8 [C,H,W]."""
    surfaces = (c + ATOM - 1) // ATOM
    arr = np.frombuffer(buf, dtype=np.int8, count=surfaces * ATOM * h * w)
    arr = arr.reshape(surfaces, h, w, ATOM).transpose(0, 3, 1, 2).reshape(surfaces * ATOM, h, w)
    return arr[:c].copy()


def feature_size(c: int, h: int, w: int) -> int:
    return ((c + ATOM - 1) // ATOM) * ATOM * h * w


def pack_weights(wt: np.ndarray) -> bytes:
    """int8 [K,C,KH,KW] -> direct-conv weight bytes (kernel groups of 8)."""
    assert wt.dtype == np.int8 and wt.ndim == 4
    k, c, kh, kw = wt.shape
    if c > 32:
        raise ValueError("kernel channel > 32: NVDLA channel-group ordering not verified yet")
    parts = []
    for g in range(0, k, ATOM):
        grp = wt[g:g + ATOM]  # [kg,C,KH,KW]
        # spatial row-major -> kernel-in-group -> channel
        parts.append(grp.transpose(2, 3, 0, 1).tobytes())
    return b"".join(parts)


# ---- int-exact pipeline reference ------------------------------------------------

def ref_conv_sdp(x: np.ndarray, wt: np.ndarray, bias: np.ndarray, *, stride: int,
                 pad: int, bias_lshift: int, scale: int, truncate: int,
                 relu: bool, rounding: bool) -> np.ndarray:
    """Model conv(int8) -> +bias<<sh -> *scale>>truncate -> relu -> sat8 exactly."""
    c, h, w = x.shape
    k, _, kh, kw = wt.shape
    out_h = (h - kh + 2 * pad) // stride + 1
    out_w = (w - kw + 2 * pad) // stride + 1
    xp = np.zeros((c, h + 2 * pad, w + 2 * pad), dtype=np.int64)
    xp[:, pad:pad + h, pad:pad + w] = x
    w64 = wt.astype(np.int64)
    acc = np.zeros((k, out_h, out_w), dtype=np.int64)
    for ky in range(kh):
        for kx in range(kw):
            patch = xp[:, ky:ky + (out_h - 1) * stride + 1:stride,
                       kx:kx + (out_w - 1) * stride + 1:stride]
            acc += np.tensordot(w64[:, :, ky, kx], patch, axes=(1, 0))
    acc += (bias.astype(np.int64) << bias_lshift)[:, None, None]
    v = acc * scale
    if truncate:
        if rounding:  # round half away from zero (hardware semantics)
            half = np.int64(1) << (truncate - 1)
            mag = (np.abs(v) + half) >> truncate
            v = np.sign(v) * mag
        else:
            v = v >> truncate
    if relu:
        v = np.maximum(v, 0)
    return np.clip(v, -128, 127).astype(np.int8)


def ref_maxpool(x: np.ndarray, *, pool: int, stride: int, pad: int) -> np.ndarray:
    """int8 [C,H,W] max pool (pad cells count as -128)."""
    c, h, w = x.shape
    out_h = (h - pool + 2 * pad) // stride + 1
    out_w = (w - pool + 2 * pad) // stride + 1
    xp = np.full((c, h + 2 * pad, w + 2 * pad), -128, dtype=np.int8)
    xp[:, pad:pad + h, pad:pad + w] = x
    out = np.empty((c, out_h, out_w), dtype=np.int8)
    for oy in range(out_h):
        for ox in range(out_w):
            out[:, oy, ox] = xp[:, oy * stride:oy * stride + pool,
                                ox * stride:ox * stride + pool].max(axis=(1, 2))
    return out


# ---- NVJ1 job serialization --------------------------------------------------------

@dataclass
class ConvLayer:
    in_w: int
    in_h: int
    in_c: int
    out_c: int
    kw: int
    kh: int
    stride: int
    pad: int
    bias_lshift: int
    relu: bool
    out_scale: int
    out_truncate: int
    src_offset: int
    wt_offset: int
    bias_offset: int
    dst_offset: int
    has_pdp: bool = False
    pool_w: int = 0
    pool_h: int = 0
    pool_stride: int = 0
    pool_pad: int = 0
    pool_out_w: int = 0
    pool_out_h: int = 0

    @property
    def conv_out_w(self) -> int:
        return (self.in_w - self.kw + 2 * self.pad) // self.stride + 1

    @property
    def conv_out_h(self) -> int:
        return (self.in_h - self.kh + 2 * self.pad) // self.stride + 1


_LAYER_FMT = "<10H4Bi4B2H4I12x"  # 64 bytes, mirrors board/nvdla/nna_runner.cpp


def emit_layer(l: ConvLayer) -> bytes:
    rec = struct.pack(
        _LAYER_FMT,
        l.in_w, l.in_h, l.in_c, l.out_c, l.kw, l.kh, l.stride, l.pad,
        l.conv_out_w, l.conv_out_h,
        1 if l.has_pdp else 0, 1 if l.relu else 0, l.bias_lshift, l.out_truncate,
        l.out_scale,
        l.pool_w, l.pool_h, l.pool_stride, l.pool_pad,
        l.pool_out_w, l.pool_out_h,
        l.src_offset, l.wt_offset, l.bias_offset, l.dst_offset,
    )
    assert len(rec) == 64
    return rec


def emit_job(layers: list[ConvLayer], blobs: list[tuple[int, bytes]], *,
             ion_size: int, out_offset: int, out_size: int,
             in_offset: int = 0, in_size: int = 0) -> bytes:
    """in_offset/in_size: where the runner loads its per-inference input file
    (packed feature bytes); 0 size = input ships inside the blobs (parity jobs)."""
    head = b"NVJ1" + struct.pack("<6I", len(layers), ion_size, out_offset, out_size,
                                 in_offset, in_size)
    body = b"".join(emit_layer(l) for l in layers)
    blob = struct.pack("<I", len(blobs))
    for off, data in blobs:
        blob += struct.pack("<II", off, len(data)) + data
    return head + body + blob


def parse_job_header(job: bytes) -> tuple[int, int]:
    assert job[:4] == b"NVJ1"
    n_layers, ion_size, _, _, _, _ = struct.unpack_from("<6I", job, 4)
    return n_layers, ion_size
