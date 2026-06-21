/* ledblink.c - blink an LED on the KidBright µAI (Allwinner V831) via the raw
 * GPIO character device (/dev/gpiochipN), no vendor library — the same
 * "direct ioctl / mmap, full control" spirit as fbtest.c (FBIO*) and audio.c
 * (SNDRV_PCM_IOCTL_*). This is the toolkit's first GPIO program.
 *
 * --------------------------------------------------------------------------
 * PIN SELECTION — READ docs/research/2026-06-21-led-gpio-investigation.md
 * --------------------------------------------------------------------------
 * That on-hardware investigation found, with high confidence, that the running
 * V831/M2Dock device tree declares NO discrete software-controllable GPIO user
 * LED: there is no `gpio-leds` node, /sys/class/leds is empty, and every claimed
 * output line is a known non-LED function. In particular:
 *
 *     /dev/gpiochip1 line 144 = PE16 = OV2685 CAMERA RESET      <- DO NOT DRIVE
 *     /dev/gpiochip1 line 145 = PE17 = OV2685 CAMERA POWER-DOWN <- DO NOT DRIVE
 *     /dev/gpiochip1 lines 117/229 = LCD control/reset, 228 = Wi-Fi enable
 *
 * Driving those would reset the camera / Wi-Fi / LCD, not blink an LED. So the
 * defaults below are deliberately a SAFE PLACEHOLDER, not a guessed LED:
 *
 *   - LED_CHIP defaults to the MAIN PIO bank, which on THIS 4.9 kernel is
 *     /dev/gpiochip1 (base 0, PA..PI) — NOT gpiochip0 (that is the small
 *     R_PIO/PL bank, base 352). Getting the chip wrong lands on the wrong
 *     controller entirely.
 *   - LED_LINE defaults to 0u (UNCONFIRMED). On gpiochip1 line 0 is PA0, which
 *     the report saw as an idle input — toggling it is harmless but will not
 *     light anything. You MUST set the real line for your physical board.
 *   - LED_ACTIVE_LOW defaults to 0 (on = drive high); flip to 1 if the LED
 *     sinks to the pin (on = drive low).
 *
 * HOW TO CONFIRM ON HARDWARE (a human with eyes on the board — see the report's
 * "SAFE step-by-step procedure"): pick a candidate line that is NOT in the
 * avoid-list (117/144/145/228/229) and is GPIO-muxed (check
 * /sys/kernel/debug/sunxi_pinctrl/function), then sweep it WITHOUT recompiling
 * via the argv overrides:
 *
 *     /tmp/ledblink /dev/gpiochip1 <candidate-line> 500 6   # 6 blinks @ 2 Hz
 *
 * When a line visibly drives an LED, record chip=gpiochip1 line=<N> and the SoC
 * pin (bank*32+pin) and bake it into the LED_* #defines below + the report +
 * CLAUDE.md. The line is auto-released by the kernel on close, so a wrong guess
 * is reversible (only the avoid-list lines have side effects — never default
 * to them).
 *
 * --------------------------------------------------------------------------
 * ABI — GPIO_V2 chardev, with a v1 fallback for this old kernel
 * --------------------------------------------------------------------------
 * The settled design is the GPIO_V2 chardev ioctl (gpio_v2_line_request +
 * GPIO_V2_GET_LINE_IOCTL + GPIO_V2_LINE_SET_VALUES_IOCTL). BUT the board runs
 * Linux 4.9.118 and the GPIO_V2 uABI only landed in Linux 5.10 — so on this
 * kernel GPIO_V2_GET_LINE_IOCTL is very likely ENOTTY/EINVAL. The legacy v1
 * chardev (GPIO_GET_LINEHANDLE_IOCTL, present since 4.8) is the realistic path.
 * This file therefore TRIES V2 first and, on ENOTTY/EINVAL, falls back to v1 —
 * one self-contained file, same #defines, same flow (request a line, then set
 * its value). Both ABIs live in the cross sysroot's <linux/gpio.h>.
 *
 * Sleeping uses nanosleep, NOT usleep/select: those are time64-redirected by the
 * musl-1.2 cross toolchain and would fail to bind on the board's musl 1.1.16.
 *
 * Usage:  ledblink [chip] [line] [period_ms] [count]
 *   chip       gpiochip device path           (default LED_CHIP)
 *   line       line offset on that chip        (default LED_LINE)
 *   period_ms  full on+off period in ms        (default 1000 -> 1 Hz)
 *   count      number of blinks, <=0 = forever (default 0 -> until SIGINT/TERM)
 *
 * Build (board):
 *   arm-unknown-linux-musleabihf-gcc -O2 -mcpu=cortex-a7 -mfpu=neon-vfpv4 \
 *       -mfloat-abi=hard -o bin/ledblink src/ledblink.c
 * (no -lm, no MPP, no dlopen — just libc + the kernel UAPI header). See
 * `make ledblink` / `make deploy-ledblink`.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <signal.h>
#include <time.h>
#include <sys/ioctl.h>
#include <linux/gpio.h>

/* ---- The three hardware facts (from the investigation report) ------------ */
/* LED pin — from docs/research/2026-06-21-led-gpio-investigation.md.
 * SAFE PLACEHOLDERS: the report found no discrete GPIO LED in the running DT.
 * Confirm on hardware before trusting (see the header comment). */
#define LED_CHIP "/dev/gpiochip1"   /* HW-CONFIRM: main PIO bank (base 0) on this kernel */
#define LED_LINE 0u                 /* HW-CONFIRM: line offset on LED_CHIP (0 = PA0, a placeholder) */
#define LED_ACTIVE_LOW 0            /* HW-CONFIRM: 1 if the LED sinks (on = low) */

#define CONSUMER "kbdk-ledblink"

/* musl time64 trap: the musl-1.2 cross toolchain's <time.h> does
 *   __REDIR(nanosleep, __nanosleep_time64)
 * so a plain nanosleep() call links against __nanosleep_time64, which the
 * board's musl 1.1.16 does NOT export -> "symbol not found" at load (the same
 * trap camcc.c dodges for dlsym). Bind the plain `nanosleep` symbol directly,
 * matching the board's 32-bit-time struct timespec. (We use ms granularity, so
 * the time64 struct difference never bites — the kernel's 4.9 nanosleep takes
 * the 32-bit timespec the board's libc passes.) */
extern int aw_nanosleep(const struct timespec *, struct timespec *) __asm__("nanosleep");

static volatile sig_atomic_t g_stop = 0;
static void on_stop(int s){ (void)s; g_stop = 1; }

/* Sleep `ms` milliseconds (time64-safe on the board's musl, see aw_nanosleep). */
static void sleep_ms(long ms){
    struct timespec ts = { .tv_sec = ms / 1000, .tv_nsec = (ms % 1000) * 1000000L };
    while (aw_nanosleep(&ts, &ts) == -1 && errno == EINTR && !g_stop) { /* resume */ }
}

/* ---- GPIO_V2 path (Linux >= 5.10) ----------------------------------------
 * Request the single line as an output, return a per-line fd, or -1 (with
 * errno preserved) so the caller can decide whether to fall back to v1. */
static int line_request_v2(int chipfd, unsigned line, int active_low){
    struct gpio_v2_line_request req;
    memset(&req, 0, sizeof req);
    req.offsets[0] = line;
    req.num_lines  = 1;
    req.config.flags = GPIO_V2_LINE_FLAG_OUTPUT;
    if (active_low) req.config.flags |= GPIO_V2_LINE_FLAG_ACTIVE_LOW;
    strncpy(req.consumer, CONSUMER, sizeof req.consumer - 1);
    if (ioctl(chipfd, GPIO_V2_GET_LINE_IOCTL, &req) < 0) return -1;
    return req.fd;          /* the dedicated line fd */
}
static int line_set_v2(int linefd, int level){
    struct gpio_v2_line_values vals;
    memset(&vals, 0, sizeof vals);
    vals.mask = 1;                    /* line index 0 */
    vals.bits = level ? 1 : 0;
    return ioctl(linefd, GPIO_V2_LINE_SET_VALUES_IOCTL, &vals);
}

/* ---- GPIO v1 fallback (Linux >= 4.8 — what the 4.9 board actually has) ----
 * Same idea with the legacy gpiohandle_request / gpiohandle_data structs. */
static int line_request_v1(int chipfd, unsigned line, int active_low){
    struct gpiohandle_request req;
    memset(&req, 0, sizeof req);
    req.lineoffsets[0]  = line;
    req.lines           = 1;
    req.flags           = GPIOHANDLE_REQUEST_OUTPUT;
    if (active_low) req.flags |= GPIOHANDLE_REQUEST_ACTIVE_LOW;
    req.default_values[0] = 0;        /* start off */
    strncpy(req.consumer_label, CONSUMER, sizeof req.consumer_label - 1);
    if (ioctl(chipfd, GPIO_GET_LINEHANDLE_IOCTL, &req) < 0) return -1;
    return req.fd;
}
static int line_set_v1(int linefd, int level){
    struct gpiohandle_data data;
    memset(&data, 0, sizeof data);
    data.values[0] = level ? 1 : 0;
    return ioctl(linefd, GPIOHANDLE_SET_LINE_VALUES_IOCTL, &data);
}

int main(int argc, char **argv){
    setvbuf(stdout, NULL, _IONBF, 0);
    const char *chip = (argc > 1) ? argv[1] : LED_CHIP;
    unsigned    line = (argc > 2) ? (unsigned)strtoul(argv[2], NULL, 0) : LED_LINE;
    long      period = (argc > 3) ? strtol(argv[3], NULL, 0) : 1000;   /* ms */
    long       count = (argc > 4) ? strtol(argv[4], NULL, 0) : 0;      /* 0 = forever */
    if (period < 2) period = 2;
    long half = period / 2;

    signal(SIGTERM, on_stop);
    signal(SIGINT,  on_stop);

    int chipfd = open(chip, O_RDWR | O_CLOEXEC);
    if (chipfd < 0){ fprintf(stderr, "open %s: %s\n", chip, strerror(errno)); return 1; }

    /* Try the modern GPIO_V2 ABI; fall back to v1 if this kernel rejects it. */
    const char *abi = "v2";
    int linefd = line_request_v2(chipfd, line, LED_ACTIVE_LOW);
    int (*set_value)(int, int) = line_set_v2;
    if (linefd < 0 && (errno == ENOTTY || errno == EINVAL)){
        abi = "v1";
        linefd = line_request_v1(chipfd, line, LED_ACTIVE_LOW);
        set_value = line_set_v1;
    }
    if (linefd < 0){
        fprintf(stderr, "request %s line %u: %s\n", chip, line, strerror(errno));
        close(chipfd);
        return 1;
    }
    printf("ledblink: %s line %u (%s ABI), period %ld ms, count %ld%s\n",
           chip, line, abi, period, count, count <= 0 ? " (forever)" : "");

    long n = 0;
    int level = 0;
    while (!g_stop && (count <= 0 || n < count * 2)){
        level = !level;
        if (set_value(linefd, level) < 0){
            fprintf(stderr, "set value: %s\n", strerror(errno));
            break;
        }
        sleep_ms(half);
        n++;
    }

    /* Leave the line off, then release it. Closing the line fd hands the pin
     * back to its default state (the kernel releases the request on close). */
    set_value(linefd, 0);
    close(linefd);
    close(chipfd);
    return 0;
}
