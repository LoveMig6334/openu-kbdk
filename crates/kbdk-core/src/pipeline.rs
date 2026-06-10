//! Shared runner for the py/ uv workspace scripts (kbdk-train, kbdk-convert).
//! Streams the scripts' JSON-lines progress to a callback so the CLI can print
//! them and the UI can chart them from one implementation.

use anyhow::{bail, Context, Result};
use std::io::BufRead;
use std::path::Path;
use std::process::{Child, Command, Stdio};

#[derive(Debug, Clone)]
pub enum PyEvent {
    /// A parsed JSON-line progress event ({"event": ..., ...}).
    Line(serde_json::Value),
    /// Non-JSON stdout chatter (tool logs etc).
    Raw(String),
    /// Process finished with this exit code.
    Exited(i32),
}

/// Spawn `uv run <script> <args...>` inside `<repo_root>/py`.
pub fn spawn_py_script(repo_root: &Path, script: &str, args: &[String]) -> Result<Child> {
    Command::new("uv")
        .current_dir(repo_root.join("py"))
        .arg("run")
        .arg(script)
        .args(args)
        .stdout(Stdio::piped())
        .spawn()
        .map_err(|e| anyhow::anyhow!("spawn uv: {e} (is uv installed?)"))
}

/// Stream a spawned child's stdout into `on_event` until it exits.
/// Returns Err on non-zero exit (after emitting Exited).
pub fn stream_child(mut child: Child, on_event: &mut dyn FnMut(PyEvent)) -> Result<()> {
    let stdout = child.stdout.take().context("child has no stdout")?;
    for line in std::io::BufReader::new(stdout).lines() {
        let line = line?;
        match serde_json::from_str::<serde_json::Value>(&line) {
            Ok(v) if v.is_object() => on_event(PyEvent::Line(v)),
            _ => on_event(PyEvent::Raw(line)),
        }
    }
    let st = child.wait()?;
    let rc = st.code().unwrap_or(-1);
    on_event(PyEvent::Exited(rc));
    if rc != 0 {
        bail!("script exited with rc={rc}");
    }
    Ok(())
}

/// Convenience: spawn + stream in one call.
pub fn run_py_script(
    repo_root: &Path,
    script: &str,
    args: &[String],
    on_event: &mut dyn FnMut(PyEvent),
) -> Result<()> {
    let child = spawn_py_script(repo_root, script, args)?;
    stream_child(child, on_event)
}

/// One-line human summary of a JSON event (shared by CLI stderr + UI status line).
pub fn summarize(v: &serde_json::Value) -> String {
    let mut parts = vec![];
    for (k, val) in v.as_object().into_iter().flatten() {
        if k != "event" && !val.is_null() {
            parts.push(format!("{k}={val}"));
        }
    }
    parts.join(" ")
}

#[cfg(test)]
mod tests {
    use super::*;

    fn run_sh(cmd: &str) -> (Vec<PyEvent>, Result<()>) {
        let child = Command::new("sh")
            .arg("-c")
            .arg(cmd)
            .stdout(Stdio::piped())
            .spawn()
            .unwrap();
        let mut events = vec![];
        let r = stream_child(child, &mut |e| events.push(e));
        (events, r)
    }

    #[test]
    fn parses_json_and_raw_lines() {
        let (events, r) = run_sh(r#"echo '{"event":"epoch","n":1}'; echo plain"#);
        r.unwrap();
        assert!(matches!(&events[0], PyEvent::Line(v) if v["event"] == "epoch"));
        assert!(matches!(&events[1], PyEvent::Raw(s) if s == "plain"));
        assert!(matches!(&events[2], PyEvent::Exited(0)));
    }

    #[test]
    fn nonzero_exit_is_error() {
        let (events, r) = run_sh("exit 3");
        assert!(r.is_err());
        assert!(matches!(events.last(), Some(PyEvent::Exited(3))));
    }

    #[test]
    fn summarize_skips_event_key() {
        let v: serde_json::Value = serde_json::json!({"event":"epoch","n":2,"loss":0.5});
        let s = summarize(&v);
        assert!(s.contains("n=2") && s.contains("loss=0.5") && !s.contains("event"));
    }
}
