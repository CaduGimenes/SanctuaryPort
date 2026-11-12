//! The Performance page: frame rate, shaders and GPU data.
//!
//! The Aspect ratio box is not saved: it follows the render resolution, and choosing another
//! aspect moves the resolution to that aspect's closest size.


use iced::widget::{button, column, pick_list, text};
use iced::Element;
use serde_json::Value;

use crate::core::gpu::{no_msaa_decision, MsaaDecision};
use crate::core::i18n::{tr, tr_fmt};
use crate::core::options::{
    aspect_of, aspects, closest_resolution, resolution_label, resolutions_for, DRAW_PIPE,
    FPS_LIMITS, FRAMES_QUEUED, NO_MSAA, READBACKS, RESOLUTIONS, STREAM_MEMORY,
};
use crate::ui::app::{Message, State};
use crate::ui::theme;
use crate::ui::widgets::{
    choice_pairs, min_width, row, section, select_pairs, SELECT_MIN, SELECT_TEXT,
};

/// Suffix of the labels of the sizes a mod adds.
const MOD_SUFFIX: &str = " (mod)";

pub const NO_MSAA_HINT: &str = "The game's 2-sample depth buffer, read as a texture by its lighting. AMD's and Intel's Windows drivers read it wrong and the lighting shows black lines, so Automatic turns multisampling off on those cards (Radeon, Arc) and keeps it on elsewhere (NVIDIA). Off and On force the choice on any card.";


/// A built-in render resolution's label in `lang`, as the Python's `_()` of it: the whole label
/// when the tables have it, else the English label with its "Native" word translated.
fn shown_label(lang: &str, name: &str, aspect: bool) -> String {
    let english = resolution_label(name, aspect);
    let whole = tr(lang, &english);
    if whole != english || name != "native" {
        return whole;
    }
    english.replacen("Native", &tr(lang, "Native"), 1)
}

/// The render resolutions to choose from: the built-in ones and those of the enabled mods.
/// The render resolutions Diablo III has a patch for (patches/Diablo_III.xml), and native.
pub const DIABLO_III_RESOLUTIONS: [&str; 11] = [
    "native", "480p", "720p", "900p", "1440p", "1800p", "4K", "2560x1080", "3440x1440", "3840x1600",
    "5120x1440",
];

pub fn resolution_options(state: &State) -> Vec<(String, String)> {
    // SanctuaryPort: the sizes patches/Diablo_III.xml has a Resolution Patch for (16:9).
    let builtin = RESOLUTIONS
        .iter()
        .filter(|name| DIABLO_III_RESOLUTIONS.contains(name))
        .map(|name| (name.to_string(), shown_label(&state.lang, name, true)));
    builtin.collect()
}

fn resolution_values(state: &State) -> Vec<String> {
    resolution_options(state)
        .into_iter()
        .map(|(value, _)| value)
        .collect()
}

/// The aspect ratios the render resolutions have (built-in and mods).
pub fn aspect_options(state: &State) -> Vec<String> {
    aspects(&resolution_values(state))
}

/// The render resolutions of the chosen aspect, smallest first, without the ratio in the label.
pub fn aspect_resolution_options(state: &State) -> Vec<(String, String)> {
    let options = resolution_options(state);
    resolutions_for(&state.aspect, &resolution_values(state))
        .into_iter()
        .map(|name| {
            let is_mod = options
                .iter()
                .any(|(value, label)| *value == name && label.ends_with(MOD_SUFFIX));
            let label = if is_mod {
                resolution_label(&name, false) + MOD_SUFFIX
            } else {
                shown_label(&state.lang, &name, false)
            };
            (name, label)
        })
        .collect()
}

fn current_resolution(state: &State) -> String {
    state.store.text("render_resolution")
}

/// Chooses an aspect ratio and moves the render resolution to its closest size.
pub fn choose_aspect(state: &mut State, aspect: &str) {
    state.aspect = aspect.to_string();
    let current = current_resolution(state);
    if let Some(closest) = closest_resolution(aspect, &current, &resolution_values(state)) {
        if closest != current {
            state.store.set("render_resolution", Value::String(closest));
        }
    }
}

/// After the render resolution changed: the aspect box follows it.
pub(crate) fn resolution_chosen(state: &mut State) {
    if let Some(aspect) = aspect_of(&current_resolution(state)) {
        if aspect != state.aspect {
            state.aspect = aspect;
        }
    }
}

/// `NO_MSAA` with Automatic naming the chosen card and the game's decision for it.
pub fn no_msaa_label(state: &State, lang: &str) -> Vec<(String, String)> {
    let decision = no_msaa_decision(&state.gpus, &state.store.text("gpu_id"));
    NO_MSAA
        .iter()
        .map(|choice| {
            let label = match &decision {
                MsaaDecision::Known { name, off } if choice.value.is_empty() => {
                    let english = if *off {
                        "Automatic ({}: off)"
                    } else {
                        "Automatic ({}: on)"
                    };
                    tr_fmt(lang, english, &[name])
                }
                _ => tr(lang, choice.en),
            };
            (choice.value.to_string(), label)
        })
        .collect()
}

/// Automatic, then the cards in the order the game numbers them.
pub fn gpu_pairs(state: &State) -> Vec<(String, String)> {
    std::iter::once((String::new(), "Automatic".to_string()))
        .chain(
            state
                .gpus
                .iter()
                .map(|gpu| (gpu.index.clone(), gpu.name.clone())),
        )
        .collect()
}

/// One drop-down of the page: the key it stores, its (value, label) choices and the current value.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct SelectRow {
    pub key: &'static str,
    pub pairs: Vec<(String, String)>,
    pub current: String,
}

/// The drop-downs of the page; `view` builds them from this list.
pub fn rows(state: &State, lang: &str) -> Vec<SelectRow> {
    let stored = |key: &str| state.store.text(key);
    let cards = gpu_pairs(state)
        .into_iter()
        .map(|(value, label)| {
            let label = if value.is_empty() {
                tr(lang, &label)
            } else {
                label
            };
            (value, label)
        })
        .collect();
    let row = |key, pairs, current| SelectRow {
        key,
        pairs,
        current,
    };
    vec![
        row(
            "fps_limit",
            choice_pairs(FPS_LIMITS, lang),
            stored("fps_limit"),
        ),
        row(
            "frames_queued",
            choice_pairs(FRAMES_QUEUED, lang),
            stored("frames_queued"),
        ),
        row(
            "render_resolution",
            aspect_resolution_options(state),
            stored("render_resolution"),
        ),
        row("gpu_id", cards, stored("gpu_id")),
        row(
            "stream_memory",
            choice_pairs(STREAM_MEMORY, lang),
            stored("stream_memory"),
        ),
        row(
            "draw_pipe",
            choice_pairs(DRAW_PIPE, lang),
            stored("draw_pipe"),
        ),
        row(
            "readbacks",
            choice_pairs(READBACKS, lang),
            stored("readbacks"),
        ),
        row("no_msaa", no_msaa_label(state, lang), stored("no_msaa")),
    ]
}

/// Delete shader cache: the Advanced page's Clear shader cache under the Python's other name,
/// held while the game runs.
fn delete_cache_button<'a>(state: &State, lang: &str) -> Element<'a, Message> {
    let delete = button(text(tr(lang, "Delete shader cache"))).style(theme::button_flat);
    if state.game.is_live() {
        delete.into()
    } else {
        delete.on_press(Message::ClearShaderCache).into()
    }
}

/// The drop-down of `key` as the Performance page draws it (its pairs and current value).
pub fn select_row<'a>(state: &State, lang: &str, key: &str) -> Element<'a, Message> {
    select_from(&rows(state, lang), key)
}

/// The drop-down of `key` among rows already built; an empty one when no row has that key.
fn select_from<'a>(rows: &[SelectRow], key: &str) -> Element<'a, Message> {
    match rows.iter().find(|row| row.key == key) {
        Some(row) => select_pairs(key, &row.pairs, &row.current),
        None => select_pairs(key, &[], ""),
    }
}

/// SanctuaryPort: not here for Diablo III: the shader cache switches the runtime does not read
/// (disk reuse, prefetch, corner notes), screen effect switches the game lacks, and the deferred and
/// stale readbacks the game profile keeps off (with them the 3D hero of hero creation does not draw).
pub fn view(state: &State) -> Element<'_, Message> {
    let lang = state.lang.as_str();
    let hint = |english: &str| tr(lang, english);
    // A row that needs a new start of the game carries a small `restart` note.
    let restart = |english: &str| format!("{}  ·  {}", tr(lang, english), tr(lang, "restart"));

    // The rows are built once for the whole page.
    let selects = rows(state, lang);
    let pick = |key: &str| select_from(&selects, key);

    // SanctuaryPort: what players change. The pipeline paths, shader cache loading, readbacks,
    // stream memory and frames queued keep their defaults (the in-game menu still has the live
    // ones). The screen shape picks the render resolutions (16:9, ultrawide 21:9 and 32:9).
    let aspect = min_width(
        SELECT_MIN,
        pick_list(
            aspect_options(state),
            Some(state.aspect.clone()),
            Message::Aspect,
        )
        .text_size(SELECT_TEXT)
        .style(theme::pick_list_dark),
    );
    column![
        section(state.rtl, &tr(lang, "Frame rate")),
        row(
            state.rtl,
            &tr(lang, "Frame rate limit"),
            pick("fps_limit"),
            Some(&hint("Diablo III runs at 60 frames per second.")),
        ),
        row(
            state.rtl,
            &tr(lang, "Aspect ratio"),
            aspect.into(),
            Some(&hint("Your screen's shape: 21:9 and 32:9 are ultrawide monitors. Lists the render resolutions of that shape; the game widens its view to it.")),
        ),
        row(
            state.rtl,
            &restart("Render resolution"),
            pick("render_resolution"),
            Some(&hint(
                "The size the game draws at; higher is sharper and slower."
            )),
        ),
        section(state.rtl, &tr(lang, "Graphics card")),
        row(
            state.rtl,
            &restart("Graphics card"),
            pick("gpu_id"),
            Some(&hint(
                "The GPU the game runs on. Choose your dedicated card if the game uses the integrated one."
            )),
        ),
        row(
            state.rtl,
            &restart("Game multisampling"),
            pick("no_msaa"),
            Some(&hint(NO_MSAA_HINT)),
        ),
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
