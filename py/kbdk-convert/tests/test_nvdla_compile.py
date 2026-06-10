"""npu_slim -> NVDLA pack compiler: BN folding, quant param selection, int sim."""

import numpy as np
import pytest
import torch
import torch.nn as nn

from kbdk_convert import nvdla, nvdla_compile


def test_fold_bn_preserves_forward():
    conv = nn.Conv2d(3, 8, 3, 1, 1, bias=False)
    bn = nn.BatchNorm2d(8)
    bn.weight.data.uniform_(0.5, 1.5)
    bn.bias.data.uniform_(-0.5, 0.5)
    bn.running_mean.uniform_(-1, 1)
    bn.running_var.uniform_(0.5, 2.0)
    bn.eval()
    conv.eval()
    x = torch.randn(2, 3, 8, 8)
    want = bn(conv(x))
    w, b = nvdla_compile.fold_bn(
        conv.weight.detach().numpy(), None,
        bn.weight.detach().numpy(), bn.bias.detach().numpy(),
        bn.running_mean.numpy(), bn.running_var.numpy(), bn.eps)
    conv2 = nn.Conv2d(3, 8, 3, 1, 1, bias=True)
    conv2.weight.data = torch.from_numpy(w.copy())
    conv2.bias.data = torch.from_numpy(b.copy())
    got = conv2(x)
    assert torch.allclose(want, got, atol=1e-5)


def test_requant_params_approximate_multiplier():
    for m in (0.0123, 0.5, 3.7, 1e-4):
        scale, trunc = nvdla_compile.requant_params(m)
        assert 1 <= scale <= 32767
        assert 0 <= trunc <= 31
        assert abs(scale / (1 << trunc) - m) / m < 1e-3


def test_bias_params_fit_int16():
    bq = np.array([100.0, -40000.0, 70000.0])
    b16, lshift = nvdla_compile.bias_params(bq)
    assert b16.dtype == np.int16
    approx = b16.astype(np.int64) << lshift
    assert np.all(np.abs(approx - bq) <= (1 << lshift))


def test_simulate_single_layer_matches_ref():
    rng = np.random.default_rng(0)
    x = rng.integers(-128, 128, size=(3, 8, 8), dtype=np.int8)
    wt = rng.integers(-128, 128, size=(8, 3, 3, 3), dtype=np.int8)
    bias = rng.integers(-100, 100, size=8, dtype=np.int16)
    ql = nvdla_compile.QuantLayer(
        wq=wt, b16=bias, bias_lshift=1, scale=20, truncate=10, relu=True,
        stride=1, pad=1, pool=2, pool_stride=2)
    got = nvdla_compile.simulate([ql], x)
    want = nvdla.ref_conv_sdp(x, wt, bias, stride=1, pad=1, bias_lshift=1,
                              scale=20, truncate=10, relu=True, rounding=True)
    want = nvdla.ref_maxpool(want, pool=2, stride=2, pad=0)
    np.testing.assert_array_equal(got, want)


def test_compile_slim_quant_sim_agrees_with_float():
    """On easily-separable random blobs, the int8 pipeline must match the
    float net's argmax on nearly every sample."""
    torch.manual_seed(0)
    from kbdk_train.train import npu_slim
    model = npu_slim(3).eval()
    rng = np.random.default_rng(0)
    # solid-color images, the toy task shape
    imgs = []
    for i in range(12):
        c = [(200, 40, 40), (40, 200, 40), (40, 40, 200)][i % 3]
        img = np.full((64, 64, 3), c, dtype=np.uint8)
        img = np.clip(img.astype(np.int16) + rng.integers(-20, 20, img.shape), 0, 255)
        imgs.append(img.astype(np.uint8))
    imgs = np.stack(imgs)
    layers = nvdla_compile.extract_slim_layers(model)
    qlayers, meta = nvdla_compile.quantize_slim(layers, imgs)
    agree = 0
    for img in imgs:
        xq = nvdla_compile.preprocess(img)
        qlog = nvdla_compile.simulate(qlayers, xq).reshape(-1)
        x = torch.from_numpy((img.astype(np.float32) / 255.0 - 0.5) / 0.5)
        flog = model(x.permute(2, 0, 1).unsqueeze(0)).detach().numpy().reshape(-1)
        agree += int(np.argmax(qlog[:3]) == np.argmax(flog))
    assert agree >= 11  # untrained net: outputs are tiny but ordering should hold
