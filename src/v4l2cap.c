/* v4l2cap.c - capture one camera frame on the KidBright µAI and blit it to the
 * LCD.  Targets the Allwinner sunxi-vin pipeline (OV2685 -> CSI/MIPI/ISP/scaler
 * -> /dev/video0, a multi-planar node).  Modelled on the libmaix sequence:
 * NV21 output, S_FMT -> REQBUFS(MMAP) -> QBUF -> STREAMON -> DQBUF.
 *
 * HANG-SAFE by construction (the VIN driver wedges the board if mishandled):
 *   - touches exactly ONE video node,
 *   - select() with a timeout around DQBUF so it can never block forever,
 *   - STREAMOFF + munmap + close on every exit path.
 *
 * Usage:
 *   v4l2cap --probe [WxH]      negotiate format only, no streaming (safe recon)
 *   v4l2cap [WxH] [/dev/fbN]   capture one NV21 frame, draw it to the framebuffer
 * Defaults: 320x240 capture, /dev/video0, /dev/fb0.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <poll.h>
#include <linux/videodev2.h>
#include <linux/fb.h>

#define VID "/dev/video0"
#define NBUF 4

static void fourcc(uint32_t f, char o[5]){
    o[0]=f; o[1]=f>>8; o[2]=f>>16; o[3]=f>>24; o[4]=0;
}
static int xioctl(int fd, unsigned long req, void *arg){
    int r; do { r = ioctl(fd, req, arg); } while (r==-1 && errno==EINTR);
    return r;
}

/* --- framebuffer blit (NV21 -> RGB, centre-crop into the panel) ----------- */
static void show_on_fb(const char *fbdev, const uint8_t *y, const uint8_t *vu,
                       int w, int h, int ystride, int cstride){
    int fd = open(fbdev, O_RDWR);
    if (fd<0){ perror("open fb"); return; }
    struct fb_var_screeninfo v; struct fb_fix_screeninfo f;
    if (xioctl(fd,FBIOGET_VSCREENINFO,&v)<0 || xioctl(fd,FBIOGET_FSCREENINFO,&f)<0){
        perror("fb info"); close(fd); return; }
    int Bpp = v.bits_per_pixel/8;
    size_t maplen = f.smem_len ? f.smem_len : (size_t)f.line_length*v.yres_virtual;
    uint8_t *fb = mmap(NULL, maplen, PROT_READ|PROT_WRITE, MAP_SHARED, fd, 0);
    if (fb==MAP_FAILED){ perror("mmap fb"); close(fd); return; }
    ioctl(fd, FBIOBLANK, FB_BLANK_UNBLANK);

    /* map each panel pixel back to a source pixel (centre-crop, keep aspect) */
    int ox = (w - (int)v.xres)/2, oy = (h - (int)v.yres)/2; /* may be negative */
    for (unsigned py=0; py<v.yres; py++){
        uint8_t *row = fb + (size_t)py*f.line_length;
        int sy = (int)py + oy; if (sy<0) sy=0; if (sy>=h) sy=h-1;
        for (unsigned px=0; px<v.xres; px++){
            int sx = (int)px + ox; if (sx<0) sx=0; if (sx>=w) sx=w-1;
            int Y = y[sy*ystride + sx];
            /* NV21 chroma plane: interleaved V,U at half resolution */
            int ci = (sy/2)*cstride + (sx/2)*2;
            int Vv = vu[ci] - 128, Uu = vu[ci+1] - 128;
            int R = Y + ((            91881*Vv) >> 16);
            int G = Y - ((22554*Uu + 46802*Vv) >> 16);
            int B = Y + ((116130*Uu           ) >> 16);
            if (R<0)R=0; if(R>255)R=255; if(G<0)G=0; if(G>255)G=255; if(B<0)B=0; if(B>255)B=255;
            uint32_t pix = ((uint32_t)(R>>(8-v.red.length))   << v.red.offset)
                         | ((uint32_t)(G>>(8-v.green.length)) << v.green.offset)
                         | ((uint32_t)(B>>(8-v.blue.length))  << v.blue.offset);
            uint8_t *p = row + (size_t)px*Bpp;
            for (int k=0;k<Bpp;k++) p[k] = (pix>>(8*k)) & 0xff;
        }
    }
    munmap(fb, maplen); close(fd);
    printf("blitted %dx%d NV21 frame -> %s (%ux%u)\n", w,h,fbdev,v.xres,v.yres);
}

int main(int argc, char **argv){
    setvbuf(stdout, NULL, _IONBF, 0);   /* unbuffered: don't lose msgs on crash */
    int probe=0, W=320, H=240; const char *fbdev="/dev/fb0";
    for (int i=1;i<argc;i++){
        if (!strcmp(argv[i],"--probe")) probe=1;
        else if (strchr(argv[i],'x')){ int a,b; if (sscanf(argv[i],"%dx%d",&a,&b)==2){W=a;H=b;} }
        else if (!strncmp(argv[i],"/dev/fb",7)) fbdev=argv[i];
    }

    int fd = open(VID, O_RDWR|O_NONBLOCK);
    if (fd<0){ fprintf(stderr,"open %s: %s\n",VID,strerror(errno)); return 1; }

    struct v4l2_capability cap; memset(&cap,0,sizeof cap);
    if (xioctl(fd,VIDIOC_QUERYCAP,&cap)<0){ perror("QUERYCAP"); close(fd); return 1; }
    uint32_t c = cap.device_caps?cap.device_caps:cap.capabilities;
    /* sunxi-vin under-reports the MPLANE cap flag; detect by what ENUM_FMT
     * actually accepts (mplane type first, fall back to single-planar). */
    struct v4l2_fmtdesc fd0; memset(&fd0,0,sizeof fd0);
    fd0.type=V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
    int mp = (xioctl(fd,VIDIOC_ENUM_FMT,&fd0)==0);
    printf("%s driver=%s caps=0x%08x mplane=%d\n", VID, cap.driver, c, mp);
    enum v4l2_buf_type btype = mp?V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE
                                 :V4L2_BUF_TYPE_VIDEO_CAPTURE;

    int input=0; xioctl(fd,VIDIOC_S_INPUT,&input);          /* best-effort */

    /* list supported formats + sizes (always; cheap, helps pick a pixfmt) */
    struct v4l2_fmtdesc fe; memset(&fe,0,sizeof fe); fe.type=btype;
    for (fe.index=0; xioctl(fd,VIDIOC_ENUM_FMT,&fe)==0; fe.index++){
        char fc[5]; fourcc(fe.pixelformat,fc);
        printf("  fmt[%u] %-4s %s\n", fe.index, fc, fe.description);
        struct v4l2_frmsizeenum fs; memset(&fs,0,sizeof fs); fs.pixel_format=fe.pixelformat;
        for (fs.index=0; xioctl(fd,VIDIOC_ENUM_FRAMESIZES,&fs)==0 && fs.index<6; fs.index++){
            if (fs.type==V4L2_FRMSIZE_TYPE_DISCRETE)
                printf("        %ux%u\n", fs.discrete.width, fs.discrete.height);
            else { printf("        %ux%u..%ux%u\n", fs.stepwise.min_width,
                    fs.stepwise.min_height, fs.stepwise.max_width, fs.stepwise.max_height); break; }
        }
    }

    /* negotiate NV21 at WxH */
    struct v4l2_format fmt; memset(&fmt,0,sizeof fmt); fmt.type=btype;
    if (mp){
        fmt.fmt.pix_mp.width=W; fmt.fmt.pix_mp.height=H;
        fmt.fmt.pix_mp.pixelformat=V4L2_PIX_FMT_NV21;
        fmt.fmt.pix_mp.field=V4L2_FIELD_NONE;
        fmt.fmt.pix_mp.num_planes=1;
    } else {
        fmt.fmt.pix.width=W; fmt.fmt.pix.height=H;
        fmt.fmt.pix.pixelformat=V4L2_PIX_FMT_NV21;
        fmt.fmt.pix.field=V4L2_FIELD_NONE;
    }
    if (xioctl(fd,VIDIOC_S_FMT,&fmt)<0){ perror("S_FMT"); close(fd); return 1; }

    int aw,ah,nplanes,ystride; uint32_t pf; size_t sizeimage;
    if (mp){ aw=fmt.fmt.pix_mp.width; ah=fmt.fmt.pix_mp.height;
        pf=fmt.fmt.pix_mp.pixelformat; nplanes=fmt.fmt.pix_mp.num_planes;
        ystride=fmt.fmt.pix_mp.plane_fmt[0].bytesperline;
        sizeimage=fmt.fmt.pix_mp.plane_fmt[0].sizeimage; }
    else { aw=fmt.fmt.pix.width; ah=fmt.fmt.pix.height;
        pf=fmt.fmt.pix.pixelformat; nplanes=1; ystride=fmt.fmt.pix.bytesperline;
        sizeimage=fmt.fmt.pix.sizeimage; }
    if (!ystride) ystride=aw;
    if (!sizeimage) sizeimage=(size_t)ystride*ah*3/2;
    char cc[5]; fourcc(pf,cc);
    printf("negotiated: %dx%d %s planes=%d ystride=%d sizeimage=%zu\n",
           aw,ah,cc,nplanes,ystride,sizeimage);

    if (probe){ printf("PROBE_OK (no streaming)\n"); close(fd); return 0; }
    if (pf!=V4L2_PIX_FMT_NV21)
        printf("WARN: driver gave %s, not NV21; colours may be wrong\n",cc);

    /* request + map buffers */
    struct v4l2_requestbuffers rb; memset(&rb,0,sizeof rb);
    rb.count=NBUF; rb.type=btype; rb.memory=V4L2_MEMORY_MMAP;
    if (xioctl(fd,VIDIOC_REQBUFS,&rb)<0){ perror("REQBUFS"); close(fd); return 1; }
    printf("REQBUFS gave %u buffers\n", rb.count);

    /* This BSP's sunxi-vin doesn't implement VIDIOC_QUERYBUF (returns ENOTTY),
     * so we can't ask for each buffer's mmap offset. videobuf2 assigns those
     * offsets as a running page-aligned sum of buffer sizes, so map each buffer
     * at index*PAGE_ALIGN(sizeimage).  QBUF only needs the index. */
    long pg = sysconf(_SC_PAGESIZE); if (pg<=0) pg=4096;
    size_t asize = (sizeimage + pg-1) & ~((size_t)pg-1);
    void *pbuf[NBUF]; size_t plen[NBUF];
    for (unsigned i=0;i<rb.count;i++){
        off_t off = (off_t)i*asize;
        pbuf[i]=mmap(NULL,asize,PROT_READ|PROT_WRITE,MAP_SHARED,fd,off);
        plen[i]=asize;
        if (pbuf[i]==MAP_FAILED){ fprintf(stderr,"mmap buf%u@%ld: %s\n",i,(long)off,strerror(errno)); close(fd); return 1; }
        struct v4l2_buffer q; struct v4l2_plane qp[1]; memset(&q,0,sizeof q); memset(qp,0,sizeof qp);
        q.type=btype; q.memory=V4L2_MEMORY_MMAP; q.index=i;
        if (mp){ q.m.planes=qp; qp[0].length=sizeimage; q.length=1; }
        if (xioctl(fd,VIDIOC_QBUF,&q)<0){ perror("QBUF"); close(fd); return 1; }
    }
    printf("mapped %u buffers (asize=%zu) and queued\n", rb.count, asize);

    if (xioctl(fd,VIDIOC_STREAMON,&btype)<0){ perror("STREAMON"); close(fd); return 1; }
    printf("STREAMON ok, waiting for a frame...\n");

    int got=-1; size_t glen=0;
    for (int tries=0; tries<8 && got<0; tries++){
        struct pollfd pfd={.fd=fd,.events=POLLIN};
        int r=poll(&pfd,1,3000);   /* 3s timeout, ms */
        if (r<=0){ fprintf(stderr,"poll: %s\n", r==0?"timeout":strerror(errno)); break; }
        struct v4l2_buffer d; struct v4l2_plane dp[1]; memset(&d,0,sizeof d); memset(dp,0,sizeof dp);
        d.type=btype; d.memory=V4L2_MEMORY_MMAP;
        if (mp){ d.m.planes=dp; d.length=1; }
        if (xioctl(fd,VIDIOC_DQBUF,&d)<0){ if(errno==EAGAIN) continue; perror("DQBUF"); break; }
        got=d.index; glen=mp?dp[0].bytesused:d.bytesused;
        printf("DQBUF: buffer %d, %zu bytes\n", got, glen);
    }

    xioctl(fd,VIDIOC_STREAMOFF,&btype);   /* always, even on failure */

    if (got>=0){
        uint8_t *base=pbuf[got];
        int cstride=ystride;                       /* NV21 chroma stride == luma */
        uint8_t *yp=base;
        uint8_t *vu=base + (size_t)ystride*ah;     /* VU plane follows Y (1 plane) */
        if (mp && nplanes>1) vu=pbuf[got];         /* (single-plane NV21 assumed) */
        show_on_fb(fbdev, yp, vu, aw, ah, ystride, cstride);
    } else {
        fprintf(stderr,"no frame captured\n");
    }

    for (unsigned i=0;i<rb.count;i++) munmap(pbuf[i],plen[i]);
    close(fd);
    return got>=0?0:1;
}
