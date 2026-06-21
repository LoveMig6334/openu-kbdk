# Board IDE — Remaining Features Roadmap (builds 3–5)

> **Status:** consolidated planning document for the three remaining board-IDE
> features. This is a roadmap at the design/decomposition level — goals, scope,
> recommended approach, the units to build, a task outline, dependencies, and the
> open design decisions to settle. Each feature still gets its design confirmed
> (via `superpowers:brainstorming`) and a detailed, bite-sized implementation plan
> (via `superpowers:writing-plans`) **before** it is built. Do not implement
> directly from this file — the per-feature "Open design decisions" must be resolved
> first.

## Where this fits

`kbdk-ui` is becoming a board IDE (vision approved 2026-06-21). Shipped so far:

- ✅ **Build 1** — Files + Tasks tabs (`docs/superpowers/specs/2026-06-21-kbdk-ui-files-tasks-design.md`)
- ✅ **Build 2** — Hardware panel (`docs/superpowers/specs/2026-06-21-hardware-panel-design.md`)

Remaining (this document):

- ⬜ **Build 3** — Code editor + build/deploy (the IDE spine)
- ⬜ **Build 4** — Example templates (incl. a new GPIO LED-blink board program)
- ⬜ **Build 5** — Navigation reshell (collapse ML under a menu)

**Build order:** 3 → 4 (Examples need the editor to open/build/flash into) and 5 can
slot in anywhere (it's independent; now is a fine time since six tabs exist). Each is
its own spec → plan → subagent-driven build → review → merge cycle, exactly like
builds 1–2.

## Shared context (applies to all three)

- **UI pattern:** tabs are a `Tab` enum variant + a `top_bar` `selectable_value` + a
  `ui()` match arm + a `<name>_tab::show(app, ui)` module. State lives on `KbdkApp`;
  all board/subprocess I/O runs on worker threads via `Workers` → `Msg` (mpsc) →
  `pump()`, never on the UI thread. egui 0.34, `theme::` color constants.
- **Board I/O:** `kbdk_core::transport::Transport` (`exec`/`push`/`pull`) via
  `AdbTransport::new(None)`. Deploy/run primitives in `kbdk_core::deploy`.
- **Host subprocess streaming:** `kbdk_core::pipeline` already spawns child processes
  (uv) and streams their output line-by-line over a callback — the model for streaming
  a compiler's output (Build) is the same shape.
- **Cross-toolchain (host):** `arm-unknown-linux-musleabihf-gcc` (C) /
  `…-g++` (C++), `CROSSFLAGS = -O2 -mcpu=cortex-a7 -mfpu=neon-vfpv4 -mfloat-abi=hard`
  (musl-armhf, hard-float — the wrong ABI silently faults on the board). The repo
  `Makefile` already cross-compiles the board C programs; the build path can shell the
  Makefile or invoke the compiler directly.
- **Verification reality:** UI features are gated on a clean build + a **live
  on-board screenshot** (the app self-captures via `--screenshot PATH`; persisted tab
  set through eframe storage). Builds 1–2 both had layout/probe bugs that ONLY the live
  run caught — every UI build here must end with an on-board screenshot check.

---

## Build 3 — Code editor + build/deploy (the spine)

### Goal
Edit a board C/C++ source file in-app, cross-compile it on the host, deploy the
binary over ADB, run it on the board, and see compiler + runtime output — without
leaving kbdk-ui. This is what makes it feel like an IDE.

### Scope
- An **Edit** tab: a file picker/open (reuse the Files-tab local tree or an OS dialog),
  a text editor pane, Save.
- **Build:** invoke the cross-compiler on the host, stream compiler stdout/stderr
  (errors clickable-to-line is a stretch goal, not v1), show pass/fail.
- **Deploy + Run:** on success, `push` the binary to `/tmp`, `chmod +x`, `exec` it
  (optionally with `LD_LIBRARY_PATH=/usr/lib/eyesee-mpp:/usr/lib` for MPP programs),
  stream its output back.
- Output panel (compiler + runtime), reusing the `pipeline`/exec streaming pattern.

### Non-goals (v1)
- A full multi-file project/build system (single-file or single-Makefile-target only).
- LSP / autocomplete / inline diagnostics.
- A board-side terminal (the Tasks tab is monitor+kill; a REPL/terminal is its own future feature).
- Debugging (gdbserver) — future.

### Recommended approach
- **Editor widget:** start with egui's built-in `TextEdit::multiline` (monospace,
  code-styled) — zero new deps, good enough for v1. Syntax highlighting is an
  enhancement: either a small hand-rolled C tokenizer feeding `LayoutJob`, or a crate
  (`egui_code_editor`, or `syntect` via `egui_extras`) — adds a dependency; decide in
  brainstorming. **Recommendation: ship v1 with plain monospace `TextEdit`, add
  highlighting as a fast-follow.**
- **Build invocation:** a new `kbdk-core::build` module that shells
  `arm-unknown-linux-musleabihf-gcc` with `CROSSFLAGS` (read the triple/flags so they
  stay in one place) and streams output via the existing `pipeline::stream_child`
  shape. Single source → single binary in `bin/`. Reuse the repo Makefile's flags
  verbatim (hard-float ABI is non-negotiable).
- **Deploy+run:** reuse `Transport::push` + an `exec("chmod +x … && …")`; stream
  runtime output with a bounded exec (long-running programs redirect to a file +
  tail, like kbrun).

### Components / files
- Create `crates/kbdk-core/src/build.rs` — locate the cross compiler, compile one
  source to a host binary, stream compiler output, return success/diagnostics. Pure
  helpers (e.g. parse a gcc error line → file:line:col:msg) unit-tested.
- Create `crates/kbdk-ui/src/edit_tab.rs` — editor + build/deploy/run buttons + output panel.
- Modify `workers.rs` (`Msg::BuildOutput`/`BuildDone`/`RunOutput`, `build`/`deploy_run` workers),
  `app.rs` (Tab + state: open file path, buffer, dirty flag, output log), `main.rs`.

### Task outline (full TDD plan written later)
1. `kbdk-core::build` — compiler discovery + compile + gcc-error parser (TDD on the parser).
2. Build streaming worker + `Msg` + pump (host subprocess, mirrors `pipeline`).
3. Empty **Edit** tab wired into the shell.
4. Editor pane: open (local file) / edit / save, dirty indicator.
5. Build button + streamed compiler output panel; error display.
6. Deploy + Run button + streamed runtime output (reuse Transport push/exec).
7. On-board screenshot verification + docs.

### Open design decisions (resolve in brainstorming first)
- Syntax highlighting in v1 or fast-follow? (recommend fast-follow.)
- File source: reuse the Files-tab local tree, an OS file dialog, or both?
- Single-file compile vs "build a Makefile target"? (recommend single-file first; the
  existing `make hello/fbtest/...` targets are the alternative.)
- Run model for long-running/graphical programs (MPP camera, fb): redirect+tail like
  kbrun, or a fixed timeout? How does the user stop a running program (tie into Tasks-tab kill)?
- C and C++ both, or C only for v1?

---

## Build 4 — Example templates (+ GPIO LED-blink board program)

### Goal
A library of ready-to-run example C programs the user can load into the editor, build,
and flash — so a newcomer can go from zero to blinking an LED / opening the camera in
one click.

### Scope
- An **Examples** browser (a panel, or a section of the Edit tab): a curated list of
  templates; "Load into editor" opens the source in Build 3's editor.
- Templates ship as real source files in the repo (e.g. `examples/board/*.c`), each a
  self-contained, documented program:
  - **hello** — printf + sqrt (hard-float smoke test; `src/hello.c` already exists).
  - **camera open** — wraps the existing MPP capture/preview (`cammpp.c`/`camcc.c` are
    the working basis; the template is a minimal, commented version).
  - **audio tone** — raw-ALSA tone (basis: `src/audio.c`).
  - **screen** — framebuffer test pattern (basis: `src/fbtest.c`).
  - **LED blink** — **a NEW GPIO board program that must be written** (see below).

### The LED-blink board program (real new work)
This is board-C work, separate from the UI. The board exposes GPIO via **`/dev/gpiochip0`
+ `/dev/gpiochip1`** (raw chardev `GPIO_V2_*` ioctl) and **`/sys/class/gpio`** (sysfs
export). There is **no `/sys/class/leds`**, so the on-board LED is a raw GPIO pin that
must be identified first.
- **Open question (blocking):** which gpiochip + line drives the KidBright µAI LED?
  Determine from the board schematic / device-tree / trial (toggle candidate lines and
  watch). This is a small hardware-investigation task that precedes writing the program.
- **Approach:** prefer the raw `/dev/gpiochipN` chardev ioctl path (the project's
  "direct ioctl, no vendor lib" philosophy; `linux/gpio.h` UAPI), with sysfs as a
  documented fallback. The program: open the chip, request the LED line as output,
  toggle with a delay loop. Add a `Makefile` target (`make ledblink` / `deploy-ledblink`)
  mirroring `hello`.
- This becomes the first real **GPIO** capability in the toolkit (the capability table
  in CLAUDE.md lists GPIO as "ready, not yet written").

### Components / files
- New `examples/board/` source files (curated, commented templates) + Makefile targets.
- New `src/ledblink.c` (or `examples/board/ledblink.c`) — the GPIO program.
- `crates/kbdk-ui` — an Examples list (manifest of {name, description, path}); "Load"
  wires into the Edit tab's open. Likely small; may live inside `edit_tab.rs` or a
  sibling `examples_tab.rs`.

### Task outline (full TDD plan written later)
1. (Board) Identify the LED GPIO line (hardware investigation).
2. (Board) Write `ledblink.c` via raw `/dev/gpiochip` ioctl + Makefile target; verify it blinks on hardware.
3. Curate the other templates as documented `examples/board/*.c` (refactor from existing probes).
4. Examples list UI + "Load into editor" (depends on Build 3's editor open path).
5. On-board screenshot verification + docs (update the GPIO capability row in CLAUDE.md).

### Open design decisions (resolve in brainstorming first)
- Examples as their own tab vs a panel inside the Edit tab?
- Template manifest: a checked-in JSON/TOML index, or just scan `examples/board/`?
- Scope of the starter set (which of camera/audio/screen/LED/GPIO-button to ship first)?
- LED program: raw chardev ioctl (recommended) vs sysfs — and the pin-identification method.

### Dependency
Needs Build 3 (the editor + build/deploy) to exist so templates have somewhere to load,
build, and flash. The LED-blink *board program* can be written independently/earlier (it
has standalone value and a Makefile target).

---

## Build 5 — Navigation reshell

### Goal
Realize the agreed "full board IDE" navigation: collapse the ML pipeline
(Train/Convert/Deploy) under an **ML ▾** menu, and present the board/dev areas
(Files, Tasks, Hardware, Edit, Examples) as the primary top-level navigation.

### Scope
- Restructure `top_bar` in `app.rs`: an `ML ▾` dropdown (egui `menu_button`) holding
  Train / Convert / Deploy & Run; the dev tabs as direct top-level entries.
- Preserve the persisted `Tab` enum + `f.tab` selection and all existing test hooks
  (`--tab`, `KBDK_*`) — purely a presentation change, no behavior/state change.
- Optionally a left rail instead of a top bar — decide in brainstorming.

### Non-goals
- Any change to tab contents or state. This is navigation chrome only.

### Recommended approach
A focused `top_bar` rewrite using egui `menu::menu_button` for the ML group; keep the
`Tab` enum and routing untouched. Lowest-risk of the three. Best done **after** Edit +
Examples exist so the final tab set is known (avoids reshaping nav twice).

### Components / files
- Modify `crates/kbdk-ui/src/app.rs` (`top_bar` only; possibly the `--tab` hook to
  accept the new tab names). No new modules.

### Task outline (full TDD plan written later)
1. Rewrite `top_bar`: `ML ▾` menu_button (Train/Convert/Deploy) + dev tabs top-level.
2. Update the `--tab` test hook to accept all current tab names (files/tasks/hardware/edit/examples).
3. On-board screenshot verification (nav renders, every tab still reachable) + docs.

### Open design decisions (resolve in brainstorming first)
- Top-bar menu vs left sidebar rail?
- Exact grouping/labels (is "ML ▾" right; where do Files/Tasks/Hardware/Edit/Examples sit)?
- Do this before or after Edit/Examples? (Recommend after, so the tab set is final.)

---

## Summary table

| Build | Feature | New kbdk-core | New kbdk-ui | Board C | Risk | Depends on |
| --- | --- | --- | --- | --- | --- | --- |
| 3 | Code editor + build/deploy | `build.rs` | `edit_tab.rs` | — | High (toolchain + streaming) | — |
| 4 | Example templates + LED blink | — | examples list | **`ledblink.c`** (new GPIO) | Medium (board GPIO research) | Build 3 |
| 5 | Navigation reshell | — | `top_bar` rewrite | — | Low | best after 3 & 4 |

## Next step

Pick the next build (recommend **Build 3 — Code editor**), run
`superpowers:brainstorming` to settle its "Open design decisions", write its spec, then
its full implementation plan, then build it subagent-driven — same flow as builds 1–2.
