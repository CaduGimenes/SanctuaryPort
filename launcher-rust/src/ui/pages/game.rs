//! The Game page: folders, save backups, the game language, audio, cheats and the game patches.
//!
//! Every read of the disk happens in `refresh_backups` and `refresh_game_patches`, which
//! `follow` calls; `view` only draws what they stored in the state.

use std::collections::BTreeSet;
use std::path::{Path, PathBuf};

use iced::widget::{button, column, pick_list, row as layout, text, text_input};
use iced::{Center, Element, Left, Right, Task};
use serde_json::{Map, Value};

use crate::core::game::game_profile;
use crate::core::i18n::{tr, tr_fmt};
use crate::core::options::LANGUAGES;
use crate::core::patches::{builtin_xml, game_patch_metas, selectable, PatchMeta};
use crate::core::save_backup::{list_backups, restore, KEEP};
use crate::core::save_restore;
use crate::core::seasons::{
    apply_mode, days_left, rotated_season, today, SeasonMode, EVENT_PREFIX, SEASON_PREFIX,
};
use crate::core::settings::mark_default_patches_offered;
use crate::ui::app::{Message, State};
use crate::ui::follow::{follow, Cause};
use crate::ui::play::{refuse_while_live, LiveAction};
use crate::ui::theme;
use crate::ui::widgets::{
    checkbox, choice_pairs, folder, min_width, row, section, select_pairs, toggle_row, Modal,
    SELECT_MIN, SELECT_TEXT,
};

/// The note under the Game language row; the translation tables key on this text.
pub const LANGUAGE_NOTE: &str = "The language of the game's text and speech, on every start. Only the languages your dump ships are listed: each release has its own.";

/// The note under the Player name row; the translation tables key on this text.
pub const PLAYER_NAME_NOTE: &str = "The name the game shows for you, as a PS4 user name: up to 16 characters, read when the game starts. Empty: Player.";

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
    apply_season_rules(state);
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

    let page = column![
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
        section(state.rtl, &tr(lang, "Player")),
        row(
            state.rtl,
            &tr(lang, "Player name"),
            text_input("Player", &state.store.text("player_name"))
                .on_input(|name| Message::Set("player_name".to_string(), Value::String(name)))
                .padding([6, 10])
                .width(SELECT_MIN)
                .style(theme::text_input_dark)
                .into(),
            Some(&hint(PLAYER_NAME_NOTE)),
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
    ];
    page.spacing(2).into()
}

/// The note under the first offline event; the translation tables key on this text.
const OFFLINE_EVENTS_HINT: &str = "On top of the season's theme, for the next start. They need an offline season; the Altar of Rites and Visions of Enmity are always on with one.";

/// The note under the offline season row; the translation tables key on this text.
const OFFLINE_SEASON_HINT: &str = "Seasonal heroes offline: the season the game runs. Takes effect on the next start. Off: no season.";

/// One entry of the offline season drop-down: a season number, or None for Off. The shown text is
/// translated; the number is what picking it selects.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct SeasonChoice {
    pub number: Option<String>,
    label: String,
}

impl std::fmt::Display for SeasonChoice {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        f.write_str(&self.label)
    }
}

/// The season numbers of the patch names "Offline Seasons - Season N", in file order.
fn season_numbers<'a>(names: impl Iterator<Item = &'a str>) -> Vec<String> {
    names
        .filter_map(|name| name.strip_prefix(SEASON_PREFIX))
        .map(str::to_string)
        .collect()
}

/// The drop-down entries: Off, then one per season.
fn season_choices(numbers: &[String], lang: &str) -> Vec<SeasonChoice> {
    let mut choices = vec![SeasonChoice { number: None, label: tr(lang, "Off") }];
    choices.extend(numbers.iter().map(|n| SeasonChoice {
        number: Some(n.clone()),
        label: format!("{} {n}", tr(lang, "Season")),
    }));
    choices
}

/// Ticks the season patch `number` and unticks every other one (None ticks none: at most one
/// season is on).
fn pick_season(chosen: &mut BTreeSet<String>, number: Option<&str>) {
    chosen.retain(|name| !name.starts_with(SEASON_PREFIX));
    if let Some(n) = number {
        chosen.insert(format!("{SEASON_PREFIX}{n}"));
    }
}

/// The Offline season drop-down: Off, or one of the season patches (at most one is ticked).
fn season_row(state: &State) -> Element<'_, Message> {
    let lang = state.lang.as_str();
    let numbers = season_numbers(state.patches.selectable.iter().map(|m| m.name.as_str()));
    let choices = season_choices(&numbers, lang);
    let current = state
        .patches
        .chosen
        .iter()
        .find_map(|n| n.strip_prefix(SEASON_PREFIX))
        .and_then(|n| choices.iter().find(|c| c.number.as_deref() == Some(n)).cloned())
        .or_else(|| choices.first().cloned());
    let list = pick_list(choices, current, Message::SeasonPicked)
        .text_size(SELECT_TEXT)
        .style(theme::pick_list_dark);
    row(
        state.rtl,
        &tr(lang, "Offline season"),
        min_width(SELECT_MIN, list).into(),
        Some(&tr(lang, OFFLINE_SEASON_HINT)),
    )
}

/// The Seasons tab: the offline season, how its events follow it, the 15 day rotation and, in
/// Manual mode, one check box per event.
pub fn seasons_view(state: &State) -> Element<'_, Message> {
    let lang = state.lang.as_str();
    let any = state
        .patches
        .selectable
        .iter()
        .any(|m| m.name.starts_with(SEASON_PREFIX) || m.name.starts_with(EVENT_PREFIX));
    if !any {
        return text(tr(lang, "No offline seasons for this game."))
            .style(theme::text_muted)
            .into();
    }
    let mut page = column![];
    // The offline season, when the patch file has "Offline Seasons - Season N" entries.
    if state.patches.selectable.iter().any(|m| m.name.starts_with(SEASON_PREFIX)) {
        page = page
            .push(section(state.rtl, &tr(lang, "Offline seasons")))
            .push(season_row(state))
            .push(mode_row(state))
            .push(rotate_row(state));
    }
    // The offline community events ("Event: ..." patches), one check box each, when the player
    // picks them: the other modes choose them with the season.
    let manual = SeasonMode::from_key(&state.store.text("season_mode")) == SeasonMode::Manual
        || !state.patches.selectable.iter().any(|m| m.name.starts_with(SEASON_PREFIX));
    let events: Vec<&PatchMeta> = state
        .patches
        .selectable
        .iter()
        .filter(|m| m.name.starts_with(EVENT_PREFIX))
        .filter(|_| manual)
        .collect();
    if !events.is_empty() {
        page = page.push(section(state.rtl, &tr(lang, "Offline events")));
        for (i, meta) in events.iter().enumerate() {
            let name = meta.name.clone();
            let label = name.strip_prefix(EVENT_PREFIX).unwrap_or(&name).to_string();
            let checked = state.patches.chosen.contains(&name);
            let hint = (i == 0).then(|| tr(lang, OFFLINE_EVENTS_HINT));
            page = page.push(toggle_row(
                state.rtl,
                &label,
                checked,
                Some(move |on| Message::PatchToggled(name.clone(), on)),
                hint.as_deref(),
            ));
        }
    }
    page.spacing(2).into()
}

/// Picks the offline season: unticks every season patch, then ticks the chosen one. With the
/// rotation on, the picked season is where it starts again.
pub fn season_picked(state: &mut State, choice: &SeasonChoice) {
    pick_season(&mut state.patches.chosen, choice.number.as_deref());
    if let Some(number) = choice.number.as_deref() {
        if state.store.flag("season_rotate") {
            start_rotation(state, number);
        }
    }
    apply_season_rules(state);
}

/// The note under the season mode row; the translation tables key on this text.
const SEASON_MODE_HINT: &str = "Manual: you tick the events below. With its events: the season brings its own events and no others, like Blizzard's calendar. Full: the season with every event at once.";

/// The note under the rotation row; the translation tables key on this text.
const SEASON_ROTATE_HINT: &str = "From the day you switch it on, the season moves to the next one every 15 days, with its events. Seasonal heroes of past seasons stay in your saves.";

/// One entry of the season mode drop-down.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct ModeChoice {
    pub mode: SeasonMode,
    label: String,
}

impl std::fmt::Display for ModeChoice {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        f.write_str(&self.label)
    }
}

fn mode_choices(lang: &str) -> Vec<ModeChoice> {
    [
        (SeasonMode::Manual, "Manual: I tick the events"),
        (SeasonMode::WithEvents, "Season with its events"),
        (SeasonMode::Full, "Full season: all events"),
    ]
    .into_iter()
    .map(|(mode, label)| ModeChoice { mode, label: tr(lang, label) })
    .collect()
}

/// The season mode drop-down.
fn mode_row(state: &State) -> Element<'_, Message> {
    let lang = state.lang.as_str();
    let choices = mode_choices(lang);
    let mode = SeasonMode::from_key(&state.store.text("season_mode"));
    let current = choices.iter().find(|c| c.mode == mode).cloned();
    let list = pick_list(choices, current, Message::SeasonModePicked)
        .text_size(SELECT_TEXT)
        .style(theme::pick_list_dark);
    row(
        state.rtl,
        &tr(lang, "Season events"),
        min_width(SELECT_MIN, list).into(),
        Some(&tr(lang, SEASON_MODE_HINT)),
    )
}

/// The rotation check box, with the day it is on under it.
fn rotate_row(state: &State) -> Element<'_, Message> {
    let lang = state.lang.as_str();
    let on = state.store.flag("season_rotate");
    let check = toggle_row(
        state.rtl,
        &tr(lang, "Rotate the season every 15 days"),
        on,
        Some(Message::SeasonRotateToggled),
        Some(&tr(lang, SEASON_ROTATE_HINT)),
    );
    let started = parse_rotation(&state.store.text("season_rotate_start"));
    match (on, started) {
        (true, Some((_, start))) => {
            let now = state
                .patches
                .chosen
                .iter()
                .find_map(|n| n.strip_prefix(SEASON_PREFIX))
                .unwrap_or("?");
            column![
                check,
                text(format!("{} {now}: {} {}", tr(lang, "Now: Season"), days_left(start, today()), tr(lang, "days left")))
                    .size(13)
                    .style(theme::text_muted)
            ]
            .spacing(2)
            .into()
        }
        _ => check,
    }
}

/// The saved rotation start: "<first season>:<day>".
pub fn parse_rotation(saved: &str) -> Option<(u32, i64)> {
    let (season, day) = saved.split_once(':')?;
    Some((season.parse().ok()?, day.parse().ok()?))
}

/// The ticked season number.
pub fn current_season(chosen: &BTreeSet<String>) -> Option<u32> {
    chosen.iter().find_map(|n| n.strip_prefix(SEASON_PREFIX)).and_then(|n| n.parse().ok())
}

/// The rotation starts today at `first`.
fn start_rotation(state: &mut State, first: &str) {
    state
        .store
        .set("season_rotate_start", Value::String(format!("{first}:{}", today())));
}

/// Picks the season mode.
pub fn season_mode_picked(state: &mut State, choice: &ModeChoice) {
    state.store.set("season_mode", Value::String(choice.mode.key().to_string()));
    apply_season_rules(state);
}

/// Switches the 15 day rotation on (starting today at the ticked season, or at the first one) or off.
pub fn season_rotate_toggled(state: &mut State, on: bool) {
    state.store.set("season_rotate", Value::Bool(on));
    if on {
        let numbers = sorted_season_numbers(state);
        let first = current_season(&state.patches.chosen)
            .or_else(|| numbers.first().copied())
            .map(|n| n.to_string());
        if let Some(first) = first {
            pick_season(&mut state.patches.chosen, Some(&first));
            start_rotation(state, &first);
        }
    }
    apply_season_rules(state);
}

fn sorted_season_numbers(state: &State) -> Vec<u32> {
    let mut numbers: Vec<u32> =
        season_numbers(state.patches.selectable.iter().map(|m| m.name.as_str()))
            .iter()
            .filter_map(|n| n.parse().ok())
            .collect();
    numbers.sort_unstable();
    numbers.dedup();
    numbers
}

/// The season rules on the ticked patches: the rotation's season for today, then the events the
/// season mode asks for. Run when the page is shown, on every season change and at PLAY.
pub fn apply_season_rules(state: &mut State) {
    if state.patches.title_id.is_none() {
        return;
    }
    if state.store.flag("season_rotate") {
        if let Some((first, start)) = parse_rotation(&state.store.text("season_rotate_start")) {
            let numbers = sorted_season_numbers(state);
            if let Some(number) = rotated_season(&numbers, first, start, today()) {
                pick_season(&mut state.patches.chosen, Some(&number.to_string()));
            }
        }
    }
    let available: BTreeSet<String> =
        state.patches.selectable.iter().map(|m| m.name.clone()).collect();
    let mode = SeasonMode::from_key(&state.store.text("season_mode"));
    let season = current_season(&state.patches.chosen);
    apply_mode(&mut state.patches.chosen, &available, mode, season);
    store_game_patches(state);
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
    let current_version = state.installation.info().map(|info| info.version.clone());
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
        let mut addons = false;
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
                    } else {
                        addons = true;
                    }
                    result.push(format!("✓ {name}"));
                }
                Err(e) => result.push(format!("✗ {name}: {e}")),
            }
        }
        if let Ok(mut s) = status.lock() {
            s.running = false;
            s.installed_game = game_dir.clone();
            s.installed_update = update_dir.clone();
            s.text = format!("{} {}", tr(&lang, "Install finished:"), result.join("  "));
            // GitHub #28: add-ons on a game below the update that mounts them.
            let version = update_dir
                .as_ref()
                .or(game_dir.as_ref())
                .and_then(|dir| crate::core::game::GameFolder::read(dir).app_version().map(str::to_string))
                .or(current_version);
            if let Some(version) = version.filter(|v| addons && crate::core::game::addons_need_update(v)) {
                let need = crate::core::game::ADDON_MIN_VERSION.trim_start_matches('0');
                s.text.push('\n');
                s.text.push_str(&tr_fmt(
                    &lang,
                    "Add-ons need the game's update {} or later (this game is {}): install the update PKG too.",
                    &[need, &version],
                ));
            }
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

#[cfg(test)]
mod season_tests {
    use super::*;

    #[test]
    fn the_drop_down_lists_off_then_the_seasons_of_the_patch_file() {
        let names = ["Offline Seasons", "Offline Seasons - Season 14", "Event: Pandemonium", "Offline Seasons - Season 30"];
        let numbers = season_numbers(names.into_iter());
        assert_eq!(numbers, ["14", "30"]);
        let choices = season_choices(&numbers, "en");
        let shown: Vec<String> = choices.iter().map(ToString::to_string).collect();
        assert_eq!(shown, ["Off", "Season 14", "Season 30"]);
        assert_eq!(choices[0].number, None);
        assert_eq!(choices[2].number.as_deref(), Some("30"));
    }

    #[test]
    fn picking_a_season_keeps_exactly_one_season_and_leaves_the_other_patches() {
        let mut chosen: BTreeSet<String> = ["Event: Pandemonium", "Offline Seasons - Season 14", "Offline: no sign-in warning"]
            .into_iter()
            .map(String::from)
            .collect();
        pick_season(&mut chosen, Some("30"));
        assert!(chosen.contains("Offline Seasons - Season 30"));
        assert!(!chosen.contains("Offline Seasons - Season 14"));
        assert!(chosen.contains("Event: Pandemonium") && chosen.contains("Offline: no sign-in warning"));
        pick_season(&mut chosen, None);
        assert!(!chosen.iter().any(|n| n.starts_with(SEASON_PREFIX)));
        assert_eq!(chosen.len(), 2);
    }
}
