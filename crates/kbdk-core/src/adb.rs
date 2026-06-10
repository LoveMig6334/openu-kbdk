//! ADB transport: the fast path over USB-OTG (the board runs adbd on FunctionFS).
//! adbd here predates shell_v2 — no exit codes, stderr merged — so exec goes
//! through the sentinel protocol, and every push is md5-verified because the
//! board's /mnt/UDISK vfat is flaky (silent corruption + I/O errors observed).

use crate::protocol::{parse_transcript, wrap_command_checked};
use crate::transport::{ExecResult, Transport};
use anyhow::{bail, Context, Result};
use std::path::Path;
use std::process::Command;

pub struct AdbTransport {
    pub serial: Option<String>, // adb -s
}

impl AdbTransport {
    pub fn new(serial: Option<String>) -> Self {
        Self { serial }
    }

    fn adb(&self) -> Command {
        let mut c = Command::new("adb");
        if let Some(s) = &self.serial {
            c.args(["-s", s]);
        }
        c
    }

    fn raw_shell(&self, cmd: &str) -> Result<String> {
        let out = self.adb().arg("shell").arg(cmd).output().context("spawn adb")?;
        if !out.status.success() {
            bail!("adb shell failed: {}", String::from_utf8_lossy(&out.stderr));
        }
        Ok(String::from_utf8_lossy(&out.stdout).into_owned())
    }

    fn board_md5(&self, remote: &str) -> Result<String> {
        let r = self.exec(&format!("md5sum {remote}"), 60)?;
        if r.rc != 0 {
            bail!("md5sum {remote} rc={} ({})", r.rc, r.output.trim());
        }
        Ok(r.output.split_whitespace().next().unwrap_or_default().to_string())
    }
}

impl Transport for AdbTransport {
    fn name(&self) -> &'static str {
        "adb"
    }

    fn exec(&self, cmd: &str, _timeout_secs: u64) -> Result<ExecResult> {
        let wrapped = wrap_command_checked(cmd)?;
        let raw = self.raw_shell(&wrapped)?;
        parse_transcript(&raw)
    }

    fn push(&self, local: &Path, remote: &str) -> Result<()> {
        let want = format!("{:x}", md5::compute(std::fs::read(local)?));
        for attempt in 1..=3 {
            let st = self
                .adb()
                .arg("push")
                .arg(local)
                .arg(remote)
                .output()
                .context("spawn adb push")?;
            if st.status.success() {
                match self.board_md5(remote) {
                    Ok(got) if got == want => return Ok(()),
                    Ok(got) => eprintln!(
                        "push verify mismatch (attempt {attempt}): {got} != {want} — flaky UDISK vfat?"
                    ),
                    Err(e) => eprintln!("push verify failed (attempt {attempt}): {e}"),
                }
            }
        }
        bail!(
            "push {} -> {remote}: md5 verify failed after 3 attempts",
            local.display()
        );
    }

    fn pull(&self, remote: &str, local: &Path) -> Result<()> {
        let st = self.adb().arg("pull").arg(remote).arg(local).output()?;
        if !st.status.success() {
            bail!("adb pull failed: {}", String::from_utf8_lossy(&st.stderr));
        }
        Ok(())
    }
}
