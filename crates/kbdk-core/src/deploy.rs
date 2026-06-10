//! Pack deployment + runner lifecycle on the board.

use crate::{pack::Manifest, transport::Transport};
use anyhow::{bail, Result};
use std::path::Path;

pub const BOARD_PACK_ROOT: &str = "/mnt/UDISK/kbdk";
pub const RUNNER: &str = "/tmp/kbrun";

/// Push a local pack directory to /mnt/UDISK/kbdk/<name>; returns the remote dir.
pub fn deploy_pack(t: &dyn Transport, local_dir: &Path) -> Result<String> {
    let m = Manifest::load(local_dir)?;
    m.verify(local_dir)?;
    let remote = format!("{BOARD_PACK_ROOT}/{}", m.name);
    let r = t.exec(&format!("mount -o remount,rw /mnt/UDISK 2>/dev/null; mkdir -p {remote}"), 15)?;
    if r.rc != 0 {
        bail!("mkdir {remote} failed rc={} ({})", r.rc, r.output.trim());
    }
    for rel in [
        "manifest.json",
        m.files.param.as_str(),
        m.files.bin.as_str(),
        m.files.labels_file.as_str(),
    ] {
        // absolute paths = stock model files already on the board; don't push
        if rel.starts_with('/') {
            continue;
        }
        t.push(&local_dir.join(rel), &format!("{remote}/{rel}"))?;
    }
    Ok(remote)
}

/// Push the runner binary itself (bin/kbrun) to /tmp and chmod it.
pub fn deploy_runner(t: &dyn Transport, local_kbrun: &Path) -> Result<()> {
    t.push(local_kbrun, RUNNER)?;
    let r = t.exec(&format!("chmod +x {RUNNER}"), 15)?;
    if r.rc != 0 {
        bail!("chmod failed rc={}", r.rc);
    }
    Ok(())
}

/// Start the runner. adbd kills the session's whole process group on close, so
/// shell backgrounding cannot survive; instead kbrun self-daemonizes
/// (KBRUN_DAEMON=1 → fork + setsid + writes /tmp/kbrun.pid) and this command
/// returns when the parent exits.
pub fn start_runner(t: &dyn Transport, remote_pack: &str, res: &str, nframes: u32) -> Result<()> {
    let cmd = format!(
        "rm -f /tmp/kbrun.pid; KBRUN_DAEMON=1 LD_LIBRARY_PATH=/usr/lib/eyesee-mpp:/usr/lib \
         {RUNNER} {remote_pack} {res} {nframes} < /dev/null > /tmp/kbrun.log 2>/tmp/kbrun.err; true"
    );
    let r = t.exec(&cmd, 15)?;
    if r.rc != 0 {
        bail!("start failed rc={}", r.rc);
    }
    Ok(())
}

pub fn stop_runner(t: &dyn Transport) -> Result<()> {
    t.exec(
        "[ -f /tmp/kbrun.pid ] && kill $(cat /tmp/kbrun.pid) 2>/dev/null; rm -f /tmp/kbrun.pid; true",
        15,
    )?;
    Ok(())
}
