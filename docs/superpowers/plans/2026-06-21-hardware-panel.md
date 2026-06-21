# Hardware Panel Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add a read-only **Hardware** tab to kbdk-ui — a curated static board inventory (collapsing sections) plus a live monitor (CPU load / free RAM / temperature / uptime + free-RAM sparkline) that auto-polls only while the tab is visible.

**Architecture:** A new `kbdk-core::hwinfo` module wraps two batched board shell commands (static probe + live probe) over the existing `Transport` trait, each with a pure, unit-tested parser. A new `hardware_tab` module renders the result, dispatching all board I/O to worker threads via the existing `Workers`/`Msg` mpsc plumbing; the live poll is driven from `show()` (egui runs only the visible tab) so it stops when the user switches away.

**Tech Stack:** Rust, eframe/egui 0.34, egui_plot 0.35, `kbdk-core` (anyhow), board access via `AdbTransport` (adb shell over USB-OTG).

## Global Constraints

- **Transport:** board I/O goes through `kbdk_core::transport::Transport`; workers construct `kbdk_core::adb::AdbTransport::new(None)` inside the spawned thread (matches every existing worker).
- **Threading:** board I/O NEVER runs on the UI thread. Workers spawn a `std::thread`, send a `Msg` over `self.tx`, then call `ctx.request_repaint()`.
- **Exec signature:** `Transport::exec(&self, cmd: &str, timeout_secs: u64) -> Result<ExecResult>` where `ExecResult { output: String, rc: i32 }`.
- **Read-only:** the panel only reads board state — no writes, no config changes.
- **No background polling:** the live poll runs ONLY from the Hardware tab's `show()` (so only while that tab is visible); an in-flight guard prevents overlapping probes. Static inventory is fetched once on first open + on manual refresh.
- **Defensive probes:** every sub-probe yields `n/a` (or is skipped) when a file/tool is absent; the batched command ends `; true` so it always returns rc 0. Parsers tolerate malformed lines.
- **Single batched exec** per static refresh and per live sample — no per-row round trips (the single Cortex-A7 competes with any running kbrun).
- **egui:** pinned egui 0.34 + egui_plot 0.35. For the sparkline, mirror the existing `Plot`/`Line`/`PlotPoints` usage in `crates/kbdk-ui/src/deploy_tab.rs` (around lines 491–512). Use `theme::` color constants, never hardcoded `Color32`.

---

## File Structure

**Created:**
- `crates/kbdk-core/src/hwinfo.rs` — types (`HwSection`, `HwInfo`, `LiveStats`), batched probe commands, parsers (`parse_hwinfo`, `parse_live`), probe wrappers (`probe_hwinfo`, `probe_live`).
- `crates/kbdk-core/tests/hw_hwinfo.rs` — `KBDK_HW`-gated hardware smoke test.
- `crates/kbdk-ui/src/hardware_tab.rs` — Hardware tab UI.

**Modified:**
- `crates/kbdk-core/src/lib.rs` — register `pub mod hwinfo;`.
- `crates/kbdk-ui/src/main.rs` — declare `mod hardware_tab;`.
- `crates/kbdk-ui/src/workers.rs` — `Msg::HwInfo` / `Msg::HwLive` variants + `probe_hw` / `probe_hw_live` worker methods.
- `crates/kbdk-ui/src/app.rs` — `Tab::Hardware`, nav button, routing, Hardware state fields + initializers, `pump()` arms, extend the `OpError` arm.

---

## Task 1: kbdk-core `hwinfo.rs` — probes and parsers

**Files:**
- Create: `crates/kbdk-core/src/hwinfo.rs`
- Modify: `crates/kbdk-core/src/lib.rs`

**Interfaces:**
- Consumes: `crate::transport::Transport` (`exec`).
- Produces:
  - `pub struct HwSection { pub title: String, pub rows: Vec<(String, String)> }` (derives `Debug, Clone`)
  - `pub struct HwInfo { pub sections: Vec<HwSection> }` (derives `Debug, Clone`)
  - `pub struct LiveStats { pub load1: f32, pub mem_avail_kb: u64, pub mem_total_kb: u64, pub temp_c: Option<f32>, pub uptime_s: u64 }` (derives `Debug, Clone`)
  - `pub const STATIC_PROBE_CMD: &str`, `pub fn parse_hwinfo(out: &str) -> HwInfo`, `pub fn probe_hwinfo(t: &dyn Transport) -> anyhow::Result<HwInfo>`
  - `pub const LIVE_PROBE_CMD: &str`, `pub fn parse_live(out: &str) -> Option<LiveStats>`, `pub fn probe_live(t: &dyn Transport) -> anyhow::Result<LiveStats>`

- [ ] **Step 1: Register the module**

In `crates/kbdk-core/src/lib.rs`, add after `pub mod fs;`:

```rust
pub mod hwinfo;
```

- [ ] **Step 2: Write the failing parser tests**

Create `crates/kbdk-core/src/hwinfo.rs` with only the test module first:

```rust
#[cfg(test)]
mod tests {
    use super::*;

    const STATIC: &str = "\
SECTION SoC
Model\tAllwinner sun8iw19
Cores\t1
SECTION Memory
Total\t60048 kB
SECTION Display
fb0 size\t240,480
";

    #[test]
    fn parse_hwinfo_groups_sections_and_rows() {
        let info = parse_hwinfo(STATIC);
        assert_eq!(info.sections.len(), 3);
        assert_eq!(info.sections[0].title, "SoC");
        assert_eq!(info.sections[0].rows.len(), 2);
        assert_eq!(info.sections[0].rows[0], ("Model".into(), "Allwinner sun8iw19".into()));
        assert_eq!(info.sections[1].title, "Memory");
        assert_eq!(info.sections[1].rows[0], ("Total".into(), "60048 kB".into()));
        assert_eq!(info.sections[2].rows[0], ("fb0 size".into(), "240,480".into()));
    }

    #[test]
    fn parse_hwinfo_tolerates_junk() {
        // a key line before any SECTION is dropped; blank/garbage lines skipped
        let info = parse_hwinfo("stray\tvalue\n\nSECTION X\na\t1\nnotabhere\n");
        assert_eq!(info.sections.len(), 1);
        assert_eq!(info.sections[0].rows, vec![("a".to_string(), "1".to_string())]);
        assert!(parse_hwinfo("").sections.is_empty());
    }

    #[test]
    fn parse_live_reads_all_fields() {
        let s = parse_live("load 0.42\nmemavail 41000\nmemtotal 60048\ntemp 48200\nuptime 1234.56\n").unwrap();
        assert_eq!(s.load1, 0.42);
        assert_eq!(s.mem_avail_kb, 41000);
        assert_eq!(s.mem_total_kb, 60048);
        assert_eq!(s.temp_c, Some(48.2));
        assert_eq!(s.uptime_s, 1234);
    }

    #[test]
    fn parse_live_handles_missing_temp_and_garbage() {
        // temp "na" -> None, but the sample still parses
        let s = parse_live("load 0.1\nmemavail 1\nmemtotal 2\ntemp na\nuptime 5\n").unwrap();
        assert_eq!(s.temp_c, None);
        // missing a required field (no load) -> None
        assert!(parse_live("memavail 1\nmemtotal 2\n").is_none());
        assert!(parse_live("").is_none());
    }
}
```

- [ ] **Step 3: Run the tests to verify they fail**

Run: `cargo test -p kbdk-core hwinfo:: 2>&1 | head -30`
Expected: FAIL — `parse_hwinfo`, `parse_live`, `HwInfo`, `LiveStats` not found.

- [ ] **Step 4: Implement the module**

Prepend to `crates/kbdk-core/src/hwinfo.rs` (above the test module):

```rust
//! Read-only board hardware inventory + live stats over the Transport. Two
//! batched shell commands (static probe + live probe), each with a pure parser.
//! Probes are defensive: a missing file/tool yields `n/a` for that row and the
//! command ends `; true`, so the panel never errors on an unfamiliar rootfs.

use crate::transport::Transport;
use anyhow::{anyhow, bail, Result};

#[derive(Debug, Clone)]
pub struct HwSection {
    pub title: String,
    pub rows: Vec<(String, String)>,
}

#[derive(Debug, Clone)]
pub struct HwInfo {
    pub sections: Vec<HwSection>,
}

#[derive(Debug, Clone)]
pub struct LiveStats {
    pub load1: f32,
    pub mem_avail_kb: u64,
    pub mem_total_kb: u64,
    pub temp_c: Option<f32>, // None if no thermal sensor
    pub uptime_s: u64,
}

/// Static inventory probe. Emits `SECTION <title>` markers and `KEY<TAB>VALUE`
/// rows; the parser just groups them, so this command can grow/shrink freely.
/// (Raw-string fragments keep shell quoting readable; printf interprets the
/// literal `\t`/`\n`.)
pub const STATIC_PROBE_CMD: &str = concat!(
    r#"echo 'SECTION SoC'; "#,
    r#"printf 'Model\t%s\n' "$(awk -F: '/model name|Hardware/{print $2; exit}' /proc/cpuinfo | sed 's/^ *//')"; "#,
    r#"printf 'CPU part\t%s\n' "$(awk -F: '/CPU part/{print $2; exit}' /proc/cpuinfo | sed 's/^ *//')"; "#,
    r#"printf 'Cores\t%s\n' "$(grep -c ^processor /proc/cpuinfo)"; "#,
    r#"printf 'Features\t%s\n' "$(awk -F: '/Features|flags/{print $2; exit}' /proc/cpuinfo | sed 's/^ *//')"; "#,
    r#"echo 'SECTION Memory'; "#,
    r#"printf 'Total\t%s kB\n' "$(awk '/MemTotal/{print $2}' /proc/meminfo)"; "#,
    r#"echo 'SECTION Kernel'; "#,
    r#"printf 'uname\t%s\n' "$(uname -a)"; "#,
    r#"printf 'Hostname\t%s\n' "$(cat /proc/sys/kernel/hostname 2>/dev/null || echo na)"; "#,
    r#"echo 'SECTION Display'; "#,
    r#"printf 'fb0 size\t%s\n' "$(cat /sys/class/graphics/fb0/virtual_size 2>/dev/null || echo na)"; "#,
    r#"printf 'fb0 modes\t%s\n' "$(head -n1 /sys/class/graphics/fb0/modes 2>/dev/null || echo na)"; "#,
    r#"echo 'SECTION Camera/I2C'; "#,
    r#"printf 'camera\t%s\n' "$(cat /sys/bus/i2c/devices/1-003c/name 2>/dev/null || echo na)"; "#,
    r#"printf 'i2c-1 addrs\t%s\n' "$(i2cdetect -y -r 1 2>/dev/null | sed '1d' | cut -d: -f2- | tr ' ' '\n' | grep -vE '^(--|UU|)$' | tr '\n' ' ' || echo na)"; "#,
    r#"echo 'SECTION Storage'; "#,
    r#"printf 'partitions\t%s\n' "$(awk 'NR>2{printf "%s(%sK) ", $4, $3}' /proc/partitions)"; "#,
    r#"echo 'SECTION SPI'; "#,
    r#"printf 'devices\t%s\n' "$(ls /dev/spidev* 2>/dev/null | tr '\n' ' ' || echo na)"; "#,
    r#"echo 'SECTION Audio'; "#,
    r#"printf 'cards\t%s\n' "$(tr '\n' ' ' < /proc/asound/cards 2>/dev/null || echo na)"; "#,
    r#"echo 'SECTION Network'; "#,
    r#"printf 'interfaces\t%s\n' "$(ls /sys/class/net 2>/dev/null | tr '\n' ' ')"; "#,
    r#"echo 'SECTION NPU'; "#,
    r#"printf 'nna node\t%s\n' "$(ls -d /proc/device-tree/*nna* 2>/dev/null | tr '\n' ' ' || echo na)"; "#,
    r#"printf 'status\t%s\n' "$(cat /proc/device-tree/*nna*/status 2>/dev/null | tr -d '\0' || echo na)"; "#,
    "true",
);

pub fn parse_hwinfo(out: &str) -> HwInfo {
    let mut sections: Vec<HwSection> = Vec::new();
    for line in out.lines() {
        if let Some(title) = line.strip_prefix("SECTION ") {
            sections.push(HwSection { title: title.trim().to_string(), rows: Vec::new() });
        } else if let Some((k, v)) = line.split_once('\t') {
            if let Some(sec) = sections.last_mut() {
                sec.rows.push((k.trim().to_string(), v.trim().to_string()));
            }
        }
    }
    HwInfo { sections }
}

pub fn probe_hwinfo(t: &dyn Transport) -> Result<HwInfo> {
    let r = t.exec(STATIC_PROBE_CMD, 15)?;
    if r.rc != 0 {
        bail!("hwinfo probe rc={} ({})", r.rc, r.output.trim());
    }
    Ok(parse_hwinfo(&r.output))
}

/// Live stats probe: one `KEY VALUE` line per metric (space-separated).
pub const LIVE_PROBE_CMD: &str = concat!(
    r#"printf 'load %s\n' "$(cut -d' ' -f1 /proc/loadavg)"; "#,
    r#"printf 'memavail %s\n' "$(awk '/MemAvailable/{print $2}' /proc/meminfo)"; "#,
    r#"printf 'memtotal %s\n' "$(awk '/MemTotal/{print $2}' /proc/meminfo)"; "#,
    r#"printf 'temp %s\n' "$(cat /sys/class/thermal/thermal_zone0/temp 2>/dev/null || echo na)"; "#,
    r#"printf 'uptime %s\n' "$(cut -d' ' -f1 /proc/uptime)"; "#,
    "true",
);

pub fn parse_live(out: &str) -> Option<LiveStats> {
    let (mut load, mut avail, mut total, mut temp, mut up) = (None, None, None, None, None);
    for line in out.lines() {
        let mut it = line.split_whitespace();
        match (it.next(), it.next()) {
            (Some("load"), Some(v)) => load = v.parse::<f32>().ok(),
            (Some("memavail"), Some(v)) => avail = v.parse::<u64>().ok(),
            (Some("memtotal"), Some(v)) => total = v.parse::<u64>().ok(),
            (Some("temp"), Some(v)) => {
                temp = if v == "na" { None } else { v.parse::<f32>().ok().map(|m| m / 1000.0) }
            }
            (Some("uptime"), Some(v)) => up = v.parse::<f64>().ok().map(|s| s as u64),
            _ => {}
        }
    }
    Some(LiveStats {
        load1: load?,
        mem_avail_kb: avail?,
        mem_total_kb: total?,
        temp_c: temp,
        uptime_s: up.unwrap_or(0),
    })
}

pub fn probe_live(t: &dyn Transport) -> Result<LiveStats> {
    let r = t.exec(LIVE_PROBE_CMD, 15)?;
    parse_live(&r.output).ok_or_else(|| anyhow!("live probe: unparseable ({})", r.output.trim()))
}
```

- [ ] **Step 5: Run the tests to verify they pass**

Run: `cargo test -p kbdk-core hwinfo:: 2>&1 | tail -20`
Expected: PASS — all four tests ok.

- [ ] **Step 6: Commit**

```bash
git add crates/kbdk-core/src/hwinfo.rs crates/kbdk-core/src/lib.rs
git commit -m "kbdk-core: hwinfo module — board inventory + live stats probes"
```

---

## Task 2: Hardware smoke test (KBDK_HW-gated)

**Files:**
- Create: `crates/kbdk-core/tests/hw_hwinfo.rs`

**Interfaces:**
- Consumes: `kbdk_core::hwinfo::{probe_hwinfo, probe_live}`, `kbdk_core::adb::AdbTransport`.

No unit-test cycle — this runs only against a real board. The verification step is the gated command itself.

- [ ] **Step 1: Write the gated integration test**

Create `crates/kbdk-core/tests/hw_hwinfo.rs`:

```rust
// Hardware integration smoke test for the hwinfo probes.
// Run with: KBDK_HW=1 cargo test -p kbdk-core --test hw_hwinfo -- --nocapture
use kbdk_core::adb::AdbTransport;
use kbdk_core::hwinfo;

fn enabled() -> bool {
    std::env::var("KBDK_HW").is_ok()
}

#[test]
fn static_inventory_has_core_sections() {
    if !enabled() {
        eprintln!("skipping hw_hwinfo (set KBDK_HW=1 with a board attached)");
        return;
    }
    let t = AdbTransport::new(None);
    let info = hwinfo::probe_hwinfo(&t).expect("probe hwinfo");
    for sec in &info.sections {
        println!("[{}]", sec.title);
        for (k, v) in &sec.rows {
            println!("  {k} = {v}");
        }
    }
    let titles: Vec<&str> = info.sections.iter().map(|s| s.title.as_str()).collect();
    for want in ["SoC", "Memory", "Kernel"] {
        assert!(titles.contains(&want), "missing section {want}; got {titles:?}");
    }
}

#[test]
fn live_sample_is_plausible() {
    if !enabled() {
        return;
    }
    let t = AdbTransport::new(None);
    let s = hwinfo::probe_live(&t).expect("probe live");
    println!("live: {s:?}");
    assert!(s.mem_total_kb > 1000, "implausible MemTotal {}", s.mem_total_kb);
    assert!(s.mem_avail_kb <= s.mem_total_kb);
}
```

- [ ] **Step 2: Verify it compiles (no board needed)**

Run: `cargo test -p kbdk-core --test hw_hwinfo --no-run 2>&1 | tail -5`
Expected: compiles; test binary built.

- [ ] **Step 3: (Board attached) run it**

Run: `KBDK_HW=1 cargo test -p kbdk-core --test hw_hwinfo -- --nocapture`
Expected: PASS — prints the inventory sections and a live sample; asserts SoC/Memory/Kernel present and MemTotal plausible. (Skips cleanly if `KBDK_HW` unset.)

If a section's rows are empty or wrong on the real board (e.g. `i2cdetect` absent or different columns), report it — the shell command may need adjusting; do NOT weaken the assertions to hide it.

- [ ] **Step 4: Commit**

```bash
git add crates/kbdk-core/tests/hw_hwinfo.rs
git commit -m "kbdk-core: KBDK_HW smoke test for hwinfo probes"
```

---

## Task 3: Wire an empty Hardware tab into the shell

**Files:**
- Modify: `crates/kbdk-ui/src/main.rs`
- Modify: `crates/kbdk-ui/src/app.rs` (Tab enum, top_bar, ui routing)
- Create: `crates/kbdk-ui/src/hardware_tab.rs` (stub)

**Interfaces:**
- Produces: `hardware_tab::show(app: &mut KbdkApp, ui: &mut egui::Ui)`, `Tab::Hardware`.

UI tasks have no automated test; the gate is a clean build.

- [ ] **Step 1: Declare the module**

In `crates/kbdk-ui/src/main.rs`, add alongside the existing `mod tasks_tab;`:

```rust
mod hardware_tab;
```

- [ ] **Step 2: Create the stub tab module**

Create `crates/kbdk-ui/src/hardware_tab.rs`:

```rust
//! Hardware tab: read-only board inventory + live monitor.

use crate::app::KbdkApp;
use eframe::egui;

pub fn show(_app: &mut KbdkApp, ui: &mut egui::Ui) {
    ui.heading("Hardware");
    ui.label("board inventory — coming in the next step");
}
```

- [ ] **Step 3: Add the Tab variant**

In `crates/kbdk-ui/src/app.rs`, extend the enum (after `Tasks`):

```rust
pub enum Tab {
    Train,
    Convert,
    Deploy,
    Files,
    Tasks,
    Hardware,
}
```

- [ ] **Step 4: Add the nav button**

In `crates/kbdk-ui/src/app.rs`, in `top_bar`, after the `Tab::Tasks` selectable_value line:

```rust
            ui.selectable_value(&mut self.f.tab, Tab::Tasks, "Tasks");
            ui.selectable_value(&mut self.f.tab, Tab::Hardware, "Hardware");
```

- [ ] **Step 5: Route the tab + import the module**

In `crates/kbdk-ui/src/app.rs`, add the match arm in the `ui()` CentralPanel:

```rust
                Tab::Tasks => tasks_tab::show(self, ui),
                Tab::Hardware => hardware_tab::show(self, ui),
```

And add `hardware_tab` to the `use crate::{...}` line (alphabetical):

```rust
use crate::{convert_tab, deploy_tab, files_tab, hardware_tab, tasks_tab, theme, train_tab};
```

- [ ] **Step 6: Build and manually verify**

Run: `cargo build -p kbdk-ui 2>&1 | tail -8`
Expected: builds clean, no warnings.

Launch `cargo run -p kbdk-ui` and confirm a **Hardware** tab appears in the top bar and shows its placeholder text when clicked. Close the app.

- [ ] **Step 7: Commit**

```bash
git add crates/kbdk-ui/src/main.rs crates/kbdk-ui/src/app.rs crates/kbdk-ui/src/hardware_tab.rs
git commit -m "kbdk-ui: add empty Hardware tab to the shell"
```

---

## Task 4: Hardware tab — static inventory

**Files:**
- Modify: `crates/kbdk-ui/src/workers.rs`
- Modify: `crates/kbdk-ui/src/app.rs` (state, init, pump, OpError arm)
- Modify: `crates/kbdk-ui/src/hardware_tab.rs`

**Interfaces:**
- Consumes: `kbdk_core::hwinfo::{probe_hwinfo, HwInfo}`, `AdbTransport`.
- Produces:
  - `Msg::HwInfo(kbdk_core::hwinfo::HwInfo)`
  - `Workers::probe_hw(&self)`
  - `KbdkApp` fields: `hw_info: Option<kbdk_core::hwinfo::HwInfo>`, `hw_probing: bool`, `hw_status: String`
  - `hardware_tab::show` (static sections + refresh + first-open fetch)

- [ ] **Step 1: Add the Msg variant + worker**

In `crates/kbdk-ui/src/workers.rs`, add to `enum Msg`:

```rust
    HwInfo(kbdk_core::hwinfo::HwInfo),
```

Add to `impl Workers`:

```rust
    pub fn probe_hw(&self) {
        let tx = self.tx.clone();
        let ctx = self.ctx.clone();
        std::thread::spawn(move || {
            let t = AdbTransport::new(None);
            match kbdk_core::hwinfo::probe_hwinfo(&t) {
                Ok(info) => { let _ = tx.send(Msg::HwInfo(info)); }
                Err(e) => { let _ = tx.send(Msg::OpError { context: "hardware probe".into(), message: e.to_string() }); }
            }
            ctx.request_repaint();
        });
    }
```

- [ ] **Step 2: Add app state + initializers**

In `crates/kbdk-ui/src/app.rs`, add to `struct KbdkApp`:

```rust
    // hardware tab
    pub hw_info: Option<kbdk_core::hwinfo::HwInfo>,
    pub hw_probing: bool,
    pub hw_status: String,
```

In `KbdkApp::new`, add to the struct initializer:

```rust
            hw_info: None,
            hw_probing: false,
            hw_status: String::new(),
```

- [ ] **Step 3: Handle HwInfo + extend the OpError arm in pump()**

In `crates/kbdk-ui/src/app.rs`, add to the `pump()` match:

```rust
                Msg::HwInfo(info) => {
                    self.hw_info = Some(info);
                    self.hw_probing = false;
                    self.hw_status = "updated".into();
                }
```

Find the existing `Msg::OpError` arm (it currently sets `tasks_status` and `files.status`) and replace it with one that also surfaces in the Hardware tab and clears the probing flag:

```rust
                Msg::OpError { context, message } => {
                    let msg = format!("{context}: {message}");
                    self.tasks_status = msg.clone();
                    self.files.status = msg.clone();
                    self.hw_status = msg;
                    self.hw_probing = false;
                }
```

- [ ] **Step 4: Implement the static inventory UI**

Replace the body of `crates/kbdk-ui/src/hardware_tab.rs`:

```rust
//! Hardware tab: read-only board inventory + live monitor.

use crate::app::KbdkApp;
use crate::theme;
use eframe::egui;

pub fn show(app: &mut KbdkApp, ui: &mut egui::Ui) {
    ui.horizontal(|ui| {
        ui.heading("Hardware");
        if ui.button("⟳ refresh").clicked() {
            app.hw_probing = true;
            app.workers.probe_hw();
        }
        ui.label(egui::RichText::new(&app.hw_status).color(theme::SUBTEXT));
    });
    ui.separator();

    // fetch the static inventory once on first open
    if app.hw_info.is_none() && !app.hw_probing {
        app.hw_probing = true;
        app.workers.probe_hw();
    }

    egui::ScrollArea::vertical().show(ui, |ui| {
        if let Some(info) = app.hw_info.clone() {
            for sec in &info.sections {
                egui::CollapsingHeader::new(&sec.title)
                    .default_open(true)
                    .show(ui, |ui| {
                        egui::Grid::new(format!("hw_sec_{}", sec.title))
                            .num_columns(2)
                            .striped(true)
                            .show(ui, |ui| {
                                for (k, v) in &sec.rows {
                                    ui.label(egui::RichText::new(k).color(theme::SUBTEXT));
                                    ui.label(v);
                                    ui.end_row();
                                }
                            });
                    });
            }
        } else {
            ui.label(egui::RichText::new("probing board…").color(theme::SUBTEXT));
        }
    });
}
```

- [ ] **Step 5: Build and manually verify**

Run: `cargo build -p kbdk-ui 2>&1 | tail -10`
Expected: builds clean, no warnings.

With a board attached, run `cargo run -p kbdk-ui`, open **Hardware**: the inventory populates within ~1 s into collapsing sections (SoC, Memory, Kernel, Display, Camera/I2C, Storage, SPI, Audio, Network, NPU). **⟳ refresh** re-probes. Without a board, the status line shows the `hardware probe: …` error.

- [ ] **Step 6: Commit**

```bash
git add crates/kbdk-ui/src/workers.rs crates/kbdk-ui/src/app.rs crates/kbdk-ui/src/hardware_tab.rs
git commit -m "kbdk-ui: Hardware tab — static board inventory"
```

---

## Task 5: Hardware tab — live monitor

**Files:**
- Modify: `crates/kbdk-ui/src/workers.rs`
- Modify: `crates/kbdk-ui/src/app.rs` (state, init, pump)
- Modify: `crates/kbdk-ui/src/hardware_tab.rs`

**Interfaces:**
- Consumes: `kbdk_core::hwinfo::{probe_live, LiveStats}`, `egui_plot::{Line, Plot, PlotPoints}`, `app::push_capped` (existing module-private helper — see note below).
- Produces:
  - `Msg::HwLive(kbdk_core::hwinfo::LiveStats)`
  - `Workers::probe_hw_live(&self)`
  - `KbdkApp` fields: `hw_live: Option<kbdk_core::hwinfo::LiveStats>`, `hw_mem_hist: Vec<[f64; 2]>`, `hw_last_live_poll: Option<std::time::Instant>`, `hw_live_inflight: bool`

- [ ] **Step 1: Add the Msg variant + worker**

In `crates/kbdk-ui/src/workers.rs`, add to `enum Msg`:

```rust
    HwLive(kbdk_core::hwinfo::LiveStats),
```

Add to `impl Workers`:

```rust
    pub fn probe_hw_live(&self) {
        let tx = self.tx.clone();
        let ctx = self.ctx.clone();
        std::thread::spawn(move || {
            let t = AdbTransport::new(None);
            match kbdk_core::hwinfo::probe_live(&t) {
                Ok(s) => { let _ = tx.send(Msg::HwLive(s)); }
                Err(e) => { let _ = tx.send(Msg::OpError { context: "hardware live".into(), message: e.to_string() }); }
            }
            ctx.request_repaint();
        });
    }
```

- [ ] **Step 2: Add app state + initializers**

In `crates/kbdk-ui/src/app.rs`, add to `struct KbdkApp` (after the `hw_status` field):

```rust
    pub hw_live: Option<kbdk_core::hwinfo::LiveStats>,
    pub hw_mem_hist: Vec<[f64; 2]>,
    pub hw_last_live_poll: Option<std::time::Instant>,
    pub hw_live_inflight: bool,
```

In `KbdkApp::new`, add to the struct initializer:

```rust
            hw_live: None,
            hw_mem_hist: vec![],
            hw_last_live_poll: None,
            hw_live_inflight: false,
```

- [ ] **Step 3: Handle HwLive in pump() + unblock on error**

In `crates/kbdk-ui/src/app.rs`, add to the `pump()` match:

```rust
                Msg::HwLive(s) => {
                    let t = self.started.elapsed().as_secs_f64();
                    push_capped(&mut self.hw_mem_hist, [t, s.mem_avail_kb as f64]);
                    self.hw_live = Some(s);
                    self.hw_live_inflight = false;
                }
```

In the same file, update the `Msg::OpError` arm (added in Task 4) to also clear the live in-flight flag, so a failed live probe doesn't wedge polling. The arm becomes:

```rust
                Msg::OpError { context, message } => {
                    let msg = format!("{context}: {message}");
                    self.tasks_status = msg.clone();
                    self.files.status = msg.clone();
                    self.hw_status = msg;
                    self.hw_probing = false;
                    self.hw_live_inflight = false;
                }
```

Note: `push_capped` is the existing module-private helper in `app.rs` (`fn push_capped(v: &mut Vec<[f64; 2]>, p: [f64; 2])`, caps the vec at 240) already used by the Deploy perf plots — reuse it, do not redefine.

- [ ] **Step 4: Add the live poll + live section to the UI**

In `crates/kbdk-ui/src/hardware_tab.rs`, add the egui_plot import at the top (below the existing `use` lines):

```rust
use egui_plot::{Line, Plot, PlotPoints};
```

In `hardware_tab::show`, insert the visible-only poll **after** the first-open fetch block (after the `if app.hw_info.is_none() …` block) and **before** the `egui::ScrollArea`:

```rust
    // Live monitor: poll every ~2 s, but only while this tab is visible (egui
    // runs only the active tab's show()). The in-flight guard avoids overlap.
    let now = std::time::Instant::now();
    let due = app
        .hw_last_live_poll
        .map_or(true, |t| now.duration_since(t) >= std::time::Duration::from_secs(2));
    if due && !app.hw_live_inflight {
        app.hw_live_inflight = true;
        app.hw_last_live_poll = Some(now);
        app.workers.probe_hw_live();
    }
    ui.ctx().request_repaint_after(std::time::Duration::from_secs(2));
```

Then, inside the `egui::ScrollArea::vertical().show(ui, |ui| { ... })` closure, add the Live section as the FIRST thing rendered (before the `if let Some(info) = app.hw_info.clone()` block):

```rust
        if let Some(s) = app.hw_live.clone() {
            egui::CollapsingHeader::new("Live")
                .default_open(true)
                .show(ui, |ui| {
                    egui::Grid::new("hw_live")
                        .num_columns(2)
                        .striped(true)
                        .show(ui, |ui| {
                            ui.label(egui::RichText::new("CPU load").color(theme::SUBTEXT));
                            ui.label(format!("{:.2}", s.load1));
                            ui.end_row();
                            ui.label(egui::RichText::new("Free RAM").color(theme::SUBTEXT));
                            ui.label(format!("{} / {} kB", s.mem_avail_kb, s.mem_total_kb));
                            ui.end_row();
                            ui.label(egui::RichText::new("Temp").color(theme::SUBTEXT));
                            ui.label(match s.temp_c {
                                Some(c) => format!("{c:.1} °C"),
                                None => "n/a".into(),
                            });
                            ui.end_row();
                            ui.label(egui::RichText::new("Uptime").color(theme::SUBTEXT));
                            ui.label(fmt_uptime(s.uptime_s));
                            ui.end_row();
                        });
                    if app.hw_mem_hist.len() > 1 {
                        Plot::new("hw_mem").height(60.0).show(ui, |pui| {
                            pui.line(Line::new(
                                "free kB",
                                PlotPoints::from(app.hw_mem_hist.clone()),
                            ));
                        });
                    }
                });
        }
```

Add the `fmt_uptime` helper at the bottom of `hardware_tab.rs`:

```rust
fn fmt_uptime(s: u64) -> String {
    let (h, m) = (s / 3600, (s % 3600) / 60);
    format!("{h}h {m}m")
}
```

- [ ] **Step 5: Build and manually verify**

Run: `cargo build -p kbdk-ui 2>&1 | tail -10`
Expected: builds clean, no warnings.

With a board attached, run `cargo run -p kbdk-ui`, open **Hardware**: a **Live** section appears at the top with CPU load, Free RAM, Temp (or `n/a`), Uptime, updating every ~2 s, and a free-RAM sparkline that grows as samples accrue. Switch to another tab and back — polling pauses off-tab (no errors) and resumes on return.

- [ ] **Step 6: Commit**

```bash
git add crates/kbdk-ui/src/workers.rs crates/kbdk-ui/src/app.rs crates/kbdk-ui/src/hardware_tab.rs
git commit -m "kbdk-ui: Hardware tab — live monitor (load/RAM/temp/uptime + sparkline)"
```

---

## Task 6: Workspace check + docs

**Files:**
- Modify: `CLAUDE.md`, `README.md`

- [ ] **Step 1: Full workspace build + test**

Run: `cargo build 2>&1 | tail -5 && cargo test -p kbdk-core --lib 2>&1 | grep "test result"`
Expected: workspace builds; all `kbdk-core` lib tests pass (including the new `hwinfo::` tests).

- [ ] **Step 2: Update CLAUDE.md**

In `CLAUDE.md`, in the kbdk-ui notes area (near the Files/Tasks sentence added in build 1), add:

> The **Hardware** tab is a read-only board inventory: a curated static probe
> (SoC/memory/kernel/display/camera+i2c/storage/spi/audio/network/NPU, one batched
> `Transport::exec` parsed by `kbdk-core::hwinfo`) shown as collapsing sections, plus
> a live monitor (CPU load / free RAM / temperature / uptime + free-RAM sparkline)
> that auto-polls every ~2 s **only while the tab is visible** (driven from `show()`
> via `request_repaint_after`; egui runs only the active tab). Static probe is fetched
> on first open + manual refresh. Parsers are hardware-free unit-tested.

- [ ] **Step 3: Update README.md**

In `README.md`, in the kbdk-ui GUI paragraph (where Files/Tasks are mentioned), add a clause noting the **Hardware** tab (live board inventory + monitor), matching the existing sentence style.

- [ ] **Step 4: Commit**

```bash
git add CLAUDE.md README.md
git commit -m "docs: kbdk-ui Hardware tab"
```

---

## Self-Review

**Spec coverage** (against `2026-06-21-hardware-panel-design.md`):
- Curated static inventory in collapsing sections → Task 1 (`parse_hwinfo`, `STATIC_PROBE_CMD`) + Task 4 (UI). ✓
- All listed sections (SoC, Memory, Kernel, Display, Camera/I²C, Storage, SPI, Audio, Network, NPU) → `STATIC_PROBE_CMD` in Task 1. ✓
- Live monitor (CPU load, free RAM, temp, uptime) + free-RAM sparkline → Task 1 (`parse_live`) + Task 5 (UI + plot). ✓
- Visible-only ~2 s auto-poll, no background drain, in-flight guard → Task 5 (`show()` poll + `request_repaint_after`, `hw_live_inflight`). ✓
- Static fetched once on open + manual refresh → Task 4 (`hw_probing` guard + ⟳ button). ✓
- New `kbdk-core::hwinfo` with hardware-free parser tests → Task 1. ✓
- All board I/O on worker threads → `probe_hw`/`probe_hw_live` spawn threads (Tasks 4–5). ✓
- Read-only → only reads `/proc`/sysfs/`uname`/`i2cdetect`/device-tree; no writes. ✓
- Defensive probes (`n/a`, `; true`) → `STATIC_PROBE_CMD`/`LIVE_PROBE_CMD` in Task 1. ✓
- Errors via `OpError` in the Hardware status line → Task 4 (OpError arm extended). ✓
- `KBDK_HW` integration test → Task 2. ✓
- Flat tab, no nav reshell → Task 3. ✓

**Placeholder scan:** No TBD/TODO; every code step shows complete code. The `OpError` pump arm is shown in full in Task 4 and again (with the added `hw_live_inflight = false`) in Task 5 — this is an intentional, called-out evolution, not a placeholder.

**Type consistency:** `HwInfo`/`HwSection`/`LiveStats` field names and the `Msg::HwInfo(HwInfo)` / `Msg::HwLive(LiveStats)` shapes are used identically across Tasks 1, 4, 5. `probe_hwinfo`/`probe_live` (core) vs `probe_hw`/`probe_hw_live` (workers) are distinct by design — core functions vs worker methods. `push_capped` is the existing `app.rs` helper (caps at 240), reused not redefined. `hw_live_inflight` is set in Task 5 and cleared in both the `HwLive` and `OpError` arms.

**Note for the implementer (egui 0.34 / egui_plot 0.35):** `CollapsingHeader`, `Grid`, `ScrollArea`, `request_repaint_after` are egui 0.34; `Plot`/`Line::new(name, points)`/`PlotPoints::from` are egui_plot 0.35 exactly as used in `deploy_tab.rs` (lines ~491–512) — mirror that file if any call differs.
