//! Catppuccin Mocha, hand-rolled into egui Visuals (no catppuccin-egui dependency
//! so we aren't coupled to its egui-version release lag).

use eframe::egui::{self, Color32};

pub const BASE: Color32 = Color32::from_rgb(0x1e, 0x1e, 0x2e);
pub const MANTLE: Color32 = Color32::from_rgb(0x18, 0x18, 0x25);
pub const CRUST: Color32 = Color32::from_rgb(0x11, 0x11, 0x1b);
pub const SURFACE0: Color32 = Color32::from_rgb(0x31, 0x32, 0x44);
pub const SURFACE1: Color32 = Color32::from_rgb(0x45, 0x47, 0x5a);
pub const SURFACE2: Color32 = Color32::from_rgb(0x58, 0x5b, 0x70);
pub const OVERLAY0: Color32 = Color32::from_rgb(0x6c, 0x70, 0x86);
pub const TEXT: Color32 = Color32::from_rgb(0xcd, 0xd6, 0xf4);
pub const SUBTEXT: Color32 = Color32::from_rgb(0xa6, 0xad, 0xc8);
pub const BLUE: Color32 = Color32::from_rgb(0x89, 0xb4, 0xfa);
pub const LAVENDER: Color32 = Color32::from_rgb(0xb4, 0xbe, 0xfe);
pub const GREEN: Color32 = Color32::from_rgb(0xa6, 0xe3, 0xa1);
pub const RED: Color32 = Color32::from_rgb(0xf3, 0x8b, 0xa8);
pub const YELLOW: Color32 = Color32::from_rgb(0xf9, 0xe2, 0xaf);
pub const PEACH: Color32 = Color32::from_rgb(0xfa, 0xb3, 0x87);
pub const MAUVE: Color32 = Color32::from_rgb(0xcb, 0xa6, 0xf7);
pub const TEAL: Color32 = Color32::from_rgb(0x94, 0xe2, 0xd5);

pub fn apply(ctx: &egui::Context) {
    let mut v = egui::Visuals::dark();
    v.override_text_color = Some(TEXT);
    v.panel_fill = BASE;
    v.window_fill = MANTLE;
    v.extreme_bg_color = CRUST;
    v.faint_bg_color = SURFACE0;
    v.code_bg_color = CRUST;
    v.hyperlink_color = BLUE;
    v.warn_fg_color = YELLOW;
    v.error_fg_color = RED;
    v.selection.bg_fill = BLUE.linear_multiply(0.35);
    v.selection.stroke = egui::Stroke::new(1.0, BLUE);

    v.widgets.noninteractive.bg_fill = BASE;
    v.widgets.noninteractive.weak_bg_fill = MANTLE;
    v.widgets.noninteractive.bg_stroke = egui::Stroke::new(1.0, SURFACE0);
    v.widgets.noninteractive.fg_stroke = egui::Stroke::new(1.0, SUBTEXT);

    v.widgets.inactive.bg_fill = SURFACE0;
    v.widgets.inactive.weak_bg_fill = SURFACE0;
    v.widgets.inactive.fg_stroke = egui::Stroke::new(1.0, TEXT);

    v.widgets.hovered.bg_fill = SURFACE1;
    v.widgets.hovered.weak_bg_fill = SURFACE1;
    v.widgets.hovered.bg_stroke = egui::Stroke::new(1.0, OVERLAY0);
    v.widgets.hovered.fg_stroke = egui::Stroke::new(1.5, TEXT);

    v.widgets.active.bg_fill = SURFACE2;
    v.widgets.active.weak_bg_fill = SURFACE2;
    v.widgets.active.bg_stroke = egui::Stroke::new(1.0, LAVENDER);
    v.widgets.active.fg_stroke = egui::Stroke::new(2.0, TEXT);

    v.widgets.open.bg_fill = SURFACE1;
    v.widgets.open.weak_bg_fill = SURFACE1;

    ctx.set_visuals(v);
}
