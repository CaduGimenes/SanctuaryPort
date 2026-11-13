//! The picture settings shared by the Graphics page and the first start: render resolutions and
//! aspect ratios, the multisampling choice and the drop-downs built from them (the Performance
//! page itself is gone: its rows are on the Graphics page).
//!
//! The Aspect ratio box is not saved: it follows the render resolution, and choosing another
//! aspect moves the resolution to that aspect's closest size.


use iced::Element;
use serde_json::Value;

use crate::core::gpu::{no_msaa_decision, MsaaDecision};
use crate::core::i18n::{tr, tr_fmt};
use crate::core::options::{
    aspect_of, aspects, closest_resolution, resolution_label, resolutions_for, DRAW_PIPE,
    FPS_LIMITS, FRAMES_QUEUED, NO_MSAA, READBACKS, RESOLUTIONS, STREAM_MEMORY,
};
use crate::ui::app::{Message, State};
use crate::ui::widgets::{choice_pairs, select_pairs};

/// Suffix of the labels of the sizes a mod adds.
const MOD_SUFFIX: &str = " (mod)";

pub const NO_MSAA_HINT: &str = "The game's own edge smoothing. Automatic turns it off on AMD and Intel cards on Windows, where it can draw black lines; On is experimental there.";


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

/// The drop-down of `key` as the Graphics page draws it (its pairs and current value).
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
