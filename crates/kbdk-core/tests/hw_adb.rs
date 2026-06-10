// Hardware integration test — needs the board attached over USB-OTG.
// Run with: KBDK_HW=1 cargo test -p kbdk-core --test hw_adb -- --nocapture
use kbdk_core::{adb::AdbTransport, transport::Transport};

fn hw() -> bool {
    std::env::var("KBDK_HW").is_ok()
}

#[test]
fn exec_uname_and_rc() {
    if !hw() {
        return;
    }
    let t = AdbTransport::new(None);
    let r = t.exec("uname -a", 15).unwrap();
    assert_eq!(r.rc, 0);
    assert!(r.output.contains("sipeed"));
    assert_eq!(t.exec("false", 15).unwrap().rc, 1);
}

#[test]
fn push_verify_roundtrip() {
    if !hw() {
        return;
    }
    let t = AdbTransport::new(None);
    let tmp = std::env::temp_dir().join("kbdk_push_test.bin");
    std::fs::write(&tmp, vec![0xA5u8; 200_000]).unwrap();
    t.push(&tmp, "/tmp/kbdk_push_test.bin").unwrap();
    let back = std::env::temp_dir().join("kbdk_pull_test.bin");
    t.pull("/tmp/kbdk_push_test.bin", &back).unwrap();
    assert_eq!(std::fs::read(&tmp).unwrap(), std::fs::read(&back).unwrap());
    t.exec("rm /tmp/kbdk_push_test.bin", 15).unwrap();
}
