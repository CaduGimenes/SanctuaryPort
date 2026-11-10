//! State and fixture builders the UI tests share. Each test crate uses only some of them.
#![allow(dead_code)]

use std::path::{Path, PathBuf};
use std::sync::Arc;

use iced::widget::text;
use iced::{Element, Font, Size};
use iced_test::simulator::simulator;
use sanctuaryport_launcher::core::gpu::{parse_display_list, Display, GpuRow};
use sanctuaryport_launcher::core::joysticks::JoyError;
use sanctuaryport_launcher::core::paths::Paths;
use sanctuaryport_launcher::core::update::ReleaseHost;
use sanctuaryport_launcher::ui::app::{self, Message, State};
use sanctuaryport_launcher::ui::pages::Page;

pub mod releases;

/// A state over `dir` only, with the splash already gone; its release host is a fake that never
/// reaches the network or the browser.
pub fn state(dir: &Path) -> State {
    state_with(dir, releases::FakeReleases::new().0.host())
}

/// `state` over `host`.
pub fn state_with(dir: &Path, host: Arc<dyn ReleaseHost>) -> State {
    let root = dir.display().to_string();
    let env = move |key: &str| match key {
        "APPDATA" | "USERPROFILE" | "HOME" | "XDG_CONFIG_HOME" => Some(root.clone()),
        _ => None,
    };
    let mut state = State::with_host(Paths::from_env(dir, &env), host);
    state.splash.done = true;
    // No test reaches the machine's own SDL3 or pads: they get a scripted one.
    state.joystick_opener = |_| Err(JoyError::Missing);
    state
}

/// Runs `task` to its end and returns the messages it produced, in order; its window and clipboard
/// actions are dropped. The host's calls run on worker threads, so this waits for them.
pub fn run_task(task: iced::Task<Message>) -> Vec<Message> {
    use iced::futures::{executor::block_on, StreamExt};
    use iced_test::runtime::task::into_stream;
    let Some(stream) = into_stream(task) else {
        return Vec::new();
    };
    block_on(
        stream
            .filter_map(|action| async move {
                match action {
                    iced_test::runtime::Action::Output(message) => Some(message),
                    _ => None,
                }
            })
            .collect(),
    )
}

pub fn open(state: &mut State, page: Page) {
    let _ = app::update(state, Message::ShowPage(page));
}

/// Clicks `label` in the current view and returns what the click emitted.
pub fn messages(state: &State, label: &str) -> Vec<Message> {
    let mut ui = simulator(app::view(state));
    ui.click(label)
        .unwrap_or_else(|error| panic!("click {label:?}: {error:?}"));
    ui.into_messages().collect()
}

/// Clicks `label` and feeds the emitted messages to `update`.
pub fn click(state: &mut State, label: &str) {
    for message in messages(state, label) {
        let _ = app::update(state, message);
    }
}

/// The size `label` takes on its own, on one line, at `size`: in `font`, or with `None` in the
/// renderer's default font, as a text without `.font(..)` gets it. (An explicit `Font::DEFAULT` is
/// the system's sans-serif instead, whose width differs from one system to another.)
pub fn natural_size(label: &str, font: Option<Font>, size: f32) -> Size {
    let alone = text(label).size(size);
    let alone: Element<'_, Message> = match font {
        Some(font) => alone.font(font).into(),
        None => alone.into(),
    };
    simulator(alone).find(label).unwrap().bounds().size()
}

/// A headless tiny-skia renderer with the simulator's default font, for tests that build a
/// `UserInterface` themselves (to keep its widget state from one view to the next, or to run an
/// operation of their own).
pub fn headless_renderer() -> iced::Renderer {
    use iced::advanced::renderer::Headless;
    iced::futures::executor::block_on(<iced::Renderer as Headless>::new(
        Font::with_name("Fira Sans"),
        iced::Pixels(16.0),
        Some("tiny-skia"),
    ))
    .unwrap()
}

/// How many containers with `id` the 1280 × 800 window of `view` holds.
pub fn count_ids(view: Element<'_, Message>, id: &iced::widget::Id) -> usize {
    use iced::advanced::widget::Operation;
    use iced_test::runtime::user_interface::{Cache, UserInterface};
    struct Count<'a>(&'a iced::widget::Id, usize);
    impl Operation for Count<'_> {
        fn traverse(&mut self, operate: &mut dyn FnMut(&mut dyn Operation)) {
            operate(self);
        }
        fn container(&mut self, id: Option<&iced::widget::Id>, _bounds: iced::Rectangle) {
            if id == Some(self.0) {
                self.1 += 1;
            }
        }
    }
    let mut renderer = headless_renderer();
    let mut ui = UserInterface::build(
        view,
        Size::new(1280.0, 800.0),
        Cache::default(),
        &mut renderer,
    );
    let mut count = Count(id, 0);
    ui.operate(&renderer, &mut count);
    count.1
}

/// The folder dialog answered `path` for the setting `key`.
pub fn browsed(state: &mut State, key: &str, path: &Path) {
    let _ = app::update(
        state,
        Message::Browsed(key.into(), Some(path.to_path_buf())),
    );
}

// --- Monitors and graphics cards ---

const TWO_DISPLAYS: &str =
    "0\tMonitor A\t1\t0\t0\t1920\t1080\n1\tMonitor B\t0\t1920\t0\t2560\t1440\n";

pub fn two_displays() -> Vec<Display> {
    parse_display_list(TWO_DISPLAYS)
}

pub type GpuTuple = (
    &'static str,
    &'static str,
    &'static str,
    &'static str,
    &'static str,
);

pub const ARC_LAPTOP: [GpuTuple; 2] = [
    ("0", "Intel(R) Arc(TM) Graphics", "integrated", "off", "0"),
    (
        "1",
        "NVIDIA GeForce RTX 4060 Laptop GPU",
        "discrete",
        "on",
        "1",
    ),
];

/// Rows as `sanctuary-gpu-capabilities --list` prints them: index, name, type, default, pick.
pub fn gpu_rows(rows: &[GpuTuple]) -> Vec<GpuRow> {
    rows.iter()
        .map(|(index, name, kind, msaa, chosen)| GpuRow {
            index: index.to_string(),
            name: name.to_string(),
            kind: kind.to_string(),
            msaa: (!msaa.is_empty()).then(|| msaa.to_string()),
            chosen: *chosen == "1",
        })
        .collect()
}

// --- Game folder, patches and mods ---

pub fn sfo(entries: &[(&str, &str)]) -> Vec<u8> {
    let keys_off = 20 + 16 * entries.len();
    let (mut keys, mut values, mut table) = (Vec::new(), Vec::new(), Vec::new());
    for (key, value) in entries {
        let mut raw = value.as_bytes().to_vec();
        raw.push(0);
        table.extend_from_slice(&(keys.len() as u16).to_le_bytes());
        table.extend_from_slice(&0x204u16.to_le_bytes());
        table.extend_from_slice(&(raw.len() as u32).to_le_bytes());
        table.extend_from_slice(&(raw.len() as u32).to_le_bytes());
        table.extend_from_slice(&(values.len() as u32).to_le_bytes());
        keys.extend_from_slice(key.as_bytes());
        keys.push(0);
        values.extend_from_slice(&raw);
    }
    let values_off = keys_off + keys.len();
    let mut out = b"\0PSF".to_vec();
    out.extend_from_slice(&0x101u32.to_le_bytes());
    out.extend_from_slice(&(keys_off as u32).to_le_bytes());
    out.extend_from_slice(&(values_off as u32).to_le_bytes());
    out.extend_from_slice(&(entries.len() as u32).to_le_bytes());
    out.extend(table);
    out.extend(keys);
    out.extend(values);
    out
}

/// A dump with `eboot.bin` and a tiny `param.sfo`, plus a test patch file under the port folder.
pub fn dump_with_patches(port: &Path) -> PathBuf {
    let dump = port.join("dump");
    std::fs::create_dir_all(dump.join("sce_sys")).unwrap();
    std::fs::write(dump.join("eboot.bin"), b"elf").unwrap();
    std::fs::write(
        dump.join("sce_sys").join("param.sfo"),
        sfo(&[
            ("TITLE_ID", "CUSA00243"),
            ("TITLE", "Diablo III"),
            ("APP_VER", "01.47"),
        ]),
    )
    .unwrap();
    std::fs::create_dir_all(port.join("patches")).unwrap();
    std::fs::write(
        port.join("patches").join("Diablo_III.xml"),
        "<Patch><TitleID><ID>CUSA00243</ID></TitleID>\
         <Metadata Name=\"Fix Alpha\" Author=\"Ann\" Note=\"Fixes alpha.\" AppVer=\"01.47\" isEnabled=\"true\"><PatchList/></Metadata>\
         <Metadata Name=\"Resolution Patch - 3440x1440\" AppVer=\"01.47\"><PatchList/></Metadata>\
         <Metadata Name=\"Fix Other\" AppVer=\"01.01\" isEnabled=\"true\"><PatchList/></Metadata></Patch>",
    )
    .unwrap();
    dump
}

pub fn write_mod_patch(folder: &Path, size: &str) {
    std::fs::write(
        folder.join("u.xml"),
        format!(
            "<Patch><TitleID><ID>CUSA00243</ID></TitleID>\
             <Metadata Name=\"Resolution Patch - {size}\" AppVer=\"01.47\"><PatchList/></Metadata></Patch>"
        ),
    )
    .unwrap();
}

pub fn make_mod(state: &State, name: &str, file: &str) {
    let folder = state.paths.mods_dir("").join(name).join("EXEC");
    std::fs::create_dir_all(&folder).unwrap();
    std::fs::write(folder.join(file), b"data").unwrap();
}
