// Unit tests for the TCP frame protocol (no hardware): a local server stands in
// for kbrun's one-shot frame listener ("send KBF1 frame, close").
use kbdk_core::frames;
use std::io::Write;
use std::net::TcpListener;
use std::time::Duration;

/// Serve `payload` to the first connection, then close. Returns the port.
fn serve_once(payload: Vec<u8>) -> u16 {
    let l = TcpListener::bind("127.0.0.1:0").unwrap();
    let port = l.local_addr().unwrap().port();
    std::thread::spawn(move || {
        if let Ok((mut s, _)) = l.accept() {
            let _ = s.write_all(&payload);
        }
    });
    port
}

fn kbf1(w: u16, h: u16, rgb: &[u8]) -> Vec<u8> {
    let mut p = vec![b'K', b'B', b'F', b'1'];
    p.extend_from_slice(&w.to_le_bytes());
    p.extend_from_slice(&h.to_le_bytes());
    p.extend_from_slice(rgb);
    p
}

#[test]
fn fetch_reads_one_kbf1_frame() {
    let rgb: Vec<u8> = (0..4 * 3 * 3).map(|i| i as u8).collect();
    let port = serve_once(kbf1(4, 3, &rgb));
    let f = frames::fetch_frame(port, Duration::from_secs(2)).unwrap();
    assert_eq!((f.w, f.h), (4, 3));
    assert_eq!(f.rgb, rgb);
}

#[test]
fn fetch_rejects_bad_magic() {
    let mut p = kbf1(4, 3, &[0u8; 36]);
    p[0] = b'X';
    let port = serve_once(p);
    assert!(frames::fetch_frame(port, Duration::from_secs(2)).is_err());
}

#[test]
fn fetch_errors_on_truncated_frame() {
    // server closes after half the pixel data
    let rgb: Vec<u8> = vec![7u8; 4 * 3 * 3];
    let mut p = kbf1(4, 3, &rgb);
    p.truncate(8 + rgb.len() / 2);
    let port = serve_once(p);
    assert!(frames::fetch_frame(port, Duration::from_secs(2)).is_err());
}

#[test]
fn fetch_errors_when_nothing_listens() {
    // a bound-then-dropped listener gives us a port with nothing on it
    let port = {
        let l = TcpListener::bind("127.0.0.1:0").unwrap();
        l.local_addr().unwrap().port()
    };
    assert!(frames::fetch_frame(port, Duration::from_millis(500)).is_err());
}
