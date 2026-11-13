//! The Game page: folders, save backups, the game language, audio, cheats and the game patches.
//!
//! Every read of the disk happens in `refresh_backups` and `refresh_game_patches`, which
//! `follow` calls; `view` only draws what they stored in the state.

use std::collections::BTreeSet;
use std::path::{Path, PathBuf};

use iced::widget::{button, column, pick_list, row as layout, text};
use iced::{Center, Element, Left, Right, Task};
use serde_json::{Map, Value};

use crate::core::game::game_profile;
use crate::core::i18n::{tr, tr_fmt};
use crate::core::options::LANGUAGES;
use crate::core::patches::{builtin_xml, game_patch_metas, selectable, PatchMeta};
use crate::core::save_backup::{list_backups, restore, KEEP};
use crate::core::save_restore;
use crate::core::settings::mark_default_patches_offered;
use crate::ui::app::{Message, State};
use crate::ui::follow::{follow, Cause};
use crate::ui::play::{refuse_while_live, LiveAction};
use crate::ui::theme;
use crate::ui::widgets::{
    checkbox, choice_pairs, folder, min_width, row, section, select_pairs, Modal, SELECT_MIN,
    SELECT_TEXT,
};

/// The note under the Game language row; the translation tables key on this text.
pub const LANGUAGE_NOTE: &str = "The language of the game's text and speech, on every start. Only the languages your dump ships are listed: each release has its own.";

/// Game patches of the current dump.
#[derive(Debug, Clone, Default)]
pub struct PatchesView {
    /// The title ID of the game the patches belong to; `None` without a known game.
    pub title_id: Option<String>,
    /// The patches the page offers, in file order.
    pub selectable: Vec<PatchMeta>,
    /// The names that are ticked.
    pub chosen: BTreeSet<String>,
    /// The game version the patches are for.
    pub version: String,
    /// The patch file the list came from.
    pub xml_name: String,
}

/// What the save backups row can do.
#[derive(Debug, Clone)]
pub enum BackupMessage {
    Refresh,
    Choose(String),
    Restore,
    RestoreConfirmed,
    /// The restore finished: the backup it made of the replaced saves, or why it failed.
    Restored(Result<Option<String>, String>),
}

/// The dialog title of a folder key's `Browse…`.
pub fn browse_prompt(key: &str) -> &'static str {
    match key {
        "update_dir" => "Choose the folder of the update (eboot.bin, sce_sys)",
        "user_dir" => "Choose the saves folder",
        "mods_dir" => "Choose the mods folder",
        "patches_dir" => "Choose the patches folder",
        _ => "Choose the folder with eboot.bin",
    }
}

fn user_dir(state: &State) -> PathBuf {
    state.paths.user_dir(&state.store.text("user_dir"))
}

// --- Save backups ---

/// Lists the backups of the saves folder and chooses the newest.
pub(crate) fn refresh_backups(state: &mut State) {
    if state.restoring_saves {
        return;
    }
    state.backup_details = save_restore::list(&user_dir(state), &state.paths.port_dir);
    state.backups = state.backup_details.iter().map(|s| s.key.clone()).collect();
    // Keep empty automatic directories visible as before; restoring one safely refuses it.
    for name in list_backups(&user_dir(state)) {
        if !state.backups.contains(&name) {
            state.backups.push(name);
        }
    }
    state.backup_choice = state.backups.first().cloned().unwrap_or_default();
}

/// Replaces the saves with backup `name`; returns the name of the backup of the replaced saves.
pub fn restore_saves(user_dir: &Path, name: &str) -> Result<Option<String>, String> {
    restore(user_dir, name, None)
        .map(|safety| {
            safety.and_then(|path| path.file_name().map(|n| n.to_string_lossy().into_owned()))
        })
        .map_err(|error| error.to_string())
}

/// Applies a save backups event.
pub fn backup_update(state: &mut State, message: BackupMessage) -> Task<Message> {
    let lang = state.lang.clone();
    match message {
        BackupMessage::Refresh if !state.restoring_saves => refresh_backups(state),
        BackupMessage::Choose(name) if !state.restoring_saves => state.backup_choice = name,
        BackupMessage::Refresh | BackupMessage::Choose(_) => {}
        BackupMessage::Restore => {
            if state.restoring_saves {
                return Task::none();
            }
            let name = state.backup_choice.clone();
            if name.is_empty() {
                return Task::none();
            }
            if refuse_while_live(state, LiveAction::RestoreSaves) {
                return Task::none();
            }
            let mut text = tr_fmt(
                &lang,
                "Replace the current saves with the backup {}? The current saves are backed up first.",
                &[&name],
            );
            if let Some(snapshot) = state.backup_details.iter().find(|s| s.key == name) {
                text += &format!("\n\n{}", snapshot.path.display());
                if state
                    .installation
                    .title_id()
                    .is_some_and(|id| backup_is_for_another_game(id, &snapshot.titles))
                {
                    text += &format!("\n\n{}", tr(&lang, "This backup is for a different game ID and may be incompatible. Game IDs will not be changed."));
                }
            }
            state.modal = Some(Modal::confirm(
                text,
                Message::Backup(BackupMessage::RestoreConfirmed),
            ));
        }
        BackupMessage::RestoreConfirmed => {
            if state.restoring_saves || refuse_while_live(state, LiveAction::RestoreSaves) {
                return Task::none();
            }
            state.modal = None;
            state.restoring_saves = true;
            let user = user_dir(state);
            let port = state.paths.port_dir.clone();
            let name = state.backup_choice.clone();
            return crate::ui::tasks::blocking_or(move || {
                save_restore::restore(&user, &port, &name, None)
                    .map(|path| path.map(|p| p.display().to_string()))
                    .map_err(|e| e.to_string())
            }, Err("Save restore stopped unexpectedly. Check the before-restore backup before trying again.".to_string()))
                .map(|result| Message::Backup(BackupMessage::Restored(result)));
        }
        BackupMessage::Restored(Ok(safety)) => {
            state.restoring_saves = false;
            // Every view of the saves folder follows, the trophies included; the backups are
            // listed again, which moves the choice: name the backup first.
            let name = state.backup_choice.clone();
            let saves = follow(state, Cause::Keys(&["user_dir"]));
            let mut body = tr_fmt(&lang, "Saves restored from {}.", &[&name]);
            if let Some(safety) = safety {
                body += &tr_fmt(&lang, " Previous saves: {}.", &[&safety]);
            }
            state.modal = Some(Modal::info(body));
            return saves;
        }
        BackupMessage::Restored(Err(error)) => {
            state.restoring_saves = false;
            state.modal = Some(Modal::error(tr_fmt(
                &lang,
                "Could not restore: {}",
                &[&error],
            )));
        }
    }
    Task::none()
}

// --- Game patches ---

fn saved_choice(state: &State, title_id: &str) -> Option<BTreeSet<String>> {
    let list = state
        .store
        .json("game_patches")?
        .get(title_id)?
        .as_array()?;
    Some(
        list.iter()
            .filter_map(|name| name.as_str().map(str::to_string))
            .collect(),
    )
}

/// The page's selection into the settings: the whole list for this game, in file order, without
/// the resolution and frame rate patches (the start adds those from `sanctuary.ini`).
pub fn store_game_patches(state: &mut State) {
    let Some(title_id) = state.patches.title_id.clone() else {
        return;
    };
    let names: Vec<Value> = state
        .patches
        .selectable
        .iter()
        .filter(|meta| state.patches.chosen.contains(&meta.name))
        .map(|meta| Value::String(meta.name.clone()))
        .collect();
    let mut all = match state.store.json("game_patches") {
        Some(Value::Object(map)) => map.clone(),
        _ => Map::new(),
    };
    all.insert(title_id.clone(), Value::Array(names));
    state.store.set("game_patches", Value::Object(all));
    let mut offered = Map::new();
    if let Some(saved) = state.store.json("patches_offered") {
        offered.insert("patches_offered".into(), saved.clone());
    }
    mark_default_patches_offered(&mut offered, &title_id);
    if let Some(list) = offered.remove("patches_offered") {
        state.store.set("patches_offered", list);
    }
}

/// Reads the built-in patches of the game in `state.installation` into `state.patches`, keeping
/// the selection of the game that was shown until now.
pub(crate) fn refresh_game_patches(state: &mut State) {
    store_game_patches(state);
    let (title_id, profile) = (
        state.installation.title_id().map(str::to_string),
        state.installation.profile(),
    );
    let (Some(title_id), Some(profile)) = (title_id, profile) else {
        state.patches = PatchesView::default();
        return;
    };
    let xml = builtin_xml(&state.paths, profile);
    let metas = game_patch_metas(&xml, profile.app_version).unwrap_or_default();
    let chosen = saved_choice(state, &title_id).unwrap_or_else(|| {
        metas
            .iter()
            .filter(|meta| meta.enabled_by_default)
            .map(|meta| meta.name.clone())
            .collect()
    });
    state.patches = PatchesView {
        title_id: Some(title_id),
        selectable: selectable(&metas),
        chosen,
        version: profile.app_version.to_string(),
        xml_name: xml
            .file_name()
            .map(|name| name.to_string_lossy().into_owned())
            .unwrap_or_default(),
    };
}

/// Ticks or unticks a patch.
pub fn patch_toggled(state: &mut State, name: &str, on: bool) {
    if on {
        state.patches.chosen.insert(name.to_string());
    } else {
        state.patches.chosen.remove(name);
    }
    store_game_patches(state);
}

// --- View ---

/// A folder row with its note under it.
pub fn folder_row<'a>(state: &State, key: &str, label: &str, note: &str) -> Element<'a, Message> {
    let value = state.store.text(key);
    // Python `open_path`: an empty saves folder opens `DATA_DIR/user`; an unset game or update
    // folder has nothing to open.
    let open = if key == "user_dir" {
        Some(state.paths.user_dir(&value))
    } else {
        (!value.is_empty()).then(|| PathBuf::from(&value))
    };
    column![
        folder(state.rtl, key, label, &value, "", open, &state.lang),
        text(note.to_string()).size(13).style(theme::text_muted)
    ]
    .spacing(2)
    .align_x(if state.rtl { Right } else { Left })
    .into()
}

fn backups_row(state: &State) -> Element<'_, Message> {
    let lang = state.lang.as_str();
    let choose = min_width(
        SELECT_MIN,
        pick_list(
            state.backups.clone(),
            (!state.backup_choice.is_empty()).then(|| state.backup_choice.clone()),
            |name| Message::Backup(BackupMessage::Choose(name)),
        )
        .text_size(SELECT_TEXT)
        .style(theme::pick_list_dark),
    );
    let restore = button(text(tr(lang, "Restore")))
        .style(theme::button_flat)
        .on_press(Message::Backup(BackupMessage::Restore));
    let refresh = button(text(tr(lang, "Refresh")))
        .style(theme::button_flat)
        .on_press(Message::Backup(BackupMessage::Refresh));
    let controls = row(
        state.rtl,
        &tr(lang, "Save backups"),
        layout![choose, restore, refresh]
            .spacing(6)
            .align_y(Center)
            .into(),
        Some(&tr_fmt(
            lang,
            "Automatic backups keep the last {} starts. Before-restore backups are kept separately and are never automatically deleted.",
            &[&KEEP.to_string()],
        )),
    );
    let mut body = column![controls].spacing(4);
    if let Some(snapshot) = state
        .backup_details
        .iter()
        .find(|s| s.key == state.backup_choice)
    {
        let titles = snapshot
            .titles
            .iter()
            .cloned()
            .collect::<Vec<_>>()
            .join(", ");
        body = body.push(
            text(tr_fmt(
                lang,
                "{} · {} files · {} bytes · Game IDs: {}",
                &[
                    &tr(lang, snapshot.source.label()),
                    &snapshot.files.to_string(),
                    &snapshot.bytes.to_string(),
                    &titles,
                ],
            ))
            .size(12),
        );
        body = body.push(
            text(format!(
                "{}\n{}",
                snapshot.time_label(),
                snapshot.path.display()
            ))
            .size(12),
        );
        if state
            .installation
            .title_id()
            .is_some_and(|id| backup_is_for_another_game(id, &snapshot.titles))
        {
            body = body.push(text(tr(lang, "This backup is for a different game ID and may be incompatible. Game IDs will not be changed.")).size(12));
        }
    }
    body.into()
}

pub fn view(state: &State) -> Element<'_, Message> {
    let lang = state.lang.as_str();
    let hint = |english: &str| tr(lang, english);
    let language = state.store.text("language");
    let (language_pairs, shown_language) = game_languages(state, &language, lang);
    let default_saves = state.paths.user_dir("").display().to_string();
    // The Python marks a row that needs a new start of the game with a small `restart` note.

    column![
        install_section(state),
        section(state.rtl, &tr(lang, "Folders")),
        folder_row(
            state,
            "game_dir",
            &tr(lang, "Game folder"),
            &hint(
                "Your own dump of the game (eboot.bin, sce_module, sce_sys): SanctuaryPort, any region and version."
            ),
        ),
        folder_row(
            state,
            "update_dir",
            &tr(lang, "Update folder (optional)"),
            &hint(
                "Only if the update is dumped apart from the game: it is laid over the game folder without changing either. Leave empty when the game folder already has the update."
            ),
        ),
        folder_row(
            state,
            "user_dir",
            &tr(lang, "Saves folder"),
            &tr_fmt(
                lang,
                "Empty: {} (shader caches are kept there too).",
                &[&default_saves],
            ),
        ),
        section(state.rtl, &tr(lang, "Saves")),
        backups_row(state),
        checkbox(
            state.rtl,
            "unlock_cosmetics",
            &tr(lang, "Unlock all cosmetics and transmogs"),
            state.store.flag("unlock_cosmetics"),
            Some(&hint(
                "Every wing, pet, pennant and portrait frame of the game, season rewards included, and every legendary and set item look at the Mystic, in the account saves when the game starts (a backup is kept next to each). Some wings have no icon in the menus but show in game."
            )),
        ),
        section(state.rtl, &tr(lang, "Language")),
        row(
            state.rtl,
            &tr(lang, "Game language"),
            select_pairs("language", &language_pairs, &shown_language),
            Some(&hint(LANGUAGE_NOTE)),
        ),
        // No patches or audio settings here: Diablo III keeps its volume in its own options, and
        // its render resolution patches follow the Graphics page.
    ]
    .spacing(2)
    .into()
}

/// Whether a backup holding the saves of `titles` belongs to another game than `current`. The
/// releases of one game (Diablo III's US, Brazilian, European... title IDs) share their saves.
pub fn backup_is_for_another_game(current: &str, titles: &BTreeSet<String>) -> bool {
    let current_game = game_profile(Some(current), None);
    let same = |title: &str| {
        title == current
            || current_game.is_some_and(|game| game_profile(Some(title), None) == Some(game))
    };
    !titles.is_empty() && !titles.iter().any(|t| same(t))
}

/// The system language codes and the language pack (CPK prefix) each one needs, in the order
/// run.py falls back when the chosen pack is missing (scripts: `D3_LANGUAGE_PACKS`).
const LANGUAGE_PACKS: [(&str, &str); 10] = [
    ("17", "ptBR"),
    ("1", "enUS"),
    ("20", "esMX"),
    ("2", "frFR"),
    ("16", "plPL"),
    ("8", "ruRU"),
    ("3", "esES"),
    ("4", "deDE"),
    ("5", "itIT"),
    ("18", "enGB"),
];

/// The Game language list: only the languages whose pack the game (or its update) folder has,
/// all of them while no folder is set; and the one run.py will use when the saved choice has
/// no pack there.
pub fn game_languages(state: &State, chosen: &str, lang: &str) -> (Vec<(String, String)>, String) {
    let folders: Vec<PathBuf> = ["game_dir", "update_dir"]
        .iter()
        .map(|key| state.store.text(key))
        .filter(|dir| !dir.is_empty())
        .map(|dir| PathBuf::from(dir).join("CPKs"))
        .collect();
    let has = |pack: &str| {
        folders
            .iter()
            .any(|cpks| cpks.join(format!("{pack}_Common.cpk")).is_file())
    };
    let available: Vec<&str> = LANGUAGE_PACKS
        .iter()
        .filter(|(_, pack)| has(pack))
        .map(|(code, _)| *code)
        .collect();
    let pairs: Vec<(String, String)> = choice_pairs(LANGUAGES, lang)
        .into_iter()
        .filter(|(code, _)| available.is_empty() || available.contains(&code.as_str()))
        .collect();
    let shown = if available.is_empty() || available.contains(&chosen) {
        chosen.to_string()
    } else {
        available[0].to_string()
    };
    (pairs, shown)
}

// --- SanctuaryPort: install from PKG ---

/// The state of a PKG install, shared between its thread and the view.
#[derive(Debug, Default)]
pub struct PkgStatus {
    pub running: bool,
    /// What the row shows: the file and percent, or the result.
    pub text: String,
    /// The game folder an install wrote; the next tick sets it as the game folder.
    pub installed_game: Option<std::path::PathBuf>,
    /// The update folder an install wrote; the next tick sets it as the update folder.
    pub installed_update: Option<std::path::PathBuf>,
}

/// Where PKGs install: `~/Games/PS4` (the game in `<TITLE_ID>`, add-ons in `addcont/`).
/// Windows has no `HOME`: the user folder. (Before, the path stayed relative and the add-ons
/// landed in the launcher's own folder; run.py still looks there, GitHub #13.)
pub fn pkg_games_dir() -> std::path::PathBuf {
    std::env::var_os("HOME")
        .filter(|home| !home.is_empty())
        .or_else(|| std::env::var_os("USERPROFILE").filter(|home| !home.is_empty()))
        .map(std::path::PathBuf::from)
        .unwrap_or_default()
        .join("Games")
        .join("PS4")
}

/// Installs `files` one after the other in a thread: games first, then add-ons.
pub fn start_pkg_install(state: &mut State, files: Vec<std::path::PathBuf>) {
    use crate::core::pkg_install;
    if files.is_empty() {
        return;
    }
    let status = state.pkg_install.clone();
    if let Ok(mut s) = status.lock() {
        if s.running {
            return;
        }
        s.running = true;
        s.text = tr(&state.lang, "Reading the PKG files…");
    }
    let lang = state.lang.clone();
    std::thread::spawn(move || {
        let games = pkg_games_dir();
        let mut jobs: Vec<_> = files
            .into_iter()
            .filter_map(|f| pkg_install::read_info(&f).ok().map(|i| (f, i)))
            .collect();
        // The game first, then its update, then the add-ons.
        jobs.sort_by_key(|(_, info)| (!info.is_game(), info.is_patch()));
        let count = jobs.len();
        let mut result = Vec::new();
        let mut game_dir = None;
        let mut update_dir = None;
        for (n, (file, info)) in jobs.into_iter().enumerate() {
            let target = pkg_install::target_dir(&info, &games);
            let name = info.content_id.clone();
            let shared = status.clone();
            let mut progress = |done: u64, total: u64, current: &str| {
                if let Ok(mut s) = shared.lock() {
                    let percent = (done * 100).checked_div(total).unwrap_or(100);
                    s.text = format!("{}/{count} · {name} · {percent}% · {current}", n + 1);
                }
                true
            };
            match pkg_install::install(&file, &target, &mut progress) {
                Ok(_) => {
                    if info.is_patch() {
                        update_dir = Some(target.clone());
                    } else if info.is_game() {
                        game_dir = Some(target.clone());
                    }
                    result.push(format!("✓ {name}"));
                }
                Err(e) => result.push(format!("✗ {name}: {e}")),
            }
        }
        if let Ok(mut s) = status.lock() {
            s.running = false;
            s.installed_game = game_dir;
            s.installed_update = update_dir;
            s.text = format!("{} {}", tr(&lang, "Install finished:"), result.join("  "));
        }
    });
}

fn install_section(state: &State) -> Element<'_, Message> {
    let lang = state.lang.as_str();
    column![
        section(state.rtl, &tr(lang, "Installation")),
        install_row(state, &tr(lang, "Game and add-ons")),
    ]
    .spacing(2)
    .into()
}

/// The Install from PKG row (also on the first start's Folders step), named `label`.
pub fn install_row<'a>(state: &'a State, label: &str) -> Element<'a, Message> {
    let lang = state.lang.as_str();
    let (running, text) = state
        .pkg_install
        .lock()
        .map(|s| (s.running, s.text.clone()))
        .unwrap_or_default();
    let mut button = iced::widget::button(iced::widget::text(tr(lang, "Install from PKG…")))
        .style(theme::button_flat);
    if !running {
        button = button.on_press(Message::InstallPkg);
    }
    let note = if text.is_empty() {
        tr_fmt(
            lang,
            "Your own game and add-on PKG files: they are installed into {} and the game folder is set.",
            &[&pkg_games_dir().display().to_string()],
        )
    } else {
        text
    };
    row(state.rtl, label, button.into(), Some(&note))
}
