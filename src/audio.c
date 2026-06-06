/* audio.c - raw-ioctl ALSA tool for the KidBright µAI (Allwinner V831 codec).
 *
 * Drives /dev/snd/pcmC0D0{p,c} DIRECTLY via the kernel SNDRV_PCM_IOCTL_*
 * interface -- no alsa-lib / libasound (none is in the musl sysroot), in the
 * same "full control, no vendor library" spirit as fbtest.c / v4l2cap.c.
 *
 * Modes:
 *   audio probe [p|c]          read-only: print the PCM's supported ranges/formats
 *   audio tone [FREQ] [SECS]   play a sine wave (speaker smoke test; default 440Hz 2s)
 *   audio play FILE.wav        play a 16-bit PCM WAV file
 *   audio rec  FILE.wav [SECS] record the mic to a 16-bit PCM WAV (default 3s)
 *
 * Fixed to S16_LE interleaved; rate/channels are negotiated and adapted to what
 * the codec actually accepts (the chosen values are read back after HW_PARAMS).
 * Card 0, device 0 (matches what the board exposes: controlC0 / pcmC0D0p/c).
 *
 * Build (board): arm-...-gcc -O2 -mcpu=cortex-a7 -mfpu=neon-vfpv4 \
 *                  -mfloat-abi=hard -o bin/audio src/audio.c -lm
 */
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

#define CARD 0
#define DEV  0
#define DEF_RATE 44100

static int xioctl(int fd, unsigned long req, void *arg){
    int r; do { r = ioctl(fd, req, arg); } while (r == -1 && errno == EINTR);
    return r;
}

/* ---- snd_pcm_hw_params accessors (the mask/interval flat-array ABI) -------- */
static struct snd_mask *pmask(struct snd_pcm_hw_params *p, int n){
    return &p->masks[n - SNDRV_PCM_HW_PARAM_FIRST_MASK];
}
static struct snd_interval *pintv(struct snd_pcm_hw_params *p, int n){
    return &p->intervals[n - SNDRV_PCM_HW_PARAM_FIRST_INTERVAL];
}
/* "any": every mask all-ones, every interval the full range -- the starting
 * point the kernel refines down to a real configuration. */
static void hw_any(struct snd_pcm_hw_params *p){
    memset(p, 0, sizeof *p);
    for (size_t i = 0; i < sizeof(p->masks)/sizeof(p->masks[0]); i++)
        memset(p->masks[i].bits, 0xff, sizeof p->masks[i].bits);
    for (size_t i = 0; i < sizeof(p->intervals)/sizeof(p->intervals[0]); i++){
        p->intervals[i].min = 0; p->intervals[i].max = UINT_MAX;
        p->intervals[i].openmin = p->intervals[i].openmax = 0;
        p->intervals[i].integer = p->intervals[i].empty = 0;
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
    i->min = i->max = val; i->integer = 1; i->openmin = i->openmax = i->empty = 0;
}
static unsigned intv_get(struct snd_pcm_hw_params *p, int n){ return pintv(p, n)->min; }

static int pcm_open(char dir){     /* dir = 'p' playback, 'c' capture */
    char path[64];
    snprintf(path, sizeof path, "/dev/snd/pcmC%dD%d%c", CARD, DEV, dir);
    int fd = open(path, O_RDWR);   /* blocking: writei/readi wait for the DMA */
    if (fd < 0) fprintf(stderr, "open %s: %s\n", path, strerror(errno));
    return fd;
}

/* Negotiate S16_LE interleaved at *rate/*ch; both are in/out (refined to what
 * the codec gives). period/buffer (frames) come back for the SW config. */
static int pcm_config(int fd, unsigned *rate, unsigned *ch,
                      snd_pcm_uframes_t *period, snd_pcm_uframes_t *buffer){
    struct snd_pcm_hw_params p; hw_any(&p);
    mask_one(&p, SNDRV_PCM_HW_PARAM_ACCESS, SNDRV_PCM_ACCESS_RW_INTERLEAVED);
    mask_one(&p, SNDRV_PCM_HW_PARAM_FORMAT, SNDRV_PCM_FORMAT_S16_LE);
    intv_one(&p, SNDRV_PCM_HW_PARAM_CHANNELS, *ch);
    intv_one(&p, SNDRV_PCM_HW_PARAM_RATE, *rate);
    p.rmask = ~0U;
    if (xioctl(fd, SNDRV_PCM_IOCTL_HW_PARAMS, &p) < 0) return -1;
    *rate   = intv_get(&p, SNDRV_PCM_HW_PARAM_RATE);
    *ch     = intv_get(&p, SNDRV_PCM_HW_PARAM_CHANNELS);
    *period = intv_get(&p, SNDRV_PCM_HW_PARAM_PERIOD_SIZE);
    *buffer = intv_get(&p, SNDRV_PCM_HW_PARAM_BUFFER_SIZE);

    struct snd_pcm_sw_params s; memset(&s, 0, sizeof s);
    s.tstamp_mode = SNDRV_PCM_TSTAMP_NONE;
    s.period_step = 1;
    s.avail_min = *period;
    s.start_threshold = *period;     /* auto-start once a period is queued */
    s.stop_threshold = *buffer;
    unsigned long b = *buffer ? *buffer : 1;   /* boundary: large 2^k * buffer */
    while (b * 2 <= (unsigned long)LONG_MAX) b *= 2;
    s.boundary = b;
    if (xioctl(fd, SNDRV_PCM_IOCTL_SW_PARAMS, &s) < 0) return -1;
    return 0;
}

/* config with a graceful channel fallback (mic/codec may be mono-only etc.) */
static int pcm_setup(int fd, unsigned *rate, unsigned *ch,
                     snd_pcm_uframes_t *period, snd_pcm_uframes_t *buffer){
    unsigned want_ch = *ch;
    if (pcm_config(fd, rate, ch, period, buffer) == 0) return 0;
    *ch = (want_ch == 1) ? 2 : 1; *rate = *rate ? *rate : DEF_RATE;
    fprintf(stderr, "  %u-ch refused, retrying %u-ch...\n", want_ch, *ch);
    return pcm_config(fd, rate, ch, period, buffer);
}

/* ---- blocking interleaved transfer (handles xrun by re-preparing) --------- */
static int xfer(int fd, unsigned long ioc, void *buf, unsigned frames, unsigned framesz){
    uint8_t *p = buf;
    while (frames){
        struct snd_xferi x = { .buf = p, .frames = frames, .result = 0 };
        if (xioctl(fd, ioc, &x) < 0){
            if (errno == EPIPE){ xioctl(fd, SNDRV_PCM_IOCTL_PREPARE, NULL); continue; }
            perror("xfer"); return -1;
        }
        if (x.result <= 0) break;
        p += (size_t)x.result * framesz; frames -= x.result;
    }
    return 0;
}

/* ---- minimal canonical WAV (PCM16) read/write ----------------------------- */
static uint32_t rd32(const uint8_t *p){ return p[0]|p[1]<<8|p[2]<<16|(uint32_t)p[3]<<24; }
static uint16_t rd16(const uint8_t *p){ return p[0]|p[1]<<8; }

static int16_t *wav_read(const char *path, unsigned *rate, unsigned *ch, unsigned *nframes){
    FILE *f = fopen(path, "rb");
    if (!f){ fprintf(stderr, "%s: %s\n", path, strerror(errno)); return NULL; }
    uint8_t h[12]; if (fread(h,1,12,f)!=12 || memcmp(h,"RIFF",4) || memcmp(h+8,"WAVE",4)){
        fprintf(stderr, "%s: not a WAV\n", path); fclose(f); return NULL; }
    unsigned bits=0, c=0, r=0; long dataoff=-1; uint32_t datalen=0;
    uint8_t ck[8];
    while (fread(ck,1,8,f)==8){
        uint32_t sz = rd32(ck+4);
        if (!memcmp(ck,"fmt ",4)){
            uint8_t fm[16]; if (fread(fm,1,16,f)!=16) break;
            if (rd16(fm)!=1){ fprintf(stderr,"only PCM WAV supported\n"); fclose(f); return NULL; }
            c=rd16(fm+2); r=rd32(fm+4); bits=rd16(fm+14);
            if (sz>16) fseek(f, sz-16, SEEK_CUR);
        } else if (!memcmp(ck,"data",4)){
            dataoff=ftell(f); datalen=sz; fseek(f, sz, SEEK_CUR);
        } else fseek(f, sz, SEEK_CUR);
        if (sz & 1) fseek(f, 1, SEEK_CUR);          /* chunks are word-aligned */
    }
    if (bits!=16 || dataoff<0){ fprintf(stderr,"need 16-bit PCM data chunk\n"); fclose(f); return NULL; }
    int16_t *pcm = malloc(datalen);
    fseek(f, dataoff, SEEK_SET);
    if (fread(pcm,1,datalen,f)!=datalen){ free(pcm); fclose(f); return NULL; }
    fclose(f);
    *rate=r; *ch=c; *nframes = datalen / (2*c);
    return pcm;
}

static void wr32(FILE *f, uint32_t v){ uint8_t b[4]={v,v>>8,v>>16,v>>24}; fwrite(b,1,4,f); }
static void wr16(FILE *f, uint16_t v){ uint8_t b[2]={v,v>>8}; fwrite(b,1,2,f); }
static int wav_write(const char *path, const int16_t *pcm, unsigned nframes,
                     unsigned rate, unsigned ch){
    FILE *f = fopen(path, "wb");
    if (!f){ fprintf(stderr,"%s: %s\n",path,strerror(errno)); return -1; }
    uint32_t datalen = nframes * 2 * ch;
    fwrite("RIFF",1,4,f); wr32(f, 36+datalen); fwrite("WAVE",1,4,f);
    fwrite("fmt ",1,4,f); wr32(f,16); wr16(f,1); wr16(f,ch);
    wr32(f,rate); wr32(f, rate*ch*2); wr16(f, ch*2); wr16(f,16);
    fwrite("data",1,4,f); wr32(f, datalen);
    fwrite(pcm,1,datalen,f);
    fclose(f);
    return 0;
}

/* ---- modes ---------------------------------------------------------------- */
static int do_probe(char dir){
    int fd = pcm_open(dir); if (fd<0) return 1;
    struct snd_pcm_hw_params p; hw_any(&p);
    if (xioctl(fd, SNDRV_PCM_IOCTL_HW_REFINE, &p) < 0){ perror("HW_REFINE"); close(fd); return 1; }
    struct snd_interval *rate = pintv(&p, SNDRV_PCM_HW_PARAM_RATE);
    struct snd_interval *chan = pintv(&p, SNDRV_PCM_HW_PARAM_CHANNELS);
    struct snd_interval *psz  = pintv(&p, SNDRV_PCM_HW_PARAM_PERIOD_SIZE);
    struct snd_interval *bsz  = pintv(&p, SNDRV_PCM_HW_PARAM_BUFFER_SIZE);
    printf("pcmC%dD%d%c: rate %u..%u Hz, channels %u..%u, period %u..%u, buffer %u..%u frames\n",
           CARD, DEV, dir, rate->min, rate->max, chan->min, chan->max,
           psz->min, psz->max, bsz->min, bsz->max);
    static const struct { int bit; const char *n; } F[] = {
        {0,"S8"},{1,"U8"},{2,"S16_LE"},{3,"S16_BE"},{6,"S24_LE"},{10,"S32_LE"},{14,"FLOAT_LE"} };
    struct snd_mask *fm = pmask(&p, SNDRV_PCM_HW_PARAM_FORMAT);
    printf("  formats:");
    for (size_t i=0;i<sizeof F/sizeof F[0];i++)
        if (fm->bits[F[i].bit>>5] & (1u<<(F[i].bit&31))) printf(" %s", F[i].n);
    printf("\n");
    close(fd);
    return 0;
}

static int play_pcm(int16_t *pcm, unsigned nframes, unsigned rate, unsigned ch){
    int fd = pcm_open('p'); if (fd<0) return 1;
    snd_pcm_uframes_t period, buffer;
    if (pcm_setup(fd, &rate, &ch, &period, &buffer) < 0){ perror("HW/SW params"); close(fd); return 1; }
    printf("playback: %u Hz, %u ch, period %lu, buffer %lu frames\n", rate, ch,
           (unsigned long)period, (unsigned long)buffer);
    xioctl(fd, SNDRV_PCM_IOCTL_PREPARE, NULL);
    int rc = xfer(fd, SNDRV_PCM_IOCTL_WRITEI_FRAMES, pcm, nframes, ch*2);
    xioctl(fd, SNDRV_PCM_IOCTL_DRAIN, NULL);
    close(fd);
    return rc ? 1 : 0;
}

static int do_tone(int argc, char **argv){
    double freq = argc>0 ? atof(argv[0]) : 440.0;
    double secs = argc>1 ? atof(argv[1]) : 2.0;
    unsigned rate = DEF_RATE, ch = 2, n = (unsigned)(rate*secs);
    int16_t *pcm = malloc((size_t)n*ch*2);
    for (unsigned i=0;i<n;i++){
        double env = 1.0;                                   /* short fade in/out */
        if (i < rate/50) env = (double)i/(rate/50);
        else if (i > n-rate/50) env = (double)(n-i)/(rate/50);
        int16_t s = (int16_t)(env * 9000.0 * sin(2.0*M_PI*freq*i/rate));
        for (unsigned k=0;k<ch;k++) pcm[i*ch+k]=s;
    }
    printf("tone: %.0f Hz for %.1fs\n", freq, secs);
    int rc = play_pcm(pcm, n, rate, ch);
    free(pcm);
    return rc;
}

static int do_play(const char *path){
    unsigned rate, ch, n;
    int16_t *pcm = wav_read(path, &rate, &ch, &n);
    if (!pcm) return 1;
    printf("%s: %u Hz, %u ch, %u frames (%.1fs)\n", path, rate, ch, n, (double)n/rate);
    int rc = play_pcm(pcm, n, rate, ch);
    free(pcm);
    return rc;
}

static int do_rec(const char *path, double secs){
    int fd = pcm_open('c'); if (fd<0) return 1;
    unsigned rate = DEF_RATE, ch = 1;
    snd_pcm_uframes_t period, buffer;
    if (pcm_setup(fd, &rate, &ch, &period, &buffer) < 0){ perror("HW/SW params"); close(fd); return 1; }
    unsigned n = (unsigned)(rate*secs);
    int16_t *pcm = malloc((size_t)n*ch*2);
    printf("recording %.1fs: %u Hz, %u ch...\n", secs, rate, ch);
    xioctl(fd, SNDRV_PCM_IOCTL_PREPARE, NULL);
    xioctl(fd, SNDRV_PCM_IOCTL_START, NULL);
    int rc = xfer(fd, SNDRV_PCM_IOCTL_READI_FRAMES, pcm, n, ch*2);
    close(fd);
    if (rc==0) rc = wav_write(path, pcm, n, rate, ch);
    if (rc==0) printf("wrote %s (%u frames)\n", path, n);
    free(pcm);
    return rc ? 1 : 0;
}

static void usage(void){
    fprintf(stderr,
      "audio - raw-ioctl ALSA tool for the KidBright uAI\n"
      "usage:\n"
      "  audio probe [p|c]          print supported ranges/formats (default p)\n"
      "  audio tone [FREQ] [SECS]   play a sine wave (default 440 2)\n"
      "  audio play FILE.wav        play a 16-bit PCM WAV\n"
      "  audio rec FILE.wav [SECS]  record mic to 16-bit PCM WAV (default 3)\n");
}

int main(int argc, char **argv){
    setvbuf(stdout, NULL, _IONBF, 0);
    if (argc < 2){ usage(); return 1; }
    const char *m = argv[1];
    if (!strcmp(m,"probe"))      return do_probe(argc>2 ? argv[2][0] : 'p');
    else if (!strcmp(m,"tone"))  return do_tone(argc-2, argv+2);
    else if (!strcmp(m,"play")){ if (argc<3){ usage(); return 1; } return do_play(argv[2]); }
    else if (!strcmp(m,"rec")){  if (argc<3){ usage(); return 1; }
                                 return do_rec(argv[2], argc>3 ? atof(argv[3]) : 3.0); }
    usage();
    return 1;
}
