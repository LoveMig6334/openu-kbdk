//! Hardware tab: read-only board inventory + live monitor.

use crate::app::KbdkApp;
use crate::theme;
use eframe::egui;

pub fn show(app: &mut KbdkApp, ui: &mut egui::Ui) {
    ui.horizontal(|ui| {
        ui.heading("Hardware");
        if ui.button("⟳ refresh").clicked() {
            app.hw_probing = true;
            app.workers.probe_hw();
        }
        ui.label(egui::RichText::new(&app.hw_status).color(theme::SUBTEXT));
    });
    ui.separator();

    // fetch the static inventory once on first open
    if app.hw_info.is_none() && !app.hw_probing {
        app.hw_probing = true;
        app.workers.probe_hw();
    }

    egui::ScrollArea::vertical().show(ui, |ui| {
        if let Some(info) = app.hw_info.clone() {
            for sec in &info.sections {
                egui::CollapsingHeader::new(&sec.title)
                    .default_open(true)
                    .show(ui, |ui| {
                        egui::Grid::new(format!("hw_sec_{}", sec.title))
                            .num_columns(2)
                            .striped(true)
                            .show(ui, |ui| {
                                for (k, v) in &sec.rows {
                                    ui.label(egui::RichText::new(k).color(theme::SUBTEXT));
                                    ui.label(v);
                                    ui.end_row();
                                }
                            });
                    });
            }
        } else {
            ui.label(egui::RichText::new("probing board…").color(theme::SUBTEXT));
        }
    });
}
