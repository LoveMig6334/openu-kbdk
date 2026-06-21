//! Examples tab: a curated gallery of single-file, heavily-commented board-C
//! templates (`examples/board/*.c`). Each template carries a `kbdk-example`
//! header-comment metadata block (name / desc / extra_args) which this module
//! parses; the tab renders a card per template with a `Load →` button that
//! drops the source straight into the Edit tab (buffer + open_path + the
//! template's `extra_args`) so it can be built and run on the board.
//!
//! No board I/O — the scan is a synchronous local-filesystem read on the UI
//! thread (matches `files_tab::read_local_dir`), cached until `⟳ Refresh`.

use crate::app::{KbdkApp, Tab};
use crate::theme;
use eframe::egui;
use std::path::{Path, PathBuf};

/// Parsed metadata from a template's `kbdk-example` header comment.
pub struct ParsedMeta {
    pub name: String,
    pub desc: String,
    pub extra_args: String,
    /// `mpp_libs: true` ⇒ Load also ticks the Edit tab's "MPP libs" so the program
    /// runs with `LD_LIBRARY_PATH=/usr/lib/eyesee-mpp:/usr/lib` (camera/audio).
    pub mpp_libs: bool,
}

/// A scanned template: parsed metadata plus the source path.
pub struct Example {
    pub name: String,
    pub desc: String,
    pub extra_args: String,
    pub mpp_libs: bool,
    pub path: PathBuf,
}

/// Parse the leading `kbdk-example` header-comment metadata block.
///
/// Convention (verbatim from the spec):
/// - The block is the opening `/* … */` comment that begins on the file's first
///   non-blank line. Only this first block comment is scanned.
/// - Its first content line must be the literal marker `kbdk-example`; otherwise
///   this returns `None` (a plain doc comment is never mistaken for metadata).
/// - Each subsequent line is `key: value`. Strip leading whitespace and a leading
///   `*` + optional space (the comment margin) first. Key = up to the first `:`,
///   value = the rest, trimmed (split-once, so a `:` in the value is kept).
/// - Recognized keys: `name`, `desc`, `extra_args`, `mpp_libs` (lowercase,
///   case-sensitive); unknown keys are ignored. `mpp_libs` is `true` for the
///   value `true`/`1`, else `false`.
/// - `name` and `desc` are required (missing → `None`); `extra_args` is optional
///   (absent or empty → `""`).
/// - The block ends at the first `*/`; later lines are ignored.
pub fn parse_metadata(source: &str) -> Option<ParsedMeta> {
    // Find the opening `/*` of the first block comment, allowing only blank lines
    // (whitespace) before it — the block must begin on the first non-blank line.
    let trimmed = source.trim_start();
    let rest = trimmed.strip_prefix("/*")?;
    // Cut off everything from the first `*/` onward (block end).
    let body = match rest.find("*/") {
        Some(end) => &rest[..end],
        None => rest,
    };

    let mut name: Option<String> = None;
    let mut desc: Option<String> = None;
    let mut extra_args = String::new();
    let mut mpp_libs = false;
    let mut seen_marker = false;

    for raw in body.lines() {
        // Strip leading whitespace + a leading `*` + optional single space.
        let mut line = raw.trim_start();
        if let Some(after) = line.strip_prefix('*') {
            line = after.strip_prefix(' ').unwrap_or(after);
        }
        let line = line.trim_end();
        if line.is_empty() {
            continue;
        }
        if !seen_marker {
            // The first content line must be the literal marker.
            if line == "kbdk-example" {
                seen_marker = true;
                continue;
            } else {
                return None;
            }
        }
        // `key: value`, split on the first `:`.
        let Some((key, value)) = line.split_once(':') else {
            continue;
        };
        let value = value.trim();
        match key.trim() {
            "name" => name = Some(value.to_string()),
            "desc" => desc = Some(value.to_string()),
            "extra_args" => extra_args = value.to_string(),
            "mpp_libs" => mpp_libs = matches!(value, "true" | "1"),
            _ => {} // unknown key ignored
        }
    }

    Some(ParsedMeta {
        name: name?,
        desc: desc?,
        extra_args,
        mpp_libs,
    })
}

/// Scan `dir` for `*.c`/`*.cpp` templates with valid `kbdk-example` metadata,
/// returning them sorted by display name. Files without valid metadata (or that
/// fail to read) are skipped with a log line.
pub fn scan_examples(dir: &Path) -> Vec<Example> {
    let mut out: Vec<Example> = vec![];
    let entries = match std::fs::read_dir(dir) {
        Ok(e) => e,
        Err(e) => {
            eprintln!("examples: cannot read {}: {e}", dir.display());
            return out;
        }
    };
    for entry in entries.flatten() {
        let path = entry.path();
        let ext = path.extension().and_then(|s| s.to_str()).unwrap_or("");
        if ext != "c" && ext != "cpp" {
            continue;
        }
        let source = match std::fs::read_to_string(&path) {
            Ok(s) => s,
            Err(e) => {
                eprintln!("examples: skip {} (read error: {e})", path.display());
                continue;
            }
        };
        match parse_metadata(&source) {
            Some(m) => out.push(Example {
                name: m.name,
                desc: m.desc,
                extra_args: m.extra_args,
                mpp_libs: m.mpp_libs,
                path,
            }),
            None => eprintln!("examples: skip {} (no kbdk-example metadata)", path.display()),
        }
    }
    out.sort_by_key(|e| e.name.to_lowercase());
    out
}

pub fn show(app: &mut KbdkApp, ui: &mut egui::Ui) {
    ui.horizontal(|ui| {
        ui.heading("Examples");
        if ui.button("⟳ Refresh").clicked() {
            app.examples.clear();
            app.examples_scanned = false;
        }
        ui.label(egui::RichText::new(&app.examples_status).color(theme::SUBTEXT));
    });
    ui.label(
        egui::RichText::new(
            "Curated board-C templates — pick one to load it into the Edit tab, then build + run.",
        )
        .color(theme::SUBTEXT),
    );
    ui.separator();

    // Scan once (first open / after a Refresh). Guarded by an explicit flag, not
    // `examples.is_empty()`, so a genuinely empty or missing dir does not re-scan
    // the filesystem on the UI thread every frame.
    if !app.examples_scanned {
        let dir = app.workers.repo_root.join("examples/board");
        app.examples = scan_examples(&dir);
        app.examples_scanned = true;
        app.examples_status = if app.examples.is_empty() {
            String::new()
        } else {
            format!("{} templates", app.examples.len())
        };
    }

    if app.examples.is_empty() {
        ui.label(
            egui::RichText::new("no templates found in examples/board").color(theme::SUBTEXT),
        );
        return;
    }

    egui::ScrollArea::vertical()
        .auto_shrink([false, false])
        .show(ui, |ui| {
            // Take the cards' data so the immutable borrow of `app.examples`
            // doesn't conflict with the Load handler's `&mut app`.
            let cards: Vec<(String, String, String, bool, PathBuf)> = app
                .examples
                .iter()
                .map(|e| {
                    (
                        e.name.clone(),
                        e.desc.clone(),
                        e.extra_args.clone(),
                        e.mpp_libs,
                        e.path.clone(),
                    )
                })
                .collect();

            for (name, desc, extra_args, mpp_libs, path) in cards {
                egui::Frame::group(ui.style())
                    .fill(theme::MANTLE)
                    .show(ui, |ui| {
                        ui.set_width(ui.available_width());
                        ui.horizontal(|ui| {
                            ui.vertical(|ui| {
                                ui.label(
                                    egui::RichText::new(&name).color(theme::TEXT).strong(),
                                );
                                ui.label(
                                    egui::RichText::new(&desc).color(theme::SUBTEXT),
                                );
                                if !extra_args.is_empty() {
                                    ui.label(
                                        egui::RichText::new(&extra_args)
                                            .monospace()
                                            .small()
                                            .color(theme::OVERLAY0),
                                    );
                                }
                            });
                            ui.with_layout(
                                egui::Layout::right_to_left(egui::Align::Center),
                                |ui| {
                                    if ui.button("Load →").clicked() {
                                        load(app, &name, &path, &extra_args, mpp_libs);
                                    }
                                },
                            );
                        });
                    });
                ui.add_space(6.0);
            }
        });
}

/// Read the template source and hand it off to the Edit tab via the shared
/// `edit_tab::load_into_editor` (buffer + open_path + the template's `extra_args`
/// and `mpp_libs`, all build state reset), then switch to the Edit tab.
fn load(app: &mut KbdkApp, name: &str, path: &Path, extra_args: &str, mpp_libs: bool) {
    match std::fs::read_to_string(path) {
        Ok(text) => {
            crate::edit_tab::load_into_editor(
                app,
                path.to_path_buf(),
                text,
                Some(extra_args.to_string()),
                Some(mpp_libs),
            );
            app.examples_status = format!("loaded {name}");
            app.f.tab = Tab::Edit;
        }
        Err(e) => app.examples_status = format!("load failed: {e}"),
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn meta(src: &str) -> Option<ParsedMeta> {
        parse_metadata(src)
    }

    #[test]
    fn valid_full() {
        let src = "/* kbdk-example\n * name: Hello\n * desc: a smoke test\n * extra_args: -lm -lpthread\n */\nint main(){}";
        let m = meta(src).expect("parses");
        assert_eq!(m.name, "Hello");
        assert_eq!(m.desc, "a smoke test");
        assert_eq!(m.extra_args, "-lm -lpthread");
    }

    #[test]
    fn margin_stripped() {
        let src = "/* kbdk-example\n * name: X\n * desc: Y\n */";
        let m = meta(src).expect("parses");
        assert_eq!(m.name, "X");
        assert_eq!(m.desc, "Y");
    }

    #[test]
    fn no_extra_args() {
        let src = "/* kbdk-example\n * name: X\n * desc: Y\n */";
        let m = meta(src).expect("parses");
        assert_eq!(m.extra_args, "");
    }

    #[test]
    fn empty_extra_args() {
        // an explicit empty `extra_args:` line still yields "".
        let src = "/* kbdk-example\n * name: X\n * desc: Y\n * extra_args:\n */";
        let m = meta(src).expect("parses");
        assert_eq!(m.extra_args, "");
    }

    #[test]
    fn not_a_kbdk_block() {
        let src = "/* something else\n * name: X\n * desc: Y\n */";
        assert!(meta(src).is_none());
    }

    #[test]
    fn no_block_comment() {
        let src = "int main() { return 0; }";
        assert!(meta(src).is_none());
    }

    #[test]
    fn missing_name() {
        let src = "/* kbdk-example\n * desc: Y\n */";
        assert!(meta(src).is_none());
    }

    #[test]
    fn missing_desc() {
        let src = "/* kbdk-example\n * name: X\n */";
        assert!(meta(src).is_none());
    }

    #[test]
    fn unknown_key_ignored() {
        let src = "/* kbdk-example\n * name: X\n * author: someone\n * desc: Y\n */";
        let m = meta(src).expect("parses");
        assert_eq!(m.name, "X");
        assert_eq!(m.desc, "Y");
    }

    #[test]
    fn colon_in_value() {
        let src = "/* kbdk-example\n * name: X\n * desc: see http://x:8080\n */";
        let m = meta(src).expect("parses");
        assert_eq!(m.desc, "see http://x:8080");
    }

    #[test]
    fn block_end_stops_parsing() {
        // a `name:` line after `*/` must not be picked up.
        let src = "/* kbdk-example\n * name: X\n * desc: Y\n */\n/* name: WRONG */";
        let m = meta(src).expect("parses");
        assert_eq!(m.name, "X");
    }

    #[test]
    fn leading_blank_lines() {
        let src = "\n\n/* kbdk-example\n * name: X\n * desc: Y\n */";
        let m = meta(src).expect("parses");
        assert_eq!(m.name, "X");
    }

    // --- Drift guard: the literal header strings of the five shipped templates ---

    #[test]
    fn fixture_hello() {
        let src = "/* kbdk-example\n * name: Hello (hard-float)\n * desc: printf + sqrt smoke test — proves the FPU / hard-float ABI works\n * extra_args: -lm\n */\n";
        let m = meta(src).expect("parses");
        assert_eq!(m.name, "Hello (hard-float)");
        assert_eq!(
            m.desc,
            "printf + sqrt smoke test — proves the FPU / hard-float ABI works"
        );
        assert_eq!(m.extra_args, "-lm");
    }

    #[test]
    fn fixture_screen() {
        let src = "/* kbdk-example\n * name: Screen colour bars\n * desc: mmap /dev/fb0 and paint colour bars + a white border (panel test)\n * extra_args:\n */\n";
        let m = meta(src).expect("parses");
        assert_eq!(m.name, "Screen colour bars");
        assert_eq!(
            m.desc,
            "mmap /dev/fb0 and paint colour bars + a white border (panel test)"
        );
        assert_eq!(m.extra_args, "");
    }

    #[test]
    fn fixture_audio() {
        let src = "/* kbdk-example\n * name: Audio tone\n * desc: play a 440 Hz sine for 2 s via raw SNDRV_PCM_IOCTL (no alsa-lib)\n * extra_args: -lm\n */\n";
        let m = meta(src).expect("parses");
        assert_eq!(m.name, "Audio tone");
        assert_eq!(
            m.desc,
            "play a 440 Hz sine for 2 s via raw SNDRV_PCM_IOCTL (no alsa-lib)"
        );
        assert_eq!(m.extra_args, "-lm");
    }

    #[test]
    fn fixture_camera() {
        let src = "/* kbdk-example\n * name: Camera preview\n * desc: Live colour-corrected camera on the LCD (MPP capture -> fb0)\n * extra_args: -DAWCHIP=0x1817 -Ivendor/eyesee-mpp/sun8iw19p1/include -Ivendor/eyesee-mpp/sun8iw19p1/include/media -Ivendor/eyesee-mpp/sun8iw19p1/include/utils -ldl -lpthread\n * mpp_libs: true\n */\n";
        let m = meta(src).expect("parses");
        assert_eq!(m.name, "Camera preview");
        assert_eq!(
            m.desc,
            "Live colour-corrected camera on the LCD (MPP capture -> fb0)"
        );
        assert_eq!(
            m.extra_args,
            "-DAWCHIP=0x1817 -Ivendor/eyesee-mpp/sun8iw19p1/include -Ivendor/eyesee-mpp/sun8iw19p1/include/media -Ivendor/eyesee-mpp/sun8iw19p1/include/utils -ldl -lpthread"
        );
        assert!(m.mpp_libs, "camera template must request MPP runtime libs");
    }

    #[test]
    fn mpp_libs_defaults_false() {
        let src = "/* kbdk-example\n * name: X\n * desc: Y\n */";
        assert!(!meta(src).expect("parses").mpp_libs);
    }

    #[test]
    fn fixture_ledblink() {
        let src = "/* kbdk-example\n * name: LED blink (GPIO)\n * desc: blink an LED via the raw /dev/gpiochip chardev ioctl (GPIO_V2, v1 fallback)\n * extra_args:\n */\n";
        let m = meta(src).expect("parses");
        assert_eq!(m.name, "LED blink (GPIO)");
        assert_eq!(
            m.desc,
            "blink an LED via the raw /dev/gpiochip chardev ioctl (GPIO_V2, v1 fallback)"
        );
        assert_eq!(m.extra_args, "");
    }

    /// Smoke test over the real templates: exactly the five expected names.
    #[test]
    fn scan_real_examples() {
        let dir = std::path::Path::new(env!("CARGO_MANIFEST_DIR"))
            .join("../../examples/board");
        let found = scan_examples(&dir);
        let names: Vec<&str> = found.iter().map(|e| e.name.as_str()).collect();
        assert_eq!(
            names,
            vec![
                "Audio tone",
                "Camera preview",
                "Hello (hard-float)",
                "LED blink (GPIO)",
                "Screen colour bars",
            ]
        );
    }
}
