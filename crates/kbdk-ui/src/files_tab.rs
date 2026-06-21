//! Files tab: dual-pane local <-> board file transfer + management.

use crate::app::KbdkApp;
use crate::theme;
use eframe::egui;
use kbdk_core::fs::DirEntry;
use std::collections::{BTreeSet, HashMap};

/// Modal state for the rm/chmod/mkdir actions.
pub enum FsDialog {
    ConfirmRm { path: String, is_dir: bool, parent: String },
    Chmod { path: String, parent: String, mode: String },
    Mkdir { parent: String, name: String },
}

/// One side of the dual pane. `children` is a lazy cache: a dir's entries are
/// fetched (board) or read (local) the first time it's expanded.
pub struct FileTree {
    pub root: String,
    pub expanded: BTreeSet<String>,
    pub children: HashMap<String, Vec<DirEntry>>,
    pub selected: Option<String>, // full path of the selected file
}

impl FileTree {
    fn new(root: String) -> Self {
        Self {
            root,
            expanded: BTreeSet::new(),
            children: HashMap::new(),
            selected: None,
        }
    }
}

pub struct FilesState {
    pub local: FileTree,
    pub board: FileTree,
    pub status: String,
    pub dialog: Option<FsDialog>,
    pub preview: Option<(String, String, bool)>, // (path, body, is_binary)
}

impl FilesState {
    pub fn new(local_root: String, board_root: String) -> Self {
        Self {
            local: FileTree::new(local_root),
            board: FileTree::new(board_root),
            status: String::new(),
            dialog: None,
            preview: None,
        }
    }
}

/// Read a host directory into DirEntry rows (mode left blank — unused locally).
pub fn read_local_dir(path: &str) -> Vec<DirEntry> {
    let mut v = Vec::new();
    if let Ok(rd) = std::fs::read_dir(path) {
        for e in rd.filter_map(|e| e.ok()) {
            let md = e.metadata().ok();
            let is_dir = md.as_ref().map(|m| m.is_dir()).unwrap_or(false);
            v.push(DirEntry {
                name: e.file_name().to_string_lossy().into_owned(),
                is_dir,
                size: md.as_ref().map(|m| m.len()).unwrap_or(0),
                mode: String::new(),
            });
        }
    }
    v.sort_by(|a, b| (b.is_dir, &a.name).cmp(&(a.is_dir, &b.name)));
    v
}

/// Join for the LOCAL side (host path separators).
fn local_join(parent: &str, name: &str) -> String {
    std::path::Path::new(parent).join(name).to_string_lossy().into_owned()
}

pub fn show(app: &mut KbdkApp, ui: &mut egui::Ui) {
    ui.horizontal(|ui| {
        ui.heading("Files");
        ui.label(egui::RichText::new(&app.files.status).color(theme::SUBTEXT));
    });
    ui.separator();

    let avail_h = ui.available_height() - 8.0;
    ui.horizontal_top(|ui| {
        let pane_w = (ui.available_width() - 80.0) / 2.0;

        // LEFT: local
        ui.allocate_ui(egui::vec2(pane_w, avail_h), |ui| {
            ui.group(|ui| {
                ui.label(egui::RichText::new(format!("LOCAL  {}", app.files.local.root)).strong());
                egui::ScrollArea::vertical()
                    .id_salt("local_tree")
                    .auto_shrink([false, false])
                    .show(ui, |ui| {
                        let root = app.files.local.root.clone();
                        local_node(ui, app, &root);
                    });
            });
        });

        // CENTER: transfer + board-dir actions
        ui.vertical(|ui| {
            ui.add_space(40.0);
            let local_sel = app.files.local.selected.clone();
            let board_dir = current_board_dir(app);
            if ui.add_enabled(local_sel.is_some(), egui::Button::new("push →")).clicked() {
                if let Some(p) = local_sel {
                    app.workers.push_file(std::path::PathBuf::from(p), board_dir.clone());
                }
            }
            let board_sel = app.files.board.selected.clone();
            let local_dir = current_local_dir(app);
            if ui.add_enabled(board_sel.is_some(), egui::Button::new("← pull")).clicked() {
                if let Some(p) = board_sel {
                    app.workers.pull_file(p, std::path::PathBuf::from(local_dir));
                }
            }
            ui.separator();
            if ui.button("＋ mkdir").clicked() {
                app.files.dialog = Some(FsDialog::Mkdir { parent: board_dir.clone(), name: String::new() });
            }
        });

        // RIGHT: board
        ui.allocate_ui(egui::vec2(pane_w, avail_h), |ui| {
            ui.group(|ui| {
                ui.horizontal(|ui| {
                    ui.label(egui::RichText::new(format!("BOARD  {}", app.files.board.root)).strong());
                    if ui.small_button("⟳").clicked() {
                        let root = app.files.board.root.clone();
                        app.files.board.children.remove(&root);
                        app.workers.list_dir(root);
                    }
                });
                egui::ScrollArea::vertical()
                    .id_salt("board_tree")
                    .auto_shrink([false, false])
                    .show(ui, |ui| {
                        let root = app.files.board.root.clone();
                        // fetch root once
                        if !app.files.board.children.contains_key(&root) {
                            app.workers.list_dir(root.clone());
                            app.files.board.children.insert(root.clone(), vec![]);
                        }
                        board_node(ui, app, &root);
                    });
            });
        });
    });

    if let Some((path, body, is_binary)) = app.files.preview.clone() {
        ui.separator();
        ui.horizontal(|ui| {
            ui.label(egui::RichText::new(format!("preview: {path}")).strong());
            ui.label(egui::RichText::new(if is_binary { "binary (hex)" } else { "text" }).color(theme::SUBTEXT));
            if ui.small_button("✕").clicked() {
                app.files.preview = None;
            }
        });
        egui::ScrollArea::vertical().id_salt("preview").max_height(180.0).show(ui, |ui| {
            ui.add(egui::Label::new(egui::RichText::new(&body).monospace()).wrap());
        });
    }

    render_dialog(app, ui);
}

/// Render the LOCAL subtree rooted at `path` (read synchronously on expand).
fn local_node(ui: &mut egui::Ui, app: &mut KbdkApp, path: &str) {
    if !app.files.local.children.contains_key(path) {
        let entries = read_local_dir(path);
        app.files.local.children.insert(path.to_string(), entries);
    }
    let entries = app.files.local.children.get(path).cloned().unwrap_or_default();
    for e in entries {
        let child = local_join(path, &e.name);
        row(ui, app, true, &child, &e);
    }
}

/// Render the BOARD subtree rooted at `path` (children arrive via DirListed).
fn board_node(ui: &mut egui::Ui, app: &mut KbdkApp, path: &str) {
    let entries = app.files.board.children.get(path).cloned().unwrap_or_default();
    for e in entries {
        let child = kbdk_core::fs::join_path(path, &e.name);
        row(ui, app, false, &child, &e);
    }
}

/// One row: a clickable disclosure for dirs, a selectable label for files.
fn row(ui: &mut egui::Ui, app: &mut KbdkApp, is_local: bool, full: &str, e: &DirEntry) {
    let tree = if is_local { &mut app.files.local } else { &mut app.files.board };
    if e.is_dir {
        let open = tree.expanded.contains(full);
        let arrow = if open { "▾" } else { "▸" };
        if ui.selectable_label(false, format!("{arrow} 📁 {}", e.name)).clicked() {
            if open {
                tree.expanded.remove(full);
            } else {
                tree.expanded.insert(full.to_string());
                // board dirs need a fetch the first time
                if !is_local && !app.files.board.children.contains_key(full) {
                    app.files.board.children.insert(full.to_string(), vec![]);
                    app.workers.list_dir(full.to_string());
                }
            }
        }
        // recurse if expanded (re-borrow because the closure above took &mut)
        let open_now = if is_local {
            app.files.local.expanded.contains(full)
        } else {
            app.files.board.expanded.contains(full)
        };
        if open_now {
            ui.indent(full, |ui| {
                if is_local {
                    local_node(ui, app, full);
                } else {
                    board_node(ui, app, full);
                }
            });
        }
    } else {
        let selected = if is_local {
            app.files.local.selected.as_deref() == Some(full)
        } else {
            app.files.board.selected.as_deref() == Some(full)
        };
        let label = format!("   📄 {}  {}", e.name, human_size(e.size));
        let resp = ui.selectable_label(selected, label);
        if resp.clicked() {
            if is_local {
                app.files.local.selected = Some(full.to_string());
            } else {
                app.files.board.selected = Some(full.to_string());
                app.workers.preview_file(full.to_string());
            }
        }
        if !is_local {
            let parent = full.rsplit_once('/').map(|(d, _)| if d.is_empty() { "/".to_string() } else { d.to_string() }).unwrap_or_else(|| "/".to_string());
            resp.context_menu(|ui| {
                if ui.button("pull").clicked() {
                    let dir = current_local_dir(app);
                    app.workers.pull_file(full.to_string(), std::path::PathBuf::from(dir));
                    ui.close();
                }
                if ui.button("chmod…").clicked() {
                    app.files.dialog = Some(FsDialog::Chmod { path: full.to_string(), parent: parent.clone(), mode: "755".into() });
                    ui.close();
                }
                if ui.button("rm").clicked() {
                    app.files.dialog = Some(FsDialog::ConfirmRm { path: full.to_string(), is_dir: false, parent: parent.clone() });
                    ui.close();
                }
            });
        }
    }
}

fn human_size(n: u64) -> String {
    if n >= 1 << 20 {
        format!("{:.1}M", n as f64 / (1 << 20) as f64)
    } else if n >= 1 << 10 {
        format!("{:.1}K", n as f64 / (1 << 10) as f64)
    } else {
        format!("{n}B")
    }
}

/// Board directory to act on: the selected board file's parent, else board root.
fn current_board_dir(app: &KbdkApp) -> String {
    match &app.files.board.selected {
        Some(p) => p.rsplit_once('/').map(|(d, _)| if d.is_empty() { "/".into() } else { d.to_string() })
            .unwrap_or_else(|| app.files.board.root.clone()),
        None => app.files.board.root.clone(),
    }
}

/// Local directory to pull into: the selected local file's parent, else root.
fn current_local_dir(app: &KbdkApp) -> String {
    match &app.files.local.selected {
        Some(p) => std::path::Path::new(p).parent()
            .map(|d| d.to_string_lossy().into_owned())
            .unwrap_or_else(|| app.files.local.root.clone()),
        None => app.files.local.root.clone(),
    }
}

fn render_dialog(app: &mut KbdkApp, ui: &mut egui::Ui) {
    let Some(dialog) = app.files.dialog.take() else { return };
    let mut keep: Option<FsDialog> = None;
    match dialog {
        FsDialog::ConfirmRm { path, is_dir, parent } => {
            egui::Window::new("Delete?").collapsible(false).resizable(false).show(ui.ctx(), |ui| {
                ui.label(format!("Remove {path} from the board?"));
                ui.horizontal(|ui| {
                    if ui.button(egui::RichText::new("Delete").color(theme::RED)).clicked() {
                        app.workers.fs_remove(path.clone(), is_dir, parent.clone());
                    } else if ui.button("Cancel").clicked() {
                        // drop
                    } else {
                        keep = Some(FsDialog::ConfirmRm { path: path.clone(), is_dir, parent: parent.clone() });
                    }
                });
            });
        }
        FsDialog::Chmod { path, parent, mut mode } => {
            egui::Window::new("chmod").collapsible(false).resizable(false).show(ui.ctx(), |ui| {
                ui.horizontal(|ui| {
                    ui.label("mode");
                    ui.text_edit_singleline(&mut mode);
                });
                ui.horizontal(|ui| {
                    if ui.button("Apply").clicked() {
                        app.workers.fs_chmod(path.clone(), mode.clone(), parent.clone());
                    } else if ui.button("Cancel").clicked() {
                        // drop
                    } else {
                        keep = Some(FsDialog::Chmod { path: path.clone(), parent: parent.clone(), mode: mode.clone() });
                    }
                });
            });
        }
        FsDialog::Mkdir { parent, mut name } => {
            egui::Window::new("New folder").collapsible(false).resizable(false).show(ui.ctx(), |ui| {
                ui.horizontal(|ui| {
                    ui.label("name");
                    ui.text_edit_singleline(&mut name);
                });
                ui.horizontal(|ui| {
                    if ui.button("Create").clicked() && !name.trim().is_empty() {
                        let path = kbdk_core::fs::join_path(&parent, name.trim());
                        app.workers.fs_mkdir(path, parent.clone());
                    } else if ui.button("Cancel").clicked() {
                        // drop
                    } else {
                        keep = Some(FsDialog::Mkdir { parent: parent.clone(), name: name.clone() });
                    }
                });
            });
        }
    }
    app.files.dialog = keep;
}
