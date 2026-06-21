//! Board filesystem ops over the Transport. Listing is one `ls -la <dir>` per
//! directory (lazy, never recursive). Binary content is never round-tripped
//! through exec (lossy UTF-8) — preview fetches into a tmpfs file then pulls it.

use crate::transport::Transport;
use anyhow::{bail, Result};
use std::path::Path;

#[derive(Debug, Clone, PartialEq)]
pub struct DirEntry {
    pub name: String,
    pub is_dir: bool,
    pub size: u64,
    pub mode: String,
}

/// Strip ANSI SGR escape sequences (`ESC [ … m`) — some busybox `ls` builds
/// colorize names by default, which would otherwise land inside DirEntry.name.
pub fn strip_ansi(s: &str) -> String {
    let mut out = String::with_capacity(s.len());
    let mut chars = s.chars();
    while let Some(c) = chars.next() {
        if c == '\u{1b}' {
            // consume up to and including the final 'm' of the escape
            for e in chars.by_ref() {
                if e == 'm' {
                    break;
                }
            }
        } else {
            out.push(c);
        }
    }
    out
}

/// Parse busybox `ls -la` output. Drops the `total` line and `.`/`..`.
/// Defensive: lines that don't look like a long listing are skipped.
pub fn parse_ls(out: &str) -> Vec<DirEntry> {
    let mut v = Vec::new();
    for raw in out.lines() {
        let line = strip_ansi(raw);
        let cols: Vec<&str> = line.split_whitespace().collect();
        // perms links owner group size mon day time name...  => >= 9 cols
        if cols.len() < 9 || cols[0] == "total" {
            continue;
        }
        let mode = cols[0];
        // first char of perms: 'd' dir, 'l' symlink, '-' file, etc.
        let kind = mode.as_bytes()[0];
        if kind != b'd' && kind != b'l' && kind != b'-' {
            continue; // not a recognisable entry line
        }
        // name = everything from column 8 onward (handles spaces in names)
        let name_part = cols[8..].join(" ");
        // symlink: "name -> target" — keep just the name
        let name = name_part
            .split(" -> ")
            .next()
            .unwrap_or(&name_part)
            .to_string();
        if name == "." || name == ".." {
            continue;
        }
        let size = cols[4].parse::<u64>().unwrap_or(0);
        v.push(DirEntry {
            name,
            is_dir: kind == b'd',
            size,
            mode: mode.to_string(),
        });
    }
    v
}

/// Heuristic: a NUL byte, or >30% control chars (excluding tab/newline/CR),
/// means binary. Empty = text.
pub fn looks_binary(bytes: &[u8]) -> bool {
    if bytes.is_empty() {
        return false;
    }
    if bytes.contains(&0) {
        return true;
    }
    let ctrl = bytes
        .iter()
        .filter(|&&b| b < 0x09 || (b > 0x0d && b < 0x20))
        .count();
    ctrl * 100 / bytes.len() > 30
}

/// Join a board path and a child name without doubling the slash at root.
pub fn join_path(parent: &str, name: &str) -> String {
    if parent == "/" {
        format!("/{name}")
    } else {
        format!("{}/{name}", parent.trim_end_matches('/'))
    }
}

/// Single-quote a path for the board shell (board paths here never contain `'`).
fn q(path: &str) -> String {
    format!("'{}'", path.replace('\'', "'\\''"))
}

pub fn list_dir(t: &dyn Transport, path: &str) -> Result<Vec<DirEntry>> {
    let r = t.exec(&format!("ls -la {}", q(path)), 15)?;
    if r.rc != 0 {
        bail!("ls {path} rc={} ({})", r.rc, r.output.trim());
    }
    Ok(parse_ls(&r.output))
}

/// Fetch up to `max_bytes` of a board file into `local` (binary-safe) and
/// return the bytes. Uses `head -c` into tmpfs, then pulls the truncated copy.
pub fn read_head(t: &dyn Transport, path: &str, max_bytes: usize, local: &Path) -> Result<Vec<u8>> {
    let tmp = "/tmp/kbdk_preview.bin";
    let r = t.exec(
        &format!("head -c {max_bytes} {} > {tmp} 2>/dev/null && echo KBDK_OK", q(path)),
        15,
    )?;
    if !r.output.contains("KBDK_OK") {
        bail!("read {path}: {}", r.output.trim());
    }
    t.pull(tmp, local)?;
    Ok(std::fs::read(local)?)
}

pub fn remove(t: &dyn Transport, path: &str, is_dir: bool) -> Result<()> {
    let flag = if is_dir { "-rf" } else { "-f" };
    let r = t.exec(&format!("rm {flag} {}", q(path)), 15)?;
    if r.rc != 0 {
        bail!("rm {path} rc={} ({})", r.rc, r.output.trim());
    }
    Ok(())
}

pub fn chmod(t: &dyn Transport, path: &str, mode: &str) -> Result<()> {
    let r = t.exec(&format!("chmod {mode} {}", q(path)), 15)?;
    if r.rc != 0 {
        bail!("chmod {mode} {path} rc={} ({})", r.rc, r.output.trim());
    }
    Ok(())
}

pub fn mkdir(t: &dyn Transport, path: &str) -> Result<()> {
    let r = t.exec(&format!("mkdir -p {}", q(path)), 15)?;
    if r.rc != 0 {
        bail!("mkdir {path} rc={} ({})", r.rc, r.output.trim());
    }
    Ok(())
}

#[cfg(test)]
mod tests {
    use super::*;

    const LS: &str = "\
total 28
drwxr-xr-x    3 root     root          4096 Jun 10 12:00 .
drwxr-xr-x    5 root     root          4096 Jun 10 11:00 ..
-rwxr-xr-x    1 root     root         38000 Jun 10 12:00 fbtest
drwxr-xr-x    2 root     root          4096 Jun 10 12:01 sub dir
lrwxrwxrwx    1 root     root             7 Jun 10 12:00 sh -> busybox
";

    #[test]
    fn parse_ls_extracts_entries() {
        let v = parse_ls(LS);
        // `.` and `..` and the `total` line are dropped
        let names: Vec<&str> = v.iter().map(|e| e.name.as_str()).collect();
        assert_eq!(names, ["fbtest", "sub dir", "sh"]);

        let fbtest = &v[0];
        assert!(!fbtest.is_dir);
        assert_eq!(fbtest.size, 38000);
        assert_eq!(fbtest.mode, "-rwxr-xr-x");

        assert!(v[1].is_dir); // "sub dir" — name with a space survives
        assert_eq!(v[1].name, "sub dir");

        // symlink: target stripped, treated as a file
        assert_eq!(v[2].name, "sh");
        assert!(!v[2].is_dir);
    }

    #[test]
    fn parse_ls_tolerates_junk() {
        assert!(parse_ls("").is_empty());
        assert!(parse_ls("ls: /nope: No such file or directory").is_empty());
    }

    #[test]
    fn parse_ls_strips_ansi_color() {
        // some busybox `ls` builds colorize names: ESC[1;34m<name>ESC[0m
        let colored = "drwxr-xr-x    2 root root 4096 Jun 10 12:00 \u{1b}[1;34mkbdk\u{1b}[0m\n\
                       -rw-r--r--    1 root root   12 Jun 10 12:00 \u{1b}[0mnotes.txt\u{1b}[0m";
        let v = parse_ls(colored);
        assert_eq!(v.len(), 2);
        assert_eq!(v[0].name, "kbdk");
        assert!(v[0].is_dir);
        assert_eq!(v[1].name, "notes.txt");
        assert_eq!(v[1].size, 12);
        // strip_ansi leaves plain text untouched
        assert_eq!(strip_ansi("plain"), "plain");
    }

    #[test]
    fn looks_binary_detects_nul_and_text() {
        assert!(looks_binary(b"\x7fELF\x01\x01\x00"));
        assert!(looks_binary(&[0u8, 1, 2, 3]));
        assert!(!looks_binary(b"hello\nworld\n"));
        assert!(!looks_binary(b"")); // empty = treat as text
        // control-char ratio branch (no NUL): >30% control bytes = binary
        assert!(looks_binary(b"\x01\x02\x03\x04\x05"));
        // mostly-text with one control byte stays text
        assert!(!looks_binary(b"normal text line\x01\n"));
    }

    #[test]
    fn join_path_handles_root() {
        assert_eq!(join_path("/", "tmp"), "/tmp");
        assert_eq!(join_path("/tmp", "x"), "/tmp/x");
    }
}
