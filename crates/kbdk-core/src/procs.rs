//! Board process listing + kill over the Transport. busybox `ps` omits RSS,
//! so processes are read straight from /proc in one batched shell command
//! (proven approach — the Deploy tab's perf monitor already reads /proc).

use crate::transport::Transport;
use anyhow::{bail, Result};

#[derive(Debug, Clone, PartialEq)]
pub struct Proc {
    pub pid: u32,
    pub rss_kb: u64,
    pub state: String,
    pub cmd: String,
}

/// One exec, one tab-separated line per pid: `pid \t rss_kb \t state \t cmd`.
/// cmd = /proc/<pid>/cmdline (NULs -> spaces); kernel threads have an empty
/// cmdline so fall back to the bracketed comm from /proc/<pid>/stat.
pub const PROC_SCAN_CMD: &str = "\
for d in /proc/[0-9]*; do \
  p=${d#/proc/}; \
  r=$(awk '/^VmRSS:/{print $2}' \"$d/status\" 2>/dev/null); \
  s=$(awk -F') ' '{print substr($NF,1,1)}' \"$d/stat\" 2>/dev/null); \
  c=$(tr '\\0' ' ' < \"$d/cmdline\" 2>/dev/null); \
  c=$(echo \"$c\" | sed 's/ *$//'); \
  if [ -z \"$c\" ]; then n=$(awk '{print $2}' \"$d/stat\" 2>/dev/null); c=\"$n\"; fi; \
  printf '%s\\t%s\\t%s\\t%s\\n' \"$p\" \"${r:-0}\" \"${s:-?}\" \"$c\"; \
done; true";

pub fn parse_procs(out: &str) -> Vec<Proc> {
    let mut v = Vec::new();
    for line in out.lines() {
        let mut it = line.splitn(4, '\t');
        let (Some(pid), Some(rss), Some(state), Some(cmd)) =
            (it.next(), it.next(), it.next(), it.next())
        else {
            continue;
        };
        let Ok(pid) = pid.trim().parse::<u32>() else {
            continue;
        };
        v.push(Proc {
            pid,
            rss_kb: rss.trim().parse().unwrap_or(0),
            state: state.trim().to_string(),
            cmd: cmd.trim().to_string(),
        });
    }
    v
}

pub fn list_procs(t: &dyn Transport) -> Result<Vec<Proc>> {
    let r = t.exec(PROC_SCAN_CMD, 15)?;
    if r.rc != 0 {
        bail!("proc scan rc={} ({})", r.rc, r.output.trim());
    }
    let mut v = parse_procs(&r.output);
    v.sort_by(|a, b| b.rss_kb.cmp(&a.rss_kb)); // heaviest first
    Ok(v)
}

pub fn kill(t: &dyn Transport, pid: u32, sig: i32) -> Result<()> {
    let r = t.exec(&format!("kill -{sig} {pid}"), 15)?;
    if r.rc != 0 {
        bail!("kill -{sig} {pid} rc={} ({})", r.rc, r.output.trim());
    }
    Ok(())
}

#[cfg(test)]
mod tests {
    use super::*;

    // tab-separated: pid \t rss_kb \t state \t cmd
    const PS: &str = "\
1\t1240\tS\t/sbin/init
412\t14208\tS\t/tmp/kbrun /mnt/UDISK/kbdk/toy3 320x240
7\t0\tS\t[kworker/0:0]
notanumber\t0\tS\tjunk
";

    #[test]
    fn parse_procs_reads_rows() {
        let v = parse_procs(PS);
        assert_eq!(v.len(), 3); // the "notanumber" row is skipped
        assert_eq!(v[1].pid, 412);
        assert_eq!(v[1].rss_kb, 14208);
        assert_eq!(v[1].state, "S");
        assert_eq!(v[1].cmd, "/tmp/kbrun /mnt/UDISK/kbdk/toy3 320x240");
        // kernel thread: empty cmdline fell back to comm in brackets
        assert_eq!(v[2].cmd, "[kworker/0:0]");
    }

    #[test]
    fn parse_procs_tolerates_empty() {
        assert!(parse_procs("").is_empty());
    }
}
