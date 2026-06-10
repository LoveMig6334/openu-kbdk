#![cfg_attr(not(debug_assertions), windows_subsystem = "windows")]

mod app;
mod convert_tab;
mod deploy_tab;
mod theme;
mod train_tab;
mod workers;

fn main() -> eframe::Result {
    let options = eframe::NativeOptions {
        viewport: eframe::egui::ViewportBuilder::default()
            .with_inner_size([1060.0, 720.0])
            .with_min_inner_size([800.0, 560.0])
            .with_title("kbdk — KidBright µAI dev-kit"),
        ..Default::default()
    };
    eframe::run_native(
        "kbdk-ui",
        options,
        Box::new(|cc| Ok(Box::new(app::KbdkApp::new(cc)))),
    )
}
