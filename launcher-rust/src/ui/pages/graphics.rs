//! The Graphics page: display, resolution, anti-aliasing, upscaling, textures, frame rate,
//! graphics card, overlay, screenshots and troubleshooting.
//!
//! Everything about the picture lives here (the Performance page it shared this with is gone):
//! a player looking for anti-aliasing finds the game's MSAA and FSR 3 in one section, with a line
//! saying what they do with the current choices. FSR 1 and FSR 3 both enlarge a picture rendered
//! below the window; FSR 3 wins, so the Upscaler row shows it and lets FSR 1 be chosen only when
//! FSR 3 is off. `output_res`, `live_resolution` and `object_motion` stay parked.

use iced::widget::{button, column, pick_list, text};
use iced::Element;
use serde_json::Value;

use crate::core::gpu::{no_msaa_decision, MsaaDecision};
use crate::core::i18n::tr;
use crate::core::options::{ANISOTROPY, DISPLAY_MODES, TEMPORAL_AA};
use crate::ui::app::{Message, State};
use crate::ui::pages::controls::ini_number;
use crate::ui::pages::performance::{aspect_options, select_row, NO_MSAA_HINT};
use crate::ui::theme;
use crate::ui::widgets::{checkbox, min_width, row, section, select, slider, SELECT_MIN, SELECT_TEXT};

/// The hint of the Upscaler row.
pub const UPSCALER_HINT: &str =
    "Used when the Render resolution is below the window or screen: AMD FSR 1 enlarges it sharper than a plain stretch.";

/// The hint of the temporal anti-aliasing choice (SanctuaryPort).
pub const TEMPORAL_AA_HINT: &str =
    "Smooths edges using the previous frames: the best quality. Costs about 3-6 ms on integrated graphics.";

/// The hint of the FXAA switch.
pub const FXAA_HINT: &str =
    "Cheap smoothing of the 3D picture (the HUD stays sharp), a little soft. Not used while FSR 3 is on.";

/// The hint of the Frame rate row.
pub const FRAME_RATE_HINT: &str =
    "The game's own frame limiter. 120 and Unlimited go past 60 when your PC keeps up; the game's speed stays the same.";

/// The Upscaler row's choices: (FSR 1 on, English label).
const UPSCALERS: [(bool, &str); 2] = [(false, "Plain stretch"), (true, "AMD FSR 1")];

/// Whether the game's multisampling ends up on with the stored choice and the chosen card.
fn msaa_on(state: &State) -> bool {
    match state.store.text("no_msaa").as_str() {
        "0" => true,
        "1" => false,
        _ => !matches!(
            no_msaa_decision(&state.gpus, &state.store.text("gpu_id")),
            MsaaDecision::Known { off: true, .. }
        ),
    }
}

/// What the anti-aliasing choices do now, in one line (English, the translation key).
pub fn anti_aliasing_now(state: &State) -> &'static str {
    let fsr3 = state.store.text("temporal_aa") == "fsr3";
    let window = state.store.text("display_mode") == "windowed";
    let fxaa = state.store.flag("fxaa");
    match (fsr3, window, msaa_on(state), fxaa) {
        (true, true, _, _) => "Now: FSR 3 smooths edges (in a window the window is the render size).",
        (true, false, _, _) => {
            "Now: FSR 3 smooths edges and enlarges the picture when the Render resolution is below your screen."
        }
        (false, _, true, true) => "Now: the game's MSAA and FXAA smooth edges.",
        (false, _, false, true) => "Now: FXAA smooths edges.",
        (false, _, true, false) => "Now: only the game's MSAA smooths edges.",
        (false, _, false, false) => "Now: no edge smoothing. Turn FSR 3 or FXAA on to smooth jagged edges.",
    }
}

/// The Upscaler row: FSR 3 when it is on (set in Anti-aliasing), else plain stretch or FSR 1.
fn upscaler<'a>(state: &State, lang: &str) -> Element<'a, Message> {
    if state.store.text("temporal_aa") == "fsr3" {
        return text(tr(lang, "FSR 3 (set in Anti-aliasing)"))
            .size(SELECT_TEXT)
            .color(theme::palette::MUTED)
            .into();
    }
    let labels: Vec<String> = UPSCALERS.iter().map(|(_, en)| tr(lang, en)).collect();
    let fsr1 = state.store.flag("fsr1");
    let selected = UPSCALERS
        .iter()
        .find(|(on, _)| *on == fsr1)
        .map(|(_, en)| tr(lang, en));
    let lang = lang.to_string();
    let list = pick_list(labels, selected, move |chosen: String| {
        UPSCALERS
            .iter()
            .find(|(_, en)| tr(&lang, en) == chosen)
            .map(|(on, _)| Message::Set("fsr1".to_string(), Value::Bool(*on)))
            .unwrap_or(Message::Noop)
    })
    .text_size(SELECT_TEXT)
    .style(theme::pick_list_dark);
    min_width(SELECT_MIN, list).into()
}

/// Delete shader cache, held while the game runs.
fn delete_cache_button<'a>(state: &State, lang: &str) -> Element<'a, Message> {
    let delete = button(text(tr(lang, "Delete shader cache"))).style(theme::button_flat);
    if state.game.is_live() {
        delete.into()
    } else {
        delete.on_press(Message::ClearShaderCache).into()
    }
}

/// SanctuaryPort: Diablo III shows only what its runtime reads. Not here: the monitor (the game
/// opens on the primary one), VSync (the profile keeps it off: FIFO presents capped the engine at
/// 20 FPS), frame generation (not calibrated for this game), the pipeline paths, readbacks, stream
/// memory and frames queued (their defaults; the in-game menu has the live ones) and screen effects
/// the game does not have.
pub fn view(state: &State) -> Element<'_, Message> {
    let lang = state.lang.as_str();
    let hint = |english: &str| tr(lang, english);
    let ini = |key: &str| state.store.text(key);
    // A row that needs a new start of the game carries a small `restart` note.
    let restart = |english: &str| format!("{}  ·  {}", tr(lang, english), tr(lang, "restart"));

    let aspect = min_width(
        SELECT_MIN,
        pick_list(aspect_options(state), Some(state.aspect.clone()), Message::Aspect)
            .text_size(SELECT_TEXT)
            .style(theme::pick_list_dark),
    );
    let strength: Element<'_, Message> = if state.store.flag("rcas") {
        slider("rcas_strength", 0.0, 100.0, "%", ini_number(state, "rcas_strength"))
    } else {
        text(tr(lang, "Turn Sharpening on to use this."))
            .size(SELECT_TEXT)
            .color(theme::palette::MUTED)
            .into()
    };
    let shots = button(text(tr(lang, "Open the screenshots folder")))
        .style(theme::button_flat)
        .on_press(Message::OpenFolder(
            state
                .paths
                .user_dir(&state.store.text("user_dir"))
                .join("screenshots"),
        ));

    column![
        section(state.rtl, &tr(lang, "Display")),
        row(
            state.rtl,
            &tr(lang, "Display mode"),
            select("display_mode", DISPLAY_MODES, &ini("display_mode"), lang),
            Some(&hint("Borderless covers the screen without changing the display mode.")),
        ),
        section(state.rtl, &tr(lang, "Resolution")),
        row(
            state.rtl,
            &tr(lang, "Aspect ratio"),
            aspect.into(),
            Some(&hint("Your screen's shape: 21:9 and 32:9 are ultrawide monitors. Lists the render resolutions of that shape; the game widens its view to it.")),
        ),
        row(
            state.rtl,
            &restart("Render resolution"),
            select_row(state, lang, "render_resolution"),
            Some(&hint(
                "Native is the sharpest; lower gives more FPS and is enlarged to the window."
            )),
        ),
        section(state.rtl, &tr(lang, "Anti-aliasing")),
        row(
            state.rtl,
            &restart("Game MSAA (2x)"),
            select_row(state, lang, "no_msaa"),
            Some(&hint(NO_MSAA_HINT)),
        ),
        checkbox(
            state.rtl,
            "fxaa",
            "FXAA",
            state.store.flag("fxaa"),
            Some(&hint(FXAA_HINT)),
        ),
        row(
            state.rtl,
            &tr(lang, "Temporal anti-aliasing (FSR 3)"),
            select("temporal_aa", TEMPORAL_AA, &ini("temporal_aa"), lang),
            Some(&hint(TEMPORAL_AA_HINT)),
        ),
        text(tr(lang, anti_aliasing_now(state)))
            .size(12)
            .color(theme::palette::GOLD),
        section(state.rtl, &tr(lang, "Upscaling")),
        row(
            state.rtl,
            &tr(lang, "Upscaler"),
            upscaler(state, lang),
            Some(&hint(UPSCALER_HINT)),
        ),
        checkbox(
            state.rtl,
            "rcas",
            &tr(lang, "Sharpening (RCAS)"),
            state.store.flag("rcas"),
            Some(&hint("Adds back detail after smoothing or enlarging. Also in the in-game menu.")),
        ),
        row(state.rtl, &tr(lang, "Sharpening strength"), strength, None),
        section(state.rtl, &tr(lang, "Textures")),
        row(
            state.rtl,
            &restart("Anisotropic filtering"),
            select("anisotropy", ANISOTROPY, &ini("anisotropy"), lang),
            Some(&hint(
                "Sharper floors and walls seen at an angle, at little cost on most graphics cards."
            )),
        ),
        section(state.rtl, &tr(lang, "Frame rate")),
        row(
            state.rtl,
            &restart("Frame rate"),
            select_row(state, lang, "frame_rate"),
            Some(&hint(FRAME_RATE_HINT)),
        ),
        section(state.rtl, &tr(lang, "Graphics card")),
        row(
            state.rtl,
            &restart("Graphics card"),
            select_row(state, lang, "gpu_id"),
            Some(&hint(
                "The GPU the game runs on. Choose your dedicated card if the game uses the integrated one."
            )),
        ),
        section(state.rtl, &tr(lang, "Overlay")),
        checkbox(
            state.rtl,
            "show_fps",
            &tr(lang, "Show performance overlay"),
            state.store.flag("show_fps"),
            Some(&hint("Items, size and corner: in-game menu (Insert / R3+L2) > Overlay.")),
        ),
        section(state.rtl, &tr(lang, "Screenshots")),
        row(
            state.rtl,
            &tr(lang, "Screenshot key: F12"),
            shots.into(),
            Some(&hint(
                "Saves the game's image, without the overlay, as a PNG in the screenshots folder of the saves."
            )),
        ),
        section(state.rtl, &tr(lang, "Troubleshooting")),
        row(
            state.rtl,
            &tr(lang, "Shader cache"),
            delete_cache_button(state, lang),
            Some(&hint(
                "Deletes the saved shaders and pipelines of this saves folder. Use it if graphics look wrong after an update or the cache grew too big; the next start compiles everything again."
            )),
        ),
    ]
    .spacing(2)
    .into()
}
