/* kbdk-example
 * name: Audio tone
 * desc: play a 440 Hz sine for 2 s via raw SNDRV_PCM_IOCTL (no alsa-lib)
 * extra_args: -lm
 */
/* Derived from src/audio.c (the `tone` mode only). Drives the codec's playback
 * PCM (/dev/snd/pcmC0D0p) directly through the kernel ALSA ioctl ABI — there is
 * no alsa-lib in the musl sysroot, and we don't want one anyway.
 *
 * The fiddly part is struct snd_pcm_hw_params: it is a flat array of "masks"
 * (access, format) and "intervals" (rate, channels, period, buffer). You set
 * every field to "any", pin the few you care about (S16_LE, stereo, 44100), then
 * SNDRV_PCM_IOCTL_HW_PARAMS collapses each interval to the value the codec
 * actually chose — which you read back via ->min. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <limits.h>
#include <math.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <sys/ioctl.h>
#include <sound/asound.h>

#define RATE 44100
#define FREQ 440.0
#define SECS 2.0

static int xioctl(int fd, unsigned long req, void *arg){
    int r; do { r = ioctl(fd, req, arg); } while (r == -1 && errno == EINTR);
    return r;
}

/* hw_params is masks[] + intervals[]; these helpers index into them. */
static struct snd_mask *pmask(struct snd_pcm_hw_params *p, int n){
    return &p->masks[n - SNDRV_PCM_HW_PARAM_FIRST_MASK];
}
static struct snd_interval *pintv(struct snd_pcm_hw_params *p, int n){
    return &p->intervals[n - SNDRV_PCM_HW_PARAM_FIRST_INTERVAL];
}
/* "any": all masks all-ones, all intervals full-range — the starting point the
 * kernel narrows down to a real configuration. */
static void hw_any(struct snd_pcm_hw_params *p){
    memset(p, 0, sizeof *p);
    for (size_t i = 0; i < sizeof(p->masks)/sizeof(p->masks[0]); i++)
        memset(p->masks[i].bits, 0xff, sizeof p->masks[i].bits);
    for (size_t i = 0; i < sizeof(p->intervals)/sizeof(p->intervals[0]); i++){
        p->intervals[i].min = 0; p->intervals[i].max = UINT_MAX;
    }
    p->rmask = ~0U; p->info = ~0U;
}
static void mask_one(struct snd_pcm_hw_params *p, int n, unsigned val){
    struct snd_mask *m = pmask(p, n);
    memset(m->bits, 0, sizeof m->bits);
    m->bits[val >> 5] |= 1u << (val & 31);
}
static void intv_one(struct snd_pcm_hw_params *p, int n, unsigned val){
    struct snd_interval *i = pintv(p, n);
    i->min = i->max = val; i->integer = 1;
}
static unsigned intv_get(struct snd_pcm_hw_params *p, int n){ return pintv(p, n)->min; }

int main(void){
    setvbuf(stdout, NULL, _IONBF, 0);

    /* Open the playback PCM blocking, so WRITEI paces itself to the DMA. */
    int fd = open("/dev/snd/pcmC0D0p", O_RDWR);
    if (fd < 0){ fprintf(stderr, "open pcm: %s\n", strerror(errno)); return 1; }

    /* Negotiate S16_LE interleaved stereo at 44100 Hz. */
    struct snd_pcm_hw_params hp; hw_any(&hp);
    mask_one(&hp, SNDRV_PCM_HW_PARAM_ACCESS, SNDRV_PCM_ACCESS_RW_INTERLEAVED);
    mask_one(&hp, SNDRV_PCM_HW_PARAM_FORMAT, SNDRV_PCM_FORMAT_S16_LE);
    intv_one(&hp, SNDRV_PCM_HW_PARAM_CHANNELS, 2);
    intv_one(&hp, SNDRV_PCM_HW_PARAM_RATE, RATE);
    hp.rmask = ~0U;
    if (xioctl(fd, SNDRV_PCM_IOCTL_HW_PARAMS, &hp) < 0){ perror("HW_PARAMS"); return 1; }
    unsigned rate   = intv_get(&hp, SNDRV_PCM_HW_PARAM_RATE);
    unsigned ch     = intv_get(&hp, SNDRV_PCM_HW_PARAM_CHANNELS);
    unsigned period = intv_get(&hp, SNDRV_PCM_HW_PARAM_PERIOD_SIZE);
    unsigned buffer = intv_get(&hp, SNDRV_PCM_HW_PARAM_BUFFER_SIZE);

    /* Software params: auto-start once one period is queued. */
    struct snd_pcm_sw_params sw; memset(&sw, 0, sizeof sw);
    sw.tstamp_mode = SNDRV_PCM_TSTAMP_NONE;
    sw.period_step = 1;
    sw.avail_min = period;
    sw.start_threshold = period;
    sw.stop_threshold = buffer;
    unsigned long b = buffer ? buffer : 1;          /* boundary: a big 2^k * buffer */
    while (b * 2 <= (unsigned long)LONG_MAX) b *= 2;
    sw.boundary = b;
    if (xioctl(fd, SNDRV_PCM_IOCTL_SW_PARAMS, &sw) < 0){ perror("SW_PARAMS"); return 1; }
    printf("playback: %u Hz, %u ch\n", rate, ch);

    /* Synthesize a 440 Hz sine with a short fade in/out (avoids a click). */
    unsigned n = (unsigned)(rate * SECS);
    int16_t *pcm = malloc((size_t)n * ch * 2);
    if (!pcm){ perror("malloc"); close(fd); return 1; }
    for (unsigned i = 0; i < n; i++){
        double env = 1.0;
        if (i < rate/50)        env = (double)i / (rate/50);
        else if (i > n-rate/50) env = (double)(n-i) / (rate/50);
        int16_t s = (int16_t)(env * 9000.0 * sin(2.0*M_PI*FREQ*i/rate));
        for (unsigned k = 0; k < ch; k++) pcm[i*ch+k] = s;
    }

    /* Write it. On an underrun (EPIPE) re-prepare and continue. */
    printf("tone: %.0f Hz for %.1fs\n", FREQ, SECS);
    xioctl(fd, SNDRV_PCM_IOCTL_PREPARE, NULL);
    uint8_t *p = (uint8_t*)pcm; unsigned frames = n, framesz = ch*2;
    while (frames){
        struct snd_xferi x = { .buf = p, .frames = frames, .result = 0 };
        if (xioctl(fd, SNDRV_PCM_IOCTL_WRITEI_FRAMES, &x) < 0){
            if (errno == EPIPE){ xioctl(fd, SNDRV_PCM_IOCTL_PREPARE, NULL); continue; }
            perror("WRITEI"); break;
        }
        if (x.result <= 0) break;
        p += (size_t)x.result * framesz; frames -= x.result;
    }
    xioctl(fd, SNDRV_PCM_IOCTL_DRAIN, NULL);

    free(pcm);
    close(fd);
    printf("done\n");
    return 0;
}
