#!/usr/bin/env python3
# NV21 (V831 cammpp dump) -> PPM + color stats, pure stdlib.
# Usage: nv21.py IN.nv21[.gz] OUT.ppm W H [uoff voff sat]
#   uoff/voff: added to (U-128)/(V-128) shift (white-balance trim)
#   sat:       chroma saturation multiplier (1.0 = unchanged)
import sys, gzip

def load(p):
    b = open(p,'rb').read()
    if p.endswith('.gz'): b = gzip.decompress(b)
    return b

def main():
    inp,out,W,H = sys.argv[1],sys.argv[2],int(sys.argv[3]),int(sys.argv[4])
    uoff = float(sys.argv[5]) if len(sys.argv)>5 else 0.0
    voff = float(sys.argv[6]) if len(sys.argv)>6 else 0.0
    sat  = float(sys.argv[7]) if len(sys.argv)>7 else 1.0
    raw = load(inp)
    assert len(raw) >= W*H*3//2, (len(raw), W*H*3//2)
    Y = raw[:W*H]
    VU = raw[W*H:W*H*3//2]          # V,U,V,U... at (W/2 x H/2)
    cw = W//2
    # stats
    sy=su=sv=0; ymin=255;ymax=0
    for v in Y:
        sy+=v
        if v<ymin:ymin=v
        if v>ymax:ymax=v
    nuv=len(VU)//2
    for i in range(nuv):
        sv+=VU[2*i]; su+=VU[2*i+1]
    print("Y  mean=%.1f min=%d max=%d"%(sy/len(Y),ymin,ymax))
    print("U  mean=%.1f   V mean=%.1f   (neutral 128)"%(su/nuv, sv/nuv))
    # convert BT.601 full-range with WB trim + saturation
    out_b=bytearray()
    for y in range(H):
        for x in range(W):
            Yv=Y[y*W+x]
            ci=(y//2)*cw+(x//2)
            Vv=VU[2*ci]; Uv=VU[2*ci+1]
            u=((Uv-128.0)+uoff)*sat   # neutralize illuminant (uoff/voff), then saturate
            v=((Vv-128.0)+voff)*sat
            r=Yv+1.402*v
            g=Yv-0.344*u-0.714*v
            b=Yv+1.772*u
            out_b.append(0 if r<0 else 255 if r>255 else int(r))
            out_b.append(0 if g<0 else 255 if g>255 else int(g))
            out_b.append(0 if b<0 else 255 if b>255 else int(b))
    with open(out,'wb') as f:
        f.write(b"P6\n%d %d\n255\n"%(W,H)); f.write(out_b)
    print("wrote",out)

main()
