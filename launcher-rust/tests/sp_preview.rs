#![allow(clippy::unwrap_used, clippy::expect_used)]
//! SanctuaryPort preview (development): renders Home and every tab with tiny-skia, in the
//! language of `SP_PREVIEW_LANG` (default pt), from the real port folder and its settings, and
//! writes `<SP_PREVIEW_DIR>/<page>.png`. Ignored by default:
//!
//!     SP_PREVIEW_DIR=/tmp/preview cargo test --release --test sp_preview -- --ignored

use std::borrow::Cow;
use std::path::PathBuf;

use iced::{Settings, Size};
use iced_test::simulator::Simulator;
use sanctuaryport_launcher::core::paths::Paths;
use sanctuaryport_launcher::ui::app::{self, Message, State};
use sanctuaryport_launcher::ui::pages::Page;
use sanctuaryport_launcher::ui::theme;

fn render(state: &State, out: &PathBuf, name: &str) {
    let settings = Settings {
        fonts: theme::font_bytes().map(Cow::Borrowed).to_vec(),
        ..Settings::default()
    };
    let mut ui = Simulator::with_size(settings, Size::new(1280.0, 800.0), app::view(state));
    let snapshot = ui.snapshot(&theme::theme()).unwrap();
    let scratch = tempfile::tempdir().unwrap();
    assert!(snapshot.matches_image(scratch.path().join("render.png")).unwrap());
    let written = std::fs::read_dir(scratch.path()).unwrap().next().unwrap().unwrap().path();
    std::fs::copy(written, out.join(format!("{name}.png"))).unwrap();
}

#[test]
#[ignore]
fn preview_pages() {
    if std::env::var_os("ICED_TEST_BACKEND").is_none() { std::env::set_var("ICED_TEST_BACKEND", "tiny-skia"); }
    let out = PathBuf::from(std::env::var("SP_PREVIEW_DIR").unwrap_or_else(|_| "preview".into()));
    std::fs::create_dir_all(&out).unwrap();
    let port = PathBuf::from(env!("CARGO_MANIFEST_DIR")).parent().unwrap().to_path_buf();
    let paths = Paths::from_env(&port, &|name| {
        (name == "GOW3_PORT_DIR").then(|| port.display().to_string())
    });
    let mut state = State::new(paths);
    state.lang = std::env::var("SP_PREVIEW_LANG").unwrap_or_else(|_| "pt".into());
    state.now = state.splash.started + std::time::Duration::from_secs(30);
    state.splash.done = true;
    state.splash.skipped = true;
    for page in Page::ALL {
        let _ = app::update(&mut state, Message::ShowPage(page));
        state.now += std::time::Duration::from_secs(5);
        render(&state, &out, &format!("{page:?}").to_lowercase());
    }
    let _ = app::update(&mut state, Message::ShowPage(Page::Controls));
    let _ = app::update(
        &mut state,
        Message::Controls(sanctuaryport_launcher::ui::pages::controls::ControlsMessage::Device(
            sanctuaryport_launcher::ui::pages::controls::Device::Gamepad,
        )),
    );
    render(&state, &out, "controls_gamepad");
}
