/* kbdk-example
 * name: LED blink (GPIO)
 * desc: blink an LED via the raw /dev/gpiochip chardev ioctl (GPIO_V2, v1 fallback)
 * extra_args:
 */
/* Teaching copy of src/ledblink.c — keep the two in sync (the production file is
 * the source of truth; this is it plus the metadata header and lighter comments).
 *
 * Blinks a GPIO line through the kernel's GPIO CHARACTER DEVICE — no vendor lib,
 * no sysfs export, the same direct-ioctl spirit as the screen/audio examples.
 *
 * !! PIN IS A SAFE PLACEHOLDER — CONFIRM ON HARDWARE !!
 * The investigation (docs/research/2026-06-21-led-gpio-investigation.md) found NO
 * discrete GPIO user LED declared in the running V831/M2Dock device tree. The
 * defaults below will NOT light anything; worse, some output lines on this board
 * are the camera reset/power-down and Wi-Fi/LCD control — DO NOT point this at
 * lines 144/145 (camera) or 117/228/229 (LCD/Wi-Fi). Sweep candidate lines with
 * the argv overrides until one visibly drives an LED, then bake it into LED_LINE:
 *
 *     /tmp/ledblink /dev/gpiochip1 <candidate-line> 500 6   # 6 blinks @ 2 Hz
 *
 * On this kernel the MAIN PIO bank (PA..PI) is /dev/gpiochip1 (base 0); gpiochip0
 * is the small R_PIO/PL bank. The kernel auto-releases the line on close, so a
 * wrong guess is reversible — except for the avoid-list lines, which have real
 * side effects.
 *
 * ABI: GPIO_V2 (Linux >= 5.10) is tried first; this board is Linux 4.9.118, so it
 * falls back to the v1 chardev (GPIO_GET_LINEHANDLE_IOCTL, since 4.8). Sleeping
 * uses the plain `nanosleep` symbol (aw_nanosleep alias) to dodge the musl-1.2
 * time64 redirect the board's musl 1.1.16 can't resolve.
 *
 * Usage:  ledblink [chip] [line] [period_ms] [count]   (count<=0 = forever)
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

/* The three hardware facts — SAFE PLACEHOLDERS, see the header comment. */
#define LED_CHIP "/dev/gpiochip1"   /* HW-CONFIRM: main PIO bank (base 0) on this kernel */
#define LED_LINE 0u                 /* HW-CONFIRM: line offset (0 = PA0, a placeholder) */
#define LED_ACTIVE_LOW 0            /* HW-CONFIRM: 1 if the LED sinks (on = low) */
#define CONSUMER "kbdk-ledblink"

/* Bind the plain nanosleep symbol (the board's musl lacks __nanosleep_time64). */
extern int aw_nanosleep(const struct timespec *, struct timespec *) __asm__("nanosleep");

static volatile sig_atomic_t g_stop = 0;
static void on_stop(int s){ (void)s; g_stop = 1; }

static void sleep_ms(long ms){
    struct timespec ts = { .tv_sec = ms / 1000, .tv_nsec = (ms % 1000) * 1000000L };
    while (aw_nanosleep(&ts, &ts) == -1 && errno == EINTR && !g_stop) { /* resume */ }
}

/* GPIO_V2 (Linux >= 5.10): request one line as output, return its line fd. */
static int line_request_v2(int chipfd, unsigned line, int active_low){
    struct gpio_v2_line_request req;
    memset(&req, 0, sizeof req);
    req.offsets[0] = line;
    req.num_lines  = 1;
    req.config.flags = GPIO_V2_LINE_FLAG_OUTPUT;
    if (active_low) req.config.flags |= GPIO_V2_LINE_FLAG_ACTIVE_LOW;
    strncpy(req.consumer, CONSUMER, sizeof req.consumer - 1);
    if (ioctl(chipfd, GPIO_V2_GET_LINE_IOCTL, &req) < 0) return -1;
    return req.fd;
}
static int line_set_v2(int linefd, int level){
    struct gpio_v2_line_values vals;
    memset(&vals, 0, sizeof vals);
    vals.mask = 1; vals.bits = level ? 1 : 0;
    return ioctl(linefd, GPIO_V2_LINE_SET_VALUES_IOCTL, &vals);
}

/* GPIO v1 fallback (Linux >= 4.8 — what the 4.9 board actually has). */
static int line_request_v1(int chipfd, unsigned line, int active_low){
    struct gpiohandle_request req;
    memset(&req, 0, sizeof req);
    req.lineoffsets[0] = line;
    req.lines          = 1;
    req.flags          = GPIOHANDLE_REQUEST_OUTPUT;
    if (active_low) req.flags |= GPIOHANDLE_REQUEST_ACTIVE_LOW;
    req.default_values[0] = 0;        /* start off (matches src/ledblink.c) */
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

    signal(SIGTERM, on_stop); signal(SIGINT, on_stop);

    int chipfd = open(chip, O_RDWR | O_CLOEXEC);
    if (chipfd < 0){ fprintf(stderr, "open %s: %s\n", chip, strerror(errno)); return 1; }

    /* Try GPIO_V2; fall back to v1 if this kernel rejects it (ENOTTY/EINVAL). */
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

    long n = 0; int level = 0;
    while (!g_stop && (count <= 0 || n < count * 2)){
        level = !level;
        if (set_value(linefd, level) < 0){ fprintf(stderr, "set value: %s\n", strerror(errno)); break; }
        sleep_ms(half);
        n++;
    }

    set_value(linefd, 0);    /* leave it off, then release (close hands the pin back) */
    close(linefd);
    close(chipfd);
    return 0;
}
