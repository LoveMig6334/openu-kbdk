# Build 3 — Edit tab (code editor + build / deploy / run)

> **Status:** design approved 2026-06-21. This is the spec for **Build 3** of the
> board-IDE roadmap (`docs/superpowers/plans/2026-06-21-board-ide-remaining-features.md`).
> Next step after this spec is reviewed: `superpowers:writing-plans` → a bite-sized
> TDD implementation plan → subagent-driven build → review → merge (same flow as
> builds 1–2: Files/Tasks and the Hardware panel).

## Goal

Edit a board C/C++ source file inside `kbdk-ui`, cross-compile it on the host,
deploy the binary over ADB, run it on the board, and watch compiler + runtime
output stream back — without leaving the app. This is the IDE spine: the thing
that turns kbdk-ui from an ML pipeline into a board IDE.

## Settled design decisions

These resolve the "Open design decisions" the roadmap left for Build 3:

| Decision | Choice | Notes |
| --- | --- | --- |
| Editor widget | **Plain monospace `TextEdit::multiline`** in v1 | No new dep. Syntax highlighting, line numbers, clickable error→line are fast-follows. |
| File open source | **Reuse the Files-tab local tree** | Left file-tree rail (reusing `files_tab::read_local_dir` + `FileTree`/`DirEntry`); click a file to open it. |
| Build model | **Single-file direct cross-compile** | The build module invokes the cross-compiler on the open source file. No multi-file/project build. Flags kept DRY (single source of truth = the repo `Makefile`). |
| Run model | **Live-stream stdout + Stop button** | Spawn `adb shell` as a host child, stream its output live; Stop kills the child (session close → board process group dies). Handles both quick and long-running/graphical programs. |
| Languages | **C and C++** | `.c` → gcc, `.cpp`/`.cc`/`.cxx` → g++ (C++ adds `-static-libstdc++ -static-libgcc`). |

## Architecture & units

Four well-bounded units, each matching an existing pattern in the codebase.

### 1. `kbdk-core::build` (new module)

Three responsibilities — two of them pure functions that are the TDD core:

- **`parse_toolchain(makefile: &str) -> CrossToolchain`** — *pure*. Extracts the
  cross compiler + flags from the repo `Makefile` so they live in **one place**:
  - `CROSS` (e.g. `arm-unknown-linux-musleabihf-gcc`)
  - `CROSSXX` (the `…-g++` C++ driver)
  - `CROSSFLAGS` (`-O2 -mcpu=cortex-a7 -mfpu=neon-vfpv4 -mfloat-abi=hard`)

  Handles `KEY ?= value` and `KEY := value` forms. If a key is missing, falls back
  to baked constants that match the current Makefile (a unit test guards drift by
  asserting the constants appear in the checked-in Makefile).

- **`parse_diagnostic(line: &str) -> Option<Diagnostic>`** — *pure*. Parses gcc
  diagnostic lines `path:line:col: error|warning|note: message` into
  `Diagnostic { file, line, col, level, message }`. Non-diagnostic lines → `None`.
  Used for a pass/fail summary + error count in v1; clickable-to-line is a
  fast-follow that reuses this same parser.

- **`compile(src: &Path, out_dir: &Path, extra_args: &[String], on_line: &mut dyn FnMut(String)) -> Result<PathBuf>`**
  — impure. Picks gcc vs g++ from the source extension, assembles
  `CROSSFLAGS` + (`-static-libstdc++ -static-libgcc` for C++) + `-lm` +
  `extra_args`, spawns the compiler with stdout+stderr piped (merged), streams each
  line through `on_line`, and returns the output binary path (`out_dir/<stem>`)
  on success. Mirrors the line-by-line streaming shape of
  `kbdk_core::pipeline::stream_child`.

### 2. `kbdk-core` board-run path (new, small)

Lives in `build.rs` (or a sibling `run.rs`):

- **`spawn_board_run(serial: Option<&str>, remote: &str, env_prefix: &str) -> Result<Child>`**
  — spawns `adb [-s serial] shell "<env_prefix> <remote> 2>&1"` with stdout piped,
  returns the `Child` so the worker can (a) stream its stdout line-by-line and
  (b) record the child pid for Stop. `env_prefix` is empty by default, or
  `LD_LIBRARY_PATH=/usr/lib/eyesee-mpp:/usr/lib` when the MPP-libs toggle is on.
  Push + chmod still go through the existing `Transport` (`push` is md5-verified ×3).

### 3. `crates/kbdk-ui/src/edit_tab.rs` (new)

The UI, `pub fn show(app: &mut KbdkApp, ui: &mut egui::Ui)` like every other tab.
Layout (all `show_inside`, exactly like the app shell uses panels):

- **Left rail** — `SidePanel::left`: a local file tree reusing
  `files_tab::read_local_dir` + `FileTree`/`DirEntry`, rooted at the repo root by
  default (configurable). Clicking a file opens it into the editor.
- **Toolbar** — a horizontal row: `Open…` (also an rfd dialog as a convenience),
  `Save`, `Build`, `Deploy + Run`, `Stop`; an **MPP libs** checkbox (adds the
  `LD_LIBRARY_PATH` prefix for camera/audio programs); an **extra build args**
  text field (appended verbatim to the compile — the single-file escape hatch for
  `-ldl -lpthread` and MPP `-I`/`-D` includes); a dirty/status label.
- **Editor pane** — `TextEdit::multiline(&mut buffer).code_editor().lock_focus(true)`
  (monospace; `lock_focus` so Tab inserts a tab instead of moving focus), filling
  the available area in a `ScrollArea`.
- **Output panel** — `TopBottomPanel::bottom`: the combined compiler + runtime log
  (a scrolling, monospace, auto-scrolled text view).

### 4. `crates/kbdk-ui/src/workers.rs` additions

New worker methods + `Msg` variants, drained in `pump()` — same background-thread /
mpsc / `ctx.request_repaint()` template as the existing `run_py`:

- `Workers::build(src, out_dir, extra_args)` → streams `Msg::BuildOutput(String)`,
  finishes with `Msg::BuildDone(Result<PathBuf, String>)`.
- `Workers::deploy_run(bin, remote, env_prefix)` → push + chmod, then spawn
  `adb shell`, stream `Msg::RunOutput(String)`, finish with
  `Msg::RunDone(Result<(), String>)`. Records the run child pid in an
  `Arc<Mutex<Option<u32>>>` (mirrors the existing `py_pid` slot).
- `Workers::stop_run()` → kills the recorded run child on the host (`kill <pid>`)
  and fires a best-effort `adb shell kill $(pidof <name>)`.

## The run / stop mechanism (key judgment call)

This deliberately **uses** a hard-won board fact instead of fighting it:
**adbd kills the entire process group when the session closes** (the board's
BusyBox has no `setsid`/`nohup` applet, so a generic edited program cannot be
daemonized the way `kbrun` self-daemonizes). So:

- **Run** keeps the `adb shell` session *open* for the program's lifetime and
  streams its output live. A quick program exits on its own → its output is shown,
  `RunDone` fires. A long-running/graphical program (camera, framebuffer) keeps
  running and streaming until stopped.
- **Stop** kills the host `adb` child → the session closes → adbd reaps the board
  process group. As insurance for programs that fork, Stop also fires a best-effort
  `adb shell kill $(pidof <name>)`. A stuck board process is always also killable
  from the existing **Tasks** tab.

Because legacy adbd merges stderr into stdout (no shell_v2), capturing the child's
stdout (`2>&1` for safety) yields both compiler/runtime stdout and stderr.

## Components / files

**Create**
- `crates/kbdk-core/src/build.rs` — `parse_toolchain`, `parse_diagnostic`,
  `compile`, `spawn_board_run`, the `CrossToolchain`/`Diagnostic` types, unit tests.
- `crates/kbdk-ui/src/edit_tab.rs` — the tab UI.

**Modify**
- `crates/kbdk-core/src/lib.rs` — `pub mod build;`.
- `crates/kbdk-ui/src/main.rs` — `mod edit_tab;`.
- `crates/kbdk-ui/src/app.rs` — `Tab::Edit` variant; `top_bar` `selectable_value`;
  `ui()` match arm `Tab::Edit => edit_tab::show(self, ui)`; `--tab edit` hook; an
  `EditState` struct on `KbdkApp` (tree, `open_path`, `buffer`, `saved_snapshot`
  for the dirty `*`, `output` log, `building`/`running` flags, `mpp_libs` +
  `extra_args` fields, `run_pid`); optional persistence of `open_path`/`extra_args`
  via `Fields`.
- `crates/kbdk-ui/src/workers.rs` — the new `Msg` variants, worker methods, and
  `pump()` arms.

## Data flow

```
click file in tree
  → std::fs::read into buffer (dirty=false, snapshot=buffer)
edit
  → buffer != snapshot ⇒ dirty=true, "*" shown
Save
  → std::fs::write(open_path, buffer); snapshot=buffer; dirty=false
Build
  → workers.build → kbdk-core::build::compile
  → Msg::BuildOutput(line)* → Msg::BuildDone(Ok(bin) | Err)
Deploy + Run   (enabled only after a clean build)
  → workers.deploy_run → Transport::push(bin→/tmp/<name>) → chmod +x
  → spawn adb shell "<env> /tmp/<name> 2>&1"
  → Msg::RunOutput(line)* → Msg::RunDone(Ok | Err)
Stop
  → kill host adb child  (+ best-effort  adb shell kill $(pidof <name>))
```

## Error handling

- **No cross-compiler on PATH** → `compile` returns a clear `Err`
  ("install the messense musl-armhf toolchain; ensure /opt/homebrew/bin on PATH"),
  shown in the output panel. No panic.
- **Compile failure** → non-zero rc; parsed diagnostics + error count shown;
  Deploy + Run stays disabled until a clean build.
- **Board offline / push md5 mismatch** → `Transport::push` already retries ×3 then
  errors; surfaced via `Msg::RunDone(Err)`.
- **Save to a missing/read-only path** → error to the status line, buffer preserved.
- **Stop with nothing running** → no-op.

## Testing & verification

- **Unit tests (hardware-free, the TDD core):**
  - `parse_toolchain`: `?=` form, `:=` form, missing keys → fallback constants,
    and a drift guard that the fallback constants appear in the checked-in `Makefile`.
  - `parse_diagnostic`: error / warning / note lines parse to the right fields;
    non-diagnostic lines (plain text, linker output) → `None`.
- **Manual / integration (board attached):**
  - Open `src/hello.c` → Build → Deploy + Run → see the `printf` + sqrt output.
  - Open `src/fbtest.c` → Build → Deploy + Run.
  - Start a long-running program → confirm live streaming → Stop ends it (verify via
    Tasks tab that the board process is gone).
- **On-board screenshot gate (mandatory):** launch with `--screenshot PATH`
  `--tab edit` and a `KBDK_FIELDS`-seeded open file; confirm the tree + editor +
  output panel render correctly. Builds 1–2 both shipped layout/probe bugs that
  only the live screenshot caught — this gate is non-negotiable.

## Non-goals (v1) and fast-follows

**Non-goals:** multi-file / project build system; LSP / autocomplete / inline
diagnostics; a board-side terminal/REPL (Tasks tab is monitor+kill); a debugger
(gdbserver); complex multi-lib C++ (ncnn/MPP-linked like `kbrun`) — the
"extra build args" field is the escape hatch for moderate cases.

**Fast-follows (explicit, not in v1):** syntax highlighting (hand-rolled C
tokenizer → `LayoutJob`, or `egui_code_editor`/`syntect`); line numbers;
clickable error→line (reusing `parse_diagnostic`); persisting a recent-files list.

## Dependencies

None new — `rfd` (file dialog) is already a `kbdk-ui` dependency. The cross
toolchain (`arm-unknown-linux-musleabihf-gcc`/`-g++`) is the existing host
requirement documented in `CLAUDE.md`.
