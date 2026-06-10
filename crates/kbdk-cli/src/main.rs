use anyhow::Result;
use clap::{Parser, Subcommand};
use kbdk_core::{adb::AdbTransport, discover, serial::SerialTransport, transport::Transport};
use std::path::PathBuf;

#[derive(Parser)]
#[command(name = "kbdk", about = "KidBright µAI dev-kit")]
struct Cli {
    /// adb serial (defaults to the only attached device)
    #[arg(long, global = true)]
    serial: Option<String>,
    /// auto = adb if present, else serial console
    #[arg(long, global = true, value_parser = ["auto", "adb", "serial"], default_value = "auto")]
    transport: String,
    #[command(subcommand)]
    cmd: Cmd,
}

fn make_transport(kind: &str, serial: Option<String>) -> Result<Box<dyn Transport>> {
    match kind {
        "adb" => Ok(Box::new(AdbTransport::new(serial))),
        "serial" => Ok(Box::new(SerialTransport::new(
            &std::env::var("UAI_PORT").unwrap_or_else(|_| "/dev/cu.usbserial-210".into()),
            115200,
        ))),
        _ => {
            let d = discover::discover()?;
            if !d.adb.is_empty() {
                Ok(Box::new(AdbTransport::new(serial)))
            } else if !d.serial.is_empty() {
                Ok(Box::new(SerialTransport::new(&d.serial[0], 115200)))
            } else {
                anyhow::bail!("no board found (adb or serial)")
            }
        }
    }
}

#[derive(Subcommand)]
enum Cmd {
    /// List attached boards (adb + serial console)
    Devices,
    /// Run a shell command on the board, print output, exit with its rc
    Exec { command: String },
    /// Verified upload (md5-checked on the board, retried — UDISK vfat is flaky)
    Push { local: PathBuf, remote: String },
    /// Download
    Pull { remote: String, local: PathBuf },
    /// Push a model pack dir (manifest.json + model + labels) and the runner binary
    Deploy {
        pack_dir: PathBuf,
        /// runner binary to push (default bin/kbrun)
        #[arg(long, default_value = "bin/kbrun")]
        runner: PathBuf,
    },
    /// Start the runner on a deployed pack (live camera + label overlay)
    Run {
        pack_name: String,
        #[arg(long, default_value = "320x240")]
        res: String,
        /// 0 = run until `kbdk stop`
        #[arg(long, default_value_t = 0)]
        frames: u32,
    },
    /// Convert a TorchScript model into a deployable int8 ncnn pack (via uv/Python)
    Convert {
        #[arg(long)]
        model: PathBuf,
        /// ImageFolder dataset (labels from class dirs; calibration images)
        #[arg(long)]
        data: PathBuf,
        #[arg(long)]
        name: String,
        #[arg(long, default_value = "packs")]
        out: PathBuf,
        #[arg(long, default_value_t = 224)]
        size: u32,
        #[arg(long, default_value = "mobilenet_v2")]
        backbone: String,
    },
    /// Stop a running kbrun
    Stop,
    /// Show the runner's recent JSON results + stderr tail
    Log,
}

/// Absolute path for a possibly-not-yet-existing dir (canonicalize needs existence).
fn abs_path(p: &std::path::Path) -> Result<String> {
    let abs = if p.is_absolute() {
        p.to_path_buf()
    } else {
        std::env::current_dir()?.join(p)
    };
    Ok(abs.display().to_string())
}

/// Run a Python console script from the py/ uv workspace, re-emitting its
/// JSON-lines progress (stdout passes through; events also summarized to stderr).
fn run_py_streaming(script: &str, args: &[&str]) -> Result<()> {
    use std::io::BufRead;
    let repo = std::env::current_dir()?;
    let mut child = std::process::Command::new("uv")
        .current_dir(repo.join("py"))
        .arg("run")
        .arg(script)
        .args(args)
        .stdout(std::process::Stdio::piped())
        .spawn()
        .map_err(|e| anyhow::anyhow!("spawn uv: {e} (is uv installed?)"))?;
    for line in std::io::BufReader::new(child.stdout.take().unwrap()).lines() {
        let line = line?;
        if let Ok(v) = serde_json::from_str::<serde_json::Value>(&line) {
            let ev = v["event"].as_str().unwrap_or("?");
            match ev {
                "error" => eprintln!("[{script}] ERROR: {}", v["msg"].as_str().unwrap_or("?")),
                _ => eprintln!("[{script}] {ev} {}", summary(&v)),
            }
        }
        println!("{line}");
    }
    let st = child.wait()?;
    if !st.success() {
        anyhow::bail!("{script} failed");
    }
    Ok(())
}

fn summary(v: &serde_json::Value) -> String {
    let mut parts = vec![];
    for (k, val) in v.as_object().into_iter().flatten() {
        if k != "event" && !val.is_null() {
            parts.push(format!("{k}={val}"));
        }
    }
    parts.join(" ")
}

fn main() -> Result<()> {
    let cli = Cli::parse();
    match cli.cmd {
        Cmd::Devices => {
            let d = discover::discover()?;
            for s in &d.adb {
                println!("adb\t{s}");
            }
            for p in &d.serial {
                println!("serial\t{p}");
            }
        }
        Cmd::Exec { command } => {
            let t = make_transport(&cli.transport, cli.serial)?;
            let r = t.exec(&command, 60)?;
            print!("{}", r.output);
            std::process::exit(r.rc);
        }
        Cmd::Push { local, remote } => {
            make_transport(&cli.transport, cli.serial)?.push(&local, &remote)?
        }
        Cmd::Pull { remote, local } => {
            make_transport(&cli.transport, cli.serial)?.pull(&remote, &local)?
        }
        Cmd::Deploy { pack_dir, runner } => {
            let t = make_transport(&cli.transport, cli.serial)?;
            if runner.exists() {
                kbdk_core::deploy::deploy_runner(t.as_ref(), &runner)?;
                eprintln!("runner pushed to {}", kbdk_core::deploy::RUNNER);
            }
            let remote = kbdk_core::deploy::deploy_pack(t.as_ref(), &pack_dir)?;
            println!("deployed: {remote}");
        }
        Cmd::Run {
            pack_name,
            res,
            frames,
        } => {
            let t = make_transport(&cli.transport, cli.serial)?;
            let remote = format!("{}/{pack_name}", kbdk_core::deploy::BOARD_PACK_ROOT);
            kbdk_core::deploy::start_runner(t.as_ref(), &remote, &res, frames)?;
            println!("running {pack_name} ({res}); follow with `kbdk log`, stop with `kbdk stop`");
        }
        Cmd::Convert {
            model,
            data,
            name,
            out,
            size,
            backbone,
        } => {
            run_py_streaming(
                "kbdk-convert",
                &[
                    "--model",
                    &model.canonicalize()?.display().to_string(),
                    "--data",
                    &data.canonicalize()?.display().to_string(),
                    "--name",
                    &name,
                    "--out",
                    &abs_path(&out)?,
                    "--width",
                    &size.to_string(),
                    "--height",
                    &size.to_string(),
                    "--backbone",
                    &backbone,
                ],
            )?;
        }
        Cmd::Stop => {
            kbdk_core::deploy::stop_runner(make_transport(&cli.transport, cli.serial)?.as_ref())?;
            println!("stopped");
        }
        Cmd::Log => {
            let t = make_transport(&cli.transport, cli.serial)?;
            let r = t.exec("tail -n 20 /tmp/kbrun.log 2>/dev/null; echo ---; tail -n 3 /tmp/kbrun.err 2>/dev/null | grep -v '^I'", 30)?;
            print!("{}", r.output);
        }
    }
    Ok(())
}
