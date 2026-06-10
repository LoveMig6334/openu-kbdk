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

/// per-class overlay palette (matches the board's DET_COL)
const BOX_COLORS: [egui::Color32; 6] = [
    theme::GREEN,
    theme::RED,
    theme::BLUE,
    theme::YELLOW,
    theme::MAUVE,
    theme::TEAL,
];

/// Paint detection rects (normalized coords from the result JSON) over the
/// camera preview. The preview and the net input are the same centre square,
/// so the coords map 1:1.
fn draw_boxes_overlay(ui: &egui::Ui, rect: egui::Rect, result: &Option<serde_json::Value>) {
    let Some(r) = result else { return };
    let Some(boxes) = r["boxes"].as_array() else { return };
    let painter = ui.painter_at(rect);
    for b in boxes {
        let (x, y, w, h) = (
            b["x"].as_f64().unwrap_or(0.0) as f32,
            b["y"].as_f64().unwrap_or(0.0) as f32,
            b["w"].as_f64().unwrap_or(0.0) as f32,
            b["h"].as_f64().unwrap_or(0.0) as f32,
        );
        let col = BOX_COLORS[(b["index"].as_u64().unwrap_or(0) as usize) % 6];
        let bo = egui::Rect::from_min_size(
            rect.min + egui::vec2(x * rect.width(), y * rect.height()),
            egui::vec2(w * rect.width(), h * rect.height()),
        );
        painter.rect_stroke(bo, 2.0, egui::Stroke::new(2.0, col), egui::StrokeKind::Outside);
        painter.text(
            bo.min + egui::vec2(3.0, 3.0),
            egui::Align2::LEFT_TOP,
            format!(
                "{} {:.0}%",
                b["label"].as_str().unwrap_or("?"),
                b["conf"].as_f64().unwrap_or(0.0) * 100.0
            ),
            egui::FontId::proportional(12.0),
            col,
        );
    }
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

    let show_feed = app.last_result.is_some() || app.cam_tex.is_some();
    if show_feed {
        ui.add_space(8.0);
        ui.horizontal_top(|ui| {
            // live camera preview (pulled from the board's /tmp/kbrun_frame.rgb)
            // + dataset capture (saves frames in ImageFolder layout for training)
            if app.cam_tex.is_some() {
                let mut capture_clicked = false;
                egui::Frame::default()
                    .fill(theme::CRUST)
                    .corner_radius(8.0)
                    .inner_margin(8.0)
                    .show(ui, |ui| {
                        ui.vertical(|ui| {
                            if let Some(tex) = &app.cam_tex {
                                let resp = ui.add(
                                    egui::Image::new(tex)
                                        .fit_to_exact_size(egui::vec2(280.0, 280.0))
                                        .corner_radius(4.0),
                                );
                                draw_boxes_overlay(ui, resp.rect, &app.last_result);
                            }
                            // fps over the recent frame-arrival window (TCP feed
                            // ~10-15, adb-pull fallback ~2.5)
                            let fps = match (app.frame_times.front(), app.frame_times.back()) {
                                (Some(a), Some(b)) if app.frame_times.len() > 1 => {
                                    let dt = b.duration_since(*a).as_secs_f32();
                                    if dt > 0.0 { (app.frame_times.len() - 1) as f32 / dt } else { 0.0 }
                                }
                                _ => 0.0,
                            };
                            ui.colored_label(
                                theme::SUBTEXT,
                                format!("board camera (AWB preview) — {fps:.1} fps"),
                            );
                            ui.add_space(6.0);
                            ui.horizontal(|ui| {
                                ui.label(egui::RichText::new("dataset").color(theme::SUBTEXT));
                                ui.add(
                                    egui::TextEdit::singleline(&mut app.f.capture_dir)
                                        .desired_width(130.0),
                                );
                                ui.label(egui::RichText::new("class").color(theme::SUBTEXT));
                                ui.add(
                                    egui::TextEdit::singleline(&mut app.f.capture_class)
                                        .desired_width(80.0),
                                );
                            });
                            ui.horizontal(|ui| {
                                if ui
                                    .add(
                                        egui::Button::new(
                                            egui::RichText::new("📷 Capture").color(theme::CRUST),
                                        )
                                        .fill(theme::YELLOW),
                                    )
                                    .clicked()
                                {
                                    capture_clicked = true;
                                }
                                ui.toggle_value(&mut app.burst, "burst (every frame)");
                            });
                            if !app.capture_note.is_empty() {
                                let col = if app.capture_note.contains("failed") {
                                    theme::RED
                                } else {
                                    theme::SUBTEXT
                                };
                                ui.colored_label(col, &app.capture_note);
                            }
                        });
                    });
                if capture_clicked {
                    app.save_capture();
                }
            }
            if let Some(r) = &app.last_result {
                egui::Frame::default()
                    .fill(theme::CRUST)
                    .corner_radius(8.0)
                    .inner_margin(14.0)
                    .show(ui, |ui| {
                        ui.set_max_width(420.0);
                        ui.vertical(|ui| {
                        // detection result: object count + per-box list
                        if let Some(boxes) = r["boxes"].as_array() {
                            ui.horizontal(|ui| {
                                ui.label(
                                    egui::RichText::new(if boxes.is_empty() {
                                        "no objects".into()
                                    } else {
                                        format!("{} object{}", boxes.len(), if boxes.len() == 1 { "" } else { "s" })
                                    })
                                    .color(if boxes.is_empty() { theme::SUBTEXT } else { theme::GREEN })
                                    .size(28.0)
                                    .strong(),
                                );
                                ui.label(
                                    egui::RichText::new(format!("{} ms", r["ms"].as_f64().unwrap_or(0.0)))
                                        .color(theme::SUBTEXT),
                                );
                            });
                            ui.add_space(4.0);
                            for b in boxes {
                                let col = BOX_COLORS[(b["index"].as_u64().unwrap_or(0) as usize) % 6];
                                ui.horizontal(|ui| {
                                    ui.colored_label(col, "■");
                                    ui.colored_label(col, b["label"].as_str().unwrap_or("?"));
                                    ui.label(
                                        egui::RichText::new(format!(
                                            "{:.0}%",
                                            b["conf"].as_f64().unwrap_or(0.0) * 100.0
                                        ))
                                        .color(theme::TEXT),
                                    );
                                });
                            }
                            return;
                        }
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
                                    egui::RichText::new(format!(
                                        "{} ms",
                                        r["ms"].as_f64().unwrap_or(0.0)
                                    ))
                                    .color(theme::SUBTEXT),
                                );
                            });
                            ui.add(
                                egui::ProgressBar::new(conf as f32)
                                    .fill(theme::GREEN)
                                    .desired_height(6.0),
                            );
                        }
                        ui.add_space(6.0);
                        for t in top.iter().skip(1).take(4) {
                            ui.horizontal(|ui| {
                                ui.label(
                                    egui::RichText::new(t["label"].as_str().unwrap_or("?"))
                                        .color(theme::SUBTEXT),
                                );
                                ui.label(
                                    egui::RichText::new(format!(
                                        "{:.1}%",
                                        t["conf"].as_f64().unwrap_or(0.0) * 100.0
                                    ))
                                    .color(theme::OVERLAY0),
                                );
                            });
                        }
                        });
                    });
            }
        });
    }
    if app.running && !show_feed {
        ui.add_space(8.0);
        ui.horizontal(|ui| {
            ui.spinner();
            ui.colored_label(theme::SUBTEXT, "waiting for the first classification…");
        });
    }
}
