//! Top-level app: tab routing, device badge, channel pumping, persistence.

use crate::workers::{Msg, Workers};
use crate::{convert_tab, deploy_tab, theme, train_tab};
use eframe::egui;
use std::sync::mpsc::{channel, Receiver};

#[derive(Debug, PartialEq, Clone, Copy, serde::Serialize, serde::Deserialize)]
pub enum Tab {
    Train,
    Convert,
    Deploy,
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
}

/// Keep the perf-plot vectors bounded (~8 min of 2 s samples).
fn push_capped(v: &mut Vec<[f64; 2]>, p: [f64; 2]) {
    v.push(p);
    if v.len() > 240 {
        v.remove(0);
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
        let workers = Workers {
            tx,
            repo_root: std::env::current_dir().expect("cwd"),
            ctx: cc.egui_ctx.clone(),
            py_pid: Default::default(),
            polling: Default::default(),
        };
        workers.start_device_poller();

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
        };
        app.rescan_packs();

        // test hooks: --tab <train|convert|deploy> forces a tab;
        // KBDK_POLL=1 starts the board log poller as if Run had been clicked.
        if let Some(tab) = std::env::args().skip_while(|a| a != "--tab").nth(1) {
            app.f.tab = match tab.as_str() {
                "convert" => Tab::Convert,
                "deploy" => Tab::Deploy,
                _ => Tab::Train,
            };
        }
        if std::env::var("KBDK_POLL").is_ok() {
            app.running = true;
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
            ui.selectable_value(&mut self.f.tab, Tab::Train, "Train");
            ui.selectable_value(&mut self.f.tab, Tab::Convert, "Convert");
            ui.selectable_value(&mut self.f.tab, Tab::Deploy, "Deploy & Run");

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
            });
    }
}
