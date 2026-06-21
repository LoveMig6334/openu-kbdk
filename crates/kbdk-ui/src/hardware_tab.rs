//! Hardware tab: read-only board inventory + live monitor.

use crate::app::KbdkApp;
use eframe::egui;

pub fn show(_app: &mut KbdkApp, ui: &mut egui::Ui) {
    ui.heading("Hardware");
    ui.label("board inventory — coming in the next step");
}
