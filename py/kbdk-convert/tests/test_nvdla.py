"""NVDLA (V831 NPU, nv_small) layout packers + int-exact layer reference.

The element-position oracles below transcribe the indexing math demonstrated by
third_party/v831-npu/tests/test_nna_dc_143x79x8_3x3x8x16_formats.cpp
(set_input / get_output / set_weight), i.e. the memory layouts the hardware
actually reads. The vectorized packers in kbdk_convert.nvdla must agree with
them element-for-element.
"""

import numpy as np
import pytest

from kbdk_convert import nvdla


# ---- oracles (1-based coords, as in the C test) --------------------------------

def oracle_feature_pos(w, h, c, in_w, in_h):
    """Byte position of element (w,h,c) in a packed feature cube (C cubes of 8)."""
    kpg = (c - 1) // 8
    c1 = c - kpg * 8
    start = kpg * in_w * in_h * 8
    return start + in_w * (8 * (h - 1)) + (w - 1) * 8 + c1 - 1


def oracle_weight_pos(w, h, c, k, k_w, k_h, k_c):
    """Byte position of weight (w,h,c) of kernel k (direct conv int8, k_c <= 32)."""
    kgs = (k - 1) // 8
    kernels_in_group = 8  # full groups; oracle only valid for full groups
    pos = (h - 1) * k_w + (w - 1)
    row = kgs * (kernels_in_group * k_w * k_h * k_c) + kernels_in_group * k_c * pos
    col = row + ((k - 1) % 8) * k_c
    return col + (c - 1)


# ---- feature packing ------------------------------------------------------------

def test_feature_pack_matches_oracle():
    C, H, W = 11, 5, 7  # crosses one surface boundary (11 -> 2 surfaces)
    rng = np.random.default_rng(0)
    x = rng.integers(-128, 128, size=(C, H, W), dtype=np.int8)
    packed = nvdla.pack_feature(x)
    assert len(packed) == 2 * 8 * H * W
    buf = np.frombuffer(packed, dtype=np.int8)
    for c in range(1, C + 1):
        for h in range(1, H + 1):
            for w in range(1, W + 1):
                assert buf[oracle_feature_pos(w, h, c, W, H)] == x[c - 1, h - 1, w - 1]


def test_feature_pack_pads_with_zero():
    x = np.ones((3, 2, 2), dtype=np.int8)
    buf = np.frombuffer(nvdla.pack_feature(x), dtype=np.int8)
    # channels 3..7 of every pixel must be zero
    for h in range(1, 3):
        for w in range(1, 3):
            for c in range(4, 9):
                assert buf[oracle_feature_pos(w, h, c, 2, 2)] == 0


def test_feature_roundtrip():
    rng = np.random.default_rng(1)
    x = rng.integers(-128, 128, size=(19, 6, 9), dtype=np.int8)
    packed = nvdla.pack_feature(x)
    back = nvdla.unpack_feature(packed, 19, 6, 9)
    np.testing.assert_array_equal(back, x)


# ---- weight packing --------------------------------------------------------------

def test_weight_pack_matches_oracle_full_groups():
    K, C, KH, KW = 16, 8, 3, 3
    rng = np.random.default_rng(2)
    wt = rng.integers(-128, 128, size=(K, C, KH, KW), dtype=np.int8)
    buf = np.frombuffer(nvdla.pack_weights(wt), dtype=np.int8)
    assert buf.size == K * C * KH * KW
    for k in range(1, K + 1):
        for c in range(1, C + 1):
            for h in range(1, KH + 1):
                for w in range(1, KW + 1):
                    assert buf[oracle_weight_pos(w, h, c, k, KW, KH, C)] == \
                        wt[k - 1, c - 1, h - 1, w - 1]


def test_weight_pack_partial_last_group_is_packed_tight():
    # 10 kernels -> group of 8 + group of 2; total bytes stays K*C*KH*KW
    K, C, KH, KW = 10, 4, 2, 2
    rng = np.random.default_rng(3)
    wt = rng.integers(-128, 128, size=(K, C, KH, KW), dtype=np.int8)
    buf = np.frombuffer(nvdla.pack_weights(wt), dtype=np.int8)
    assert buf.size == K * C * KH * KW
    # second group (kernels 9,10) starts right after the first
    g2 = 8 * KH * KW * C
    # kernel 9, spatial (1,1), channel 1 is the first byte of group 2
    assert buf[g2] == wt[8, 0, 0, 0]
    # kernel 10 channel cube follows kernel 9's at the same spatial position
    assert buf[g2 + C] == wt[9, 0, 0, 0]


def test_weight_pack_rejects_wide_channels_for_now():
    wt = np.zeros((8, 40, 3, 3), dtype=np.int8)  # C > 32 ordering unverified
    with pytest.raises(ValueError):
        nvdla.pack_weights(wt)


# ---- int-exact conv + SDP reference ----------------------------------------------

def naive_ref(x, wt, bias, stride, pad, bias_lshift, scale, truncate, relu, rounding):
    """Straight-line readable model of conv -> bias -> cvt -> relu -> sat."""
    C, H, W = x.shape
    K, _, KH, KW = wt.shape
    out_h = (H - KH + 2 * pad) // stride + 1
    out_w = (W - KW + 2 * pad) // stride + 1
    xp = np.zeros((C, H + 2 * pad, W + 2 * pad), dtype=np.int32)
    xp[:, pad:pad + H, pad:pad + W] = x
    out = np.zeros((K, out_h, out_w), dtype=np.int8)
    for k in range(K):
        for oy in range(out_h):
            for ox in range(out_w):
                acc = np.int64(0)
                for c in range(C):
                    for ky in range(KH):
                        for kx in range(KW):
                            acc += int(xp[c, oy * stride + ky, ox * stride + kx]) * int(wt[k, c, ky, kx])
                acc += int(bias[k]) << bias_lshift
                v = int(acc) * scale
                if truncate:
                    if rounding:  # round half away from zero (hardware semantics)
                        v = -((-v + (1 << (truncate - 1))) >> truncate) if v < 0 \
                            else (v + (1 << (truncate - 1))) >> truncate
                    else:
                        v >>= truncate
                if relu:
                    v = max(v, 0)
                out[k, oy, ox] = np.clip(v, -128, 127)
    return out


@pytest.mark.parametrize("rounding", [False, True])
def test_ref_conv_matches_naive(rounding):
    rng = np.random.default_rng(4)
    x = rng.integers(-128, 128, size=(3, 8, 8), dtype=np.int8)
    wt = rng.integers(-128, 128, size=(9, 3, 3, 3), dtype=np.int8)
    bias = rng.integers(-3000, 3000, size=9, dtype=np.int16)
    got = nvdla.ref_conv_sdp(x, wt, bias, stride=1, pad=1, bias_lshift=2,
                             scale=23, truncate=12, relu=True, rounding=rounding)
    want = naive_ref(x, wt, bias, 1, 1, 2, 23, 12, True, rounding)
    np.testing.assert_array_equal(got, want)


def test_ref_pool():
    rng = np.random.default_rng(5)
    x = rng.integers(-128, 128, size=(4, 6, 6), dtype=np.int8)
    got = nvdla.ref_maxpool(x, pool=2, stride=2, pad=0)
    assert got.shape == (4, 3, 3)
    assert got[0, 0, 0] == x[0, 0:2, 0:2].max()


# ---- job serialization ------------------------------------------------------------

def test_job_roundtrip_header():
    layer = nvdla.ConvLayer(
        in_w=8, in_h=8, in_c=3, out_c=9, kw=3, kh=3, stride=1, pad=1,
        bias_lshift=2, relu=True, out_scale=23, out_truncate=12,
        src_offset=0x1000, wt_offset=0x2000, bias_offset=0x3000, dst_offset=0x4000,
    )
    blobs = [(0x1000, b"\x01\x02"), (0x2000, b"\x03")]
    job = nvdla.emit_job([layer], blobs, ion_size=0x8000,
                         out_offset=0x4000, out_size=8 * 8 * 16)
    assert job[:4] == b"NVJ1"
    n_layers, ion_size = nvdla.parse_job_header(job)
    assert n_layers == 1
    assert ion_size == 0x8000
