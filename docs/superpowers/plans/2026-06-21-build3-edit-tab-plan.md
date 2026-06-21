# Build 3 — Edit tab: bite-sized TDD implementation plan

> **Source spec:** `docs/superpowers/specs/2026-06-21-edit-tab-design.md` (approved 2026-06-21).
> **Roadmap:** `docs/superpowers/plans/2026-06-21-board-ide-remaining-features.md`.
> **Flow:** this plan → subagent-driven build → review → merge (same as builds 1–2).

Each step is small and independently verifiable. Pure parsers come first as the TDD
core (failing test → impl). UI steps verify by `cargo build`; the final on-board
screenshot is a **human-gated** check (do NOT attach to hardware in the build session).

Conventions used below:
- `VERIFY core` = `cargo test -p kbdk-core`
- `VERIFY build` = `cargo build` (workspace root)
- "RED" = write the test, run it, watch it fail to compile/assert before writing impl.

---

## Group 1 — `kbdk-core::build` module (pure parsers first, then impure helpers)

The whole point of doing this group first: `parse_toolchain` + `parse_diagnostic`
are pure, hardware-free, and the TDD core. `compile()` and `spawn_board_run()` are
the impure shells that wrap them.

### Step 1.1 — Register the module (compiles empty)

- **Files:**
  - Create `crates/kbdk-core/src/build.rs` with just a `//!` doc comment and an
    empty `#[cfg(test)] mod tests {}`.
  - `crates/kbdk-core/src/lib.rs`: add `pub mod build;` (alphabetical-ish, near
    `pub mod deploy;`).
- **Verify:** `VERIFY core` (module compiles, no tests yet).

### Step 1.2 — `CrossToolchain` type + `parse_toolchain` (RED)

The Makefile is the single source of truth (CLAUDE.md "Cross toolchain (root
Makefile, single source of truth)"). Lines that matter, verbatim from the root
`Makefile`:
```
CROSS       ?= arm-unknown-linux-musleabihf-gcc
CROSSFLAGS  ?= -O2 -mcpu=cortex-a7 -mfpu=neon-vfpv4 -mfloat-abi=hard
CROSSXX  ?= arm-unknown-linux-musleabihf-g++
```

- **Files:** `crates/kbdk-core/src/build.rs`.
- **Define (impl in 1.3):**
  ```rust
  pub struct CrossToolchain { pub cc: String, pub cxx: String, pub flags: Vec<String> }
  pub fn parse_toolchain(makefile: &str) -> CrossToolchain
  ```
  Plus fallback constants (used when a key is absent — these must match the
  checked-in Makefile, guarded by a drift test in 1.4):
  ```rust
  pub const FALLBACK_CC: &str  = "arm-unknown-linux-musleabihf-gcc";
  pub const FALLBACK_CXX: &str = "arm-unknown-linux-musleabihf-g++";
  pub const FALLBACK_FLAGS: &str = "-O2 -mcpu=cortex-a7 -mfpu=neon-vfpv4 -mfloat-abi=hard";
  ```
- **Test (RED), in `mod tests`:**
  - `parses_qmark_form`: feed `"CROSS ?= some-gcc\nCROSSXX ?= some-g++\nCROSSFLAGS ?= -O2 -mcpu=cortex-a7\n"`; assert `cc == "some-gcc"`, `cxx == "some-g++"`, `flags == ["-O2", "-mcpu=cortex-a7"]`.
  - `parses_colon_eq_form`: same keys with `:=`; assert the parsed values.
  - `missing_keys_fall_back`: feed `""`; assert `cc == FALLBACK_CC`, `cxx == FALLBACK_CXX`, `flags == FALLBACK_FLAGS.split_whitespace().collect()`.
  - `ignores_unrelated_lines`: a Makefile snippet with comments + other `KEY ?=`
    lines (e.g. `CFLAGS ?= -O2 -Wall`) must not pollute the three keys.
- **Verify:** `VERIFY core` — fails to compile (`parse_toolchain` unimplemented). This is the RED state.

### Step 1.3 — `parse_toolchain` impl (GREEN)

- **Files:** `crates/kbdk-core/src/build.rs`.
- **Impl notes:** for each line, `trim()`, skip `#` comments; split on the first of
  `?=` or `:=` (try `:=` and `?=`); match the trimmed key against `CROSS`,
  `CROSSXX`, `CROSSFLAGS`; store value trimmed. `CROSSFLAGS` → `split_whitespace`.
  After scanning, any key not seen → its fallback. Be careful `CROSS` doesn't also
  match `CROSSXX`/`CROSSFLAGS` — compare the **exact** trimmed key string.
- **Verify:** `VERIFY core` — the four 1.2 tests pass.

### Step 1.4 — Makefile drift guard (RED→GREEN, reads the real Makefile)

- **Files:** `crates/kbdk-core/src/build.rs` (test only).
- **Test:** `fallback_constants_match_checked_in_makefile`:
  - Read the repo `Makefile` via
    `include_str!(concat!(env!("CARGO_MANIFEST_DIR"), "/../../Makefile"))`
    (manifest dir = `crates/kbdk-core`, so `../../Makefile` is the repo root).
  - Assert the file contains each of `FALLBACK_CC`, `FALLBACK_CXX`,
    `FALLBACK_FLAGS` as substrings. This fails loudly if someone edits the
    Makefile flags without updating the fallbacks.
- **Verify:** `VERIFY core` — passes against the current Makefile (the constants in
  1.2 were copied from it verbatim, so this is GREEN immediately; it's a guard, not
  a feature).

### Step 1.5 — `Diagnostic` type + `parse_diagnostic` (RED)

gcc diagnostics look like `path:line:col: error|warning|note: message` (col is
sometimes absent: `path:line: error: msg`).

- **Files:** `crates/kbdk-core/src/build.rs`.
- **Define (impl in 1.6):**
  ```rust
  #[derive(Debug, PartialEq)]
  pub enum Level { Error, Warning, Note }
  #[derive(Debug, PartialEq)]
  pub struct Diagnostic { pub file: String, pub line: u32, pub col: Option<u32>, pub level: Level, pub message: String }
  pub fn parse_diagnostic(line: &str) -> Option<Diagnostic>
  ```
- **Test (RED):**
  - `parses_error_with_col`: `"src/hello.c:12:5: error: 'x' undeclared"` →
    `Some(Diagnostic { file: "src/hello.c", line: 12, col: Some(5), level: Error, message: "'x' undeclared" })`.
  - `parses_warning`: `"a.c:3:1: warning: unused variable 'y'"` → `Warning`, message intact.
  - `parses_note`: `"a.c:9: note: in expansion of macro"` → `col == None`, `Note`.
  - `non_diagnostic_returns_none`: `"plain compiler chatter"`, `""`,
    `"/usr/bin/ld: cannot find -lfoo"` (no `line:col: level:` shape) → all `None`.
  - `windows_drive_not_misparsed` (defensive): `"C:\\x.c:1:1: error: msg"` may be
    `None` — assert it does **not** panic (document expected return).
- **Verify:** `VERIFY core` — fails (unimplemented). RED.

### Step 1.6 — `parse_diagnostic` impl (GREEN)

- **Files:** `crates/kbdk-core/src/build.rs`.
- **Impl notes:** split the line on `": "` into `head` + `message`-ish? Cleaner:
  find the `<sp>error: ` / `<sp>warning: ` / `<sp>note: ` marker via
  `find(" error: ")` etc., take the prefix as `file:line[:col]` and the suffix as
  the message. Parse the prefix by `rsplitn` on `:` to peel optional `col` then
  `line` (both must be numeric — if not numeric, it's not a diagnostic → `None`),
  leaving `file`. Return `None` if no level marker found or numbers don't parse.
- **Verify:** `VERIFY core` — the 1.5 tests pass.

### Step 1.7 — `compile()` (impure; streams compiler output)

Mirrors `pipeline::stream_child`'s line loop (BufReader over merged stdout+stderr).

- **Files:** `crates/kbdk-core/src/build.rs`.
- **Define:**
  ```rust
  pub fn compile(
      src: &Path, out_dir: &Path, extra_args: &[String],
      on_line: &mut dyn FnMut(String),
  ) -> Result<PathBuf>
  ```
- **Impl notes:**
  - `parse_toolchain(&std::fs::read_to_string(<repo Makefile>)?)` — locate the
    Makefile relative to a repo root passed in, OR simpler for v1: accept the
    toolchain via `parse_toolchain` on a Makefile path derived from `src`'s repo.
    **Decision:** add a `makefile: &Path` param OR read fallbacks when no Makefile
    is found. Keep it simple — add `repo_root: &Path` param and read
    `repo_root.join("Makefile")` (fall back to constants if missing). Update the
    signature to `compile(repo_root, src, out_dir, extra_args, on_line)`.
  - Pick compiler by extension: `.cpp`/`.cc`/`.cxx` → `cxx` + append
    `-static-libstdc++ -static-libgcc`; else (`.c`) → `cc`. (Matches the Makefile:
    `kbrun`/`nnacam`/`nna-*` use `CROSSXX` + `-static-libstdc++ -static-libgcc`.)
  - Assemble args: `flags` + (C++ static flags) + `-o <out_dir>/<stem>` + `<src>` +
    `-lm` + `extra_args`. (`-lm` is in the `hello`/`audio` rules; harmless for C++.)
    **Order matters for the linker:** put `extra_args` and `-lm` **after** the
    source so `-ldl -lpthread` style flags from the escape hatch link correctly.
  - Spawn with `stdout(piped())` + `stderr` merged. adbd merges stderr, but the
    **host compiler does not** — so explicitly pipe both and read them. Simplest:
    `Stdio::piped()` on stdout, and redirect stderr to stdout by reading both, OR
    (cleaner, matches gcc) set `.stderr(Stdio::piped())` and read stderr lines too.
    **Decision:** merge by spawning via a small helper that reads stdout then
    stderr, OR use `2>&1` semantics by setting `.stderr(child_stdout_clone)` —
    Rust can't dup easily, so read **both** streams: spawn two BufReaders is racy.
    Cleanest portable form: spawn the compiler under `sh -c "<cc> ... 2>&1"`? No —
    arg quoting is fragile. **Use:** `Command::new(cc).args(...).stdout(piped).stderr(piped)`,
    then drain stderr first (gcc writes diagnostics to stderr), then stdout, then
    `wait()`. gcc emits all diagnostics to stderr and finishes before exit, so
    reading stderr to EOF then stdout to EOF is safe (no interleave deadlock for
    the small single-file builds here). Call `on_line(line)` per line of each.
  - On spawn `ErrorKind::NotFound` → return the friendly
    `Err("cross-compiler '<cc>' not found on PATH — install the messense
    musl-armhf toolchain and ensure /opt/homebrew/bin is on PATH")` (spec Error
    handling). No panic.
  - On non-zero rc → `bail!("compile failed (rc={rc})")`. On success → return
    `out_dir.join(stem)`.
- **Test (integration, host gcc — guarded so CI without arm-cross still passes):**
  - `compile_uses_host_cc_via_override`: set up so the test compiles a trivial
    `.c` with the **host** `cc` rather than the arm cross (which may be absent on
    CI). Achieve by writing a temp Makefile whose `CROSS ?= cc` and `CROSSFLAGS ?=
    -O2`, a temp `hi.c` (`int main(){return 0;}`), call `compile(tmp_root, &hi,
    &out, &[], &mut |l| lines.push(l))`; assert `Ok` and the output file exists.
    This exercises the real spawn/stream path without needing the arm toolchain.
  - `missing_compiler_is_clean_err`: temp Makefile `CROSS ?= definitely-not-a-cc-12345`;
    assert `Err` whose message contains "not found" (no panic).
- **Verify:** `VERIFY core`.

### Step 1.8 — `spawn_board_run()` (impure; returns the live `Child`)

The run/stop mechanism (spec "The run / stop mechanism"): keep the `adb shell`
session open for the program's lifetime; Stop kills the host child → session close
→ adbd reaps the board process group. adbd merges stderr, but add `2>&1` for safety.

- **Files:** `crates/kbdk-core/src/build.rs`.
- **Define:**
  ```rust
  pub fn spawn_board_run(serial: Option<&str>, remote: &str, env_prefix: &str) -> Result<Child>
  ```
- **Impl notes:**
  - Build `Command::new("adb")`; if `serial` is `Some`, push `["-s", serial]`
    (mirrors `AdbTransport::adb()`).
  - Args: `["shell", &format!("{env_prefix} {remote} 2>&1")]` — `env_prefix` is
    `""` by default, or `LD_LIBRARY_PATH=/usr/lib/eyesee-mpp:/usr/lib` when the
    MPP-libs toggle is on (same prefix as `deploy::start_runner` and the Makefile
    `deploy-*` echoes).
  - `.stdout(Stdio::piped())`. Return the `Child` so the worker streams its stdout
    and records `child.id()` for Stop.
- **Test (no hardware — assert command shape, not execution):**
  - This function only builds + spawns `adb`; a hardware-free unit test would try
    to spawn `adb` (may be absent on CI). **Decision:** keep `spawn_board_run`
    thin and instead unit-test a pure helper it delegates to:
    ```rust
    pub fn board_run_argv(serial: Option<&str>, remote: &str, env_prefix: &str) -> Vec<String>
    ```
    returning the exact argv (`["-s","SER","shell","ENV /tmp/x 2>&1"]` /
    `["shell","/tmp/x 2>&1"]`). `spawn_board_run` calls `board_run_argv` then
    spawns. Test `board_run_argv`:
    - `argv_no_serial_no_env`: `board_run_argv(None, "/tmp/hello", "")` →
      `["shell", "/tmp/hello 2>&1"]` (env prefix collapses to a leading space-trim:
      assert the command string trims to `"/tmp/hello 2>&1"`).
    - `argv_with_serial_and_mpp`: `board_run_argv(Some("ABC"), "/tmp/camcc",
      "LD_LIBRARY_PATH=/usr/lib/eyesee-mpp:/usr/lib")` → contains `-s`, `ABC`,
      `shell`, and the command string starts with the LD_LIBRARY_PATH prefix and
      ends with ` 2>&1`.
- **Verify:** `VERIFY core` — all build.rs tests green; **Group 1 complete.**

---

## Group 2 — `workers.rs` Msg variants + methods + pump arms

Same background-thread / mpsc / `ctx.request_repaint()` template as `run_py`. All
board + subprocess I/O stays off the UI thread.

### Step 2.1 — New `Msg` variants

- **Files:** `crates/kbdk-ui/src/workers.rs` (the `enum Msg`).
- **Add:**
  ```rust
  BuildOutput(String),
  BuildDone(Result<std::path::PathBuf, String>),
  RunOutput(String),
  RunDone(Result<(), String>),   // NOTE: a RunDone already exists for the pack
                                  // runner; rename the new one EditRunDone to
                                  // avoid colliding with the deploy-tab semantics.
  ```
  **Decision (avoid collision):** the existing `Msg::RunDone(Result<(), String>)`
  is consumed by the Deploy tab's pump arm to set `self.running = true` and clear
  perf history — do **not** reuse it. Name the Edit-tab variants distinctly:
  `EditBuildOutput(String)`, `EditBuildDone(Result<PathBuf,String>)`,
  `EditRunOutput(String)`, `EditRunDone(Result<(),String>)`, `EditRunStopped`.
- **Verify:** `VERIFY build` — will fail until pump() has arms (Step 2.4). Expected;
  add arms in 2.4 before re-verifying.

### Step 2.2 — `run_child` slot on `Workers`

Mirror the existing `py_pid: Arc<Mutex<Option<u32>>>` slot for the edit-run child.

- **Files:** `crates/kbdk-ui/src/workers.rs` (struct `Workers`), and
  `crates/kbdk-ui/src/app.rs` (the `Workers { ... }` literal in `KbdkApp::new`).
- **Add:** `pub run_pid: Arc<std::sync::Mutex<Option<u32>>>,` to `Workers`; init
  `run_pid: Default::default(),` in `app.rs`'s `Workers { ... }` construction.
- **Verify:** `VERIFY build` (still red on pump until 2.4 — fine).

### Step 2.3 — `build`, `deploy_run`, `stop_run` methods

- **Files:** `crates/kbdk-ui/src/workers.rs` (`impl Workers`).
- **`build(&self, src: PathBuf, out_dir: PathBuf, extra_args: Vec<String>)`:**
  - Clone `tx`, `ctx`, `repo_root`; `std::thread::spawn`.
  - `let r = kbdk_core::build::compile(&repo_root, &src, &out_dir, &extra_args,
    &mut |line| { let _ = tx.send(Msg::EditBuildOutput(line)); ctx.request_repaint(); });`
  - Send `Msg::EditBuildDone(r.map_err(|e| e.to_string()))`; `ctx.request_repaint()`.
- **`deploy_run(&self, bin: PathBuf, remote: String, env_prefix: String)`:**
  - Spawn thread. Inside:
    - `let t = AdbTransport::new(None);`
    - `t.push(&bin, &remote)` (md5-verified ×3) → on Err send
      `Msg::EditRunDone(Err(...))` and return.
    - `t.exec(&format!("chmod +x {remote}"), 15)` → check rc; on failure send Err.
    - `let child = kbdk_core::build::spawn_board_run(None, &remote, &env_prefix)`;
      record `*run_pid.lock().unwrap() = Some(child.id())`.
    - Stream the child's stdout line-by-line (BufReader::lines), each →
      `Msg::EditRunOutput(line)` + `ctx.request_repaint()`.
    - `child.wait()`, clear `*run_pid.lock().unwrap() = None`, send
      `Msg::EditRunDone(Ok(()))`.
  - **Note:** the run binary's remote name (for the best-effort kill in stop) =
    `remote.rsplit('/').next()`. Store nothing extra; recompute in `stop_run` from
    the app's known remote (pass it in, see 2.3 stop).
- **`stop_run(&self, remote_name: String)`:**
  - Clone `tx`, `ctx`, `run_pid`. Spawn thread.
    - If `Some(pid) = *run_pid.lock().unwrap()` → `Command::new("kill").arg(pid.to_string()).status()`
      (kills the host adb child → session closes → board process group reaped,
      per CLAUDE.md adbd-process-group fact).
    - Best-effort board kill: `AdbTransport::new(None).exec(&format!("kill
      $(pidof {remote_name}) 2>/dev/null; true"), 15)` (insurance for forked progs).
    - Send `Msg::EditRunStopped`; `ctx.request_repaint()`.
- **Verify:** `VERIFY build` (still needs pump arms — do 2.4 next).

### Step 2.4 — `pump()` match arms

- **Files:** `crates/kbdk-ui/src/app.rs` (`fn pump`).
- **Add arms (refer to `EditState` from Group 3 — sequence Group 3 step 3.2 with
  this if the borrow checker needs the fields to exist; in practice add the fields
  first, then these arms):**
  ```rust
  Msg::EditBuildOutput(s) => { self.edit.output.push(s); }
  Msg::EditBuildDone(r) => {
      self.edit.building = false;
      match r {
          Ok(bin) => { self.edit.last_bin = Some(bin.clone());
                       self.edit.output.push(format!("build ok: {}", bin.display())); }
          Err(e) => { self.edit.last_bin = None;
                      self.edit.output.push(format!("build failed: {e}")); }
      }
  }
  Msg::EditRunOutput(s) => { self.edit.output.push(s); }
  Msg::EditRunDone(r) => {
      self.edit.running = false;
      if let Err(e) = r { self.edit.output.push(format!("run error: {e}")); }
      else { self.edit.output.push("(program exited)".into()); }
  }
  Msg::EditRunStopped => { self.edit.running = false; self.edit.output.push("(stopped)".into()); }
  ```
  Keep `self.edit.output` bounded (e.g. `if len > 2000 { drain first 500 }`) to
  avoid unbounded growth on long-running camera programs.
- **Verify:** `VERIFY build` — workspace compiles once `EditState` exists (Group 3
  3.1–3.2 must land for the `self.edit` field). Build Groups 2 and 3 together if
  the compiler requires; the steps are ordered so 3.1/3.2 can precede 2.4.

---

## Group 3 — `app.rs` Tab::Edit + EditState + wiring

### Step 3.1 — `Tab::Edit` variant + top_bar + ui() match + --tab hook

- **Files:** `crates/kbdk-ui/src/app.rs`, `crates/kbdk-ui/src/main.rs`.
- **Edits:**
  - `enum Tab { ... , Edit }` — add `Edit`.
  - `top_bar()`: add `ui.selectable_value(&mut self.f.tab, Tab::Edit, "Edit");`
    (place it after `Tab::Hardware` or wherever reads best; spec just needs it
    selectable). Suggested order: put `Edit` right after `Files` (it's the IDE
    spine and pairs with the local tree) — `Train, Convert, Deploy & Run, Files,
    Edit, Tasks, Hardware`.
  - `ui()` central match: add `Tab::Edit => edit_tab::show(self, ui),`.
  - `use` line at top: add `edit_tab` to the `use crate::{...}` import group.
  - `--tab` startup match (the `match tab.as_str()` around line 279): add
    `"edit" => Tab::Edit,`.
  - `crates/kbdk-ui/src/main.rs`: add `mod edit_tab;` to the module list.
- **Verify:** `VERIFY build` — fails until `edit_tab::show` exists (Group 4). Add a
  stub `pub fn show(_: &mut KbdkApp, _: &mut egui::Ui) {}` in 3.3 first, or land
  Group 4 step 4.1 (the file skeleton) before re-verifying. Order: do 3.3 (state),
  then 4.1 (stub), then `VERIFY build`.

### Step 3.2 — `EditState` struct on `KbdkApp`

- **Files:** `crates/kbdk-ui/src/app.rs`.
- **Add a struct** (in `app.rs`, or co-locate in `edit_tab.rs` and re-export — keep
  it in `app.rs` next to where other tab state lives, mirroring `files` /
  `hw_info`):
  ```rust
  pub struct EditState {
      pub tree: files_tab::FileTree,   // local-only file tree (reuse the type)
      pub open_path: Option<std::path::PathBuf>,
      pub buffer: String,
      pub saved_snapshot: String,      // dirty = buffer != saved_snapshot
      pub output: Vec<String>,         // combined compile + run log
      pub building: bool,
      pub running: bool,
      pub last_bin: Option<std::path::PathBuf>, // set by EditBuildDone(Ok); gates Deploy+Run
      pub status: String,
  }
  ```
  `mpp_libs` (bool) and `extra_args` (String) are **persisted** → put them on
  `Fields` (see 3.4), not `EditState`. `FileTree::new` is private in
  `files_tab.rs` — **make it `pub`** (one-word change in `files_tab.rs`) so
  `EditState` can build a local tree, OR add a small `pub fn local_tree(root) ->
  FileTree` constructor in `files_tab.rs`. Prefer the latter (keeps the API
  intentional): add to `files_tab.rs`:
  ```rust
  pub fn local_tree(root: String) -> FileTree { FileTree::new(root) }
  ```
- **Add field to `KbdkApp`:** `pub edit: EditState,` and initialize it in
  `KbdkApp::new` (root the tree at the repo root: `files_tab::local_tree(
  self.workers.repo_root.to_string_lossy().into_owned())` — but `workers` is built
  earlier in `new`, so construct from `std::env::current_dir()` like the Files tab
  does, or reuse `f.last_local_path`). Use the repo root string; default `buffer`
  empty, `output` empty, flags false.
- **Verify:** `VERIFY build` (after the 4.1 stub exists).

### Step 3.3 — Add the `edit_tab` module reference (stub to unblock build)

- **Files:** `crates/kbdk-ui/src/edit_tab.rs` (create with a stub).
- **Stub:** `use crate::app::KbdkApp; use eframe::egui; pub fn show(app: &mut
  KbdkApp, ui: &mut egui::Ui) { ui.label("Edit (stub)"); }` (replaced fully in
  Group 4).
- **Verify:** `VERIFY build` — workspace compiles green with the new tab selectable.
  Optional smoke: `cargo run -p kbdk-ui` and click the Edit tab → "Edit (stub)".

### Step 3.4 — Persist `mpp_libs` + `extra_args` (+ optional `open_path`) via `Fields`

- **Files:** `crates/kbdk-ui/src/app.rs`.
- **Add to `Fields`** (with `#[serde(default…)]` so older storage still loads —
  CLAUDE.md "New fields need `#[serde(default…)]`"):
  ```rust
  #[serde(default)] pub edit_mpp_libs: bool,
  #[serde(default)] pub edit_extra_args: String,
  #[serde(default)] pub edit_open_path: String, // last opened file (optional convenience)
  ```
  Add matching initializers in `impl Default for Fields`.
- **Optional:** extend `apply_field_overrides` so `KBDK_FIELDS=edit_open_path=…`
  can seed the screenshot's open file (the spec's screenshot gate seeds an open
  file via `KBDK_FIELDS`). Add arms:
  `"edit_open_path" => f.edit_open_path = v.into(),`
  `"edit_extra_args" => f.edit_extra_args = v.into(),`
  `"edit_mpp_libs" => f.edit_mpp_libs = v == "1" || v == "true",`
  Then in `KbdkApp::new`, after `apply_field_overrides`, if `f.edit_open_path` is
  non-empty, load it into `edit.buffer`/`edit.open_path`/`edit.saved_snapshot` so
  `--tab edit` screenshots show real content.
- **Verify:** `VERIFY build`.

---

## Group 4 — `edit_tab.rs` UI

Layout = all `show_inside` panels nested in the tab's `ui` (egui 0.34: use
`Panel::*::show_inside`, never `update`/`show`). Order panels so each reserves its
space before the editor claims the rest (mirrors `files_tab.rs`'s bottom-then-side
ordering).

### Step 4.1 — Module skeleton (replaces the 3.3 stub) + dirty helper

- **Files:** `crates/kbdk-ui/src/edit_tab.rs`.
- **Content:** `pub fn show(app, ui)` with a header row (`ui.heading("Edit")` +
  `app.edit.status` in SUBTEXT), then `ui.separator()`. Add a private
  `fn is_dirty(s: &EditState) -> bool { s.buffer != s.saved_snapshot }`.
- **Verify:** `VERIFY build`.

### Step 4.2 — Left file tree (reuse `files_tab::read_local_dir`)

- **Files:** `crates/kbdk-ui/src/edit_tab.rs`.
- **Impl:** `egui::SidePanel::left("edit_tree").resizable(true).default_width(240.0)
  .show_inside(ui, |ui| { ... ScrollArea::vertical ... edit_node(ui, app, &root) });`
  - Reimplement a **local-only** recursive `edit_node` (no board side, no
    context menus, no md5 transfer) modeled on `files_tab::local_node` + the
    file/dir `row` logic, but **clicking a file opens it into the editor** instead
    of selecting for transfer:
    - dir row: arrow disclosure, toggle `app.edit.tree.expanded`, lazy-fill
      `app.edit.tree.children` via `files_tab::read_local_dir(path)`.
    - file row: `selectable_label`; on click → `open_file(app, &full)`.
  - `fn open_file(app, path)`: `std::fs::read_to_string(path)` → on Ok set
    `buffer`, `saved_snapshot = buffer.clone()`, `open_path = Some(path)`,
    `status = format!("opened {path}")`; on Err set `status` to the error (don't
    clobber the buffer).
  - Show only source-ish files prominently but don't hard-filter (user may open
    headers); keep it simple — list all like the Files tab.
- **Verify:** `VERIFY build`. Smoke: `cargo run -p kbdk-ui` → Edit tab → expand
  `src/`, click `hello.c` → buffer fills.

### Step 4.3 — Toolbar row (Open / Save / Build / Deploy+Run / Stop + toggles)

- **Files:** `crates/kbdk-ui/src/edit_tab.rs`.
- **Impl:** a `ui.horizontal(|ui| { ... })` directly under the header:
  - **Open…** — `rfd::FileDialog::new().pick_file()` (rfd is already a dep); on
    `Some(path)` call `open_file`.
  - **Save** — enabled when `open_path.is_some()`; `std::fs::write(path, &buffer)`
    → on Ok set `saved_snapshot = buffer.clone()`, status "saved"; on Err status
    error, buffer preserved (spec Error handling).
  - **Build** — enabled when `open_path.is_some() && !building`; on click:
    auto-save first (write buffer), set `building = true`, clear `output`,
    compute `out_dir = repo_root/"bin"` (matches Makefile output dir; `mkdir -p`
    via `std::fs::create_dir_all`), parse `extra_args` from `f.edit_extra_args`
    by `split_whitespace`, call
    `app.workers.build(open_path.clone(), out_dir, extra_args)`.
  - **Deploy + Run** — enabled when `last_bin.is_some() && !running` (gated on a
    clean build per spec data-flow "enabled only after a clean build"); on click:
    `let name = last_bin.file_name()`; `remote = format!("/tmp/{name}")`;
    `env_prefix = if f.edit_mpp_libs { "LD_LIBRARY_PATH=/usr/lib/eyesee-mpp:/usr/lib" } else { "" }`;
    set `running = true`, `app.workers.deploy_run(last_bin.clone(), remote, env_prefix.into())`.
  - **Stop** — enabled when `running`; on click: `name = last_bin.file_name()` (or
    track the deployed remote name on `EditState`); `app.workers.stop_run(name)`.
    Stop-with-nothing-running is a no-op (button disabled).
  - **MPP libs** checkbox → `ui.checkbox(&mut app.f.edit_mpp_libs, "MPP libs")`.
  - **extra build args** field → spec warns `TextEdit` inside a `Grid` collapses;
    we're in a `horizontal` row, so a sized field is fine:
    `ui.add(egui::TextEdit::singleline(&mut app.f.edit_extra_args)
    .hint_text("-ldl -lpthread …").desired_width(220.0));`
  - **dirty/status label** → `if is_dirty(&app.edit) { ui.colored_label(theme::PEACH
    or YELLOW, "●") }` + the file name with a `*` suffix when dirty; plus
    `app.edit.status` text.
- **Verify:** `VERIFY build`.

### Step 4.4 — Bottom output panel + monospace editor

- **Files:** `crates/kbdk-ui/src/edit_tab.rs`.
- **Order (reserve bottom first, like `files_tab.rs`):**
  - `egui::TopBottomPanel::bottom("edit_output").resizable(true).default_height(180.0)
    .show_inside(ui, |ui| { ... })`:
    - header row: `ui.label("output")` + a small "clear" button (`app.edit.output.clear()`).
    - `egui::ScrollArea::vertical().stick_to_bottom(true).auto_shrink([false,false])
      .show(ui, |ui| { ui.add(egui::Label::new(egui::RichText::new(
      app.edit.output.join("\n")).monospace()).wrap()); });`
      (auto-scroll via `stick_to_bottom` so streamed compile/run lines follow).
  - Then the editor fills the remaining central area:
    `egui::CentralPanel::default().show_inside(ui, |ui| {
       egui::ScrollArea::vertical().auto_shrink([false,false]).show(ui, |ui| {
         ui.add_sized(ui.available_size(),
           egui::TextEdit::multiline(&mut app.edit.buffer)
             .code_editor().lock_focus(true).desired_width(f32::INFINITY));
       });
     });`
    (`code_editor()` = monospace; `lock_focus(true)` so Tab inserts a tab — spec.)
- **Verify:** `VERIFY build`. Smoke: `cargo run -p kbdk-ui` → Edit tab renders tree
  + toolbar + editor + output; typing flips the dirty `●`/`*`.

### Step 4.5 — Full workspace + core test pass

- **Verify:**
  - `VERIFY core` — all `kbdk-core::build` unit tests green.
  - `VERIFY build` — workspace compiles with no warnings introduced.
  - `cargo clippy -p kbdk-core -p kbdk-ui` (optional) — no new lints.

---

## Group 5 — Board cross-compile sanity (no UI, confirms the toolchain path)

A quick non-UI confirmation that `kbdk-core::build::compile` produces a real
armv7l binary with the **exact** Makefile flags (wrong float ABI silently faults
on the board — CLAUDE.md). This is host-only (no board attached).

### Step 5.1 — Manual compile check via the real arm cross toolchain

- **Run (if the arm cross toolchain is installed on the build host):** a tiny
  throwaway test or a `cargo run`-driven Build of `src/hello.c` through the UI,
  then inspect the output binary:
  - `file bin/hello` → must report `ELF 32-bit LSB ... ARM ... EABI5`.
  - `arm-unknown-linux-musleabihf-readelf -A bin/hello` → Tag_CPU_arch ARM v7,
    `Tag_ABI_VFP_args: VFP registers` (hard-float), NEON present.
  - Compare the spawned argv (printed via a temporary `eprintln!` or the output
    panel) against the `hello` Makefile rule: `<cc> -O2 -mcpu=cortex-a7
    -mfpu=neon-vfpv4 -mfloat-abi=hard -o bin/hello src/hello.c -lm`.
- **If the cross toolchain is absent on the build host:** skip the binary
  inspection; the `compile_uses_host_cc_via_override` test (Step 1.7) already
  proves the spawn/stream/path mechanics. Note the skip in the PR description so a
  reviewer with the toolchain runs it.
- **Verify:** binary is ARM hard-float EABI5; flags match the Makefile verbatim.

---

## Group 6 — On-board screenshot gate (HUMAN-GATED, final check)

> **Do NOT attach to hardware in the build session.** The live screenshot is run by
> the human afterward. Builds 1–2 both shipped layout/probe bugs that only the live
> screenshot caught — this gate is non-negotiable (spec "On-board screenshot gate").

### Step 6.1 — Capture the Edit tab with a seeded open file

- **Command (human runs it; board may be attached for a Deploy+Run smoke but not
  required for the layout screenshot):**
  ```sh
  KBDK_FIELDS='edit_open_path=src/hello.c' KBDK_SHOT_DELAY=2 \
    cargo run -p kbdk-ui -- --tab edit --screenshot /tmp/edit_tab.png
  ```
  (`--tab edit` from Step 3.1; `--screenshot`/`KBDK_SHOT_DELAY` from app.rs's
  `screenshot_tick`; `KBDK_FIELDS=edit_open_path=…` from Step 3.4 seeds the editor
  so the screenshot shows the tree + populated editor + empty output panel.)
- **Confirm in `/tmp/edit_tab.png`:** left file tree visible and rooted at the
  repo; toolbar row (Open/Save/Build/Deploy+Run/Stop + MPP-libs checkbox + extra-args
  field) laid out without clipping; monospace editor showing `hello.c` contents;
  bottom output panel present. Fix any layout/clipping issues found, then re-shoot.

### Step 6.2 — (Optional, board attached) live build→deploy→run smoke

- Open `src/hello.c` → **Build** → confirm compiler lines stream into the output
  panel and "build ok" appears → **Deploy + Run** → confirm the `printf` + sqrt
  output streams back → for a long-running program (`src/camcc.c` with **MPP libs**
  checked) confirm live streaming and that **Stop** ends it (verify via the **Tasks**
  tab that the board process is gone — spec manual/integration tests).

---

## Done criteria

- `cargo test -p kbdk-core` green (parse_toolchain, parse_diagnostic, compile
  spawn/err, board_run_argv, Makefile drift guard).
- `cargo build` (workspace) green; Edit tab selectable; `--tab edit` works.
- On-board screenshot (`/tmp/edit_tab.png`) reviewed by a human and layout-correct.
- (If board available) Build → Deploy + Run → Stop verified live.
