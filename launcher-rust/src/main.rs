// A windowed program on Windows: no console opens with the launcher. From a terminal it attaches
// to the terminal's console (`platform::attach_parent_console`).
#![windows_subsystem = "windows"]

use std::ffi::OsString;
use std::path::{Path, PathBuf};

use sanctuaryport_launcher::core::env::{environment_of, game_environment};
use sanctuaryport_launcher::core::i18n::{launcher_language, system_language};
use sanctuaryport_launcher::core::installation::Installation;
use sanctuaryport_launcher::core::launch::port_command;
use sanctuaryport_launcher::core::paths::Paths;
use sanctuaryport_launcher::core::platform;
use sanctuaryport_launcher::core::release::ReleaseId;
use sanctuaryport_launcher::core::settings::load_app_settings;
use sanctuaryport_launcher::startup::{self, Route};
use sanctuaryport_launcher::{ui, APP_NAME};

fn main() {
    // First, so `--version`, the update installer and start-up errors print in a terminal (and
    // `--play`'s output: the Python's `attach_stdio`).
    platform::attach_parent_console();
    let args: Vec<OsString> = std::env::args_os().skip(1).collect();
    // SanctuaryPort: `--install-pkg <file.pkg> <games folder>` installs a game or add-on PKG.
    if args.first().is_some_and(|a| a == "--install-pkg") && args.len() == 3 {
        std::process::exit(install_pkg_cli(Path::new(&args[1]), Path::new(&args[2])));
    }
    let exe_dir = std::env::current_exe()
        .ok()
        .and_then(|exe| exe.parent().map(Path::to_path_buf))
        .unwrap_or_else(|| PathBuf::from("."));
    // The environment and the folders derived once, for every route; a packaged port, or the
    // checkout a cargo build sits in (`launcher-rust/target/...`).
    let (route, context) = startup::start(&args, &exe_dir, std::env::vars_os(), cfg!(windows));
    match route {
        Route::Version => println!("{APP_NAME} {}", ReleaseId::this().plain()),
        Route::Play => {
            // As the Python `main`: the start-up steps and the settings first.
            startup::apply_to_process(&context);
            let paths = &context.paths;
            startup::prepare(paths);
            let mut settings = load_app_settings(paths);
            startup::apply_game_options(&mut settings, &args);
            // Without a usable game folder there is nothing to play yet: the window opens instead.
            if Installation::read(
                &settings.get_str("game_dir"),
                &settings.get_str("update_dir"),
            )
            .verdict()
            .passes()
            {
                // SanctuaryPort: the port's runner (game profiles) when it is there (run.sh, or
                // run.py on Windows, as PLAY), with the environment PLAY gives it (update folder,
                // language, icons, key maps...).
                if let Some(command) = port_command(&paths.port_dir, cfg!(windows)) {
                    let lang = launcher_language(&settings.get_str("ui_language"), system_language);
                    let env = game_environment(
                        &settings,
                        paths,
                        &lang,
                        &environment_of(std::env::vars_os()),
                        cfg!(windows),
                    );
                    let status = std::process::Command::new(&command[0])
                        .args(&command[1..])
                        .current_dir(&paths.port_dir)
                        .env_clear()
                        .envs(&env)
                        .status();
                    std::process::exit(status.ok().and_then(|s| s.code()).unwrap_or(1));
                }
                eprintln!("No runner next to the launcher (run.sh, or run.py on Windows).");
                std::process::exit(1);
            }
            open_window(context.paths);
        }
        Route::InstallUpdate { target, pid } => {
            let lang = launcher_language(
                &load_app_settings(&context.paths).get_str("ui_language"),
                system_language,
            );
            std::process::exit(startup::install_update(&exe_dir, &target, pid, &lang))
        }
        Route::Launcher => {
            // Before the folders below are read from the environment.
            startup::apply_to_process(&context);
            startup::prepare(&context.paths);
            open_window(context.paths);
        }
    }
}

/// The launcher window; exit code 1 when it cannot open.
fn open_window(paths: Paths) {
    let data_dir = paths.data_dir.clone();
    let hook = std::panic::take_hook();
    let panic_dir = data_dir.clone();
    std::panic::set_hook(Box::new(move |info| {
        startup::log_failure(&panic_dir, &info.to_string());
        hook(info);
    }));
    // Still a single thread: nothing has started the window's yet.
    let chosen = ["ICED_BACKEND", "WGPU_BACKEND"]
        .iter()
        .any(|key| std::env::var_os(key).is_some());
    if let Some((key, value)) = startup::prepare_window(&data_dir, chosen, cfg!(windows)) {
        std::env::set_var(key, value);
    }
    if let Err(error) = ui::app::run(paths) {
        eprintln!("{APP_NAME}: {error}");
        startup::log_failure(&data_dir, &error.to_string());
        rfd::MessageDialog::new()
            .set_level(rfd::MessageLevel::Error)
            .set_title(APP_NAME)
            .set_description(error.to_string())
            .show();
        std::process::exit(1);
    }
}

/// The command-line installer: progress on stderr, the folder on stdout.
fn install_pkg_cli(pkg: &Path, games: &Path) -> i32 {
    use sanctuaryport_launcher::core::pkg_install;
    let info = match pkg_install::read_info(pkg) {
        Ok(info) => info,
        Err(e) => {
            eprintln!("{}: {e}", pkg.display());
            return 1;
        }
    };
    let target = pkg_install::target_dir(&info, games);
    eprintln!("{} ({:#x}) -> {}", info.content_id, info.content_type, target.display());
    let mut last = 0u64;
    let mut progress = |done: u64, total: u64, file: &str| {
        let percent = (done * 100).checked_div(total).unwrap_or(100);
        if percent != last {
            last = percent;
            eprintln!("{percent:3}% {file}");
        }
        true
    };
    match pkg_install::install(pkg, &target, &mut progress) {
        Ok(bytes) => {
            println!("{}", target.display());
            eprintln!("done: {bytes} bytes");
            0
        }
        Err(e) => {
            eprintln!("install failed: {e}");
            1
        }
    }
}
