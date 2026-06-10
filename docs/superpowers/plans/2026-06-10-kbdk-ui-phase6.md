# kbdk-ui (Phase 6) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Desktop app (egui/eframe, Catppuccin Mocha) driving the whole kbdk pipeline: pick a dataset → train with live curves → convert → deploy → run on the board with live result feed.

**Architecture:** `crates/kbdk-ui` (eframe). All long work (uv subprocesses, board I/O) runs on `std::thread` workers that send typed events over `mpsc` channels; the UI thread only renders and polls channels (`request_repaint` on arrival). Python subprocess spawning/JSON-line parsing moves into `kbdk-core::pipeline` so CLI and UI share one implementation.

**Tech Stack:** eframe 0.34 + egui_plot 0.34, rfd (native file dialogs), hand-rolled Catppuccin Mocha palette (no catppuccin-egui version coupling), kbdk-core for transports/deploy.

**Verification:** unit tests for pipeline event parsing; UI launched headed and screenshot-checked; Deploy tab actions verified against the live board (adb attached).

---

### Task 1: kbdk-core::pipeline (shared uv-subprocess streaming)

**Files:** Create `crates/kbdk-core/src/pipeline.rs`; modify `lib.rs`, refactor `kbdk-cli/src/main.rs::run_py_streaming` onto it.

- `pub enum PyEvent { Line(serde_json::Value), Raw(String), Exited(i32) }`
- `pub fn run_py_script(repo_root, script, args, on_event: &mut dyn FnMut(PyEvent)) -> Result<()>` — spawns `uv run <script> <args...>` in `<repo_root>/py`, streams stdout lines (JSON → `Line`, other → `Raw`), sends `Exited(rc)` at the end; errors if spawn fails or rc != 0.
- Unit test: spawn with a stub — add `pub fn run_streaming(cmd: &mut Command, on_event)` inner function tested via `sh -c 'echo {"event":"x"}; echo plain'`.
- CLI keeps identical behavior (prints raw lines to stdout, event summaries to stderr).

### Task 2: kbdk-ui scaffold + Mocha theme + device status

**Files:** Create `crates/kbdk-ui/Cargo.toml`, `src/main.rs`, `src/theme.rs`, `src/app.rs`; workspace member add.

- `theme.rs`: Catppuccin Mocha palette constants (base #1e1e2e, mantle #181825, crust #11111b, surface0 #313244, surface1 #45475a, text #cdd6f4, subtext #a6adc8, blue #89b4fa, green #a6e3a1, red #f38ba8, yellow #f9e2af, mauve #cba6f7, peach #fab387) + `fn apply(ctx)` setting `egui::Visuals`.
- `app.rs`: `KbdkApp` with tab enum (Train/Convert/Deploy), top bar with device badge.
- Device poller thread: every 3 s run `discover()`, send `Devices` over channel.
- Verify: `cargo run -p kbdk-ui` opens themed window; screenshot check.

### Task 3: Train tab

- Fields: dataset dir (text + rfd Browse), backbone combo (mobilenet_v2 default, resnet18), epochs (DragValue 1–50), size (DragValue 64–224), out path (auto: `models/<dataset-name>/model.pt`).
- Start button → worker thread → `pipeline::run_py_script("kbdk-train", ...)`, events over channel.
- Live state: per-epoch `(loss, val_acc)` vectors; two `egui_plot::Plot` curves; status line (device, classes, n_train); Stop = kill child (store `Child` handle in `Arc<Mutex<Option<Child>>>`, expose kill in pipeline via spawn handle return).
- On `saved` event: stash model path for the Convert tab.

### Task 4: Convert tab

- Fields: model path (prefilled from train), data dir (prefilled), pack name, out dir (`packs/`), size.
- Start → worker → `kbdk-convert` events; show step progress (pnnx → quantize → parity → done), parity gauge (green ≥0.8/red), final pack path; stash for Deploy tab.

### Task 5: Deploy & Run tab (hardware-verified)

- Pack list: scan `packs/*/manifest.json` (name, backbone, input size, quant, label count).
- Deploy button → worker: `deploy::deploy_runner` (bin/kbrun) + `deploy_pack`; progress + result.
- Run/Stop buttons → `deploy::start_runner` / `stop_runner` (res combo 320x240, frames 0).
- Log poller thread while running: every 2 s `exec("tail -n 5 /tmp/kbrun.log")`, parse last `result` JSON → show top-1 label huge (Mocha green) + conf bar + top-5 list + ms; stderr tail collapsible.
- Hardware verification: deploy `packs/toy3`, run, see live results in the UI, stop.

### Task 6: Polish + docs + merge

- Persist field values via eframe `Storage` (`save`/`load` on `KbdkApp` serde struct).
- `kbdk-ui` release profile note; README + CLAUDE.md kbdk section gains the UI; commit; finishing-a-development-branch.

**Self-review:** spec coverage = spec phase 6 list (dataset picker ✓ Task 3, train curves ✓ Task 3, convert tab ✓ Task 4, deploy tab device status/push/start-stop/last results ✓ Tasks 2+5). Types: `PyEvent` defined Task 1, consumed Tasks 3–4; deploy fns exist in kbdk-core already.
