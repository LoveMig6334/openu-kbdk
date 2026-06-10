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
    /// Stop a running kbrun
    Stop,
    /// Show the runner's recent JSON results + stderr tail
    Log,
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
