//! TCP frame feed from kbrun's listener (port 18902, reached via `adb forward`).
//! Protocol is one-shot per connection: kbrun sends "KBF1" + u16le w + u16le h
//! + w*h*3 RGB888 bytes, then closes. The server itself paces delivery (it waits
//! for a *new* preview frame before replying), so the client just reconnects in
//! a loop. This is ~10-15 fps vs ~2.5 fps for the adb-pull-a-tmpfs-file path.

use anyhow::{bail, Context, Result};
use std::io::Read;
use std::net::{Ipv4Addr, SocketAddr, TcpStream};
use std::process::Command;
use std::time::Duration;

/// kbrun's frame-server port; the same number is forwarded host->board.
pub const FRAME_PORT: u16 = 18902;

pub struct Frame {
    pub w: usize,
    pub h: usize,
    pub rgb: Vec<u8>,
}

/// Connect to 127.0.0.1:`port` and read exactly one KBF1 frame.
pub fn fetch_frame(port: u16, timeout: Duration) -> Result<Frame> {
    let addr = SocketAddr::from((Ipv4Addr::LOCALHOST, port));
    let mut s = TcpStream::connect_timeout(&addr, timeout).context("frame connect")?;
    s.set_read_timeout(Some(timeout))?;
    let mut hdr = [0u8; 8];
    s.read_exact(&mut hdr).context("frame header read")?;
    if &hdr[0..4] != b"KBF1" {
        bail!("bad frame magic {:02x?}", &hdr[0..4]);
    }
    let w = u16::from_le_bytes([hdr[4], hdr[5]]) as usize;
    let h = u16::from_le_bytes([hdr[6], hdr[7]]) as usize;
    if w == 0 || h == 0 || w > 4096 || h > 4096 {
        bail!("implausible frame dims {w}x{h}");
    }
    let mut rgb = vec![0u8; w * h * 3];
    s.read_exact(&mut rgb).context("frame pixel read")?;
    Ok(Frame { w, h, rgb })
}

/// `adb forward tcp:port tcp:port`, removed again on drop. Setting the forward
/// succeeds even when nothing listens on the board yet — only `fetch` tells you
/// whether the running kbrun actually serves frames.
pub struct FrameStream {
    serial: Option<String>,
    port: u16,
}

impl FrameStream {
    pub fn connect(serial: Option<String>, port: u16) -> Result<Self> {
        let st = adb(&serial)
            .args(["forward", &format!("tcp:{port}"), &format!("tcp:{port}")])
            .output()
            .context("spawn adb forward")?;
        if !st.status.success() {
            bail!("adb forward failed: {}", String::from_utf8_lossy(&st.stderr));
        }
        Ok(Self { serial, port })
    }

    pub fn fetch(&self, timeout: Duration) -> Result<Frame> {
        fetch_frame(self.port, timeout)
    }
}

impl Drop for FrameStream {
    fn drop(&mut self) {
        let _ = adb(&self.serial)
            .args(["forward", "--remove", &format!("tcp:{}", self.port)])
            .output();
    }
}

fn adb(serial: &Option<String>) -> Command {
    let mut c = Command::new("adb");
    if let Some(s) = serial {
        c.args(["-s", s]);
    }
    c
}
