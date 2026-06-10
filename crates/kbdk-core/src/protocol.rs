//! Sentinel exec protocol shared by every transport (adb has no shell_v2 on this
//! board's ancient adbd, and the serial console never had exit codes), so the rc
//! travels in-band: `echo 'KB''_BEGIN'; cmd; echo 'KB''_END_'$?`. The quote-split
//! keeps the literal marker out of the board's command echo.

use crate::transport::ExecResult;
use anyhow::{bail, Context, Result};

pub const BEGIN: &str = "KB_BEGIN";
pub const END: &str = "KB_END_";

/// Quote-split so the *echoed command line* never contains the literal marker.
pub fn wrap_command(cmd: &str) -> String {
    format!("echo 'KB''_BEGIN'; {cmd}; echo 'KB''_END_'$?")
}

pub fn wrap_command_checked(cmd: &str) -> Result<String> {
    if cmd.trim_end().ends_with('&') {
        bail!("command must not end in a bare '&' (rc capture breaks); use '(cmd) & true'");
    }
    Ok(wrap_command(cmd))
}

pub fn parse_transcript(raw: &str) -> Result<ExecResult> {
    let begin = raw
        .find(&format!("{BEGIN}\r\n"))
        .or_else(|| raw.find(&format!("{BEGIN}\n")))
        .context("BEGIN sentinel not found")?;
    let after = &raw[begin + BEGIN.len()..];
    let after = after.trim_start_matches(['\r', '\n']);
    let end = after.find(END).context("END sentinel not found")?;
    let output = after[..end].to_string();
    let rc_str: String = after[end + END.len()..]
        .chars()
        .take_while(|c| c.is_ascii_digit())
        .collect();
    let rc: i32 = rc_str.parse().context("rc parse")?;
    Ok(ExecResult { output, rc })
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn wrap_splits_sentinel_tokens() {
        let w = wrap_command("uname -a");
        // The literal tokens KB_BEGIN / KB_END_ must NOT appear in the wrapped
        // command (quote-split), so the board's command echo can't fool the parser.
        assert!(!w.contains("KB_BEGIN"));
        assert!(!w.contains("KB_END_"));
        assert!(w.contains("uname -a"));
    }

    #[test]
    fn parse_extracts_output_and_rc() {
        let raw = "echo 'KB''_BEGIN'; uname -a; echo 'KB''_END_'$?\r\nKB_BEGIN\r\nLinux sipeed 4.9.118 armv7l\r\nKB_END_0\r\n";
        let r = parse_transcript(raw).unwrap();
        assert_eq!(r.rc, 0);
        assert_eq!(r.output.trim(), "Linux sipeed 4.9.118 armv7l");
    }

    #[test]
    fn parse_nonzero_rc() {
        let raw = "KB_BEGIN\r\nls: bad: No such file or directory\r\nKB_END_1\r\n";
        assert_eq!(parse_transcript(raw).unwrap().rc, 1);
    }

    #[test]
    fn parse_missing_end_is_error() {
        assert!(parse_transcript("KB_BEGIN\r\npartial...").is_err());
    }

    #[test]
    fn rejects_trailing_ampersand() {
        assert!(wrap_command_checked("sleep 5 &").is_err());
        assert!(wrap_command_checked("(sleep 5) & true").is_ok());
    }
}
