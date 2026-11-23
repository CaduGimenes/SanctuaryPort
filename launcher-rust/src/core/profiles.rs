//! Player profiles for local co-op (GitHub #93, #86): a profile is a named save folder,
//! `<user>/savedata/<id>/`, listed in `<user>/profiles.txt` as `id<TAB>name` lines. The runtime
//! (`src/runtime_profiles.c`) reads the same list and adds to it when a player makes a profile
//! in the game. Each controller the launcher has seen plays the profile chosen for it here
//! (`pad_profiles`), whatever order the players join in, or asks in the game (`ask`).
//!
//! Ids are numbers, so the saves players 1..4 had before profiles are profiles 1..4 as they are.

use std::fs;
use std::io;
use std::path::{Path, PathBuf};

/// The list in the saves folder.
pub const LIST_FILE: &str = "profiles.txt";
/// A controller whose player chooses a profile in the game when it joins.
pub const ASK: &str = "ask";
/// The setting with the controllers and their profiles: `{key: PadRecord}`.
pub const PADS_KEY: &str = "pad_profiles";
/// The keyboard and mouse's record key and guid.
pub const KEYBOARD: &str = "keyboard";
/// A PS4 user name: at most 16 bytes of UTF-8.
pub const NAME_BYTES: usize = 16;

#[derive(Debug, Clone, PartialEq, Eq)]
pub struct Profile {
    pub id: String,
    /// As typed: limited to [`NAME_BYTES`], spaces kept while typing; the file has it trimmed.
    pub name: String,
}

impl Profile {
    /// The name as the game shows it: trimmed, "Player <id>" when empty.
    pub fn shown_name(&self) -> String {
        let name = clean_name(&self.name);
        if name.is_empty() {
            format!("Player {}", self.id)
        } else {
            name
        }
    }
}

/// At most [`NAME_BYTES`] bytes cut between characters, no tabs or line breaks; spaces kept (a
/// field being typed into).
pub fn limit_name(name: &str) -> String {
    let mut out = String::new();
    for c in name.chars().filter(|c| !c.is_control()) {
        if out.len() + c.len_utf8() > NAME_BYTES {
            break;
        }
        out.push(c);
    }
    out
}

/// The name as the runtime keeps it: leading spaces off, [`limit_name`], trailing spaces off.
pub fn clean_name(name: &str) -> String {
    limit_name(name.trim_start()).trim_end().to_string()
}

fn valid_id(id: &str) -> bool {
    !id.is_empty() && id.len() < 16 && id.bytes().all(|b| b.is_ascii_digit())
}

fn id_number(id: &str) -> u64 {
    id.parse().unwrap_or(0)
}

pub fn list_path(user_dir: &Path) -> PathBuf {
    user_dir.join(LIST_FILE)
}

fn save_folders(user_dir: &Path) -> Vec<String> {
    let mut ids: Vec<String> = fs::read_dir(user_dir.join("savedata"))
        .into_iter()
        .flatten()
        .flatten()
        .filter(|e| e.path().is_dir())
        .filter_map(|e| e.file_name().into_string().ok())
        .filter(|name| valid_id(name))
        .collect();
    ids.sort_by_key(|id| id_number(id));
    ids
}

/// Parses the list; lines that are not `id<TAB>name` with a number id are skipped, as the
/// runtime does.
pub fn parse(text: &str) -> Vec<Profile> {
    let mut list: Vec<Profile> = Vec::new();
    for line in text.lines() {
        let Some((id, name)) = line.trim_end_matches('\r').split_once('\t') else {
            continue;
        };
        if valid_id(id) && !list.iter().any(|p| p.id == id) {
            list.push(Profile {
                id: id.to_string(),
                name: clean_name(name),
            });
        }
    }
    list
}

/// The first list, from what there was before profiles (as the runtime makes it): profile 1 with
/// the old Player name (`player_name`), and every other numbered save folder as "Player N".
pub fn first_list(user_dir: &Path, player_name: &str) -> Vec<Profile> {
    let first = clean_name(player_name);
    let mut list = vec![Profile {
        id: "1".to_string(),
        name: if first.is_empty() {
            "Player".to_string()
        } else {
            first
        },
    }];
    for id in save_folders(user_dir).into_iter().filter(|id| id != "1") {
        list.push(Profile {
            name: format!("Player {id}"),
            id,
        });
    }
    list
}

/// The list of the saves folder; made (and written) from the saves there are when missing or
/// when none is left in it (emptied by hand), so there is always a profile for player 1.
pub fn read(user_dir: &Path, player_name: &str) -> io::Result<Vec<Profile>> {
    let listed = match fs::read_to_string(list_path(user_dir)) {
        Ok(text) => parse(&text),
        Err(e) if e.kind() == io::ErrorKind::NotFound => Vec::new(),
        Err(e) => return Err(e),
    };
    if !listed.is_empty() {
        return Ok(listed);
    }
    let list = first_list(user_dir, player_name);
    write(user_dir, &list)?;
    Ok(list)
}

/// Writes the list (through a temporary file, so the game never reads half of it).
pub fn write(user_dir: &Path, list: &[Profile]) -> io::Result<()> {
    fs::create_dir_all(user_dir)?;
    let text: String = list
        .iter()
        .map(|p| format!("{}\t{}\n", p.id, clean_name(&p.name)))
        .collect();
    let path = list_path(user_dir);
    let temp = user_dir.join(format!("{LIST_FILE}.tmp"));
    fs::write(&temp, text)?;
    fs::rename(&temp, &path)
}

/// The id of a new profile: above every listed profile and every save folder, so it never
/// takes over saves left behind.
pub fn next_id(user_dir: &Path, list: &[Profile]) -> String {
    let top = list
        .iter()
        .map(|p| id_number(&p.id))
        .chain(save_folders(user_dir).iter().map(|id| id_number(id)))
        .max()
        .unwrap_or(0);
    (top + 1).to_string()
}

/// Adds a profile named `name` ("Player <id>" when empty) and writes the list; its id.
pub fn add(user_dir: &Path, list: &mut Vec<Profile>, name: &str) -> io::Result<String> {
    let id = next_id(user_dir, list);
    let name = clean_name(name);
    list.push(Profile {
        name: if name.is_empty() {
            format!("Player {id}")
        } else {
            name
        },
        id: id.clone(),
    });
    write(user_dir, list)?;
    Ok(id)
}

/// Renames profile `id` (kept as typed in `list`, trimmed in the file) and writes the list.
pub fn rename(user_dir: &Path, list: &mut [Profile], id: &str, name: &str) -> io::Result<()> {
    if let Some(p) = list.iter_mut().find(|p| p.id == id) {
        p.name = limit_name(name);
    }
    write(user_dir, list)
}

/// Deletes profile `id`: its line, then its save folder.
pub fn delete(user_dir: &Path, list: &mut Vec<Profile>, id: &str) -> io::Result<()> {
    list.retain(|p| p.id != id);
    write(user_dir, list)?;
    let folder = user_dir.join("savedata").join(id);
    match fs::remove_dir_all(&folder) {
        Err(e) if e.kind() != io::ErrorKind::NotFound => Err(e),
        _ => Ok(()),
    }
}

// --- Controllers and their profiles ---

/// A controller the launcher has seen, as `pad_profiles` keeps it.
#[derive(Debug, Clone, PartialEq, Eq, serde::Serialize, serde::Deserialize)]
pub struct PadRecord {
    pub name: String,
    pub guid: String,
    #[serde(default)]
    pub serial: String,
    #[serde(default)]
    pub path: String,
    /// A profile id, or [`ASK`].
    pub profile: String,
}

/// The controllers of the `pad_profiles` setting, by key; the keyboard always has one.
pub fn pad_records(value: &serde_json::Value) -> Vec<(String, PadRecord)> {
    let mut records: Vec<(String, PadRecord)> = value
        .as_object()
        .into_iter()
        .flatten()
        .filter_map(|(key, record)| {
            serde_json::from_value(record.clone())
                .ok()
                .map(|record| (key.clone(), record))
        })
        .collect();
    if !records.iter().any(|(key, _)| key == KEYBOARD) {
        records.insert(
            0,
            (
                KEYBOARD.to_string(),
                PadRecord {
                    name: "Keyboard and mouse".to_string(),
                    guid: KEYBOARD.to_string(),
                    serial: String::new(),
                    path: String::new(),
                    profile: ASK.to_string(),
                },
            ),
        );
    }
    records
}

pub fn pad_value(records: &[(String, PadRecord)]) -> serde_json::Value {
    serde_json::Value::Object(
        records
            .iter()
            .filter_map(|(key, record)| {
                serde_json::to_value(record)
                    .ok()
                    .map(|value| (key.clone(), value))
            })
            .collect(),
    )
}

/// The record of a controller, matched as the runtime matches it (`src/runtime_profiles.c`): by
/// serial; else by device path when either has no serial; else by model (guid) when one record
/// alone has it. `taken`: records another connected controller already is.
pub fn find_pad(
    records: &[(String, PadRecord)],
    serial: &str,
    path: &str,
    guid: &str,
    taken: &[usize],
) -> Option<usize> {
    let pads = || {
        records
            .iter()
            .enumerate()
            .filter(|(i, (key, _))| key != KEYBOARD && !taken.contains(i))
    };
    if !serial.is_empty() {
        if let Some((i, _)) = pads().find(|(_, (_, r))| r.serial == serial) {
            return Some(i);
        }
    }
    if !path.is_empty() {
        if let Some((i, _)) =
            pads().find(|(_, (_, r))| r.path == path && (r.serial.is_empty() || serial.is_empty()))
        {
            return Some(i);
        }
    }
    let same: Vec<usize> = pads()
        .filter(|(_, (_, r))| !guid.is_empty() && r.guid == guid)
        .map(|(i, _)| i)
        .collect();
    match same.as_slice() {
        [i] if serial.is_empty() || records[*i].1.serial.is_empty() => Some(*i),
        _ => None,
    }
}

/// Remembers a connected controller: its record, updated (name, path, a serial it now reports),
/// or a new one that asks. `taken`: the records of the other controllers connected now. Answers
/// whether the records changed, and the controller's record.
pub fn remember(
    records: &mut Vec<(String, PadRecord)>,
    name: &str,
    guid: &str,
    serial: &str,
    path: &str,
    taken: &[usize],
) -> (bool, usize) {
    if let Some(i) = find_pad(records, serial, path, guid, taken) {
        let record = &mut records[i].1;
        let before = record.clone();
        record.name = name.to_string();
        if !path.is_empty() {
            record.path = path.to_string();
        }
        if record.serial.is_empty() {
            record.serial = serial.to_string();
        }
        return (*record != before, i);
    }
    let key = if !serial.is_empty() {
        format!("serial:{serial}")
    } else if !path.is_empty() {
        format!("path:{path}")
    } else {
        let n = records.iter().filter(|(_, r)| r.guid == guid).count();
        format!("guid:{guid}:{n}")
    };
    records.push((
        key,
        PadRecord {
            name: name.to_string(),
            guid: guid.to_string(),
            serial: serial.to_string(),
            path: path.to_string(),
            profile: ASK.to_string(),
        },
    ));
    (true, records.len() - 1)
}

/// Whether two records could be taken for each other: same model, and neither has a serial.
pub fn may_mix_up(records: &[(String, PadRecord)], index: usize) -> bool {
    let record = &records[index].1;
    record.serial.is_empty()
        && records.iter().enumerate().any(|(i, (key, r))| {
            i != index && key != KEYBOARD && r.guid == record.guid && r.serial.is_empty()
        })
}

/// `SANCTUARY_PAD_PROFILES`: "profile<TAB>guid<TAB>serial<TAB>path" per controller with a profile.
pub fn pads_env(records: &[(String, PadRecord)]) -> String {
    let clean = |s: &str| s.replace(['\t', '\n', '\r'], " ");
    records
        .iter()
        .filter(|(_, r)| r.profile != ASK && valid_id(&r.profile))
        .map(|(_, r)| {
            format!(
                "{}\t{}\t{}\t{}",
                r.profile,
                clean(&r.guid),
                clean(&r.serial),
                clean(&r.path)
            )
        })
        .collect::<Vec<_>>()
        .join("\n")
}

#[cfg(test)]
mod tests {
    use super::*;

    fn dir() -> tempfile::TempDir {
        tempfile::tempdir().unwrap()
    }

    #[test]
    fn names_are_ps4_user_names() {
        assert_eq!(clean_name("  Ação Bárbaro Supremo"), "Ação Bárbaro");
        assert_eq!(clean_name("Deckard\tCain\n"), "DeckardCain");
        assert_eq!(limit_name("Deckard "), "Deckard ");
        assert_eq!(clean_name("   "), "");
    }

    #[test]
    fn the_first_list_keeps_the_saves_there_were() {
        let dir = dir();
        for id in ["1", "3", "12", "backup"] {
            fs::create_dir_all(dir.path().join("savedata").join(id)).unwrap();
        }
        let list = read(dir.path(), "Stavros").unwrap();
        assert_eq!(
            fs::read_to_string(list_path(dir.path())).unwrap(),
            "1\tStavros\n3\tPlayer 3\n12\tPlayer 12\n"
        );
        assert_eq!(list.len(), 3);
        // Read again: the file as it is, the old name no longer used.
        assert_eq!(read(dir.path(), "Other").unwrap(), list);
    }

    #[test]
    fn an_empty_list_is_made_again() {
        let dir = dir();
        fs::create_dir_all(dir.path().join("savedata/2")).unwrap();
        fs::write(list_path(dir.path()), "bad line\n").unwrap();
        let list = read(dir.path(), "Stavros").unwrap();
        let ids: Vec<&str> = list.iter().map(|p| p.id.as_str()).collect();
        assert_eq!(ids, ["1", "2"]);
        assert_eq!(
            fs::read_to_string(list_path(dir.path())).unwrap(),
            "1\tStavros\n2\tPlayer 2\n"
        );
    }

    #[test]
    fn a_list_from_the_game_is_read_as_the_runtime_reads_it() {
        let list = parse("1\tStavros\n3\tLeah\r\n3\tTwice\nbad line\nx\tNot an id\n13\t\n");
        let ids: Vec<&str> = list.iter().map(|p| p.id.as_str()).collect();
        assert_eq!(ids, ["1", "3", "13"]);
        assert_eq!(list[1].name, "Leah");
        assert_eq!(list[2].shown_name(), "Player 13");
    }

    #[test]
    fn new_profiles_never_take_a_folder_left_behind() {
        let dir = dir();
        fs::create_dir_all(dir.path().join("savedata/7")).unwrap();
        let mut list = read(dir.path(), "").unwrap();
        assert_eq!(list[0].name, "Player");
        let id = add(dir.path(), &mut list, " Leah ").unwrap();
        assert_eq!(id, "8");
        assert_eq!(list.last().unwrap().name, "Leah");
        rename(dir.path(), &mut list, "8", "Leah Adria ").unwrap();
        assert_eq!(list.last().unwrap().name, "Leah Adria ");
        assert!(fs::read_to_string(list_path(dir.path()))
            .unwrap()
            .contains("8\tLeah Adria\n"));
    }

    #[test]
    fn deleting_a_profile_deletes_its_saves() {
        let dir = dir();
        fs::create_dir_all(dir.path().join("savedata/1/CUSA00242/autosave")).unwrap();
        fs::create_dir_all(dir.path().join("savedata/2/CUSA00242/autosave")).unwrap();
        let mut list = read(dir.path(), "").unwrap();
        delete(dir.path(), &mut list, "2").unwrap();
        assert!(!dir.path().join("savedata/2").exists());
        assert!(dir.path().join("savedata/1/CUSA00242/autosave").exists());
        assert_eq!(
            parse(&fs::read_to_string(list_path(dir.path())).unwrap()),
            list
        );
    }

    #[test]
    fn controllers_are_known_again_as_the_runtime_knows_them() {
        let mut records = pad_records(&serde_json::json!({}));
        assert_eq!(records[0].0, KEYBOARD);
        assert_eq!(remember(&mut records, "DualSense", "guidA", "SER1", "/dev/a", &[]), (true, 1));
        assert_eq!(remember(&mut records, "Xbox", "guidB", "", "/dev/hidraw4", &[]), (true, 2));
        // Seen again: by serial on another port, by path; nothing new.
        assert_eq!(remember(&mut records, "DualSense", "guidA", "SER1", "/dev/b", &[]), (true, 1));
        assert_eq!(remember(&mut records, "Xbox", "guidB", "", "/dev/hidraw4", &[]), (false, 2));
        assert_eq!(records.len(), 3);
        assert_eq!(records[1].1.path, "/dev/b");
        // Another of that model with its own serial is a new controller.
        assert_eq!(remember(&mut records, "DualSense", "guidA", "SER9", "/dev/c", &[]).1, 3);
        // An Xbox pad on another port, no serial: the only one of its model, so the same one.
        assert_eq!(find_pad(&records, "", "/dev/hidraw7", "guidB", &[]), Some(2));
        // A second one of that model without a serial, connected beside the first: a new one,
        // and the two may be mixed up on another start.
        assert_eq!(remember(&mut records, "Xbox", "guidB", "", "/dev/hidraw8", &[2]), (true, 4));
        assert!(may_mix_up(&records, 2) && !may_mix_up(&records, 1));
        assert_eq!(find_pad(&records, "", "/dev/hidraw9", "guidB", &[]), None);

        records[1].1.profile = "3".into();
        records[0].1.profile = "1".into();
        let env = pads_env(&records);
        assert_eq!(env, "1\tkeyboard\t\t\n3\tguidA\tSER1\t/dev/b");
        let value = pad_value(&records);
        assert_eq!(pad_records(&value), records);
    }
}
