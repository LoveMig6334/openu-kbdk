#!/usr/bin/env python3
"""ppm2raw.py SRC.ppm DST.rgb [W H]
Convert a binary P6 PPM to raw interleaved RGB888, nearest-neighbor resized.
Default size 128x128 (the fe_res18_117 NPU input). Pure stdlib, no deps."""
import sys

def main():
    if len(sys.argv) < 3:
        sys.exit("usage: ppm2raw.py SRC.ppm DST.rgb [W H]")
    src, dst = sys.argv[1], sys.argv[2]
    ow = int(sys.argv[3]) if len(sys.argv) > 3 else 128
    oh = int(sys.argv[4]) if len(sys.argv) > 4 else 128
    data = open(src, "rb").read()
    if data[:2] != b"P6":
        sys.exit("not a binary P6 PPM")
    idx, toks = 2, []
    while len(toks) < 3:                       # parse width, height, maxval
        while idx < len(data) and data[idx] in b" \t\r\n":
            idx += 1
        if data[idx:idx+1] == b"#":            # comment line
            while idx < len(data) and data[idx] not in b"\r\n":
                idx += 1
            continue
        start = idx
        while idx < len(data) and data[idx] not in b" \t\r\n":
            idx += 1
        toks.append(int(data[start:idx]))
    w, h, _maxv = toks
    idx += 1                                    # one whitespace byte after maxval
    pix = data[idx:idx + w*h*3]
    out = bytearray(ow*oh*3)
    for y in range(oh):
        sy = y * h // oh
        for x in range(ow):
            sx = x * w // ow
            si = (sy*w + sx) * 3
            di = (y*ow + x) * 3
            out[di:di+3] = pix[si:si+3]
    open(dst, "wb").write(out)
    print(f"wrote {dst}: {ow}x{oh}x3 = {len(out)} bytes (from {w}x{h})")

main()
