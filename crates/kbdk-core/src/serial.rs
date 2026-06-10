//! Serial-console transport — the Rust port of src/uai.c. Slow (115200 8N1,
//! octal-printf upload ≈ 4 wire bytes per payload byte) but works with nothing
//! on the board besides the BusyBox shell; it is the fallback when adb is absent
//! and the only channel that sees boot logs. One process may hold the port at a
//! time (a leftover holder looks like exec timeouts — lsof + kill it).

use crate::protocol::{parse_transcript, wrap_command_checked};
use crate::transport::{ExecResult, Transport};
use anyhow::{bail, Context, Result};
use std::io::{Read, Write};
use std::path::Path;
use std::time::{Duration, Instant};

const CHUNK: usize = 45; // payload bytes per printf line (uai.c-compatible)

pub fn octal_chunk(bytes: &[u8]) -> String {
    bytes.iter().map(|b| format!("\\{:03o}", b)).collect()
}

/// The push wire format: first command truncates (>), the rest append (>>).
/// No base64 on this BusyBox, hence octal printf escapes.
pub fn push_commands(data: &[u8], remote: &str) -> Vec<String> {
    let mut v = Vec::new();
    for (i, chunk) in data.chunks(CHUNK).enumerate() {
        let redir = if i == 0 { ">" } else { ">>" };
        v.push(format!("printf '{}' {} {}", octal_chunk(chunk), redir, remote));
    }
    v
}

pub struct SerialTransport {
    pub port: String,
    pub baud: u32,
}

impl SerialTransport {
    pub fn new(port: &str, baud: u32) -> Self {
        Self {
            port: port.into(),
            baud,
        }
    }

    fn session(&self) -> Result<Box<dyn serialport::SerialPort>> {
        serialport::new(&self.port, self.baud)
            .timeout(Duration::from_millis(200))
            .open()
            .with_context(|| format!("open {} (another process holding it?)", self.port))
    }

    fn exec_on(
        &self,
        sp: &mut Box<dyn serialport::SerialPort>,
        cmd: &str,
        timeout_secs: u64,
    ) -> Result<ExecResult> {
        let wrapped = wrap_command_checked(cmd)?;
        sp.write_all(b"\r")?; // wake the shell
        std::thread::sleep(Duration::from_millis(100));
        let mut junk = [0u8; 4096];
        let _ = sp.read(&mut junk); // drain prompt echo
        sp.write_all(wrapped.as_bytes())?;
        sp.write_all(b"\r")?;
        let mut buf = String::new();
        let deadline = Instant::now() + Duration::from_secs(timeout_secs);
        let mut tmp = [0u8; 1024];
        loop {
            if let Ok(n) = sp.read(&mut tmp) {
                if n > 0 {
                    buf.push_str(&String::from_utf8_lossy(&tmp[..n]));
                    if let Ok(r) = parse_transcript(&buf) {
                        return Ok(r);
                    }
                }
            }
            if Instant::now() > deadline {
                bail!("serial exec timeout: {cmd} ({} bytes buffered)", buf.len());
            }
        }
    }
}

impl Transport for SerialTransport {
    fn name(&self) -> &'static str {
        "serial"
    }

    fn exec(&self, cmd: &str, timeout_secs: u64) -> Result<ExecResult> {
        let mut sp = self.session()?;
        self.exec_on(&mut sp, cmd, timeout_secs)
    }

    fn push(&self, local: &Path, remote: &str) -> Result<()> {
        let data = std::fs::read(local)?;
        let want = format!("{:x}", md5::compute(&data));
        let mut sp = self.session()?;
        for cmd in push_commands(&data, remote) {
            let r = self.exec_on(&mut sp, &cmd, 15)?;
            if r.rc != 0 {
                bail!("push chunk failed rc={}", r.rc);
            }
        }
        let r = self.exec_on(&mut sp, &format!("md5sum {remote}"), 30)?;
        let got = r.output.split_whitespace().next().unwrap_or_default();
        if got != want {
            bail!("serial push verify failed: {got} != {want}");
        }
        Ok(())
    }

    fn pull(&self, _remote: &str, _local: &Path) -> Result<()> {
        bail!("pull over serial not supported; use adb")
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn octal_escapes_every_byte() {
        assert_eq!(octal_chunk(&[0, b'A', 0xFF]), "\\000\\101\\377");
    }

    #[test]
    fn chunks_fit_serial_line() {
        let cmds = push_commands(&vec![7u8; 100], "/tmp/x");
        assert!(cmds[0].starts_with("printf '"));
        assert!(cmds[0].contains("' > /tmp/x"));
        assert!(cmds.iter().all(|c| c.len() < 250));
        assert!(cmds[1..].iter().all(|c| c.contains(">>")));
        assert_eq!(cmds.len(), 3); // 45+45+10
    }
}
