/* v4l2probe.c - inspect a V4L2 capture node on the KidBright µAI (sunxi VIN).
 *
 * Read-only recon: QUERYCAP, ENUM_INPUT, ENUM_FMT (single- and multi-planar),
 * frame sizes per format, and current G_FMT. Tells us whether /dev/videoN is a
 * plain or mplane capture node and which pixel formats the OV2685 path offers,
 * before we attempt an actual REQBUFS/STREAMON capture.
 *
 * Usage: v4l2probe [/dev/videoN]   (default /dev/video0)
 */
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <sys/ioctl.h>
#include <linux/videodev2.h>

static void fourcc(uint32_t f, char out[5]){
    out[0]=f&0xff; out[1]=(f>>8)&0xff; out[2]=(f>>16)&0xff; out[3]=(f>>24)&0xff; out[4]=0;
}

static void enum_fmts(int fd, enum v4l2_buf_type type, const char *label){
    struct v4l2_fmtdesc fmt; memset(&fmt,0,sizeof fmt); fmt.type=type;
    int any=0;
    for (fmt.index=0; ioctl(fd, VIDIOC_ENUM_FMT, &fmt)==0; fmt.index++){
        char cc[5]; fourcc(fmt.pixelformat, cc);
        printf("  [%s] %-4s  %s\n", label, cc, fmt.description);
        any=1;
        struct v4l2_frmsizeenum fs; memset(&fs,0,sizeof fs);
        fs.pixel_format=fmt.pixelformat;
        for (fs.index=0; ioctl(fd, VIDIOC_ENUM_FRAMESIZES, &fs)==0 && fs.index<8; fs.index++){
            if (fs.type==V4L2_FRMSIZE_TYPE_DISCRETE)
                printf("        %ux%u\n", fs.discrete.width, fs.discrete.height);
            else { printf("        stepwise %ux%u..%ux%u\n",
                    fs.stepwise.min_width, fs.stepwise.min_height,
                    fs.stepwise.max_width, fs.stepwise.max_height); break; }
        }
    }
    if (!any) printf("  [%s] (none / not supported: %s)\n", label, strerror(errno));
}

int main(int argc, char **argv){
    const char *dev = argc>1 ? argv[1] : "/dev/video0";
    int fd = open(dev, O_RDWR);
    if (fd<0){ fprintf(stderr,"open %s: %s\n", dev, strerror(errno)); return 1; }

    struct v4l2_capability cap; memset(&cap,0,sizeof cap);
    if (ioctl(fd, VIDIOC_QUERYCAP, &cap)<0){ perror("QUERYCAP"); return 1; }
    printf("%s\n  driver=%s card=%s bus=%s\n", dev, cap.driver, cap.card, cap.bus_info);
    printf("  caps=0x%08x device_caps=0x%08x\n", cap.capabilities, cap.device_caps);
    uint32_t c = cap.device_caps ? cap.device_caps : cap.capabilities;
    printf("    %s%s%s%s%s\n",
        c&V4L2_CAP_VIDEO_CAPTURE? "CAPTURE ":"",
        c&V4L2_CAP_VIDEO_CAPTURE_MPLANE? "CAPTURE_MPLANE ":"",
        c&V4L2_CAP_STREAMING? "STREAMING ":"",
        c&V4L2_CAP_READWRITE? "READWRITE ":"",
        c&V4L2_CAP_VIDEO_M2M? "M2M ":"");

    struct v4l2_input in; memset(&in,0,sizeof in);
    for (in.index=0; ioctl(fd, VIDIOC_ENUMINPUT, &in)==0; in.index++)
        printf("  input[%u]: %s\n", in.index, in.name);

    printf("formats:\n");
    enum_fmts(fd, V4L2_BUF_TYPE_VIDEO_CAPTURE,        "cap");
    enum_fmts(fd, V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE, "mp");

    struct v4l2_format g; memset(&g,0,sizeof g);
    g.type = (c&V4L2_CAP_VIDEO_CAPTURE_MPLANE)? V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE
                                              : V4L2_BUF_TYPE_VIDEO_CAPTURE;
    if (ioctl(fd, VIDIOC_G_FMT, &g)==0){
        if (g.type==V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE){
            char cc[5]; fourcc(g.fmt.pix_mp.pixelformat,cc);
            printf("current(mp): %ux%u %s planes=%u\n",
                g.fmt.pix_mp.width, g.fmt.pix_mp.height, cc, g.fmt.pix_mp.num_planes);
        } else {
            char cc[5]; fourcc(g.fmt.pix.pixelformat,cc);
            printf("current: %ux%u %s bytesperline=%u sizeimage=%u\n",
                g.fmt.pix.width, g.fmt.pix.height, cc,
                g.fmt.pix.bytesperline, g.fmt.pix.sizeimage);
        }
    } else perror("G_FMT");

    close(fd);
    return 0;
}
