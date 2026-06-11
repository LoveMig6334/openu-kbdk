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
                "training on {} — {} train, classes: {}",
                v["device"].as_str().unwrap_or("?"),
                v["n_train"],
                app.train_classes.join(", ")
            );
        }
        "epoch" => {
            let n = v["n"].as_f64().unwrap_or(0.0);
            if let Some(loss) = v["loss"].as_f64() {
                app.epoch_loss.push([n, loss]);
            }
            // classification reports val_acc, detection reports det_rate —
            // both land on the same 0..1 quality plot
            if let Some(acc) = v["val_acc"].as_f64().or_else(|| v["det_rate"].as_f64()) {
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

fn dataset_summary(dir: &std::path::Path, task: &str) -> Option<String> {
    if task == "detection" {
        // YOLO/Darknet layout: images/, labels/, classes.txt
        let classes = std::fs::read_to_string(dir.join("classes.txt")).ok()?;
        let n = std::fs::read_dir(dir.join("images")).ok()?.count();
        return Some(format!("{} classes, {n} images (YOLO)", classes.split_whitespace().count()));
    }
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

/// The input size each task/backbone combination is built for.
fn default_size(task: &str, backbone: &str) -> u32 {
    match (task, backbone) {
        (_, "npu_mid") => 112,            // npu_mid: fixed 112 (cls and det)
        (_, "npu_repvgg") => 112,         // pretrained transfer: fixed 112
        ("detection", "npu_slim") => 112, // npu_det: 4 pools -> 7x7 grid
        (_, "npu_slim") => 64,            // npu_slim classifier
        _ => 224,
    }
}

/// Kick off training with the current fields (the Train button's action; also
/// driven by the KBDK_AUTOTRAIN test hook).
pub fn start(app: &mut KbdkApp) {
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
        "--task".into(),
        app.f.task.clone(),
        "--backbone".into(),
        app.f.backbone.clone(),
        "--epochs".into(),
        app.f.epochs.to_string(),
        "--size".into(),
        app.f.size.to_string(),
    ]);
}

pub fn show(app: &mut KbdkApp, ui: &mut egui::Ui) {
    ui.heading("Train a model");
    ui.add_space(6.0);

    let (prev_task, prev_backbone) = (app.f.task.clone(), app.f.backbone.clone());

    egui::Grid::new("train_grid").num_columns(3).spacing([10.0, 8.0]).show(ui, |ui| {
        ui.label("Task");
        egui::ComboBox::from_id_salt("task")
            .selected_text(&app.f.task)
            .show_ui(ui, |ui| {
                ui.selectable_value(&mut app.f.task, "classification".into(), "classification (ImageFolder dataset)");
                ui.selectable_value(&mut app.f.task, "detection".into(), "detection (YOLO dataset: images/ labels/ classes.txt)");
            });
        ui.label("");
        ui.end_row();

        ui.label(if app.f.task == "detection" { "Dataset (YOLO)" } else { "Dataset (ImageFolder)" });
        ui.add(egui::TextEdit::singleline(&mut app.f.data_dir).desired_width(380.0));
        ui.horizontal(|ui| {
            if ui.button("Browse…").clicked() {
                if let Some(d) = rfd::FileDialog::new().pick_folder() {
                    app.f.data_dir = d.display().to_string();
                }
            }
            match dataset_summary(&app.workers.repo_root.join(&app.f.data_dir), &app.f.task) {
                Some(s) => ui.colored_label(theme::GREEN, s),
                None => ui.colored_label(theme::YELLOW, "dataset layout not recognized"),
            };
        });
        ui.end_row();

        ui.label("Backbone");
        egui::ComboBox::from_id_salt("backbone")
            .selected_text(&app.f.backbone)
            .show_ui(ui, |ui| {
                ui.selectable_value(&mut app.f.backbone, "mobilenet_v2".into(), "mobilenet_v2 (CPU int8: ~470 ms cls / ~720 ms det)");
                if app.f.task == "classification" {
                    ui.selectable_value(&mut app.f.backbone, "resnet18".into(), "resnet18 (slow on board: ~6 s int8)");
                }
                ui.selectable_value(&mut app.f.backbone, "npu_slim".into(), "npu_slim (NPU: 1.9 ms cls @64² / 3 ms det @112²)");
                ui.selectable_value(&mut app.f.backbone, "npu_mid".into(), "npu_mid (NPU, wider: ~5 ms @112², more accuracy headroom)");
                if app.f.task == "classification" {
                    ui.selectable_value(&mut app.f.backbone, "npu_repvgg".into(), "npu_repvgg (NPU, ImageNet-pretrained RepVGG-B0 transfer @112²)");
                }
            });
        ui.label("");
        ui.end_row();

        ui.label("Epochs / input size");
        ui.horizontal(|ui| {
            ui.add(egui::DragValue::new(&mut app.f.epochs).range(1..=200));
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

    // task/backbone changes re-seed the input size with that combo's default
    // (npu_slim nets are fixed-size; resnet18 has no detection head)
    if app.f.task != prev_task || app.f.backbone != prev_backbone {
        if app.f.task == "detection" && app.f.backbone == "resnet18" {
            app.f.backbone = "mobilenet_v2".into();
        }
        app.f.size = default_size(&app.f.task, &app.f.backbone);
    }

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
            start(app);
        }
        ui.label(egui::RichText::new(&app.train_status).color(theme::SUBTEXT));
    });

    ui.add_space(10.0);
    ui.columns(2, |cols| {
        cols[0].label(egui::RichText::new("loss").color(theme::PEACH));
        Plot::new("loss_plot").height(260.0).show(&mut cols[0], |p| {
            p.line(Line::new("loss", PlotPoints::from(app.epoch_loss.clone())).color(theme::PEACH));
        });
        cols[1].label(egui::RichText::new("val accuracy / det rate").color(theme::GREEN));
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
