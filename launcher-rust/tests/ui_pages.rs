#![allow(clippy::unwrap_used, clippy::expect_used)]
//! The settings pages of the drawer (Tasks 24-31).

mod common;

use common::{
    browsed, click, dump_with_patches, gpu_rows, load_trophies, messages,
    open, state, state_with_trophies, two_displays, ARC_LAPTOP,
};
use sanctuaryport_launcher::core::env::game_environment;
use sanctuaryport_launcher::core::gpu::{display_label, parse_display_list};
use sanctuaryport_launcher::core::options::DISPLAY_MODES;
use sanctuaryport_launcher::core::pad_model::PadModel;
use sanctuaryport_launcher::ui::app::{self, Message};
use sanctuaryport_launcher::ui::pages::{performance, Page};
use sanctuaryport_launcher::ui::widgets::{self, select_message};

#[test]
fn graphics_rows_change_the_right_keys() {
    let dir = tempfile::tempdir().unwrap();
    let mut state = state(dir.path());
    open(&mut state, Page::Graphics);
    assert!(!state.store.flag("hdr"));
    click(&mut state, "Allow HDR output");
    assert!(state.store.flag("hdr"));
    // iced_test cannot open a pick_list: the pure mapping stands in for the click.
    let pick = select_message("display_mode", DISPLAY_MODES, "en", "Borderless").unwrap();
    assert!(
        matches!(&pick, Message::Set(key, value) if key == "display_mode" && value == "borderless")
    );
    let _ = app::update(&mut state, pick);
    assert_eq!(state.store.text("display_mode"), "borderless");
    assert!(state.store.json("display_mode").is_none());
}

#[test]
fn a_stored_string_zero_shows_checked_like_the_game_gets_it() {
    // Python truthiness: "0" is a non-empty string, so the game gets SANCTUARY_HDR=1, and the box
    // shows what the game gets.
    let dir = tempfile::tempdir().unwrap();
    std::fs::write(dir.path().join("settings.json"), r#"{"hdr": "0"}"#).unwrap();
    let mut state = state(dir.path());
    open(&mut state, Page::Graphics);
    // A checked box unchecks.
    let sent = messages(&state, "Allow HDR output");
    assert!(
        matches!(&sent[..], [Message::Set(key, value)] if key == "hdr" && *value == serde_json::json!(false)),
        "{sent:?}"
    );
    let env = game_environment(
        state.store.app(),
        &state.paths,
        "en",
        &indexmap::IndexMap::new(),
        true,
    );
    assert_eq!(env["SANCTUARY_HDR"], "1");
}

#[test]
fn ini_migrates_legacy_display_preferences_and_then_takes_precedence() {
    let dir = tempfile::tempdir().unwrap();
    std::fs::write(
        dir.path().join("settings.json"),
        r#"{"monitor": "2", "hdr": true}"#,
    )
    .unwrap();
    let mut state = state(dir.path());
    let ini = state.paths.ini_path();
    assert_eq!(
        (state.store.text("monitor"), state.store.text("hdr")),
        ("2".to_string(), "1".to_string())
    );
    // The Graphics page shows the ini's value: a checked box unchecks.
    open(&mut state, Page::Graphics);
    let sent = messages(&state, "Allow HDR output");
    assert!(
        matches!(&sent[..], [Message::Set(key, value)] if key == "hdr" && *value == serde_json::json!(false)),
        "{sent:?}"
    );
    std::fs::write(&ini, "monitor=0\nhdr=0\n").unwrap();
    let mut state = common::state(dir.path());
    assert_eq!(
        (state.store.text("monitor"), state.store.text("hdr")),
        ("0".to_string(), "0".to_string())
    );
    open(&mut state, Page::Graphics);
    let sent = messages(&state, "Allow HDR output");
    assert!(
        matches!(&sent[..], [Message::Set(key, value)] if key == "hdr" && *value == serde_json::json!(true)),
        "{sent:?}"
    );
}

#[test]
fn glyph_style_ini_overrides_legacy_launcher_choice() {
    let dir = tempfile::tempdir().unwrap();
    std::fs::write(
        dir.path().join("settings.json"),
        r#"{"glyph_style": "nintendo"}"#,
    )
    .unwrap();
    let state = common::state(dir.path());
    assert_eq!(state.store.text("glyph_style"), "nintendo");
    std::fs::write(state.paths.ini_path(), "glyph_style=keyboard\n").unwrap();
    let state = common::state(dir.path());
    assert_eq!(state.store.text("glyph_style"), "keyboard");
}

/// The window of `state` at `width` × `height` with `page` open, once the drawer has stopped.
fn settled(
    state: &mut app::State,
    page: Page,
    width: f32,
    height: f32,
) -> iced_test::simulator::Simulator<'_, Message> {
    open(state, page);
    let _ = app::update(
        state,
        Message::Tick(iced::time::Instant::now() + std::time::Duration::from_secs(1)),
    );
    iced_test::simulator::Simulator::with_size(
        iced::Settings::default(),
        iced::Size::new(width, height),
        app::view(state),
    )
}

/// The bounds of the control of the row named `label`.
fn control(ui: &mut iced_test::simulator::Simulator<'_, Message>, label: &str) -> iced::Rectangle {
    ui.find(iced_test::selector::id(widgets::row::control_id(label)))
        .unwrap_or_else(|error| panic!("control of {label:?}: {error:?}"))
        .bounds()
}

#[test]
fn every_row_control_ends_on_the_same_right_edge() {
    // Mockup `.row { grid-template-columns: 1fr auto }`: drop-downs, check boxes and buttons end
    // where the row ends, whatever the name before them; right to left they start where it starts.
    for lang in ["en", "pt", "ar"] {
        let dir = tempfile::tempdir().unwrap();
        let mut state = state(dir.path());
        state.lang = lang.into();
        state.rtl = lang == "ar";
        let tr = |english: &str| sanctuaryport_launcher::core::i18n::tr(lang, english);
        let labels = [
            tr("Display mode"),
            tr("Allow HDR output"),
            format!("{}  ·  {}", tr("Anisotropic filtering"), tr("restart")),
            tr("Show performance overlay"),
            tr("F12 in the game"),
        ];
        let mut ui = settled(&mut state, Page::Graphics, 1280.0, 800.0);
        let edges: Vec<(String, f32)> = labels
            .iter()
            .map(|label| {
                let bounds = control(&mut ui, label);
                let edge = if lang == "ar" {
                    bounds.x
                } else {
                    bounds.x + bounds.width
                };
                (label.clone(), edge)
            })
            .collect();
        let first = edges[0].1;
        assert!(
            edges.iter().all(|(_, edge)| (edge - first).abs() < 0.5),
            "{lang}: {edges:?}"
        );
    }
}

#[test]
fn a_drop_down_keeps_the_width_of_its_longest_option() {
    // The user's screenshot: "RAM do sistema (com cache, padrão)" wrapped inside its drop-down,
    // which got what a fixed-width name left of the row. The drop-down takes its own width and
    // the name what is left; the note spans the row under both, from the name's edge.
    let dir = tempfile::tempdir().unwrap();
    let mut state = state(dir.path());
    state.lang = "pt".into();
    state.gpus = gpu_rows(&ARC_LAPTOP);
    let tr = |english: &str| sanctuaryport_launcher::core::i18n::tr("pt", english);
    let restart = |english: &str| format!("{}  ·  {}", tr(english), tr("restart"));
    let rows = performance::rows(&state, "pt");
    let mut ui = settled(&mut state, Page::Performance, 1280.0, 800.0);
    // Mockup `.sel { font-size: 12.5px }` (R98).
    let size = 12.5;
    for (key, name, hint) in [
        (
            "stream_memory",
            restart("Stream buffer memory"),
            tr("System RAM can improve FPS on NVIDIA laptops. Choose GPU memory to restore the previous behavior. Applies on the next game launch."),
        ),
        // The longest option of the page.
        (
            "no_msaa",
            restart("Game multisampling"),
            tr(performance::NO_MSAA_HINT),
        ),
    ] {
        let row = rows.iter().find(|row| row.key == key).unwrap();
        let longest = row
            .pairs
            .iter()
            .map(|(_, label)| common::natural_size(label, None, size).width)
            .fold(0.0, f32::max);
        let control = control(&mut ui, &name);
        // iced sizes a pick list to its longest label, the arrow (the text size), the padding's
        // left side and then the padding (10 px a side) around it all, on one line.
        let natural = longest + size + 30.0;
        assert!(
            (control.width - natural).abs() < 0.5,
            "{key}: {} wide, its longest option at {size} px needs {natural}",
            control.width,
        );
        let label = ui.find(name.as_str()).unwrap().bounds();
        let note = ui.find(hint.as_str()).unwrap().bounds();
        assert!((note.x - label.x).abs() < 0.5, "{key}: {note:?}, {label:?}");
        assert!(
            (note.x + note.width - (control.x + control.width)).abs() < 0.5,
            "{key}: the note {note:?} does not span the row to {control:?}"
        );
    }
}

#[test]
fn the_multisampling_name_keeps_clear_of_its_drop_down() {
    // N1 (R98): at 1280 × 800 the long "Automatic (NVIDIA GeForce RTX 4060 Laptop GPU: on)" option
    // left the row's name a sliver, and the name's words ran over the drop-down.
    let dir = tempfile::tempdir().unwrap();
    let mut state = state(dir.path());
    state.lang = "pt".into();
    state.gpus = gpu_rows(&ARC_LAPTOP);
    let tr = |english: &str| sanctuaryport_launcher::core::i18n::tr("pt", english);
    let name = format!("{}  ·  {}", tr("Game multisampling"), tr("restart"));
    let mut ui = settled(&mut state, Page::Performance, 1280.0, 800.0);
    let control = control(&mut ui, &name);
    let label = ui.find(name.as_str()).unwrap().bounds();
    assert!(
        label.x + label.width <= control.x + 0.5,
        "the name {label:?} runs over the drop-down {control:?}"
    );
    assert!(label.width >= 120.0 - 0.5, "the name is {label:?}");
}

#[test]
fn monitor_row_lists_the_probe_indices() {
    let dir = tempfile::tempdir().unwrap();
    let mut state = state(dir.path());
    let displays = two_displays();
    let _ = app::update(&mut state, Message::MonitorsListed(displays.clone()));
    assert_eq!(
        state.monitor_pairs,
        vec![
            (String::new(), "Primary (default)".to_string()),
            ("0".to_string(), display_label(&displays[0])),
            ("1".to_string(), display_label(&displays[1])),
        ]
    );
    let second = state.monitor_pairs[2].1.clone();
    let pick = state
        .monitor_pairs
        .iter()
        .find(|(_, label)| *label == second)
        .map(|(value, _)| Message::Set("monitor".into(), value.clone().into()))
        .unwrap();
    let _ = app::update(&mut state, pick);
    assert_eq!(state.store.text("monitor"), "1");
}

#[test]
fn graphics_page_offers_the_choice() {
    let dir = tempfile::tempdir().unwrap();
    let mut state = state(dir.path());
    open(&mut state, Page::Graphics);
    // The page lists the probe's monitors, a gap in the indices stays a gap.
    let gap = parse_display_list("0\tA\t1\t0\t0\t1920\t1080\n2\tB\t0\t0\t0\t1280\t720\n");
    let _ = app::update(&mut state, Message::MonitorsListed(gap));
    let values: Vec<&str> = state
        .monitor_pairs
        .iter()
        .map(|(v, _)| v.as_str())
        .collect();
    assert_eq!(values, ["", "0", "2"]);
    assert_eq!(state.monitor_pairs[1].1, "A · 1920 × 1080 · (primary)");
    assert_eq!(state.monitor_pairs[2].1, "B · 1280 × 720");

    // A saved choice comes back; one whose monitor is gone falls back to the primary default.
    state.store.set("monitor", "2".into());
    let _ = app::update(&mut state, Message::MonitorsListed(two_displays()));
    assert_eq!(state.store.text("monitor"), "");
    state.store.set("monitor", "1".into());
    let _ = app::update(&mut state, Message::MonitorsListed(two_displays()));
    assert_eq!(state.store.text("monitor"), "1");
    // The choice is the ini's `monitor`, the key the in-game menu shares; settings.json keeps its own.
    assert_eq!(state.store.ini().values["monitor"], "1");
    assert_eq!(state.store.json("monitor"), Some(&serde_json::json!("")));
    let _ = app::update(&mut state, Message::MonitorsListed(Vec::new()));
    assert_eq!(state.monitor_pairs.len(), 1);
    assert_eq!(state.store.text("monitor"), "");

    // The screenshots button opens <saves>/screenshots, made first when missing. The page is
    // taller than the default window since the Effects section (the drawer scrolls).
    let mut ui = iced_test::simulator::Simulator::with_size(
        iced::Settings::default(),
        iced::Size::new(1280.0, 4000.0),
        app::view(&state),
    );
    ui.click("Open the screenshots folder").unwrap();
    let sent: Vec<Message> = ui.into_messages().collect();
    let expected = state.paths.user_dir("").join("screenshots");
    assert!(matches!(sent.as_slice(), [Message::OpenFolder(path)] if *path == expected));
}

#[test]
fn open_makes_a_missing_folder_first() {
    let dir = tempfile::tempdir().unwrap();
    let mut state = state(dir.path());
    // Python `open_path`: `mkdir` before the file manager opens it.
    let missing = dir.path().join("user").join("screenshots");
    let task = app::update(&mut state, Message::OpenFolder(missing.clone()));
    assert!(missing.is_dir());
    assert_eq!(task.units(), 1, "then it opens");
}

fn set(state: &mut app::State, key: &str, value: &str) {
    let _ = app::update(state, Message::Set(key.into(), value.into()));
}

fn values(pairs: &[(String, String)]) -> Vec<&str> {
    pairs.iter().map(|(value, _)| value.as_str()).collect()
}

#[test]
fn aspect_follows_and_filters_the_render_resolution() {
    // SanctuaryPort: Diablo III's resolution patches are 16:9 and the 21:9 ultrawide ones.
    let dir = tempfile::tempdir().unwrap();
    let mut state = state(dir.path());
    assert_eq!(state.store.text("render_resolution"), "native");
    assert_eq!(state.aspect, "16:9");
    assert_eq!(performance::aspect_options(&state), ["16:9", "21:9"]);
    assert_eq!(
        values(&performance::aspect_resolution_options(&state)),
        ["native", "480p", "720p", "900p", "1440p", "1800p", "4K"]
    );
    set(&mut state, "render_resolution", "1440p");
    assert_eq!(state.aspect, "16:9");
    // Choosing a 21:9 size moves the aspect box and lists the 21:9 sizes.
    set(&mut state, "render_resolution", "3440x1440");
    assert_eq!(state.aspect, "21:9");
    assert_eq!(
        values(&performance::aspect_resolution_options(&state)),
        ["2560x1080", "3440x1440", "3840x1600"]
    );
}

#[test]
fn native_is_translated_in_the_resolution_lists() {
    let dir = tempfile::tempdir().unwrap();
    let mut state = state(dir.path());
    let native = |pairs: Vec<(String, String)>| {
        pairs
            .into_iter()
            .find(|(value, _)| value == "native")
            .unwrap()
            .1
    };
    state.lang = "en".into();
    assert_eq!(
        native(performance::resolution_options(&state)),
        "Native · 1920 × 1080 · 16:9"
    );
    assert_eq!(
        native(performance::aspect_resolution_options(&state)),
        "Native · 1920 × 1080"
    );
    // The Python passes the labels through `_()`; the word alone has its Portuguese too.
    state.lang = "pt".into();
    assert_eq!(
        native(performance::resolution_options(&state)),
        "Nativa · 1920 × 1080 · 16:9"
    );
    assert_eq!(
        native(performance::aspect_resolution_options(&state)),
        "Nativa · 1920 × 1080"
    );
    // A whole label of the Python's tables still wins.
    state.lang = "zh".into();
    assert_eq!(
        native(performance::resolution_options(&state)),
        "原生 · 1920 × 1080 · 16:9"
    );
}

#[test]
fn no_msaa_auto_label_names_the_card_and_the_decision() {
    let dir = tempfile::tempdir().unwrap();
    let mut state = state(dir.path());
    state.gpus = gpu_rows(&ARC_LAPTOP);
    let options = performance::no_msaa_label(&state, "en");
    assert_eq!(values(&options), ["", "1", "0"]);
    assert_eq!(
        options[0].1,
        "Automatic (NVIDIA GeForce RTX 4060 Laptop GPU: on)"
    );
    assert_eq!(options[1].1, "Off (fixes black lines)");
    assert_eq!(options[2].1, "On");
    state.store.set("gpu_id", "0".into());
    assert_eq!(
        performance::no_msaa_label(&state, "en")[0].1,
        "Automatic (Intel(R) Arc(TM) Graphics: off)"
    );
    // An old tool, none at all, or a spelling this launcher does not know: the plain label.
    state.gpus = gpu_rows(&[("0", "AMD Radeon RX 6600", "discrete", "", "0")]);
    assert_eq!(performance::no_msaa_label(&state, "en")[0].1, "Automatic");
    state.gpus = Vec::new();
    state.store.set("gpu_id", "".into());
    assert_eq!(performance::no_msaa_label(&state, "en")[0].1, "Automatic");
    state.gpus = gpu_rows(&[("0", "x", "discrete", "OFF", "1")]);
    assert_eq!(performance::no_msaa_label(&state, "en")[0].1, "Automatic");
    state.gpus = gpu_rows(&ARC_LAPTOP);
    state.store.set("gpu_id", "0".into());
    assert_eq!(
        performance::no_msaa_label(&state, "pt")[0].1,
        "Automático (Intel(R) Arc(TM) Graphics: desligado)"
    );
    state.store.set("gpu_id", "1".into());
    assert_eq!(
        performance::no_msaa_label(&state, "pt")[0].1,
        "Automático (NVIDIA GeForce RTX 4060 Laptop GPU: ligado)"
    );
}

/// The labels the simulator finds on the rendered page, in a window tall enough for all rows.
fn page_has(state: &app::State, labels: &[&str]) {
    let mut ui = iced_test::simulator::Simulator::with_size(
        iced::Settings::default(),
        iced::Size::new(1280.0, 4000.0),
        app::view(state),
    );
    for label in labels {
        ui.find(*label)
            .unwrap_or_else(|error| panic!("find {label:?}: {error:?}"));
    }
}

#[test]
fn performance_page_offers_the_choice() {
    let dir = tempfile::tempdir().unwrap();
    let mut state = state(dir.path());
    state.lang = "en".into();
    state.gpus = gpu_rows(&ARC_LAPTOP);
    open(&mut state, Page::Performance);
    assert_eq!(state.store.text("no_msaa"), "");
    // The page shows the rows with their hints. The text inside a closed pick_list is not a
    // widget the simulator can find, so the options are checked through `rows` below.
    page_has(
        &state,
        &[
            "Game multisampling  ·  restart",
            performance::NO_MSAA_HINT,
            "Graphics card  ·  restart",
        ],
    );
    // iced_test cannot open a pick_list: the page builds its drop-downs from `rows`, so those
    // are what the page offers.
    let rows = performance::rows(&state, "en");
    let row = |key: &str| rows.iter().find(|row| row.key == key).unwrap().clone();
    let boxes = row("no_msaa");
    assert_eq!(boxes.pairs, performance::no_msaa_label(&state, "en"));
    assert_eq!(boxes.current, "");
    let pick = |row: &performance::SelectRow, label: &str| {
        let (value, _) = row.pairs.iter().find(|(_, l)| l == label).unwrap();
        Message::Set(row.key.into(), value.clone().into())
    };
    let _ = app::update(&mut state, pick(&boxes, "Off (fixes black lines)"));
    assert_eq!(state.store.text("no_msaa"), "1");
    // The graphics card list offers Automatic and both cards, in the order of the probe.
    let cards = row("gpu_id");
    let labels: Vec<&str> = cards.pairs.iter().map(|(_, l)| l.as_str()).collect();
    assert_eq!(
        labels,
        [
            "Automatic",
            "Intel(R) Arc(TM) Graphics",
            "NVIDIA GeForce RTX 4060 Laptop GPU"
        ]
    );
    // Choosing a card relabels Automatic at once and keeps the multisampling choice.
    let _ = app::update(&mut state, pick(&cards, "Intel(R) Arc(TM) Graphics"));
    assert_eq!(state.store.text("gpu_id"), "0");
    let boxes = performance::rows(&state, "en")
        .into_iter()
        .find(|row| row.key == "no_msaa")
        .unwrap();
    assert_eq!(
        boxes.pairs[0].1,
        "Automatic (Intel(R) Arc(TM) Graphics: off)"
    );
    assert_eq!(boxes.current, "1");
    assert_eq!(state.store.text("no_msaa"), "1");
    // The Portuguese page shows the translated title, hint and label.
    state.lang = "pt".into();
    let hint = sanctuaryport_launcher::core::i18n::tr("pt", performance::NO_MSAA_HINT);
    assert_ne!(hint, performance::NO_MSAA_HINT);
    page_has(&state, &["Multisampling do jogo  ·  reiniciar", &hint]);
    let boxes = performance::rows(&state, "pt")
        .into_iter()
        .find(|row| row.key == "no_msaa")
        .unwrap();
    assert_eq!(
        boxes.pairs[0].1,
        "Automático (Intel(R) Arc(TM) Graphics: desligado)"
    );
}

// --- Game page (Task 26) ---

use sanctuaryport_launcher::core::i18n::tr;
use sanctuaryport_launcher::core::options::LANGUAGES;
use sanctuaryport_launcher::ui::home;
use sanctuaryport_launcher::ui::pages::game::{self, BackupMessage};
use sanctuaryport_launcher::ui::widgets::{ModalKind, ModalMessage};

#[test]
fn choosing_the_game_folder_refreshes_title_and_patches() {
    let dir = tempfile::tempdir().unwrap();
    let mut state = state(dir.path());
    assert_eq!(state.patches.title_id, None);
    let dump = dump_with_patches(dir.path());
    browsed(&mut state, "game_dir", &dump);
    assert_eq!(state.store.text("game_dir"), dump.display().to_string());
    assert_eq!(state.patches.title_id.as_deref(), Some("CUSA00243"));
    let names: Vec<&str> = state
        .patches
        .selectable
        .iter()
        .map(|m| m.name.as_str())
        .collect();
    assert_eq!(names, ["Fix Alpha"]);
    assert!(state.patches.chosen.contains("Fix Alpha"));
    assert_eq!(state.patches.version, "01.47");
    assert_eq!(state.patches.xml_name, "Diablo_III.xml");
    assert_eq!(home::chips(&state)[1], "✓ CUSA00243 · v01.47");
    // Cancelling the dialog changes nothing.
    let _ = app::update(&mut state, Message::Browsed("game_dir".into(), None));
    assert_eq!(state.store.text("game_dir"), dump.display().to_string());
}

#[test]
fn a_long_folder_stays_on_one_line_in_its_field() {
    // The user's screenshot: the path broke into five lines. Ruling R89: an editable one-line
    // field holds it (the Python's Entry) and scrolls a long one, between the name and the buttons.
    let dir = tempfile::tempdir().unwrap();
    let mut state = state(dir.path());
    state.lang = "en".into();
    let folder = r"C:\Users\Someone\Documents\My Games\Diablo III\Diablo.III.Eternal.Collection.PS4-CUSA00243\CUSA00243";
    state.store.set("game_dir", folder.into());
    let mut ui = settled(&mut state, Page::Game, 1280.0, 800.0);
    let field = ui.find(folder).unwrap();
    assert!(
        matches!(field, iced_test::selector::Text::Input { .. }),
        "{field:?}"
    );
    let field = field.bounds();
    let line = common::natural_size(folder, None, 16.0);
    assert!(
        line.width > field.width,
        "the field is {} wide",
        field.width
    );
    assert!(field.height < 2.0 * line.height, "{} high", field.height);
    // Mockup `.folder { grid-template-columns: 150px 1fr auto auto; gap: 8px }`.
    let name = ui.find("Game folder").unwrap().bounds();
    let browse = ui.find("Browse…").unwrap().bounds();
    assert!(
        (field.x - (name.x + 150.0 + 8.0)).abs() < 0.5,
        "name {name:?}, field {field:?}"
    );
    // Browse pads its label by 10.
    assert!(
        (browse.x - 10.0 - (field.x + field.width + 8.0)).abs() < 0.5,
        "field {field:?}, Browse {browse:?}"
    );
}

#[test]
fn typing_a_folder_sets_it_like_browse() {
    let dir = tempfile::tempdir().unwrap();
    let mut state = state(dir.path());
    state.lang = "en".into();
    let dump = dump_with_patches(dir.path()).display().to_string();
    let mut ui = settled(&mut state, Page::Game, 1280.0, 800.0);
    ui.click(iced_test::selector::id(widgets::folder::input_id(
        "game_dir",
    )))
    .unwrap();
    ui.typewrite(&dump);
    let sent: Vec<Message> = ui.into_messages().collect();
    assert!(!sent.is_empty());
    for message in sent {
        let _ = app::update(&mut state, message);
    }
    assert_eq!(state.store.text("game_dir"), dump);
    // The refresh of a browsed folder: the Home reads the dump.
    assert_eq!(
        sanctuaryport_launcher::ui::home::chips(&state)[1],
        "✓ CUSA00243 · v01.47"
    );
}

/// Like `click`, in a window tall enough to show the whole page (the drawer scrolls).
fn click_tall(state: &mut app::State, label: &str) {
    let mut ui = iced_test::simulator::Simulator::with_size(
        iced::Settings::default(),
        iced::Size::new(1280.0, 4000.0),
        app::view(state),
    );
    ui.click(label)
        .unwrap_or_else(|error| panic!("click {label:?}: {error:?}"));
    let sent: Vec<Message> = ui.into_messages().collect();
    for message in sent {
        let _ = app::update(state, message);
    }
}

#[test]
fn restore_asks_first_and_is_blocked_while_running() {
    let dir = tempfile::tempdir().unwrap();
    let mut state = state(dir.path());
    state.lang = "en".into();
    let user = state.paths.user_dir("");
    let backup = user.join("savedata_backups").join("20261009-180000");
    std::fs::create_dir_all(&backup).unwrap();
    std::fs::write(backup.join("data.bin"), b"old").unwrap();
    std::fs::create_dir_all(user.join("savedata")).unwrap();
    std::fs::write(user.join("savedata").join("data.bin"), b"new").unwrap();

    // Nothing chosen: nothing happens.
    let _ = app::update(&mut state, Message::Backup(BackupMessage::Restore));
    assert!(state.modal.is_none());

    let _ = app::update(&mut state, Message::Backup(BackupMessage::Refresh));
    assert_eq!(state.backups, ["20261009-180000"]);
    assert_eq!(state.backup_choice, "20261009-180000");
    // An idle game does not block it; `ui_play.rs` covers a running one.
    assert!(!state.game.is_live());

    let _ = app::update(&mut state, Message::Backup(BackupMessage::Restore));
    let asked = state.modal.clone().unwrap();
    assert!(matches!(
        &asked.kind,
        ModalKind::YesNo {
            yes: Message::Backup(BackupMessage::RestoreConfirmed),
            no: Message::Modal(ModalMessage::Dismiss)
        }
    ));
    assert!(asked.text.starts_with("Replace the current saves with the backup 20261009-180000? The current saves are backed up first."));
    assert!(asked.text.contains(&backup.display().to_string()));
    // Nothing was restored yet.
    assert_eq!(
        std::fs::read(user.join("savedata/data.bin")).unwrap(),
        b"new"
    );

    // Yes closes the dialog and starts the restore in the background.
    let _ = app::update(&mut state, Message::Backup(BackupMessage::RestoreConfirmed));
    assert!(state.modal.is_none());
    // The background work runs on its own thread: wait for the saves to come back.
    let saves = user.join("savedata/data.bin");
    for _ in 0..200 {
        if std::fs::read(&saves).is_ok_and(|bytes| bytes == b"old") {
            break;
        }
        std::thread::sleep(std::time::Duration::from_millis(25));
    }
    assert_eq!(std::fs::read(&saves).unwrap(), b"old");
    let _ = app::update(
        &mut state,
        Message::Backup(BackupMessage::Restored(Ok(Some("20261009-181000".into())))),
    );
    let done = state.modal.clone().unwrap();
    assert!(matches!(done.kind, ModalKind::Info));
    assert_eq!(
        done.text,
        "Saves restored from 20261009-180000. Previous saves: 20261009-181000."
    );
    let _ = app::update(
        &mut state,
        Message::Backup(BackupMessage::Restored(Err("disk full".into()))),
    );
    let failed = state.modal.clone().unwrap();
    assert!(matches!(failed.kind, ModalKind::Error));
    assert_eq!(failed.text, "Could not restore: disk full");
    assert!(matches!(
        game::restore_saves(&user, "nope"),
        Err(text) if text.contains("nope")
    ));
}

#[test]
fn brazilian_portuguese_is_offered_with_its_system_id() {
    let offered = LANGUAGES
        .iter()
        .find(|choice| choice.value == "17")
        .unwrap();
    assert_eq!(offered.en, "Portuguese (Brazil)");
}

#[test]
fn choosing_brazilian_portuguese_starts_the_game_with_id_17() {
    // The Language row stores the system id the game is started with (Task 33 passes it on).
    let dir = tempfile::tempdir().unwrap();
    let mut state = state(dir.path());
    let pick = select_message("language", LANGUAGES, "en", "Portuguese (Brazil)").unwrap();
    let _ = app::update(&mut state, pick);
    assert_eq!(state.store.text("language"), "17");
}

#[test]
fn brazilian_portuguese_label_is_translated_in_portuguese_and_spanish() {
    assert_eq!(tr("pt", "Portuguese (Brazil)"), "Português (Brasil)");
    assert_eq!(tr("es", "Portuguese (Brazil)"), "Portugués (Brasil)");
    assert_eq!(tr("id", "Portuguese (Brazil)"), "Portugis (Brasil)");
}

// --- Trophies page (Task 27) ---

use common::TROPHY_COUNT;
use sanctuaryport_launcher::ui::pages::trophies::{Filter, TrophiesMessage};

fn tall_has(state: &app::State, label: &str) -> bool {
    let mut ui = iced_test::simulator::Simulator::with_size(
        iced::Settings::default(),
        iced::Size::new(1280.0, 6000.0),
        app::view(state),
    );
    ui.find(label).is_ok()
}

#[test]
fn trophies_page_lists_35_cards_from_a_test_package() {
    let dir = tempfile::tempdir().unwrap();
    let mut state = state_with_trophies(dir.path());
    open(&mut state, Page::Trophies);
    load_trophies(&mut state);
    assert_eq!(state.trophies.icons.len(), TROPHY_COUNT);
    assert_eq!(state.trophies.locked_icons.len(), TROPHY_COUNT);
    assert_eq!(state.trophies.tiers.len(), TROPHY_COUNT);
    assert_eq!(
        state.trophies.unlocked.iter().copied().collect::<Vec<_>>(),
        [0, 3]
    );
    assert!(!state.trophies.loading);
    assert!(tall_has(&state, "2 of 35 unlocked"));
    // Names and descriptions from the built-in Diablo III list (ids 0 and 34 of 0..35).
    assert!(tall_has(&state, "Diablo III: Reaper of Souls Platinum Trophy"));
    assert!(tall_has(&state, "Demolition Derby"));
    assert!(tall_has(&state, "Get a destruction callout for breaking 50 objects."));
    assert!(tall_has(&state, "Diablo III: Reaper of Souls"));
    assert!(tall_has(
        &state,
        "Only trophies unlocked with this port are listed; the ones earned on a PlayStation are not known to it."
    ));
}

#[test]
fn trophies_page_without_a_package_says_so() {
    let dir = tempfile::tempdir().unwrap();
    let mut state = state(dir.path());
    state.lang = "en".into();
    open(&mut state, Page::Trophies);
    load_trophies(&mut state);
    assert!(state.trophies.icons.is_empty());
    assert!(tall_has(
        &state,
        "No trophy list: choose the game folder on the Game page (its sce_sys/trophy/trophy00.trp is read)."
    ));
}

#[test]
fn filter_unlocked_hides_locked() {
    let dir = tempfile::tempdir().unwrap();
    let mut state = state_with_trophies(dir.path());
    open(&mut state, Page::Trophies);
    load_trophies(&mut state);
    let (first, second, other) = (
        "Diablo III: Reaper of Souls Platinum Trophy".to_string(),
        "Up All Night To Get Lucky".to_string(),
        "Deep Pockets".to_string(),
    );
    let _ = app::update(
        &mut state,
        Message::Trophies(TrophiesMessage::Filter(Filter::Unlocked)),
    );
    assert!(tall_has(&state, &first) && tall_has(&state, &second));
    assert!(!tall_has(&state, &other));
    let _ = app::update(
        &mut state,
        Message::Trophies(TrophiesMessage::Filter(Filter::Locked)),
    );
    assert!(!tall_has(&state, &first) && !tall_has(&state, &second));
    assert!(tall_has(&state, &other));
    // A reload keeps the chosen filter.
    load_trophies(&mut state);
    assert_eq!(state.trophies.filter, Filter::Locked);
}

#[test]
fn reset_asks_and_relocks() {
    let dir = tempfile::tempdir().unwrap();
    let mut state = state_with_trophies(dir.path());
    open(&mut state, Page::Trophies);
    // Nothing unlocked is known before the first load.
    let _ = app::update(&mut state, Message::Trophies(TrophiesMessage::Reset));
    let none = state.modal.clone().unwrap();
    assert!(matches!(none.kind, ModalKind::Info));
    assert_eq!(none.text, "No trophy is unlocked.");

    load_trophies(&mut state);
    let _ = app::update(&mut state, Message::Trophies(TrophiesMessage::Reset));
    let asked = state.modal.clone().unwrap();
    assert!(matches!(
        &asked.kind,
        ModalKind::YesNo {
            yes: Message::Trophies(TrophiesMessage::ResetConfirmed),
            no: Message::Modal(ModalMessage::Dismiss)
        }
    ));
    assert_eq!(
        asked.text,
        "Lock all 2 unlocked trophies again? A copy is kept as trophies.txt.bak."
    );
    let dat = state.paths.user_dir("").join("trophies.txt");
    assert!(dat.is_file());

    let _ = app::update(
        &mut state,
        Message::Trophies(TrophiesMessage::ResetConfirmed),
    );
    assert!(state.modal.is_none());
    // The reset runs on its own thread: wait for the file to move.
    for _ in 0..200 {
        if !dat.exists() {
            break;
        }
        std::thread::sleep(std::time::Duration::from_millis(25));
    }
    assert!(!dat.exists());
    assert!(dat.with_file_name("trophies.txt.bak").is_file());
    let _ = app::update(
        &mut state,
        Message::Trophies(TrophiesMessage::ResetDone(Ok(2))),
    );
    load_trophies(&mut state);
    assert!(state.trophies.unlocked.is_empty());
    assert!(tall_has(&state, "0 of 35 unlocked"));

    let _ = app::update(
        &mut state,
        Message::Trophies(TrophiesMessage::ResetDone(Err("disk full".into()))),
    );
    let failed = state.modal.clone().unwrap();
    assert!(matches!(failed.kind, ModalKind::Error));
    assert_eq!(failed.text, "Could not reset the trophies: disk full");
}

#[test]
fn opening_the_page_and_the_buttons_start_a_load() {
    let dir = tempfile::tempdir().unwrap();
    let mut state = state(dir.path());
    state.lang = "en".into();
    open(&mut state, Page::Trophies);
    assert!(state.trophies.loading);
    load_trophies(&mut state);
    assert!(!state.trophies.loading);
    click(&mut state, "Refresh");
    assert!(state.trophies.loading);
    assert!(messages(&state, "Reset trophies")
        .iter()
        .any(|m| matches!(m, Message::Trophies(TrophiesMessage::Reset))));
}

// --- Controls page (Task 28) ---

use iced::keyboard::key::Named;
use iced::keyboard::{Key, Location, Modifiers};
use iced::mouse;
use sanctuaryport_launcher::core::controls::{Kind, Slot, PAD_STYLES};
use sanctuaryport_launcher::ui::pages::controls::{
    self, ControlsMessage, ControlsView, Device, MouseInput, PadChoice, CAPTURE_SECONDS,
};

fn controls_update(state: &mut app::State, message: ControlsMessage) {
    let _ = app::update(state, Message::Controls(message));
}

/// Clicks the key box of `button`: the page now waits for an input for it.
fn capture(state: &mut app::State, button: &str) {
    controls_update(state, ControlsMessage::Capture(button.into(), Slot::Main));
}

/// A key press as the keyboard subscription delivers it.
fn press_key(state: &mut app::State, key: Key, location: Location) {
    let _ = app::update(state, Message::Key(key, Modifiers::default(), location));
}

fn char_key(text: &str) -> Key {
    Key::Character(text.into())
}

/// Like `messages`, in a window tall enough to show the whole page (the drawer scrolls).
fn tall_messages(state: &app::State, label: &str) -> Vec<Message> {
    let mut ui = iced_test::simulator::Simulator::with_size(
        iced::Settings::default(),
        iced::Size::new(1280.0, 4000.0),
        app::view(state),
    );
    ui.click(label)
        .unwrap_or_else(|error| panic!("click {label:?}: {error:?}"));
    ui.into_messages().collect()
}

fn controls_state(dir: &std::path::Path) -> app::State {
    let mut state = state(dir);
    state.lang = "en".into();
    open(&mut state, Page::Controls);
    state
}

#[test]
fn pressed_keys_become_sdl_names() {
    let dir = tempfile::tempdir().unwrap();
    let mut state = controls_state(dir.path());
    // The Tk keysyms of the Python test (q, Q, 7, F12, space, Shift_L, Control_L, KP_5,
    // BackSpace) as iced reports those keys, then a few more of the mapping.
    let presses = [
        (char_key("q"), Location::Standard, "Q"),
        (char_key("Q"), Location::Standard, "Q"),
        (char_key("7"), Location::Standard, "7"),
        (Key::Named(Named::F12), Location::Standard, "F12"),
        (Key::Named(Named::Space), Location::Standard, "Space"),
        (Key::Named(Named::Shift), Location::Left, "Left Shift"),
        (Key::Named(Named::Control), Location::Left, "Left Ctrl"),
        (char_key("5"), Location::Numpad, "Keypad 5"),
        (
            Key::Named(Named::Backspace),
            Location::Standard,
            "Backspace",
        ),
        (Key::Named(Named::Enter), Location::Standard, "Return"),
        (Key::Named(Named::Enter), Location::Numpad, "Keypad Enter"),
        (Key::Named(Named::Alt), Location::Right, "Right Alt"),
        (Key::Named(Named::Escape), Location::Standard, "Escape"),
        (Key::Named(Named::ArrowLeft), Location::Standard, "Left"),
        (
            Key::Named(Named::ContextMenu),
            Location::Standard,
            "Application",
        ),
        (Key::Named(Named::NumLock), Location::Numpad, "Numlock"),
        // The numpad with Num Lock off reports its other names; SDL sees the same keys.
        (Key::Named(Named::Home), Location::Numpad, "Keypad 7"),
        (Key::Named(Named::Delete), Location::Numpad, "Keypad ."),
        (char_key("="), Location::Standard, "Equals"),
        (char_key(","), Location::Standard, "Comma"),
        (char_key("-"), Location::Standard, "-"),
        (Key::Named(Named::F24), Location::Standard, "F24"),
    ];
    for (key, location, name) in presses {
        capture(&mut state, "cross");
        assert_eq!(state.controls.capturing.as_deref(), Some("cross"));
        press_key(&mut state, key.clone(), location);
        assert_eq!(state.controls.key["cross"], name, "{key:?} {location:?}");
        assert!(state.controls.capturing.is_none(), "{key:?}");
    }
    // While no box waits, the keyboard picture shows the key; it neither binds nor moves away.
    let bound = state.controls.key.clone();
    press_key(&mut state, Key::Named(Named::ArrowLeft), Location::Standard);
    assert_eq!(state.page, Page::Controls);
    assert_eq!(state.controls.key, bound);
}

#[test]
fn keys_that_cannot_be_bound() {
    let dir = tempfile::tempdir().unwrap();
    let mut state = controls_state(dir.path());
    let before = state.controls.key.clone();
    // Insert, Win_L, Win_R, Print, F25 and eacute of the Python test.
    let presses = [
        (Key::Named(Named::Insert), Location::Standard),
        (Key::Named(Named::Super), Location::Left),
        (Key::Named(Named::Super), Location::Right),
        (Key::Named(Named::PrintScreen), Location::Standard),
        (Key::Named(Named::F25), Location::Standard),
        (char_key("é"), Location::Standard),
    ];
    capture(&mut state, "cross");
    for (key, location) in presses {
        press_key(&mut state, key.clone(), location);
        // The box keeps waiting and nothing changed.
        assert_eq!(
            state.controls.capturing.as_deref(),
            Some("cross"),
            "{key:?}"
        );
        assert_eq!(state.controls.key, before, "{key:?}");
    }
    assert_eq!(state.page, Page::Controls);
}

#[test]
fn binding_a_used_key_swaps_the_buttons_in_the_view() {
    let dir = tempfile::tempdir().unwrap();
    let mut state = controls_state(dir.path());
    // Nobody has G by default: Square takes it.
    capture(&mut state, "square");
    press_key(&mut state, char_key("g"), Location::Standard);
    assert_eq!(state.controls.key["square"], "G");
    // Cross's box shows Space; clicking it waits for an input for Cross.
    click_tall(&mut state, "Space");
    assert_eq!(state.controls.capturing.as_deref(), Some("cross"));
    assert!(tall_has(&state, "Press a key or mouse (5)"));
    press_key(&mut state, char_key("g"), Location::Standard);
    assert_eq!(state.controls.key["cross"], "G");
    assert_eq!(state.controls.key["square"], "Space");
    // The rendered boxes swapped too: G is Cross's box, Space is Square's.
    assert!(matches!(
        tall_messages(&state, "G").as_slice(),
        [Message::Controls(ControlsMessage::Capture(button, Slot::Main))] if button == "cross"
    ));
    assert!(matches!(
        tall_messages(&state, "Space").as_slice(),
        [Message::Controls(ControlsMessage::Capture(button, Slot::Main))] if button == "square"
    ));
    let rows = controls::key_rows(&state, "en");
    let input = |button: &str| {
        rows.iter()
            .find(|row| row.button == button)
            .unwrap()
            .input
            .clone()
    };
    assert_eq!(
        (input("cross"), input("square")),
        ("G".into(), "Space".into())
    );
}

#[test]
fn device_is_its_own_choice() {
    assert_eq!(Device::ALL.map(Device::value), ["keyboard", "gamepad"]);
    let styles: Vec<&str> = PAD_STYLES.iter().map(|(style, _)| *style).collect();
    assert_eq!(styles, ["playstation", "xbox", "nintendo"]);

    let dir = tempfile::tempdir().unwrap();
    let mut state = controls_state(dir.path());
    // The first open looks for a gamepad once.
    assert!(state.controls.gamepad_probe_started);
    assert_eq!(state.controls.device, Device::Keyboard);
    assert!(!state.controls.device_chosen);
    // A gamepad found shows its table, but an untouched first choice stays automatic.
    controls_update(&mut state, ControlsMessage::GamepadDetected(true));
    assert_eq!(state.controls.device, Device::Gamepad);
    assert!(!state.controls.device_chosen);
    assert!(state.store.json("controls_device").is_none());
    assert!(tall_has(&state, "Gamepad Mapping"));
    assert!(!tall_has(&state, "Keyboard & Mouse Mapping"));

    // The device radio only chooses the table shown; the tables keep their inputs.
    capture(&mut state, "cross");
    press_key(&mut state, char_key("g"), Location::Standard);
    let (key, pad) = (state.controls.key.clone(), state.controls.pad.clone());
    click(&mut state, "Keyboard & mouse");
    assert_eq!(state.controls.device, Device::Keyboard);
    assert!(state.controls.device_chosen);
    assert_eq!(state.store.text("controls_device"), "keyboard");
    assert!(tall_has(&state, "Keyboard & Mouse Mapping"));
    assert!(!tall_has(&state, "Gamepad Mapping"));
    assert_eq!((&state.controls.key, &state.controls.pad), (&key, &pad));
    // Once chosen, a gamepad found later does not switch the page.
    controls_update(&mut state, ControlsMessage::GamepadDetected(true));
    assert_eq!(state.controls.device, Device::Keyboard);
    click(&mut state, "Gamepad");
    assert_eq!(state.controls.device, Device::Gamepad);
    assert_eq!(state.store.text("controls_device"), "gamepad");
    assert_eq!((&state.controls.key, &state.controls.pad), (&key, &pad));
    // The saved choice comes back and the probe does not run.
    let again = ControlsView::new(state.store.app());
    assert_eq!(again.device, Device::Gamepad);
    assert!(again.device_chosen);
    let mut reopened = state;
    reopened.controls = again;
    open(&mut reopened, Page::Controls);
    assert!(!reopened.controls.gamepad_probe_started);
}

#[test]
fn nintendo_names_follow_the_button_positions() {
    let names = controls::pad_labels("nintendo", "en");
    let label = |sdl: &str| {
        names
            .iter()
            .find(|(name, _)| name == sdl)
            .unwrap()
            .1
            .clone()
    };
    assert_eq!(
        (label("a"), label("b"), label("x"), label("y")),
        ("B".into(), "A".into(), "Y".into(), "X".into())
    );
    // A label maps back to one button.
    let unique: std::collections::BTreeSet<&String> = names.iter().map(|(_, l)| l).collect();
    assert_eq!(unique.len(), names.len());

    let dir = tempfile::tempdir().unwrap();
    let mut state = controls_state(dir.path());
    click(&mut state, "Gamepad");
    assert_eq!(state.controls.pad_style, "playstation");
    click_tall(&mut state, "Switch Pro");
    assert_eq!(state.controls.pad_style, "nintendo");
    assert_eq!(state.store.text("pad_style"), "nintendo");
    // The table relabels its boxes, the triggers too; Touchpad left stays on its combination.
    let rows = controls::pad_rows(&state, "en");
    let row = |button: &str| {
        rows.iter()
            .find(|row| row.button == button)
            .unwrap()
            .clone()
    };
    let PadChoice { pairs, current } = row("cross").choice;
    assert_eq!(current, "B");
    assert_eq!(pairs, controls::pad_labels("nintendo", "en"));
    assert_eq!(row("circle").choice.current, "A");
    assert_eq!(row("l2").choice.current, "ZL");
    assert_eq!(row("touchpad_left").choice.current, "L1 + Touchpad right");
    // Choosing a label stores its SDL name: Nintendo's A is the east button.
    let pick = controls::pad_pick("cross", &pairs, "A");
    assert!(matches!(
        &pick,
        Message::Controls(ControlsMessage::PadPick(button, name)) if button == "cross" && name == "b"
    ));
    let _ = app::update(&mut state, pick);
    assert_eq!(state.controls.pad["cross"], "b");
    assert_eq!(state.controls.pad["circle"], "b");
}

#[test]
fn menu_confirm_swap_option_shows_and_updates_in_controls_page() {
    let dir = tempfile::tempdir().unwrap();
    let mut state = controls_state(dir.path());
    click(&mut state, "Gamepad");
    assert_eq!(state.store.text("menu_confirm_swap"), "0");
    page_has(&state, &["Menu confirmation"]);
    let _ = app::update(
        &mut state,
        Message::Set("menu_confirm_swap".into(), serde_json::json!("1")),
    );
    assert_eq!(state.store.text("menu_confirm_swap"), "1");
}

#[test]
fn capture_takes_the_mouse_and_gives_up_in_time() {
    let pressed = |button| iced::Event::Mouse(mouse::Event::ButtonPressed(button));
    let wheel = |delta| iced::Event::Mouse(mouse::Event::WheelScrolled { delta });
    let bind = MouseInput::Bind;
    let inputs = [
        (pressed(mouse::Button::Left), Some(bind("Mouse Left"))),
        (pressed(mouse::Button::Middle), Some(bind("Mouse Middle"))),
        (pressed(mouse::Button::Right), Some(bind("Mouse Right"))),
        (pressed(mouse::Button::Back), Some(bind("Mouse Back"))),
        (pressed(mouse::Button::Forward), Some(bind("Mouse Forward"))),
        // A button without an input name: the box gives up (Python `clicked` -> `finish`).
        (pressed(mouse::Button::Other(8)), Some(MouseInput::GiveUp)),
        (
            wheel(mouse::ScrollDelta::Lines { x: 0.0, y: 1.0 }),
            Some(bind("Wheel Up")),
        ),
        (
            wheel(mouse::ScrollDelta::Pixels { x: 0.0, y: -40.0 }),
            Some(bind("Wheel Down")),
        ),
        (
            iced::Event::Mouse(mouse::Event::ButtonReleased(mouse::Button::Left)),
            None,
        ),
    ];
    for (event, name) in inputs {
        assert_eq!(controls::mouse_input(&event), name, "{event:?}");
    }

    let dir = tempfile::tempdir().unwrap();
    let mut state = controls_state(dir.path());
    capture(&mut state, "cross");
    controls_update(
        &mut state,
        ControlsMessage::Captured("cross".into(), Slot::Main, "Wheel Up".into()),
    );
    assert_eq!(state.controls.key["cross"], "Wheel Up");
    assert!(state.controls.capturing.is_none());
    // An input that arrives when no box waits binds nothing.
    controls_update(
        &mut state,
        ControlsMessage::Captured("cross".into(), Slot::Main, "Mouse Left".into()),
    );
    assert_eq!(state.controls.key["cross"], "Wheel Up");

    // The box counts down and gives up after CAPTURE_SECONDS.
    assert_eq!(CAPTURE_SECONDS, 5);
    capture(&mut state, "circle");
    let started = state.controls.capture_started.unwrap();
    let _ = app::update(
        &mut state,
        Message::Tick(started + std::time::Duration::from_millis(4900)),
    );
    assert_eq!(state.controls.capturing.as_deref(), Some("circle"));
    assert!(tall_has(&state, "Press a key or mouse (1)"));
    // While it waits, a click anywhere is only its input (the mouse subscription delivers it):
    // the button, the box or the tab under the cursor does not react.
    for label in ["Reset keyboard & mouse to defaults", "Wheel Up", "Graphics"] {
        let sent = tall_messages(&state, label);
        assert!(
            sent.iter().all(|message| matches!(message, Message::Noop)),
            "{label}: {sent:?}"
        );
    }
    let _ = app::update(
        &mut state,
        Message::Tick(started + std::time::Duration::from_secs(CAPTURE_SECONDS)),
    );
    assert!(state.controls.capturing.is_none());
    assert_eq!(state.controls.key["circle"], "Left Shift");
    assert!(tall_has(&state, "Left Shift"));
}

#[test]
fn reset_buttons_put_the_defaults_back() {
    let dir = tempfile::tempdir().unwrap();
    let mut state = controls_state(dir.path());
    let defaults = state.controls.key.clone();
    assert_eq!(defaults["square"], "Mouse Left");
    assert_eq!(defaults["r3"], "X");
    // The arrow puts one button back on its default, swapping with the button that had it.
    capture(&mut state, "cross");
    press_key(&mut state, char_key("h"), Location::Standard);
    assert_eq!(state.controls.key["cross"], "H");
    controls_update(&mut state, ControlsMessage::ResetKey("cross".into()));
    assert_eq!(state.controls.key["cross"], "Space");
    // L1 leaves Q, R1 takes it: L1's arrow gives L1 its Q back and R1 the old H.
    capture(&mut state, "l1");
    press_key(&mut state, char_key("h"), Location::Standard);
    capture(&mut state, "r1");
    press_key(&mut state, char_key("q"), Location::Standard);
    controls_update(&mut state, ControlsMessage::ResetKey("l1".into()));
    assert_eq!(state.controls.key["l1"], "Q");
    assert_eq!(state.controls.key["r1"], "H");
    click_tall(&mut state, "Reset keyboard & mouse to defaults");
    assert_eq!(state.controls.key, defaults);

    // The gamepad table goes back to the controller's own buttons.
    controls_update(
        &mut state,
        ControlsMessage::PadPick("cross".into(), "y".into()),
    );
    controls_update(&mut state, ControlsMessage::ResetTable(Kind::Pad));
    assert_eq!(state.controls.pad["cross"], "a");
    assert_eq!(state.controls.pad["l2"], "lefttrigger");
    assert_eq!(state.controls.pad["touchpad_left"], "");
}

/// `state` on the Controls page once the drawer has stopped (a key box's click restarts the clock).
fn settle(state: &mut app::State) {
    let _ = app::update(
        state,
        Message::Tick(iced::time::Instant::now() + std::time::Duration::from_secs(1)),
    );
}

#[test]
fn the_keyboard_reset_works_in_the_window_as_it_opens() {
    // Item 8 (adjustments 4): "Reset keyboard & mouse to defaults does nothing". At 1280 × 800,
    // with the page scrolled down to the button, a click puts a saved change back on its default
    // and Save turns on (the user's own files held no changed key: nothing to reset there).
    let dir = tempfile::tempdir().unwrap();
    std::fs::write(
        dir.path().join("settings.json"),
        r#"{"controls": {"key": {"cross": "H"}}}"#,
    )
    .unwrap();
    let mut state = controls_state(dir.path());
    settle(&mut state);
    assert_eq!(state.controls.key["cross"], "H");
    assert!(!sanctuaryport_launcher::ui::persist::dirty(&state));
    let mut ui = iced_test::simulator::Simulator::with_size(
        iced::Settings::default(),
        iced::Size::new(1280.0, 800.0),
        app::view(&state),
    );
    ui.point_at(iced::Point::new(300.0, 500.0));
    // Scroll until the button is in view, whatever the number of rows above it.
    let mut scrolls = 0;
    while ui.click("Reset keyboard & mouse to defaults").is_err() {
        scrolls += 1;
        assert!(scrolls <= 40, "the reset button never came into view");
        let _ = ui.simulate([iced::Event::Mouse(mouse::Event::WheelScrolled {
            delta: mouse::ScrollDelta::Pixels { x: 0.0, y: -300.0 },
        })]);
    }
    let sent: Vec<Message> = ui.into_messages().collect();
    for message in sent {
        let _ = app::update(&mut state, message);
    }
    assert_eq!(state.controls.key["cross"], "Space");
    assert!(sanctuaryport_launcher::ui::persist::dirty(&state));
}

/// Clicks the alternative box of `button`.
fn capture_alternative(state: &mut app::State, button: &str) {
    controls_update(
        state,
        ControlsMessage::Capture(button.into(), Slot::Alternative),
    );
}

fn capture_second_alternative(state: &mut app::State, button: &str) {
    controls_update(
        state,
        ControlsMessage::Capture(button.into(), Slot::SecondAlternative),
    );
}

fn key_row(state: &app::State, button: &str) -> controls::KeyRow {
    controls::key_rows(state, "en")
        .into_iter()
        .find(|row| row.button == button)
        .unwrap()
}

#[test]
fn an_alternative_box_binds_next_to_the_main_key() {
    let dir = tempfile::tempdir().unwrap();
    let mut state = controls_state(dir.path());
    assert!(state.controls.key_alt.is_empty());
    assert_eq!(key_row(&state, "square").alternative, "(alternative)");
    capture_alternative(&mut state, "square");
    assert_eq!(
        key_row(&state, "square").alternative,
        "Press a key or mouse (5)"
    );
    // Only the clicked box waits: the main one still shows its input.
    assert_eq!(key_row(&state, "square").input, "Mouse Left");
    press_key(&mut state, char_key("j"), Location::Standard);
    assert_eq!(state.controls.key_alt["square"], "J");
    assert_eq!(state.controls.key["square"], "Mouse Left");
    assert!(state.controls.capturing.is_none());
    let row = key_row(&state, "square");
    assert_eq!(
        (row.input.as_str(), row.alternative.as_str()),
        ("Mouse Left", "J")
    );
    assert!(row.has_alternative && !key_row(&state, "cross").has_alternative);
    // A mouse button or the wheel is an alternative too.
    capture_alternative(&mut state, "cross");
    controls_update(
        &mut state,
        ControlsMessage::Captured("cross".into(), Slot::Alternative, "Mouse Forward".into()),
    );
    assert_eq!(state.controls.key_alt["cross"], "Mouse Forward");
}

#[test]
fn the_third_box_binds_a_second_alternative_next_to_the_other_two() {
    let dir = tempfile::tempdir().unwrap();
    let mut state = controls_state(dir.path());
    assert_eq!(
        key_row(&state, "square").second_alternative,
        "(2nd alternative)"
    );
    capture_alternative(&mut state, "square");
    press_key(&mut state, char_key("j"), Location::Standard);
    capture_second_alternative(&mut state, "square");
    assert_eq!(
        key_row(&state, "square").second_alternative,
        "Press a key or mouse (5)"
    );
    // Only the clicked box waits.
    assert_eq!(key_row(&state, "square").alternative, "J");
    press_key(&mut state, char_key("k"), Location::Standard);
    let row = key_row(&state, "square");
    assert_eq!(
        (
            row.input.as_str(),
            row.alternative.as_str(),
            row.second_alternative.as_str()
        ),
        ("Mouse Left", "J", "K")
    );
    assert!(row.has_second_alternative && !key_row(&state, "cross").has_second_alternative);
    assert_eq!(state.controls.key_alt2["square"], "K");
    // A key of another box of the same button swaps with it.
    capture_second_alternative(&mut state, "square");
    press_key(&mut state, char_key("j"), Location::Standard);
    assert_eq!(state.controls.key_alt["square"], "K");
    assert_eq!(state.controls.key_alt2["square"], "J");
    // Its × drops it and leaves the other box alone; the row arrow and the reset drop both.
    controls_update(
        &mut state,
        ControlsMessage::Clear("square".into(), Slot::SecondAlternative),
    );
    assert!(state.controls.key_alt2.is_empty());
    assert_eq!(state.controls.key_alt["square"], "K");
    capture_second_alternative(&mut state, "square");
    press_key(&mut state, char_key("m"), Location::Standard);
    controls_update(&mut state, ControlsMessage::ResetKey("square".into()));
    assert!(state.controls.key_alt.is_empty() && state.controls.key_alt2.is_empty());
    capture_second_alternative(&mut state, "cross");
    press_key(&mut state, char_key("m"), Location::Standard);
    controls_update(&mut state, ControlsMessage::ResetTable(Kind::Key));
    assert!(state.controls.key_alt2.is_empty());
}

#[test]
fn an_input_for_the_other_box_of_the_row_binds_nothing() {
    let dir = tempfile::tempdir().unwrap();
    let mut state = controls_state(dir.path());
    capture(&mut state, "cross");
    controls_update(
        &mut state,
        ControlsMessage::Captured("cross".into(), Slot::Alternative, "K".into()),
    );
    assert!(state.controls.key_alt.is_empty());
    assert_eq!(state.controls.capturing.as_deref(), Some("cross"));
    assert_eq!(key_row(&state, "cross").alternative, "(alternative)");
}

#[test]
fn an_alternative_in_use_swaps_with_the_other_button() {
    let dir = tempfile::tempdir().unwrap();
    let mut state = controls_state(dir.path());
    capture_alternative(&mut state, "square");
    press_key(&mut state, char_key("j"), Location::Standard);
    // Cross takes J: Square, which had it, is left without an alternative.
    capture_alternative(&mut state, "cross");
    press_key(&mut state, char_key("j"), Location::Standard);
    assert_eq!(state.controls.key_alt.len(), 1);
    assert_eq!(state.controls.key_alt["cross"], "J");
    // R1's main key takes J from Cross's alternative, which gets the E that R1 had.
    capture(&mut state, "r1");
    press_key(&mut state, char_key("j"), Location::Standard);
    assert_eq!(state.controls.key["r1"], "J");
    assert_eq!(state.controls.key_alt["cross"], "E");
    assert_eq!(state.controls.key["cross"], "Space");
}

#[test]
fn clearing_the_alternative_leaves_the_main_key() {
    let dir = tempfile::tempdir().unwrap();
    let mut state = controls_state(dir.path());
    capture_alternative(&mut state, "square");
    press_key(&mut state, char_key("j"), Location::Standard);
    capture_alternative(&mut state, "circle");
    press_key(&mut state, char_key("k"), Location::Standard);
    let mut ui = iced_test::simulator::Simulator::with_size(
        iced::Settings::default(),
        iced::Size::new(1280.0, 4000.0),
        app::view(&state),
    );
    ui.click(iced_test::selector::id(controls::clear_alternative_id(
        "square",
    )))
    .unwrap();
    let sent: Vec<Message> = ui.into_messages().collect();
    for message in sent {
        let _ = app::update(&mut state, message);
    }
    assert!(!state.controls.key_alt.contains_key("square"));
    assert_eq!(state.controls.key_alt["circle"], "K");
    assert_eq!(state.controls.key["square"], "Mouse Left");
    // "Clear input" of the row empties the main box and keeps the alternative.
    controls_update(
        &mut state,
        ControlsMessage::Clear("circle".into(), Slot::Main),
    );
    assert_eq!(state.controls.key["circle"], "None");
    assert_eq!(state.controls.key_alt["circle"], "K");
}

#[test]
fn the_resets_drop_the_alternatives() {
    let dir = tempfile::tempdir().unwrap();
    let mut state = controls_state(dir.path());
    let resets = |state: &app::State| {
        tall_messages(state, "Reset keyboard & mouse to defaults")
            .into_iter()
            .filter(|message| matches!(message, Message::Controls(ControlsMessage::ResetTable(_))))
            .count()
    };
    assert_eq!(resets(&state), 0);
    // An alternative alone moves the table off the defaults.
    capture_alternative(&mut state, "square");
    press_key(&mut state, char_key("j"), Location::Standard);
    capture_alternative(&mut state, "cross");
    press_key(&mut state, char_key("k"), Location::Standard);
    assert_eq!(resets(&state), 1);
    // The arrow of a row puts it back on its default, with no alternative.
    controls_update(&mut state, ControlsMessage::ResetKey("cross".into()));
    assert!(!state.controls.key_alt.contains_key("cross"));
    assert!(state.controls.key_alt.contains_key("square"));
    click_tall(&mut state, "Reset keyboard & mouse to defaults");
    assert!(state.controls.key_alt.is_empty());
    assert_eq!(resets(&state), 0);
}

#[test]
fn alternatives_are_saved_only_when_there_are_any() {
    let dir = tempfile::tempdir().unwrap();
    std::fs::write(
        dir.path().join("settings.json"),
        r#"{"controls_layout": 4, "controls": {"key": {"cross": "H"}}}"#,
    )
    .unwrap();
    // Settings from before alternatives load without any and save the same controls.
    let mut state = controls_state(dir.path());
    assert!(state.controls.key_alt.is_empty());
    assert_eq!(state.controls.key["cross"], "H");
    let saved = sanctuaryport_launcher::ui::persist::gather(&state).app["controls"].clone();
    assert_eq!(saved, serde_json::json!({"key": {"cross": "H"}, "pad": {}}));
    // The shape of the example: the main keys stay in `key`, the alternatives in `key_alt`.
    for (button, key) in [
        ("cross", "K"),
        ("circle", "L"),
        ("square", "J"),
        ("triangle", "I"),
    ] {
        capture_alternative(&mut state, button);
        controls_update(
            &mut state,
            ControlsMessage::Captured(button.into(), Slot::Alternative, key.into()),
        );
    }
    let saved = sanctuaryport_launcher::ui::persist::gather(&state).app["controls"].clone();
    assert_eq!(
        saved,
        serde_json::json!({
            "key": {"cross": "H"},
            "pad": {},
            "key_alt": {"cross": "K", "circle": "L", "square": "J", "triangle": "I"}
        })
    );
    // And they come back from the file.
    let file = serde_json::json!({"controls_layout": 4, "controls": saved});
    std::fs::write(dir.path().join("settings.json"), file.to_string()).unwrap();
    let reopened = controls_state(dir.path());
    assert_eq!(reopened.controls.key_alt, state.controls.key_alt);
    let wrong = serde_json::json!({"controls_layout": 4, "controls": {"key_alt": ["J"]}});
    std::fs::write(dir.path().join("settings.json"), wrong.to_string()).unwrap();
    assert!(controls_state(dir.path()).controls.key_alt.is_empty());
}

#[test]
fn second_alternatives_are_saved_only_when_there_are_any_and_come_back() {
    let dir = tempfile::tempdir().unwrap();
    std::fs::write(
        dir.path().join("settings.json"),
        r#"{"controls_layout": 4, "controls": {"key": {"cross": "H"}, "key_alt": {"cross": "K"}}}"#,
    )
    .unwrap();
    // Settings with one alternative load without a second and save the same controls.
    let mut state = controls_state(dir.path());
    assert!(state.controls.key_alt2.is_empty());
    let saved = sanctuaryport_launcher::ui::persist::gather(&state).app["controls"].clone();
    assert_eq!(
        saved,
        serde_json::json!({"key": {"cross": "H"}, "pad": {}, "key_alt": {"cross": "K"}})
    );
    capture_second_alternative(&mut state, "cross");
    controls_update(
        &mut state,
        ControlsMessage::Captured("cross".into(), Slot::SecondAlternative, "Keypad 2".into()),
    );
    let saved = sanctuaryport_launcher::ui::persist::gather(&state).app["controls"].clone();
    assert_eq!(
        saved,
        serde_json::json!({
            "key": {"cross": "H"},
            "pad": {},
            "key_alt": {"cross": "K"},
            "key_alt2": {"cross": "Keypad 2"}
        })
    );
    let file = serde_json::json!({"controls_layout": 4, "controls": saved});
    std::fs::write(dir.path().join("settings.json"), file.to_string()).unwrap();
    let reopened = controls_state(dir.path());
    assert_eq!(reopened.controls.key_alt2, state.controls.key_alt2);
    let wrong = serde_json::json!({"controls_layout": 4, "controls": {"key_alt2": ["J"]}});
    std::fs::write(dir.path().join("settings.json"), wrong.to_string()).unwrap();
    assert!(controls_state(dir.path()).controls.key_alt2.is_empty());
}

#[test]
fn clear_input_unbinds_one_row() {
    let dir = tempfile::tempdir().unwrap();
    let mut state = controls_state(dir.path());
    let before = state.controls.key.clone();
    assert_eq!(before["r3"], "X");
    let mut ui = iced_test::simulator::Simulator::with_size(
        iced::Settings::default(),
        iced::Size::new(1280.0, 4000.0),
        app::view(&state),
    );
    ui.click(iced_test::selector::id(controls::clear_id("r3")))
        .unwrap();
    let sent: Vec<Message> = ui.into_messages().collect();
    for message in sent {
        let _ = app::update(&mut state, message);
    }
    assert_eq!(state.controls.key["r3"], "None");
    for (button, input) in &before {
        if button != "r3" {
            assert_eq!(&state.controls.key[button], input, "{button}");
        }
    }
}

#[test]
fn the_reset_buttons_are_active_only_off_the_defaults() {
    // On the defaults a reset has nothing to change: its button is off, as Save is with nothing
    // to save, so a click that changes rows out of sight still shows that it acted.
    let dir = tempfile::tempdir().unwrap();
    let mut state = controls_state(dir.path());
    let resets = |state: &app::State, label: &str| {
        tall_messages(state, label)
            .into_iter()
            .filter(|message| matches!(message, Message::Controls(ControlsMessage::ResetTable(_))))
            .count()
    };
    assert_eq!(resets(&state, "Reset keyboard & mouse to defaults"), 0);
    capture(&mut state, "cross");
    press_key(&mut state, char_key("h"), Location::Standard);
    assert_eq!(resets(&state, "Reset keyboard & mouse to defaults"), 1);
    click_tall(&mut state, "Reset keyboard & mouse to defaults");
    assert_eq!(resets(&state, "Reset keyboard & mouse to defaults"), 0);

    controls_update(&mut state, ControlsMessage::Device(Device::Gamepad));
    assert_eq!(resets(&state, "Reset gamepad to defaults"), 0);
    controls_update(
        &mut state,
        ControlsMessage::PadPick("cross".into(), "y".into()),
    );
    assert_eq!(resets(&state, "Reset gamepad to defaults"), 1);
}

/// The vertical offsets of the scrollables of `ui`.
fn scroll_offsets(
    ui: &mut iced_test::runtime::UserInterface<'_, Message, iced::Theme, iced::Renderer>,
    renderer: &iced::Renderer,
) -> Vec<f32> {
    use iced::advanced::widget::operation::{Operation, Scrollable};
    struct Offsets(Vec<f32>);
    impl Operation for Offsets {
        fn traverse(&mut self, operate: &mut dyn FnMut(&mut dyn Operation)) {
            operate(self);
        }
        fn scrollable(
            &mut self,
            _id: Option<&iced::widget::Id>,
            _bounds: iced::Rectangle,
            _content_bounds: iced::Rectangle,
            translation: iced::Vector,
            _state: &mut dyn Scrollable,
        ) {
            self.0.push(translation.y);
        }
    }
    let mut offsets = Offsets(Vec::new());
    ui.operate(renderer, &mut offsets);
    offsets.0
}

#[test]
fn a_waiting_key_box_keeps_the_page_where_it_was_scrolled() {
    // Found while looking at item 8: clicking a key box low on the page threw the page back to
    // its top (the waiting layer changed the window's root from a column to a stack, which drops
    // the state of everything under it), so the waiting box and the reset button went out of
    // sight; the end of the wait threw it to the top again.
    use iced::advanced::renderer::Headless;
    use iced_test::runtime::user_interface::{Cache, UserInterface};
    let dir = tempfile::tempdir().unwrap();
    let mut state = controls_state(dir.path());
    settle(&mut state);
    let mut renderer = iced::futures::executor::block_on(<iced::Renderer as Headless>::new(
        iced::Font::with_name("Fira Sans"),
        iced::Pixels(16.0),
        Some("tiny-skia"),
    ))
    .unwrap();
    let size = iced::Size::new(1280.0, 800.0);
    let cursor = mouse::Cursor::Available(iced::Point::new(300.0, 500.0));
    let wheel = iced::Event::Mouse(mouse::Event::WheelScrolled {
        delta: mouse::ScrollDelta::Pixels { x: 0.0, y: -600.0 },
    });
    let mut ui = UserInterface::build(app::view(&state), size, Cache::default(), &mut renderer);
    let mut sent = Vec::new();
    let _ = ui.update(
        &[wheel],
        cursor,
        &mut renderer,
        &mut iced::advanced::clipboard::Null,
        &mut sent,
    );
    let scrolled = scroll_offsets(&mut ui, &renderer);
    assert!(scrolled.iter().any(|y| *y > 100.0), "{scrolled:?}");
    let mut cache = ui.into_cache();
    // The box waits, then gets its key.
    for step in 0..2 {
        if step == 0 {
            capture(&mut state, "dodge");
        } else {
            press_key(&mut state, char_key("h"), Location::Standard);
        }
        settle(&mut state);
        let mut ui = UserInterface::build(app::view(&state), size, cache, &mut renderer);
        assert_eq!(scroll_offsets(&mut ui, &renderer), scrolled, "step {step}");
        cache = ui.into_cache();
    }
}

#[test]
fn a_gamepad_found_while_a_box_waits_ends_the_capture() {
    let dir = tempfile::tempdir().unwrap();
    let mut state = controls_state(dir.path());
    let before = state.controls.key.clone();
    // The player clicks a key box before the first-open probe answers.
    capture(&mut state, "cross");
    controls_update(&mut state, ControlsMessage::GamepadDetected(true));
    assert_eq!(state.controls.device, Device::Gamepad);
    assert!(state.controls.capturing.is_none());
    assert!(state.controls.capture_started.is_none());
    assert_eq!(state.controls.key, before);
    // No hidden box takes the next click: it reaches the gamepad section.
    assert!(matches!(
        tall_messages(&state, "Switch Pro").as_slice(),
        [Message::Controls(ControlsMessage::PadModel(
            PadModel::SwitchPro
        ))]
    ));
}

#[test]
fn losing_the_focus_ends_the_capture() {
    let dir = tempfile::tempdir().unwrap();
    let mut state = controls_state(dir.path());
    let before = state.controls.key.clone();
    capture(&mut state, "cross");
    let _ = app::update(&mut state, Message::WindowFocused(false));
    assert!(!state.focused);
    assert!(state.controls.capturing.is_none());
    assert!(state.controls.capture_started.is_none());
    assert_eq!(state.controls.key, before);
    // The next key binds nothing.
    let _ = app::update(&mut state, Message::WindowFocused(true));
    press_key(&mut state, Key::Named(Named::ArrowLeft), Location::Standard);
    assert_eq!(state.page, Page::Controls);
    assert_eq!(state.controls.key, before);
}

use sanctuaryport_launcher::core::options::{UI_LANGUAGES, UI_SCALES};
use sanctuaryport_launcher::ui::pages::advanced;
use sanctuaryport_launcher::ui::play::GameState;

fn pick_row(state: &app::State, key: &str, value: &str) -> Message {
    let rows = advanced::rows(state, "en");
    let row = rows.iter().find(|row| row.key == key).unwrap();
    let label = &row.pairs.iter().find(|(v, _)| v == value).unwrap().1;
    let choices = match key {
        "ui_language" => UI_LANGUAGES,
        _ => UI_SCALES,
    };
    select_message(key, choices, "en", label).unwrap()
}

#[test]
fn language_and_scale_rows_change_settings() {
    let dir = tempfile::tempdir().unwrap();
    let mut state = state(dir.path());
    state.lang = "en".into();
    let pick = pick_row(&state, "ui_language", "pt");
    let _ = app::update(&mut state, pick);
    assert_eq!(state.store.text("ui_language"), "pt");
    // The interface changes language when the launcher opens again (Save is Task 32).
    assert_eq!(state.lang, "en");
    let pick = pick_row(&state, "ui_scale", "1.5");
    let _ = app::update(&mut state, pick);
    assert_eq!(state.store.text("ui_scale"), "1.5");
    // A file from before shows its closest choice: "2" as 1.5x, "auto" (or anything unknown) as Auto.
    for (stored, shown) in [("auto", "auto"), ("2", "1.5"), ("bogus", "auto")] {
        state.store.set("ui_scale", stored.into());
        let rows = advanced::rows(&state, "en");
        let row = rows.iter().find(|row| row.key == "ui_scale").unwrap();
        assert_eq!(row.current, shown, "{stored}");
    }
}

#[test]
fn clear_cache_is_blocked_while_running() {
    let dir = tempfile::tempdir().unwrap();
    let mut state = state(dir.path());
    // An idle game does not block it; `ui_play.rs` covers a running one.
    assert!(!state.game.is_live());
    state.game = GameState::Idle;
    let cache = dir.path().join("user").join("cache");
    std::fs::create_dir_all(&cache).unwrap();
    std::fs::write(cache.join("a.bin"), b"x").unwrap();
    assert_eq!(advanced::clear_cache(&dir.path().join("user")), Ok(()));
    assert!(!cache.exists());
    // Nothing to remove is not an error.
    assert_eq!(advanced::clear_cache(&dir.path().join("user")), Ok(()));
    let _ = app::update(&mut state, Message::ShaderCacheCleared(Ok(())));
    let shown = state.modal.take().unwrap();
    assert!(matches!(shown.kind, ModalKind::Info));
    assert!(shown.text.starts_with("Shader cache cleared."));
    let _ = app::update(&mut state, Message::ShaderCacheCleared(Err("busy".into())));
    let shown = state.modal.take().unwrap();
    assert!(matches!(shown.kind, ModalKind::Error));
    assert_eq!(
        shown.text,
        "Could not delete all of the shader cache; another program may be using it: busy"
    );
}

#[test]
fn performance_page_has_the_shader_cache_rows() {
    let dir = tempfile::tempdir().unwrap();
    let mut state = state(dir.path());
    state.lang = "en".into();
    open(&mut state, Page::Performance);
    page_has(&state, &["Shader cache", "Delete shader cache"]);
    // SanctuaryPort: the cache switches the runtime does not read are not offered.
    let mut ui = settled(&mut state, Page::Performance, 1280.0, 4000.0);
    assert!(ui.find("Reuse compiled shaders from disk  ·  restart").is_err());
    ui.click("Delete shader cache").unwrap();
    let emitted: Vec<Message> = ui.into_messages().collect();
    // The button asks the same question as the Advanced page's.
    assert!(matches!(emitted.last(), Some(Message::ClearShaderCache)));
}

#[test]
fn deleting_the_shader_cache_asks_then_deletes() {
    let dir = tempfile::tempdir().unwrap();
    let mut state = state(dir.path());
    state.lang = "en".into();
    let user = state.paths.user_dir("");
    let cache = user.join("cache");
    // An empty cache needs no question.
    assert_eq!(advanced::cache_size(&user), 0);
    let _ = app::update(&mut state, Message::ShaderCacheSized(0));
    let shown = state.modal.take().unwrap();
    assert!(matches!(shown.kind, ModalKind::Info));
    assert_eq!(shown.text, "The shader cache is already empty.");
    // 4 KiB deep inside: the question names the size in MB and the folder.
    let deep = cache
        .join("CUSA00243")
        .join("pipeline-binaries")
        .join("shared");
    std::fs::create_dir_all(&deep).unwrap();
    std::fs::write(deep.join("a.bin"), vec![b'x'; 4096]).unwrap();
    assert_eq!(advanced::cache_size(&user), 4096);
    let _ = app::update(&mut state, Message::ShaderCacheSized(4096));
    let asked = state.modal.take().unwrap();
    assert!(matches!(
        &asked.kind,
        ModalKind::YesNo {
            yes: Message::ClearShaderCacheConfirmed,
            no: Message::Modal(ModalMessage::Dismiss)
        }
    ));
    assert_eq!(
        asked.text,
        format!(
            "Delete the shader cache (0 MB in {})? The game compiles the shaders again, so the next start stutters for a few minutes.",
            cache.display()
        )
    );
    assert!(cache.exists());
    // Yes removes it and says so; a folder that stays names itself.
    assert_eq!(advanced::clear_cache(&user), Ok(()));
    assert!(!cache.exists());
    let _ = app::update(&mut state, Message::ShaderCacheCleared(Ok(())));
    let shown = state.modal.take().unwrap();
    assert!(shown.text.starts_with("Shader cache cleared."));
    let _ = app::update(
        &mut state,
        Message::ShaderCacheCleared(Err(cache.display().to_string())),
    );
    let shown = state.modal.take().unwrap();
    assert!(matches!(shown.kind, ModalKind::Error));
    assert_eq!(
        shown.text,
        format!(
            "Could not delete all of the shader cache; another program may be using it: {}",
            cache.display()
        )
    );
    // Whole megabytes, halves to even, as Python's round().
    assert_eq!(advanced::megabytes(2_621_440), 2);
    assert_eq!(advanced::megabytes(1_572_864), 2);
    assert_eq!(advanced::megabytes(3_145_729), 3);
}

#[test]
fn effects_row_toggles_the_fire() {
    let dir = tempfile::tempdir().unwrap();
    let mut state = state(dir.path());
    state.lang = "en".into();
    assert!(state.store.flag("launcher_effects"));
    open(&mut state, Page::Advanced);
    click(&mut state, "Effects");
    assert_eq!(state.store.json("launcher_effects"), Some(&false.into()));
    assert!(!state.store.flag("launcher_effects"));
    click(&mut state, "Effects");
    assert!(state.store.flag("launcher_effects"));
    // Saved off: the next start begins with the fire off.
    std::fs::write(
        dir.path().join("settings.json"),
        r#"{"launcher_effects": false}"#,
    )
    .unwrap();
    let again = common::state(dir.path());
    assert!(!again.store.flag("launcher_effects"));
}

#[test]
fn check_updates_row_hidden_without_self_update() {
    let dir = tempfile::tempdir().unwrap();
    let fake = common::releases::FakeReleases::new().0.self_update(false);
    let state = common::state_with(dir.path(), fake.host());
    assert!(!advanced::check_keys(&state).contains(&"check_updates"));
    assert!(advanced::check_keys(&state).contains(&"close_on_play"));
    let state = common::state(dir.path());
    assert!(advanced::check_keys(&state).contains(&"check_updates"));
}

#[test]
fn the_launcher_buttons_wrap_instead_of_squeezing() {
    let dir = tempfile::tempdir().unwrap();
    let mut state = state(dir.path());
    // Portuguese has the longest labels of the row; 1280 × 800 gives the narrowest drawer. The row
    // of Windows (with Check for updates) on every system.
    state.lang = "pt".into();
    open(&mut state, Page::Advanced);
    let _ = app::update(
        &mut state,
        Message::Tick(iced::time::Instant::now() + std::time::Duration::from_secs(1)),
    );
    let mut ui = iced_test::simulator::Simulator::with_size(
        iced::Settings::default(),
        iced::Size::new(1280.0, 800.0),
        app::view(&state),
    );
    for label in [tr("pt", "Desktop shortcut"), tr("pt", "Add to Steam")] {
        let shown = ui.find(label.as_str()).unwrap().bounds().width;
        // The buttons' labels set no font: the default one.
        let alone = common::natural_size(&label, None, 16.0).width;
        assert!(shown >= alone - 0.5, "{label}: {shown} wide, needs {alone}");
    }
}

#[test]
fn the_files_buttons_share_one_row_at_its_end() {
    // R93: "Port folder", "sanctuary.ini" and "Clear shader cache" on one line, ending where
    // every row's control ends (starting there right to left), each label on one line.
    for lang in [
        "en", "pt", "es", "fr", "de", "it", "pl", "cs", "ru", "tr", "ar", "zh", "ja", "ko", "id",
    ] {
        let dir = tempfile::tempdir().unwrap();
        let mut state = state(dir.path());
        state.lang = lang.into();
        state.rtl = lang == "ar";
        let labels = [
            tr(lang, "Port folder"),
            "sanctuary.ini".to_string(),
            tr(lang, "Clear shader cache"),
        ];
        let mut ui = settled(&mut state, Page::Advanced, 1280.0, 4000.0);
        let scale = control(&mut ui, &tr(lang, "GUI Scale"));
        let files = control(&mut ui, advanced::FILES_ROW);
        let shown: Vec<iced::Rectangle> = labels
            .iter()
            .map(|label| ui.find(label.as_str()).unwrap().bounds())
            .collect();
        for (label, bounds) in labels.iter().zip(&shown) {
            let alone = common::natural_size(label, None, 16.0);
            assert!(
                (bounds.y - shown[0].y).abs() < 0.5 && bounds.height < alone.height * 1.5,
                "{lang}: {label} at {bounds:?}, the first at {:?}",
                shown[0]
            );
            assert!(
                bounds.x >= files.x && bounds.x + bounds.width <= files.x + files.width + 0.5,
                "{lang}: {label} {bounds:?} outside {files:?}"
            );
        }
        let (edge, row_edge) = if lang == "ar" {
            (files.x, scale.x)
        } else {
            (files.x + files.width, scale.x + scale.width)
        };
        assert!(
            (edge - row_edge).abs() < 0.5,
            "{lang}: {files:?} vs {scale:?}"
        );
        // The last button pads its label by 10 to the box's end.
        let last = shown[2];
        if lang != "ar" {
            assert!(
                (last.x + last.width + 10.0 - edge).abs() < 0.5,
                "{lang}: {last:?} vs {edge}"
            );
        }
    }
}

#[test]
fn diagnostics_is_a_plain_section_after_the_others() {
    // R93: no fold; the Diagnostics heading and its rows are always shown, last on the page.
    let dir = tempfile::tempdir().unwrap();
    let mut state = state(dir.path());
    state.lang = "en".into();
    for key in [
        "frame_stats",
        "gpu_profile",
        "perf_diag",
        "frame_capture_key",
        "vk_validation",
    ] {
        assert!(advanced::check_keys(&state).contains(&key), "{key}");
    }
    open(&mut state, Page::Advanced);
    let _ = app::update(
        &mut state,
        Message::Tick(iced::time::Instant::now() + std::time::Duration::from_secs(1)),
    );
    let mut ui = iced_test::simulator::Simulator::with_size(
        iced::Settings::default(),
        iced::Size::new(1280.0, 4000.0),
        app::view(&state),
    );
    let heading = ui.find("Diagnostics (for development)").unwrap().bounds();
    let files = ui.find("Files").unwrap().bounds();
    assert!(heading.y > files.y, "{heading:?} before {files:?}");
    for label in [
        "Frame statistics in the log (every 5 s)",
        "Vulkan validation layers (needs the Vulkan SDK; much slower)",
    ] {
        let shown = ui.find(label).unwrap().bounds();
        assert!(shown.y > heading.y, "{label}: {shown:?}");
    }
    drop(ui);
    click_tall(&mut state, "Frame statistics in the log (every 5 s)");
    assert!(state.store.flag("frame_stats"));
}

#[test]
fn steam_shortcut_starts_in_the_programs_folder() {
    let shortcut =
        advanced::steam_shortcut(std::path::Path::new("/home/u/Apps/spartanport.AppImage"));
    assert_eq!(
        shortcut.exe,
        std::path::PathBuf::from("/home/u/Apps/spartanport.AppImage")
    );
    assert_eq!(shortcut.start_dir, std::path::PathBuf::from("/home/u/Apps"));
    assert_eq!(shortcut.options, "--play");
}

#[test]
fn steam_results_become_the_texts() {
    use sanctuaryport_launcher::core::steam::{Added, SteamError};
    let added = |updated| {
        Ok(Added {
            accounts: 2,
            updated,
            app_id: 1,
        })
    };
    assert_eq!(
        advanced::steam_message("en", &added(false)).unwrap(),
        "The shortcut \"SanctuaryPort\" was added to Steam (accounts: 2), with its icon, capsules, background and logo. Open Steam to see it in the library."
    );
    assert_eq!(
        advanced::steam_message("en", &added(true)).unwrap(),
        "The shortcut \"SanctuaryPort\" was updated in Steam (accounts: 2), with its icon, capsules, background and logo. Open Steam to see it in the library."
    );
    assert_eq!(
        advanced::steam_message("en", &Err(SteamError::Running)).unwrap_err(),
        "Steam is open. Close it completely (Steam > Exit) and click again: Steam rewrites its shortcuts when it closes."
    );
    assert_eq!(
        advanced::steam_message("en", &Err(SteamError::NoSteam)).unwrap_err(),
        "Steam was not found for this user."
    );
    assert_eq!(
        advanced::steam_message("en", &Err(SteamError::NoArt("x".into()))).unwrap_err(),
        "The game folder has no sce_sys/pic1.png for the pictures."
    );
    let long = std::io::Error::other("x".repeat(500));
    let other = advanced::steam_message("en", &Err(SteamError::Io(long))).unwrap_err();
    assert_eq!(other.chars().count(), 400);
}

// --- Log page (Task 31) ---

use sanctuaryport_launcher::ui::pages::log::{self, LogMessage};

#[test]
fn log_page_shows_buffer_lines_and_clears() {
    let dir = tempfile::tempdir().unwrap();
    let mut state = state(dir.path());
    state.lang = "en".into();
    state.log.push_text("line one\nline two\n");
    open(&mut state, Page::Log);
    let mut ui = iced_test::simulator::simulator(app::view(&state));
    // The body is one text widget, so it is found by its whole text.
    assert!(ui
        .find(
            "line one
line two"
        )
        .is_ok());
    drop(ui);

    // Copy hands the text to the clipboard; the buffer stays as it was.
    click(&mut state, "Copy");
    assert_eq!(state.log.len(), 2);

    // Open last_run.log targets <user_dir>/last_run.log.
    let expected = state.paths.run_log_path(&state.store.text("user_dir"));
    assert!(matches!(
        log::open_last_run_message(&state),
        Message::OpenPath(path) if path == expected
    ));
    let sent = messages(&state, "Open last_run.log");
    assert!(matches!(
        sent.as_slice(),
        [Message::Log(LogMessage::OpenLastRun)]
    ));
    let _ = app::update(&mut state, Message::Log(LogMessage::OpenLastRun));

    click(&mut state, "Clear");
    assert_eq!(state.log.len(), 0);
    let mut ui = iced_test::simulator::simulator(app::view(&state));
    assert!(ui
        .find(
            "line one
line two"
        )
        .is_err());
}

#[test]
fn a_red_hairline_separates_the_log_header_from_the_log() {
    // Mockup `#logbox` border-top (ruling R88): between the button row and the text, across it.
    let dir = tempfile::tempdir().unwrap();
    let mut state = state(dir.path());
    state.lang = "en".into();
    state.log.push_text(
        "line one
",
    );
    open(&mut state, Page::Log);
    let _ = app::update(
        &mut state,
        Message::Tick(iced::time::Instant::now() + std::time::Duration::from_secs(1)),
    );
    let mut ui = iced_test::simulator::simulator(app::view(&state));
    let line = ui
        .find(iced_test::selector::id(log::SEPARATOR_ID))
        .unwrap()
        .bounds();
    let copy = ui.find("Copy").unwrap().bounds();
    let kept = ui.find("6 000 lines kept").unwrap().bounds();
    let text = ui.find("line one").unwrap().bounds();
    assert_eq!(line.height, 1.0);
    assert!(
        line.y >= copy.y + copy.height && line.y + line.height <= text.y,
        "header {copy:?}, line {line:?}, log {text:?}"
    );
    // From the first button (which pads its label by 10) to past the line count.
    assert!((line.x - (copy.x - 10.0)).abs() < 0.5, "{line:?}, {copy:?}");
    assert!(
        line.x + line.width >= kept.x + kept.width,
        "{line:?}, {kept:?}"
    );
}

#[test]
fn log_header_translates() {
    let dir = tempfile::tempdir().unwrap();
    let mut state = state(dir.path());
    state.lang = "pt".into();
    open(&mut state, Page::Log);
    let mut ui = iced_test::simulator::simulator(app::view(&state));
    assert!(ui.find("6 000 linhas mantidas").is_ok());
}

/// The release list and the install of any listed release (Task 34, version menu Task 5):
/// nothing here reaches the network or spawns anything.
mod update_flow {
    use std::path::PathBuf;

    use sanctuaryport_launcher::core::release::{Channel, Release, ReleaseId};
    use sanctuaryport_launcher::ui::app::{self, Message};
    use sanctuaryport_launcher::ui::splash::{self};
    use sanctuaryport_launcher::ui::update::{Listing, UpdateMessage};
    use sanctuaryport_launcher::ui::widgets::ModalKind;

    use sanctuaryport_launcher::core::update::{InstallTarget, RELEASES_PAGE};

    use super::common::releases::{
        downloading_state, next_call, state_installed, Call, FakeReleases,
    };
    use super::common::{run_task, state, state_with};

    const OLDER: &str = "This version is older than the installed one.";

    fn release(version: &str, build: Option<&str>, prerelease: bool) -> Release {
        Release {
            id: ReleaseId {
                channel: if prerelease {
                    Channel::PreRelease
                } else {
                    Channel::Release
                },
                version: version.to_string(),
                build: build.map(str::to_string),
            },
            published: "2026-10-22T00:00:00Z".to_string(),
            url: format!("https://example.test/{version}.zip"),
            page: "https://example.test/page".to_string(),
        }
    }

    fn checked(manual: bool, result: Result<Vec<Release>, String>) -> Message {
        Message::Update(UpdateMessage::Checked { manual, result })
    }

    fn modal_text(state: &app::State) -> String {
        state
            .modal
            .as_ref()
            .map(|m| m.text.clone())
            .unwrap_or_default()
    }

    #[test]
    fn a_list_with_a_newer_stable_is_ready_and_newer() {
        let dir = tempfile::tempdir().unwrap();
        let mut state = state(dir.path());
        let status = state.status.clone();
        let list = vec![
            release("999.0.0", None, false),
            release(&ReleaseId::this().version, None, false),
        ];
        let _ = app::update(&mut state, checked(false, Ok(list.clone())));
        assert_eq!(state.updater.listing(), Listing::Listed);
        assert_eq!(state.updater.releases(), list.as_slice());
        assert_eq!(state.updater.newer(), Some(&list[0]));
        assert!(state.modal.is_none());
        assert_eq!(state.status, status);
    }

    #[test]
    fn manual_check_with_latest_shows_the_info() {
        let dir = tempfile::tempdir().unwrap();
        let mut state = state(dir.path());
        let list = vec![release(&ReleaseId::this().version, None, false)];
        let _ = app::update(&mut state, checked(true, Ok(list.clone())));
        assert_eq!(state.updater.releases(), list.as_slice());
        assert_eq!(
            modal_text(&state),
            format!(
                "You have the latest version ({}).",
                ReleaseId::this().version
            )
        );
        assert!(matches!(
            state.modal.as_ref().map(|m| &m.kind),
            Some(ModalKind::Info)
        ));
        // The same list found silently shows nothing.
        state.modal = None;
        let _ = app::update(&mut state, checked(false, Ok(list)));
        assert!(state.modal.is_none());
    }

    #[test]
    fn a_failed_manual_check_reports_and_a_silent_one_does_not() {
        let dir = tempfile::tempdir().unwrap();
        let mut state = state(dir.path());
        let _ = app::update(&mut state, checked(true, Err("offline".into())));
        assert_eq!(modal_text(&state), "Could not check for updates: offline");
        assert!(matches!(
            state.modal.as_ref().map(|m| &m.kind),
            Some(ModalKind::Error)
        ));
        state.modal = None;
        let _ = app::update(&mut state, checked(false, Err("offline".into())));
        assert!(state.modal.is_none());
        assert_eq!(state.updater.listing(), Listing::Failed);
        assert_eq!(state.updater.error(), Some("offline"));
    }

    #[test]
    fn a_failed_check_keeps_a_ready_list() {
        let dir = tempfile::tempdir().unwrap();
        let mut state = state(dir.path());
        let list = vec![release("2.7.0", None, false)];
        let _ = app::update(&mut state, checked(false, Ok(list.clone())));
        let _ = app::update(&mut state, checked(true, Err("offline".into())));
        assert_eq!(state.updater.releases(), list.as_slice());
        assert_eq!(modal_text(&state), "Could not check for updates: offline");
        assert!(matches!(
            state.modal.as_ref().map(|m| &m.kind),
            Some(ModalKind::Error)
        ));
        state.modal = None;
        let _ = app::update(&mut state, checked(false, Err("offline".into())));
        assert_eq!(state.updater.releases(), list.as_slice());
        assert!(state.modal.is_none());
    }

    #[test]
    fn the_start_up_list_is_ready() {
        let dir = tempfile::tempdir().unwrap();
        let mut state = state(dir.path());
        let list = vec![release("2.7.0", None, false), release("2.6.0", None, false)];
        splash::start_up_offer(&mut state, list.clone());
        assert_eq!(state.updater.listing(), Listing::Listed);
        assert_eq!(state.updater.releases(), list.as_slice());
        // A download keeps its state.
        let fetching = release("2.7.0", None, false);
        let mut state = downloading_state(
            dir.path(),
            release(&ReleaseId::this().version, None, false).id,
            fetching.clone(),
            0,
        );
        splash::start_up_offer(&mut state, vec![release("2.8.0", None, false)]);
        assert_eq!(state.updater.downloading(), Some((&fetching, 0)));
        assert_eq!(state.updater.releases(), [fetching].as_slice());
    }

    #[test]
    fn installing_the_installed_release_does_nothing() {
        let dir = tempfile::tempdir().unwrap();
        let mut state = state(dir.path());
        let installed = Release {
            id: ReleaseId::this(),
            ..release("x", None, false)
        };
        assert_eq!(installed.id, *state.updater.installed());
        let task = app::update(
            &mut state,
            Message::Update(UpdateMessage::Install(installed)),
        );
        assert!(state.modal.is_none());
        assert_eq!(task.units(), 0);

        // The channel compiled in is frozen; the list's follows the API.
        let mut state = state_installed(dir.path(), release("2.7.0", None, true).id);
        let task = app::update(
            &mut state,
            Message::Update(UpdateMessage::Install(release("2.7.0", None, false))),
        );
        assert!(state.modal.is_none());
        assert_eq!(task.units(), 0);
    }

    #[test]
    fn install_on_a_read_only_appimage_offers_the_page() {
        let dir = tempfile::tempdir().unwrap();
        let (fake, calls) = FakeReleases::new();
        let target = InstallTarget::AppImage {
            folder: PathBuf::from("/opt/app"),
            writable: false,
        };
        let mut state = state_with(dir.path(), fake.target(target).host());
        let mut newer = release("2.7.0", None, false);
        newer.page = RELEASES_PAGE.to_string();
        let task = app::update(
            &mut state,
            Message::Update(UpdateMessage::Install(newer.clone())),
        );
        assert_eq!(task.units(), 0);
        assert!(
            modal_text(&state).contains("/opt/app"),
            "{}",
            modal_text(&state)
        );
        let Some(ModalKind::YesNo { yes, .. }) = state.modal.as_ref().map(|m| m.kind.clone())
        else {
            panic!("expected the yes/no dialog");
        };
        assert!(
            matches!(&yes, Message::Update(UpdateMessage::OpenPage(page)) if page == RELEASES_PAGE)
        );
        // Nothing reached the host before the Yes.
        assert!(calls.try_recv().is_err());
        let _ = app::update(&mut state, yes);
        assert_eq!(next_call(&calls), Call::OpenPage(RELEASES_PAGE.to_string()));
    }

    #[test]
    fn install_from_a_source_tree_opens_the_page() {
        let dir = tempfile::tempdir().unwrap();
        let (fake, calls) = FakeReleases::new();
        let mut state = state_with(dir.path(), fake.target(InstallTarget::WindowsSource).host());
        let mut newer = release("2.7.0", None, false);
        newer.page = RELEASES_PAGE.to_string();
        let _ = app::update(&mut state, Message::Update(UpdateMessage::Install(newer)));
        assert!(state.modal.is_none());
        assert_eq!(next_call(&calls), Call::OpenPage(RELEASES_PAGE.to_string()));
    }

    #[test]
    fn a_confirmed_install_runs_through_the_host() {
        let dir = tempfile::tempdir().unwrap();
        let (fake, calls) = FakeReleases::new();
        let mut state = state_with(dir.path(), fake.install(Err("boom".into())).host());
        let newer = release("2.7.0", None, false);
        let task = app::update(
            &mut state,
            Message::Update(UpdateMessage::Confirmed(newer.clone())),
        );
        assert_eq!(state.updater.downloading(), Some((&newer, 0)));
        let sent = run_task(task);
        assert_eq!(next_call(&calls), Call::Install(newer.url));
        assert!(
            matches!(sent.as_slice(), [Message::Update(UpdateMessage::Done(Err(e)))] if e == "boom"),
            "{sent:?}"
        );
    }

    #[test]
    fn install_asks_and_marks_an_older_release() {
        let dir = tempfile::tempdir().unwrap();
        let question = "Install version {} now? The launcher closes, installs it and opens again. Saves and settings are kept.";
        let asked = |installed: ReleaseId, picked: Release| {
            let (fake, calls) = FakeReleases::new();
            let mut state = state_with(dir.path(), fake.installed(installed).host());
            let task = app::update(&mut state, Message::Update(UpdateMessage::Install(picked)));
            assert_eq!(task.units(), 0);
            assert!(calls.try_recv().is_err());
            assert!(matches!(
                state.modal.as_ref().map(|m| &m.kind),
                Some(ModalKind::YesNo { .. })
            ));
            modal_text(&state)
        };
        let installed = release("2.6.0", None, false).id;
        assert_eq!(
            asked(installed.clone(), release("2.5.0", None, false)),
            format!(
                "{}
{OLDER}",
                question.replace("{}", "2.5.0")
            )
        );
        assert_eq!(
            asked(installed.clone(), release("2.7.0", None, false)),
            question.replace("{}", "2.7.0")
        );
        // A test build of the installed version is not older.
        let text = asked(installed, release("2.6.0", Some("x"), true));
        assert!(text.starts_with("Install version 2.6.0 · x now?"), "{text}");
        assert!(!text.ends_with(OLDER), "{text}");
    }

    #[test]
    fn install_follows_the_hosts_target() {
        let dir = tempfile::tempdir().unwrap();
        let folder = PathBuf::from("/opt/app");
        let image = |writable| InstallTarget::AppImage {
            folder: folder.clone(),
            writable,
        };
        let mut newer = release("2.7.0", None, false);
        newer.page = RELEASES_PAGE.to_string();
        let install = |target: InstallTarget| {
            let (fake, calls) = FakeReleases::new();
            let mut state = state_with(dir.path(), fake.target(target).host());
            let _ = app::update(
                &mut state,
                Message::Update(UpdateMessage::Install(newer.clone())),
            );
            (state, calls)
        };
        // Not an install the launcher can do: the release page opens.
        let (state, calls) = install(InstallTarget::Elsewhere);
        assert!(state.modal.is_none());
        assert_eq!(next_call(&calls), Call::OpenPage(RELEASES_PAGE.to_string()));
        // A writable AppImage and a package ask first.
        for target in [image(true), InstallTarget::WindowsPackage] {
            let (state, calls) = install(target);
            assert!(matches!(
                state.modal.as_ref().map(|m| &m.kind),
                Some(ModalKind::YesNo {
                    yes: Message::Update(UpdateMessage::Confirmed(_)),
                    ..
                })
            ));
            assert!(calls.try_recv().is_err());
        }
    }

    #[test]
    fn install_is_ignored_while_downloading() {
        let dir = tempfile::tempdir().unwrap();
        let fetching = release("2.7.0", None, false);
        let mut state = downloading_state(
            dir.path(),
            release(&ReleaseId::this().version, None, false).id,
            fetching.clone(),
            0,
        );
        let task = app::update(
            &mut state,
            Message::Update(UpdateMessage::Install(release("2.5.0", None, false))),
        );
        assert_eq!(state.updater.downloading(), Some((&fetching, 0)));
        assert!(state.modal.is_none());
        assert_eq!(task.units(), 0);
    }

    #[test]
    fn a_failed_download_goes_back_to_the_list() {
        let dir = tempfile::tempdir().unwrap();
        let fetched = release("2.7.0", None, false);
        let mut state = downloading_state(
            dir.path(),
            release(&ReleaseId::this().version, None, false).id,
            fetched.clone(),
            0,
        );
        let _ = app::update(
            &mut state,
            Message::Update(UpdateMessage::Done(Err("boom".into()))),
        );
        assert_eq!(state.updater.listing(), Listing::Listed);
        assert_eq!(state.updater.releases(), [fetched].as_slice());
        assert!(state.updater.downloading().is_none());
        assert_eq!(modal_text(&state), "Update failed: boom");
        assert!(matches!(
            state.modal.as_ref().map(|m| &m.kind),
            Some(ModalKind::Error)
        ));
    }

    #[test]
    fn progress_updates_the_percent() {
        let dir = tempfile::tempdir().unwrap();
        let fetching = release("2.7.0", None, false);
        let mut state = downloading_state(
            dir.path(),
            release(&ReleaseId::this().version, None, false).id,
            fetching.clone(),
            0,
        );
        let _ = app::update(&mut state, Message::Update(UpdateMessage::Progress(42)));
        assert_eq!(state.updater.downloading(), Some((&fetching, 42)));
    }

    #[test]
    fn a_silent_check_asks_the_host() {
        let dir = tempfile::tempdir().unwrap();
        let (fake, calls) = FakeReleases::new();
        let list = vec![release("2.7.0", None, false)];
        let mut state = state_with(dir.path(), fake.list(Ok(list.clone())).host());
        assert_eq!(state.updater.listing(), Listing::Fetching);
        let task = app::update(
            &mut state,
            Message::Update(UpdateMessage::Check { manual: false }),
        );
        assert_eq!(next_call(&calls), Call::List);
        for message in run_task(task) {
            let _ = app::update(&mut state, message);
        }
        assert_eq!(state.updater.releases(), list.as_slice());
        assert!(state.modal.is_none());
        assert!(calls.try_recv().is_err());
    }

}


// --- SanctuaryPort: Diablo III's languages and shared saves ---

#[test]
fn the_language_list_follows_the_packs_of_the_dump() {
    let dir = tempfile::tempdir().unwrap();
    let mut state = state(dir.path());
    // No game folder yet: every language.
    let (pairs, shown) = game::game_languages(&state, "1", "en");
    assert_eq!(pairs.len(), LANGUAGES.len());
    assert_eq!(shown, "1");
    // The Brazilian release: Portuguese and Spanish only. English, saved before, shows as the
    // Portuguese run.py starts the game in.
    let game = dir.path().join("CUSA00243");
    std::fs::create_dir_all(game.join("CPKs")).unwrap();
    for pack in ["ptBR", "esMX"] {
        std::fs::write(game.join("CPKs").join(format!("{pack}_Common.cpk")), b"").unwrap();
    }
    set(&mut state, "game_dir", &game.display().to_string());
    let (pairs, shown) = game::game_languages(&state, "1", "en");
    let codes: Vec<&str> = pairs.iter().map(|(code, _)| code.as_str()).collect();
    assert_eq!(codes, ["20", "17"]);
    assert_eq!(shown, "17");
    let (_, shown) = game::game_languages(&state, "20", "en");
    assert_eq!(shown, "20");
}

#[test]
fn diablo_iii_releases_share_their_saves() {
    use std::collections::BTreeSet;
    let titles = |ids: &[&str]| ids.iter().map(|s| s.to_string()).collect::<BTreeSet<String>>();
    // The Brazilian release reads the saves the game keeps under the US title id.
    assert!(!game::backup_is_for_another_game("CUSA00243", &titles(&["CUSA00242"])));
    assert!(!game::backup_is_for_another_game("CUSA00242", &titles(&["CUSA00242"])));
    assert!(game::backup_is_for_another_game("CUSA00243", &titles(&["CUSA12345"])));
    // A backup without a title id says nothing either way.
    assert!(!game::backup_is_for_another_game("CUSA00243", &titles(&[])));
}
