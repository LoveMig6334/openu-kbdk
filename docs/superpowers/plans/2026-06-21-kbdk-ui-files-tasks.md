# kbdk-ui Files & Tasks Tabs Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add two new tabs to the kbdk-ui desktop app — a dual-pane Files manager (local ↔ board transfer + rm/chmod/mkdir + read-only preview) and a Tasks manager (board process list + kill) — as the first step toward a full board IDE.

**Architecture:** Two new pure-parser modules in `kbdk-core` (`fs.rs`, `procs.rs`) wrap board shell commands over the existing `Transport` trait; their text parsers are unit-tested without hardware. The UI adds two tab modules following the existing `show(app, ui)` pattern, with all board I/O dispatched to background threads via the existing `Workers`/`Msg` mpsc plumbing — the UI thread never blocks.

**Tech Stack:** Rust, eframe/egui 0.34, `kbdk-core` (anyhow, md5), board access via `AdbTransport` (adb shell over USB-OTG).

## Global Constraints

- **Transport:** all board I/O goes through `kbdk_core::transport::Transport`; the UI constructs `kbdk_core::adb::AdbTransport::new(None)` inside worker threads (matches every existing worker — `workers.rs`). Serial fallback is a non-goal for this build.
- **Threading:** board I/O NEVER runs on the UI thread. Workers spawn a `std::thread`, send a `Msg` over `self.tx`, then call `ctx.request_repaint()` (see `Workers::deploy`). Local-host filesystem reads (left pane) may run synchronously on the UI thread (they're instant).
- **Exec signature:** `Transport::exec(&self, cmd: &str, timeout_secs: u64) -> Result<ExecResult>` where `ExecResult { output: String, rc: i32 }`. Always check `rc`.
- **No recursive board listing:** one `ls -la <dir>` per expanded directory (lazy). No `find` over the tree — it overruns the exec window and loads the single Cortex-A7.
- **No background polling** of board state in these tabs (manual refresh only) — adbd competes with any running kbrun on the single A7.
- **Destructive ops** (`rm`, `kill -9`) require an in-UI confirmation before the worker is dispatched.
- **Binary-safe transfers:** never round-trip binary file bytes through `exec` (its output is lossy UTF-8). Preview fetches `head -c` into a tmpfs file then `Transport::pull`s it.
- **Theme:** use `crate::theme` color constants (e.g. `theme::RED`, `theme::GREEN`, `theme::SUBTEXT`); never hardcode `Color32`.
- **Persistence:** new persisted `Fields` members get `#[serde(default = "…")]` so storage written by older builds still deserializes.

---

## File Structure

**Created:**
- `crates/kbdk-core/src/fs.rs` — board filesystem ops: `DirEntry`, `parse_ls`, `list_dir`, `looks_binary`, `read_head`, `remove`, `chmod`, `mkdir`.
- `crates/kbdk-core/src/procs.rs` — process ops: `Proc`, `parse_procs`, `list_procs`, `kill`.
- `crates/kbdk-core/tests/hw_fs.rs` — `KBDK_HW`-gated hardware smoke test.
- `crates/kbdk-ui/src/files_tab.rs` — Files tab UI + per-tab state (`FilesState`, `FileTree`).
- `crates/kbdk-ui/src/tasks_tab.rs` — Tasks tab UI + per-tab state (`TasksState`).

**Modified:**
- `crates/kbdk-core/src/lib.rs` — register `pub mod fs;` and `pub mod procs;`.
- `crates/kbdk-ui/src/main.rs` — declare the two new modules.
- `crates/kbdk-ui/src/workers.rs` — new `Msg` variants + worker methods.
- `crates/kbdk-ui/src/app.rs` — `Tab` enum variants, `top_bar` buttons, `ui()` routing, new state on `KbdkApp`, `pump()` arms, `Fields` last-path members.

---

## Task 1: kbdk-core `fs.rs` — directory listing & content sniff

**Files:**
- Create: `crates/kbdk-core/src/fs.rs`
- Modify: `crates/kbdk-core/src/lib.rs`

**Interfaces:**
- Consumes: `crate::transport::Transport` (`exec`, `pull`).
- Produces:
  - `pub struct DirEntry { pub name: String, pub is_dir: bool, pub size: u64, pub mode: String }`
  - `pub fn parse_ls(out: &str) -> Vec<DirEntry>`
  - `pub fn looks_binary(bytes: &[u8]) -> bool`
  - `pub fn list_dir(t: &dyn Transport, path: &str) -> anyhow::Result<Vec<DirEntry>>`
  - `pub fn read_head(t: &dyn Transport, path: &str, max_bytes: usize, local: &std::path::Path) -> anyhow::Result<Vec<u8>>`
  - `pub fn remove(t: &dyn Transport, path: &str, is_dir: bool) -> anyhow::Result<()>`
  - `pub fn chmod(t: &dyn Transport, path: &str, mode: &str) -> anyhow::Result<()>`
  - `pub fn mkdir(t: &dyn Transport, path: &str) -> anyhow::Result<()>`
  - `pub fn join_path(parent: &str, name: &str) -> String`

- [ ] **Step 1: Register the module**

In `crates/kbdk-core/src/lib.rs`, add after `pub mod discover;`:

```rust
pub mod fs;
```

- [ ] **Step 2: Write the failing parser tests**

Create `crates/kbdk-core/src/fs.rs` with only the test module first:

```rust
#[cfg(test)]
mod tests {
    use super::*;

    const LS: &str = "\
total 28
drwxr-xr-x    3 root     root          4096 Jun 10 12:00 .
drwxr-xr-x    5 root     root          4096 Jun 10 11:00 ..
-rwxr-xr-x    1 root     root         38000 Jun 10 12:00 fbtest
drwxr-xr-x    2 root     root          4096 Jun 10 12:01 sub dir
lrwxrwxrwx    1 root     root             7 Jun 10 12:00 sh -> busybox
";

    #[test]
    fn parse_ls_extracts_entries() {
        let v = parse_ls(LS);
        // `.` and `..` and the `total` line are dropped
        let names: Vec<&str> = v.iter().map(|e| e.name.as_str()).collect();
        assert_eq!(names, ["fbtest", "sub dir", "sh"]);

        let fbtest = &v[0];
        assert!(!fbtest.is_dir);
        assert_eq!(fbtest.size, 38000);
        assert_eq!(fbtest.mode, "-rwxr-xr-x");

        assert!(v[1].is_dir); // "sub dir" — name with a space survives
        assert_eq!(v[1].name, "sub dir");

        // symlink: target stripped, treated as a file
        assert_eq!(v[2].name, "sh");
        assert!(!v[2].is_dir);
    }

    #[test]
    fn parse_ls_tolerates_junk() {
        assert!(parse_ls("").is_empty());
        assert!(parse_ls("ls: /nope: No such file or directory").is_empty());
    }

    #[test]
    fn looks_binary_detects_nul_and_text() {
        assert!(looks_binary(b"\x7fELF\x01\x01\x00"));
        assert!(looks_binary(&[0u8, 1, 2, 3]));
        assert!(!looks_binary(b"hello\nworld\n"));
        assert!(!looks_binary(b"")); // empty = treat as text
    }

    #[test]
    fn join_path_handles_root() {
        assert_eq!(join_path("/", "tmp"), "/tmp");
        assert_eq!(join_path("/tmp", "x"), "/tmp/x");
    }
}
```

- [ ] **Step 3: Run the tests to verify they fail**

Run: `cargo test -p kbdk-core fs:: 2>&1 | head -30`
Expected: FAIL — compile errors (`parse_ls`, `looks_binary`, `join_path`, `DirEntry` not found).

- [ ] **Step 4: Implement the parsers and wrappers**

Prepend to `crates/kbdk-core/src/fs.rs` (above the test module):

```rust
//! Board filesystem ops over the Transport. Listing is one `ls -la <dir>` per
//! directory (lazy, never recursive). Binary content is never round-tripped
//! through exec (lossy UTF-8) — preview fetches into a tmpfs file then pulls it.

use crate::transport::Transport;
use anyhow::{bail, Result};
use std::path::Path;

#[derive(Debug, Clone, PartialEq)]
pub struct DirEntry {
    pub name: String,
    pub is_dir: bool,
    pub size: u64,
    pub mode: String,
}

/// Parse busybox `ls -la` output. Drops the `total` line and `.`/`..`.
/// Defensive: lines that don't look like a long listing are skipped.
pub fn parse_ls(out: &str) -> Vec<DirEntry> {
    let mut v = Vec::new();
    for line in out.lines() {
        let cols: Vec<&str> = line.split_whitespace().collect();
        // perms links owner group size mon day time name...  => >= 9 cols
        if cols.len() < 9 || cols[0] == "total" {
            continue;
        }
        let mode = cols[0];
        // first char of perms: 'd' dir, 'l' symlink, '-' file, etc.
        let kind = mode.as_bytes()[0];
        if kind != b'd' && kind != b'l' && kind != b'-' {
            continue; // not a recognisable entry line
        }
        // name = everything from column 8 onward (handles spaces in names)
        let name_part = cols[8..].join(" ");
        // symlink: "name -> target" — keep just the name
        let name = name_part
            .split(" -> ")
            .next()
            .unwrap_or(&name_part)
            .to_string();
        if name == "." || name == ".." {
            continue;
        }
        let size = cols[4].parse::<u64>().unwrap_or(0);
        v.push(DirEntry {
            name,
            is_dir: kind == b'd',
            size,
            mode: mode.to_string(),
        });
    }
    v
}

/// Heuristic: a NUL byte, or >30% control chars (excluding tab/newline/CR),
/// means binary. Empty = text.
pub fn looks_binary(bytes: &[u8]) -> bool {
    if bytes.is_empty() {
        return false;
    }
    if bytes.contains(&0) {
        return true;
    }
    let ctrl = bytes
        .iter()
        .filter(|&&b| b < 0x09 || (b > 0x0d && b < 0x20))
        .count();
    ctrl * 100 / bytes.len() > 30
}

/// Join a board path and a child name without doubling the slash at root.
pub fn join_path(parent: &str, name: &str) -> String {
    if parent == "/" {
        format!("/{name}")
    } else {
        format!("{}/{name}", parent.trim_end_matches('/'))
    }
}

/// Single-quote a path for the board shell (board paths here never contain `'`).
fn q(path: &str) -> String {
    format!("'{}'", path.replace('\'', "'\\''"))
}

pub fn list_dir(t: &dyn Transport, path: &str) -> Result<Vec<DirEntry>> {
    let r = t.exec(&format!("ls -la {}", q(path)), 15)?;
    if r.rc != 0 {
        bail!("ls {path} rc={} ({})", r.rc, r.output.trim());
    }
    Ok(parse_ls(&r.output))
}

/// Fetch up to `max_bytes` of a board file into `local` (binary-safe) and
/// return the bytes. Uses `head -c` into tmpfs, then pulls the truncated copy.
pub fn read_head(t: &dyn Transport, path: &str, max_bytes: usize, local: &Path) -> Result<Vec<u8>> {
    let tmp = "/tmp/kbdk_preview.bin";
    let r = t.exec(
        &format!("head -c {max_bytes} {} > {tmp} 2>/dev/null; echo rc=$?", q(path)),
        15,
    )?;
    if !r.output.contains("rc=0") {
        bail!("read {path}: {}", r.output.trim());
    }
    t.pull(tmp, local)?;
    Ok(std::fs::read(local)?)
}

pub fn remove(t: &dyn Transport, path: &str, is_dir: bool) -> Result<()> {
    let flag = if is_dir { "-rf" } else { "-f" };
    let r = t.exec(&format!("rm {flag} {}", q(path)), 15)?;
    if r.rc != 0 {
        bail!("rm {path} rc={} ({})", r.rc, r.output.trim());
    }
    Ok(())
}

pub fn chmod(t: &dyn Transport, path: &str, mode: &str) -> Result<()> {
    let r = t.exec(&format!("chmod {mode} {}", q(path)), 15)?;
    if r.rc != 0 {
        bail!("chmod {mode} {path} rc={} ({})", r.rc, r.output.trim());
    }
    Ok(())
}

pub fn mkdir(t: &dyn Transport, path: &str) -> Result<()> {
    let r = t.exec(&format!("mkdir -p {}", q(path)), 15)?;
    if r.rc != 0 {
        bail!("mkdir {path} rc={} ({})", r.rc, r.output.trim());
    }
    Ok(())
}
```

- [ ] **Step 5: Run the tests to verify they pass**

Run: `cargo test -p kbdk-core fs:: 2>&1 | tail -20`
Expected: PASS — `parse_ls_extracts_entries`, `parse_ls_tolerates_junk`, `looks_binary_detects_nul_and_text`, `join_path_handles_root` all ok.

- [ ] **Step 6: Commit**

```bash
git add crates/kbdk-core/src/fs.rs crates/kbdk-core/src/lib.rs
git commit -m "kbdk-core: fs module — board ls parse, content sniff, file ops"
```

---

## Task 2: kbdk-core `procs.rs` — process list & kill

**Files:**
- Create: `crates/kbdk-core/src/procs.rs`
- Modify: `crates/kbdk-core/src/lib.rs`

**Interfaces:**
- Consumes: `crate::transport::Transport` (`exec`).
- Produces:
  - `pub struct Proc { pub pid: u32, pub rss_kb: u64, pub state: String, pub cmd: String }`
  - `pub fn parse_procs(out: &str) -> Vec<Proc>`
  - `pub fn list_procs(t: &dyn Transport) -> anyhow::Result<Vec<Proc>>`
  - `pub fn kill(t: &dyn Transport, pid: u32, sig: i32) -> anyhow::Result<()>`
  - `pub const PROC_SCAN_CMD: &str` (the batched /proc shell command)

- [ ] **Step 1: Register the module**

In `crates/kbdk-core/src/lib.rs`, add after `pub mod fs;`:

```rust
pub mod procs;
```

- [ ] **Step 2: Write the failing parser tests**

Create `crates/kbdk-core/src/procs.rs` with only the test module first:

```rust
#[cfg(test)]
mod tests {
    use super::*;

    // tab-separated: pid \t rss_kb \t state \t cmd
    const PS: &str = "\
1\t1240\tS\t/sbin/init
412\t14208\tS\t/tmp/kbrun /mnt/UDISK/kbdk/toy3 320x240
7\t0\tS\t[kworker/0:0]
notanumber\t0\tS\tjunk
";

    #[test]
    fn parse_procs_reads_rows() {
        let v = parse_procs(PS);
        assert_eq!(v.len(), 3); // the "notanumber" row is skipped
        assert_eq!(v[1].pid, 412);
        assert_eq!(v[1].rss_kb, 14208);
        assert_eq!(v[1].state, "S");
        assert_eq!(v[1].cmd, "/tmp/kbrun /mnt/UDISK/kbdk/toy3 320x240");
        // kernel thread: empty cmdline fell back to comm in brackets
        assert_eq!(v[2].cmd, "[kworker/0:0]");
    }

    #[test]
    fn parse_procs_tolerates_empty() {
        assert!(parse_procs("").is_empty());
    }
}
```

- [ ] **Step 3: Run the tests to verify they fail**

Run: `cargo test -p kbdk-core procs:: 2>&1 | head -30`
Expected: FAIL — `parse_procs`, `Proc` not found.

- [ ] **Step 4: Implement the parser and wrappers**

Prepend to `crates/kbdk-core/src/procs.rs` (above the test module):

```rust
//! Board process listing + kill over the Transport. busybox `ps` omits RSS,
//! so processes are read straight from /proc in one batched shell command
//! (proven approach — the Deploy tab's perf monitor already reads /proc).

use crate::transport::Transport;
use anyhow::{bail, Result};

#[derive(Debug, Clone, PartialEq)]
pub struct Proc {
    pub pid: u32,
    pub rss_kb: u64,
    pub state: String,
    pub cmd: String,
}

/// One exec, one tab-separated line per pid: `pid \t rss_kb \t state \t cmd`.
/// cmd = /proc/<pid>/cmdline (NULs -> spaces); kernel threads have an empty
/// cmdline so fall back to the bracketed comm from /proc/<pid>/stat.
pub const PROC_SCAN_CMD: &str = "\
for d in /proc/[0-9]*; do \
  p=${d#/proc/}; \
  r=$(awk '/^VmRSS:/{print $2}' \"$d/status\" 2>/dev/null); \
  s=$(awk '{print $3}' \"$d/stat\" 2>/dev/null); \
  c=$(tr '\\0' ' ' < \"$d/cmdline\" 2>/dev/null); \
  c=$(echo \"$c\" | sed 's/ *$//'); \
  if [ -z \"$c\" ]; then n=$(awk '{print $2}' \"$d/stat\" 2>/dev/null); c=\"$n\"; fi; \
  printf '%s\\t%s\\t%s\\t%s\\n' \"$p\" \"${r:-0}\" \"${s:-?}\" \"$c\"; \
done; true";

pub fn parse_procs(out: &str) -> Vec<Proc> {
    let mut v = Vec::new();
    for line in out.lines() {
        let mut it = line.splitn(4, '\t');
        let (Some(pid), Some(rss), Some(state), Some(cmd)) =
            (it.next(), it.next(), it.next(), it.next())
        else {
            continue;
        };
        let Ok(pid) = pid.trim().parse::<u32>() else {
            continue;
        };
        v.push(Proc {
            pid,
            rss_kb: rss.trim().parse().unwrap_or(0),
            state: state.trim().to_string(),
            cmd: cmd.trim().to_string(),
        });
    }
    v
}

pub fn list_procs(t: &dyn Transport) -> Result<Vec<Proc>> {
    let r = t.exec(PROC_SCAN_CMD, 15)?;
    if r.rc != 0 {
        bail!("proc scan rc={} ({})", r.rc, r.output.trim());
    }
    let mut v = parse_procs(&r.output);
    v.sort_by(|a, b| b.rss_kb.cmp(&a.rss_kb)); // heaviest first
    Ok(v)
}

pub fn kill(t: &dyn Transport, pid: u32, sig: i32) -> Result<()> {
    let r = t.exec(&format!("kill -{sig} {pid}"), 15)?;
    if r.rc != 0 {
        bail!("kill -{sig} {pid} rc={} ({})", r.rc, r.output.trim());
    }
    Ok(())
}
```

- [ ] **Step 5: Run the tests to verify they pass**

Run: `cargo test -p kbdk-core procs:: 2>&1 | tail -20`
Expected: PASS — `parse_procs_reads_rows`, `parse_procs_tolerates_empty`.

- [ ] **Step 6: Commit**

```bash
git add crates/kbdk-core/src/procs.rs crates/kbdk-core/src/lib.rs
git commit -m "kbdk-core: procs module — /proc-based process list + kill"
```

---

## Task 3: Hardware smoke test (KBDK_HW-gated)

**Files:**
- Create: `crates/kbdk-core/tests/hw_fs.rs`

**Interfaces:**
- Consumes: `kbdk_core::fs`, `kbdk_core::procs`, `kbdk_core::adb::AdbTransport`.

This task has no unit-test cycle — it runs only against a real board. The verification step is the gated command itself.

- [ ] **Step 1: Write the gated integration test**

Create `crates/kbdk-core/tests/hw_fs.rs`:

```rust
// Hardware integration smoke test for fs + procs.
// Run with: KBDK_HW=1 cargo test -p kbdk-core --test hw_fs -- --nocapture
use kbdk_core::adb::AdbTransport;
use kbdk_core::{fs, procs};

fn enabled() -> bool {
    std::env::var("KBDK_HW").is_ok()
}

#[test]
fn list_tmp_and_procs() {
    if !enabled() {
        eprintln!("skipping hw_fs (set KBDK_HW=1 with a board attached)");
        return;
    }
    let t = AdbTransport::new(None);

    let entries = fs::list_dir(&t, "/tmp").expect("list /tmp");
    println!("/tmp has {} entries", entries.len());

    let procs = procs::list_procs(&t).expect("list procs");
    assert!(procs.iter().any(|p| p.pid == 1), "expected pid 1 in list");
    println!("{} processes; heaviest: {:?}", procs.len(), procs.first());
}

#[test]
fn push_read_remove_roundtrip() {
    if !enabled() {
        return;
    }
    let t = AdbTransport::new(None);
    let local = std::env::temp_dir().join("kbdk_hw_fs.txt");
    std::fs::write(&local, b"hello board\n").unwrap();
    t.push(&local, "/tmp/kbdk_hw_fs.txt").expect("push");

    let back = std::env::temp_dir().join("kbdk_hw_fs_back.txt");
    let bytes = fs::read_head(&t, "/tmp/kbdk_hw_fs.txt", 4096, &back).expect("read_head");
    assert_eq!(&bytes, b"hello board\n");
    assert!(!fs::looks_binary(&bytes));

    fs::remove(&t, "/tmp/kbdk_hw_fs.txt", false).expect("remove");
}
```

- [ ] **Step 2: Verify it compiles (no board needed)**

Run: `cargo test -p kbdk-core --test hw_fs --no-run 2>&1 | tail -5`
Expected: compiles; test binary built.

- [ ] **Step 3: (Optional, board attached) run it**

Run: `KBDK_HW=1 cargo test -p kbdk-core --test hw_fs -- --nocapture`
Expected: PASS — prints `/tmp` entry count and process count, round-trip asserts hold. (Skips cleanly if no board / `KBDK_HW` unset.)

- [ ] **Step 4: Commit**

```bash
git add crates/kbdk-core/tests/hw_fs.rs
git commit -m "kbdk-core: KBDK_HW smoke test for fs + procs"
```

---

## Task 4: Wire two empty tabs into the app shell

**Files:**
- Modify: `crates/kbdk-ui/src/main.rs`
- Modify: `crates/kbdk-ui/src/app.rs` (Tab enum, top_bar, ui routing)
- Create: `crates/kbdk-ui/src/files_tab.rs` (stub)
- Create: `crates/kbdk-ui/src/tasks_tab.rs` (stub)

**Interfaces:**
- Produces: `files_tab::show(app: &mut KbdkApp, ui: &mut egui::Ui)`, `tasks_tab::show(app: &mut KbdkApp, ui: &mut egui::Ui)`, `Tab::Files`, `Tab::Tasks`.

This and later UI tasks have no automated test (the repo doesn't unit-test egui UI); the gate is a clean build plus the described manual check.

- [ ] **Step 1: Declare the modules**

In `crates/kbdk-ui/src/main.rs`, add the two module declarations alongside the existing `mod deploy_tab;` etc. (keep the file's existing ordering/style):

```rust
mod files_tab;
mod tasks_tab;
```

- [ ] **Step 2: Create stub tab modules**

Create `crates/kbdk-ui/src/files_tab.rs`:

```rust
//! Files tab: dual-pane local <-> board file transfer + management.

use crate::app::KbdkApp;
use eframe::egui;

pub fn show(_app: &mut KbdkApp, ui: &mut egui::Ui) {
    ui.heading("Files");
    ui.label("dual-pane file manager — coming in the next step");
}
```

Create `crates/kbdk-ui/src/tasks_tab.rs`:

```rust
//! Tasks tab: board process list + kill.

use crate::app::KbdkApp;
use eframe::egui;

pub fn show(_app: &mut KbdkApp, ui: &mut egui::Ui) {
    ui.heading("Tasks");
    ui.label("process manager — coming in the next step");
}
```

- [ ] **Step 3: Add the Tab variants**

In `crates/kbdk-ui/src/app.rs`, extend the enum:

```rust
pub enum Tab {
    Train,
    Convert,
    Deploy,
    Files,
    Tasks,
}
```

- [ ] **Step 4: Add the nav buttons**

In `crates/kbdk-ui/src/app.rs`, in `top_bar`, after the existing `Deploy & Run` selectable_value line:

```rust
            ui.selectable_value(&mut self.f.tab, Tab::Deploy, "Deploy & Run");
            ui.selectable_value(&mut self.f.tab, Tab::Files, "Files");
            ui.selectable_value(&mut self.f.tab, Tab::Tasks, "Tasks");
```

- [ ] **Step 5: Route the tabs**

In `crates/kbdk-ui/src/app.rs`, in the `CentralPanel` match in `ui()`:

```rust
            .show_inside(ui, |ui| match self.f.tab {
                Tab::Train => train_tab::show(self, ui),
                Tab::Convert => convert_tab::show(self, ui),
                Tab::Deploy => deploy_tab::show(self, ui),
                Tab::Files => files_tab::show(self, ui),
                Tab::Tasks => tasks_tab::show(self, ui),
            });
```

- [ ] **Step 6: Add the modules to the use list**

In `crates/kbdk-ui/src/app.rs`, update the existing `use crate::{...}` line to include the new modules:

```rust
use crate::{convert_tab, deploy_tab, files_tab, tasks_tab, theme, train_tab};
```

- [ ] **Step 7: Build and manually verify**

Run: `cargo build -p kbdk-ui 2>&1 | tail -5`
Expected: builds clean.

Run: `cargo run -p kbdk-ui` and confirm two new tabs **Files** and **Tasks** appear in the top bar and show their placeholder text when clicked. Close the app.

- [ ] **Step 8: Commit**

```bash
git add crates/kbdk-ui/src/main.rs crates/kbdk-ui/src/app.rs \
        crates/kbdk-ui/src/files_tab.rs crates/kbdk-ui/src/tasks_tab.rs
git commit -m "kbdk-ui: add empty Files and Tasks tabs to the shell"
```

---

## Task 5: Tasks tab — process list + kill

**Files:**
- Modify: `crates/kbdk-ui/src/workers.rs`
- Modify: `crates/kbdk-ui/src/app.rs` (state, pump)
- Modify: `crates/kbdk-ui/src/tasks_tab.rs`

**Interfaces:**
- Consumes: `kbdk_core::procs::{list_procs, kill, Proc}`, `AdbTransport`.
- Produces:
  - `Msg::ProcList(Vec<kbdk_core::procs::Proc>)`, `Msg::Killed { pid: u32 }`, `Msg::OpError { context: String, message: String }`
  - `Workers::list_procs(&self)`, `Workers::kill_proc(&self, pid: u32, sig: i32)`
  - `KbdkApp` fields: `procs: Vec<kbdk_core::procs::Proc>`, `tasks_status: String`, `kill_confirm: Option<(u32, String)>`
  - `tasks_tab::show`

- [ ] **Step 1: Add Msg variants**

In `crates/kbdk-ui/src/workers.rs`, add to the `enum Msg`:

```rust
    ProcList(Vec<kbdk_core::procs::Proc>),
    Killed { pid: u32 },
    OpError { context: String, message: String },
```

- [ ] **Step 2: Add worker methods**

In `crates/kbdk-ui/src/workers.rs`, add inside `impl Workers`:

```rust
    pub fn list_procs(&self) {
        let tx = self.tx.clone();
        let ctx = self.ctx.clone();
        std::thread::spawn(move || {
            let t = AdbTransport::new(None);
            match kbdk_core::procs::list_procs(&t) {
                Ok(v) => { let _ = tx.send(Msg::ProcList(v)); }
                Err(e) => { let _ = tx.send(Msg::OpError { context: "list processes".into(), message: e.to_string() }); }
            }
            ctx.request_repaint();
        });
    }

    pub fn kill_proc(&self, pid: u32, sig: i32) {
        let tx = self.tx.clone();
        let ctx = self.ctx.clone();
        std::thread::spawn(move || {
            let t = AdbTransport::new(None);
            match kbdk_core::procs::kill(&t, pid, sig) {
                Ok(()) => { let _ = tx.send(Msg::Killed { pid }); }
                Err(e) => { let _ = tx.send(Msg::OpError { context: format!("kill {pid}"), message: e.to_string() }); }
            }
            ctx.request_repaint();
        });
    }
```

- [ ] **Step 3: Add app state**

In `crates/kbdk-ui/src/app.rs`, add to `struct KbdkApp` (after the deploy state block):

```rust
    // tasks tab
    pub procs: Vec<kbdk_core::procs::Proc>,
    pub tasks_status: String,
    pub kill_confirm: Option<(u32, String)>, // (pid, cmd) awaiting confirm
```

In `KbdkApp::new`, add to the struct initializer (near the other defaults):

```rust
            procs: vec![],
            tasks_status: String::new(),
            kill_confirm: None,
```

- [ ] **Step 4: Handle the messages in pump()**

In `crates/kbdk-ui/src/app.rs`, add arms to the `pump()` match (before the closing `}` of the match):

```rust
                Msg::ProcList(v) => {
                    self.procs = v;
                    self.tasks_status = format!("{} processes", self.procs.len());
                }
                Msg::Killed { pid } => {
                    self.tasks_status = format!("killed {pid}");
                    self.procs.retain(|p| p.pid != pid);
                }
                Msg::OpError { context, message } => {
                    self.tasks_status = format!("{context}: {message}");
                    self.files_status_set(format!("{context}: {message}"));
                }
```

Note: `files_status_set` is added in Task 6. For now, to keep this task building on its own, **temporarily** make the `OpError` arm set only `tasks_status`:

```rust
                Msg::OpError { context, message } => {
                    self.tasks_status = format!("{context}: {message}");
                }
```

(Task 6 expands this arm to also surface the error in the Files tab.)

- [ ] **Step 5: Implement the Tasks tab UI**

Replace the body of `crates/kbdk-ui/src/tasks_tab.rs`:

```rust
//! Tasks tab: board process list + kill.

use crate::app::KbdkApp;
use crate::theme;
use eframe::egui;

pub fn show(app: &mut KbdkApp, ui: &mut egui::Ui) {
    ui.horizontal(|ui| {
        ui.heading("Board processes");
        if ui.button("⟳ refresh").clicked() {
            app.workers.list_procs();
        }
        ui.label(egui::RichText::new(&app.tasks_status).color(theme::SUBTEXT));
    });
    ui.separator();

    // confirm dialog for kill -9
    if let Some((pid, cmd)) = app.kill_confirm.clone() {
        egui::Window::new("Force kill?")
            .collapsible(false)
            .resizable(false)
            .show(ui.ctx(), |ui| {
                ui.label(format!("Send SIGKILL (-9) to pid {pid}?"));
                ui.label(egui::RichText::new(&cmd).color(theme::SUBTEXT));
                ui.horizontal(|ui| {
                    if ui.button("Kill -9").clicked() {
                        app.workers.kill_proc(pid, 9);
                        app.kill_confirm = None;
                    }
                    if ui.button("Cancel").clicked() {
                        app.kill_confirm = None;
                    }
                });
            });
    }

    egui::ScrollArea::vertical().show(ui, |ui| {
        egui::Grid::new("procs")
            .striped(true)
            .num_columns(5)
            .show(ui, |ui| {
                ui.strong("PID");
                ui.strong("RSS");
                ui.strong("ST");
                ui.strong("COMMAND");
                ui.strong("");
                ui.end_row();

                for p in app.procs.clone() {
                    ui.label(p.pid.to_string());
                    ui.label(format!("{:.1}M", p.rss_kb as f64 / 1024.0));
                    ui.label(&p.state);
                    ui.label(egui::RichText::new(&p.cmd).color(theme::TEXT));
                    ui.horizontal(|ui| {
                        if ui.small_button("kill").clicked() {
                            app.workers.kill_proc(p.pid, 15); // SIGTERM
                        }
                        if ui
                            .small_button(egui::RichText::new("-9").color(theme::RED))
                            .clicked()
                        {
                            app.kill_confirm = Some((p.pid, p.cmd.clone()));
                        }
                    });
                    ui.end_row();
                }
            });
    });

    if app.procs.is_empty() {
        ui.label(egui::RichText::new("press ⟳ refresh to list board processes").color(theme::SUBTEXT));
    }
}
```

- [ ] **Step 6: Build and manually verify**

Run: `cargo build -p kbdk-ui 2>&1 | tail -5`
Expected: builds clean.

With a board attached, run `cargo run -p kbdk-ui`, open **Tasks**, click **⟳ refresh**: the process grid populates (kbrun/adbd/init visible), heaviest RSS first. `kill` sends SIGTERM; `-9` opens the confirm window. Without a board, the status line shows the `list processes: …` error.

- [ ] **Step 7: Commit**

```bash
git add crates/kbdk-ui/src/workers.rs crates/kbdk-ui/src/app.rs crates/kbdk-ui/src/tasks_tab.rs
git commit -m "kbdk-ui: Tasks tab — board process list + kill"
```

---

## Task 6: Files tab — dual-pane browsing (local + board)

**Files:**
- Modify: `crates/kbdk-ui/src/workers.rs`
- Modify: `crates/kbdk-ui/src/app.rs` (Fields paths, state, pump, init)
- Modify: `crates/kbdk-ui/src/files_tab.rs`

**Interfaces:**
- Consumes: `kbdk_core::fs::{list_dir, join_path, DirEntry}`, `AdbTransport`.
- Produces:
  - `Msg::DirListed { path: String, entries: Vec<kbdk_core::fs::DirEntry> }`
  - `Workers::list_dir(&self, path: String)`
  - `KbdkApp::files: files_tab::FilesState`, `KbdkApp::files_status_set(&mut self, s: String)`
  - `files_tab::FilesState`, `files_tab::FileTree`, `files_tab::show`
  - `Fields::last_local_path: String`, `Fields::last_board_path: String`

- [ ] **Step 1: Add persisted path fields**

In `crates/kbdk-ui/src/app.rs`, add default fns near `default_runtime`:

```rust
fn default_local_path() -> String {
    ".".into()
}
fn default_board_path() -> String {
    "/tmp".into()
}
```

Add to `struct Fields`:

```rust
    #[serde(default = "default_local_path")]
    pub last_local_path: String,
    #[serde(default = "default_board_path")]
    pub last_board_path: String,
```

Add to `impl Default for Fields`:

```rust
            last_local_path: default_local_path(),
            last_board_path: default_board_path(),
```

- [ ] **Step 2: Add the DirListed message + worker**

In `crates/kbdk-ui/src/workers.rs`, add to `enum Msg`:

```rust
    DirListed { path: String, entries: Vec<kbdk_core::fs::DirEntry> },
```

Add to `impl Workers`:

```rust
    pub fn list_dir(&self, path: String) {
        let tx = self.tx.clone();
        let ctx = self.ctx.clone();
        std::thread::spawn(move || {
            let t = AdbTransport::new(None);
            match kbdk_core::fs::list_dir(&t, &path) {
                Ok(entries) => { let _ = tx.send(Msg::DirListed { path, entries }); }
                Err(e) => { let _ = tx.send(Msg::OpError { context: format!("ls {path}"), message: e.to_string() }); }
            }
            ctx.request_repaint();
        });
    }
```

- [ ] **Step 3: Define the Files state and tree model**

At the top of `crates/kbdk-ui/src/files_tab.rs`, replace the stub with the state model and a helper to read local dirs:

```rust
//! Files tab: dual-pane local <-> board file transfer + management.

use crate::app::KbdkApp;
use crate::theme;
use eframe::egui;
use kbdk_core::fs::DirEntry;
use std::collections::{BTreeSet, HashMap};

/// One side of the dual pane. `children` is a lazy cache: a dir's entries are
/// fetched (board) or read (local) the first time it's expanded.
pub struct FileTree {
    pub root: String,
    pub expanded: BTreeSet<String>,
    pub children: HashMap<String, Vec<DirEntry>>,
    pub selected: Option<String>, // full path of the selected file
}

impl FileTree {
    fn new(root: String) -> Self {
        Self {
            root,
            expanded: BTreeSet::new(),
            children: HashMap::new(),
            selected: None,
        }
    }
}

pub struct FilesState {
    pub local: FileTree,
    pub board: FileTree,
    pub status: String,
}

impl FilesState {
    pub fn new(local_root: String, board_root: String) -> Self {
        Self {
            local: FileTree::new(local_root),
            board: FileTree::new(board_root),
            status: String::new(),
        }
    }
}

/// Read a host directory into DirEntry rows (mode left blank — unused locally).
pub fn read_local_dir(path: &str) -> Vec<DirEntry> {
    let mut v = Vec::new();
    if let Ok(rd) = std::fs::read_dir(path) {
        for e in rd.filter_map(|e| e.ok()) {
            let md = e.metadata().ok();
            let is_dir = md.as_ref().map(|m| m.is_dir()).unwrap_or(false);
            v.push(DirEntry {
                name: e.file_name().to_string_lossy().into_owned(),
                is_dir,
                size: md.as_ref().map(|m| m.len()).unwrap_or(0),
                mode: String::new(),
            });
        }
    }
    v.sort_by(|a, b| (b.is_dir, &a.name).cmp(&(a.is_dir, &b.name)));
    v
}

/// Join for the LOCAL side (host path separators).
fn local_join(parent: &str, name: &str) -> String {
    std::path::Path::new(parent).join(name).to_string_lossy().into_owned()
}
```

- [ ] **Step 4: Add Files state to the app + a status setter + init**

In `crates/kbdk-ui/src/app.rs`, add to `struct KbdkApp`:

```rust
    // files tab
    pub files: files_tab::FilesState,
```

Add a method in `impl KbdkApp` (near `rescan_packs`):

```rust
    pub fn files_status_set(&mut self, s: String) {
        self.files.status = s;
    }
```

In `KbdkApp::new`, build the state from the persisted paths. Add this **before** the `let mut app = Self {` initializer:

```rust
        let files = files_tab::FilesState::new(f.last_local_path.clone(), f.last_board_path.clone());
```

and add to the struct initializer:

```rust
            files,
```

- [ ] **Step 5: Expand the OpError arm to surface in Files too**

In `crates/kbdk-ui/src/app.rs`, replace the temporary `Msg::OpError` arm from Task 5 with:

```rust
                Msg::OpError { context, message } => {
                    let msg = format!("{context}: {message}");
                    self.tasks_status = msg.clone();
                    self.files.status = msg;
                }
```

Add the `DirListed` arm to `pump()`:

```rust
                Msg::DirListed { path, entries } => {
                    self.files.board.children.insert(path, entries);
                }
```

- [ ] **Step 6: Persist the current paths on save**

In `crates/kbdk-ui/src/app.rs`, in `eframe::App::save`, before `eframe::set_value`:

```rust
        self.f.last_local_path = self.files.local.root.clone();
        self.f.last_board_path = self.files.board.root.clone();
```

- [ ] **Step 7: Implement dual-pane browsing UI**

Replace `files_tab::show` (append below the state model in `files_tab.rs`):

```rust
pub fn show(app: &mut KbdkApp, ui: &mut egui::Ui) {
    ui.horizontal(|ui| {
        ui.heading("Files");
        ui.label(egui::RichText::new(&app.files.status).color(theme::SUBTEXT));
    });
    ui.separator();

    let avail_h = ui.available_height() - 8.0;
    ui.horizontal_top(|ui| {
        let pane_w = (ui.available_width() - 16.0) / 2.0;

        // LEFT: local
        ui.allocate_ui(egui::vec2(pane_w, avail_h), |ui| {
            ui.group(|ui| {
                ui.label(egui::RichText::new(format!("LOCAL  {}", app.files.local.root)).strong());
                egui::ScrollArea::vertical()
                    .id_salt("local_tree")
                    .auto_shrink([false, false])
                    .show(ui, |ui| {
                        let root = app.files.local.root.clone();
                        local_node(ui, app, &root);
                    });
            });
        });

        // RIGHT: board
        ui.allocate_ui(egui::vec2(pane_w, avail_h), |ui| {
            ui.group(|ui| {
                ui.horizontal(|ui| {
                    ui.label(egui::RichText::new(format!("BOARD  {}", app.files.board.root)).strong());
                    if ui.small_button("⟳").clicked() {
                        let root = app.files.board.root.clone();
                        app.files.board.children.remove(&root);
                        app.workers.list_dir(root);
                    }
                });
                egui::ScrollArea::vertical()
                    .id_salt("board_tree")
                    .auto_shrink([false, false])
                    .show(ui, |ui| {
                        let root = app.files.board.root.clone();
                        // fetch root once
                        if !app.files.board.children.contains_key(&root) {
                            app.workers.list_dir(root.clone());
                            app.files.board.children.insert(root.clone(), vec![]);
                        }
                        board_node(ui, app, &root);
                    });
            });
        });
    });
}

/// Render the LOCAL subtree rooted at `path` (read synchronously on expand).
fn local_node(ui: &mut egui::Ui, app: &mut KbdkApp, path: &str) {
    if !app.files.local.children.contains_key(path) {
        let entries = read_local_dir(path);
        app.files.local.children.insert(path.to_string(), entries);
    }
    let entries = app.files.local.children.get(path).cloned().unwrap_or_default();
    for e in entries {
        let child = local_join(path, &e.name);
        row(ui, app, true, &child, &e);
    }
}

/// Render the BOARD subtree rooted at `path` (children arrive via DirListed).
fn board_node(ui: &mut egui::Ui, app: &mut KbdkApp, path: &str) {
    let entries = app.files.board.children.get(path).cloned().unwrap_or_default();
    for e in entries {
        let child = kbdk_core::fs::join_path(path, &e.name);
        row(ui, app, false, &child, &e);
    }
}

/// One row: a clickable disclosure for dirs, a selectable label for files.
fn row(ui: &mut egui::Ui, app: &mut KbdkApp, is_local: bool, full: &str, e: &DirEntry) {
    let tree = if is_local { &mut app.files.local } else { &mut app.files.board };
    if e.is_dir {
        let open = tree.expanded.contains(full);
        let arrow = if open { "▾" } else { "▸" };
        if ui.selectable_label(false, format!("{arrow} 📁 {}", e.name)).clicked() {
            if open {
                tree.expanded.remove(full);
            } else {
                tree.expanded.insert(full.to_string());
                // board dirs need a fetch the first time
                if !is_local && !app.files.board.children.contains_key(full) {
                    app.files.board.children.insert(full.to_string(), vec![]);
                    app.workers.list_dir(full.to_string());
                }
            }
        }
        // recurse if expanded (re-borrow because the closure above took &mut)
        let open_now = if is_local {
            app.files.local.expanded.contains(full)
        } else {
            app.files.board.expanded.contains(full)
        };
        if open_now {
            ui.indent(full, |ui| {
                if is_local {
                    local_node(ui, app, full);
                } else {
                    board_node(ui, app, full);
                }
            });
        }
    } else {
        let selected = if is_local {
            app.files.local.selected.as_deref() == Some(full)
        } else {
            app.files.board.selected.as_deref() == Some(full)
        };
        let label = format!("   📄 {}  {}", e.name, human_size(e.size));
        if ui.selectable_label(selected, label).clicked() {
            if is_local {
                app.files.local.selected = Some(full.to_string());
            } else {
                app.files.board.selected = Some(full.to_string());
            }
        }
    }
}

fn human_size(n: u64) -> String {
    if n >= 1 << 20 {
        format!("{:.1}M", n as f64 / (1 << 20) as f64)
    } else if n >= 1 << 10 {
        format!("{:.1}K", n as f64 / (1 << 10) as f64)
    } else {
        format!("{n}B")
    }
}
```

- [ ] **Step 8: Build and manually verify**

Run: `cargo build -p kbdk-ui 2>&1 | tail -15`
Expected: builds clean.

Run `cargo run -p kbdk-ui`, open **Files**. Left pane shows the repo dir; expanding folders reveals contents. With a board attached, the right pane lists `/tmp`; expanding board folders fetches lazily (one `ls` per dir). Clicking a file highlights it. `⟳` re-fetches the board root.

- [ ] **Step 9: Commit**

```bash
git add crates/kbdk-ui/src/workers.rs crates/kbdk-ui/src/app.rs crates/kbdk-ui/src/files_tab.rs
git commit -m "kbdk-ui: Files tab — dual-pane local/board browsing"
```

---

## Task 7: Files tab — transfer + rm/chmod/mkdir

**Files:**
- Modify: `crates/kbdk-ui/src/workers.rs`
- Modify: `crates/kbdk-ui/src/app.rs` (pump)
- Modify: `crates/kbdk-ui/src/files_tab.rs`

**Interfaces:**
- Consumes: `kbdk_core::fs::{remove, chmod, mkdir, join_path}`, `Transport::{push, pull}`, `AdbTransport`.
- Produces:
  - `Msg::FileOpDone { context: String, refresh_board: Option<String> }`
  - `Workers::push_file`, `Workers::pull_file`, `Workers::fs_remove`, `Workers::fs_chmod`, `Workers::fs_mkdir`
  - `FilesState::dialog: Option<FsDialog>`, enum `FsDialog`

- [ ] **Step 1: Add the op-done message + workers**

In `crates/kbdk-ui/src/workers.rs`, add to `enum Msg`:

```rust
    /// A file op finished; if `refresh_board` is set, re-list that board dir.
    FileOpDone { context: String, refresh_board: Option<String> },
```

Add to `impl Workers` (each clones what it needs, runs the kbdk-core call, reports):

```rust
    pub fn push_file(&self, local: std::path::PathBuf, remote_dir: String) {
        let tx = self.tx.clone();
        let ctx = self.ctx.clone();
        std::thread::spawn(move || {
            let t = AdbTransport::new(None);
            let name = local.file_name().map(|s| s.to_string_lossy().into_owned()).unwrap_or_default();
            let remote = kbdk_core::fs::join_path(&remote_dir, &name);
            let msg = match t.push(&local, &remote) {
                Ok(()) => Msg::FileOpDone { context: format!("pushed {name}"), refresh_board: Some(remote_dir) },
                Err(e) => Msg::OpError { context: format!("push {name}"), message: e.to_string() },
            };
            let _ = tx.send(msg);
            ctx.request_repaint();
        });
    }

    pub fn pull_file(&self, remote: String, local_dir: std::path::PathBuf) {
        let tx = self.tx.clone();
        let ctx = self.ctx.clone();
        std::thread::spawn(move || {
            let t = AdbTransport::new(None);
            let name = remote.rsplit('/').next().unwrap_or("file").to_string();
            let dest = local_dir.join(&name);
            let msg = match t.pull(&remote, &dest) {
                Ok(()) => Msg::FileOpDone { context: format!("pulled {name}"), refresh_board: None },
                Err(e) => Msg::OpError { context: format!("pull {name}"), message: e.to_string() },
            };
            let _ = tx.send(msg);
            ctx.request_repaint();
        });
    }

    pub fn fs_remove(&self, path: String, is_dir: bool, parent: String) {
        let tx = self.tx.clone();
        let ctx = self.ctx.clone();
        std::thread::spawn(move || {
            let t = AdbTransport::new(None);
            let msg = match kbdk_core::fs::remove(&t, &path, is_dir) {
                Ok(()) => Msg::FileOpDone { context: format!("removed {path}"), refresh_board: Some(parent) },
                Err(e) => Msg::OpError { context: format!("rm {path}"), message: e.to_string() },
            };
            let _ = tx.send(msg);
            ctx.request_repaint();
        });
    }

    pub fn fs_chmod(&self, path: String, mode: String, parent: String) {
        let tx = self.tx.clone();
        let ctx = self.ctx.clone();
        std::thread::spawn(move || {
            let t = AdbTransport::new(None);
            let msg = match kbdk_core::fs::chmod(&t, &path, &mode) {
                Ok(()) => Msg::FileOpDone { context: format!("chmod {mode} {path}"), refresh_board: Some(parent) },
                Err(e) => Msg::OpError { context: format!("chmod {path}"), message: e.to_string() },
            };
            let _ = tx.send(msg);
            ctx.request_repaint();
        });
    }

    pub fn fs_mkdir(&self, path: String, parent: String) {
        let tx = self.tx.clone();
        let ctx = self.ctx.clone();
        std::thread::spawn(move || {
            let t = AdbTransport::new(None);
            let msg = match kbdk_core::fs::mkdir(&t, &path) {
                Ok(()) => Msg::FileOpDone { context: format!("mkdir {path}"), refresh_board: Some(parent) },
                Err(e) => Msg::OpError { context: format!("mkdir {path}"), message: e.to_string() },
            };
            let _ = tx.send(msg);
            ctx.request_repaint();
        });
    }
```

- [ ] **Step 2: Handle FileOpDone in pump()**

In `crates/kbdk-ui/src/app.rs`, add to the `pump()` match:

```rust
                Msg::FileOpDone { context, refresh_board } => {
                    self.files.status = context;
                    if let Some(dir) = refresh_board {
                        self.files.board.children.remove(&dir);
                        self.workers.list_dir(dir);
                    }
                }
```

- [ ] **Step 3: Add the dialog enum to FilesState**

In `crates/kbdk-ui/src/files_tab.rs`, add near the top (below the `use` lines):

```rust
/// Modal state for the rm/chmod/mkdir actions.
pub enum FsDialog {
    ConfirmRm { path: String, is_dir: bool, parent: String },
    Chmod { path: String, parent: String, mode: String },
    Mkdir { parent: String, name: String },
}
```

Add a `dialog` field to `FilesState`:

```rust
pub struct FilesState {
    pub local: FileTree,
    pub board: FileTree,
    pub status: String,
    pub dialog: Option<FsDialog>,
}
```

and initialize it in `FilesState::new`:

```rust
            dialog: None,
```

- [ ] **Step 4: Add the transfer buttons between panes**

In `files_tab::show`, insert a center column of transfer buttons between the LEFT and RIGHT panes. Replace the `ui.horizontal_top(|ui| {` body's pane layout so the panes are narrower and a button column sits between them — add this block **after** the LEFT pane `ui.allocate_ui(...)` and **before** the RIGHT pane:

```rust
        // CENTER: transfer + board-dir actions
        ui.vertical(|ui| {
            ui.add_space(40.0);
            let local_sel = app.files.local.selected.clone();
            let board_dir = current_board_dir(app);
            if ui.add_enabled(local_sel.is_some(), egui::Button::new("push →")).clicked() {
                if let Some(p) = local_sel {
                    app.workers.push_file(std::path::PathBuf::from(p), board_dir.clone());
                }
            }
            let board_sel = app.files.board.selected.clone();
            let local_dir = current_local_dir(app);
            if ui.add_enabled(board_sel.is_some(), egui::Button::new("← pull")).clicked() {
                if let Some(p) = board_sel {
                    app.workers.pull_file(p, std::path::PathBuf::from(local_dir));
                }
            }
            ui.separator();
            if ui.button("＋ mkdir").clicked() {
                app.files.dialog = Some(FsDialog::Mkdir { parent: board_dir.clone(), name: String::new() });
            }
        });
```

Add these helpers at the bottom of `files_tab.rs` (the "current dir" = the selected file's parent, or the pane root):

```rust
/// Board directory to act on: the selected board file's parent, else board root.
fn current_board_dir(app: &KbdkApp) -> String {
    match &app.files.board.selected {
        Some(p) => p.rsplit_once('/').map(|(d, _)| if d.is_empty() { "/".into() } else { d.to_string() })
            .unwrap_or_else(|| app.files.board.root.clone()),
        None => app.files.board.root.clone(),
    }
}

/// Local directory to pull into: the selected local file's parent, else root.
fn current_local_dir(app: &KbdkApp) -> String {
    match &app.files.local.selected {
        Some(p) => std::path::Path::new(p).parent()
            .map(|d| d.to_string_lossy().into_owned())
            .unwrap_or_else(|| app.files.local.root.clone()),
        None => app.files.local.root.clone(),
    }
}
```

- [ ] **Step 5: Add a right-click context menu on board files**

In `files_tab::row`, for the **board file** branch (the `else` for non-dir, when `!is_local`), attach a context menu. Replace the file `else` branch in `row` with:

```rust
    } else {
        let selected = if is_local {
            app.files.local.selected.as_deref() == Some(full)
        } else {
            app.files.board.selected.as_deref() == Some(full)
        };
        let label = format!("   📄 {}  {}", e.name, human_size(e.size));
        let resp = ui.selectable_label(selected, label);
        if resp.clicked() {
            if is_local {
                app.files.local.selected = Some(full.to_string());
            } else {
                app.files.board.selected = Some(full.to_string());
            }
        }
        if !is_local {
            let parent = full.rsplit_once('/').map(|(d, _)| if d.is_empty() { "/".to_string() } else { d.to_string() }).unwrap_or_else(|| "/".to_string());
            resp.context_menu(|ui| {
                if ui.button("pull").clicked() {
                    let dir = current_local_dir(app);
                    app.workers.pull_file(full.to_string(), std::path::PathBuf::from(dir));
                    ui.close_menu();
                }
                if ui.button("chmod…").clicked() {
                    app.files.dialog = Some(FsDialog::Chmod { path: full.to_string(), parent: parent.clone(), mode: "755".into() });
                    ui.close_menu();
                }
                if ui.button("rm").clicked() {
                    app.files.dialog = Some(FsDialog::ConfirmRm { path: full.to_string(), is_dir: false, parent: parent.clone() });
                    ui.close_menu();
                }
            });
        }
    }
```

- [ ] **Step 6: Render the dialogs**

Add a dialog renderer call at the end of `files_tab::show` (before the function's closing brace):

```rust
    render_dialog(app, ui);
```

Add the function at the bottom of `files_tab.rs`:

```rust
fn render_dialog(app: &mut KbdkApp, ui: &mut egui::Ui) {
    let Some(dialog) = app.files.dialog.take() else { return };
    let mut keep: Option<FsDialog> = None;
    match dialog {
        FsDialog::ConfirmRm { path, is_dir, parent } => {
            egui::Window::new("Delete?").collapsible(false).resizable(false).show(ui.ctx(), |ui| {
                ui.label(format!("Remove {path} from the board?"));
                ui.horizontal(|ui| {
                    if ui.button(egui::RichText::new("Delete").color(theme::RED)).clicked() {
                        app.workers.fs_remove(path.clone(), is_dir, parent.clone());
                    } else if ui.button("Cancel").clicked() {
                        // drop
                    } else {
                        keep = Some(FsDialog::ConfirmRm { path: path.clone(), is_dir, parent: parent.clone() });
                    }
                });
            });
        }
        FsDialog::Chmod { path, parent, mut mode } => {
            egui::Window::new("chmod").collapsible(false).resizable(false).show(ui.ctx(), |ui| {
                ui.horizontal(|ui| {
                    ui.label("mode");
                    ui.text_edit_singleline(&mut mode);
                });
                ui.horizontal(|ui| {
                    if ui.button("Apply").clicked() {
                        app.workers.fs_chmod(path.clone(), mode.clone(), parent.clone());
                    } else if ui.button("Cancel").clicked() {
                        // drop
                    } else {
                        keep = Some(FsDialog::Chmod { path: path.clone(), parent: parent.clone(), mode: mode.clone() });
                    }
                });
            });
        }
        FsDialog::Mkdir { parent, mut name } => {
            egui::Window::new("New folder").collapsible(false).resizable(false).show(ui.ctx(), |ui| {
                ui.horizontal(|ui| {
                    ui.label("name");
                    ui.text_edit_singleline(&mut name);
                });
                ui.horizontal(|ui| {
                    if ui.button("Create").clicked() && !name.trim().is_empty() {
                        let path = kbdk_core::fs::join_path(&parent, name.trim());
                        app.workers.fs_mkdir(path, parent.clone());
                    } else if ui.button("Cancel").clicked() {
                        // drop
                    } else {
                        keep = Some(FsDialog::Mkdir { parent: parent.clone(), name: name.clone() });
                    }
                });
            });
        }
    }
    app.files.dialog = keep;
}
```

- [ ] **Step 7: Build and manually verify**

Run: `cargo build -p kbdk-ui 2>&1 | tail -15`
Expected: builds clean.

With a board attached: select a local file → **push →** copies it to the board dir (right pane refreshes, file appears). Right-click a board file → **pull** (lands next to the selected local file / local root), **chmod…** (enter `644`, applies), **rm** (confirm window, then it's gone). **＋ mkdir** creates a folder in the current board dir.

- [ ] **Step 8: Commit**

```bash
git add crates/kbdk-ui/src/workers.rs crates/kbdk-ui/src/app.rs crates/kbdk-ui/src/files_tab.rs
git commit -m "kbdk-ui: Files tab — push/pull + rm/chmod/mkdir with confirms"
```

---

## Task 8: Files tab — read-only preview

**Files:**
- Modify: `crates/kbdk-ui/src/workers.rs`
- Modify: `crates/kbdk-ui/src/app.rs` (pump)
- Modify: `crates/kbdk-ui/src/files_tab.rs`

**Interfaces:**
- Consumes: `kbdk_core::fs::{read_head, looks_binary}`, `AdbTransport`.
- Produces:
  - `Msg::PreviewLoaded { path: String, body: String, is_binary: bool }`
  - `Workers::preview_file(&self, path: String)`
  - `FilesState::preview: Option<(String, String, bool)>` (path, body, is_binary)

- [ ] **Step 1: Add the preview message + worker**

In `crates/kbdk-ui/src/workers.rs`, add to `enum Msg`:

```rust
    PreviewLoaded { path: String, body: String, is_binary: bool },
```

Add to `impl Workers` (fetches a bounded head, sniffs, renders hex if binary):

```rust
    pub fn preview_file(&self, path: String) {
        let tx = self.tx.clone();
        let ctx = self.ctx.clone();
        std::thread::spawn(move || {
            let t = AdbTransport::new(None);
            let local = std::env::temp_dir().join("kbdk_preview_ui.bin");
            let msg = match kbdk_core::fs::read_head(&t, &path, 64 * 1024, &local) {
                Ok(bytes) => {
                    let is_binary = kbdk_core::fs::looks_binary(&bytes);
                    let body = if is_binary { hex_dump(&bytes) } else { String::from_utf8_lossy(&bytes).into_owned() };
                    Msg::PreviewLoaded { path, body, is_binary }
                }
                Err(e) => Msg::OpError { context: format!("preview {path}"), message: e.to_string() },
            };
            let _ = tx.send(msg);
            ctx.request_repaint();
        });
    }
```

Add a free function in `workers.rs` (below the `impl Workers` block) for the hex render:

```rust
/// Classic 16-bytes-per-line hex dump for binary previews.
fn hex_dump(bytes: &[u8]) -> String {
    let mut out = String::new();
    for (i, chunk) in bytes.chunks(16).enumerate() {
        let hex: Vec<String> = chunk.iter().map(|b| format!("{b:02x}")).collect();
        let ascii: String = chunk.iter().map(|&b| if (0x20..0x7f).contains(&b) { b as char } else { '.' }).collect();
        out.push_str(&format!("{:08x}  {:<47}  {}\n", i * 16, hex.join(" "), ascii));
    }
    out
}
```

- [ ] **Step 2: Add preview state + pump arm**

In `crates/kbdk-ui/src/files_tab.rs`, add to `FilesState`:

```rust
    pub preview: Option<(String, String, bool)>, // (path, body, is_binary)
```

and init in `FilesState::new`:

```rust
            preview: None,
```

In `crates/kbdk-ui/src/app.rs`, add to `pump()`:

```rust
                Msg::PreviewLoaded { path, body, is_binary } => {
                    self.files.preview = Some((path, body, is_binary));
                }
```

- [ ] **Step 3: Trigger preview on board-file selection**

In `files_tab::row`, in the board-file branch where `resp.clicked()` sets the board selection, also request the preview. Update that block:

```rust
        if resp.clicked() {
            if is_local {
                app.files.local.selected = Some(full.to_string());
            } else {
                app.files.board.selected = Some(full.to_string());
                app.workers.preview_file(full.to_string());
            }
        }
```

- [ ] **Step 4: Render the preview region**

In `files_tab::show`, add a preview panel below the panes. Insert before the `render_dialog(app, ui);` call:

```rust
    if let Some((path, body, is_binary)) = app.files.preview.clone() {
        ui.separator();
        ui.horizontal(|ui| {
            ui.label(egui::RichText::new(format!("preview: {path}")).strong());
            ui.label(egui::RichText::new(if is_binary { "binary (hex)" } else { "text" }).color(theme::SUBTEXT));
            if ui.small_button("✕").clicked() {
                app.files.preview = None;
            }
        });
        egui::ScrollArea::vertical().id_salt("preview").max_height(180.0).show(ui, |ui| {
            ui.add(egui::Label::new(egui::RichText::new(&body).monospace()).wrap());
        });
    }
```

- [ ] **Step 5: Build and manually verify**

Run: `cargo build -p kbdk-ui 2>&1 | tail -15`
Expected: builds clean.

With a board attached: click a text file on the board (e.g. `/tmp/kbrun.log`) → the preview region shows its first 64 KB as text. Click a binary (e.g. `/tmp/kbrun`) → a hex dump renders. The ✕ closes the preview.

- [ ] **Step 6: Commit**

```bash
git add crates/kbdk-ui/src/workers.rs crates/kbdk-ui/src/app.rs crates/kbdk-ui/src/files_tab.rs
git commit -m "kbdk-ui: Files tab — read-only text/hex preview"
```

---

## Task 9: Workspace check + docs

**Files:**
- Modify: `CLAUDE.md` (kbdk-ui notes), `README.md` (kbdk-ui GUI paragraph)

- [ ] **Step 1: Full workspace build + test**

Run: `cargo build 2>&1 | tail -5 && cargo test -p kbdk-core 2>&1 | tail -15`
Expected: workspace builds; all `kbdk-core` unit tests pass (including the new `fs::` and `procs::` tests).

- [ ] **Step 2: Update CLAUDE.md**

In `CLAUDE.md`, in the `kbdk-ui notes` area, add a sentence documenting the new tabs (match the surrounding prose style):

> The UI also has **Files** and **Tasks** tabs: Files is a dual-pane local↔board
> transfer manager (lazy `ls -la` per board dir, push/pull via the md5-verified
> Transport, rm/chmod/mkdir with confirms, read-only text/hex preview via a
> bounded `head -c`+pull); Tasks lists board processes from `/proc` (RSS-sorted)
> with SIGTERM/SIGKILL. Both run all board I/O on worker threads and refresh
> manually (no background poll — the single A7 competes with kbrun). Helpers live
> in `kbdk-core::{fs,procs}` with hardware-free parser unit tests.

- [ ] **Step 3: Update README.md**

In `README.md`, in the kbdk-ui GUI paragraph, add Files/Tasks to the tab list (one clause, matching the existing sentence about tabs).

- [ ] **Step 4: Commit**

```bash
git add CLAUDE.md README.md
git commit -m "docs: kbdk-ui Files & Tasks tabs"
```

---

## Self-Review

**Spec coverage** (against `2026-06-21-kbdk-ui-files-tasks-design.md`):
- Dual-pane local↔board transfer view → Task 6 (browsing) + Task 7 (transfer). ✓
- push/pull/rm/chmod/mkdir → Task 7. ✓
- Read-only preview (text/hex via content sniff, bounded fetch) → Task 1 (`read_head`, `looks_binary`) + Task 8. ✓
- Task manager: process list (PID, RSS, state, command) + kill, manual refresh → Task 2 + Task 5. ✓
- New `kbdk-core` module(s) with hardware-free parser tests → Task 1 (`fs.rs`), Task 2 (`procs.rs`). ✓
- All board I/O on worker threads, UI never blocks → every UI worker spawns a thread (Tasks 5–8); local FS reads sync (Task 6). ✓
- Prefer AdbTransport → all workers use `AdbTransport::new(None)` (Global Constraints note serial fallback as non-goal, matching the existing app). ✓
- rm + kill -9 confirmations → Task 5 (kill -9 window), Task 7 (rm window). ✓
- Two flat tabs, no nav reshell → Task 4. ✓
- Persisted last paths → Task 6 (`Fields::last_local_path`/`last_board_path`). ✓
- KBDK_HW integration test → Task 3. ✓
- Manual refresh / no background poll → Tasks tab `⟳` button only; Files fetch on expand/refresh only. ✓

**Placeholder scan:** No TBD/TODO; every code step shows complete code. The Task 5 `OpError` arm is intentionally written twice (a minimal form so Task 5 stands alone, then expanded in Task 6 Step 5) and this is called out explicitly — not a placeholder.

**Type consistency:** `DirEntry`/`Proc` fields, `join_path`, `read_head(t, path, max_bytes, local)`, `remove(t, path, is_dir)`, `kill(t, pid, sig)`, and the `Msg` variant shapes (`DirListed`, `ProcList`, `Killed`, `OpError`, `FileOpDone`, `PreviewLoaded`) are used identically across producer and consumer tasks. `FilesState` grows across Tasks 6→7→8 (`dialog`, then `preview`) with each addition shown where introduced.

**Note for the implementer (egui 0.34 API drift):** This codebase pins egui 0.34. If `ScrollArea::id_salt`, `Response::context_menu`/`ui.close_menu`, or `Label::wrap` differ in the exact version, mirror the equivalent call already used in `deploy_tab.rs`/`convert_tab.rs` rather than guessing — the existing tabs are the source of truth for widget API.
