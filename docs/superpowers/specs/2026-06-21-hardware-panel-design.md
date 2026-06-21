# kbdk-ui — Hardware panel (Board IDE, build 2)

Date: 2026-06-21
Status: approved (design), ready for implementation plan

## Context

`kbdk-ui` is growing into a board IDE for the KidBright µAI. Build 1 added the
**Files** and **Tasks** tabs (dual-pane file manager + process list) on top of two
new `kbdk-core` modules (`fs`, `procs`) that wrap board shell commands over the
`Transport` trait. This is **build 2**: a read-only **Hardware** tab — a live board
inventory — chosen as the next independent, low-risk increment.

The board's hardware facts are reachable from userspace shell (`/proc`, sysfs,
`uname`, `i2cdetect`, device-tree), exactly the `Transport::exec` pattern the Tasks
tab already established. This panel surfaces them in the UI instead of requiring
ad-hoc `kbdk exec` probing.

Remaining board-IDE features after this (tracked, not specced here): Code editor +
build/deploy, Example templates (incl. a new GPIO LED-blink board program), and the
IDE navigation reshell.

## Goals

- A new **Hardware** tab with two parts:
  1. A curated static/semi-static **inventory**, grouped into collapsing sections.
  2. A **live monitor** of dynamic stats (CPU load, free RAM, temperature, uptime)
     that auto-refreshes while the tab is open, including a free-RAM sparkline.
- New `kbdk-core::hwinfo` module with hardware-free parser unit tests.
- All board I/O on worker threads; the UI thread never blocks.

## Non-goals

- The IDE navigation reshell — Hardware is added as another flat tab
  (`Train | Convert | Deploy | Files | Tasks | Hardware`).
- Editing, writing, or changing any board configuration — the panel is strictly
  read-only.
- A full duplicate of the Deploy tab's performance monitor. The live section is a
  lightweight always-current readout (load/RAM/temp/uptime + one sparkline), not
  the Deploy tab's inference-latency / camera-fps plots. Partial overlap with the
  Deploy perf monitor is accepted by design.
- Background polling when the tab is not visible (see Performance constraints).
- Per-device deep decoding of the unidentified I²C parts (0x48/0x51/0x62) — they
  are listed as detected addresses with `no driver`, not probed further.

## Decisions (resolved during brainstorming)

| Question | Decision |
| --- | --- |
| Next feature after Files/Tasks | **Hardware panel** |
| Depth | Curated inventory **+ live monitor** (dynamic stats) |
| Live-poll cadence | ~2 s |
| Live-poll scope | **Only while the Hardware tab is visible** (no background drain) |
| Inventory layout | **Collapsing sections** of label/value rows |
| Static inventory refresh | Once on first open + manual **⟳ refresh** |

## Architecture

### Transport reuse

Board I/O goes through `kbdk_core::transport::Transport`; the UI constructs
`AdbTransport::new(None)` inside worker threads (consistent with every existing
worker). No new transport.

### New `kbdk-core` module: `hwinfo.rs`

Two batched shell commands, each paired with a pure parser unit-tested without
hardware. Every probe is defensive: a missing file or absent tool yields `n/a` for
that row and never fails the whole command (each sub-probe ends `|| echo n/a` style
and the command ends `; true`).

```rust
/// One section of the inventory: a titled group of (label, value) rows.
pub struct HwSection { pub title: String, pub rows: Vec<(String, String)> }
pub struct HwInfo { pub sections: Vec<HwSection> }

pub const STATIC_PROBE_CMD: &str;            // batched sysfs/proc/uname/i2c/dt probe
pub fn parse_hwinfo(out: &str) -> HwInfo;    // pure
pub fn probe_hwinfo(t: &dyn Transport) -> anyhow::Result<HwInfo>;

pub struct LiveStats {
    pub load1: f32,        // /proc/loadavg field 1
    pub mem_avail_kb: u64, // /proc/meminfo MemAvailable
    pub mem_total_kb: u64, // /proc/meminfo MemTotal
    pub temp_c: Option<f32>, // thermal_zone0/temp millidegrees -> °C; None if absent
    pub uptime_s: u64,     // /proc/uptime field 1
}
pub const LIVE_PROBE_CMD: &str;
pub fn parse_live(out: &str) -> Option<LiveStats>; // pure; None if core fields missing
pub fn probe_live(t: &dyn Transport) -> anyhow::Result<LiveStats>;
```

**Static probe** emits a delimited stream the parser splits into sections. The
batched command concatenates labelled fragments, e.g. a `SECTION <title>` marker
line then `KEY<TAB>VALUE` lines, so `parse_hwinfo` is a simple line scanner. Probes
and their sources:

- **SoC / CPU** — `/proc/cpuinfo` (model name / Hardware, CPU part, features),
  core count.
- **Memory** — `/proc/meminfo` `MemTotal` (the live free value lives in the live
  section).
- **Kernel** — `uname -a` (version, machine, hostname).
- **Display** — `/sys/class/graphics/fb0/virtual_size`, `/sys/class/graphics/fb0/modes`.
- **Camera + I²C** — `i2cdetect -y -r 1` (addresses on bus 1) plus the bound camera
  driver name from `/sys/bus/i2c/devices/1-003c/name` (or similar); list detected
  addresses, marking unbound ones `no driver`.
- **Storage** — `/proc/partitions` (mmcblk0 + partition sizes).
- **SPI** — presence of `/dev/spidev*`.
- **Audio** — `/proc/asound/cards`.
- **Network** — `/sys/class/net/*` interface names.
- **NPU** — device-tree `nna@*` node presence + status under `/proc/device-tree`
  (`tr -d '\0'` for the status string).

**Live probe** reads `/proc/loadavg`, `/proc/meminfo` (`MemAvailable`, `MemTotal`),
`/sys/class/thermal/thermal_zone0/temp` (millidegrees → °C; `n/a` if the path is
absent), and `/proc/uptime`.

### UI: `kbdk-ui/hardware_tab.rs`

- **Static inventory**: each `HwSection` rendered as an `egui::CollapsingHeader`
  (open by default) containing a label/value grid. A **⟳ refresh** button re-probes.
  Fetched once on first open (guard: only if `app.hw_info` is `None`).
- **Live** section: rows for CPU load, temperature, uptime, and free/total RAM, plus
  a **free-RAM sparkline** via `egui_plot` (same dependency the Deploy tab uses); a
  bounded ring of recent samples (~120 points).
- **Visible-only auto-poll**: `show()` checks a `last_live_poll: Instant`; if ≥ 2 s
  elapsed and no probe is in flight (`hw_live_inflight: bool`), it spawns
  `probe_hw_live()` and calls `ui.ctx().request_repaint_after(Duration::from_secs(2))`
  so the tab keeps ticking while visible. Because egui only runs the active tab's
  `show()`, polling stops automatically when the user switches tabs — no background
  thread lifecycle to manage.

### Workers & messages

New `Msg` variants over the existing mpsc channel:
- `Msg::HwInfo(kbdk_core::hwinfo::HwInfo)` — static inventory result.
- `Msg::HwLive(kbdk_core::hwinfo::LiveStats)` — one live sample.
- Errors reuse the existing `Msg::OpError { context, message }` (surfaced in the
  Hardware tab's status line).

New `Workers` methods `probe_hw(&self)` and `probe_hw_live(&self)` follow the
established pattern (clone tx/ctx, spawn thread, `AdbTransport::new(None)` inside,
send Msg, `request_repaint`).

### App state (`app.rs`)

- `Tab` gains a `Hardware` variant; `top_bar` adds the nav button; `ui()` routes it.
- `KbdkApp` gains: `hw_info: Option<HwInfo>`, `hw_status: String`,
  `hw_live: Option<LiveStats>`, `hw_mem_hist: Vec<[f64; 2]>` (sparkline),
  `hw_last_live_poll: Option<Instant>`, `hw_live_inflight: bool`.
- `pump()` handles `HwInfo` (store), `HwLive` (store + push capped to `hw_mem_hist`,
  clear the in-flight flag).

## Error handling & performance constraints

- Defensive probes: a missing file/tool yields `n/a`, never an error; the static
  command still returns rc 0 (`; true`). A transport-level failure surfaces via
  `OpError` in the Hardware status line.
- **No background polling**: the live poll runs only while the Hardware tab is the
  active tab. The in-flight guard prevents overlapping probes if the board is slow.
- Single batched exec per static refresh and per live sample — no per-row round
  trips (the single A7 competes with any running kbrun).

## Testing

- **Unit (no hardware):** `parse_hwinfo` (section/row extraction, `n/a` handling,
  malformed lines skipped) and `parse_live` (load/mem/temp/uptime parsing, missing
  temp → `None`, garbage → `None`) over captured board-output samples.
- **Hardware integration (`KBDK_HW`):** probe a real board — assert the inventory
  has the expected core sections (SoC, Memory, Kernel) and that a live sample parses
  with a plausible `mem_total_kb` and `load1`.
- **UI:** verified manually (consistent with the rest of kbdk-ui).

## Open implementation questions (for the plan, not blockers)

- Exact sysfs paths on this rootfs for the bound camera driver name and the NPU
  device-tree status — confirm against live probes and keep the parser defensive.
- Whether `i2cdetect` is present (CLAUDE.md says it was used to build the inventory,
  so assume yes) and its exact `-y -r 1` output columns.
- Sparkline: free-RAM only, or also CPU load — default to free-RAM only (one plot)
  unless trivial to add both.

## Follow-on builds (tracked, not specced here)

Code editor + build/deploy; Example templates (incl. a new GPIO LED-blink board
program); IDE navigation reshell.
