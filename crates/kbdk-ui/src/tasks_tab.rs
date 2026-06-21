//! Tasks tab: board process list + kill.

use crate::app::KbdkApp;
use eframe::egui;

pub fn show(_app: &mut KbdkApp, ui: &mut egui::Ui) {
    ui.heading("Tasks");
    ui.label("process manager — coming in the next step");
}
