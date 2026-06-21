// Hardware integration smoke test for fs + procs.
// Run with: KBDK_HW=1 cargo test -p kbdk-core --test hw_fs -- --nocapture
use kbdk_core::adb::AdbTransport;
use kbdk_core::transport::Transport;
use kbdk_core::{fs, procs};

fn enabled() -> bool {
    std::env::var("KBDK_HW").is_ok()
}

#[test]
fn list_tmp_and_procs() {
    if !enabled() {
        eprintln!("skipping hw_fs (set KBDK_HW=1 with a board attached)");
        return;
    }
    let t = AdbTransport::new(None);

    let entries = fs::list_dir(&t, "/tmp").expect("list /tmp");
    println!("/tmp has {} entries", entries.len());

    let procs = procs::list_procs(&t).expect("list procs");
    assert!(procs.iter().any(|p| p.pid == 1), "expected pid 1 in list");
    println!("{} processes; heaviest: {:?}", procs.len(), procs.first());
}

#[test]
fn push_read_remove_roundtrip() {
    if !enabled() {
        return;
    }
    let t = AdbTransport::new(None);
    let local = std::env::temp_dir().join("kbdk_hw_fs.txt");
    std::fs::write(&local, b"hello board\n").unwrap();
    t.push(&local, "/tmp/kbdk_hw_fs.txt").expect("push");

    let back = std::env::temp_dir().join("kbdk_hw_fs_back.txt");
    let bytes = fs::read_head(&t, "/tmp/kbdk_hw_fs.txt", 4096, &back).expect("read_head");
    assert_eq!(&bytes, b"hello board\n");
    assert!(!fs::looks_binary(&bytes));

    fs::remove(&t, "/tmp/kbdk_hw_fs.txt", false).expect("remove");
}

#[test]
fn procs_cmdline_and_state_parse() {
    if !enabled() {
        return;
    }
    let t = AdbTransport::new(None);
    let ps = procs::list_procs(&t).expect("list procs");
    // print the heaviest dozen for eyeballing
    for p in ps.iter().take(12) {
        println!("pid={:<6} rss={:<8} state={:<3} cmd={}", p.pid, p.rss_kb, p.state, p.cmd);
    }
    // states should be single-letter codes (R/S/D/Z/T/t/I/x...), never multi-token
    assert!(ps.iter().all(|p| p.state.chars().count() <= 1 || p.state == "?"),
        "a state field is not a single char — comm-with-spaces parsing likely broke");
    // at least one process should have a multi-token cmdline, proving NUL->space worked
    let multi = ps.iter().filter(|p| p.cmd.contains(' ') && !p.cmd.starts_with('[')).count();
    println!("{} processes with multi-token cmdlines", multi);
    assert!(multi >= 1, "no multi-arg cmdlines — tr '\\0' ' ' may not be translating NULs on this busybox");
}
