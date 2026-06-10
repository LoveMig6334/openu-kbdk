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
    /// Verified upload (md5-checked on the board, retried — UDISK vfat is flaky)
    Push { local: PathBuf, remote: String },
    /// Download
    Pull { remote: String, local: PathBuf },
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
