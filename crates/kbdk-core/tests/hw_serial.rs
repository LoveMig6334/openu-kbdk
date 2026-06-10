// Hardware test over the UART console. Needs the board attached AND the port
// free (no uai monitor/term). Run:
//   KBDK_HW=1 cargo test -p kbdk-core --test hw_serial -- --test-threads=1
use kbdk_core::{serial::SerialTransport, transport::Transport};

fn hw() -> bool {
    std::env::var("KBDK_HW").is_ok()
}

fn port() -> String {
    std::env::var("UAI_PORT").unwrap_or_else(|_| "/dev/cu.usbserial-210".into())
}

#[test]
fn serial_exec_uname() {
    if !hw() {
        return;
    }
    let t = SerialTransport::new(&port(), 115200);
    let r = t.exec("uname -a", 15).unwrap();
    assert_eq!(r.rc, 0);
    assert!(r.output.contains("sipeed"));
}

#[test]
fn serial_push_small_file() {
    if !hw() {
        return;
    }
    let t = SerialTransport::new(&port(), 115200);
    let tmp = std::env::temp_dir().join("kbdk_serial_push.bin");
    std::fs::write(&tmp, b"hello kbdk over serial \x00\xff\x07").unwrap();
    t.push(&tmp, "/tmp/kbdk_serial_push.bin").unwrap();
    t.exec("rm /tmp/kbdk_serial_push.bin", 15).unwrap();
}
