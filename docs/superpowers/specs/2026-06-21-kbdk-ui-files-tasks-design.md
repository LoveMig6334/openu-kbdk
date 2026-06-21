# kbdk-ui — Files & Tasks tabs (Board IDE, build 1)

Date: 2026-06-21
Status: approved (design), ready for implementation plan

## Context

`kbdk-ui` today is the ML pipeline app: `Train | Convert | Deploy` tabs, an egui
desktop frontend over `kbdk-core`. The agreed direction is to grow it into a **full
board IDE** for the KidBright µAI — edit C, build via the cross-toolchain, deploy over
ADB, browse files, manage processes, see live hardware — with the ML pipeline becoming
one area among many.

That full vision is decomposed into independently-buildable pieces, each with its own
spec and plan:

1. **Hardware panel** — live board inventory (read-only). *independent*
2. **File tree manager** — board ↔ host file transfer + management. *independent*
3. **Task manager** — board process list + kill. *independent*
4. **Code editor + build/deploy** — the spine (edit C → cross-compile → deploy → run).
5. **Example code** — loadable templates (camera open; LED blink, which needs a new
   GPIO board C program written).

The IDE navigation reshell (collapse `Train/Convert/Deploy` under an `ML ▾` menu, add a
top-level area bar) is **deferred** to a later build, once enough dev tabs exist to make
the layout decisions worthwhile.

**This document specs build 1: the File tree manager and the Task manager**, delivered
together because they share the same "run a board command → parse → render" pattern and
the same worker plumbing.

## Goals

- A **Files** tab: dual-pane transfer view (local project ↔ board FS) with push/pull,
  rm/chmod/mkdir, and a read-only file preview.
- A **Tasks** tab: board process list (PID, RSS, state, command) with per-process kill.
- All board I/O on worker threads; the UI thread never blocks on board I/O.
- New board-FS / process helpers in `kbdk-core` with hardware-free unit tests for every
  output parser.

## Non-goals (explicitly out of scope for this build)

- The IDE navigation reshell (`ML ▾` menu). Files and Tasks are added as flat tabs
  alongside the existing three: `Train | Convert | Deploy | Files | Tasks`.
- Any code editing / build / deploy of source (that is build 4).
- A general board shell / arbitrary command runner (the Tasks tab is monitor + kill
  only; "run a command" was considered and dropped).
- Background auto-refresh of the process list or file trees (manual refresh only — see
  Performance constraints).
- Recursive board-FS listing or search.

## Decisions (resolved during brainstorming)

| Question | Decision |
| --- | --- |
| Long-term role of kbdk-ui | Full board IDE (ML becomes one area) |
| First build | File tree manager + Task manager |
| File manager layout | **Dual-pane** transfer view (local ↔ board) |
| File preview | **Read-only** preview included (text, or hex for binaries) |
| Task manager scope | **Monitor + kill** only (no run/launch) |
| Nav change this build | **Add two flat tabs now**, reshell later |

## Architecture

### Transport reuse

Board I/O goes through the existing `kbdk-core` `Transport` trait. The app already
discovers ADB devices and serial ports (`discover.rs`); Files/Tasks **prefer
`AdbTransport`** when a device is present (fast, ~6 MB/s, `adb pull` available) and fall
back to `SerialTransport`. No new transport is introduced.

### New `kbdk-core` surface

A new module `fs.rs` (board filesystem ops) plus process helpers. Every function that
consumes board text output is split into `run + parse`, where `parse` is a pure function
unit-tested without hardware.

```
// board filesystem
pub struct DirEntry { pub name: String, pub is_dir: bool, pub size: u64, pub mode: String }
pub fn list_dir(t: &dyn Transport, path: &str) -> Result<Vec<DirEntry>>;   // one `ls -la <path>`, non-recursive
pub fn parse_ls(out: &str) -> Vec<DirEntry>;                               // busybox `ls -la` parser (pure)

pub fn read_head(t: &dyn Transport, path: &str, max_bytes: usize) -> Result<Vec<u8>>; // bounded fetch for preview
pub fn looks_binary(bytes: &[u8]) -> bool;                                 // text/binary sniff (pure)

pub fn remove(t: &dyn Transport, path: &str) -> Result<()>;               // rm / rm -rf (dirs)
pub fn chmod(t: &dyn Transport, path: &str, mode: &str) -> Result<()>;
pub fn mkdir(t: &dyn Transport, path: &str) -> Result<()>;
pub fn pull_file(t: &dyn Transport, remote: &str, local: &Path) -> Result<()>; // adb pull; serial = chunked read
// push_file reuses the existing md5-verified deploy push path.

// processes
pub struct Proc { pub pid: u32, pub rss_kb: u64, pub state: String, pub cmd: String }
pub fn list_procs(t: &dyn Transport) -> Result<Vec<Proc>>;                // batched /proc read (one exec)
pub fn parse_procs(out: &str) -> Vec<Proc>;                              // pure parser
pub fn kill(t: &dyn Transport, pid: u32, sig: i32) -> Result<()>;
```

**Listing** uses one `ls -la <dir>` per expanded directory (lazy), not a recursive
`find` — a full-tree walk would overrun the exec timeout and load the single A7.

**Process info**: busybox `ps` omits RSS, so processes are enumerated from `/proc`:
read `comm`/state from `/proc/<pid>/stat` and `VmRSS` from `/proc/<pid>/status`, batched
into **one** shell command to avoid per-process round-trips. `/proc` reads are already
proven by the Deploy tab's perf monitor.

**Pull**: `AdbTransport::pull_file` shells `adb pull` directly (fast, bypasses the slow
octal-printf path); the serial transport falls back to a chunked hex read. Pulls verify
size; pushes stay md5-verified (the UDISK vfat partition is flaky).

### UI: workers and messages

New `Msg` variants flow over the existing mpsc channel; new worker functions run the
`kbdk-core` calls off-thread:

- `DirListed { side, path, entries }`
- `PreviewLoaded { path, bytes, is_binary }`
- `Pulled { remote, local }` / `Pushed { local, remote }`
- `Removed { path }` / `Chmodded { path }` / `Mkdired { path }`
- `ProcList(Vec<Proc>)` / `Killed { pid }`
- `OpError { context, message }`

Local-FS listing (left pane) reads via `std::fs` and is fast enough to do synchronously
on the UI thread; only **board** operations go to workers.

### UI: tabs and state

- `app.rs`: extend `enum Tab` with `Files` and `Tasks`; route them; add their state.
- `Fields` gains persisted `last_local_path` and `last_board_path` (with `#[serde(default)]`
  so older storage still loads — matching the existing convention).
- `files_tab.rs`:
  - Two `egui` trees. Local tree built from `std::fs`; board tree lazily expands via
    `DirListed`. Each tracks an expanded-set and a single selection.
  - Center `push →` / `← pull` buttons operate on the selected file.
  - Right-click context menu: `rm`, `chmod`, `mkdir`, `pull`.
  - Clicking a file requests `read_head`; result renders in a read-only preview region
    (monospace text, or a hex dump when `looks_binary`).
  - A per-tab status line shows the last op result / error.
- `tasks_tab.rs`:
  - Table of `Proc` rows (PID, RSS, STAT, COMMAND) with a `⟳ refresh` button.
  - Per-row `✕` kill button.

## Error handling & safety

- `rm` and `kill -9` require a confirmation dialog before the worker is dispatched.
- Kill escalates: `kill <pid>` first; the row's next `✕` (or a follow-up) sends
  `kill -9`.
- All board ops report rc; failures surface in the tab's status line as `OpError`
  (no silent failures).
- Pushes md5-verified; pulls size-verified.
- Destructive operations never execute on the UI thread without confirmation.

## Performance constraints

- **Manual refresh only.** No background polling of the process list or file trees —
  the board is a single Cortex-A7 and adbd competes with any running kbrun. The user
  refreshes explicitly.
- Lazy directory expansion; bounded preview fetch (~64 KB cap).

## Testing

- **Unit (no hardware):** `parse_ls`, `parse_procs`, `looks_binary` — table-driven tests
  over captured busybox output samples.
- **Hardware integration:** behind the existing `KBDK_HW=1` gate in `kbdk-core` — list a
  known dir, read a small file head, list processes, push/pull a temp file round-trip.
- **UI:** verified manually (consistent with the rest of kbdk-ui).

## Open implementation questions (for the plan, not blockers)

- Exact busybox `ls -la` column layout on this rootfs (symlink `->` handling, device
  nodes) — confirm against a live `ls -la` sample and make `parse_ls` defensive.
- The single batched `/proc` shell command form (portable to this busybox `sh`).
- Whether `chmod`/`mkdir` get inline text inputs or small modal dialogs (UI detail).

## Follow-on builds (tracked, not specced here)

Hardware panel; Code editor + build/deploy; Example templates (incl. a new GPIO LED-blink
board program); and the IDE navigation reshell.
