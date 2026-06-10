//! Board discovery: adb serials (the board enumerates as VID 0x18d1 PID 0x0002
//! "MaixPy3", serial 20080411) and the UART console bridge (/dev/cu.usbserial-*).

use anyhow::Result;
use std::process::Command;

#[derive(Debug)]
pub struct Devices {
    pub adb: Vec<String>,    // adb serials (board: "20080411")
    pub serial: Vec<String>, // /dev/cu.usbserial-*
}

pub fn parse_adb_devices(out: &str) -> Vec<String> {
    out.lines()
        .skip(1)
        .filter(|l| l.contains("\tdevice") || l.contains(" device"))
        .filter_map(|l| l.split_whitespace().next().map(str::to_string))
        .collect()
}

pub fn serial_port_candidates(paths: &[String]) -> Vec<String> {
    paths
        .iter()
        .filter(|p| p.starts_with("/dev/cu.usbserial"))
        .cloned()
        .collect()
}

pub fn discover() -> Result<Devices> {
    let adb_out = Command::new("adb")
        .arg("devices")
        .output()
        .map(|o| String::from_utf8_lossy(&o.stdout).into_owned())
        .unwrap_or_default();
    let ports: Vec<String> = std::fs::read_dir("/dev")?
        .filter_map(|e| e.ok())
        .map(|e| e.path().display().to_string())
        .collect();
    Ok(Devices {
        adb: parse_adb_devices(&adb_out),
        serial: serial_port_candidates(&ports),
    })
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn parses_adb_devices() {
        let out = "List of devices attached\n20080411               device usb:1-1 transport_id:1\n\n";
        assert_eq!(parse_adb_devices(out), vec!["20080411".to_string()]);
    }

    #[test]
    fn finds_serial_ports() {
        let paths = vec![
            "/dev/cu.usbserial-210".to_string(),
            "/dev/cu.Bluetooth".to_string(),
        ];
        assert_eq!(
            serial_port_candidates(&paths),
            vec!["/dev/cu.usbserial-210".to_string()]
        );
    }
}
