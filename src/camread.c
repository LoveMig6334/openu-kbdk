/* camread.c - last pure-syscall capture attempt on the sunxi-vin BSP.
 *
 * The node lacks VIDIOC_QBUF/DQBUF (proven via camdiag) but advertises
 * V4L2_CAP_READWRITE.  This tests whether a plain read() yields a frame -- if
 * so we can capture without the vendor MPP stack.  Two attempts: read() alone
 * (vb2 read-emul auto-streams), then STREAMON+read().  HANG-SAFE: O_NONBLOCK +
 * poll() timeout, always STREAMOFF.
 *
 * Usage: camread [/dev/videoN]
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <poll.h>
#include <sys/ioctl.h>
#include <linux/videodev2.h>

#define W 320
#define H 240

static int xioctl(int fd, unsigned long req, void *arg){
    int r; do { r = ioctl(fd, req, arg); } while (r==-1 && errno==EINTR);
    return r;
}

static int try_read(int fd, uint8_t *buf, size_t sz, const char *label){
    struct pollfd pfd = { .fd=fd, .events=POLLIN };
    int pr = poll(&pfd, 1, 3000);
    if (pr <= 0){ printf("  [%s] poll: %s\n", label, pr==0?"timeout":strerror(errno)); return -1; }
    ssize_t n = read(fd, buf, sz);
    if (n < 0){ printf("  [%s] read: errno=%d (%s)\n", label, errno, strerror(errno)); return -1; }
    /* crude liveliness: are the bytes non-constant? */
    int mn=255, mx=0; for (size_t i=0;i<(size_t)n && i<4096;i++){ if(buf[i]<mn)mn=buf[i]; if(buf[i]>mx)mx=buf[i]; }
    printf("  [%s] read %zd bytes  (Y range %d..%d %s)\n", label, n, mn, mx,
           mx>mn ? "<- real image data" : "<- looks blank");
    return n>0 ? 0 : -1;
}

int main(int argc, char **argv){
    setvbuf(stdout, NULL, _IONBF, 0);
    const char *dev = argc>1 ? argv[1] : "/dev/video0";
    int fd = open(dev, O_RDWR|O_NONBLOCK);
    if (fd<0){ printf("open %s: %s\n", dev, strerror(errno)); return 1; }

    struct v4l2_capability cap; memset(&cap,0,sizeof cap);
    xioctl(fd, VIDIOC_QUERYCAP, &cap);
    printf("%s caps=0x%08x READWRITE=%d\n", dev,
           cap.capabilities, !!(cap.capabilities & V4L2_CAP_READWRITE));

    enum v4l2_buf_type bt = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
    struct v4l2_format fmt; memset(&fmt,0,sizeof fmt); fmt.type=bt;
    fmt.fmt.pix_mp.width=W; fmt.fmt.pix_mp.height=H;
    fmt.fmt.pix_mp.pixelformat=V4L2_PIX_FMT_NV21;
    fmt.fmt.pix_mp.field=V4L2_FIELD_NONE; fmt.fmt.pix_mp.num_planes=1;
    if (xioctl(fd, VIDIOC_S_FMT, &fmt)<0){ printf("S_FMT: %s\n", strerror(errno)); close(fd); return 1; }
    size_t sz = fmt.fmt.pix_mp.plane_fmt[0].sizeimage;
    if (!sz) sz = (size_t)W*H*3/2;
    printf("S_FMT ok: NV21 %ux%u sizeimage=%zu\n",
           fmt.fmt.pix_mp.width, fmt.fmt.pix_mp.height, sz);

    uint8_t *buf = malloc(sz);

    /* attempt 1: bare read() */
    if (try_read(fd, buf, sz, "read-only") == 0){ free(buf); close(fd); return 0; }

    /* attempt 2: STREAMON then read() */
    if (xioctl(fd, VIDIOC_STREAMON, &bt) < 0)
        printf("  STREAMON: errno=%d (%s)\n", errno, strerror(errno));
    else {
        int rc = try_read(fd, buf, sz, "streamon+read");
        xioctl(fd, VIDIOC_STREAMOFF, &bt);
        if (rc==0){ free(buf); close(fd); return 0; }
    }

    printf("=> pure read() capture not available on this node\n");
    free(buf); close(fd);
    return 1;
}
