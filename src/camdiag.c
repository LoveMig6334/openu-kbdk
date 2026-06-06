/* camdiag.c - decide the sunxi-vin buffer ABI before writing the real capture.
 *
 * Tests, for BOTH single-planar and multi-planar buffer types: S_FMT(NV21),
 * REQBUFS(MMAP), VIDIOC_QUERYBUF, mmap, VIDIOC_QBUF -- printing the exact errno
 * at each step. STOPS BEFORE STREAMON, so no DMA starts and the VIN can't wedge.
 * Reopens the node between the two attempts to reset queue state.
 *
 * Usage: camdiag [/dev/videoN]   (default /dev/video0)
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
#include <linux/videodev2.h>

#define W 320
#define H 240

static int xioctl(int fd, unsigned long req, void *arg){
    int r; do { r = ioctl(fd, req, arg); } while (r==-1 && errno==EINTR);
    return r;
}
#define STEP(name, call) do { \
    int _r = (call); \
    printf("  %-9s r=%d errno=%d (%s)\n", name, _r, _r?errno:0, _r?strerror(errno):"ok"); \
} while(0)

static void try_type(const char *dev, int mp){
    const char *label = mp ? "MPLANE" : "single-planar";
    int fd = open(dev, O_RDWR);            /* blocking; we never STREAMON */
    if (fd < 0){ printf("open %s: %s\n", dev, strerror(errno)); return; }
    enum v4l2_buf_type btype = mp ? V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE
                                  : V4L2_BUF_TYPE_VIDEO_CAPTURE;
    printf("\n== %s ==\n", label);

    struct v4l2_format fmt; memset(&fmt,0,sizeof fmt); fmt.type=btype;
    if (mp){ fmt.fmt.pix_mp.width=W; fmt.fmt.pix_mp.height=H;
        fmt.fmt.pix_mp.pixelformat=V4L2_PIX_FMT_NV21;
        fmt.fmt.pix_mp.field=V4L2_FIELD_NONE; fmt.fmt.pix_mp.num_planes=1;
    } else { fmt.fmt.pix.width=W; fmt.fmt.pix.height=H;
        fmt.fmt.pix.pixelformat=V4L2_PIX_FMT_NV21; fmt.fmt.pix.field=V4L2_FIELD_NONE; }
    STEP("S_FMT", xioctl(fd, VIDIOC_S_FMT, &fmt));
    unsigned sizeimage = mp ? fmt.fmt.pix_mp.plane_fmt[0].sizeimage : fmt.fmt.pix.sizeimage;
    unsigned bpl = mp ? fmt.fmt.pix_mp.plane_fmt[0].bytesperline : fmt.fmt.pix.bytesperline;
    printf("            -> bytesperline=%u sizeimage=%u\n", bpl, sizeimage);

    struct v4l2_requestbuffers rb; memset(&rb,0,sizeof rb);
    rb.count=4; rb.type=btype; rb.memory=V4L2_MEMORY_MMAP;
    STEP("REQBUFS", xioctl(fd, VIDIOC_REQBUFS, &rb));
    printf("            -> count=%u\n", rb.count);

    struct v4l2_buffer q; struct v4l2_plane qp[1];
    memset(&q,0,sizeof q); memset(qp,0,sizeof qp);
    q.type=btype; q.memory=V4L2_MEMORY_MMAP; q.index=0;
    if (mp){ q.m.planes=qp; q.length=1; }
    STEP("QUERYBUF", xioctl(fd, VIDIOC_QUERYBUF, &q));
    unsigned long off = mp ? qp[0].m.mem_offset : q.m.offset;
    unsigned len = mp ? qp[0].length : q.length;
    printf("            -> offset=%lu length=%u\n", off, len);

    if (len){
        void *p = mmap(NULL, len, PROT_READ|PROT_WRITE, MAP_SHARED, fd, off);
        printf("  %-9s %s\n", "mmap@off", p==MAP_FAILED?strerror(errno):"ok");
        if (p!=MAP_FAILED) munmap(p,len);
    }

    memset(&q,0,sizeof q); memset(qp,0,sizeof qp);
    q.type=btype; q.memory=V4L2_MEMORY_MMAP; q.index=0;
    if (mp){ q.m.planes=qp; q.length=1; qp[0].length=len?len:sizeimage; }
    STEP("QBUF", xioctl(fd, VIDIOC_QBUF, &q));

    memset(&rb,0,sizeof rb); rb.count=0; rb.type=btype; rb.memory=V4L2_MEMORY_MMAP;
    xioctl(fd, VIDIOC_REQBUFS, &rb);      /* free buffers */
    close(fd);
}

int main(int argc, char **argv){
    setvbuf(stdout, NULL, _IONBF, 0);
    const char *dev = argc>1 ? argv[1] : "/dev/video0";

    int fd = open(dev, O_RDWR);
    if (fd<0){ printf("open %s: %s\n", dev, strerror(errno)); return 1; }
    struct v4l2_capability cap; memset(&cap,0,sizeof cap);
    if (xioctl(fd,VIDIOC_QUERYCAP,&cap)==0)
        printf("%s driver=%s capabilities=0x%08x device_caps=0x%08x\n",
               dev, cap.driver, cap.capabilities, cap.device_caps);
    close(fd);

    try_type(dev, 0);     /* single-planar */
    try_type(dev, 1);     /* multi-planar  */
    return 0;
}
