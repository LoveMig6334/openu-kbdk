# Build 4 — Examples templates + GPIO LED-blink

> **Status:** design approved 2026-06-21. This is the spec for **Build 4** of the
> board-IDE roadmap (`docs/superpowers/plans/2026-06-21-board-ide-remaining-features.md`).
> The matching bite-sized TDD plan is
> `docs/superpowers/plans/2026-06-21-build4-examples-templates-plan.md`.
> Build order is 3 → 4: this depends on **Build 3 — Edit tab** (the editor +
> build/deploy/run spine, `docs/superpowers/specs/2026-06-21-edit-tab-design.md`)
> for the buffer that "Load" drops source into. Build 3 was **not yet implemented**
> when this spec was written — see *Dependency on Build 3* below.

## Goal

A library of ready-to-run, heavily-commented single-file board C programs the user
can browse, **Load** into the Edit-tab editor with one click, then Build → Deploy →
Run on the board. A newcomer goes from zero to a blinking LED / live camera / a tone
in one click + Build + Run. Ships the toolkit's **first real GPIO capability**
(`src/ledblink.c`, raw `/dev/gpiochip` chardev ioctl) as one of the templates.

## Settled design decisions

These resolve the "Open design decisions" the roadmap left for Build 4. **All five
were settled before this spec** and are not re-litigated here:

| Decision | Choice | Notes |
| --- | --- | --- |
| Examples placement | **Its own tab** (`Tab::Examples`) | Not a panel inside Edit. A flat list of templates; "Load" hands off to the Edit tab. |
| Starter set | **All five** templates | hello, screen, audio, camera, **LED**. No partial set. |
| LED GPIO path | **Raw `/dev/gpiochipN` GPIO_V2 chardev ioctl** | `linux/gpio.h` UAPI, `GPIO_V2_GET_LINE_IOCTL` + `GPIO_V2_LINE_SET_VALUES_IOCTL`. "Direct ioctl, no vendor lib" philosophy. sysfs is a documented fallback only (see *Kernel-version risk*). |
| Template manifest | **Scan `examples/board/`** | No checked-in JSON/TOML index. The list is built by reading `examples/board/*.{c,cpp}` and parsing a **header-comment metadata block** (defined below) from each file. |
| LED pin source | **`docs/research/2026-06-21-led-gpio-investigation.md`** | `ledblink.c` `#define`s the chip + line, defaulting to that report's **top candidate**, marked `/* HW-CONFIRM */`. The report is the single source of truth for the pin and for which chardev ABI actually works on the 4.9 kernel. |

## Dependency on Build 3 (read this first)

Build 4's UI half is *only* the Examples tab and a one-line hand-off into Build 3's
editor. It needs two things from Build 3 to exist on `KbdkApp`:

1. **An editor buffer to load into.** Build 3's spec puts the open file's text in an
   `EditState` on `KbdkApp` (`open_path: Option<PathBuf>`, `buffer: String`,
   `saved_snapshot: String` for the dirty `*`). "Load" sets `buffer = <file text>`,
   `open_path = Some(<file>)`, `saved_snapshot = buffer` (loaded == clean), and
   `extra_args = <metadata extra_args>` (the Edit-tab "extra build args" field).
2. **`Tab::Edit`** to switch to after loading.

If this build is executed **before** Build 3 is merged, do **not** stub a fake
editor. Either (a) execute Build 3 first (the roadmap order), or (b) implement the
board-C half (`src/ledblink.c` + Makefile + `examples/board/*` templates + the
`examples_scan` parser and its unit tests) now — all of which are independent of the
UI — and land the `examples_tab.rs` + `app.rs` wiring as the final step once
`EditState` exists. The plan is sequenced so the board-C + parser work (TDD-able,
hardware-free) comes first and the UI hand-off comes last, exactly so this ordering
is possible. **A single integration point** is assumed from Build 3 and named
explicitly here so the two builds compose:

```rust
// provided by Build 3 (edit_tab / EditState on KbdkApp). Examples calls it:
pub fn load_into_editor(app: &mut KbdkApp, path: &Path, text: String, extra_args: String) {
    app.edit.open_path = Some(path.to_path_buf());
    app.edit.buffer = text;
    app.edit.saved_snapshot = app.edit.buffer.clone(); // freshly loaded == not dirty
    app.edit.extra_args = extra_args;                   // metadata-driven compile flags
    app.f.tab = Tab::Edit;
}
```

If Build 3 named these fields differently, the Examples tab adapts to the real names;
the **contract** is "set the editor buffer + open_path + extra_args, switch to Edit".

---

## Part 1 — `src/ledblink.c` (the new GPIO board program)

The first real **GPIO** capability in the toolkit (the CLAUDE.md capability table
lists GPIO as "ready, not yet written"). A minimal, heavily-commented blinker that
drives the LED line directly through the GPIO character device — no vendor lib, same
"raw ioctl / mmap" spirit as `fbtest.c` (FBIO) and `audio.c` (SNDRV_PCM_IOCTL).

### Pin selection (from the investigation report)

`ledblink.c` does **not** hunt for the pin at runtime. It `#define`s the chip and
line, defaulting to the **top candidate** from
`docs/research/2026-06-21-led-gpio-investigation.md`, with a glaring marker:

```c
/* LED pin — from docs/research/2026-06-21-led-gpio-investigation.md.
 * Defaulting to the report's top candidate; CONFIRM ON HARDWARE before shipping. */
#define LED_CHIP "/dev/gpiochip0"   /* HW-CONFIRM */
#define LED_LINE 0u                 /* HW-CONFIRM: line offset on LED_CHIP */
#define LED_ACTIVE_LOW 0            /* HW-CONFIRM: 1 if the LED sinks (on = low) */
```

These three `#define`s are the **only** things the hardware investigation must pin
down. They are also overridable from `argv` (`ledblink [chip] [line] [period_ms]
[count]`) so the implementer can sweep candidate lines without recompiling while
confirming the pin — but the compiled-in defaults are what ships. The actual values
(chip path, line offset, active-low) come **verbatim from the report**; this spec does
not invent them. (`gpiochip0` line `0` above is a placeholder so the file compiles —
the implementer replaces it with the report's value.)

### ABI: GPIO_V2 chardev, with a hard kernel-version caveat

The settled path is the **GPIO_V2** chardev ioctl. The exact ABI (confirmed present
in the cross sysroot's `linux/gpio.h`, GPIO_MAX_NAME_SIZE 32, GPIO_V2_LINES_MAX 64):

- `open("/dev/gpiochipN", O_RDWR | O_CLOEXEC)` → the **chip** fd.
- Fill `struct gpio_v2_line_request`:
  `offsets[0] = LED_LINE; num_lines = 1; config.flags = GPIO_V2_LINE_FLAG_OUTPUT`
  (`| GPIO_V2_LINE_FLAG_ACTIVE_LOW` if `LED_ACTIVE_LOW`); copy a `consumer[]` label
  ("kbdk-ledblink").
- `ioctl(chipfd, GPIO_V2_GET_LINE_IOCTL, &req)` → on success `req.fd` is a **line**
  fd dedicated to that one line.
- Per toggle: fill `struct gpio_v2_line_values { mask = 1; bits = level ? 1 : 0; }`
  and `ioctl(linefd, GPIO_V2_LINE_SET_VALUES_IOCTL, &vals)`.
- Sleep `period_ms/2` between edges (`nanosleep`; avoid `usleep`/`select` to dodge
  the musl time64 redirect — `nanosleep` is safe), flip level, loop `count` times
  (`count<=0` = forever until SIGTERM/SIGINT → clean line release + `_exit`).
- Close the line fd then the chip fd on exit (releasing the line returns the pin to
  its default).

> **⚠ Kernel-version risk (must be resolved by the investigation report).** The
> board runs **Linux 4.9.118** (CLAUDE.md hardware table). The GPIO character device
> **v2** uABI (`gpio_v2_*`, `GPIO_V2_*_IOCTL`) was introduced in **Linux 5.10**. On a
> 4.9 kernel these ioctls are very likely **ENOTTY/EINVAL**, and only the **v1**
> chardev (`GPIO_GET_LINEHANDLE_IOCTL` / `GPIOHANDLE_SET_LINE_VALUES_IOCTL`, present
> since 4.8) or **sysfs** (`/sys/class/gpio` export, present in this kernel per the
> roadmap) will actually toggle a pin. The investigation report
> (`docs/research/2026-06-21-led-gpio-investigation.md`) **must** state which chardev
> ABI version the board's `/dev/gpiochipN` accepts. The settled decision is GPIO_V2,
> so `ledblink.c` is written V2-first; **if the report finds the 4.9 kernel rejects
> V2**, the implementer adds a v1-chardev path under the same `#define`s (the structs
> differ but the flow is identical: request a line handle, then set values), keeping
> the program a single self-contained file. The header-comment `desc` notes which ABI
> the shipped binary uses. **Do not claim the LED blinks until the report + an
> on-hardware run confirm both the pin and the ABI** — this is the one genuinely
> hardware-gated piece of Build 4.

The cross sysroot's `linux/gpio.h` is the only include needed beyond libc
(`fcntl.h`, `unistd.h`, `string.h`, `time.h`, `signal.h`, `errno.h`). No `-lm`, no
MPP, no dlopen.

### Makefile targets

Mirror the `hello` rule exactly (same `$(CROSS) $(CROSSFLAGS)` — hard-float ABI is
non-negotiable; no extra libs):

```make
ledblink: bin/ledblink
bin/ledblink: src/ledblink.c | bin
	$(CROSS) $(CROSSFLAGS) -o $@ $<

deploy-ledblink: ledblink
	$(UAI) push bin/ledblink /tmp/ledblink && $(UAI) exec "chmod +x /tmp/ledblink"
	@echo 'run: ./bin/uai exec "/tmp/ledblink"'
```

Add `ledblink deploy-ledblink` to the `.PHONY` list and `bin/ledblink` to `clean`.

---

## Part 2 — `examples/board/*` curated templates

Five **single-file**, self-contained, heavily-commented templates living in
`examples/board/`. They are *derived from* the existing probe sources but are
**teaching versions**: shorter, one clear thing each, no diagnostic/sweep modes, with
narration comments aimed at a newcomer. They are real source the Edit tab compiles
unchanged via Build 3's single-file build (CROSSFLAGS + the metadata `extra_args`).

| File | Derived from | What it does (minimal) | extra_args |
| --- | --- | --- | --- |
| `examples/board/hello.c` | `src/hello.c` | printf + `sqrt(2)` hard-float smoke test. | `-lm` |
| `examples/board/screen.c` | `src/fbtest.c` | mmap `/dev/fb0`, paint colour bars + border (orientation check). | *(none)* |
| `examples/board/audio_tone.c` | `src/audio.c` | raw SNDRV_PCM_IOCTL: play a 440 Hz sine for 2 s. | `-lm` |
| `examples/board/camera.c` | `src/camcc.c` / `cammpp.c` | dlopen MPP, capture NV21, colour-correct, blit to `/dev/fb0` until SIGTERM. | `-DAWCHIP=0x1817 -Ivendor/eyesee-mpp/sun8iw19p1/include -Ivendor/eyesee-mpp/sun8iw19p1/include/media -Ivendor/eyesee-mpp/sun8iw19p1/include/utils -ldl -lpthread` |
| `examples/board/ledblink.c` | `src/ledblink.c` | GPIO_V2 chardev blink (the teaching copy of the new program). | *(none)* |

Curation rules (each template):
- **One file, one concept.** Strip multi-mode CLIs, watchdogs, dump paths, sweep
  loops — keep the happy path a newcomer reads top-to-bottom.
- **Heavy comments**: what each ioctl/step does and *why*, the board facts that bite
  (musl-1.1.16 `dlsym` shim for camera, hard-float ABI, fb byte order from bitfields).
- **Defaults that just work**: no required args; sensible built-in WxH/freq/period.
- **Self-contained**: compiles from `examples/board/<f>` + the metadata `extra_args`
  alone (the camera's `-I` paths are relative to the repo root, which is the Edit
  tab's compile cwd).

> **`ledblink.c` exists in two places by design**: `src/ledblink.c` is the
> production program with the Makefile target (the GPIO *capability*);
> `examples/board/ledblink.c` is the teaching template the Examples tab lists. Keep
> them in sync (the template is the production file plus the metadata header + lighter
> comments). The plan generates the template from the production file in the same
> task to avoid drift.

### The header-comment metadata convention (load-bearing — match exactly)

Every file in `examples/board/` carries its metadata in the **first comment of the
file**, before any code. The scanner reads only this block; the rest of the file is
opaque source. The convention is **one `key: value` per line inside the opening
`/* … */` block comment**, with these recognized keys:

```
/* kbdk-example
 * name: <short display name>
 * desc: <one-line description shown in the list>
 * extra_args: <space-separated extra compile flags, or empty>
 */
```

**Exact rules (the parser and every template must obey these):**

1. The metadata block is the **opening C block comment** (`/* … */`) that begins on
   the file's first non-blank line. Only this first block comment is scanned.
2. Its **first content line is the literal marker `kbdk-example`** (a sentinel so a
   plain doc comment is never mistaken for metadata). A file whose first block
   comment does not start with `kbdk-example` is **skipped** (not listed).
3. Each subsequent line is `key: value`. Leading whitespace and a leading
   `*` + optional space (the comment's left margin) are stripped before parsing.
   The key is everything up to the **first** `:`; the value is the rest, trimmed.
4. Recognized keys: **`name`**, **`desc`**, **`extra_args`**. Unknown keys are
   ignored (forward-compatible). Keys are case-sensitive, lowercase.
5. `name` and `desc` are **required**; a file missing either is skipped (and the
   scanner logs it to stderr). `extra_args` is **optional** — absent or empty means
   no extra flags (just CROSSFLAGS).
6. `extra_args` is the **verbatim** string the Edit tab appends to the compile (it is
   the Edit tab's "extra build args" field, prefilled). It is split on whitespace by
   the compiler invocation, not by the parser — the parser stores it as one string.
   (This is why the camera template carries its MPP `-I`/`-D` + `-ldl -lpthread`
   here: the single-file Edit build has no Makefile to supply them.)
7. The block ends at the first `*/`. Lines after it are ignored.

**Canonical example header** (this is exactly what `examples/board/camera.c` starts
with — copy this shape):

```c
/* kbdk-example
 * name: Camera preview
 * desc: Live colour-corrected camera on the LCD (MPP capture -> fb0)
 * extra_args: -DAWCHIP=0x1817 -Ivendor/eyesee-mpp/sun8iw19p1/include -Ivendor/eyesee-mpp/sun8iw19p1/include/media -Ivendor/eyesee-mpp/sun8iw19p1/include/utils -ldl -lpthread
 */
#define _GNU_SOURCE
#include <stdio.h>
/* …rest of the camera template… */
```

`hello.c`'s header (minimal, with one flag):

```c
/* kbdk-example
 * name: Hello (hard-float)
 * desc: printf + sqrt smoke test — proves the FPU / hard-float ABI works
 * extra_args: -lm
 */
```

`screen.c` / `ledblink.c` carry an **empty** `extra_args` (omit the line, or
`extra_args:` with nothing after it — both mean "no extra flags").

---

## Part 3 — `examples/board` scanner (`examples_scan` — the TDD core)

A pure parser so the metadata convention is unit-tested with **zero filesystem and
zero hardware**. Lives where the Examples tab can call it; the recommended home is a
small module function in `crates/kbdk-ui/src/examples_tab.rs` (the project keeps
hardware-free parsers next to their tab, e.g. `workers::parse_kbstat`), or, if it is
reused, `kbdk-core`. v1: keep it in `examples_tab.rs`.

```rust
pub struct Example {
    pub name: String,
    pub desc: String,
    pub extra_args: String, // "" if none
    pub path: PathBuf,      // examples/board/<file>
}

/// Pure: parse one file's header-comment metadata block. None if the file's
/// first block comment is not a `kbdk-example` block, or name/desc are missing.
pub fn parse_metadata(source: &str) -> Option<ParsedMeta>;   // {name, desc, extra_args}

/// Impure: read `dir` (examples/board), parse each *.c / *.cpp, return the
/// Examples sorted by name. Files without valid metadata are skipped (logged).
pub fn scan_examples(dir: &Path) -> Vec<Example>;
```

`parse_metadata` is the unit-tested function (see Testing). `scan_examples` is the
thin filesystem wrapper that calls it per file (reads the whole file is fine — these
are small) and is exercised manually + by the on-board screenshot.

---

## Part 4 — `crates/kbdk-ui/src/examples_tab.rs` (the UI)

`pub fn show(app: &mut KbdkApp, ui: &mut egui::Ui)` like every other tab. **No board
I/O** in this tab (pure local file reads); a one-shot scan is cheap and synchronous —
no worker thread, no `Msg` variants needed. (This is the one tab that legitimately
needs neither `workers.rs` nor `pump()` changes.)

Layout:
- A heading "Examples" + a short subtitle ("Load a template into the editor, then
  Build → Deploy → Run").
- A `⟳` refresh button that re-scans (clears the cached list).
- A vertical `ScrollArea` of cards, one per `Example`: **name** (strong) + **desc**
  (subtext) + a `Load →` button. Optionally show the `extra_args` as dim monospace so
  the user sees what flags will be set.
- Clicking **Load →** reads the file fresh from disk (so an edited-on-disk template
  is current), calls the Build-3 hand-off (`load_into_editor`, see *Dependency*), and
  switches to `Tab::Edit`.

State on `KbdkApp` (one field; lazily filled on first show):

```rust
pub examples: Vec<examples_tab::Example>,   // scanned list; empty == not yet scanned
```

On `show()`: if `app.examples.is_empty()`, scan `repo_root.join("examples/board")`
once and store it (mirrors the Hardware tab's "fetch on first open"). The `⟳` button
clears it to force a re-scan. The scan reads at most ~5 small files — synchronous on
the UI thread is acceptable and matches the project's local-file idiom
(`files_tab::read_local_dir` reads synchronously too).

Load handler:

```rust
if ui.button("Load →").clicked() {
    match std::fs::read_to_string(&ex.path) {
        Ok(text) => edit_tab::load_into_editor(app, &ex.path, text, ex.extra_args.clone()),
        Err(e)   => app.examples_status = format!("load failed: {e}"),
    }
}
```

(`examples_status: String` is an optional second `KbdkApp` field for the error line;
keep it if the empty-state/error message is wanted, else fold into an inline label.)

---

## Part 5 — `crates/kbdk-ui/src/app.rs` + `main.rs` wiring

Standard tab-add (the project's documented pattern):

- `main.rs`: `mod examples_tab;`.
- `app.rs`:
  - `Tab` enum: add `Examples` (after `Hardware`).
  - `top_bar()`: `ui.selectable_value(&mut self.f.tab, Tab::Examples, "Examples");`.
  - `ui()` match: `Tab::Examples => examples_tab::show(self, ui),`.
  - `--tab` startup hook: accept `"examples" => Tab::Examples` (and, while here, fix
    the hook so `files`/`tasks`/`hardware`/`edit` map too — Build 3/5 also need this;
    the current match only handles convert/deploy/train).
  - `KbdkApp`: add `pub examples: Vec<examples_tab::Example>` (init `vec![]`) and
    optionally `pub examples_status: String` (init `String::new()`).

No `Fields` (persistence) change is required — the scanned list is derived from disk,
not user input. (`Tab::Examples` persists automatically via the existing `f.tab`.)

---

## Data flow

```
open Examples tab
  → if app.examples empty: scan_examples(repo_root/examples/board) -> Vec<Example>
list renders one card per Example (name, desc, [extra_args])
click "Load →"
  → std::fs::read_to_string(ex.path)
  → edit_tab::load_into_editor(app, ex.path, text, ex.extra_args)   [Build 3]
      sets EditState.buffer / open_path / saved_snapshot / extra_args
  → app.f.tab = Tab::Edit
(user is now in the Edit tab with the template loaded)
  → Build (CROSSFLAGS + extra_args) → Deploy + Run   [all Build 3]
```

## Error handling

- **`examples/board` missing/empty** → `scan_examples` returns `vec![]`; the tab
  shows an empty-state hint ("no templates found in examples/board"). No panic.
- **A template with malformed/absent metadata** → skipped by the scanner, logged to
  stderr; the rest still list.
- **Load read error** (file deleted between scan and click) → status line, editor
  unchanged.
- **Camera/audio template won't link in the Edit build** → that is a Build-3 compile
  error surfaced in the Edit-tab output panel; the metadata `extra_args` is exactly
  what prevents it (carries the MPP `-I`/`-D`/`-ldl -lpthread`). If a user clears the
  extra-args field, the build fails loudly in the output panel — acceptable.
- **LED program on a board that rejects GPIO_V2** → runtime ENOTTY printed by
  `ledblink.c`; the fix is the v1/sysfs fallback the investigation report dictates
  (see *Kernel-version risk*), not a UI concern.

## Testing & verification

- **Unit tests (hardware-free, the TDD core) — `parse_metadata`:**
  - a valid `kbdk-example` header with `name`/`desc`/`extra_args` → `Some` with the
    three fields trimmed correctly;
  - the comment left-margin `*` and indentation are stripped;
  - missing `extra_args` → `Some` with `extra_args == ""`;
  - a block comment **not** starting with `kbdk-example` → `None`;
  - missing `name` or `desc` → `None`;
  - unknown keys ignored;
  - a value containing `:` (e.g. a URL or a flag) keeps everything after the **first**
    `:` (split-once semantics);
  - real fixtures: feed the actual header strings of all five shipped templates and
    assert the expected `{name, desc, extra_args}` (this is the drift guard — if a
    template's header is edited to break the convention, this test fails).
- **Board C — `src/ledblink.c`:** cross-compiles clean via `make ledblink`
  (`arm-unknown-linux-musleabihf-gcc $(CROSSFLAGS)`); `nm -D -u bin/ledblink` shows no
  `*_time64` leaks (the musl-1.1.16 trap check from CLAUDE.md). **Hardware run is
  gated on the investigation report** (pin + ABI) and is the human's confirmation
  step — do not assert "it blinks" without it.
- **Templates compile:** each `examples/board/*` cross-compiles with
  `CROSSFLAGS + <its extra_args>` (a quick CI-style loop in the plan; the camera one
  needs the vendored MPP headers present, which they are).
- **On-board screenshot gate (mandatory):** launch with `--screenshot PATH
  --tab examples`; confirm the five cards render with names/descriptions and a
  `Load →` button each. (Builds 1–2 both shipped layout bugs only the live screenshot
  caught — non-negotiable.) A second screenshot after a programmatic Load (or a manual
  human click) confirms the Edit tab opens with the template text — but the Load
  hand-off depends on Build 3, so this second shot is part of the integrated check.

## Non-goals (v1) and fast-follows

**Non-goals:** template categories/search/tags; user-authored templates discovered
outside `examples/board/`; a parameterised template wizard; per-template README/run
instructions in-app (the in-source comments are the docs); GPIO *input* (button)
examples (this build ships output/blink only — input is a fast-follow once the chardev
ABI is settled by the report); board-side daemonization of the camera template (Run's
keep-session-open model from Build 3 handles long-running programs).

**Fast-follows:** a GPIO **button/input** template (read a line, same chardev);
syntax-highlighted preview in the card; a "Run all five as a smoke test" action;
hoisting `parse_metadata`/`scan_examples` into `kbdk-core` if another consumer appears.

## Dependencies

None new. The cross toolchain (`arm-unknown-linux-musleabihf-gcc`, `CROSSFLAGS`) and
the vendored MPP headers (`vendor/eyesee-mpp/sun8iw19p1/include`) already exist. The
GPIO_V2 uABI (`linux/gpio.h`) is present in the cross sysroot (verified). The only
hard dependency is **Build 3** (the Edit tab / `EditState` / `Tab::Edit`) for the
Load hand-off — see *Dependency on Build 3*.

## Files

**Create**
- `src/ledblink.c` — GPIO_V2 chardev blinker (the new capability).
- `examples/board/hello.c`, `examples/board/screen.c`,
  `examples/board/audio_tone.c`, `examples/board/camera.c`,
  `examples/board/ledblink.c` — the five curated templates with metadata headers.
- `crates/kbdk-ui/src/examples_tab.rs` — `Example`, `parse_metadata`,
  `scan_examples`, `show`, unit tests.

**Modify**
- `Makefile` — `ledblink` + `deploy-ledblink` targets; `.PHONY`; `clean`.
- `crates/kbdk-ui/src/main.rs` — `mod examples_tab;`.
- `crates/kbdk-ui/src/app.rs` — `Tab::Examples`; `top_bar` selectable; `ui()` arm;
  `--tab examples` (+ the other tab names); `examples` (and optional
  `examples_status`) field on `KbdkApp`.
- `CLAUDE.md` — flip the GPIO capability row to "✅ done — `src/ledblink.c`
  (raw `/dev/gpiochip` GPIO_V2 chardev)"; add an Examples-tab note to the kbdk-ui
  paragraph; reference the LED investigation report.
- `docs/research/2026-06-21-led-gpio-investigation.md` — the prerequisite report
  (pin + chardev ABI). **Authored as part of this build** (it did not exist when this
  spec was written); `ledblink.c`'s `#define`s come from it.
