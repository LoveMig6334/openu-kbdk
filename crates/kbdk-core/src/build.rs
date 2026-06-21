//! Edit-tab build path: cross-compile a single board C/C++ source on the host and
//! spawn it on the board over ADB. The cross toolchain + flags live in **one
//! place** — the repo `Makefile` — and are parsed out by [`parse_toolchain`] so the
//! Edit tab can never drift from the verified `hello`/`fbtest` build rules (the
//! wrong float ABI silently faults on the board; see CLAUDE.md).
//!
//! Two pure functions are the TDD core: [`parse_toolchain`] (Makefile → compiler +
//! flags) and [`parse_diagnostic`] (gcc diagnostic line → structured
//! [`Diagnostic`]). The impure shells around them — [`compile`] (spawns the
//! compiler, streams stdout+stderr merged) and [`spawn_board_run`] (spawns the
//! long-lived `adb shell` whose stdout is streamed and whose pid is the Stop
//! handle) — mirror `pipeline::stream_child`'s line loop and `adb.rs`'s argv shape.

use anyhow::{bail, Context, Result};
use std::io::BufRead;
use std::path::{Path, PathBuf};
use std::process::{Child, Command, Stdio};

/// Fallback cross-compiler driver when the `Makefile`'s `CROSS` is absent. Kept in
/// sync with the checked-in Makefile by `fallback_constants_match_checked_in_makefile`.
pub const FALLBACK_CC: &str = "arm-unknown-linux-musleabihf-gcc";
/// Fallback C++ cross-compiler driver when the `Makefile`'s `CROSSXX` is absent.
pub const FALLBACK_CXX: &str = "arm-unknown-linux-musleabihf-g++";
/// Fallback compile flags when the `Makefile`'s `CROSSFLAGS` is absent. The float
/// ABI (`-mfloat-abi=hard`) is load-bearing — the wrong ABI silently faults on the
/// board.
pub const FALLBACK_FLAGS: &str = "-O2 -mcpu=cortex-a7 -mfpu=neon-vfpv4 -mfloat-abi=hard";

/// The cross toolchain extracted from the repo `Makefile` (the single source of
/// truth for the board build).
#[derive(Debug, Clone, PartialEq)]
pub struct CrossToolchain {
    /// C cross-compiler driver (`CROSS`).
    pub cc: String,
    /// C++ cross-compiler driver (`CROSSXX`).
    pub cxx: String,
    /// Compile flags (`CROSSFLAGS`), split on whitespace into individual argv items.
    pub flags: Vec<String>,
}

/// Severity of a gcc diagnostic line.
#[derive(Debug, PartialEq, Eq, Clone, Copy)]
pub enum Level {
    Error,
    Warning,
    Note,
}

/// A parsed gcc diagnostic (`path:line[:col]: error|warning|note: message`).
#[derive(Debug, PartialEq, Eq, Clone)]
pub struct Diagnostic {
    pub file: String,
    pub line: u32,
    pub col: Option<u32>,
    pub level: Level,
    pub message: String,
}

/// Parse `CROSS`, `CROSSXX`, `CROSSFLAGS` out of `Makefile` text. Handles both the
/// `KEY ?= value` and `KEY := value` forms. Any key not present falls back to the
/// baked `FALLBACK_*` constants (which a drift test keeps in sync with the real
/// Makefile).
pub fn parse_toolchain(makefile: &str) -> CrossToolchain {
    let mut cc: Option<String> = None;
    let mut cxx: Option<String> = None;
    let mut flags: Option<String> = None;

    for raw in makefile.lines() {
        let line = raw.trim();
        if line.is_empty() || line.starts_with('#') {
            continue;
        }
        // Find the assignment operator (`:=` or `?=`), whichever comes first.
        let Some((key, val)) = split_assignment(line) else {
            continue;
        };
        match key {
            "CROSS" => cc = Some(val.to_string()),
            "CROSSXX" => cxx = Some(val.to_string()),
            "CROSSFLAGS" => flags = Some(val.to_string()),
            _ => {}
        }
    }

    let flags_str = flags.unwrap_or_else(|| FALLBACK_FLAGS.to_string());
    CrossToolchain {
        cc: cc.unwrap_or_else(|| FALLBACK_CC.to_string()),
        cxx: cxx.unwrap_or_else(|| FALLBACK_CXX.to_string()),
        flags: flags_str.split_whitespace().map(str::to_string).collect(),
    }
}

/// Split a Makefile line into `(key, value)` on the first `:=` or `?=`, trimming
/// both sides. Returns `None` if neither operator is present. The key is compared
/// **exactly** by callers (so `CROSS` never swallows `CROSSXX`/`CROSSFLAGS`).
fn split_assignment(line: &str) -> Option<(&str, &str)> {
    // Whichever operator appears earliest wins (a value could contain the other).
    let pos_colon = line.find(":=");
    let pos_qmark = line.find("?=");
    let (idx, op_len) = match (pos_colon, pos_qmark) {
        (Some(c), Some(q)) => {
            if c <= q {
                (c, 2)
            } else {
                (q, 2)
            }
        }
        (Some(c), None) => (c, 2),
        (None, Some(q)) => (q, 2),
        (None, None) => return None,
    };
    let key = line[..idx].trim();
    let val = line[idx + op_len..].trim();
    Some((key, val))
}

/// Parse a single gcc diagnostic line `path:line[:col]: error|warning|note: message`
/// into a [`Diagnostic`]. Lines without a recognized level marker, or whose
/// line/col fields aren't numeric, return `None` (plain compiler chatter, linker
/// output, empty lines).
pub fn parse_diagnostic(line: &str) -> Option<Diagnostic> {
    // Locate the level marker. gcc separates the location from the level with
    // ": " — consuming that colon here leaves `prefix` as `file:line[:col]` with
    // no trailing colon. The leading colon also disambiguates from a path that
    // happens to contain "error" etc.
    let markers = [
        (": error: ", Level::Error),
        (": warning: ", Level::Warning),
        (": note: ", Level::Note),
    ];
    let (marker, level) = markers
        .iter()
        .find_map(|(m, lvl)| line.find(m).map(|idx| ((idx, m.len()), *lvl)))?;
    let (marker_idx, marker_len) = marker;

    let prefix = &line[..marker_idx]; // file[:line[:col]]
    let message = line[marker_idx + marker_len..].to_string();

    // Peel optional col, then line, off the end of the prefix. Both must be numeric.
    let mut parts = prefix.rsplitn(3, ':');
    let last = parts.next()?; // could be col (with col) or line (no col)
    let mid = parts.next(); // line (with col) or file-tail (no col)
    let rest = parts.next(); // file (with col) or None (no col)

    let last_num: Option<u32> = last.trim().parse().ok();

    match (last_num, mid, rest) {
        // path:line:col  — last=col, mid=line, rest=file
        (Some(col), Some(line_s), Some(file)) => {
            let line_num: u32 = line_s.trim().parse().ok()?;
            Some(Diagnostic {
                file: file.to_string(),
                line: line_num,
                col: Some(col),
                level,
                message,
            })
        }
        // path:line  — last=line, mid=file (rest is None)
        (Some(line_num), Some(file), None) => Some(Diagnostic {
            file: file.to_string(),
            line: line_num,
            col: None,
            level,
            message,
        }),
        _ => None,
    }
}

/// Cross-compile a single board source file. Reads the toolchain from
/// `<repo_root>/Makefile` (falling back to the baked constants if absent), picks
/// the C vs C++ driver by the source extension (`.cc`/`.cpp`/`.cxx` → `cxx` plus
/// `-static-libstdc++ -static-libgcc`; otherwise the C driver), assembles
/// `flags … -o <out_dir>/<stem> <src> -lm <extra_args>`, spawns the compiler with
/// stdout+stderr captured, streams every line through `on_line`, and returns the
/// output binary path on success.
///
/// Linker order: `extra_args` and `-lm` go **after** the source so escape-hatch
/// flags like `-ldl -lpthread` resolve correctly.
pub fn compile(
    repo_root: &Path,
    src: &Path,
    out_dir: &Path,
    extra_args: &[String],
    on_line: &mut dyn FnMut(String),
) -> Result<PathBuf> {
    let makefile = repo_root.join("Makefile");
    let toolchain = match std::fs::read_to_string(&makefile) {
        Ok(text) => parse_toolchain(&text),
        Err(_) => parse_toolchain(""), // all fallbacks
    };

    let ext = src
        .extension()
        .and_then(|e| e.to_str())
        .unwrap_or("")
        .to_ascii_lowercase();
    let is_cpp = matches!(ext.as_str(), "cc" | "cpp" | "cxx" | "c++");
    let compiler = if is_cpp { &toolchain.cxx } else { &toolchain.cc };

    let stem = src
        .file_stem()
        .and_then(|s| s.to_str())
        .context("source file has no stem")?;
    let out_bin = out_dir.join(stem);

    let mut args: Vec<String> = toolchain.flags.clone();
    if is_cpp {
        args.push("-static-libstdc++".to_string());
        args.push("-static-libgcc".to_string());
    }
    args.push("-o".to_string());
    args.push(out_bin.to_string_lossy().into_owned());
    args.push(src.to_string_lossy().into_owned());
    // After the source so escape-hatch link flags resolve.
    args.push("-lm".to_string());
    args.extend(extra_args.iter().cloned());

    let mut child = match Command::new(compiler)
        // Run from the repo root so relative escape-hatch flags (e.g. the camera
        // template's `-Ivendor/eyesee-mpp/...` includes) resolve regardless of
        // where the GUI process was launched.
        .current_dir(repo_root)
        .args(&args)
        .stdout(Stdio::piped())
        .stderr(Stdio::piped())
        .spawn()
    {
        Ok(c) => c,
        Err(e) if e.kind() == std::io::ErrorKind::NotFound => {
            bail!(
                "cross-compiler '{compiler}' not found on PATH — install the messense \
                 musl-armhf toolchain and ensure /opt/homebrew/bin is on PATH"
            );
        }
        Err(e) => return Err(anyhow::anyhow!("spawn '{compiler}': {e}")),
    };

    // Drain stdout on a helper thread while this thread forwards stderr live, so a
    // child that fills one pipe while we are blocked reading the other cannot
    // deadlock. gcc/g++ emit diagnostics on stderr (forwarded as they arrive);
    // any stdout (rare for a single-file build, but possible via escape-hatch
    // extra_args like -v) is collected and replayed after.
    let out_rx = child.stdout.take().map(|stdout| {
        let (tx, rx) = std::sync::mpsc::channel::<String>();
        let handle = std::thread::spawn(move || {
            for line in std::io::BufReader::new(stdout).lines().map_while(Result::ok) {
                if tx.send(line).is_err() {
                    break;
                }
            }
        });
        (rx, handle)
    });
    if let Some(stderr) = child.stderr.take() {
        for line in std::io::BufReader::new(stderr).lines() {
            on_line(line?);
        }
    }
    if let Some((rx, handle)) = out_rx {
        let _ = handle.join();
        for line in rx {
            on_line(line);
        }
    }

    let st = child.wait().context("wait for compiler")?;
    let rc = st.code().unwrap_or(-1);
    if rc != 0 {
        bail!("compile failed (rc={rc})");
    }
    Ok(out_bin)
}

/// Build the `adb` argv for running `remote` on the board with `env_prefix`
/// prepended and stderr merged (`2>&1`, since legacy adbd has no shell_v2). When
/// `serial` is `Some`, the `-s SERIAL` selector is included (mirrors
/// `AdbTransport::adb()`). `env_prefix` is empty by default, or
/// `LD_LIBRARY_PATH=/usr/lib/eyesee-mpp:/usr/lib` for MPP camera/audio programs.
pub fn board_run_argv(serial: Option<&str>, remote: &str, env_prefix: &str) -> Vec<String> {
    let mut argv = Vec::new();
    if let Some(s) = serial {
        argv.push("-s".to_string());
        argv.push(s.to_string());
    }
    argv.push("shell".to_string());
    // Collapse a missing/blank env prefix to just the command (no leading space).
    let cmd = if env_prefix.trim().is_empty() {
        format!("{remote} 2>&1")
    } else {
        format!("{} {remote} 2>&1", env_prefix.trim())
    };
    argv.push(cmd);
    argv
}

/// Spawn the board program over ADB and return the live [`Child`] so the caller can
/// stream its stdout line-by-line and record `child.id()` as the Stop handle. The
/// `adb shell` session stays open for the program's lifetime — killing this host
/// child closes the session, and adbd reaps the whole board process group (the
/// board has no `setsid`/`nohup`, so this is the intended Stop mechanism).
pub fn spawn_board_run(serial: Option<&str>, remote: &str, env_prefix: &str) -> Result<Child> {
    let argv = board_run_argv(serial, remote, env_prefix);
    Command::new("adb")
        .args(&argv)
        .stdout(Stdio::piped())
        .spawn()
        .map_err(|e| {
            if e.kind() == std::io::ErrorKind::NotFound {
                anyhow::anyhow!("'adb' not found on PATH — install Android platform-tools")
            } else {
                anyhow::anyhow!("spawn adb: {e}")
            }
        })
}

#[cfg(test)]
mod tests {
    use super::*;

    // ---- parse_toolchain ----

    #[test]
    fn parses_qmark_form() {
        let mk = "CROSS ?= some-gcc\nCROSSXX ?= some-g++\nCROSSFLAGS ?= -O2 -mcpu=cortex-a7\n";
        let t = parse_toolchain(mk);
        assert_eq!(t.cc, "some-gcc");
        assert_eq!(t.cxx, "some-g++");
        assert_eq!(t.flags, vec!["-O2", "-mcpu=cortex-a7"]);
    }

    #[test]
    fn parses_colon_eq_form() {
        let mk = "CROSS := some-gcc\nCROSSXX := some-g++\nCROSSFLAGS := -O2 -mcpu=cortex-a7\n";
        let t = parse_toolchain(mk);
        assert_eq!(t.cc, "some-gcc");
        assert_eq!(t.cxx, "some-g++");
        assert_eq!(t.flags, vec!["-O2", "-mcpu=cortex-a7"]);
    }

    #[test]
    fn missing_keys_fall_back() {
        let t = parse_toolchain("");
        assert_eq!(t.cc, FALLBACK_CC);
        assert_eq!(t.cxx, FALLBACK_CXX);
        let want: Vec<String> = FALLBACK_FLAGS.split_whitespace().map(str::to_string).collect();
        assert_eq!(t.flags, want);
    }

    #[test]
    fn ignores_unrelated_lines() {
        let mk = "\
# the host compiler
CC      ?= cc
CFLAGS  ?= -O2 -Wall
# cross toolchain
CROSS       ?= arm-unknown-linux-musleabihf-gcc
CROSSFLAGS  ?= -O2 -mcpu=cortex-a7 -mfpu=neon-vfpv4 -mfloat-abi=hard
CROSSXX  ?= arm-unknown-linux-musleabihf-g++
MPPDEF  := -DAWCHIP=0x1817
";
        let t = parse_toolchain(mk);
        assert_eq!(t.cc, "arm-unknown-linux-musleabihf-gcc");
        assert_eq!(t.cxx, "arm-unknown-linux-musleabihf-g++");
        assert_eq!(
            t.flags,
            vec!["-O2", "-mcpu=cortex-a7", "-mfpu=neon-vfpv4", "-mfloat-abi=hard"]
        );
    }

    #[test]
    fn cross_does_not_match_crossxx_or_crossflags() {
        // Only CROSSXX/CROSSFLAGS present (no bare CROSS) — cc must fall back, not
        // be polluted by the longer keys.
        let mk = "CROSSXX := my-g++\nCROSSFLAGS := -Os\n";
        let t = parse_toolchain(mk);
        assert_eq!(t.cc, FALLBACK_CC);
        assert_eq!(t.cxx, "my-g++");
        assert_eq!(t.flags, vec!["-Os"]);
    }

    #[test]
    fn handles_inline_comment_lines_and_blank_lines() {
        let mk = "\n   \nCROSS ?= a-gcc\n\n";
        let t = parse_toolchain(mk);
        assert_eq!(t.cc, "a-gcc");
    }

    // ---- Makefile drift guard ----

    #[test]
    fn fallback_constants_match_checked_in_makefile() {
        let makefile = include_str!(concat!(env!("CARGO_MANIFEST_DIR"), "/../../Makefile"));
        assert!(
            makefile.contains(FALLBACK_CC),
            "FALLBACK_CC drifted from Makefile"
        );
        assert!(
            makefile.contains(FALLBACK_CXX),
            "FALLBACK_CXX drifted from Makefile"
        );
        assert!(
            makefile.contains(FALLBACK_FLAGS),
            "FALLBACK_FLAGS drifted from Makefile"
        );
    }

    // ---- parse_diagnostic ----

    #[test]
    fn parses_error_with_col() {
        let d = parse_diagnostic("src/hello.c:12:5: error: 'x' undeclared").unwrap();
        assert_eq!(
            d,
            Diagnostic {
                file: "src/hello.c".into(),
                line: 12,
                col: Some(5),
                level: Level::Error,
                message: "'x' undeclared".into(),
            }
        );
    }

    #[test]
    fn parses_warning() {
        let d = parse_diagnostic("a.c:3:1: warning: unused variable 'y'").unwrap();
        assert_eq!(d.level, Level::Warning);
        assert_eq!(d.file, "a.c");
        assert_eq!(d.line, 3);
        assert_eq!(d.col, Some(1));
        assert_eq!(d.message, "unused variable 'y'");
    }

    #[test]
    fn parses_note() {
        let d = parse_diagnostic("a.c:9: note: in expansion of macro").unwrap();
        assert_eq!(d.col, None);
        assert_eq!(d.level, Level::Note);
        assert_eq!(d.file, "a.c");
        assert_eq!(d.line, 9);
        assert_eq!(d.message, "in expansion of macro");
    }

    #[test]
    fn non_diagnostic_returns_none() {
        assert!(parse_diagnostic("plain compiler chatter").is_none());
        assert!(parse_diagnostic("").is_none());
        // Linker output: has no "path:line[:col]: level:" shape.
        assert!(parse_diagnostic("/usr/bin/ld: cannot find -lfoo").is_none());
    }

    #[test]
    fn non_numeric_line_is_none() {
        // Looks like a diagnostic but the "line" isn't a number.
        assert!(parse_diagnostic("foo.c:bar: error: nope").is_none());
    }

    #[test]
    fn windows_drive_not_misparsed() {
        // Must not panic; document the expected return. `C:\x.c:1:1` rsplits to
        // col=1, line=1, file="C:\\x.c" — accept whatever it returns, just no panic.
        let _ = parse_diagnostic("C:\\x.c:1:1: error: msg");
    }

    // ---- board_run_argv ----

    #[test]
    fn argv_no_serial_no_env() {
        let argv = board_run_argv(None, "/tmp/hello", "");
        assert_eq!(argv, vec!["shell".to_string(), "/tmp/hello 2>&1".to_string()]);
    }

    #[test]
    fn argv_with_serial_and_mpp() {
        let argv = board_run_argv(
            Some("ABC"),
            "/tmp/camcc",
            "LD_LIBRARY_PATH=/usr/lib/eyesee-mpp:/usr/lib",
        );
        assert!(argv.contains(&"-s".to_string()));
        assert!(argv.contains(&"ABC".to_string()));
        assert!(argv.contains(&"shell".to_string()));
        let cmd = argv.last().unwrap();
        assert!(cmd.starts_with("LD_LIBRARY_PATH=/usr/lib/eyesee-mpp:/usr/lib "));
        assert!(cmd.ends_with(" 2>&1"));
        assert!(cmd.contains("/tmp/camcc"));
    }

    #[test]
    fn argv_blank_env_collapses() {
        // A whitespace-only env prefix must not leave a leading space.
        let argv = board_run_argv(None, "/tmp/x", "   ");
        assert_eq!(argv.last().unwrap(), "/tmp/x 2>&1");
    }

    // ---- compile (integration, host cc via Makefile override) ----

    #[test]
    fn compile_uses_host_cc_via_override() {
        let dir = std::env::temp_dir().join(format!("kbdk_build_test_{}", std::process::id()));
        let out = dir.join("out");
        std::fs::create_dir_all(&out).unwrap();
        std::fs::write(
            dir.join("Makefile"),
            "CROSS ?= cc\nCROSSFLAGS ?= -O2\n",
        )
        .unwrap();
        let hi = dir.join("hi.c");
        std::fs::write(&hi, "int main(void){return 0;}\n").unwrap();

        let mut lines = Vec::new();
        let r = compile(&dir, &hi, &out, &[], &mut |l| lines.push(l));
        let _ = std::fs::remove_dir_all(&dir);

        let bin = r.expect("compile should succeed with host cc");
        // The file we asserted is gone (cleaned dir), so check the returned path
        // basename instead — it must be the stem under out_dir.
        assert_eq!(bin, out.join("hi"));
    }

    #[test]
    fn missing_compiler_is_clean_err() {
        let dir = std::env::temp_dir().join(format!("kbdk_build_test_nf_{}", std::process::id()));
        let out = dir.join("out");
        std::fs::create_dir_all(&out).unwrap();
        std::fs::write(
            dir.join("Makefile"),
            "CROSS ?= definitely-not-a-cc-12345\nCROSSFLAGS ?= -O2\n",
        )
        .unwrap();
        let hi = dir.join("hi.c");
        std::fs::write(&hi, "int main(void){return 0;}\n").unwrap();

        let mut lines = Vec::new();
        let r = compile(&dir, &hi, &out, &[], &mut |l| lines.push(l));
        let _ = std::fs::remove_dir_all(&dir);

        let err = r.expect_err("missing compiler should error, not panic");
        assert!(
            err.to_string().contains("not found"),
            "error should mention 'not found', got: {err}"
        );
    }
}
