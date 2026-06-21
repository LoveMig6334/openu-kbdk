# LED / GPIO investigation — KidBright µAI (Allwinner V831)

Date: 2026-06-21
Author: read-only on-hardware investigation (board attached via ADB, serial 20080411)
Purpose: find which `{gpiochip, line}` drives the on-board LED so a new `ledblink.c`
can pick correct default `#define`s.

> **Headline finding:** On this board's running device tree there is **no
> software-controllable discrete user LED** that is exposed as a GPIO. There is no
> `gpio-leds` DT node, no `/sys/class/leds`, and **every** GPIO line the kernel has
> claimed is accounted for as a non-LED function (camera reset/power-down, LCD
> reset/CS, Wi-Fi enable). The two output lines that *looked* like an LED at first
> glance (PE16/PE17) are the **OV2685 camera reset and power-down** — driving them
> would reset the camera, not blink an LED. This is exactly why the task was
> correctly scoped read-only.
>
> The only user-visible, software-dimmable light on the board is the **LCD
> backlight**, which is on **PWM channel 1 (`pwmchip0` / `pwm1`)**, not a GPIO.

This was investigated live on hardware, so the confidence in the *negative* result
(no GPIO LED in the current DT) is high. The confidence in the *recommendation* is
discussed honestly below.

---

## Board identity

```
model            : sun8iw19  /  allwinner,sun8iw19p1   (V831, Cortex-A7, armv7l)
kernel           : Linux 4.9.118 #3849 PREEMPT (Tina/musl/BusyBox)
soc@.../board    : device=m2dock  lcd=st7789,1.3,240,240  sensor=sp2305,...  wifi=rtl8189fs
lcd driver       : st7789v_cpu  (CPU/MCU-bus 240x240 panel; matches CLAUDE.md fb0)
```

So the KidBright µAI's running DTB is the **Sipeed MaixII-Dock (M2Dock)** V831
profile. (Note `board sensor=sp2305` in the DT string, but the *bound* sensor is
`ov2685_mipi` — see `vind@0/sensor@0/sensor0_mname` below and CLAUDE.md's camera row.
The DT `board` string is a stale BSP default; the actual sensor node is OV2685.)

---

## Method (all READ-ONLY — nothing was toggled or written to any GPIO)

1. `adb devices` → board present (`20080411  device`). (Fallback `./bin/uai` not needed.)
2. Enumerated GPIO infrastructure:
   - `ls /dev/gpiochip*` → `gpiochip0`, `gpiochip1`
   - `/sys/class/gpio/gpiochip*/{label,base,ngpio}`
   - `cat /sys/kernel/debug/gpio` (the authoritative list of *claimed* lines)
   - `ls /sys/class/leds/` → **empty** (no leds class)
   - searched for `gpiodetect/gpioinfo/gpioget/gpioset` → **not installed**
3. Searched the live device tree for LED nodes:
   - `find /proc/device-tree -iname '*led*'` → nothing
   - scanned every `compatible` for `gpio-leds` → nothing
   - listed all `soc@03000000/*` children → no LED node
4. Decoded every claimed GPIO line to a SoC pin and matched it to its DT owner by
   hex-dumping the sunxi GPIO descriptor properties (`lcd_gpio_0/1`, `wlan_regon`,
   `sensor0_reset`, `sensor0_pwdn`) and confirming via
   `/sys/kernel/debug/sunxi_pinctrl/{sunxi_pin,function,data}`.
5. Brute-scanned **every pin** in banks PA, PC, PD, PE, PF, PG, PH for GPIO-output
   mode (`function == 1`) via the sunxi pinctrl debug interface, to catch an LED
   driven directly by pinctrl without a `gpio_request` (which would not show in
   `/sys/kernel/debug/gpio`).
6. Checked the input subsystem (`/proc/bus/input/devices`) and PWM
   (`/sys/class/pwm/pwmchip0/*`) for a backlight/LED.

### Key decode fact (how the pin numbers were derived)

Linux sunxi GPIO global number = `bank_index * 32 + pin`, with
`A=0, B=1, C=2, D=3, E=4, F=5, G=6, H=7, I=8`.
The sunxi DT GPIO descriptor (28 bytes) is
`<&pio bank pin func pull drv data>`. This was *validated against a known line*:
`wlan_regon` decodes to `bank=7(PH) pin=4` = `7*32+4 = 228`, and
`/sys/kernel/debug/gpio` independently labels `gpio-228` as `wlan_regon`. Match. ✔

---

## Evidence

### 1. GPIO controllers

```
/dev/gpiochip0  254,0   label=r_pio   base=352  ngpio=32     (PL bank, "R_PIO"/AON)
/dev/gpiochip1  254,1   label=pio     base=0    ngpio=288    (PA..PI main pinctrl)
```

Note the kbdk/CLAUDE.md mapping ("gpiochip0 + gpiochip1") is correct, but **which
chip is the main PIO matters**: on *this* kernel the **main PIO bank (PA-PI) is
`gpiochip1` (base 0)** and the small R_PIO/PL bank is `gpiochip0` (base 352). A
naive "use gpiochip0" assumption would land on the wrong controller.

### 2. `/sys/class/leds/` — EMPTY. No `gpio-leds` node anywhere in the DT.

### 3. All claimed GPIO lines (`/sys/kernel/debug/gpio`), fully decoded

| line     | SoC pin | dir / level | DT owner (property)                  | function |
|----------|---------|-------------|--------------------------------------|----------|
| gpio-0   | PA0     | in  lo      | (unlabeled, input — idle)            | not an LED |
| gpio-117 | PD21    | in  lo      | `lcd0@.../lcd_gpio_1`                 | LCD control |
| gpio-144 | **PE16**| out **hi**  | `vind@0/sensor@0/sensor0_reset`      | **camera reset** |
| gpio-145 | **PE17**| out **lo**  | `vind@0/sensor@0/sensor0_pwdn`       | **camera power-down** |
| gpio-228 | PH4     | out hi      | `wlan/wlan_regon`                    | Wi-Fi enable |
| gpio-229 | PH5     | in  hi      | `lcd0@.../lcd_gpio_0`                 | LCD reset/CS |

Raw descriptor dumps that establish the camera ownership (the lines that look most
LED-like):

```
vind@0/sensor@0/sensor0_reset : 00 00 00 63  00 00 00 04  00 00 00 10 ...  -> &pio bank4(PE) pin0x10(16) = PE16 = gpio-144
vind@0/sensor@0/sensor0_pwdn  : 00 00 00 63  00 00 00 04  00 00 00 11 ...  -> &pio bank4(PE) pin0x11(17) = PE17 = gpio-145
vind@0/sensor@0/sensor0_mname : "ov2685_mipi"
```

(`0x63` is the `pio` node's phandle — confirmed by reading
`pinctrl@0300b000/phandle` = `00 00 00 63`.)

And the LCD / Wi-Fi lines, for completeness:

```
lcd0@.../lcd_gpio_0 : 63 .. 07 .. 05 ...  -> PH5  = gpio-229   (lcd reset/CS)
lcd0@.../lcd_gpio_1 : 63 .. 03 .. 15 ...  -> PD21 = gpio-117   (lcd control)
wlan/wlan_regon     : 63 .. 07 .. 04 ...  -> PH4  = gpio-228   (wifi enable)
```

### 4. Full pin scan for "secret" LED outputs not claimed via gpio_request

Scanned PA/PC/PD/PE/PF/PG/PH, all pins, for `function == 1` (sunxi GPIO output).
The **complete** set of GPIO-output pins on the board is:

```
PD21  func=1 data=0   (lcd_gpio_1)
PE16  func=1 data=1   (camera reset)
PE17  func=1 data=0   (camera pwdn)
PH4   func=1 data=1   (wlan_regon)
PH5   func=1 data=1   (lcd_gpio_0)
```

There is **no additional output pin** that could be a dormant/unclaimed LED. Every
output is one of the five known functions above.

### 5. Backlight / PWM (the only user-visible software-controllable light)

```
lcd0@.../lcd_pwm_used = 1
lcd0@.../lcd_pwm_ch   = 1        -> PWM channel 1
lcd0@.../lcd_pwm_freq = 0x0000c350 = 50000 Hz
lcd0@.../lcd_pwm_pol  = 1
lcd0@.../lcd_backlight= 0x32 (50)   (and disp lcd0_backlight/lcd0_bright = 50)
pwmchip0 -> .../soc/300a000.pwm/pwm/pwmchip0   (CLAUDE.md: "pwmchip0 for backlight/buzzer")
```

No `pwm*` channel was *exported* under `/sys/class/pwm/pwmchip0/` (the disp driver
holds the backlight channel internally), so the backlight cannot be poked from
sysfs without unbinding the LCD. Dimming/blinking the backlight is a `pwmchip0`
channel-1 job (raw PWM register or sysfs export), **not a GPIO**.

### 6. Buttons are analog, not GPIO

`/proc/bus/input/devices` shows the single input is `sunxi-gpadc0` (event0) — the
board buttons are on the GPADC, confirming CLAUDE.md. Not relevant to an LED, but
rules out a "button + adjacent LED on GPIO" pattern.

---

## Ranked candidates for `ledblink.c` default `{gpiochip, line}`

Confidence reflects "is this actually a discrete user LED I can blink and see?"

| rank | candidate | confidence | evidence |
|------|-----------|-----------|----------|
| 1 | **PWM backlight — `pwmchip0` channel 1 (NOT a GPIO)** | medium | Only user-visible software-controllable light. `lcd_pwm_ch=1`, lit at boot. But it dims the screen, not a discrete LED, and the disp driver owns the channel. |
| 2 | none / `gpiochip1` line TBD on the real KidBright unit | low | No GPIO LED exists in *this* DT; if the KidBright variant adds one it would be on a currently-free `pio` pin — must be confirmed against the physical unit + schematic. |
| 3 | PE16 = `gpiochip1` line 144 | low (DO NOT USE) | It is an output, but it is the **camera reset** line. Listed only to explicitly warn against it. |
| 4 | PE17 = `gpiochip1` line 145 | low (DO NOT USE) | Output, but the **camera power-down** line. Warn-only. |

I am deliberately **not** giving a high-confidence GPIO recommendation, because the
evidence says no GPIO LED is wired/declared on the running board. Picking PE16/PE17
just because they are outputs would be wrong and would disturb the camera.

---

## Recommendation for `ledblink.c` defaults

There is no honest high-confidence GPIO LED to hardcode. Two defensible paths:

- **If "the LED" means the only thing a user can see blink: target the backlight
  PWM.** Default to `pwmchip0`, channel `1`. This is *not* GPIO and would make
  `ledblink.c` really a "backlight blink" demo. It is the only guaranteed-visible
  light. (Honest caveat: the disp driver holds this channel; a clean implementation
  pokes the PWM registers / temporarily exports the channel.)

- **If `ledblink.c` must be a true GPIO blink demo:** make the chip/line a
  *parameter* with conservative, clearly-commented defaults and a loud "VERIFY ON
  YOUR BOARD" note, e.g.:

  ```c
  /* No discrete user LED is declared in the running V831/M2Dock device tree.
   * These are PLACEHOLDERS — confirm with the SAFE procedure below before trusting.
   * The main PIO bank (PA..PI) is /dev/gpiochip1 on this kernel (base 0);
   * /dev/gpiochip0 is the small R_PIO/PL bank (base 352). */
  #define LED_GPIOCHIP   "/dev/gpiochip1"
  #define LED_LINE       /* UNKNOWN — set after on-hardware confirmation */ 0
  /* DO NOT default to lines 144 (PE16, camera reset) or 145 (PE17, camera pwdn). */
  ```

  and document that the user should find a *free* `pio` pin wired to the KidBright
  LED from the schematic, then set `LED_LINE` accordingly.

My concrete suggestion: ship `ledblink.c` so that **chip and line are CLI
arguments** (e.g. `ledblink <gpiochip> <line>`), with the safe placeholder defaults
above, and add the SAFE procedure below to its header comment. That keeps the tool
honest and prevents an accidental camera reset.

---

## SAFE step-by-step procedure for a human to confirm the real LED line

You need eyes on the board for this — only a human can see the LED. Use the modern
character-device GPIO ABI (`gpioset` from libgpiod) if you add it to the rootfs, or
a tiny C probe; **do not** use the legacy `/sys/class/gpio` export, and **never
touch lines 144/145 (camera) or 228 (Wi-Fi) or 117/229 (LCD)**.

1. **List the safe set.** First confirm the claimed lines so you avoid them:
   ```sh
   adb shell cat /sys/kernel/debug/gpio
   ```
   Avoid: gpio-117, 144, 145, 228, 229 (and don't disturb PA0/gpio-0).

2. **Prefer libgpiod, transient + auto-release.** If `gpioset`/`gpiodetect` are
   present (`adb shell which gpioset`), set a *candidate* line momentarily so it
   auto-releases on exit (no permanent state, no fighting a driver):
   ```sh
   # Replace LINE with the candidate; this is on gpiochip1 (main PIO).
   adb shell gpioset --mode=time --sec=2 gpiochip1 LINE=1   # drive high 2s, watch LED
   adb shell gpioset --mode=time --sec=2 gpiochip1 LINE=0   # drive low  2s, watch LED
   ```
   Watch the board: the candidate is the LED if it changes brightness/state in sync.

3. **If libgpiod is absent**, build a tiny `ledblink.c` that opens
   `/dev/gpiochip1`, requests the candidate line as output with
   `GPIO_V2_LINE_*`/`GPIOHANDLE_*` ioctls, toggles it ~1 Hz for ~5 s, then closes
   (kernel releases the line on close). Run it per candidate line. Toggling for a
   few seconds and releasing is reversible and safe — but **only on a line you have
   verified is NOT in the avoid-list and not muxed to a peripheral**
   (`adb shell sh -c 'echo PXn > /sys/kernel/debug/sunxi_pinctrl/sunxi_pin; cat /sys/kernel/debug/sunxi_pinctrl/function'`
   should read a GPIO function, not e.g. `twi`/`lcd0`/`uart`).

4. **Backlight cross-check (the known light).** To prove the *backlight* is the
   visible element, you can vary `pwmchip0` channel 1 — but the disp driver owns it,
   so the clean test is to watch the screen dim when the system idles, or to read
   `lcd0_backlight` (currently 50). Do **not** unbind the LCD just to test.

5. **Record the winner.** When a candidate line visibly drives an LED, note
   `chip=gpiochip1 line=<N>` and the SoC pin (`<bank>*32+pin`), then set
   `ledblink.c`'s defaults and append the confirmed mapping to this report and to
   CLAUDE.md's hardware inventory.

### Why "read-only first" was the right call

The most output-y, LED-looking lines (PE16/PE17, both driven, one high one low) are
the **camera reset and power-down**. A blind "toggle the obvious output" approach
would have reset the OV2685 mid-operation instead of blinking an LED — exactly the
unsafe outcome the task guarded against.

---

## Open questions / what still needs the physical KidBright unit

- The running DT is the generic **M2Dock** profile. The **KidBright µAI** may add a
  discrete user LED on a free `pio` pin that this BSP DTB simply doesn't declare
  (the LED would then be a "raw pin" with no kernel owner — invisible to
  `/sys/kernel/debug/gpio`). Only the **schematic** + the **on-hardware toggle
  test** (step 2/3 above) can find it. Treat any GPIO LED claim as **needs
  on-hardware confirmation**.
- If the KidBright firmware/CodeBlock exposes an "LED" block, capturing what pin/PWM
  it drives (e.g. strace/ltrace of the vendor stack, or reading the CodeBlock board
  JSON) would resolve this definitively.
