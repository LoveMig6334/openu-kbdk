# Build 4 — Examples templates + GPIO LED-blink — implementation plan

> Bite-sized, TDD-first plan for **Build 4**. Spec:
> `docs/superpowers/specs/2026-06-21-examples-templates-design.md`. Roadmap:
> `docs/superpowers/plans/2026-06-21-board-ide-remaining-features.md` (Build 4).
> Execute with `superpowers:executing-plans` (or subagent-driven). Each task is
> independently verifiable; tasks are ordered so the **hardware-free** work
> (board C, the parser + its tests, the templates) lands first and the **UI hand-off
> that depends on Build 3** lands last (see Task 8's dependency gate).

## Conventions for every task

- **TDD where there is a pure function.** Red (failing test) → green (impl) → refactor.
  The only pure function in this build is `parse_metadata` (Task 5) — that task is
  strictly test-first.
- **Verification is evidence, not assertion** (`superpowers:verification-before-completion`):
  paste the actual command output. UI tasks end with a `--screenshot` render.
- **Cross-compile flags are verbatim** from the root `Makefile`:
  `CROSS = arm-unknown-linux-musleabihf-gcc`,
  `CROSSFLAGS = -O2 -mcpu=cortex-a7 -mfpu=neon-vfpv4 -mfloat-abi=hard`. The wrong
  float ABI silently faults on the board — never alter these.
- **Don't claim the LED blinks** until Task 1's report + an on-hardware run (the
  human's step) confirm the pin and the chardev ABI. This is the one genuinely
  hardware-gated piece.
- Workspace gate after Rust tasks: `cargo build` and `cargo test -p kbdk-ui`
  (the parser tests live in `examples_tab.rs`) must pass.

---

## Task 1 — LED GPIO investigation report (PREREQUISITE, hardware)

**Goal:** produce `docs/research/2026-06-21-led-gpio-investigation.md` that pins down
the **three facts** `ledblink.c` `#define`s: the gpiochip path, the line offset, and
active-low polarity — **and** which chardev ABI the board's 4.9.118 kernel accepts.

**Why first:** every later LED step references this file; the spec deliberately does
not invent the pin.

**Steps (on the board, via `./bin/uai exec` or adb):**
1. Enumerate chips: `ls -l /dev/gpiochip*`; for each,
   `cat /sys/bus/gpio/devices/gpiochip*/dev` and any `label`/`ngpio` under
   `/sys/class/gpio/`. Record chip→base/ngpio.
2. Source of truth for the LED pin: board schematic / device-tree. Dump the DT
   (`/proc/device-tree`, `tr -d '\0'`) and grep for `led`, `gpio-leds`, `pwm` (the
   board also has `pwmchip0` for backlight/buzzer per CLAUDE.md — confirm the LED is a
   plain GPIO, not a PWM/backlight line, and not `/sys/class/leds`, which the roadmap
   says is absent).
3. **ABI probe — the critical question.** Determine whether `/dev/gpiochipN` accepts
   **GPIO_V2** ioctls on this **4.9.118** kernel (GPIO_V2 landed in Linux 5.10, so
   expect it may be **rejected**). Either run a tiny throwaway probe, or reason from
   the kernel version + `ioctl` error. Record: does GPIO_V2 work? does v1
   (`GPIO_GET_LINEHANDLE_IOCTL`, ≥4.8) work? does sysfs export work?
4. Trial-toggle the top candidate line(s) and watch the physical LED to confirm pin +
   polarity (on = high or low).

**Report must state, unambiguously:**
- `LED_CHIP` (e.g. `/dev/gpiochip0`), `LED_LINE` (offset), `LED_ACTIVE_LOW` (0/1),
  each with how it was confirmed.
- **Which chardev ABI version to use** (V2 if the kernel accepts it; else V1; else
  sysfs) — this decides whether Task 2 ships the V2 path alone or adds a fallback.
- Any runner-up candidates (so a future board revision has a trail).

**Verify:** the report exists, names the three `#define` values, and answers the
GPIO_V2-vs-v1-vs-sysfs question with evidence (command output pasted).

> If hardware is unavailable to the agent, the report is the **human's** step; the
> agent may draft the report skeleton with the enumeration commands and leave the
> three values + ABI verdict marked `TODO(hardware)`. Tasks 2/3 then ship V2-first
> with the placeholder `#define`s and the v1/sysfs fallback scaffolded but the final
> values filled in by the human. Do not merge claiming the LED works until filled.

---

## Task 2 — `src/ledblink.c` (the new GPIO program)

**Depends on:** Task 1 (the `#define` values + ABI verdict). Self-contained C; no UI.

**Goal:** a minimal, heavily-commented blinker driving the LED line through the GPIO
**character device** (settled: GPIO_V2 chardev ioctl), pin from Task 1's report.

**Write `src/ledblink.c`:**
- Header comment: what it does, the `#define`s' provenance (cite the report),
  the build line, and the kernel-version caveat (GPIO_V2 needs ≥5.10; v1/sysfs
  fallback if the 4.9 board rejects it).
- The three `#define`s (`LED_CHIP`, `LED_LINE`, `LED_ACTIVE_LOW`), each marked
  `/* HW-CONFIRM */`, set to Task 1's values (or placeholders + TODO if hardware
  pending). `argv` overrides: `ledblink [chip] [line] [period_ms] [count]`.
- GPIO_V2 flow (exact ABI from `linux/gpio.h`, confirmed in the cross sysroot):
  `open(LED_CHIP, O_RDWR|O_CLOEXEC)` → fill `struct gpio_v2_line_request`
  (`offsets[0]=line; num_lines=1; config.flags = GPIO_V2_LINE_FLAG_OUTPUT`
  [`| ..._ACTIVE_LOW` if set]; `consumer="kbdk-ledblink"`) →
  `ioctl(chipfd, GPIO_V2_GET_LINE_IOCTL, &req)` → toggle loop with
  `struct gpio_v2_line_values{mask=1;bits=level}` +
  `ioctl(linefd, GPIO_V2_LINE_SET_VALUES_IOCTL, &vals)` →
  `nanosleep(period_ms/2)` (NOT usleep/select — musl time64) → flip → repeat `count`
  times (`<=0` = forever). SIGTERM/SIGINT → release line + `_exit(0)`.
- **If Task 1 found 4.9 rejects GPIO_V2:** add a v1 path under the same `#define`s
  (`GPIO_GET_LINEHANDLE_IOCTL` → `gpiohandle_request{flags=GPIOHANDLE_REQUEST_OUTPUT}`
  → `GPIOHANDLE_SET_LINE_VALUES_IOCTL`), selected by the ABI the chip accepts (try V2,
  fall back to v1 on ENOTTY/EINVAL). Keep it one file. Clear stderr on failure.

**Makefile (mirror `hello` exactly):**
```make
ledblink: bin/ledblink
bin/ledblink: src/ledblink.c | bin
	$(CROSS) $(CROSSFLAGS) -o $@ $<

deploy-ledblink: ledblink
	$(UAI) push bin/ledblink /tmp/ledblink && $(UAI) exec "chmod +x /tmp/ledblink"
	@echo 'run: ./bin/uai exec "/tmp/ledblink"'
```
Add `ledblink deploy-ledblink` to `.PHONY`; add `bin/ledblink` to the `clean` list.

**Verify:**
- `make ledblink` → compiles clean (paste output).
- `nm -D -u bin/ledblink` → **no `*_time64`** symbols (musl-1.1.16 trap check).
- File is `file bin/ledblink` ARM, hard-float EABI5.
- **Hardware (human / Task 1 confirmed):** `make deploy-ledblink` then
  `./bin/uai exec "/tmp/ledblink 0 0 500 6"` blinks the LED 6× at 2 Hz. **Do not
  mark done on "it blinks" without the report + a real run.**

---

## Task 3 — Five curated templates in `examples/board/`

**Depends on:** Task 2 for `ledblink.c`'s body; independent otherwise. No UI.

**Goal:** five single-file, heavily-commented teaching templates, each starting with
the **header-comment metadata block** (the exact convention is in the spec; reproduced
in Task 5). Derive each from the named probe source but trim to one clear concept.

Create, each with its metadata header (`name`/`desc`/`extra_args`) then the code:
- `examples/board/hello.c` — from `src/hello.c`. `extra_args: -lm`.
- `examples/board/screen.c` — from `src/fbtest.c`, colour-bars + border only (drop the
  `argv` fb path / smem math narration to the essentials). `extra_args:` (none).
- `examples/board/audio_tone.c` — from `src/audio.c`, **tone-only** (the `hw_any` /
  mask-interval ABI + a 440 Hz 2 s sine; drop probe/play/rec/WAV). `extra_args: -lm`.
- `examples/board/camera.c` — from `src/camcc.c` (or `cammpp.c`): dlopen MPP, capture
  NV21, colour-correct, blit to fb0 until SIGTERM. Keep the musl-`dlsym` shim comment.
  `extra_args: -DAWCHIP=0x1817 -Ivendor/eyesee-mpp/sun8iw19p1/include -Ivendor/eyesee-mpp/sun8iw19p1/include/media -Ivendor/eyesee-mpp/sun8iw19p1/include/utils -ldl -lpthread`.
- `examples/board/ledblink.c` — `src/ledblink.c` + the metadata header, lighter
  comments. `extra_args:` (none). **Generate from `src/ledblink.c` in this task so
  the two never drift** (note the provenance in a comment).

Curation rules (spec Part 2): one file/one concept, defaults that need no args, heavy
narration comments, the board facts that bite. `name`/`desc` must be honest and short.

**Verify — every template cross-compiles with `CROSSFLAGS + its extra_args`:**
```sh
CROSS="arm-unknown-linux-musleabihf-gcc"
FLAGS="-O2 -mcpu=cortex-a7 -mfpu=neon-vfpv4 -mfloat-abi=hard"
$CROSS $FLAGS -o /tmp/ex_hello   examples/board/hello.c      -lm
$CROSS $FLAGS -o /tmp/ex_screen  examples/board/screen.c
$CROSS $FLAGS -o /tmp/ex_audio   examples/board/audio_tone.c -lm
$CROSS $FLAGS -DAWCHIP=0x1817 -Ivendor/eyesee-mpp/sun8iw19p1/include \
  -Ivendor/eyesee-mpp/sun8iw19p1/include/media \
  -Ivendor/eyesee-mpp/sun8iw19p1/include/utils \
  -o /tmp/ex_camera examples/board/camera.c -ldl -lpthread
$CROSS $FLAGS -o /tmp/ex_led     examples/board/ledblink.c
```
Paste: all five compile with rc 0. (The flag list per file must equal that file's
`extra_args` — this is the live proof the metadata is correct.)

---

## Task 4 — Empty Examples tab wired into the shell

**Depends on:** nothing (the wiring compiles before the scanner/UI exist). No board I/O.

**Goal:** `Tab::Examples` exists, is selectable, routes to a placeholder `show`, and
the `--tab examples` hook works — so the scaffold is verified before content lands.

**Steps:**
- `crates/kbdk-ui/src/examples_tab.rs`: add
  `pub fn show(app: &mut KbdkApp, ui: &mut egui::Ui) { ui.heading("Examples"); }`
  (placeholder) for now.
- `crates/kbdk-ui/src/main.rs`: `mod examples_tab;`.
- `crates/kbdk-ui/src/app.rs`:
  - `Tab` enum: add `Examples` after `Hardware`.
  - `top_bar()`: `ui.selectable_value(&mut self.f.tab, Tab::Examples, "Examples");`
    after the Hardware one.
  - `ui()` match: `Tab::Examples => examples_tab::show(self, ui),`.
  - `--tab` startup match: extend to all current tab names —
    `"convert"=>Convert, "deploy"=>Deploy, "files"=>Files, "tasks"=>Tasks,
    "hardware"=>Hardware, "examples"=>Examples, _=>Train` (the current match only
    handles convert/deploy; Build 3/5 need the others too).
  - `import` line: add `examples_tab` to the `use crate::{…}` list.

**Verify:**
- `cargo build` clean.
- `cargo run -p kbdk-ui -- --tab examples --screenshot /tmp/ex_empty.png` → Read the
  PNG: the "Examples" tab is selected and shows the heading.

---

## Task 5 — `parse_metadata` (the TDD core — test first)

**Depends on:** Task 4 (the module exists). Pure function; **strictly test-first**.

**Goal:** parse a template's header-comment metadata block into
`ParsedMeta {name, desc, extra_args}`. This is the only pure function in the build —
write the failing tests first.

### The exact convention (the impl MUST match the spec — reproduced verbatim)

- Metadata = the **opening `/* … */` block comment** that begins on the file's first
  non-blank line. Only this first block comment is scanned.
- Its **first content line is the literal marker `kbdk-example`**; if not, return
  `None` (a plain doc comment is never mistaken for metadata).
- Each subsequent line is `key: value`. Strip leading whitespace and a leading `*`
  + optional space (the comment margin) first. Key = up to the **first** `:`; value =
  the rest, trimmed.
- Recognized keys: `name`, `desc`, `extra_args` (lowercase, case-sensitive). Unknown
  keys ignored.
- `name` and `desc` **required** (missing → `None`). `extra_args` optional (absent or
  empty → `""`).
- Value containing `:` keeps everything after the **first** `:` (split-once).
- Block ends at the first `*/`; later lines ignored.

### Step 5a — write tests (red)

In `examples_tab.rs` `#[cfg(test)] mod tests`:
- `valid_full`: a header with all three keys → `Some` with trimmed fields, including
  a multi-flag `extra_args` string preserved verbatim.
- `margin_stripped`: ` * name: X` (leading `*` + spaces) parses to `name == "X"`.
- `no_extra_args`: header with only `name`/`desc` → `extra_args == ""`.
- `not_a_kbdk_block`: first block comment whose first line is `something else` → `None`.
- `missing_name` / `missing_desc` → `None`.
- `unknown_key_ignored`: an `author: …` line is ignored, others still parse.
- `colon_in_value`: `desc: see http://x:8080` keeps `see http://x:8080`.
- `fixtures`: include the literal header strings of all five shipped templates
  (Task 3) and assert the expected `{name, desc, extra_args}` — the **drift guard**.

Run: `cargo test -p kbdk-ui parse_metadata` → fails to compile / red. Paste.

### Step 5b — implement (green)

Add to `examples_tab.rs`:
```rust
pub struct ParsedMeta { pub name: String, pub desc: String, pub extra_args: String }
pub fn parse_metadata(source: &str) -> Option<ParsedMeta> { /* per the convention */ }
```
Implementation sketch: find the first `/*`, take lines until the first `*/`; for each,
strip whitespace + a leading `*`+space; first content line must equal `kbdk-example`
else `None`; then `split_once(':')` per line into a key/value map; require `name` +
`desc`; `extra_args` defaults to `""`.

**Verify:** `cargo test -p kbdk-ui parse_metadata` → all green (paste). `cargo build`.

---

## Task 6 — `scan_examples` + `Example` (filesystem wrapper)

**Depends on:** Task 5. Thin impure wrapper; exercised manually (no new unit tests
beyond a smoke read of the real dir).

**Goal:** read `examples/board`, parse each `*.c`/`*.cpp`, return `Vec<Example>`
sorted by name; skip + log files without valid metadata.

```rust
pub struct Example { pub name: String, pub desc: String, pub extra_args: String, pub path: PathBuf }
pub fn scan_examples(dir: &Path) -> Vec<Example>;
```
Read each file with `std::fs::read_to_string`, run `parse_metadata`, build `Example`
with `path`. Sort by `name`. `eprintln!` skipped files.

**Verify:** a throwaway `#[test]` (or `cargo run` print) over the real
`examples/board` returns exactly **5** examples with the expected names; paste the
list. (Keep or delete the smoke test — the `fixtures` test in Task 5 is the real
guard.)

---

## Task 7 — Examples tab UI (list + cards)

**Depends on:** Tasks 5–6. **No board I/O**; synchronous local scan, UI-thread only
(matches `files_tab::read_local_dir`). No `workers.rs` / `pump()` changes.

**Goal:** the real `show()`: scan-on-first-open, a scroll of cards (name + desc +
optional dim `extra_args`), each with a `Load →` button, a `⟳` refresh, and an
empty-state hint.

**Steps:**
- `KbdkApp` (app.rs): add `pub examples: Vec<examples_tab::Example>` (init `vec![]`)
  and optional `pub examples_status: String` (init `String::new()`).
- `examples_tab::show`:
  - heading "Examples" + subtitle; `⟳` clears `app.examples`.
  - if `app.examples.is_empty()`: `app.examples =
    scan_examples(&app.workers.repo_root.join("examples/board"))`.
  - empty after scan → hint label "no templates found in examples/board".
  - `ScrollArea::vertical` → per `Example`: a card (name strong, desc subtext,
    optional `extra_args` dim monospace) + `Load →` button.
  - **Load handler** — defer the actual editor hand-off to Task 8 (Build 3 dep). For
    now, the button may set `examples_status = format!("would load {}", ex.name)` so
    the UI is fully testable without Build 3. (Task 8 replaces this line with the real
    `load_into_editor` call.)

**Verify:**
- `cargo build` + `cargo test -p kbdk-ui` clean.
- `cargo run -p kbdk-ui -- --tab examples --screenshot /tmp/ex_list.png` → Read it:
  five cards with the five template names + descriptions + a `Load →` button each.

---

## Task 8 — Load hand-off into the Edit tab (Build 3 integration — GATED)

**Depends on:** **Build 3 merged** (`edit_tab` + `EditState` on `KbdkApp` +
`Tab::Edit`). This is the only task that cannot land without Build 3.

**Gate:** confirm Build 3 provides the editor buffer fields. The spec assumes a
helper; if Build 3 didn't add one, add it to `edit_tab.rs`:
```rust
pub fn load_into_editor(app: &mut KbdkApp, path: &Path, text: String, extra_args: String) {
    app.edit.open_path = Some(path.to_path_buf());
    app.edit.buffer = text;
    app.edit.saved_snapshot = app.edit.buffer.clone(); // loaded == clean
    app.edit.extra_args = extra_args;
    app.f.tab = Tab::Edit;
}
```
(Use the real `EditState` field names from Build 3 if they differ; the contract is
"set buffer + open_path + extra_args, switch to Edit".)

**Steps:**
- Replace Task 7's placeholder Load handler with:
  ```rust
  if ui.button("Load →").clicked() {
      match std::fs::read_to_string(&ex.path) {
          Ok(text) => edit_tab::load_into_editor(app, &ex.path, text, ex.extra_args.clone()),
          Err(e)   => app.examples_status = format!("load failed: {e}"),
      }
  }
  ```

**Verify:**
- `cargo build` + `cargo test -p kbdk-ui` clean.
- **Integrated screenshot:** launch `--tab examples`, programmatically (or human-)
  click a card's Load, confirm the Edit tab opens with that template's source in the
  buffer and the right `extra_args` prefilled. Then in Edit: Build the camera template
  (proves the metadata `extra_args` flow) — clean compile in the output panel.

---

## Task 9 — Docs + on-board verification + close-out

**Depends on:** Tasks 1–8.

**Steps:**
- **CLAUDE.md:**
  - Capability table: flip the **GPIO** row to
    `✅ done | src/ledblink.c — raw /dev/gpiochip GPIO_V2 chardev ioctl (pin from
    docs/research/2026-06-21-led-gpio-investigation.md)` (note the v1/sysfs fallback if
    Task 1 required it).
  - kbdk-ui paragraph: add the **Examples** tab (own tab; scans `examples/board`,
    parses the `kbdk-example` header-comment metadata, Load → Edit-tab buffer).
  - Board-side programs section: add an `ledblink.c` bullet (the GPIO facts learned —
    chip/line/polarity + the GPIO_V2-vs-v1 kernel-version finding).
- **Makefile help comment** (top): mention `make ledblink` / `deploy-ledblink`.
- **On-board screenshot gate (mandatory):** `--tab examples --screenshot` renders the
  five cards (re-attach the Task 7/8 shots). The live LED run is the human's
  confirmation (Task 2/1).
- **Final gate:** `cargo build` (workspace) + `cargo test -p kbdk-core` +
  `cargo test -p kbdk-ui` all green; `make ledblink` + the five-template compile loop
  all rc 0. Paste every command's output.

---

## Task dependency graph

```
Task 1 (report, HW) ─┬─> Task 2 (ledblink.c) ─┬─> Task 3 (templates) ─┐
                     │                          │                       │
Task 4 (empty tab) ──┴──> Task 5 (parse, TDD) ─┴─> Task 6 (scan) ──> Task 7 (UI list)
                                                                          │
                                              Build 3 merged ──> Task 8 (Load hand-off)
                                                                          │
                                                                     Task 9 (docs + verify)
```

Tasks 2–3 and 4–7 are two independent chains (board-C vs Rust) that can proceed in
parallel; both must finish before Task 8 (which also waits on Build 3) and Task 9.

## Definition of done

- `parse_metadata` unit tests (incl. the five-template fixtures) green; `cargo build`
  + `cargo test -p kbdk-core` + `cargo test -p kbdk-ui` clean.
- `make ledblink` compiles with no `*_time64` leaks; the five `examples/board/*`
  templates each compile with their own `extra_args`.
- Examples tab lists all five with name/desc; Load opens the template in the Edit tab
  (Task 8, post-Build-3) with the right `extra_args`.
- The LED **physically blinks** on hardware (human-confirmed) and the investigation
  report records the pin + the chardev ABI that works.
- Docs updated (CLAUDE.md GPIO row + Examples tab + ledblink bullet).
