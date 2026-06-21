//! Edit tab: a minimal on-board IDE — a local source tree, a monospace code
//! editor, and a toolbar that cross-compiles the open file (via
//! `kbdk_core::build`) and deploys + runs it on the board over ADB, streaming
//! both the compiler output and the program's stdout into a bottom log panel.
//!
//! All blocking work (compile, push, the long-lived `adb shell`) runs on worker
//! threads (`Workers::build`/`deploy_run`/`stop_run`); this module only renders
//! and dispatches.

use crate::app::{EditState, KbdkApp};
use crate::theme;
use crate::files_tab;
use eframe::egui;

/// `buffer != saved_snapshot` ⇒ the editor has unsaved changes.
fn is_dirty(s: &EditState) -> bool {
    s.buffer != s.saved_snapshot
}

/// Drop `text` into the editor as the open file at `path`, resetting all build
/// state (dirty snapshot, last binary, built-from snapshot, output log). When
/// `extra_args`/`mpp_libs` are `Some` they override the persisted Edit-tab fields
/// — the Examples tab's Load uses this so a loaded template carries its own
/// compile flags / MPP runtime env. The single place EditState is (re)populated.
pub fn load_into_editor(
    app: &mut KbdkApp,
    path: std::path::PathBuf,
    text: String,
    extra_args: Option<String>,
    mpp_libs: Option<bool>,
) {
    app.edit.buffer = text;
    app.edit.saved_snapshot = app.edit.buffer.clone();
    app.edit.open_path = Some(path.clone());
    app.edit.last_bin = None;
    app.edit.built_from = None;
    app.edit.output.clear();
    app.edit.status = format!("opened {}", path.display());
    if let Some(a) = extra_args {
        app.f.edit_extra_args = a;
    }
    if let Some(m) = mpp_libs {
        app.f.edit_mpp_libs = m;
    }
}

/// Load a source file into the editor (file-tree click / Open… dialog). On error
/// the buffer is left untouched and the error goes into the status line.
fn open_file(app: &mut KbdkApp, path: std::path::PathBuf) {
    match std::fs::read_to_string(&path) {
        Ok(text) => load_into_editor(app, path, text, None, None),
        Err(e) => app.edit.status = format!("open failed: {e}"),
    }
}

pub fn show(app: &mut KbdkApp, ui: &mut egui::Ui) {
    // Header
    ui.horizontal(|ui| {
        ui.heading("Edit");
        ui.label(egui::RichText::new(&app.edit.status).color(theme::SUBTEXT));
    });
    ui.separator();

    // Toolbar row.
    toolbar(app, ui);
    ui.separator();

    // Left file tree (reserve its width first).
    egui::Panel::left("edit_tree")
        .resizable(true)
        .default_size(240.0)
        .show_inside(ui, |ui| {
            ui.label(egui::RichText::new(format!("FILES  {}", app.edit.tree.root)).strong());
            egui::ScrollArea::vertical()
                .id_salt("edit_tree_scroll")
                .auto_shrink([false, false])
                .show(ui, |ui| {
                    let root = app.edit.tree.root.clone();
                    edit_node(ui, app, &root);
                });
        });

    // Bottom output panel (reserve its height before the editor claims the rest).
    egui::Panel::bottom("edit_output")
        .resizable(true)
        .default_size(180.0)
        .show_inside(ui, |ui| {
            ui.horizontal(|ui| {
                ui.label(egui::RichText::new("output").strong());
                if ui.small_button("clear").clicked() {
                    app.edit.output.clear();
                }
            });
            egui::ScrollArea::vertical()
                .id_salt("edit_output_scroll")
                .stick_to_bottom(true)
                .auto_shrink([false, false])
                .show(ui, |ui| {
                    ui.add(
                        egui::Label::new(
                            egui::RichText::new(app.edit.output.join("\n")).monospace(),
                        )
                        .wrap(),
                    );
                });
        });

    // Editor fills the remaining central area.
    egui::CentralPanel::default().show_inside(ui, |ui| {
        egui::ScrollArea::vertical()
            .id_salt("edit_editor_scroll")
            .auto_shrink([false, false])
            .show(ui, |ui| {
                ui.add_sized(
                    ui.available_size(),
                    egui::TextEdit::multiline(&mut app.edit.buffer)
                        .code_editor()
                        .lock_focus(true)
                        .desired_width(f32::INFINITY),
                );
            });
    });
}

/// The Open / Save / Build / Deploy+Run / Stop row, plus the MPP-libs toggle, the
/// extra-build-args field, and the dirty/status indicator.
fn toolbar(app: &mut KbdkApp, ui: &mut egui::Ui) {
    ui.horizontal_wrapped(|ui| {
        // Open… (host file dialog)
        if ui.button("Open…").clicked() {
            if let Some(path) = rfd::FileDialog::new().pick_file() {
                open_file(app, path);
            }
        }

        // Save (auto-snapshots on success)
        let can_save = app.edit.open_path.is_some();
        if ui.add_enabled(can_save, egui::Button::new("Save")).clicked() {
            save_buffer(app);
        }

        // Build (auto-save, then cross-compile). Disabled while a program runs so
        // a rebuild can't wipe the live run log / orphan the deployed binary.
        let can_build =
            app.edit.open_path.is_some() && !app.edit.building && !app.edit.running;
        if ui
            .add_enabled(can_build, egui::Button::new("Build"))
            .clicked()
        {
            start_build(app);
        }

        // Deploy + Run: only when the built binary still matches the editor buffer
        // — a clean, current build. Editing the code or switching files changes the
        // buffer and invalidates the build, so this can't deploy a stale binary.
        let can_run = app.edit.last_bin.is_some()
            && !app.edit.running
            && !app.edit.building
            && app.edit.built_from.as_deref() == Some(app.edit.buffer.as_str());
        if ui
            .add_enabled(can_run, egui::Button::new("Deploy + Run"))
            .clicked()
        {
            start_run(app);
        }

        // Stop (only while running)
        if ui
            .add_enabled(app.edit.running, egui::Button::new("Stop"))
            .clicked()
        {
            let name = app
                .edit
                .last_bin
                .as_ref()
                .and_then(|p| p.file_name())
                .map(|s| s.to_string_lossy().into_owned())
                .unwrap_or_default();
            app.workers.stop_run(name);
        }

        ui.separator();
        ui.checkbox(&mut app.f.edit_mpp_libs, "MPP libs");
        ui.add(
            egui::TextEdit::singleline(&mut app.f.edit_extra_args)
                .hint_text("-ldl -lpthread …")
                .desired_width(220.0),
        );

        // Dirty / busy indicator.
        ui.separator();
        if is_dirty(&app.edit) {
            ui.colored_label(theme::PEACH, "●");
        }
        if let Some(p) = &app.edit.open_path {
            let name = p
                .file_name()
                .map(|s| s.to_string_lossy().into_owned())
                .unwrap_or_default();
            let star = if is_dirty(&app.edit) { "*" } else { "" };
            ui.label(format!("{name}{star}"));
        }
        if app.edit.building {
            ui.colored_label(theme::YELLOW, "building…");
        }
        if app.edit.running {
            ui.colored_label(theme::GREEN, "running");
        }
    });
}

/// Write the buffer back to the open file and refresh the dirty snapshot.
fn save_buffer(app: &mut KbdkApp) {
    let Some(path) = app.edit.open_path.clone() else {
        return;
    };
    match std::fs::write(&path, &app.edit.buffer) {
        Ok(()) => {
            app.edit.saved_snapshot = app.edit.buffer.clone();
            app.edit.status = format!("saved {}", path.display());
        }
        Err(e) => app.edit.status = format!("save failed: {e}"),
    }
}

/// Auto-save, then kick off a cross-compile of the open file into `<repo>/bin`.
fn start_build(app: &mut KbdkApp) {
    let Some(src) = app.edit.open_path.clone() else {
        return;
    };
    save_buffer(app);
    let out_dir = app.workers.repo_root.join("bin");
    if let Err(e) = std::fs::create_dir_all(&out_dir) {
        app.edit.status = format!("mkdir bin failed: {e}");
        return;
    }
    let extra_args: Vec<String> = app
        .f
        .edit_extra_args
        .split_whitespace()
        .map(str::to_string)
        .collect();
    app.edit.building = true;
    app.edit.last_bin = None;
    // Snapshot what we're compiling (buffer == saved after the auto-save above);
    // Deploy + Run is gated on this still matching the buffer at run time.
    app.edit.built_from = Some(app.edit.buffer.clone());
    app.edit.output.clear();
    app.edit.status = format!("building {}…", src.display());
    app.workers.build(src, out_dir, extra_args);
}

/// Push the freshly built binary to /tmp and run it on the board, streaming stdout.
fn start_run(app: &mut KbdkApp) {
    let Some(bin) = app.edit.last_bin.clone() else {
        return;
    };
    let name = bin
        .file_name()
        .map(|s| s.to_string_lossy().into_owned())
        .unwrap_or_else(|| "prog".into());
    let remote = format!("/tmp/{name}");
    let env_prefix = if app.f.edit_mpp_libs {
        "LD_LIBRARY_PATH=/usr/lib/eyesee-mpp:/usr/lib".to_string()
    } else {
        String::new()
    };
    app.edit.running = true;
    app.edit.status = format!("deploying {remote}…");
    app.workers.deploy_run(bin, remote, env_prefix);
}

/// Render the local subtree rooted at `path`. Dirs lazily fill their children via
/// `files_tab::read_local_dir`; clicking a file opens it in the editor.
fn edit_node(ui: &mut egui::Ui, app: &mut KbdkApp, path: &str) {
    if !app.edit.tree.children.contains_key(path) {
        let entries = files_tab::read_local_dir(path);
        app.edit.tree.children.insert(path.to_string(), entries);
    }
    let entries = app.edit.tree.children.get(path).cloned().unwrap_or_default();
    for e in entries {
        let full = std::path::Path::new(path)
            .join(&e.name)
            .to_string_lossy()
            .into_owned();
        if e.is_dir {
            let open = app.edit.tree.expanded.contains(&full);
            let arrow = if open { "▾" } else { "▸" };
            let resp = ui.selectable_label(false, format!("{arrow} 📁 {}", e.name));
            if resp.clicked() {
                if open {
                    app.edit.tree.expanded.remove(&full);
                } else {
                    app.edit.tree.expanded.insert(full.clone());
                }
            }
            if app.edit.tree.expanded.contains(&full) {
                ui.indent(&full, |ui| edit_node(ui, app, &full));
            }
        } else {
            let selected = app.edit.open_path.as_deref()
                == Some(std::path::Path::new(&full));
            let resp = ui.selectable_label(selected, format!("   📄 {}", e.name));
            if resp.clicked() {
                open_file(app, std::path::PathBuf::from(&full));
            }
        }
    }
}
