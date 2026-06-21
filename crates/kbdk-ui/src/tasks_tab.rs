//! Tasks tab: board process list + kill.

use crate::app::KbdkApp;
use crate::theme;
use eframe::egui;

pub fn show(app: &mut KbdkApp, ui: &mut egui::Ui) {
    ui.horizontal(|ui| {
        ui.heading("Board processes");
        if ui.button("⟳ refresh").clicked() {
            app.workers.list_procs();
        }
        ui.label(egui::RichText::new(&app.tasks_status).color(theme::SUBTEXT));
    });
    ui.separator();

    // confirm dialog for kill -9
    if let Some((pid, cmd)) = app.kill_confirm.clone() {
        egui::Window::new("Force kill?")
            .collapsible(false)
            .resizable(false)
            .show(ui.ctx(), |ui| {
                ui.label(format!("Send SIGKILL (-9) to pid {pid}?"));
                ui.label(egui::RichText::new(&cmd).color(theme::SUBTEXT));
                ui.horizontal(|ui| {
                    if ui.button("Kill -9").clicked() {
                        app.workers.kill_proc(pid, 9);
                        app.kill_confirm = None;
                    }
                    if ui.button("Cancel").clicked() {
                        app.kill_confirm = None;
                    }
                });
            });
    }

    egui::ScrollArea::vertical().show(ui, |ui| {
        egui::Grid::new("procs")
            .striped(true)
            .num_columns(5)
            .show(ui, |ui| {
                ui.strong("PID");
                ui.strong("RSS");
                ui.strong("ST");
                ui.strong("COMMAND");
                ui.strong("");
                ui.end_row();

                for p in app.procs.clone() {
                    ui.label(p.pid.to_string());
                    ui.label(format!("{:.1}M", p.rss_kb as f64 / 1024.0));
                    ui.label(&p.state);
                    ui.label(egui::RichText::new(&p.cmd).color(theme::TEXT));
                    ui.horizontal(|ui| {
                        if ui.small_button("kill").clicked() {
                            app.workers.kill_proc(p.pid, 15); // SIGTERM
                        }
                        if ui
                            .small_button(egui::RichText::new("-9").color(theme::RED))
                            .clicked()
                        {
                            app.kill_confirm = Some((p.pid, p.cmd.clone()));
                        }
                    });
                    ui.end_row();
                }
            });
    });

    if app.procs.is_empty() {
        ui.label(egui::RichText::new("press ⟳ refresh to list board processes").color(theme::SUBTEXT));
    }
}
