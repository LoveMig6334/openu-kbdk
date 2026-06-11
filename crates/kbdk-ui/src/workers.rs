//! Background threads: everything that blocks (uv subprocesses, board I/O,
//! device polling) runs here and reports back over an mpsc channel. The UI
//! thread only renders.

use kbdk_core::adb::AdbTransport;
use kbdk_core::frames;
use kbdk_core::pipeline::{self, PyEvent};
use kbdk_core::transport::Transport;
use kbdk_core::{deploy, discover};
use std::path::PathBuf;
use std::sync::atomic::{AtomicBool, Ordering};
use std::sync::mpsc::Sender;
use std::sync::Arc;

#[derive(Debug)]
pub enum Msg {
    Devices { adb: Vec<String>, serial: Vec<String> },
    TrainEvent(PyEvent),
    TrainDone(Result<(), String>),
    ConvertEvent(PyEvent),
    ConvertDone(Result<(), String>),
    DeployProgress(String),
    DeployDone(Result<String, String>),
    RunDone(Result<(), String>),
    StopDone,
    BoardResult(serde_json::Value),
    /// Latest camera preview frame pulled from the board (RGB888).
    BoardFrame { w: usize, h: usize, rgb: Vec<u8> },
    BoardNote(String),
}

/// Parse the kbrun preview file: "KBF1" + u16le w + u16le h + w*h*3 RGB bytes.
pub fn parse_preview(data: &[u8]) -> Option<(usize, usize, Vec<u8>)> {
    if data.len() < 8 || &data[0..4] != b"KBF1" {
        return None;
    }
    let w = u16::from_le_bytes([data[4], data[5]]) as usize;
    let h = u16::from_le_bytes([data[6], data[7]]) as usize;
    let need = 8 + w * h * 3;
    if w == 0 || h == 0 || data.len() < need {
        return None;
    }
    Some((w, h, data[8..need].to_vec()))
}

pub struct Workers {
    pub tx: Sender<Msg>,
    pub repo_root: PathBuf,
    pub ctx: eframe::egui::Context,
    /// pid of the running uv child (train or convert), for Stop
    pub py_pid: Arc<std::sync::Mutex<Option<u32>>>,
    /// log-poller liveness flag
    pub polling: Arc<AtomicBool>,
}

impl Workers {
    pub fn start_device_poller(&self) {
        let tx = self.tx.clone();
        let ctx = self.ctx.clone();
        std::thread::spawn(move || loop {
            if let Ok(d) = discover::discover() {
                let _ = tx.send(Msg::Devices {
                    adb: d.adb,
                    serial: d.serial,
                });
                ctx.request_repaint();
            }
            std::thread::sleep(std::time::Duration::from_secs(3));
        });
    }

    fn run_py(&self, script: &'static str, args: Vec<String>, wrap: fn(PyEvent) -> Msg, done: fn(Result<(), String>) -> Msg) {
        let tx = self.tx.clone();
        let ctx = self.ctx.clone();
        let root = self.repo_root.clone();
        let pid_slot = self.py_pid.clone();
        std::thread::spawn(move || {
            let result = (|| {
                let child = pipeline::spawn_py_script(&root, script, &args)
                    .map_err(|e| e.to_string())?;
                *pid_slot.lock().unwrap() = Some(child.id());
                let r = pipeline::stream_child(child, &mut |e| {
                    let _ = tx.send(wrap(e));
                    ctx.request_repaint();
                });
                *pid_slot.lock().unwrap() = None;
                r.map_err(|e| e.to_string())
            })();
            let _ = tx.send(done(result));
            ctx.request_repaint();
        });
    }

    pub fn train(&self, args: Vec<String>) {
        self.run_py("kbdk-train", args, Msg::TrainEvent, Msg::TrainDone);
    }

    pub fn convert(&self, args: Vec<String>) {
        self.run_py("kbdk-convert", args, Msg::ConvertEvent, Msg::ConvertDone);
    }

    /// NPU path: kbdk_convert.nvdla_compile (PTQ -> NVJ1 job -> runtime "nvdla" pack)
    pub fn convert_nvdla(&self, args: Vec<String>) {
        self.run_py("kbdk-nvdla", args, Msg::ConvertEvent, Msg::ConvertDone);
    }

    /// SIGTERM the running uv child (train/convert).
    pub fn kill_py(&self) {
        if let Some(pid) = *self.py_pid.lock().unwrap() {
            let _ = std::process::Command::new("kill").arg(pid.to_string()).status();
        }
    }

    pub fn deploy(&self, pack_dir: PathBuf) {
        let tx = self.tx.clone();
        let ctx = self.ctx.clone();
        let root = self.repo_root.clone();
        std::thread::spawn(move || {
            let result = (|| -> Result<String, String> {
                let t = AdbTransport::new(None);
                let kbrun = root.join("bin/kbrun");
                if kbrun.exists() {
                    let _ = tx.send(Msg::DeployProgress("pushing runner (bin/kbrun)…".into()));
                    ctx.request_repaint();
                    deploy::deploy_runner(&t, &kbrun).map_err(|e| e.to_string())?;
                }
                let _ = tx.send(Msg::DeployProgress(format!(
                    "pushing pack {} (md5-verified)…",
                    pack_dir.display()
                )));
                ctx.request_repaint();
                deploy::deploy_pack(&t, &pack_dir).map_err(|e| e.to_string())
            })();
            let _ = tx.send(Msg::DeployDone(result));
            ctx.request_repaint();
        });
    }

    pub fn run_pack(&self, pack_name: String, res: String) {
        let tx = self.tx.clone();
        let ctx = self.ctx.clone();
        std::thread::spawn(move || {
            let t = AdbTransport::new(None);
            let remote = format!("{}/{pack_name}", deploy::BOARD_PACK_ROOT);
            let r = deploy::start_runner(&t, &remote, &res, 0).map_err(|e| e.to_string());
            let _ = tx.send(Msg::RunDone(r));
            ctx.request_repaint();
        });
        self.start_log_poller();
    }

    pub fn stop_pack(&self) {
        self.polling.store(false, Ordering::Relaxed);
        let tx = self.tx.clone();
        let ctx = self.ctx.clone();
        std::thread::spawn(move || {
            let t = AdbTransport::new(None);
            let _ = deploy::stop_runner(&t);
            let _ = tx.send(Msg::StopDone);
            ctx.request_repaint();
        });
    }

    /// Test hook: start only the log poller (board runner already started externally).
    pub fn run_pack_poll_only(&self) {
        self.start_log_poller();
    }

    /// While running: stream camera preview frames — TCP via adb-forward when the
    /// board's kbrun serves them (~10-15 fps), else adb-pull of the tmpfs file
    /// (~2.5 fps) — plus the last result JSON-line every ~2 s.
    fn start_log_poller(&self) {
        self.polling.store(true, Ordering::Relaxed);
        let polling = self.polling.clone();
        let tx = self.tx.clone();
        let ctx = self.ctx.clone();
        std::thread::spawn(move || {
            use std::time::{Duration, Instant};
            let t = AdbTransport::new(None);
            let frame_local = std::env::temp_dir().join(format!("kbdk_frame_{}.rgb", std::process::id()));
            // The forward is set up once; whether anything answers on it depends
            // on the kbrun running board-side, so probe per-fetch and fall back.
            let stream = frames::FrameStream::connect(None, frames::FRAME_PORT).ok();
            let mut tcp_fail: u32 = 0; // consecutive connect/read failures
            let mut tcp_seen = false;
            let mut tick: u32 = 0;
            let mut last_log = Instant::now() - Duration::from_secs(10);
            while polling.load(Ordering::Relaxed) {
                let mut used_tcp = false;
                // TCP fast path: keep trying through kbrun's startup (~6 s of
                // failed ticks), then only re-probe occasionally (~every 10 s).
                if let Some(fs) = stream.as_ref().filter(|_| tcp_fail < 15 || tick % 25 == 0) {
                    match fs.fetch(Duration::from_secs(2)) {
                        Ok(f) => {
                            tcp_fail = 0;
                            used_tcp = true;
                            if !tcp_seen {
                                tcp_seen = true;
                                let _ = tx.send(Msg::BoardNote("frame feed: TCP".into()));
                            }
                            let _ = tx.send(Msg::BoardFrame { w: f.w, h: f.h, rgb: f.rgb });
                            ctx.request_repaint();
                        }
                        Err(_) => tcp_fail += 1,
                    }
                }
                // fallback: pull the tmpfs frame file
                if !used_tcp && t.pull("/tmp/kbrun_frame.rgb", &frame_local).is_ok() {
                    if let Ok(data) = std::fs::read(&frame_local) {
                        if let Some((w, h, rgb)) = parse_preview(&data) {
                            let _ = tx.send(Msg::BoardFrame { w, h, rgb });
                            ctx.request_repaint();
                        }
                    }
                }
                // result line every ~2 s
                if last_log.elapsed() >= Duration::from_secs(2) {
                    last_log = Instant::now();
                    match t.exec("tail -n 3 /tmp/kbrun.log 2>/dev/null; true", 15) {
                        Ok(r) => {
                            for line in r.output.lines().rev() {
                                if let Ok(v) = serde_json::from_str::<serde_json::Value>(line.trim()) {
                                    if v["event"] == "result" {
                                        let _ = tx.send(Msg::BoardResult(v));
                                        ctx.request_repaint();
                                        break;
                                    }
                                }
                            }
                        }
                        Err(e) => {
                            let _ = tx.send(Msg::BoardNote(format!("log poll: {e}")));
                            ctx.request_repaint();
                        }
                    }
                }
                tick += 1;
                // the TCP server already paces us (it waits for a fresh frame);
                // the pull path keeps its old cadence
                std::thread::sleep(Duration::from_millis(if used_tcp { 5 } else { 400 }));
            }
            let _ = std::fs::remove_file(&frame_local);
        });
    }
}
