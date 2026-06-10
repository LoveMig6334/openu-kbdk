//! Train tab: dataset picker → kbdk-train (MPS) with live loss/val_acc curves.

use crate::app::KbdkApp;
use crate::theme;
use eframe::egui;
use egui_plot::{Line, Plot, PlotPoints};
use kbdk_core::pipeline::PyEvent;

pub fn on_event(app: &mut KbdkApp, e: PyEvent) {
    let PyEvent::Line(v) = e else { return };
    match v["event"].as_str().unwrap_or("") {
        "start" => {
            app.train_classes = v["classes"]
                .as_array()
                .map(|a| a.iter().filter_map(|x| x.as_str().map(String::from)).collect())
                .unwrap_or_default();
            app.train_status = format!(
                "training on {} — {} train / {} val, classes: {}",
                v["device"].as_str().unwrap_or("?"),
                v["n_train"],
                v["n_val"],
                app.train_classes.join(", ")
            );
        }
        "epoch" => {
            let n = v["n"].as_f64().unwrap_or(0.0);
            if let Some(loss) = v["loss"].as_f64() {
                app.epoch_loss.push([n, loss]);
            }
            if let Some(acc) = v["val_acc"].as_f64() {
                app.epoch_acc.push([n, acc]);
            }
        }
        "saved" => {
            let path = v["path"].as_str().unwrap_or("").to_string();
            app.train_status = format!("saved {path}");
            app.saved_model = Some(path);
        }
        "error" => {
            app.train_status = format!("error: {}", v["msg"].as_str().unwrap_or("?"));
        }
        _ => {}
    }
}

fn dataset_summary(dir: &std::path::Path) -> Option<String> {
    let classes: Vec<String> = std::fs::read_dir(dir)
        .ok()?
        .filter_map(|e| e.ok())
        .filter(|e| e.path().is_dir())
        .map(|e| e.file_name().to_string_lossy().into_owned())
        .collect();
    if classes.is_empty() {
        return None;
    }
    let n: usize = classes
        .iter()
        .filter_map(|c| std::fs::read_dir(dir.join(c)).ok().map(|d| d.count()))
        .sum();
    Some(format!("{} classes, {n} images", classes.len()))
}

pub fn show(app: &mut KbdkApp, ui: &mut egui::Ui) {
    ui.heading("Fine-tune a classifier");
    ui.add_space(6.0);

    egui::Grid::new("train_grid").num_columns(3).spacing([10.0, 8.0]).show(ui, |ui| {
        ui.label("Dataset (ImageFolder)");
        ui.add(egui::TextEdit::singleline(&mut app.f.data_dir).desired_width(380.0));
        ui.horizontal(|ui| {
            if ui.button("Browse…").clicked() {
                if let Some(d) = rfd::FileDialog::new().pick_folder() {
                    app.f.data_dir = d.display().to_string();
                }
            }
            match dataset_summary(&app.workers.repo_root.join(&app.f.data_dir)) {
                Some(s) => ui.colored_label(theme::GREEN, s),
                None => ui.colored_label(theme::YELLOW, "no class subdirs found"),
            };
        });
        ui.end_row();

        ui.label("Backbone");
        egui::ComboBox::from_id_salt("backbone")
            .selected_text(&app.f.backbone)
            .show_ui(ui, |ui| {
                ui.selectable_value(&mut app.f.backbone, "mobilenet_v2".into(), "mobilenet_v2 (default, ~470 ms on board)");
                ui.selectable_value(&mut app.f.backbone, "resnet18".into(), "resnet18 (slow on board: ~6 s int8)");
            });
        ui.label("");
        ui.end_row();

        ui.label("Epochs / input size");
        ui.horizontal(|ui| {
            ui.add(egui::DragValue::new(&mut app.f.epochs).range(1..=50));
            ui.label("epochs @");
            ui.add(egui::DragValue::new(&mut app.f.size).range(64..=224).speed(8));
            ui.label("px");
        });
        ui.label("");
        ui.end_row();

        ui.label("Model output");
        ui.add(egui::TextEdit::singleline(&mut app.f.model_out).desired_width(380.0));
        ui.label("");
        ui.end_row();
    });

    ui.add_space(8.0);
    ui.horizontal(|ui| {
        if app.training {
            ui.spinner();
            if ui.button("Stop").clicked() {
                app.workers.kill_py();
            }
        } else if ui
            .add(egui::Button::new(egui::RichText::new("▶ Train").color(theme::CRUST)).fill(theme::GREEN))
            .clicked()
        {
            app.training = true;
            app.epoch_loss.clear();
            app.epoch_acc.clear();
            app.train_status = "starting (first run downloads torch weights)…".into();
            let root = &app.workers.repo_root;
            app.workers.train(vec![
                "--data".into(),
                root.join(&app.f.data_dir).display().to_string(),
                "--out".into(),
                root.join(&app.f.model_out).display().to_string(),
                "--backbone".into(),
                app.f.backbone.clone(),
                "--epochs".into(),
                app.f.epochs.to_string(),
                "--size".into(),
                app.f.size.to_string(),
            ]);
        }
        ui.label(egui::RichText::new(&app.train_status).color(theme::SUBTEXT));
    });

    ui.add_space(10.0);
    ui.columns(2, |cols| {
        cols[0].label(egui::RichText::new("loss").color(theme::PEACH));
        Plot::new("loss_plot").height(260.0).show(&mut cols[0], |p| {
            p.line(Line::new("loss", PlotPoints::from(app.epoch_loss.clone())).color(theme::PEACH));
        });
        cols[1].label(egui::RichText::new("val accuracy").color(theme::GREEN));
        Plot::new("acc_plot")
            .height(260.0)
            .include_y(0.0)
            .include_y(1.0)
            .show(&mut cols[1], |p| {
                p.line(Line::new("val_acc", PlotPoints::from(app.epoch_acc.clone())).color(theme::GREEN));
            });
    });

    if let Some(m) = app.saved_model.clone() {
        ui.add_space(6.0);
        ui.horizontal(|ui| {
            ui.colored_label(theme::GREEN, format!("model ready: {m}"));
            if ui.button("→ Convert this model").clicked() {
                app.f.tab = crate::app::Tab::Convert;
            }
        });
    }
}
