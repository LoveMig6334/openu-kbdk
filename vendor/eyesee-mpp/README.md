# Vendored Allwinner eyesee-mpp headers (V831 / sun8iw19p1)

These headers are **not ours** — they are Allwinner Technology's eyesee-mpp
middleware interface headers, copied verbatim so the camera tools (`cammpp`,
`campreview`) compile against the exact struct ABI of the board's libraries.

- **Source:** `github.com/amartol/tina-v83x-softwinner`, path
  `eyesee-mpp/middleware/sun8iw19p1/include/` (plus `cdx_list_type.h` from
  `eyesee-mpp/system/public/include/utils/` and `sunxi_camera_v2.h` from the
  libisp `V4l2Camera/` dir). `sun8iw19p1` is the V831's chip id.
- **Why vendored:** only the headers are needed at build time; the actual
  `AW_MPI_*` symbols are resolved at runtime via `dlopen` against the board's
  `/usr/lib/eyesee-mpp/*.so`. Nothing here is linked or redistributed as a binary.
- **Build flag:** these headers need `-DAWCHIP=0x1817` (cosmetic; see `plat_defines.h`).

© Allwinner Technology — retained under their original copyright. If publishing
this repo, confirm redistribution of these headers is acceptable or replace them
with a fetch-at-build step.
