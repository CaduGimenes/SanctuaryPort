//! The program's start (Python `main`): what the arguments ask for, and the steps that run before
//! the window opens. None of these steps stops the launcher.

use std::ffi::{OsStr, OsString};
use std::io::Write;
use std::path::{Path, PathBuf};
use std::process::{Command, Stdio};
use std::time::Duration;

use indexmap::IndexMap;
use serde_json::Value;

use crate::core::env::{environment_of, start_up_environment};
use crate::core::i18n::tr;
use crate::core::paths::{default_port_dir, Paths};
use crate::core::settings::{load_app_settings, Settings};
use crate::core::update::{self, is_packaged, update_dir};
use crate::core::{desktop, platform};
use crate::{APP_NAME, VERSION};

/// What the command line asks for, with the arguments each role takes.
#[derive(Debug, Clone, PartialEq, Eq)]
pub enum Route {
    /// `--version`: the name and the version.
    Version,
    /// `--play`: starts the game (the port's run.sh) with the launcher's settings.
    Play,
    /// `--install-update TARGET PID`: the downloaded version installs itself over the old one.
    /// A `pid` that is not a number is 0.
    InstallUpdate { target: PathBuf, pid: u32 },
    /// Anything else: the window.
    Launcher,
}

/// [`route_on`] for the system this program runs on.
pub fn route<S: AsRef<OsStr>>(args: &[S]) -> Route {
    route_on(args, cfg!(windows))
}

/// `--install-update` counts first (what follows it is its own) and needs exactly `TARGET PID` and Windows, the only system that updates itself this way. `--play`
/// counts anywhere, as in the Python `main`. Other arguments, `--script` included, are ignored.
pub fn route_on<S: AsRef<OsStr>>(args: &[S], windows: bool) -> Route {
    let is = |arg: &S, flag: &str| arg.as_ref() == OsStr::new(flag);
    match args.first() {
        Some(first) if is(first, "--install-update") && windows && args.len() == 3 => {
            return Route::InstallUpdate {
                target: PathBuf::from(args[1].as_ref()),
                pid: args[2]
                    .as_ref()
                    .to_str()
                    .and_then(|pid| pid.parse().ok())
                    .unwrap_or(0),
            };
        }
        _ => {}
    }
    if args.iter().any(|arg| is(arg, "--version")) {
        return Route::Version;
    }
    if args.iter().any(|arg| is(arg, "--play")) {
        return Route::Play;
    }
    Route::Launcher
}

/// The text after `flag` (`--game <folder>`), or `None` when the flag is missing or last.
pub fn option_value<S: AsRef<OsStr>>(args: &[S], flag: &str) -> Option<String> {
    let at = args
        .iter()
        .position(|arg| arg.as_ref() == OsStr::new(flag))?;
    let value = args.get(at + 1)?;
    value.as_ref().to_str().map(str::to_owned)
}

/// `--game <folder>` (and `--update <folder>`) name the dump for this start only, so another
/// launcher can pass the player's copy. Nothing is saved. Without `--game` the saved folders stay
/// as they are. An update folder that is not there is left out: it is optional (the US release
/// has none).
pub fn apply_game_options<S: AsRef<OsStr>>(settings: &mut Settings, args: &[S]) {
    let Some(game) = option_value(args, "--game") else {
        return;
    };
    settings.set("game_dir", Value::String(game));
    let mut update = option_value(args, "--update").unwrap_or_default();
    if !Path::new(&update).is_dir() {
        update.clear();
    }
    settings.set("update_dir", Value::String(update));
}

/// Python `install_update`: waits for the old launcher (`pid`) to close, copies this version
/// (`source`) over `target` and starts it. Returns the exit code.
pub fn install_update(source: &Path, target: &Path, pid: u32, lang: &str) -> i32 {
    platform::wait_for_process(pid, Duration::from_secs(60));
    let mut pause = || std::thread::sleep(Duration::from_secs(1));
    if update::install_over(source, target, 30, &mut pause).is_err() {
        platform::show_error(
            APP_NAME,
            &tr(
                lang,
                "Could not install the update. Download it from the releases page.",
            ),
        );
        let _ = desktop::open_url(update::RELEASES_PAGE);
        return 1;
    }
    let started = Command::new(target.join("SanctuaryPort.exe"))
        .current_dir(target)
        .stdin(Stdio::null())
        .spawn();
    match started {
        Ok(_) => 0,
        Err(error) => {
            platform::show_error(APP_NAME, &error.to_string());
            1
        }
    }
}

/// What every route starts from: the folders and the environment, upper-cased once.
#[derive(Debug, Clone)]
pub struct Context {
    pub paths: Paths,
    pub env: IndexMap<String, String>,
}

/// The route and its [`Context`], in the Python's order: the environment (on Windows with upper
/// case names, as `os.environ`), then the folders from it. The `--run` folders start at the port
/// the runner resolves. Nothing here touches the process: see [`apply_to_process`].
pub fn start(
    args: &[OsString],
    exe_dir: &Path,
    vars: impl IntoIterator<Item = (OsString, OsString)>,
    windows: bool,
) -> (Route, Context) {
    let route = route_on(args, windows);
    let mut env = environment_of(vars);
    if windows {
        env = env
            .into_iter()
            .map(|(key, value)| (key.to_uppercase(), value))
            .collect();
    }
    start_up_environment(&mut env, windows);
    let port_dir = default_port_dir(exe_dir);
    let read = |env: &IndexMap<String, String>, port: Option<&Path>| {
        let port = port.map(|port| port.to_string_lossy().into_owned());
        Paths::from_env(&port_dir, &|key| match (key, &port) {
            ("SANCTUARY_PORT_DIR", Some(port)) => Some(port.clone()),
            _ => env.get(key).cloned(),
        })
    };
    let paths = read(&env, None);
    (route, Context { paths, env })
}

/// Sets what `context` changed in this process's environment, for the code that reads it there
/// (the window and the game it starts). Runs while the program has a single thread.
pub fn apply_to_process(context: &Context) {
    for (key, value) in &context.env {
        if std::env::var_os(key).as_deref() != Some(OsStr::new(value)) {
            std::env::set_var(key, value);
        }
    }
}

/// In the data folder: how the window starts. `starting VERSION` until the window has run a
/// while; a mark still there means the start died, and a driver that takes the process down
/// leaves nothing else behind. Each death moves one step away from the default: Direct3D 12 only
/// (`trying-dx12` until it has run a while, then `dx12`), and last the software renderer, which
/// opens anywhere but is slow.
pub const WINDOW_STATE: &str = "launcher_gpu.txt";
/// In the data folder: what stopped the launcher. A windowed program has no console to say it on.
const ERROR_LOG: &str = "launcher_error.log";
/// A mark younger than this is another launcher still starting, not a dead one: a second click
/// must not push a healthy PC towards the slow software renderer.
const WINDOW_RETRY: Duration = Duration::from_secs(10);

/// The environment variable the window has to start with, if any. `chosen`: a backend set by hand
/// wins. `dx12`: the system has Direct3D 12, which leaves out the Vulkan drivers the default
/// loads. A failed write only costs the fallback.
pub fn prepare_window(
    data_dir: &Path,
    chosen: bool,
    dx12: bool,
) -> Option<(&'static str, &'static str)> {
    if chosen {
        return None;
    }
    let state = data_dir.join(WINDOW_STATE);
    let last = std::fs::read_to_string(&state).unwrap_or_default();
    let step = last.strip_suffix(VERSION).map(str::trim_end);
    let fresh = std::fs::metadata(&state)
        .and_then(|metadata| metadata.modified())
        .is_ok_and(|at| at.elapsed().is_ok_and(|age| age < WINDOW_RETRY));
    let next = match step {
        Some(going @ ("starting" | "trying-dx12")) if fresh => going,
        Some("software" | "trying-dx12") => "software",
        Some("starting") if !dx12 => "software",
        Some("starting" | "dx12") => "trying-dx12",
        _ => "starting",
    };
    if step != Some(next) {
        let _ = std::fs::write(&state, format!("{next} {VERSION}"));
    }
    match next {
        "software" => Some(("ICED_BACKEND", "tiny-skia")),
        "trying-dx12" => Some(("WGPU_BACKEND", "dx12")),
        _ => None,
    }
}

/// The window works: the default needs no mark, and `dx12` stays for the next start. `software`
/// stays until the next version.
pub fn confirm_window(data_dir: &Path) {
    let state = data_dir.join(WINDOW_STATE);
    let Ok(last) = std::fs::read_to_string(&state) else {
        return;
    };
    if last.starts_with("starting ") {
        let _ = std::fs::remove_file(state);
    } else if last.starts_with("trying-dx12 ") {
        let _ = std::fs::write(state, format!("dx12 {VERSION}"));
    }
}

/// Adds one line to the error log; with nowhere to write it, the failure stays unrecorded.
pub fn log_failure(data_dir: &Path, text: &str) {
    let at = chrono::Local::now().format("%Y-%m-%d %H:%M:%S");
    let line = format!("{at} {VERSION}: {}\n", text.replace('\n', " "));
    let file = std::fs::OpenOptions::new()
        .create(true)
        .append(true)
        .open(data_dir.join(ERROR_LOG));
    if let Ok(mut file) = file {
        let _ = file.write_all(line.as_bytes());
    }
}

/// The steps between the arguments and the window, in the Python `main`'s order: the settings,
/// the Linux menu entry and what a finished update left behind. The language is the window's
/// (`State::new`).
/// The settings file was `gow3.ini` before SanctuaryPort named everything after itself: an old one
/// beside the new name's place becomes it, unless the new file is already there.
pub fn migrate_settings_file(paths: &Paths) {
    let ini = paths.ini_path();
    if ini.file_name().is_some_and(|name| name == "sanctuary.ini") && !ini.exists() {
        let old = ini.with_file_name("gow3.ini");
        if old.is_file() {
            let _ = std::fs::rename(&old, &ini);
        }
    }
}

pub fn prepare(paths: &Paths) {
    migrate_settings_file(paths);
    // Python `WINDOWS and FROZEN and UPDATE_DIR not in PORT_DIR.parents`: the installed Windows
    // launcher, not the copy that runs from a downloaded update.
    let installed = cfg!(windows) && is_packaged() && !runs_from_update_dir(&paths.port_dir);
    let settings = load_app_settings(paths);
    if !cfg!(windows) {
        let env = |key: &str| std::env::var(key).ok();
        let appimage = env("APPIMAGE");
        let icon = paths.port_dir.join("launcher").join("sanctuaryport.png");
        desktop::integrate_desktop(
            settings.flag("desktop_integration"),
            appimage.as_deref(),
            &icon,
            &paths.data_dir,
            None,
            &env,
        );
    }
    if installed {
        // In use or already gone: tried again at the next start.
        let _ = std::fs::remove_dir_all(update_dir());
    }
}

/// Python `UPDATE_DIR in PORT_DIR.parents`: this copy runs from a downloaded update. Both folders
/// are compared in their canonical form, so a short (8.3) temporary folder still matches.
fn runs_from_update_dir(port_dir: &Path) -> bool {
    let port = port_dir
        .canonicalize()
        .unwrap_or_else(|_| port_dir.to_path_buf());
    let update = update_dir();
    let update = update.canonicalize().unwrap_or(update);
    port != update && port.starts_with(&update)
}

/// SanctuaryPort: the command-line options, for `--help` and the README.
pub const USAGE: &str = "\
Usage: SanctuaryPort [options]
  (no option)            open the launcher
  --play                 start the game with the saved settings, without the launcher window
  --game DIR             the game folder (with eboot.bin), for this start
  --update DIR           the update folder, for this start
  --data-dir DIR         keep settings, saves and generated files in DIR
  --fullscreen | --borderless | --windowed
                         the display mode (saved, as the Graphics page does)
  --set KEY=VALUE        any sanctuary.ini setting, e.g. --set frame_rate=120 (saved; repeatable)
  --install-pkg PKG DIR  install a game, update or add-on PKG into the games folder DIR
  --version              print the version
  --help                 this text";

/// The `sanctuary.ini` edits the command line asks for: `--set key=value` (repeatable) and the
/// display mode switches. A key the ini does not have is an error, so a typo is not saved.
pub fn cli_ini_edits<S: AsRef<OsStr>>(args: &[S]) -> Result<IndexMap<String, String>, String> {
    let known = |key: &str| crate::core::ini::INI_DEFAULTS.iter().any(|(k, _)| *k == key);
    let mut edits = IndexMap::new();
    let mut i = 0;
    while i < args.len() {
        let arg = args[i].as_ref().to_string_lossy();
        match arg.as_ref() {
            "--fullscreen" | "--borderless" | "--windowed" => {
                edits.insert("display_mode".to_string(), arg[2..].to_string());
            }
            "--set" => {
                let pair = args
                    .get(i + 1)
                    .map(|a| a.as_ref().to_string_lossy().into_owned())
                    .ok_or("--set needs KEY=VALUE")?;
                let (key, value) = pair
                    .split_once('=')
                    .ok_or_else(|| format!("--set {pair}: expected KEY=VALUE"))?;
                let key = key.trim();
                if !known(key) {
                    return Err(format!("--set {key}: not a sanctuary.ini setting"));
                }
                edits.insert(key.to_string(), value.trim().to_string());
                i += 1;
            }
            _ => {}
        }
        i += 1;
    }
    Ok(edits)
}

/// Writes `edits` into the ini at `path`, keeping its other lines.
pub fn apply_ini_edits(path: &Path, edits: &IndexMap<String, String>) -> std::io::Result<()> {
    if edits.is_empty() {
        return Ok(());
    }
    let ini = crate::core::ini::load_ini(path);
    crate::core::ini::save_ini(path, edits, &ini.lines)
}

/// `--data-dir DIR`, made absolute.
pub fn cli_data_dir<S: AsRef<OsStr>>(args: &[S]) -> Option<PathBuf> {
    let dir = option_value(args, "--data-dir")?;
    Some(std::path::absolute(&dir).unwrap_or_else(|_| PathBuf::from(dir)))
}

#[cfg(test)]
mod cli_tests {
    use super::*;

    #[test]
    fn the_command_line_edits_the_ini() {
        let args = ["--play", "--fullscreen", "--set", "frame_rate=120", "--set", "vertical_sync=1"];
        let edits = cli_ini_edits(&args).unwrap();
        assert_eq!(edits["display_mode"], "fullscreen");
        assert_eq!(edits["frame_rate"], "120");
        assert_eq!(edits["vertical_sync"], "1");
        assert!(cli_ini_edits(&["--set", "frame_rat=120"]).unwrap_err().contains("frame_rat"));
        assert!(cli_ini_edits(&["--set"]).is_err());
        assert!(cli_ini_edits(&["--set", "novalue"]).is_err());
        let dir = tempfile::tempdir().unwrap();
        let ini = dir.path().join("sanctuary.ini");
        std::fs::write(&ini, "# mine\nframe_rate=60\nfxaa=1\n").unwrap();
        apply_ini_edits(&ini, &edits).unwrap();
        let text = std::fs::read_to_string(&ini).unwrap();
        assert!(text.starts_with("# mine\nframe_rate=120\nfxaa=1\n"));
        assert!(text.contains("display_mode=fullscreen") && text.contains("vertical_sync=1"));
        assert_eq!(cli_data_dir(&["--data-dir", "/tmp/x"]), Some(PathBuf::from("/tmp/x")));
    }
}
