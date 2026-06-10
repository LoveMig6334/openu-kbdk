#!/bin/sh
# Builds (a) libncnn.a for the V831 (musl-armhf, NEON, single-thread) into
# board/ncnn/dist/board and (b) native host quantize tools (ncnn2table/ncnn2int8/
# ncnnoptimize) into board/ncnn/dist/host. Pin = commit verified on hardware.
set -e
cd "$(dirname "$0")"
PIN=b16501a   # ncnn master, verified on V831 2026-06-10
export PATH=/opt/homebrew/bin:$PATH

[ -d ncnn-src ] || git clone https://github.com/Tencent/ncnn.git ncnn-src
git -C ncnn-src fetch --depth 1 origin $PIN 2>/dev/null || true
git -C ncnn-src checkout $PIN

cmake -S ncnn-src -B build-board -GNinja \
  -DCMAKE_TOOLCHAIN_FILE="$PWD/v831-musl.toolchain.cmake" \
  -DCMAKE_BUILD_TYPE=Release \
  -DNCNN_VULKAN=OFF -DNCNN_OPENMP=OFF -DNCNN_THREADS=OFF \
  -DNCNN_BUILD_TOOLS=OFF -DNCNN_BUILD_EXAMPLES=OFF -DNCNN_BUILD_BENCHMARK=OFF \
  -DNCNN_BUILD_TESTS=OFF -DNCNN_SHARED_LIB=OFF \
  -DCMAKE_INSTALL_PREFIX="$PWD/dist/board"
ninja -C build-board install

cmake -S ncnn-src -B build-host -GNinja -DCMAKE_BUILD_TYPE=Release \
  -DNCNN_VULKAN=OFF -DNCNN_BUILD_TOOLS=ON -DNCNN_SIMPLEOCV=ON \
  -DNCNN_BUILD_EXAMPLES=OFF -DNCNN_BUILD_BENCHMARK=OFF -DNCNN_BUILD_TESTS=OFF
ninja -C build-host ncnn2table ncnn2int8 ncnnoptimize
mkdir -p dist/host
cp build-host/tools/quantize/ncnn2table build-host/tools/quantize/ncnn2int8 \
   build-host/tools/ncnnoptimize dist/host/
echo "OK: dist/board (libncnn.a) + dist/host (quantize tools)"
