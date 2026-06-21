//! Top-level app: tab routing, device badge, channel pumping, persistence.

use crate::workers::{Msg, Workers};
use crate::{
    convert_tab, deploy_tab, edit_tab, examples_tab, files_tab, hardware_tab, tasks_tab, theme,
    train_tab,
};
use eframe::egui;
use std::sync::mpsc::{channel, Receiver};

#[derive(Debug, PartialEq, Clone, Copy, serde::Serialize, serde::Deserialize)]
pub enum Tab {
    Train,
    Convert,
    Deploy,
    Files,
    Edit,
    Tasks,
    Hardware,
    Examples,
}

fn default_task() -> String {
    "classification".into()
}
fn default_runtime() -> String {
    "ncnn".into()
}
fn default_zoom() -> f32 {
    1.25
}
fn default_local_path() -> String {
    ".".into()
}
fn default_board_path() -> String {
    "/tmp".into()
}

/// Everything the user typed — persisted across sessions via eframe Storage.
/// New fields need `#[serde(default…)]` so storage from older builds still loads.
#[derive(serde::Serialize, serde::Deserialize)]
pub struct Fields {
    pub tab: Tab,
    pub data_dir: String,
    pub backbone: String,
    /// "classification" | "detection" (kbdk-train --task)
    #[serde(default = "default_task")]
    pub task: String,
    /// convert target: "ncnn" (CPU) | "nvdla" (NPU)
    #[serde(default = "default_runtime")]
    pub runtime: String,
    /// UI zoom factor (multiplies the native scale); Cmd+± changes persist here
    #[serde(default = "default_zoom")]
    pub ui_zoom: f32,
    pub epochs: u32,
    pub size: u32,
    pub model_out: String,
    pub pack_name: String,
    pub packs_dir: String,
    pub res: String,
    pub capture_dir: String,
    pub capture_class: String,
    #[serde(default = "default_local_path")]
    pub last_local_path: String,
    #[serde(default = "default_board_path")]
    pub last_board_path: String,
    /// Edit tab: prepend the MPP/eyesee LD_LIBRARY_PATH when running the program.
    #[serde(default)]
    pub edit_mpp_libs: bool,
    /// Edit tab: extra linker/compiler flags appended after the source.
    #[serde(default)]
    pub edit_extra_args: String,
    /// Edit tab: last opened file (convenience + screenshot seeding).
    #[serde(default)]
    pub edit_open_path: String,
}

impl Default for Fields {
    fn default() -> Self {
        Self {
            tab: Tab::Train,
            data_dir: "examples/toy-dataset".into(),
            backbone: "mobilenet_v2".into(),
            task: default_task(),
            runtime: default_runtime(),
            ui_zoom: default_zoom(),
            epochs: 5,
            size: 224,
            model_out: "models/model.pt".into(),
            pack_name: "mypack".into(),
            packs_dir: "packs".into(),
            res: "320x240".into(),
            capture_dir: "datasets/mydata".into(),
            capture_class: "class_a".into(),
            last_local_path: default_local_path(),
            last_board_path: default_board_path(),
            edit_mpp_libs: false,
            edit_extra_args: String::new(),
            edit_open_path: String::new(),
        }
    }
}

/// Edit-tab state (local source tree + editor buffer + build/run log). Everything
/// non-persisted lives here; `edit_mpp_libs`/`edit_extra_args`/`edit_open_path` are
/// on `Fields` so they survive restarts.
pub struct EditState {
    /// Local-only file tree rooted at the repo root (reuses the Files tab type).
    pub tree: files_tab::FileTree,
    pub open_path: Option<std::path::PathBuf>,
    pub buffer: String,
    /// Snapshot at last open/save; `buffer != saved_snapshot` ⇒ dirty.
    pub saved_snapshot: String,
    /// Combined compile + run log (bounded in `pump`).
    pub output: Vec<String>,
    pub building: bool,
    pub running: bool,
    /// Set by EditBuildDone(Ok); gates Deploy + Run on a clean build.
    pub last_bin: Option<std::path::PathBuf>,
    /// Buffer contents `last_bin` was compiled from. Deploy + Run is gated on this
    /// still matching the editor buffer, so editing the code or switching files
    /// invalidates the built binary and prevents running stale code on the board.
    pub built_from: Option<String>,
    pub status: String,
}

impl EditState {
    pub fn new(root: String) -> Self {
        Self {
            tree: files_tab::local_tree(root),
            open_path: None,
            buffer: String::new(),
            saved_snapshot: String::new(),
            output: vec![],
            building: false,
            running: false,
            last_bin: None,
            built_from: None,
            status: String::new(),
        }
    }
}

pub struct KbdkApp {
    pub f: Fields,
    pub rx: Receiver<Msg>,
    pub workers: Workers,

    // device badge
    pub adb_devices: Vec<String>,
    pub serial_ports: Vec<String>,

    // train state
    pub training: bool,
    pub train_status: String,
    pub epoch_loss: Vec<[f64; 2]>,
    pub epoch_acc: Vec<[f64; 2]>,
    pub train_classes: Vec<String>,
    pub saved_model: Option<String>,

    // convert state
    pub converting: bool,
    pub convert_steps: Vec<String>,
    pub parity: Option<f64>,
    pub built_pack: Option<String>,

    // deploy state
    pub packs: Vec<deploy_tab::PackInfo>,
    pub deploying: bool,
    pub deploy_status: Vec<String>,
    pub deployed: Option<String>,
    pub running: bool,
    pub last_result: Option<serde_json::Value>,
    pub board_note: String,
    pub cam_tex: Option<egui::TextureHandle>,

    /// arrival times of recent board frames, for the preview fps readout
    pub frame_times: std::collections::VecDeque<std::time::Instant>,

    /// full softmax from the board (all classes, classification packs)
    pub board_probs: Option<Vec<f32>>,
    /// class names of the running pack (for the all-classes list)
    pub pack_labels: Vec<String>,

    // board performance monitor (deploy tab; sampled every ~2 s while running)
    pub perf_ms: Vec<[f64; 2]>,       // (t secs, inference latency ms)
    pub perf_cam_fps: Vec<[f64; 2]>,  // (t, camera frames/s)
    pub perf_inf_rate: Vec<[f64; 2]>, // (t, inferences/s)
    pub perf_load: f32,
    pub perf_mem_kb: u64,
    pub perf_rss_kb: u64,
    perf_prev: Option<(f64, u64, u64)>, // (t, frames, infers) for rate deltas

    // dataset capture (from the live board camera)
    pub last_frame: Option<(usize, usize, Vec<u8>)>,
    pub burst: bool,
    pub captured_n: u32,
    pub capture_note: String,

    frame_count: u32,
    shot_requested: bool,
    started: std::time::Instant,

    // tasks tab
    pub procs: Vec<kbdk_core::procs::Proc>,
    pub tasks_status: String,
    pub kill_confirm: Option<(u32, String)>, // (pid, cmd) awaiting confirm

    // files tab
    pub files: files_tab::FilesState,

    // edit tab
    pub edit: EditState,

    // hardware tab
    pub hw_info: Option<kbdk_core::hwinfo::HwInfo>,
    pub hw_probing: bool,
    pub hw_status: String,
    pub hw_live: Option<kbdk_core::hwinfo::LiveStats>,
    pub hw_mem_hist: Vec<[f64; 2]>,
    pub hw_last_live_poll: Option<std::time::Instant>,
    pub hw_live_inflight: bool,

    // examples tab (cached local scan of examples/board)
    pub examples: Vec<examples_tab::Example>,
    pub examples_status: String,
    /// false until the first scan; reset by Refresh. Prevents re-scanning the
    /// filesystem every frame when the scan yields zero templates.
    pub examples_scanned: bool,
}

/// Keep the perf-plot vectors bounded (~8 min of 2 s samples).
fn push_capped(v: &mut Vec<[f64; 2]>, p: [f64; 2]) {
    v.push(p);
    if v.len() > 240 {
        v.remove(0);
    }
}

/// Keep the Edit-tab build/run log bounded (long-running camera programs stream
/// forever): drop the oldest 500 lines once it grows past 2000.
fn cap_log(v: &mut Vec<String>) {
    if v.len() > 2000 {
        v.drain(0..500);
    }
}

/// Test hook: `KBDK_FIELDS=key=val,key=val` overrides persisted form fields at
/// startup, so automated checks (KBDK_AUTOTRAIN / KBDK_AUTOCONVERT / screenshots)
/// can drive specific configurations headlessly. Harmless otherwise.
fn apply_field_overrides(f: &mut Fields) {
    let Ok(spec) = std::env::var("KBDK_FIELDS") else { return };
    for kv in spec.split(',') {
        let Some((k, v)) = kv.split_once('=') else { continue };
        match k.trim() {
            "data_dir" => f.data_dir = v.into(),
            "backbone" => f.backbone = v.into(),
            "task" => f.task = v.into(),
            "runtime" => f.runtime = v.into(),
            "epochs" => f.epochs = v.parse().unwrap_or(f.epochs),
            "size" => f.size = v.parse().unwrap_or(f.size),
            "model_out" => f.model_out = v.into(),
            "pack_name" => f.pack_name = v.into(),
            "edit_open_path" => f.edit_open_path = v.into(),
            "edit_extra_args" => f.edit_extra_args = v.into(),
            "edit_mpp_libs" => f.edit_mpp_libs = v == "1" || v == "true",
            _ => eprintln!("KBDK_FIELDS: unknown key {k}"),
        }
    }
}

impl KbdkApp {
    pub fn new(cc: &eframe::CreationContext<'_>) -> Self {
        theme::apply(&cc.egui_ctx);

        let mut f: Fields = cc
            .storage
            .and_then(|s| eframe::get_value(s, eframe::APP_KEY))
            .unwrap_or_default();
        apply_field_overrides(&mut f);

        // zoom_factor multiplies the native scale (a hardcoded
        // set_pixels_per_point used to *shrink* the UI on retina displays);
        // Cmd+± adjustments are saved back into f.ui_zoom on exit.
        cc.egui_ctx.set_zoom_factor(f.ui_zoom.clamp(0.5, 3.0));

        let (tx, rx) = channel();
        let repo_root = std::env::current_dir().expect("cwd");
        let workers = Workers {
            tx,
            repo_root: repo_root.clone(),
            ctx: cc.egui_ctx.clone(),
            py_pid: Default::default(),
            run_pid: Default::default(),
            run_cancel: Default::default(),
            polling: Default::default(),
        };
        workers.start_device_poller();

        let files = files_tab::FilesState::new(f.last_local_path.clone(), f.last_board_path.clone());
        let edit = EditState::new(repo_root.to_string_lossy().into_owned());

        let mut app = Self {
            f,
            rx,
            workers,
            adb_devices: vec![],
            serial_ports: vec![],
            training: false,
            train_status: String::new(),
            epoch_loss: vec![],
            epoch_acc: vec![],
            train_classes: vec![],
            saved_model: None,
            converting: false,
            convert_steps: vec![],
            parity: None,
            built_pack: None,
            packs: vec![],
            deploying: false,
            deploy_status: vec![],
            deployed: None,
            running: false,
            last_result: None,
            board_note: String::new(),
            cam_tex: None,
            frame_times: std::collections::VecDeque::new(),
            board_probs: None,
            pack_labels: vec![],
            perf_ms: vec![],
            perf_cam_fps: vec![],
            perf_inf_rate: vec![],
            perf_load: 0.0,
            perf_mem_kb: 0,
            perf_rss_kb: 0,
            perf_prev: None,
            last_frame: None,
            burst: false,
            captured_n: 0,
            capture_note: String::new(),
            frame_count: 0,
            shot_requested: false,
            started: std::time::Instant::now(),
            procs: vec![],
            tasks_status: String::new(),
            kill_confirm: None,
            files,
            edit,
            hw_info: None,
            hw_probing: false,
            hw_status: String::new(),
            hw_live: None,
            hw_mem_hist: vec![],
            hw_last_live_poll: None,
            hw_live_inflight: false,
            examples: vec![],
            examples_status: String::new(),
            examples_scanned: false,
        };
        app.rescan_packs();

        // test hooks: --tab <train|convert|deploy> forces a tab;
        // KBDK_POLL=1 starts the board log poller as if Run had been clicked.
        if let Some(tab) = std::env::args().skip_while(|a| a != "--tab").nth(1) {
            app.f.tab = match tab.as_str() {
                "convert" => Tab::Convert,
                "deploy" => Tab::Deploy,
                "files" => Tab::Files,
                "edit" => Tab::Edit,
                "tasks" => Tab::Tasks,
                "hardware" => Tab::Hardware,
                "examples" => Tab::Examples,
                _ => Tab::Train,
            };
        }
        if let Ok(v) = std::env::var("KBDK_POLL") {
            app.running = true;
            // KBDK_POLL=<packname> also loads that pack's labels (the runner
            // was started externally, so the Run button never resolved them)
            if v != "1" {
                if let Some(p) = app.packs.iter().find(|p| p.name == v) {
                    app.pack_labels = deploy_tab::load_labels(&p.dir);
                }
            }
            app.workers.run_pack_poll_only();
        }
        if std::env::var("KBDK_AUTOCAPTURE").is_ok() {
            app.f.capture_dir = "datasets/uitest".into();
            app.f.capture_class = "class_a".into();
            app.burst = true;
        }
        // KBDK_FIELDS (when set) supplies the configuration; otherwise the
        // auto hooks fall back to their classic uitest defaults.
        let fields_overridden = std::env::var("KBDK_FIELDS").is_ok();
        if std::env::var("KBDK_AUTOTRAIN").is_ok() {
            app.f.tab = Tab::Train;
            if !fields_overridden {
                app.f.data_dir = "examples/toy-dataset".into();
                app.f.model_out = "models/uitest/model.pt".into();
                app.f.epochs = 4;
                app.f.size = 64;
            }
            train_tab::start(&mut app);
        }
        if std::env::var("KBDK_AUTOCONVERT").is_ok() {
            app.f.tab = Tab::Convert;
            if !fields_overridden {
                app.f.data_dir = "examples/toy-dataset".into();
                app.f.pack_name = "uitest".into();
                app.f.size = 64;
            }
            let m = convert_tab::default_model(&app);
            convert_tab::start(&mut app, m);
        }
        // Seed the Edit tab's open file (for --tab edit screenshots / convenience).
        if !app.f.edit_open_path.is_empty() {
            let p = std::path::Path::new(&app.f.edit_open_path);
            let abs = if p.is_absolute() {
                p.to_path_buf()
            } else {
                app.workers.repo_root.join(p)
            };
            if let Ok(text) = std::fs::read_to_string(&abs) {
                app.edit.buffer = text;
                app.edit.saved_snapshot = app.edit.buffer.clone();
                app.edit.open_path = Some(abs.clone());
                app.edit.status = format!("opened {}", abs.display());
            }
        }
        app
    }

    pub fn rescan_packs(&mut self) {
        self.packs = deploy_tab::scan_packs(&self.workers.repo_root.join(&self.f.packs_dir));
    }

    /// Save the latest board frame as a PNG into <capture_dir>/<capture_class>/
    /// (ImageFolder layout — point the Train tab at capture_dir afterwards).
    pub fn save_capture(&mut self) {
        let Some((w, h, rgb)) = &self.last_frame else {
            self.capture_note = "no frame yet".into();
            return;
        };
        let class = self.f.capture_class.trim();
        if class.is_empty() {
            self.capture_note = "set a class name first".into();
            return;
        }
        let dir = self.workers.repo_root.join(&self.f.capture_dir).join(class);
        if let Err(e) = std::fs::create_dir_all(&dir) {
            self.capture_note = format!("mkdir failed: {e}");
            return;
        }
        let ms = std::time::SystemTime::now()
            .duration_since(std::time::UNIX_EPOCH)
            .map(|d| d.as_millis())
            .unwrap_or(0);
        let path = dir.join(format!("cap_{ms}.png"));
        match image::save_buffer(&path, rgb, *w as u32, *h as u32, image::ColorType::Rgb8) {
            Ok(()) => {
                self.captured_n += 1;
                self.capture_note = format!(
                    "{} saved ({} this session)",
                    path.strip_prefix(&self.workers.repo_root)
                        .unwrap_or(&path)
                        .display(),
                    self.captured_n
                );
            }
            Err(e) => self.capture_note = format!("save failed: {e}"),
        }
    }

    fn pump(&mut self) {
        while let Ok(msg) = self.rx.try_recv() {
            match msg {
                Msg::Devices { adb, serial } => {
                    self.adb_devices = adb;
                    self.serial_ports = serial;
                }
                Msg::TrainEvent(e) => train_tab::on_event(self, e),
                Msg::TrainDone(r) => {
                    self.training = false;
                    if let Err(e) = r {
                        self.train_status = format!("failed: {e}");
                    }
                }
                Msg::ConvertEvent(e) => convert_tab::on_event(self, e),
                Msg::ConvertDone(r) => {
                    self.converting = false;
                    match r {
                        Ok(()) => self.rescan_packs(),
                        Err(e) => self.convert_steps.push(format!("failed: {e}")),
                    }
                }
                Msg::DeployProgress(s) => self.deploy_status.push(s),
                Msg::DeployDone(r) => {
                    self.deploying = false;
                    match r {
                        Ok(remote) => {
                            self.deploy_status.push(format!("deployed: {remote}"));
                            self.deployed = Some(remote);
                        }
                        Err(e) => self.deploy_status.push(format!("failed: {e}")),
                    }
                }
                Msg::RunDone(r) => match r {
                    Ok(()) => {
                        self.running = true;
                        self.board_note = "runner started".into();
                        // fresh perf history per run (counters reset with kbrun)
                        self.perf_ms.clear();
                        self.perf_cam_fps.clear();
                        self.perf_inf_rate.clear();
                        self.perf_prev = None;
                        self.board_probs = None;
                    }
                    Err(e) => self.board_note = format!("start failed: {e}"),
                },
                Msg::StopDone => {
                    self.running = false;
                    self.board_note = "stopped".into();
                }
                Msg::BoardResult(v) => {
                    if let Some(ms) = v["ms"].as_f64() {
                        let t = self.started.elapsed().as_secs_f64();
                        push_capped(&mut self.perf_ms, [t, ms]);
                    }
                    self.last_result = Some(v);
                }
                Msg::BoardProbs(p) => self.board_probs = Some(p),
                Msg::BoardStats { load1, mem_kb, rss_kb, frames, infers } => {
                    self.perf_load = load1;
                    self.perf_mem_kb = mem_kb;
                    self.perf_rss_kb = rss_kb;
                    let t = self.started.elapsed().as_secs_f64();
                    if let Some((pt, pf, pi)) = self.perf_prev {
                        let dt = t - pt;
                        // counters reset when a runner restarts -> skip that sample
                        if dt > 0.1 && frames >= pf && infers >= pi {
                            push_capped(&mut self.perf_cam_fps, [t, (frames - pf) as f64 / dt]);
                            push_capped(&mut self.perf_inf_rate, [t, (infers - pi) as f64 / dt]);
                        }
                    }
                    self.perf_prev = Some((t, frames, infers));
                }
                Msg::BoardFrame { w, h, rgb } => {
                    let img = egui::ColorImage::from_rgb([w, h], &rgb);
                    match &mut self.cam_tex {
                        Some(tex) => tex.set(img, egui::TextureOptions::LINEAR),
                        None => {
                            self.cam_tex = Some(self.workers.ctx.load_texture(
                                "board-cam",
                                img,
                                egui::TextureOptions::LINEAR,
                            ))
                        }
                    }
                    self.last_frame = Some((w, h, rgb));
                    self.frame_times.push_back(std::time::Instant::now());
                    while self.frame_times.len() > 30 {
                        self.frame_times.pop_front();
                    }
                    if self.burst {
                        self.save_capture();
                    }
                }
                Msg::BoardNote(s) => self.board_note = s,
                Msg::ProcList(v) => {
                    self.procs = v;
                    self.tasks_status = format!("{} processes", self.procs.len());
                }
                Msg::Killed { pid } => {
                    self.tasks_status = format!("killed {pid}");
                    self.procs.retain(|p| p.pid != pid);
                }
                Msg::HwInfo(info) => {
                    self.hw_info = Some(info);
                    self.hw_probing = false;
                    self.hw_status = "updated".into();
                }
                Msg::HwLive(s) => {
                    let t = self.started.elapsed().as_secs_f64();
                    push_capped(&mut self.hw_mem_hist, [t, s.mem_avail_kb as f64]);
                    self.hw_live = Some(s);
                    self.hw_live_inflight = false;
                }
                Msg::OpError { context, message } => {
                    let msg = format!("{context}: {message}");
                    self.tasks_status = msg.clone();
                    self.files.status = msg.clone();
                    self.hw_status = msg;
                    self.hw_probing = false;
                    self.hw_live_inflight = false;
                }
                Msg::DirListed { path, entries } => {
                    self.files.board.children.insert(path, entries);
                }
                Msg::FileOpDone { context, refresh_board } => {
                    self.files.status = context;
                    if let Some(dir) = refresh_board {
                        self.files.board.children.remove(&dir);
                        self.workers.list_dir(dir);
                    }
                }
                Msg::PreviewLoaded { path, body, is_binary } => {
                    self.files.preview = Some((path, body, is_binary));
                }
                Msg::EditBuildOutput(s) => {
                    self.edit.output.push(s);
                    cap_log(&mut self.edit.output);
                }
                Msg::EditBuildDone(r) => {
                    self.edit.building = false;
                    match r {
                        Ok(bin) => {
                            self.edit.status = format!("build ok: {}", bin.display());
                            self.edit.output.push(format!("build ok: {}", bin.display()));
                            self.edit.last_bin = Some(bin);
                        }
                        Err(e) => {
                            self.edit.status = format!("build failed: {e}");
                            self.edit.output.push(format!("build failed: {e}"));
                            self.edit.last_bin = None;
                        }
                    }
                    cap_log(&mut self.edit.output);
                }
                Msg::EditRunOutput(s) => {
                    self.edit.output.push(s);
                    cap_log(&mut self.edit.output);
                }
                Msg::EditRunDone(r) => {
                    self.edit.running = false;
                    match r {
                        Ok(()) => self.edit.output.push("(program exited)".into()),
                        Err(e) => {
                            self.edit.status = format!("run error: {e}");
                            self.edit.output.push(format!("run error: {e}"));
                        }
                    }
                    cap_log(&mut self.edit.output);
                }
                Msg::EditRunStopped => {
                    self.edit.running = false;
                    self.edit.output.push("(stopped)".into());
                    cap_log(&mut self.edit.output);
                }
            }
        }
    }

    /// `kbdk-ui --screenshot PATH`: render a few frames (or `KBDK_SHOT_DELAY` seconds),
    /// save a PNG, exit. Used for automated UI verification (no OS screen-capture
    /// permission needed because the app photographs its own viewport).
    fn screenshot_tick(&mut self, ctx: &egui::Context) {
        let Some(path) = std::env::args().skip_while(|a| a != "--screenshot").nth(1) else {
            return;
        };
        let delay: f64 = std::env::var("KBDK_SHOT_DELAY")
            .ok()
            .and_then(|s| s.parse().ok())
            .unwrap_or(0.0);
        self.frame_count += 1;
        let due = self.started.elapsed().as_secs_f64() >= delay;
        if self.frame_count >= 10 && due && !self.shot_requested {
            self.shot_requested = true;
            ctx.send_viewport_cmd(egui::ViewportCommand::Screenshot(egui::UserData::default()));
        }
        let shot = ctx.input(|i| {
            i.events.iter().find_map(|e| match e {
                egui::Event::Screenshot { image, .. } => Some(image.clone()),
                _ => None,
            })
        });
        if let Some(img) = shot {
            let [w, h] = img.size;
            let rgba: Vec<u8> = img.pixels.iter().flat_map(|c| c.to_array()).collect();
            image::save_buffer(&path, &rgba, w as u32, h as u32, image::ColorType::Rgba8)
                .expect("save screenshot");
            std::process::exit(0);
        }
        ctx.request_repaint();
    }

    fn top_bar(&mut self, ui: &mut egui::Ui) {
        ui.horizontal(|ui| {
            ui.heading(egui::RichText::new("kbdk").color(theme::MAUVE).strong());
            ui.label(egui::RichText::new("KidBright µAI dev-kit").color(theme::SUBTEXT));
            ui.separator();

            // ML pipeline (Train / Convert / Deploy & Run) collapsed under a
            // dropdown; the button reflects which ML tab is active.
            let ml_active = matches!(self.f.tab, Tab::Train | Tab::Convert | Tab::Deploy);
            let ml_label = if ml_active {
                let name = match self.f.tab {
                    Tab::Convert => "Convert",
                    Tab::Deploy => "Deploy & Run",
                    _ => "Train",
                };
                format!("ML  ·  {name}  ▾")
            } else {
                "ML  ▾".to_string()
            };
            let ml_text = if ml_active {
                egui::RichText::new(ml_label).color(theme::MAUVE).strong()
            } else {
                egui::RichText::new(ml_label)
            };
            ui.menu_button(ml_text, |ui| {
                if ui
                    .selectable_value(&mut self.f.tab, Tab::Train, "Train")
                    .clicked()
                {
                    ui.close();
                }
                if ui
                    .selectable_value(&mut self.f.tab, Tab::Convert, "Convert")
                    .clicked()
                {
                    ui.close();
                }
                if ui
                    .selectable_value(&mut self.f.tab, Tab::Deploy, "Deploy & Run")
                    .clicked()
                {
                    ui.close();
                }
            });

            // Board / dev tabs as direct top-level entries.
            ui.selectable_value(&mut self.f.tab, Tab::Files, "Files");
            ui.selectable_value(&mut self.f.tab, Tab::Tasks, "Tasks");
            ui.selectable_value(&mut self.f.tab, Tab::Hardware, "Hardware");
            ui.selectable_value(&mut self.f.tab, Tab::Edit, "Edit");
            ui.selectable_value(&mut self.f.tab, Tab::Examples, "Examples");

            ui.with_layout(egui::Layout::right_to_left(egui::Align::Center), |ui| {
                if self.adb_devices.is_empty() && self.serial_ports.is_empty() {
                    ui.colored_label(theme::RED, "● no board");
                } else {
                    if !self.adb_devices.is_empty() {
                        ui.colored_label(theme::GREEN, format!("● adb {}", self.adb_devices[0]));
                    }
                    if !self.serial_ports.is_empty() {
                        ui.colored_label(theme::TEAL, "● serial");
                    }
                }
            });
        });
    }
}

impl eframe::App for KbdkApp {
    fn save(&mut self, storage: &mut dyn eframe::Storage) {
        // keep Cmd+± zoom adjustments for the next launch
        self.f.ui_zoom = self.workers.ctx.zoom_factor();
        self.f.last_local_path = self.files.local.root.clone();
        self.f.last_board_path = self.files.board.root.clone();
        self.f.edit_open_path = self
            .edit
            .open_path
            .as_ref()
            .map(|p| p.to_string_lossy().into_owned())
            .unwrap_or_default();
        eframe::set_value(storage, eframe::APP_KEY, &self.f);
    }

    fn ui(&mut self, ui: &mut egui::Ui, _frame: &mut eframe::Frame) {
        self.pump();
        self.screenshot_tick(ui.ctx());

        egui::Panel::top("top")
            .frame(egui::Frame::default().fill(theme::MANTLE).inner_margin(8.0))
            .show_inside(ui, |ui| self.top_bar(ui));

        egui::CentralPanel::default()
            .frame(egui::Frame::default().fill(theme::BASE).inner_margin(14.0))
            .show_inside(ui, |ui| match self.f.tab {
                Tab::Train => train_tab::show(self, ui),
                Tab::Convert => convert_tab::show(self, ui),
                Tab::Deploy => deploy_tab::show(self, ui),
                Tab::Files => files_tab::show(self, ui),
                Tab::Edit => edit_tab::show(self, ui),
                Tab::Tasks => tasks_tab::show(self, ui),
                Tab::Hardware => hardware_tab::show(self, ui),
                Tab::Examples => examples_tab::show(self, ui),
            });
    }
}
