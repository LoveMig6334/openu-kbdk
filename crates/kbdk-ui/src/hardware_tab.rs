//! Hardware tab: read-only board inventory + live monitor.

use crate::app::KbdkApp;
use crate::theme;
use eframe::egui;
use egui_plot::{Line, Plot, PlotPoints};

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

    // Live monitor: poll every ~2 s, but only while this tab is visible (egui
    // runs only the active tab's show()). The in-flight guard avoids overlap.
    let now = std::time::Instant::now();
    let due = app
        .hw_last_live_poll
        .map_or(true, |t| now.duration_since(t) >= std::time::Duration::from_secs(2));
    if due && !app.hw_live_inflight {
        app.hw_live_inflight = true;
        app.hw_last_live_poll = Some(now);
        app.workers.probe_hw_live();
    }
    ui.ctx().request_repaint_after(std::time::Duration::from_secs(2));

    egui::ScrollArea::vertical().show(ui, |ui| {
        if let Some(s) = app.hw_live.clone() {
            egui::CollapsingHeader::new("Live")
                .default_open(true)
                .show(ui, |ui| {
                    egui::Grid::new("hw_live")
                        .num_columns(2)
                        .striped(true)
                        .show(ui, |ui| {
                            ui.label(egui::RichText::new("CPU load").color(theme::SUBTEXT));
                            ui.label(format!("{:.2}", s.load1));
                            ui.end_row();
                            ui.label(egui::RichText::new("Free RAM").color(theme::SUBTEXT));
                            ui.label(format!("{} / {} kB", s.mem_avail_kb, s.mem_total_kb));
                            ui.end_row();
                            ui.label(egui::RichText::new("Temp").color(theme::SUBTEXT));
                            ui.label(match s.temp_c {
                                Some(c) => format!("{c:.1} °C"),
                                None => "n/a".into(),
                            });
                            ui.end_row();
                            ui.label(egui::RichText::new("Uptime").color(theme::SUBTEXT));
                            ui.label(fmt_uptime(s.uptime_s));
                            ui.end_row();
                        });
                    if app.hw_mem_hist.len() > 1 {
                        Plot::new("hw_mem").height(60.0).show(ui, |pui| {
                            pui.line(Line::new(
                                "free kB",
                                PlotPoints::from(app.hw_mem_hist.clone()),
                            ));
                        });
                    }
                });
        }
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

fn fmt_uptime(s: u64) -> String {
    let (h, m) = (s / 3600, (s % 3600) / 60);
    format!("{h}h {m}m")
}
