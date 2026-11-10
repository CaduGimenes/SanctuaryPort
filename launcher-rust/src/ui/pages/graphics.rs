//! The Graphics page: window, upscaling, effects, textures, overlay and screenshots.
//!
//! Upscaling shows the switch, the upscaler and its preset; the upscaler outputs the Render
//! resolution of the Performance page. `output_res`, `live_resolution` and `object_motion` stay
//! parked.

use iced::widget::{button, column, text};
use iced::Element;

use crate::core::i18n::tr;
use crate::core::options::{
    ANISOTROPY, DISPLAY_MODES,
};
use crate::ui::app::{Message, State};
use crate::ui::theme;
use crate::ui::pages::controls::ini_number;
use crate::ui::widgets::{checkbox, row, section, select, slider};

/// The hint of the FSR 1 switch (SanctuaryPort).
pub const FSR1_HINT: &str = "With a Render resolution (Performance page) below the window, AMD FSR 1 upscales the game's picture instead of a plain stretch: render at 720p or 900p for more FPS and keep a sharp 1080p window. Works on any graphics card; borderless and fullscreen upscale to the whole screen.";

/// The hint of the Anti-aliasing row: FXAA / SMAA here, and the port's own TAA under Upscaler.
pub const AA_HINT: &str = "Smooths jagged edges. FXAA is cheap and a little soft; SMAA keeps the picture sharper and costs a bit more. Both also touch the HUD slightly. For temporal anti-aliasing at native resolution choose TAA under Upscaler (Native AA preset): the port's own, tuned for this game. Its default values are the optimized ones, and every setting can be adjusted to your taste in the in-game menu (Insert / R3+L2) > Image. Use TAA with this set to Off.";

/// SanctuaryPort: Diablo III shows only what its runtime reads. Not here: the monitor (the game
/// opens on the primary one), VSync (the profile keeps it off: FIFO presents capped the engine at
/// 20 FPS), the upscalers and frame generation (run.py keeps them off, not calibrated for this
/// game) and the screen effect switches the game does not have (motion blur, depth of field,
/// anti-aliasing).
pub fn view(state: &State) -> Element<'_, Message> {
    let lang = state.lang.as_str();
    let hint = |english: &str| tr(lang, english);
    let ini = |key: &str| state.store.text(key);
    // The Python marks a row that needs a new start of the game with a small `restart` note.
    let anisotropy_label = format!(
        "{}  ·  {}",
        tr(lang, "Anisotropic filtering"),
        tr(lang, "restart")
    );

    let shots = button(text(tr(lang, "Open the screenshots folder")))
        .style(theme::button_flat)
        .on_press(Message::OpenFolder(
            state
                .paths
                .user_dir(&state.store.text("user_dir"))
                .join("screenshots"),
        ));

    column![
        section(state.rtl, &tr(lang, "Window")),
        row(
            state.rtl,
            &tr(lang, "Display mode"),
            select("display_mode", DISPLAY_MODES, &ini("display_mode"), lang),
            Some(&hint(
                "Borderless covers the screen without changing the display mode."
            )),
        ),
        checkbox(
            state.rtl,
            "hdr",
            &tr(lang, "Allow HDR output"),
            state.store.flag("hdr"),
            Some(&hint("When HDR is on in Windows and the display supports it.")),
        ),
        section(state.rtl, &tr(lang, "Upscaling")),
        checkbox(
            state.rtl,
            "fsr1",
            &tr(lang, "Upscale with FSR 1"),
            state.store.flag("fsr1"),
            Some(&hint(FSR1_HINT)),
        ),
        checkbox(
            state.rtl,
            "rcas",
            &tr(lang, "Sharpening (RCAS)"),
            state.store.flag("rcas"),
            Some(&hint("Sharpens the game's picture on its way to the window. Also in the in-game menu.")),
        ),
        row(
            state.rtl,
            &tr(lang, "Sharpening strength"),
            slider("rcas_strength", 0.0, 100.0, "%", ini_number(state, "rcas_strength")),
            None,
        ),
        section(state.rtl, &tr(lang, "Textures")),
        row(
            state.rtl,
            &anisotropy_label,
            select("anisotropy", ANISOTROPY, &ini("anisotropy"), lang),
            Some(&hint(
                "Sharper floors and walls seen at an angle, at little cost on most graphics cards."
            )),
        ),
        section(state.rtl, &tr(lang, "Overlay")),
        checkbox(
            state.rtl,
            "show_fps",
            &tr(lang, "Show performance overlay"),
            state.store.flag("show_fps"),
            Some(&hint(
                "Items, size and corner: in-game menu (Insert / R3+L2) > Overlay."
            )),
        ),
        section(state.rtl, &tr(lang, "Screenshots")),
        row(
            state.rtl,
            &tr(lang, "F12 in the game"),
            shots.into(),
            Some(&hint(
                "Saves the game's image, without the overlay, as a PNG in the screenshots folder of the saves."
            )),
        ),
    ]
    .spacing(2)
    .into()
}
