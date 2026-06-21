//! Files tab: dual-pane local <-> board file transfer + management.

use crate::app::KbdkApp;
use eframe::egui;

pub fn show(_app: &mut KbdkApp, ui: &mut egui::Ui) {
    ui.heading("Files");
    ui.label("dual-pane file manager — coming in the next step");
}
