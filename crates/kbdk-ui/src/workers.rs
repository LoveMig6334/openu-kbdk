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
    /// Periodic board health sample (every ~2 s while running): 1-min load,
    /// MemAvailable kB, kbrun VmRSS kB, and kbrun's frame/infer counters
    /// (host turns the counters into rates).
    BoardStats { load1: f32, mem_kb: u64, rss_kb: u64, frames: u64, infers: u64 },
    /// Full class-probability vector (classification packs; every ~2 s).
    BoardProbs(Vec<f32>),
    BoardNote(String),
    ProcList(Vec<kbdk_core::procs::Proc>),
    Killed { pid: u32 },
    OpError { context: String, message: String },
}

/// One exec per poll tick: last result lines + the KBSTAT health sample
/// (see parse_kbstat). kbrun logs "frame N … infers=M" to stderr every 30
/// frames, so the counters lag at most a second.
const LOG_STAT_CMD: &str = concat!(
    "tail -n 3 /tmp/kbrun.log 2>/dev/null; ",
    "echo KBSTAT $(cut -d' ' -f1 /proc/loadavg) ",
    "$(awk '/MemAvailable/{print $2}' /proc/meminfo) ",
    "$(p=$(pidof kbrun); ",
    "if [ -n \"$p\" ]; then awk '/VmRSS/{print $2}' /proc/${p%% *}/status 2>/dev/null || echo 0; ",
    "else echo 0; fi) ",
    "$(tail -n 8 /tmp/kbrun.err 2>/dev/null | ",
    "awk '/infers=/{f=$2; n=$NF; sub(/infers=/,\"\",n); i=n} END{printf \"%d %d\", f+0, i+0}'); ",
    "true",
);

/// Parse the board stat line the log poller requests:
/// "KBSTAT <load1> <mem_avail_kb> <kbrun_rss_kb> <frames> <infers>"
/// (loadavg + meminfo + kbrun VmRSS + the frame/infer counters kbrun logs
/// to stderr every 30 frames). Returns None unless all five fields parse.
pub fn parse_kbstat(line: &str) -> Option<(f32, u64, u64, u64, u64)> {
    let mut it = line.trim().split_whitespace();
    if it.next() != Some("KBSTAT") {
        return None;
    }
    Some((
        it.next()?.parse().ok().filter(|v: &f32| v.is_finite())?,
        it.next()?.parse().ok()?,
        it.next()?.parse().ok()?,
        it.next()?.parse().ok()?,
        it.next()?.parse().ok()?,
    ))
}

/// Parse kbrun's full class-probability file: "KBP1" + u16le n + n f32le.
/// (The result JSON only carries the top 5; this carries the whole softmax
/// for the UI's all-classes list.)
pub fn parse_probs(data: &[u8]) -> Option<Vec<f32>> {
    if data.len() < 6 || &data[0..4] != b"KBP1" {
        return None;
    }
    let n = u16::from_le_bytes([data[4], data[5]]) as usize;
    if n == 0 || data.len() < 6 + n * 4 {
        return None;
    }
    Some(
        data[6..6 + n * 4]
            .chunks_exact(4)
            .map(|c| f32::from_le_bytes([c[0], c[1], c[2], c[3]]))
            .collect(),
    )
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
            let probs_local = std::env::temp_dir().join(format!("kbdk_probs_{}.bin", std::process::id()));
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
                // result line + board health sample every ~2 s
                if last_log.elapsed() >= Duration::from_secs(2) {
                    last_log = Instant::now();
                    match t.exec(LOG_STAT_CMD, 15) {
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
                            if let Some(s) = r.output.lines().rev().find_map(parse_kbstat) {
                                let _ = tx.send(Msg::BoardStats {
                                    load1: s.0, mem_kb: s.1, rss_kb: s.2, frames: s.3, infers: s.4,
                                });
                                ctx.request_repaint();
                            }
                            // full class probabilities (absent for detection packs)
                            if t.pull("/tmp/kbrun_probs.bin", &probs_local).is_ok() {
                                if let Some(p) =
                                    std::fs::read(&probs_local).ok().and_then(|d| parse_probs(&d))
                                {
                                    let _ = tx.send(Msg::BoardProbs(p));
                                    ctx.request_repaint();
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
            let _ = std::fs::remove_file(&probs_local);
        });
    }
}

#[cfg(test)]
mod tests {
    use super::parse_kbstat;

    #[test]
    fn kbstat_line_parses() {
        let s = parse_kbstat("KBSTAT 0.93 37348 15208 120 121").unwrap();
        assert_eq!(s, (0.93, 37348, 15208, 120, 121));
    }

    #[test]
    fn probs_file_parses() {
        let mut d = b"KBP1\x03\x00".to_vec();
        for v in [0.7f32, 0.2, 0.1] {
            d.extend_from_slice(&v.to_le_bytes());
        }
        let p = super::parse_probs(&d).unwrap();
        assert_eq!(p, vec![0.7, 0.2, 0.1]);
        assert!(super::parse_probs(b"KBF1xxxx").is_none());
        assert!(super::parse_probs(&d[..10]).is_none()); // truncated
    }

    #[test]
    fn kbstat_rejects_garbage() {
        assert!(parse_kbstat("KBSTAT 0.93 nan").is_none());
        assert!(parse_kbstat("frame 120 avgY= 23").is_none());
        // idle board: no kbrun.pid / no err file -> zeros still parse
        assert_eq!(parse_kbstat("KBSTAT 0.05 40000 0 0 0").unwrap(), (0.05, 40000, 0, 0, 0));
    }
}
