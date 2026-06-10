//! Deploy & Run tab: pack list → md5-verified push → start/stop the runner →
//! live result feed read back from the board.

use crate::app::KbdkApp;
use crate::theme;
use eframe::egui;
use std::path::{Path, PathBuf};

pub struct PackInfo {
    pub dir: PathBuf,
    pub name: String,
    pub backbone: String,
    pub input: String,
    pub quant: String,
    pub runtime: String,
    pub builtin: bool, // board-models/: stock vendor model, files live on the board
    pub n_labels: usize,
}

fn scan_dir(packs_dir: &Path, builtin: bool, v: &mut Vec<PackInfo>) {
    let Ok(rd) = std::fs::read_dir(packs_dir) else {
        return;
    };
    for e in rd.filter_map(|e| e.ok()) {
        let dir = e.path();
        if let Ok(m) = kbdk_core::pack::Manifest::load(&dir) {
            let n_labels = if m.labels.is_empty() {
                std::fs::read_to_string(dir.join(&m.files.labels_file))
                    .map(|s| s.lines().count())
                    .unwrap_or(0)
            } else {
                m.labels.len()
            };
            v.push(PackInfo {
                dir,
                name: m.name,
                backbone: m.backbone,
                input: format!("{}x{}", m.input.width, m.input.height),
                quant: m.quant,
                runtime: m.runtime,
                builtin,
                n_labels,
            });
        }
    }
}

/// Your converted packs (packs/) + the stock board models (board-models/).
pub fn scan_packs(packs_dir: &Path) -> Vec<PackInfo> {
    let mut v = vec![];
    scan_dir(packs_dir, false, &mut v);
    if let Some(repo) = packs_dir.parent() {
        scan_dir(&repo.join("board-models"), true, &mut v);
    }
    v.sort_by(|a, b| (a.builtin, &a.name).cmp(&(b.builtin, &b.name)));
    v
}

pub fn show(app: &mut KbdkApp, ui: &mut egui::Ui) {
    ui.heading("Deploy & run on the board");
    ui.add_space(6.0);

    let board_ok = !app.adb_devices.is_empty();
    if !board_ok {
        ui.colored_label(theme::YELLOW, "no board on adb — connect the USB-OTG cable");
    }

    // ---- pack table ----
    ui.horizontal(|ui| {
        ui.label(egui::RichText::new("Packs").strong());
        ui.label(egui::RichText::new(format!("({}/)", app.f.packs_dir)).color(theme::SUBTEXT));
        if ui.small_button("⟳ rescan").clicked() {
            app.rescan_packs();
        }
    });

    let mut deploy_clicked: Option<PathBuf> = None;
    let mut run_clicked: Option<String> = None;
    egui::Frame::default()
        .fill(theme::MANTLE)
        .corner_radius(6.0)
        .inner_margin(8.0)
        .show(ui, |ui| {
            egui::Grid::new("packs").num_columns(7).spacing([16.0, 6.0]).striped(true).show(ui, |ui| {
                ui.label(egui::RichText::new("name").color(theme::SUBTEXT));
                ui.label(egui::RichText::new("backbone").color(theme::SUBTEXT));
                ui.label(egui::RichText::new("input").color(theme::SUBTEXT));
                ui.label(egui::RichText::new("runtime").color(theme::SUBTEXT));
                ui.label(egui::RichText::new("quant").color(theme::SUBTEXT));
                ui.label(egui::RichText::new("classes").color(theme::SUBTEXT));
                ui.label("");
                ui.end_row();
                for p in &app.packs {
                    ui.horizontal(|ui| {
                        ui.label(egui::RichText::new(&p.name).color(theme::TEXT).strong());
                        if p.builtin {
                            ui.label(
                                egui::RichText::new("board built-in")
                                    .color(theme::MAUVE)
                                    .small(),
                            );
                        }
                    });
                    ui.label(&p.backbone);
                    ui.label(&p.input);
                    ui.colored_label(
                        if p.runtime == "awnn" { theme::MAUVE } else { theme::BLUE },
                        &p.runtime,
                    );
                    ui.label(&p.quant);
                    ui.label(p.n_labels.to_string());
                    ui.horizontal(|ui| {
                        if ui.add_enabled(board_ok && !app.deploying, egui::Button::new("⤒ Deploy")).clicked() {
                            deploy_clicked = Some(p.dir.clone());
                        }
                        let deployed = app
                            .deployed
                            .as_deref()
                            .is_some_and(|d| d.ends_with(&format!("/{}", p.name)));
                        if deployed
                            && ui
                                .add_enabled(
                                    board_ok && !app.running,
                                    egui::Button::new(egui::RichText::new("▶ Run").color(theme::CRUST)).fill(theme::GREEN),
                                )
                                .clicked()
                        {
                            run_clicked = Some(p.name.clone());
                        }
                    });
                    ui.end_row();
                }
            });
            if app.packs.is_empty() {
                ui.colored_label(theme::SUBTEXT, "no packs yet — build one in the Convert tab");
            }
        });

    if let Some(dir) = deploy_clicked {
        app.deploying = true;
        app.deploy_status.clear();
        app.deployed = None;
        app.workers.deploy(dir);
    }
    if let Some(name) = run_clicked {
        app.last_result = None;
        app.workers.run_pack(name, app.f.res.clone());
    }

    // ---- deploy progress ----
    if app.deploying || !app.deploy_status.is_empty() {
        ui.add_space(6.0);
        ui.horizontal(|ui| {
            if app.deploying {
                ui.spinner();
            }
            for s in app.deploy_status.iter().rev().take(1) {
                let col = if s.starts_with("failed") { theme::RED } else { theme::SUBTEXT };
                ui.colored_label(col, s);
            }
        });
    }

    // ---- live results ----
    ui.add_space(12.0);
    ui.horizontal(|ui| {
        ui.label(egui::RichText::new("Board").strong());
        if app.running {
            ui.colored_label(theme::GREEN, "● running");
            if ui.button("■ Stop").clicked() {
                app.workers.stop_pack();
            }
        } else {
            ui.colored_label(theme::SUBTEXT, "○ idle");
        }
        if !app.board_note.is_empty() {
            ui.label(egui::RichText::new(&app.board_note).color(theme::SUBTEXT));
        }
    });

    if let Some(r) = &app.last_result {
        ui.add_space(8.0);
        egui::Frame::default()
            .fill(theme::CRUST)
            .corner_radius(8.0)
            .inner_margin(14.0)
            .show(ui, |ui| {
                let top = r["top"].as_array().cloned().unwrap_or_default();
                if let Some(best) = top.first() {
                    let label = best["label"].as_str().unwrap_or("?");
                    let conf = best["conf"].as_f64().unwrap_or(0.0);
                    ui.horizontal(|ui| {
                        ui.label(
                            egui::RichText::new(label)
                                .color(theme::GREEN)
                                .size(34.0)
                                .strong(),
                        );
                        ui.label(
                            egui::RichText::new(format!("{:.0}%", conf * 100.0))
                                .color(theme::TEXT)
                                .size(26.0),
                        );
                        ui.label(
                            egui::RichText::new(format!("{} ms", r["ms"].as_f64().unwrap_or(0.0)))
                                .color(theme::SUBTEXT),
                        );
                    });
                    ui.add(egui::ProgressBar::new(conf as f32).fill(theme::GREEN).desired_height(6.0));
                }
                ui.add_space(6.0);
                for t in top.iter().skip(1).take(4) {
                    ui.horizontal(|ui| {
                        ui.label(
                            egui::RichText::new(t["label"].as_str().unwrap_or("?")).color(theme::SUBTEXT),
                        );
                        ui.label(
                            egui::RichText::new(format!("{:.1}%", t["conf"].as_f64().unwrap_or(0.0) * 100.0))
                                .color(theme::OVERLAY0),
                        );
                    });
                }
            });
    } else if app.running {
        ui.add_space(8.0);
        ui.horizontal(|ui| {
            ui.spinner();
            ui.colored_label(theme::SUBTEXT, "waiting for the first classification…");
        });
    }
}
