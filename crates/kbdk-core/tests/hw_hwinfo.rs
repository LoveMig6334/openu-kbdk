// Hardware integration smoke test for the hwinfo probes.
// Run with: KBDK_HW=1 cargo test -p kbdk-core --test hw_hwinfo -- --nocapture
use kbdk_core::adb::AdbTransport;
use kbdk_core::hwinfo;

fn enabled() -> bool {
    std::env::var("KBDK_HW").is_ok()
}

#[test]
fn static_inventory_has_core_sections() {
    if !enabled() {
        eprintln!("skipping hw_hwinfo (set KBDK_HW=1 with a board attached)");
        return;
    }
    let t = AdbTransport::new(None);
    let info = hwinfo::probe_hwinfo(&t).expect("probe hwinfo");
    for sec in &info.sections {
        println!("[{}]", sec.title);
        for (k, v) in &sec.rows {
            println!("  {k} = {v}");
        }
    }
    let titles: Vec<&str> = info.sections.iter().map(|s| s.title.as_str()).collect();
    for want in ["SoC", "Memory", "Kernel"] {
        assert!(titles.contains(&want), "missing section {want}; got {titles:?}");
    }
}

#[test]
fn live_sample_is_plausible() {
    if !enabled() {
        return;
    }
    let t = AdbTransport::new(None);
    let s = hwinfo::probe_live(&t).expect("probe live");
    println!("live: {s:?}");
    assert!(s.mem_total_kb > 1000, "implausible MemTotal {}", s.mem_total_kb);
    assert!(s.mem_avail_kb <= s.mem_total_kb);
}
