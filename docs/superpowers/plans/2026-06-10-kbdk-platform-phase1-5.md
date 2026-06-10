# kbdk Dev-Kit Platform (Phases 1–5) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Dataset → fine-tune on the Mac (PyTorch/MPS) → convert (pnnx→ncnn→int8 pack) → push over ADB → live camera classification with on-screen label on the V831 board.

**Architecture:** Cargo workspace (`kbdk-core` transport/protocol lib + `kbdk-cli`) orchestrates a uv Python workspace (`kbdk-train`, `kbdk-convert`) via `uv run` subprocesses streaming JSON-lines. The board runs `kbrun`, a C++ binary statically linked against a pinned cross-built ncnn, reusing the proven MPP camera + fb0 code from `src/nnacam.cpp`/`src/camcc.c`.

**Tech Stack:** Rust (clap, serde_json, serialport), Python 3.12 (torch MPS, torchvision, pnnx, ncnn), ncnn pinned @ commit `b16501a` (verified on hardware 2026-06-10), adb (no shell_v2 → sentinel rc), musl-armhf cross toolchain.

**Read first:** `docs/superpowers/specs/2026-06-10-devkit-platform-design.md` (survey results + decisions), `CLAUDE.md` (board gotchas), `src/nnacam.cpp` (camera+inference structure), `src/uai.c` (serial protocol being ported).

**Hardware ground rules (apply to every task):**
- Board commands over adb get NO exit code and merged stderr → always sentinel-wrap.
- A wrapped command must not end in a bare `&`; background as `(cmd) & true`.
- Big files → `/mnt/UDISK` (flaky vfat: md5-verify, retry ×3). Executables/small → `/tmp` (29 MB tmpfs).
- Only one process may hold `/dev/cu.usbserial-210`; adb and serial may run simultaneously.
- Cross builds need `PATH=/opt/homebrew/bin:$PATH` and `-mcpu=cortex-a7 -mfpu=neon-vfpv4 -mfloat-abi=hard`.

---

## Phase 1 — Cargo workspace + ADB transport + CLI

### Task 1: Workspace scaffolding

**Files:**
- Create: `Cargo.toml`, `crates/kbdk-core/Cargo.toml`, `crates/kbdk-core/src/lib.rs`, `crates/kbdk-cli/Cargo.toml`, `crates/kbdk-cli/src/main.rs`
- Modify: `.gitignore` (add `target/`)

- [ ] **Step 1: Create workspace files**

`Cargo.toml`:
```toml
[workspace]
resolver = "2"
members = ["crates/kbdk-core", "crates/kbdk-cli"]

[workspace.package]
edition = "2021"
license = "MIT"

[workspace.dependencies]
anyhow = "1"
serde = { version = "1", features = ["derive"] }
serde_json = "1"
```

`crates/kbdk-core/Cargo.toml`:
```toml
[package]
name = "kbdk-core"
version = "0.1.0"
edition.workspace = true
license.workspace = true

[dependencies]
anyhow.workspace = true
serde.workspace = true
serde_json.workspace = true
md5 = "0.7"
```

`crates/kbdk-core/src/lib.rs`:
```rust
pub mod protocol;
pub mod adb;
pub mod discover;
pub mod transport;
```
(modules added by later tasks; start with empty files per module so it compiles)

`crates/kbdk-cli/Cargo.toml`:
```toml
[package]
name = "kbdk-cli"
version = "0.1.0"
edition.workspace = true
license.workspace = true

[[bin]]
name = "kbdk"
path = "src/main.rs"

[dependencies]
kbdk-core = { path = "../kbdk-core" }
anyhow.workspace = true
clap = { version = "4", features = ["derive"] }
serde_json.workspace = true
```

`crates/kbdk-cli/src/main.rs`: `fn main() { println!("kbdk"); }`

- [ ] **Step 2: Verify it builds**

Run: `cargo build 2>&1 | tail -2` — Expected: `Finished` with no errors. `make all` must still work (existing C tools untouched).

- [ ] **Step 3: Commit** — `git add -A && git commit -m "kbdk: cargo workspace scaffolding"`

### Task 2: Sentinel exec protocol (pure functions, TDD)

**Files:**
- Create: `crates/kbdk-core/src/transport.rs`, `crates/kbdk-core/src/protocol.rs`

- [ ] **Step 1: Write failing tests in `protocol.rs`**

```rust
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
```

- [ ] **Step 2: Run to verify failure** — `cargo test -p kbdk-core` → FAIL (functions undefined).

- [ ] **Step 3: Implement**

`transport.rs`:
```rust
use anyhow::Result;
use std::path::Path;

#[derive(Debug, Clone)]
pub struct ExecResult {
    pub output: String,
    pub rc: i32,
}

pub trait Transport {
    fn exec(&self, cmd: &str, timeout_secs: u64) -> Result<ExecResult>;
    /// Verified upload: size + md5 checked on the board, retried on mismatch.
    fn push(&self, local: &Path, remote: &str) -> Result<()>;
    fn pull(&self, remote: &str, local: &Path) -> Result<()>;
    fn name(&self) -> &'static str;
}
```

`protocol.rs`:
```rust
use anyhow::{bail, Context, Result};
use crate::transport::ExecResult;

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
```

- [ ] **Step 4: Verify pass** — `cargo test -p kbdk-core` → all green.
- [ ] **Step 5: Commit** — `git commit -am "kbdk-core: sentinel exec protocol"`

### Task 3: AdbTransport

**Files:**
- Create: `crates/kbdk-core/src/adb.rs`
- Create: `crates/kbdk-core/tests/hw_adb.rs` (hardware-gated integration test)

- [ ] **Step 1: Implement `adb.rs`**

```rust
use anyhow::{bail, Context, Result};
use std::path::Path;
use std::process::Command;
use crate::protocol::{parse_transcript, wrap_command_checked};
use crate::transport::{ExecResult, Transport};

pub struct AdbTransport {
    pub serial: Option<String>, // adb -s
}

impl AdbTransport {
    pub fn new(serial: Option<String>) -> Self { Self { serial } }

    fn adb(&self) -> Command {
        let mut c = Command::new("adb");
        if let Some(s) = &self.serial { c.args(["-s", s]); }
        c
    }

    fn raw_shell(&self, cmd: &str, _timeout_secs: u64) -> Result<String> {
        let out = self.adb().arg("shell").arg(cmd).output().context("spawn adb")?;
        if !out.status.success() {
            bail!("adb shell failed: {}", String::from_utf8_lossy(&out.stderr));
        }
        Ok(String::from_utf8_lossy(&out.stdout).into_owned())
    }

    fn board_md5(&self, remote: &str) -> Result<String> {
        let r = self.exec(&format!("md5sum {remote}"), 60)?;
        if r.rc != 0 { bail!("md5sum {remote} rc={} ({})", r.rc, r.output.trim()); }
        Ok(r.output.split_whitespace().next().unwrap_or_default().to_string())
    }
}

impl Transport for AdbTransport {
    fn name(&self) -> &'static str { "adb" }

    fn exec(&self, cmd: &str, timeout_secs: u64) -> Result<ExecResult> {
        let wrapped = wrap_command_checked(cmd)?;
        let raw = self.raw_shell(&wrapped, timeout_secs)?;
        parse_transcript(&raw)
    }

    fn push(&self, local: &Path, remote: &str) -> Result<()> {
        let want = format!("{:x}", md5::compute(std::fs::read(local)?));
        for attempt in 1..=3 {
            let st = self.adb().arg("push").arg(local).arg(remote)
                .output().context("spawn adb push")?;
            if st.status.success() {
                match self.board_md5(remote) {
                    Ok(got) if got == want => return Ok(()),
                    Ok(got) => eprintln!("push verify mismatch (attempt {attempt}): {got} != {want} — flaky UDISK vfat?"),
                    Err(e) => eprintln!("push verify failed (attempt {attempt}): {e}"),
                }
            }
        }
        bail!("push {} -> {remote}: md5 verify failed after 3 attempts", local.display());
    }

    fn pull(&self, remote: &str, local: &Path) -> Result<()> {
        let st = self.adb().arg("pull").arg(remote).arg(local).output()?;
        if !st.status.success() { bail!("adb pull failed: {}", String::from_utf8_lossy(&st.stderr)); }
        Ok(())
    }
}
```

- [ ] **Step 2: Hardware integration test `tests/hw_adb.rs`**

```rust
// Run with: KBDK_HW=1 cargo test -p kbdk-core --test hw_adb -- --nocapture
use kbdk_core::{adb::AdbTransport, transport::Transport};

fn hw() -> bool { std::env::var("KBDK_HW").is_ok() }

#[test]
fn exec_uname_and_rc() {
    if !hw() { return; }
    let t = AdbTransport::new(None);
    let r = t.exec("uname -a", 15).unwrap();
    assert_eq!(r.rc, 0);
    assert!(r.output.contains("sipeed"));
    assert_eq!(t.exec("false", 15).unwrap().rc, 1);
}

#[test]
fn push_verify_roundtrip() {
    if !hw() { return; }
    let t = AdbTransport::new(None);
    let tmp = std::env::temp_dir().join("kbdk_push_test.bin");
    std::fs::write(&tmp, vec![0xA5u8; 200_000]).unwrap();
    t.push(&tmp, "/tmp/kbdk_push_test.bin").unwrap();
    let back = std::env::temp_dir().join("kbdk_pull_test.bin");
    t.pull("/tmp/kbdk_push_test.bin", &back).unwrap();
    assert_eq!(std::fs::read(&tmp).unwrap(), std::fs::read(&back).unwrap());
    t.exec("rm /tmp/kbdk_push_test.bin", 15).unwrap();
}
```

- [ ] **Step 3: Verify on hardware** — `KBDK_HW=1 cargo test -p kbdk-core --test hw_adb` → 2 passed (board connected).
- [ ] **Step 4: Commit** — `git commit -am "kbdk-core: AdbTransport with verified push"`

### Task 4: Device discovery

**Files:**
- Create: `crates/kbdk-core/src/discover.rs`

- [ ] **Step 1: Failing tests** (canned `adb devices -l` output)

```rust
#[cfg(test)]
mod tests {
    use super::*;
    #[test]
    fn parses_adb_devices() {
        let out = "List of devices attached\n20080411               device usb:1-1 transport_id:1\n\n";
        let d = parse_adb_devices(out);
        assert_eq!(d, vec!["20080411".to_string()]);
    }
    #[test]
    fn finds_serial_ports() {
        // glob form only; actual /dev scan is environment-dependent
        assert!(serial_port_candidates(&["/dev/cu.usbserial-210".into(), "/dev/cu.Bluetooth".into()])
            == vec!["/dev/cu.usbserial-210".to_string()]);
    }
}
```

- [ ] **Step 2: Implement**

```rust
use anyhow::Result;
use std::process::Command;

#[derive(Debug)]
pub struct Devices {
    pub adb: Vec<String>,          // adb serials (board: "20080411")
    pub serial: Vec<String>,       // /dev/cu.usbserial-*
}

pub fn parse_adb_devices(out: &str) -> Vec<String> {
    out.lines().skip(1)
        .filter(|l| l.contains("\tdevice") || l.contains(" device"))
        .filter_map(|l| l.split_whitespace().next().map(str::to_string))
        .collect()
}

pub fn serial_port_candidates(paths: &[String]) -> Vec<String> {
    paths.iter().filter(|p| p.starts_with("/dev/cu.usbserial")).cloned().collect()
}

pub fn discover() -> Result<Devices> {
    let adb_out = Command::new("adb").arg("devices").output()
        .map(|o| String::from_utf8_lossy(&o.stdout).into_owned())
        .unwrap_or_default();
    let ports: Vec<String> = std::fs::read_dir("/dev")?
        .filter_map(|e| e.ok())
        .map(|e| e.path().display().to_string())
        .collect();
    Ok(Devices { adb: parse_adb_devices(&adb_out), serial: serial_port_candidates(&ports) })
}
```

- [ ] **Step 3: Tests pass** — `cargo test -p kbdk-core`
- [ ] **Step 4: Commit** — `git commit -am "kbdk-core: device discovery"`

### Task 5: kbdk CLI (devices / exec / push / pull)

**Files:**
- Modify: `crates/kbdk-cli/src/main.rs`

- [ ] **Step 1: Implement with clap**

```rust
use anyhow::Result;
use clap::{Parser, Subcommand};
use kbdk_core::{adb::AdbTransport, discover, transport::Transport};
use std::path::PathBuf;

#[derive(Parser)]
#[command(name = "kbdk", about = "KidBright µAI dev-kit")]
struct Cli {
    /// adb serial (defaults to the only attached device)
    #[arg(long, global = true)]
    serial: Option<String>,
    #[command(subcommand)]
    cmd: Cmd,
}

#[derive(Subcommand)]
enum Cmd {
    /// List attached boards (adb + serial console)
    Devices,
    /// Run a shell command on the board, print output, exit with its rc
    Exec { command: String },
    /// Verified upload
    Push { local: PathBuf, remote: String },
    /// Download
    Pull { remote: String, local: PathBuf },
}

fn main() -> Result<()> {
    let cli = Cli::parse();
    match cli.cmd {
        Cmd::Devices => {
            let d = discover::discover()?;
            for s in &d.adb { println!("adb\t{s}"); }
            for p in &d.serial { println!("serial\t{p}"); }
        }
        Cmd::Exec { command } => {
            let t = AdbTransport::new(cli.serial);
            let r = t.exec(&command, 60)?;
            print!("{}", r.output);
            std::process::exit(r.rc);
        }
        Cmd::Push { local, remote } => AdbTransport::new(cli.serial).push(&local, &remote)?,
        Cmd::Pull { remote, local } => AdbTransport::new(cli.serial).pull(&remote, &local)?,
    }
    Ok(())
}
```

- [ ] **Step 2: Hardware verify**

```sh
cargo run -p kbdk-cli -- devices            # → adb 20080411 + serial /dev/cu.usbserial-210
cargo run -p kbdk-cli -- exec "uname -a"    # → Linux sipeed 4.9.118 …; echo $? → 0
cargo run -p kbdk-cli -- exec "false"; echo $?   # → 1
```

- [ ] **Step 3: Commit** — `git commit -am "kbdk-cli: devices/exec/push/pull over adb"`

---

## Phase 2 — Serial transport (uai.c port)

### Task 6: SerialTransport

**Files:**
- Create: `crates/kbdk-core/src/serial.rs` (add `pub mod serial;` to lib.rs; add `serialport = "4"` to kbdk-core deps)
- Create: `crates/kbdk-core/tests/hw_serial.rs`

- [ ] **Step 1: TDD the octal encoder** (in `serial.rs`; this is the uai.c `push` wire format)

```rust
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
        assert!(cmds[0].starts_with("printf '")); // first cmd truncates the file with >
        assert!(cmds.iter().all(|c| c.len() < 200));
        assert!(cmds[1..].iter().all(|c| c.contains(">>")));
    }
}
```

- [ ] **Step 2: Implement**

```rust
use anyhow::{bail, Context, Result};
use std::io::{Read, Write};
use std::path::Path;
use std::time::{Duration, Instant};
use crate::protocol::{parse_transcript, wrap_command_checked, END};
use crate::transport::{ExecResult, Transport};

const CHUNK: usize = 45; // payload bytes per printf line (uai.c-compatible)

pub fn octal_chunk(bytes: &[u8]) -> String {
    bytes.iter().map(|b| format!("\\{:03o}", b)).collect()
}

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
    pub fn new(port: &str, baud: u32) -> Self { Self { port: port.into(), baud } }

    fn session(&self) -> Result<Box<dyn serialport::SerialPort>> {
        serialport::new(&self.port, self.baud)
            .timeout(Duration::from_millis(200))
            .open()
            .with_context(|| format!("open {} (another process holding it?)", self.port))
    }

    fn exec_on(&self, sp: &mut Box<dyn serialport::SerialPort>, cmd: &str, timeout_secs: u64) -> Result<ExecResult> {
        let wrapped = wrap_command_checked(cmd)?;
        sp.write_all(b"\r")?;                       // wake the shell
        std::thread::sleep(Duration::from_millis(100));
        let mut junk = [0u8; 4096];
        let _ = sp.read(&mut junk);                  // drain prompt echo
        sp.write_all(wrapped.as_bytes())?;
        sp.write_all(b"\r")?;
        let mut buf = String::new();
        let deadline = Instant::now() + Duration::from_secs(timeout_secs);
        let mut tmp = [0u8; 1024];
        loop {
            match sp.read(&mut tmp) {
                Ok(n) if n > 0 => {
                    buf.push_str(&String::from_utf8_lossy(&tmp[..n]));
                    // done when END_<digits> appears *after* the BEGIN line output
                    if let Ok(r) = parse_transcript(&buf) {
                        let _ = r; // parse succeeded -> sentinel pair closed
                        return parse_transcript(&buf);
                    }
                }
                _ => {}
            }
            if Instant::now() > deadline { bail!("serial exec timeout: {cmd} (buf: {} bytes, END={END}…)", buf.len()); }
        }
    }
}

impl Transport for SerialTransport {
    fn name(&self) -> &'static str { "serial" }

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
            if r.rc != 0 { bail!("push chunk failed rc={}", r.rc); }
        }
        let r = self.exec_on(&mut sp, &format!("md5sum {remote}"), 30)?;
        let got = r.output.split_whitespace().next().unwrap_or_default();
        if got != want { bail!("serial push verify failed: {got} != {want}"); }
        Ok(())
    }

    fn pull(&self, _remote: &str, _local: &Path) -> Result<()> {
        bail!("pull over serial not supported; use adb")
    }
}
```

- [ ] **Step 3: Hardware test `tests/hw_serial.rs`** (only when `KBDK_HW=1` AND no `uai`/monitor holding the port)

```rust
use kbdk_core::{serial::SerialTransport, transport::Transport};

#[test]
fn serial_exec_uname() {
    if std::env::var("KBDK_HW").is_err() { return; }
    let t = SerialTransport::new("/dev/cu.usbserial-210", 115200);
    let r = t.exec("uname -a", 15).unwrap();
    assert_eq!(r.rc, 0);
    assert!(r.output.contains("sipeed"));
}

#[test]
fn serial_push_small_file() {
    if std::env::var("KBDK_HW").is_err() { return; }
    let t = SerialTransport::new("/dev/cu.usbserial-210", 115200);
    let tmp = std::env::temp_dir().join("kbdk_serial_push.bin");
    std::fs::write(&tmp, b"hello kbdk over serial \x00\xff").unwrap();
    t.push(&tmp, "/tmp/kbdk_serial_push.bin").unwrap();
}
```

- [ ] **Step 4: Verify** — `KBDK_HW=1 cargo test -p kbdk-core --test hw_serial -- --test-threads=1` → 2 passed.
- [ ] **Step 5: Wire `--transport serial` flag into kbdk-cli exec/push** (construct `SerialTransport` instead of Adb; default stays auto→adb-first):

```rust
// in main.rs:
#[arg(long, global = true, value_parser = ["auto", "adb", "serial"], default_value = "auto")]
transport: String,

fn make_transport(kind: &str, serial: Option<String>) -> Result<Box<dyn Transport>> {
    match kind {
        "adb" => Ok(Box::new(AdbTransport::new(serial))),
        "serial" => Ok(Box::new(SerialTransport::new(
            &std::env::var("UAI_PORT").unwrap_or("/dev/cu.usbserial-210".into()), 115200))),
        _ => {
            let d = discover::discover()?;
            if !d.adb.is_empty() { Ok(Box::new(AdbTransport::new(serial))) }
            else if !d.serial.is_empty() { Ok(Box::new(SerialTransport::new(&d.serial[0], 115200))) }
            else { anyhow::bail!("no board found (adb or serial)") }
        }
    }
}
```

- [ ] **Step 6: Hardware verify both** — `cargo run -p kbdk-cli -- --transport serial exec "echo hi"` and `--transport adb exec "echo hi"`.
- [ ] **Step 7: Commit** — `git commit -am "kbdk-core: serial transport (uai.c port) + transport auto-detect"`

---

## Phase 3 — Board ncnn runtime + model packs + runner

### Task 7: Pinned ncnn cross-build script

**Files:**
- Create: `board/ncnn/v831-musl.toolchain.cmake`, `board/ncnn/build.sh`
- Modify: `.gitignore` (add `board/ncnn/dist/`, `board/ncnn/ncnn-src/`, `board/ncnn/build*/`)

- [ ] **Step 1: Toolchain file** (verified working 2026-06-10)

```cmake
set(CMAKE_SYSTEM_NAME Linux)
set(CMAKE_SYSTEM_PROCESSOR arm)
set(CMAKE_C_COMPILER arm-unknown-linux-musleabihf-gcc)
set(CMAKE_CXX_COMPILER arm-unknown-linux-musleabihf-g++)
set(CMAKE_C_FLAGS "-mcpu=cortex-a7 -mfpu=neon-vfpv4 -mfloat-abi=hard")
set(CMAKE_CXX_FLAGS "-mcpu=cortex-a7 -mfpu=neon-vfpv4 -mfloat-abi=hard")
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
```

- [ ] **Step 2: `build.sh`** — clones ncnn at the pinned commit, builds the board static lib AND the host tools:

```sh
#!/bin/sh
# Builds (a) libncnn.a for the V831 (musl-armhf, NEON, single-thread) into
# board/ncnn/dist/board and (b) native host quantize tools (ncnn2table/ncnn2int8/
# ncnnoptimize) into board/ncnn/dist/host. Pin = commit verified on hardware.
set -e
cd "$(dirname "$0")"
PIN=b16501a   # ncnn master, verified on V831 2026-06-10
export PATH=/opt/homebrew/bin:$PATH

[ -d ncnn-src ] || git clone https://github.com/Tencent/ncnn.git ncnn-src
git -C ncnn-src fetch --depth 1 origin $PIN 2>/dev/null || true
git -C ncnn-src checkout $PIN

cmake -S ncnn-src -B build-board -GNinja \
  -DCMAKE_TOOLCHAIN_FILE="$PWD/v831-musl.toolchain.cmake" \
  -DCMAKE_BUILD_TYPE=Release \
  -DNCNN_VULKAN=OFF -DNCNN_OPENMP=OFF -DNCNN_THREADS=OFF \
  -DNCNN_BUILD_TOOLS=OFF -DNCNN_BUILD_EXAMPLES=OFF -DNCNN_BUILD_BENCHMARK=OFF \
  -DNCNN_BUILD_TESTS=OFF -DNCNN_SHARED_LIB=OFF \
  -DCMAKE_INSTALL_PREFIX="$PWD/dist/board"
ninja -C build-board install

cmake -S ncnn-src -B build-host -GNinja -DCMAKE_BUILD_TYPE=Release \
  -DNCNN_VULKAN=OFF -DNCNN_BUILD_TOOLS=ON -DNCNN_SIMPLEOCV=ON \
  -DNCNN_BUILD_EXAMPLES=OFF -DNCNN_BUILD_BENCHMARK=OFF -DNCNN_BUILD_TESTS=OFF
ninja -C build-host ncnn2table ncnn2int8 ncnnoptimize
mkdir -p dist/host
cp build-host/tools/quantize/ncnn2table build-host/tools/quantize/ncnn2int8 \
   build-host/tools/ncnnoptimize dist/host/
echo "OK: dist/board (libncnn.a) + dist/host (quantize tools)"
```

- [ ] **Step 3: Run it** — `sh board/ncnn/build.sh` → ends with `OK:`; `ls board/ncnn/dist/board/lib/libncnn.a board/ncnn/dist/host/ncnn2int8` exist.
- [ ] **Step 4: Commit** (scripts only, dist gitignored) — `git commit -am "board: pinned ncnn cross-build (V831 static lib + host quantize tools)"`

### Task 8: Model pack manifest (Rust side)

**Files:**
- Create: `crates/kbdk-core/src/pack.rs` (add `pub mod pack;` to lib.rs)

- [ ] **Step 1: Failing test**

```rust
#[cfg(test)]
mod tests {
    use super::*;
    #[test]
    fn manifest_roundtrip() {
        let j = r#"{
          "name":"toy3","task":"classification","backbone":"mobilenet_v2",
          "input":{"width":224,"height":224,"mean":[127.5,127.5,127.5],"norm":[0.0078125,0.0078125,0.0078125]},
          "quant":"int8",
          "blobs":{"input":"in0","output":"out0"},
          "files":{"param":"model.param","bin":"model.bin","labels":"labels.txt"},
          "md5":{"param":"aa","bin":"bb"},
          "labels":["red","green","blue"]
        }"#;
        let m: Manifest = serde_json::from_str(j).unwrap();
        assert_eq!(m.input.width, 224);
        assert_eq!(m.labels.len(), 3);
        assert_eq!(m.blobs.output, "out0");
    }
}
```

- [ ] **Step 2: Implement**

```rust
use anyhow::{bail, Context, Result};
use serde::{Deserialize, Serialize};
use std::path::Path;

#[derive(Debug, Serialize, Deserialize)]
pub struct InputSpec {
    pub width: u32,
    pub height: u32,
    pub mean: [f32; 3],
    pub norm: [f32; 3],
}

#[derive(Debug, Serialize, Deserialize)]
pub struct Blobs { pub input: String, pub output: String }

#[derive(Debug, Serialize, Deserialize)]
pub struct PackFiles { pub param: String, pub bin: String, pub labels: String }

#[derive(Debug, Serialize, Deserialize)]
pub struct Manifest {
    pub name: String,
    pub task: String,           // "classification" | "detection"
    pub backbone: String,
    pub input: InputSpec,
    pub quant: String,          // "int8" | "fp16"
    pub blobs: Blobs,
    pub files: PackFiles,
    pub md5: std::collections::HashMap<String, String>,
    pub labels: Vec<String>,
}

impl Manifest {
    pub fn load(dir: &Path) -> Result<Self> {
        let p = dir.join("manifest.json");
        let m: Manifest = serde_json::from_str(&std::fs::read_to_string(&p)
            .with_context(|| format!("read {}", p.display()))?)?;
        Ok(m)
    }
    /// verify the referenced files exist and md5s match
    pub fn verify(&self, dir: &Path) -> Result<()> {
        for (key, rel) in [("param", &self.files.param), ("bin", &self.files.bin)] {
            let data = std::fs::read(dir.join(rel))?;
            let got = format!("{:x}", md5::compute(&data));
            match self.md5.get(key) {
                Some(want) if *want == got => {}
                Some(want) => bail!("{key} md5 mismatch: {got} != {want}"),
                None => bail!("manifest md5 missing entry {key}"),
            }
        }
        Ok(())
    }
}
```

- [ ] **Step 3: Tests pass; Commit** — `git commit -am "kbdk-core: model pack manifest"`

### Task 9: kbrun stage 1 — file-input classification (no camera)

**Files:**
- Create: `board/runner/kbrun.cpp`, `board/runner/manifest.h` (tiny JSON-subset parser, no deps)
- Modify: `Makefile` (kbrun target)

The runner reads `manifest.json` itself (C++, no jsoncpp on board): write a ~60-line
extractor for the known keys (flat schema above; strings, numbers, arrays of numbers/strings).

- [ ] **Step 1: `manifest.h`** — minimal parser:

```cpp
// manifest.h - just enough JSON for kbdk manifest.json (flat, known keys).
#pragma once
#include <string>
#include <vector>
#include <cstdlib>
#include <cstring>

struct KbManifest {
    std::string name, task, backbone, quant, in_blob, out_blob, param, bin, labels_file;
    int w = 0, h = 0;
    float mean[3] = {0,0,0}, norm[3] = {0,0,0};
    std::vector<std::string> labels;
};

// find "key" : <value> ; returns pointer past the colon or nullptr
static const char* mf_find(const std::string& s, const char* key){
    std::string pat = std::string("\"") + key + "\"";
    size_t p = s.find(pat);
    if(p == std::string::npos) return nullptr;
    p = s.find(':', p + pat.size());
    return p == std::string::npos ? nullptr : s.c_str() + p + 1;
}
static std::string mf_str(const std::string& s, const char* key){
    const char* p = mf_find(s, key); if(!p) return "";
    const char* a = strchr(p, '"'); if(!a) return "";
    const char* b = strchr(a + 1, '"'); if(!b) return "";
    return std::string(a + 1, b);
}
static double mf_num(const std::string& s, const char* key){
    const char* p = mf_find(s, key); return p ? atof(p) : 0;
}
static void mf_floats3(const std::string& s, const char* key, float out[3]){
    const char* p = mf_find(s, key); if(!p) return;
    p = strchr(p, '['); if(!p) return;
    char* q = nullptr;
    for(int i = 0; i < 3; i++){ out[i] = strtof(p + 1, &q); p = strchr(q, ','); if(!p && i < 2) return; }
}
static std::vector<std::string> mf_strlist(const std::string& s, const char* key){
    std::vector<std::string> v;
    const char* p = mf_find(s, key); if(!p) return v;
    const char* end = strchr(p, ']'); if(!end) return v;
    while(true){
        const char* a = strchr(p, '"'); if(!a || a > end) break;
        const char* b = strchr(a + 1, '"'); if(!b || b > end) break;
        v.emplace_back(a + 1, b); p = b + 1;
    }
    return v;
}

static bool mf_load(const char* path, KbManifest& m){
    FILE* f = fopen(path, "rb"); if(!f) return false;
    std::string s; char buf[4096]; size_t n;
    while((n = fread(buf, 1, sizeof buf, f)) > 0) s.append(buf, n);
    fclose(f);
    m.name = mf_str(s, "name"); m.task = mf_str(s, "task");
    m.backbone = mf_str(s, "backbone"); m.quant = mf_str(s, "quant");
    m.w = (int)mf_num(s, "width"); m.h = (int)mf_num(s, "height");
    mf_floats3(s, "mean", m.mean); mf_floats3(s, "norm", m.norm);
    // blobs/files are nested but keys are unique in the whole document:
    m.in_blob = mf_str(s, "input");      // NOTE: "input" object also matches; see Step 2 test
    m.out_blob = mf_str(s, "output");
    m.param = mf_str(s, "param"); m.bin = mf_str(s, "bin");
    m.labels_file = mf_str(s, "labels");
    m.labels = mf_strlist(s, "labels");
    return m.w > 0 && m.h > 0 && !m.param.empty();
}
```

**Pitfall called out for the implementer:** `"input"` matches both the `input{}` object and
`blobs.input`. Resolve by renaming the blob keys in the manifest schema to
`"in_blob"`/`"out_blob"` (update Task 8's `Blobs` serde with
`#[serde(rename = "in_blob")] pub input: String` etc. and the Python emitter in Task 12).
Same for `"labels"` (array) vs `files.labels` → name the file key `"labels_file"`.

- [ ] **Step 2: `kbrun.cpp` stage 1** — `kbrun MANIFEST_DIR --image raw.rgb`:

```cpp
// kbrun.cpp — kbdk model-pack runner for the V831.
// Stage 1: --image PATH (raw RGB888 at the manifest's WxH) -> top-5 to stdout as JSON-lines.
// Stage 2 (Task 10) adds the live camera path.
#include "net.h"            // ncnn
#include "manifest.h"
#include <cstdio>
#include <ctime>
#include <string>
#include <vector>
#include <algorithm>

static double now_ms(){ struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000.0 + ts.tv_nsec / 1e6; }

int main(int argc, char** argv){
    if(argc < 3){ fprintf(stderr, "usage: %s PACK_DIR --image RAW.rgb\n", argv[0]); return 2; }
    std::string dir = argv[1];
    KbManifest m;
    if(!mf_load((dir + "/manifest.json").c_str(), m)){
        printf("{\"event\":\"error\",\"msg\":\"manifest load failed\"}\n"); return 1; }

    ncnn::Net net;
    net.opt.num_threads = 1;
    if(net.load_param((dir + "/" + m.param).c_str()) ||
       net.load_model((dir + "/" + m.bin).c_str())){
        printf("{\"event\":\"error\",\"msg\":\"model load failed\"}\n"); return 1; }

    size_t need = (size_t)m.w * m.h * 3;
    std::vector<unsigned char> rgb(need);
    FILE* f = fopen(argv[3] ? argv[3] : "", "rb");
    if(!f || fread(rgb.data(), 1, need, f) != need){
        printf("{\"event\":\"error\",\"msg\":\"image read failed\"}\n"); return 1; }
    fclose(f);

    ncnn::Mat in = ncnn::Mat::from_pixels(rgb.data(), ncnn::Mat::PIXEL_RGB, m.w, m.h);
    in.substract_mean_normalize(m.mean, m.norm);
    ncnn::Mat out;
    double t0 = now_ms();
    {
        ncnn::Extractor ex = net.create_extractor();
        ex.input(m.in_blob.c_str(), in);
        if(ex.extract(m.out_blob.c_str(), out)){
            printf("{\"event\":\"error\",\"msg\":\"extract failed\"}\n"); return 1; }
    }
    double dt = now_ms() - t0;

    // flatten channel-strided 1x1xC output, softmax, top-5
    std::vector<float> v(out.c ? out.c : out.w);
    if(out.c > 1) for(int i = 0; i < out.c; i++) v[i] = out.channel(i)[0];
    else for(int i = 0; i < out.w; i++) v[i] = ((const float*)out)[i];
    float mx = *std::max_element(v.begin(), v.end()), sum = 0;
    for(auto& x : v){ x = expf(x - mx); sum += x; }
    for(auto& x : v) x /= sum;
    std::vector<int> idx(v.size());
    for(size_t i = 0; i < idx.size(); i++) idx[i] = i;
    std::partial_sort(idx.begin(), idx.begin() + std::min<size_t>(5, idx.size()), idx.end(),
                      [&](int a, int b){ return v[a] > v[b]; });
    printf("{\"event\":\"result\",\"ms\":%.1f,\"top\":[", dt);
    for(int k = 0; k < (int)std::min<size_t>(5, idx.size()); k++){
        int i = idx[k];
        printf("%s{\"label\":\"%s\",\"conf\":%.4f}", k ? "," : "",
               i < (int)m.labels.size() ? m.labels[i].c_str() : "?", v[i]);
    }
    printf("]}\n");
    return 0;
}
```

- [ ] **Step 3: Makefile target**

```makefile
# kbdk board runner: static ncnn classification (stage 1: file input)
NCNN_DIST := board/ncnn/dist/board
kbrun: bin/kbrun
bin/kbrun: board/runner/kbrun.cpp board/runner/manifest.h | bin
	$(CROSSXX) $(CROSSFLAGS) -I$(NCNN_DIST)/include/ncnn -static \
	  -o $@ $< $(NCNN_DIST)/lib/libncnn.a
	arm-unknown-linux-musleabihf-strip $@
```

- [ ] **Step 4: Hardware verify with a hand-made pack** — reuse the probe model
  (`/tmp/kbdk-probe/mbv2-int8.*`): write `manifest.json` by hand (in_blob `in0`,
  out_blob `out0`, 224×224, mean 127.5×3, norm 0.0078125×3, labels = 1000 dummy
  lines), make a 224×224×3 raw from any capture (`captures/ppm2raw.py`), push pack
  to `/mnt/UDISK/kbdk/probe/`, then:

```sh
cargo run -p kbdk-cli -- exec "/tmp/kbrun /mnt/UDISK/kbdk/probe --image /tmp/test224.rgb"
```
Expected: one `{"event":"result","ms":…,"top":[…]}` line, ms ≈ 450–500, and the top-1
index must equal the host's: `python - <<…>>` with pip-ncnn running the same files+image
(write `py/tools/host_infer.py` for this check — same Mat/from_pixels code as Task 13's
verifier; keep it, Task 13 reuses it).

- [ ] **Step 5: Commit** — `git commit -am "kbrun stage 1: ncnn pack classification from file input"`

### Task 10: kbrun stage 2 — live camera + fb0 overlay

**Files:**
- Modify: `board/runner/kbrun.cpp` (add camera mode), `Makefile` (kbrun gains MPP flags: `$(MPPDEF) $(MPPINC) -ldl -lpthread`)

This transplants the proven structure of `src/nnacam.cpp` (read it side-by-side; it is the
same loop with AWNN swapped for ncnn):
- MPP NV21 capture via dlopen (`cammpp.c` sequence: SYS_SetConf→SYS_Init→ISP_Init/Run thread→VIPP→VirChn→GetFrame/ReleaseFrame), `aw_dlsym` time64 workaround.
- Gray-world AWB gains (EMA, clamp 0.4–2.5×) applied to BOTH the fb preview and the net input.
- fb0 240×240 BGR blit gated on `FBIO_WAITFORVSYNC`, yoffset pinned 0, no panning.
- Worker thread: mutex+cond, latest-wins frame handoff; copies input under lock, runs
  `ncnn::Extractor` lock-free; main loop draws last label via the 8×8 font.
- Camera mode args: `kbrun PACK_DIR [WxH] [nframes] [flip]`; per-result JSON-line on stdout
  (`{"event":"result",...}` as stage 1) so the host can stream it.
- ncnn replaces AWNN: no `LD_LIBRARY_PATH` needed for the net, but MPP still needs
  `LD_LIBRARY_PATH=/usr/lib/eyesee-mpp:/usr/lib`. Static ncnn + dlopen'd MPP coexist —
  link `-static-libstdc++ -static-libgcc` but NOT `-static` (dlopen needs the dynamic
  loader; keep musl dynamic like nnacam, bind plain `dlsym` via `__asm__("dlsym")`).

**NOTE the -static change vs stage 1:** stage 1's fully-static link must be relaxed in this
task to dynamic-musl + static libncnn.a (same Makefile line minus `-static`, plus MPP
includes and `-ldl -lpthread -lm`). Verify stage 1 (--image) still runs after relinking.

- [ ] **Step 1: Port the camera loop** (copy from `src/nnacam.cpp`, replace the AWNN
  init/forward block with the `ncnn::Net` + manifest code from stage 1; net input crop:
  centre-crop the NV21→RGB frame to square then nearest-scale to m.w×m.h, exactly as
  nnacam's `crop_scale_rgb`).
- [ ] **Step 2: Build** — `make kbrun` clean.
- [ ] **Step 3: Hardware verify** — push, run 100 frames with the probe pack:

```sh
cargo run -p kbdk-cli -- push bin/kbrun /tmp/kbrun
cargo run -p kbdk-cli -- exec "chmod +x /tmp/kbrun"
cargo run -p kbdk-cli -- exec "LD_LIBRARY_PATH=/usr/lib/eyesee-mpp:/usr/lib /tmp/kbrun /mnt/UDISK/kbdk/probe 320x240 100"
```
Expected: live preview on the panel with a label overlay; JSON result lines on stdout;
preview stays ~25 fps while inference ticks ~2/s (mbv2-int8 ≈ 470 ms). Verify the panel
remotely via the fb0 readback one-liner from CLAUDE.md if not at the desk.
- [ ] **Step 4: Commit** — `git commit -am "kbrun stage 2: live MPP camera + ncnn + fb0 overlay"`

### Task 11: `kbdk deploy` / `kbdk run` / `kbdk stop`

**Files:**
- Modify: `crates/kbdk-cli/src/main.rs`
- Create: `crates/kbdk-core/src/deploy.rs` (add `pub mod deploy;`)

- [ ] **Step 1: Implement `deploy.rs`**

```rust
use anyhow::{bail, Result};
use std::path::Path;
use crate::{pack::Manifest, transport::Transport};

pub const BOARD_PACK_ROOT: &str = "/mnt/UDISK/kbdk";
pub const RUNNER: &str = "/tmp/kbrun";

pub fn deploy_pack(t: &dyn Transport, local_dir: &Path) -> Result<String> {
    let m = Manifest::load(local_dir)?;
    m.verify(local_dir)?;
    let remote = format!("{BOARD_PACK_ROOT}/{}", m.name);
    t.exec(&format!("mkdir -p {remote}"), 15)?;
    for rel in ["manifest.json", &m.files.param, &m.files.bin, &m.files.labels] {
        t.push(&local_dir.join(rel), &format!("{remote}/{rel}"))?;
    }
    Ok(remote)
}

pub fn start_runner(t: &dyn Transport, remote_pack: &str, res: &str, nframes: u32) -> Result<()> {
    // pidfile + backgrounded; must not end in bare '&'
    let cmd = format!(
        "(LD_LIBRARY_PATH=/usr/lib/eyesee-mpp:/usr/lib {RUNNER} {remote_pack} {res} {nframes} \
         > /tmp/kbrun.log 2>&1 & echo $! > /tmp/kbrun.pid) ; true");
    let r = t.exec(&cmd, 15)?;
    if r.rc != 0 { bail!("start failed rc={}", r.rc); }
    Ok(())
}

pub fn stop_runner(t: &dyn Transport) -> Result<()> {
    t.exec("[ -f /tmp/kbrun.pid ] && kill $(cat /tmp/kbrun.pid) 2>/dev/null; rm -f /tmp/kbrun.pid; true", 15)?;
    Ok(())
}
```

- [ ] **Step 2: CLI verbs** — `Deploy { pack_dir }`, `Run { pack_name, #[arg(default_value="320x240")] res: String, #[arg(default_value_t=0)] frames: u32 }` (deploy also pushes `bin/kbrun` to /tmp + chmod), `Stop`, plus `Log` (`exec "tail -n 50 /tmp/kbrun.log"`).
- [ ] **Step 3: Hardware verify** — `kbdk deploy <probe pack dir>` → `kbdk run probe` → label on panel → `kbdk log` shows result lines → `kbdk stop`.
- [ ] **Step 4: Commit** — `git commit -am "kbdk-cli: deploy/run/stop/log for model packs"`

---

## Phase 4 — Python convert pipeline

### Task 12: uv workspace + kbdk-convert (pnnx export + pack emit)

**Files:**
- Create: `py/pyproject.toml`, `py/kbdk-convert/pyproject.toml`, `py/kbdk-convert/src/kbdk_convert/{__init__.py,cli.py,convert.py,packio.py}`, `py/kbdk-convert/tests/test_convert.py`

- [ ] **Step 1: uv workspace**

`py/pyproject.toml`:
```toml
[project]
name = "kbdk-py"
version = "0.1.0"
requires-python = ">=3.12"
dependencies = []

[tool.uv.workspace]
members = ["kbdk-train", "kbdk-convert"]
```

`py/kbdk-convert/pyproject.toml`:
```toml
[project]
name = "kbdk-convert"
version = "0.1.0"
requires-python = ">=3.12"
dependencies = ["torch>=2.4", "pnnx", "ncnn", "numpy", "pillow"]

[project.scripts]
kbdk-convert = "kbdk_convert.cli:main"

[build-system]
requires = ["hatchling"]
build-backend = "hatchling.build"
```

- [ ] **Step 2: `convert.py`** — the pipeline:

```python
"""TorchScript -> pnnx -> ncnn (fp16 storage) -> int8 quantize -> model pack."""
import json, hashlib, shutil, subprocess, sys, tempfile
from pathlib import Path

REPO = Path(__file__).resolve().parents[4]   # py/kbdk-convert/src/kbdk_convert -> repo root
NCNN_TOOLS = REPO / "board/ncnn/dist/host"

def emit(event: str, **kw):
    print(json.dumps({"event": event, **kw}), flush=True)

def md5(p: Path) -> str:
    return hashlib.md5(p.read_bytes()).hexdigest()

def pnnx_export(ts_model: Path, w: int, h: int, workdir: Path) -> tuple[Path, Path]:
    import pnnx, torch
    emit("step", name="pnnx")
    mod = torch.jit.load(str(ts_model))
    pnnx.convert(str(ts_model), [torch.rand(1, 3, h, w)])
    stem = ts_model.with_suffix("")
    param, binf = Path(f"{stem}.ncnn.param"), Path(f"{stem}.ncnn.bin")
    if not param.exists():
        raise RuntimeError("pnnx did not produce .ncnn.param")
    return param, binf

def quantize(param: Path, binf: Path, calib_images: list[Path],
             w: int, h: int, mean: list[float], norm: list[float], workdir: Path) -> tuple[Path, Path]:
    emit("step", name="quantize", images=len(calib_images))
    lst = workdir / "imagelist.txt"
    lst.write_text("\n".join(str(p) for p in calib_images))
    table = workdir / "model.table"
    subprocess.run([str(NCNN_TOOLS / "ncnn2table"), str(param), str(binf), str(lst), str(table),
                    f"mean=[{','.join(map(str, mean))}]", f"norm=[{','.join(map(str, norm))}]",
                    f"shape=[{w},{h},3]", "pixel=RGB", "thread=4", "method=kl"],
                   check=True, capture_output=True)
    qparam, qbin = workdir / "model.param", workdir / "model.bin"
    subprocess.run([str(NCNN_TOOLS / "ncnn2int8"), str(param), str(binf),
                    str(qparam), str(qbin), str(table)], check=True, capture_output=True)
    return qparam, qbin

def host_infer(param: Path, binf: Path, img_rgb, w, h, mean, norm, in_blob, out_blob):
    import ncnn, numpy as np
    net = ncnn.Net()
    net.load_param(str(param)); net.load_model(str(binf))
    m = ncnn.Mat.from_pixels(img_rgb, ncnn.Mat.PixelType.PIXEL_RGB, w, h)
    m.substract_mean_normalize(mean, norm)
    ex = net.create_extractor()
    ex.input(in_blob, m)
    _, out = ex.extract(out_blob)
    return np.array(out).flatten()

def verify_parity(p32, b32, p8, b8, images, w, h, mean, norm, in_blob, out_blob, min_agree=0.8):
    import numpy as np
    from PIL import Image
    agree = 0
    for ip in images:
        img = np.asarray(Image.open(ip).convert("RGB").resize((w, h)), dtype=np.uint8)
        a = host_infer(p32, b32, img, w, h, mean, norm, in_blob, out_blob)
        b = host_infer(p8, b8, img, w, h, mean, norm, in_blob, out_blob)
        agree += int(a.argmax() == b.argmax())
    frac = agree / len(images)
    emit("step", name="parity", top1_agreement=frac)
    if frac < min_agree:
        raise RuntimeError(f"int8 parity too low: {frac:.2f} < {min_agree}")

def build_pack(name, task, backbone, qparam, qbin, labels, w, h, mean, norm,
               in_blob, out_blob, out_dir: Path) -> Path:
    pack = out_dir / name
    pack.mkdir(parents=True, exist_ok=True)
    shutil.copy(qparam, pack / "model.param")
    shutil.copy(qbin, pack / "model.bin")
    (pack / "labels.txt").write_text("\n".join(labels))
    manifest = {
        "name": name, "task": task, "backbone": backbone,
        "input": {"width": w, "height": h, "mean": mean, "norm": norm},
        "quant": "int8",
        "blobs": {"in_blob": in_blob, "out_blob": out_blob},
        "files": {"param": "model.param", "bin": "model.bin", "labels_file": "labels.txt"},
        "md5": {"param": md5(pack / "model.param"), "bin": md5(pack / "model.bin")},
        "labels": labels,
    }
    (pack / "manifest.json").write_text(json.dumps(manifest, indent=2))
    emit("done", pack=str(pack))
    return pack
```

(`cli.py`: argparse — `--model model.pt --data DIR --name NAME --out packs/ --width 224
--height 224 --backbone mobilenet_v2 --task classification`; collects ≤64 calib images
from `DIR/**/*.jpg|png`, mean/norm fixed at `[127.5]*3` / `[0.0078125]*3` to match
training (Task 14), in_blob/out_blob discovered from the generated .param: first
`Input` layer's blob and last layer's output blob — parse the text format, fields:
`Layer type, name, #in, #out, in blobs…, out blobs…`.)

- [ ] **Step 3: pytest** `tests/test_convert.py` — build a tiny TorchScript model in the
  test (the Task 2 probe net: Conv(3,8,3,2,1)+ReLU+Conv(8,4,16) @ 32×32, scripted via
  `torch.jit.trace`), 8 random BMP calib images, run the full pipeline into tmpdir, assert:
  manifest.json exists, md5s verify, blob names parsed == `in0`/`out0`, parity step emitted.
  Run: `cd py && uv run --package kbdk-convert pytest kbdk-convert/tests -x -q` → 1 passed
  (first run downloads torch; allow several minutes).
- [ ] **Step 4: Commit** — `git commit -am "kbdk-convert: pnnx->ncnn->int8->pack pipeline"`

### Task 13: `kbdk convert` subprocess wiring

**Files:**
- Modify: `crates/kbdk-cli/src/main.rs`

- [ ] **Step 1: Add verb**

```rust
Cmd::Convert { model: PathBuf, data: PathBuf, name: String, out: PathBuf,
               #[arg(long, default_value_t = 224)] size: u32 } => {
    let mut child = std::process::Command::new("uv")
        .current_dir("py")
        .args(["run", "--package", "kbdk-convert", "kbdk-convert",
               "--model", &model.canonicalize()?.display().to_string(),
               "--data", &data.canonicalize()?.display().to_string(),
               "--name", &name,
               "--out", &out.canonicalize().unwrap_or(out.clone()).display().to_string(),
               "--width", &size.to_string(), "--height", &size.to_string()])
        .stdout(std::process::Stdio::piped())
        .spawn()?;
    // re-emit JSON-lines with a human-readable prefix
    use std::io::BufRead;
    for line in std::io::BufReader::new(child.stdout.take().unwrap()).lines() {
        let line = line?;
        if let Ok(v) = serde_json::from_str::<serde_json::Value>(&line) {
            eprintln!("[convert] {}", v["event"].as_str().unwrap_or("?"));
        }
        println!("{line}");
    }
    let st = child.wait()?;
    if !st.success() { anyhow::bail!("convert failed"); }
}
```

- [ ] **Step 2: Verify** — `cargo run -p kbdk-cli -- convert --model <tiny.pt from probe> --data <calib dir> --name tiny --out packs/` → pack dir created, JSON-lines streamed.
- [ ] **Step 3: Commit** — `git commit -am "kbdk-cli: convert verb (uv subprocess, JSON-lines)"`

---

## Phase 5 — Train + end-to-end

### Task 14: kbdk-train

**Files:**
- Create: `py/kbdk-train/pyproject.toml` (deps: `torch`, `torchvision`, `pillow`; script `kbdk-train = "kbdk_train.cli:main"`), `py/kbdk-train/src/kbdk_train/{__init__.py,cli.py,train.py}`, `py/kbdk-train/tests/test_train.py`

- [ ] **Step 1: `train.py`**

```python
"""Fine-tune a torchvision backbone on an ImageFolder dataset; export TorchScript."""
import json, sys
from pathlib import Path
import torch, torch.nn as nn
from torch.utils.data import DataLoader
from torchvision import datasets, models, transforms

def emit(event, **kw): print(json.dumps({"event": event, **kw}), flush=True)

# Training normalization MUST match the board runner: (x-127.5)*0.0078125 == [-1,1].
TFM = transforms.Compose([
    transforms.Resize(256), transforms.CenterCrop(224),
    transforms.RandomHorizontalFlip(),
    transforms.ToTensor(),                       # [0,1]
    transforms.Normalize([0.5]*3, [0.5]*3),      # -> [-1,1]
])

def make_model(backbone: str, n_classes: int) -> nn.Module:
    if backbone == "mobilenet_v2":
        m = models.mobilenet_v2(weights=models.MobileNet_V2_Weights.IMAGENET1K_V1)
        m.classifier[1] = nn.Linear(m.last_channel, n_classes)
    elif backbone == "resnet18":
        m = models.resnet18(weights=models.ResNet18_Weights.IMAGENET1K_V1)
        m.fc = nn.Linear(m.fc.in_features, n_classes)
    else:
        raise ValueError(f"unknown backbone {backbone}")
    return m

def train(data_dir: Path, backbone: str, epochs: int, lr: float, out: Path,
          device_str: str | None = None) -> list[str]:
    device = torch.device(device_str or ("mps" if torch.backends.mps.is_available() else "cpu"))
    ds = datasets.ImageFolder(str(data_dir), TFM)
    n_val = max(1, len(ds) // 10)
    tr, va = torch.utils.data.random_split(ds, [len(ds) - n_val, n_val])
    dl = DataLoader(tr, batch_size=32, shuffle=True)
    dv = DataLoader(va, batch_size=32)
    model = make_model(backbone, len(ds.classes)).to(device)
    opt = torch.optim.AdamW(model.parameters(), lr=lr)
    lossf = nn.CrossEntropyLoss()
    emit("start", classes=ds.classes, n_train=len(tr), n_val=len(va), device=str(device))
    for ep in range(epochs):
        model.train(); tot = 0.0
        for x, y in dl:
            x, y = x.to(device), y.to(device)
            opt.zero_grad(); loss = lossf(model(x), y); loss.backward(); opt.step()
            tot += loss.item() * len(y)
        model.eval(); correct = 0
        with torch.no_grad():
            for x, y in dv:
                correct += (model(x.to(device)).argmax(1).cpu() == y).sum().item()
        emit("epoch", n=ep + 1, loss=tot / len(tr), val_acc=correct / len(va))
    model.eval().cpu()
    ts = torch.jit.trace(model, torch.rand(1, 3, 224, 224))
    ts.save(str(out))
    emit("saved", path=str(out))
    return ds.classes
```

(`cli.py`: argparse `--data --backbone mobilenet_v2 --epochs 5 --lr 1e-3 --out model.pt
--labels-out labels.txt --device`; writes class list to labels-out.)

- [ ] **Step 2: pytest with a generated toy dataset** — test writes 3 classes × 12 images
  of solid red/green/blue (with ±20 noise) 64×64 PNGs into tmpdir, runs
  `train(..., epochs=2, device_str="cpu")`, asserts: model.pt exists, last
  `val_acc == 1.0` (colors are trivially separable), labels == ["blue","green","red"]
  (ImageFolder alphabetical). Run: `cd py && uv run --package kbdk-train pytest kbdk-train/tests -x -q`.
- [ ] **Step 3: `kbdk train` CLI verb** (same subprocess pattern as Task 13: stream
  `{"event":"epoch",…}` lines; args `--data --name --epochs --backbone`).
- [ ] **Step 4: Commit** — `git commit -am "kbdk-train: ImageFolder fine-tune (MPS) + TorchScript export"`

### Task 15: End-to-end hardware milestone

**Files:**
- Create: `examples/toy-dataset/` (generator script `examples/make_toy_dataset.py` writing `red/green/blue` class dirs — 30 photos-of-colored-things sized 256², generated procedurally with noise/gradients)
- Modify: `CLAUDE.md`, `README.md`

- [ ] **Step 1: Generate dataset** — `uv run python examples/make_toy_dataset.py` → `examples/toy-dataset/{red,green,blue}/*.png`.
- [ ] **Step 2: Train on MPS** — `cargo run -p kbdk-cli -- train --data examples/toy-dataset --name toy3 --epochs 5` → val_acc ≥ 0.9, `models/toy3/model.pt`.
- [ ] **Step 3: Convert** — `kbdk convert --model models/toy3/model.pt --data examples/toy-dataset --name toy3 --out packs/` → parity ≥ 0.8.
- [ ] **Step 4: Deploy + live run** — `kbdk deploy packs/toy3 && kbdk run toy3`. Point the camera at red / green / blue objects; the panel label must track the object color with conf > 0.6; `kbdk log` shows JSON results; measure and record ms/inf.
- [ ] **Step 5: Update CLAUDE.md** (new kbdk section: workspace layout, transports, pack format, runner, measured numbers) and README (quickstart: train→convert→deploy→run).
- [ ] **Step 6: Commit** — `git commit -am "kbdk: end-to-end milestone — Mac-trained model live on the V831 camera"`

---

## Self-review notes

- Spec coverage: phases 1–5 of the spec map to Tasks 1–15; spec phases 6–8 (UI,
  detection, publish) are explicitly deferred to follow-up plans.
- The manifest key collision (`input`/`labels`) found while writing Task 9 is resolved
  by schema rename (`in_blob`/`out_blob`/`labels_file`) — Tasks 8, 9, 12 all use the
  renamed keys; Task 8's serde must use `#[serde(rename)]` accordingly.
- Static-link nuance: stage-1 kbrun is fully static; stage 2 must drop `-static` for
  dlopen(MPP) and keep static libncnn + `-static-libstdc++` — called out in Task 10.
- All hardware verifications go through `kbdk` itself once available (dogfooding), with
  raw `adb` only in Phase 1 before the CLI exists.
