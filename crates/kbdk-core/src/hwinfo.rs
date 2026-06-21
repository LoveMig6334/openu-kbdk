//! Read-only board hardware inventory + live stats over the Transport. Two
//! batched shell commands (static probe + live probe), each with a pure parser.
//! Probes are defensive: a missing file/tool yields `n/a` for that row and the
//! command ends `; true`, so the panel never errors on an unfamiliar rootfs.

use crate::transport::Transport;
use anyhow::{anyhow, bail, Result};

#[derive(Debug, Clone)]
pub struct HwSection {
    pub title: String,
    pub rows: Vec<(String, String)>,
}

#[derive(Debug, Clone)]
pub struct HwInfo {
    pub sections: Vec<HwSection>,
}

#[derive(Debug, Clone)]
pub struct LiveStats {
    pub load1: f32,
    pub mem_avail_kb: u64,
    pub mem_total_kb: u64,
    pub temp_c: Option<f32>, // None if no thermal sensor
    pub uptime_s: u64,
}

/// Static inventory probe. Emits `SECTION <title>` markers and `KEY<TAB>VALUE`
/// rows; the parser just groups them, so this command can grow/shrink freely.
/// (Raw-string fragments keep shell quoting readable; printf interprets the
/// literal `\t`/`\n`.)
pub const STATIC_PROBE_CMD: &str = concat!(
    r#"echo 'SECTION SoC'; "#,
    r#"printf 'Model\t%s\n' "$(awk -F: '/model name|Hardware/{print $2; exit}' /proc/cpuinfo | sed 's/^ *//')"; "#,
    r#"printf 'CPU part\t%s\n' "$(awk -F: '/CPU part/{print $2; exit}' /proc/cpuinfo | sed 's/^ *//')"; "#,
    r#"printf 'Cores\t%s\n' "$(grep -c ^processor /proc/cpuinfo)"; "#,
    r#"printf 'Features\t%s\n' "$(awk -F: '/Features|flags/{print $2; exit}' /proc/cpuinfo | sed 's/^ *//')"; "#,
    r#"echo 'SECTION Memory'; "#,
    r#"printf 'Total\t%s kB\n' "$(awk '/MemTotal/{print $2}' /proc/meminfo)"; "#,
    r#"echo 'SECTION Kernel'; "#,
    r#"printf 'uname\t%s\n' "$(uname -a)"; "#,
    r#"printf 'Hostname\t%s\n' "$(cat /proc/sys/kernel/hostname 2>/dev/null || echo na)"; "#,
    r#"echo 'SECTION Display'; "#,
    r#"printf 'fb0 size\t%s\n' "$(cat /sys/class/graphics/fb0/virtual_size 2>/dev/null || echo na)"; "#,
    r#"printf 'fb0 modes\t%s\n' "$(head -n1 /sys/class/graphics/fb0/modes 2>/dev/null || echo na)"; "#,
    r#"echo 'SECTION Camera/I2C'; "#,
    r#"printf 'camera\t%s\n' "$(cat /sys/bus/i2c/devices/1-003c/name 2>/dev/null || echo na)"; "#,
    r#"printf 'i2c-1 addrs\t%s\n' "$(a=$(i2cdetect -y -r 1 2>/dev/null | sed '1d' | cut -d: -f2- | tr ' ' '\n' | grep -vE '^(--|UU|)$' | tr '\n' ' '); echo ${a:-na})"; "#,
    r#"echo 'SECTION Storage'; "#,
    r#"printf 'partitions\t%s\n' "$(awk 'NR>2{printf "%s(%sK) ", $4, $3}' /proc/partitions)"; "#,
    r#"echo 'SECTION SPI'; "#,
    r#"printf 'devices\t%s\n' "$(s=$(ls /dev/spidev* 2>/dev/null | tr '\n' ' '); echo ${s:-na})"; "#,
    r#"echo 'SECTION Audio'; "#,
    r#"printf 'devices\t%s\n' "$(s=$(ls /dev/snd 2>/dev/null | tr '\n' ' '); echo ${s:-na})"; "#,
    r#"echo 'SECTION Network'; "#,
    r#"printf 'interfaces\t%s\n' "$(ls /sys/class/net 2>/dev/null | tr '\n' ' ')"; "#,
    r#"echo 'SECTION NPU'; "#,
    r#"printf 'nna node\t%s\n' "$(n=$(ls -d /proc/device-tree/*/nna@* /proc/device-tree/nna@* 2>/dev/null | sed 's#.*/##' | tr '\n' ' '); echo ${n:-na})"; "#,
    r#"printf 'status\t%s\n' "$(s=$(cat /proc/device-tree/*/nna@*/status 2>/dev/null | tr -d '\0'); echo ${s:-na})"; "#,
    "true",
);

pub fn parse_hwinfo(out: &str) -> HwInfo {
    let mut sections: Vec<HwSection> = Vec::new();
    for line in out.lines() {
        if let Some(title) = line.strip_prefix("SECTION ") {
            sections.push(HwSection { title: title.trim().to_string(), rows: Vec::new() });
        } else if let Some((k, v)) = line.split_once('\t') {
            if let Some(sec) = sections.last_mut() {
                sec.rows.push((k.trim().to_string(), v.trim().to_string()));
            }
        }
    }
    HwInfo { sections }
}

pub fn probe_hwinfo(t: &dyn Transport) -> Result<HwInfo> {
    let r = t.exec(STATIC_PROBE_CMD, 15)?;
    if r.rc != 0 {
        bail!("hwinfo probe rc={} ({})", r.rc, r.output.trim());
    }
    Ok(parse_hwinfo(&r.output))
}

/// Live stats probe: one `KEY VALUE` line per metric (space-separated).
pub const LIVE_PROBE_CMD: &str = concat!(
    r#"printf 'load %s\n' "$(cut -d' ' -f1 /proc/loadavg)"; "#,
    r#"printf 'memavail %s\n' "$(awk '/MemAvailable/{print $2}' /proc/meminfo)"; "#,
    r#"printf 'memtotal %s\n' "$(awk '/MemTotal/{print $2}' /proc/meminfo)"; "#,
    r#"printf 'temp %s\n' "$(cat /sys/class/thermal/thermal_zone0/temp 2>/dev/null || echo na)"; "#,
    r#"printf 'uptime %s\n' "$(cut -d' ' -f1 /proc/uptime)"; "#,
    "true",
);

pub fn parse_live(out: &str) -> Option<LiveStats> {
    let (mut load, mut avail, mut total, mut temp, mut up) = (None, None, None, None, None);
    for line in out.lines() {
        let mut it = line.split_whitespace();
        match (it.next(), it.next()) {
            (Some("load"), Some(v)) => load = v.parse::<f32>().ok(),
            (Some("memavail"), Some(v)) => avail = v.parse::<u64>().ok(),
            (Some("memtotal"), Some(v)) => total = v.parse::<u64>().ok(),
            (Some("temp"), Some(v)) => {
                temp = if v == "na" { None } else { v.parse::<f32>().ok().map(|m| m / 1000.0) }
            }
            (Some("uptime"), Some(v)) => up = v.parse::<f64>().ok().map(|s| s as u64),
            _ => {}
        }
    }
    Some(LiveStats {
        load1: load?,
        mem_avail_kb: avail?,
        mem_total_kb: total?,
        temp_c: temp,
        uptime_s: up.unwrap_or(0),
    })
}

pub fn probe_live(t: &dyn Transport) -> Result<LiveStats> {
    let r = t.exec(LIVE_PROBE_CMD, 15)?;
    parse_live(&r.output).ok_or_else(|| anyhow!("live probe: unparseable ({})", r.output.trim()))
}

#[cfg(test)]
mod tests {
    use super::*;

    const STATIC: &str = "\
SECTION SoC
Model\tAllwinner sun8iw19
Cores\t1
SECTION Memory
Total\t60048 kB
SECTION Display
fb0 size\t240,480
";

    #[test]
    fn parse_hwinfo_groups_sections_and_rows() {
        let info = parse_hwinfo(STATIC);
        assert_eq!(info.sections.len(), 3);
        assert_eq!(info.sections[0].title, "SoC");
        assert_eq!(info.sections[0].rows.len(), 2);
        assert_eq!(info.sections[0].rows[0], ("Model".into(), "Allwinner sun8iw19".into()));
        assert_eq!(info.sections[1].title, "Memory");
        assert_eq!(info.sections[1].rows[0], ("Total".into(), "60048 kB".into()));
        assert_eq!(info.sections[2].rows[0], ("fb0 size".into(), "240,480".into()));
    }

    #[test]
    fn parse_hwinfo_tolerates_junk() {
        // a key line before any SECTION is dropped; blank/garbage lines skipped
        let info = parse_hwinfo("stray\tvalue\n\nSECTION X\na\t1\nnotabhere\n");
        assert_eq!(info.sections.len(), 1);
        assert_eq!(info.sections[0].rows, vec![("a".to_string(), "1".to_string())]);
        assert!(parse_hwinfo("").sections.is_empty());
    }

    #[test]
    fn parse_live_reads_all_fields() {
        let s = parse_live("load 0.42\nmemavail 41000\nmemtotal 60048\ntemp 48200\nuptime 1234.56\n").unwrap();
        assert_eq!(s.load1, 0.42);
        assert_eq!(s.mem_avail_kb, 41000);
        assert_eq!(s.mem_total_kb, 60048);
        assert_eq!(s.temp_c, Some(48.2));
        assert_eq!(s.uptime_s, 1234);
    }

    #[test]
    fn parse_live_handles_missing_temp_and_garbage() {
        // temp "na" -> None, but the sample still parses
        let s = parse_live("load 0.1\nmemavail 1\nmemtotal 2\ntemp na\nuptime 5\n").unwrap();
        assert_eq!(s.temp_c, None);
        // missing a required field (no load) -> None
        assert!(parse_live("memavail 1\nmemtotal 2\n").is_none());
        assert!(parse_live("").is_none());
    }
}
