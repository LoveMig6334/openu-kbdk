# Vendored libmaix headers (V831 NPU / AWNN)

Source: https://github.com/sipeed/libmaix — `components/libmaix/include/`
(`libmaix_nn.h`, `libmaix_err.h`). Trimmed to the AWNN code path this toolkit uses.

These are **header-only**: we never link `libmaix`. At runtime the tool `dlopen`s the
board's own `/usr/lib/python3.8/site-packages/maix/libmaix_nn.so` and `dlsym`s the
exported symbols (verified to match these declarations against the on-board `.so`).

License: libmaix is published by Sipeed; see the upstream repository for its terms. Same
treatment/rationale as `vendor/eyesee-mpp/` (headers vendored for the build only).
