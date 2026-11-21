//! The Graphics page: display and frame rate, resolution and sharpness, and a closed "More
//! options" section (dynamic resolution, edge smoothing, texture filtering), so a pad reaches the
//! common rows first. The shader cache and warm-up are under Advanced > Troubleshooting, the
//! screenshots folder under Advanced > Files.
//!
//! Every hint is one short line. Rows that need a new start of the game end with `↻`, which the
//! drawer subtitle explains once. FSR 1 and FSR 3 both enlarge a picture rendered below the
//! window, so one Upscaling choice (Off / FSR 1 / FSR 3) sets both `fsr1` and `temporal_aa`.
//! `output_res`, `live_resolution` and `object_motion` stay parked.

use iced::widget::{column, pick_list, text};
use iced::Element;

use crate::core::gpu::{no_msaa_decision, MsaaDecision};
use crate::core::i18n::tr;
use crate::core::options::{ANISOTROPY, DISPLAY_MODES, GAME_MSAA, GTAO_MODES};
use crate::ui::app::{Message, State};
use crate::ui::pages::controls::ini_number;
use crate::ui::pages::performance::{aspect_options, select_row, NO_MSAA_HINT};
use crate::ui::theme;
use crate::ui::widgets::{
    checkbox, min_width, row, section, section_fold, select, slider, toggle_row, SELECT_MIN,
    SELECT_TEXT,
};

/// The marker after the name of a row that applies when the game starts.
pub const RESTART_MARK: &str = "↻";

/// The hint of the FXAA switch.
pub const FXAA_HINT: &str = "Smooths edges at almost no cost.";
/// The FXAA hint while FSR 3 is on (FSR 3 replaces it).
pub const FXAA_OFF_HINT: &str = "Not used with FSR 3, which smooths edges itself.";
/// The note under Dynamic resolution; the translation tables key on this text.
pub const DYNRES_HINT: &str = "Lowers the scene in heavy fights to keep the FPS.";
/// The note under V-Sync.
pub const VSYNC_HINT: &str = "Removes tearing; on a 60 Hz screen it caps at 60.";
/// The folding section with the less used rows (`State::open_sections`).
pub const MORE_OPTIONS: &str = "graphics_more";
/// The experimental patch that moves characters every frame above 60 FPS (GitHub #24).
pub const SMOOTH_PATCH: &str = "Experimental: Smooth character motion above 60 FPS";
pub const SMOOTH_LABEL: &str = "Smooth character motion above 60 FPS (experimental)";
pub const SMOOTH_HINT: &str = "Above 60 FPS, characters move on every frame instead of every other one.";
/// The note under the frame rate limit (also on the first-start page).
pub const FRAME_RATE_HINT: &str = "Above 60 the camera is smoother, but characters may judder.";
/// The note under the GTAO quality choice.
pub const GTAO_HINT: &str =
    "Adds contact shadows in the 3D scene; Low is lighter for integrated GPUs.";
/// The note under the MSAA samples choice.
pub const GAME_MSAA_HINT: &str = "2x and 4x smooth edges more; 4x costs about a quarter of the FPS.";

/// The Upscaling choices: (id, English label, hint).
pub const UPSCALING: [(&str, &str, &str); 3] = [
    ("off", "Off", "A plain stretch when the resolution is below the screen."),
    ("fsr1", "FSR 1", "Enlarges with more detail than a plain stretch."),
    ("fsr3", "FSR 3", "Also smooths edges; heavy on handhelds."),
];

/// The stored Upscaling choice: FSR 3 wins over FSR 1, as in the game.
pub fn upscaling_now(state: &State) -> &'static str {
    if state.store.text("temporal_aa") == "fsr3" {
        "fsr3"
    } else if state.store.flag("fsr1") {
        "fsr1"
    } else {
        "off"
    }
}

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

/// What the edge smoothing choices do now, in one line (English, the translation key).
pub fn anti_aliasing_now(state: &State) -> &'static str {
    let fsr3 = state.store.text("temporal_aa") == "fsr3";
    match (fsr3, msaa_on(state), state.store.flag("fxaa")) {
        (true, _, _) => "Now: FSR 3 smooths edges.",
        (false, true, true) => "Now: MSAA and FXAA smooth edges.",
        (false, false, true) => "Now: FXAA smooths edges.",
        (false, true, false) => "Now: only MSAA smooths edges.",
        (false, false, false) => "Now: no edge smoothing.",
    }
}

/// The Upscaling drop-down.
fn upscaling<'a>(state: &State, lang: &str) -> Element<'a, Message> {
    let labels: Vec<String> = UPSCALING.iter().map(|(_, en, _)| tr(lang, en)).collect();
    let now = upscaling_now(state);
    let selected = UPSCALING
        .iter()
        .find(|(id, _, _)| *id == now)
        .map(|(_, en, _)| tr(lang, en));
    let lang = lang.to_string();
    let list = pick_list(labels, selected, move |chosen: String| {
        UPSCALING
            .iter()
            .find(|(_, en, _)| tr(&lang, en) == chosen)
            .map(|(id, _, _)| Message::Upscaling(id.to_string()))
            .unwrap_or(Message::Noop)
    })
    .text_size(SELECT_TEXT)
    .style(theme::pick_list_dark);
    min_width(SELECT_MIN, list).into()
}

/// The smooth motion switch: the game patch, ticked like the offline events; nothing when the
/// patch file does not have it.
fn smooth_row<'a>(state: &State, lang: &str) -> Element<'a, Message> {
    if !state.patches.selectable.iter().any(|m| m.name == SMOOTH_PATCH) {
        return column![].into();
    }
    toggle_row(
        state.rtl,
        &format!("{}  {RESTART_MARK}", tr(lang, SMOOTH_LABEL)),
        state.patches.chosen.contains(SMOOTH_PATCH),
        Some(|on| Message::PatchToggled(SMOOTH_PATCH.to_string(), on)),
        Some(&tr(lang, SMOOTH_HINT)),
    )
}

/// SanctuaryPort: Diablo III shows only what its runtime reads. Not here: the monitor (the game
/// opens on the primary one), frame generation (not calibrated for this game), the pipeline paths,
/// readbacks, stream memory and frames queued (their defaults; the in-game menu has the live ones)
/// and screen effects the game does not have.
pub fn view(state: &State) -> Element<'_, Message> {
    let lang = state.lang.as_str();
    let hint = |english: &str| tr(lang, english);
    let ini = |key: &str| state.store.text(key);
    let restart = |english: &str| format!("{}  {RESTART_MARK}", tr(lang, english));
    let fsr3 = upscaling_now(state) == "fsr3";

    let aspect = min_width(
        SELECT_MIN,
        pick_list(aspect_options(state), Some(state.aspect.clone()), Message::Aspect)
            .text_size(SELECT_TEXT)
            .style(theme::pick_list_dark),
    );
    let upscaling_hint = UPSCALING
        .iter()
        .find(|(id, _, _)| *id == upscaling_now(state))
        .map_or("", |(_, _, hint)| *hint);
    let fxaa: Element<'_, Message> = if fsr3 {
        toggle_row(
            state.rtl,
            "FXAA",
            false,
            None::<fn(bool) -> Message>,
            Some(&hint(FXAA_OFF_HINT)),
        )
    } else {
        checkbox(state.rtl, "fxaa", "FXAA", state.store.flag("fxaa"), Some(&hint(FXAA_HINT)))
    };

    let mut page = column![
        section(state.rtl, &tr(lang, "Display and frame rate")),
        row(
            state.rtl,
            &tr(lang, "Display mode"),
            select("display_mode", DISPLAY_MODES, &ini("display_mode"), lang),
            Some(&hint("Borderless: full screen with quick window switching.")),
        ),
        row(
            state.rtl,
            &restart("Frame rate"),
            select_row(state, lang, "frame_rate"),
            Some(&hint(FRAME_RATE_HINT)),
        ),
        smooth_row(state, lang),
        checkbox(
            state.rtl,
            "vertical_sync",
            &restart("V-Sync"),
            state.store.flag("vertical_sync"),
            Some(&hint(VSYNC_HINT)),
        ),
    ]
    .spacing(2);
    // Only a choice with two cards (a laptop), or when one was chosen before.
    if state.gpus.len() > 1 || !state.store.text("gpu_id").is_empty() {
        page = page.push(row(
            state.rtl,
            &restart("Graphics card"),
            select_row(state, lang, "gpu_id"),
            Some(&hint("On laptops with two cards, choose the dedicated one.")),
        ));
    }
    page = page
        .push(checkbox(
            state.rtl,
            "show_fps",
            &tr(lang, "Show performance overlay"),
            state.store.flag("show_fps"),
            Some(&hint("Items and corner: in-game menu > Overlay.")),
        ))
        .push(section(state.rtl, &tr(lang, "Resolution and sharpness")))
        .push(row(
            state.rtl,
            &tr(lang, "Aspect ratio"),
            aspect.into(),
            Some(&hint("21:9 and 32:9 show more of the sides.")),
        ))
        .push(row(
            state.rtl,
            &restart("Render resolution"),
            select_row(state, lang, "render_resolution"),
            Some(&hint("Lower runs faster; Native is the sharpest.")),
        ))
        .push(row(
            state.rtl,
            &tr(lang, "Upscaling"),
            upscaling(state, lang),
            Some(&hint(upscaling_hint)),
        ))
        .push(checkbox(
            state.rtl,
            "rcas",
            &tr(lang, "Sharpening (RCAS)"),
            state.store.flag("rcas"),
            Some(&hint("Makes the picture crisper.")),
        ));
    // The strength only while Sharpening is on.
    if state.store.flag("rcas") {
        page = page.push(row(
            state.rtl,
            &tr(lang, "Sharpening strength"),
            slider("rcas_strength", 0.0, 100.0, "%", ini_number(state, "rcas_strength")),
            None,
        ));
    }
    page = page.push(row(
        state.rtl,
        &tr(lang, "GTAO (experimental)"),
        select("gtao", GTAO_MODES, &ini("gtao"), lang),
        Some(&hint(GTAO_HINT)),
    ));

    let open = state.open_sections.contains(&MORE_OPTIONS);
    page = page.push(section_fold(state.rtl, &tr(lang, "More options"), open, MORE_OPTIONS));
    if open {
        page = page
            .push(checkbox(
                state.rtl,
                "dynamic_resolution",
                &restart("Dynamic resolution"),
                state.store.flag("dynamic_resolution"),
                Some(&hint(DYNRES_HINT)),
            ))
            .push(
                text(tr(lang, anti_aliasing_now(state)))
                    .size(12)
                    .color(theme::palette::GOLD),
            )
            .push(row(
                state.rtl,
                &restart("Game MSAA (2x)"),
                select_row(state, lang, "no_msaa"),
                Some(&hint(NO_MSAA_HINT)),
            ))
            .push(row(
                state.rtl,
                &restart("MSAA samples"),
                select("msaa", GAME_MSAA, &ini("msaa"), lang),
                Some(&hint(GAME_MSAA_HINT)),
            ))
            .push(fxaa)
            .push(row(
                state.rtl,
                &restart("Anisotropic filtering"),
                select("anisotropy", ANISOTROPY, &ini("anisotropy"), lang),
                Some(&hint("Floors and walls stay sharp in the distance.")),
            ));
    }
    page.into()
}
