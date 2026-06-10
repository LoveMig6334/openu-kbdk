//! Convert tab: TorchScript → pnnx → ncnn → int8 → pack, with step progress.

use crate::app::KbdkApp;
use crate::theme;
use eframe::egui;
use kbdk_core::pipeline::PyEvent;

pub fn on_event(app: &mut KbdkApp, e: PyEvent) {
    let PyEvent::Line(v) = e else { return };
    match v["event"].as_str().unwrap_or("") {
        "step" => {
            let name = v["name"].as_str().unwrap_or("?");
            if name == "parity" {
                app.parity = v["top1_agreement"].as_f64();
                app.convert_steps
                    .push(format!("parity: int8 top-1 agreement {:.0}%", app.parity.unwrap_or(0.0) * 100.0));
            } else {
                app.convert_steps.push(format!("{name} {}", kbdk_core::pipeline::summarize(&v)));
            }
        }
        "done" => {
            let pack = v["pack"].as_str().unwrap_or("").to_string();
            app.convert_steps.push(format!("pack ready: {pack}"));
            app.built_pack = Some(pack);
        }
        "error" => {
            app.convert_steps
                .push(format!("error: {}", v["msg"].as_str().unwrap_or("?")));
        }
        _ => {}
    }
}

/// Kick off conversion with the current fields (the Convert button's action;
/// also driven by the KBDK_AUTOCONVERT test hook).
pub fn start(app: &mut KbdkApp, model: String) {
    app.converting = true;
    app.convert_steps.clear();
    app.parity = None;
    let root = &app.workers.repo_root;
    app.workers.convert(vec![
        "--model".into(),
        model,
        "--data".into(),
        root.join(&app.f.data_dir).display().to_string(),
        "--name".into(),
        app.f.pack_name.clone(),
        "--out".into(),
        root.join(&app.f.packs_dir).display().to_string(),
        "--width".into(),
        app.f.size.to_string(),
        "--height".into(),
        app.f.size.to_string(),
        "--backbone".into(),
        app.f.backbone.clone(),
    ]);
}

pub fn default_model(app: &KbdkApp) -> String {
    app.saved_model.clone().unwrap_or_else(|| {
        app.workers.repo_root.join(&app.f.model_out).display().to_string()
    })
}

pub fn show(app: &mut KbdkApp, ui: &mut egui::Ui) {
    ui.heading("Convert to a board pack (int8 ncnn)");
    ui.add_space(6.0);

    let mut model = default_model(app);

    let label_w = 190.0;
    ui.horizontal(|ui| {
        ui.add_sized([label_w, 18.0], egui::Label::new("TorchScript model"));
        ui.add_enabled(false, egui::TextEdit::singleline(&mut model).desired_width(420.0));
        if ui.button("Browse…").clicked() {
            if let Some(f) = rfd::FileDialog::new().add_filter("TorchScript", &["pt"]).pick_file() {
                app.saved_model = Some(f.display().to_string());
            }
        }
    });
    ui.horizontal(|ui| {
        ui.add_sized([label_w, 18.0], egui::Label::new("Dataset (labels + calibration)"));
        ui.add(egui::TextEdit::singleline(&mut app.f.data_dir).desired_width(420.0));
    });
    ui.horizontal(|ui| {
        ui.add_sized([label_w, 18.0], egui::Label::new("Pack name"));
        ui.add(egui::TextEdit::singleline(&mut app.f.pack_name).desired_width(200.0));
        ui.label(egui::RichText::new(format!("→ {}/{}", app.f.packs_dir, app.f.pack_name)).color(theme::SUBTEXT));
    });

    ui.add_space(8.0);
    ui.horizontal(|ui| {
        if app.converting {
            ui.spinner();
            ui.label("converting…");
        } else if ui
            .add(egui::Button::new(egui::RichText::new("▶ Convert").color(theme::CRUST)).fill(theme::BLUE))
            .clicked()
        {
            let m = model.clone();
            start(app, m);
        }
        if let Some(p) = app.parity {
            let col = if p >= 0.8 { theme::GREEN } else { theme::RED };
            ui.colored_label(col, format!("int8 parity {:.0}%", p * 100.0));
        }
    });

    ui.add_space(10.0);
    egui::Frame::default()
        .fill(theme::CRUST)
        .corner_radius(6.0)
        .inner_margin(10.0)
        .show(ui, |ui| {
            ui.set_min_height(180.0);
            ui.set_min_width(ui.available_width());
            egui::ScrollArea::vertical().stick_to_bottom(true).show(ui, |ui| {
                for s in &app.convert_steps {
                    let col = if s.starts_with("error") || s.starts_with("failed") {
                        theme::RED
                    } else {
                        theme::SUBTEXT
                    };
                    ui.colored_label(col, s);
                }
            });
        });

    if let Some(p) = app.built_pack.clone() {
        ui.add_space(6.0);
        ui.horizontal(|ui| {
            ui.colored_label(theme::GREEN, format!("pack ready: {p}"));
            if ui.button("→ Deploy this pack").clicked() {
                app.rescan_packs();
                app.f.tab = crate::app::Tab::Deploy;
            }
        });
    }
}
