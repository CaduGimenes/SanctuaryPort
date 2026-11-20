//! Checks on a game folder (dump or update), its patch profile, and adoption of the system files
//! that PKG extractors write to `Sc0/` beside the game files.

use std::fs::{self, File};
use std::io::{self, Read};
use std::path::{Path, PathBuf};

use indexmap::IndexMap;

use super::sfo::{load_sfo, read_sfo, SfoReadError, SfoValue};

const APP_NAME: &str = "SanctuaryPort";
const PARAM_SFO: &str = "sce_sys/param.sfo";

/// What a file the port needs looks like on disk.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum FileState {
    Ok,
    Missing,
    Folder,
    Empty,
    Unreadable,
    Invalid,
}

/// The state of a file the port needs, from one stat: an empty one (a copy that failed, a
/// placeholder made by hand) is as good as missing.
pub fn file_state(path: &Path) -> FileState {
    let meta = match fs::metadata(path) {
        Ok(meta) => meta,
        Err(e)
            if matches!(
                e.kind(),
                io::ErrorKind::NotFound | io::ErrorKind::NotADirectory
            ) =>
        {
            return FileState::Missing
        }
        Err(_) => return FileState::Unreadable,
    };
    if meta.is_dir() {
        return FileState::Folder;
    }
    if !meta.is_file() {
        return FileState::Missing;
    }
    if meta.len() == 0 {
        return FileState::Empty;
    }
    let mut byte = [0u8; 1];
    match File::open(path).and_then(|mut f| f.read(&mut byte)) {
        Ok(_) => FileState::Ok,
        Err(_) => FileState::Unreadable,
    }
}

/// [`file_state`] for a `param.sfo`, which must also parse: `Invalid` when it does not.
pub fn sfo_state(path: &Path) -> FileState {
    match file_state(path) {
        FileState::Ok => {}
        state => return state,
    }
    match load_sfo(path) {
        Ok(_) => FileState::Ok,
        Err(SfoReadError::Io(_)) => FileState::Unreadable,
        Err(SfoReadError::Invalid(_)) => FileState::Invalid,
    }
}

/// A required file and the check it must pass.
pub type Requirement = (&'static str, fn(&Path) -> FileState);

/// The files the port loads from a dump, each with the check it must pass.
pub static REQUIRED_GAME_FILES: &[Requirement] = &[
    ("eboot.bin", file_state),
    (PARAM_SFO, sfo_state),
    ("sce_module/libc.prx", file_state),
    ("sce_module/libSceFios2.prx", file_state),
];

/// What an update folder needs: the first two of the dump's files.
pub static REQUIRED_UPDATE_FILES: &[Requirement] =
    &[("eboot.bin", file_state), (PARAM_SFO, sfo_state)];

/// The required files that do not pass their check in the folder: an incomplete dump, a
/// placeholder made by hand, a file this user may not read, a `param.sfo` that is not one.
pub fn missing_game_files(folder: &Path, requirements: &[Requirement]) -> Vec<(String, FileState)> {
    requirements
        .iter()
        .filter_map(|(name, check)| match check(&folder.join(name)) {
            FileState::Ok => None,
            state => Some(((*name).to_string(), state)),
        })
        .collect()
}

/// Whether two paths name one folder (links and 8.3 spellings included).
pub fn same_folder(a: impl AsRef<Path>, b: impl AsRef<Path>) -> bool {
    fn resolved(path: &Path) -> String {
        let real = fs::canonicalize(path).unwrap_or_else(|_| path.to_path_buf());
        let text = real.to_string_lossy().into_owned();
        if cfg!(windows) {
            text.to_lowercase()
        } else {
            text
        }
    }
    resolved(a.as_ref()) == resolved(b.as_ref())
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub struct GameInfo {
    pub title: String,
    pub version: String,
}

fn strip_marks(title: &str) -> String {
    title.replace(['\u{2122}', '\u{ae}'], "")
}

/// A game folder's facts, from one read of its `sce_sys/param.sfo`: an unreadable or missing
/// file answers nothing.
#[derive(Debug, Clone)]
pub struct GameFolder {
    values: IndexMap<String, SfoValue>,
    has_eboot: bool,
    /// The version the `eboot.bin` itself is, when its size is a known Diablo III build.
    eboot_version: Option<&'static str>,
}

/// SanctuaryPort: Diablo III's eboot.bin sizes and the version they are. The builds are the same in
/// every region (1.00: sha256 2696c9a4... in CUSA00242 and CUSA00243; 1.47: 680387c8... in
/// CUSA00243 and CUSA00434). A 1.47 update copied over the base game keeps the base's
/// param.sfo (APP_VER 01.00), so the eboot tells the version then (GitHub #30).
const DIABLO_EBOOT_SIZES: [(u64, &str); 2] = [(19_773_126, "01.00"), (22_646_255, "01.47")];

/// Diablo III eboots known by their SHA-256, for builds whose size is not in the list above: the
/// European 1.46 merged into the base package (CUSA00433, GitHub #38, whose param.sfo says 01.00).
const DIABLO_EBOOT_HASHES: [(&str, &str); 3] = [
    ("2696c9a49fd3cece23664b2570cb5635742af0d0a1f1f250cff2f06047d97373", "01.00"),
    ("a2bd5058a887f624b596b4d640771162fb9c4afdfad812d59e7a58c29f7c744b", "01.46"),
    ("680387c887625dbeaef0c46fac54a893fcb45eac428c85632acb06e6062a3176", "01.47"),
];

/// Only eboots of a Diablo III's size range are hashed (about 20 MB, once per file and change).
const HASHED_EBOOT_SIZES: std::ops::RangeInclusive<u64> = 15_000_000..=40_000_000;

fn eboot_version(eboot: &Path) -> Option<&'static str> {
    let meta = std::fs::metadata(eboot).ok()?;
    let size = meta.len();
    if let Some((_, version)) = DIABLO_EBOOT_SIZES.iter().find(|(s, _)| *s == size) {
        return Some(version);
    }
    if !HASHED_EBOOT_SIZES.contains(&size) {
        return None;
    }
    let hash = eboot_hash(eboot, size, meta.modified().ok())?;
    DIABLO_EBOOT_HASHES.iter().find(|(h, _)| *h == hash).map(|(_, v)| *v)
}

/// The eboot's SHA-256 in hex, cached by path, size and modification time: the folder is read
/// again on every settings change.
fn eboot_hash(eboot: &Path, size: u64, modified: Option<std::time::SystemTime>) -> Option<String> {
    use sha2::{Digest, Sha256};
    use std::sync::{Mutex, OnceLock};
    type Key = (std::path::PathBuf, u64, Option<std::time::SystemTime>);
    static CACHE: OnceLock<Mutex<Vec<(Key, String)>>> = OnceLock::new();
    let key: Key = (eboot.to_path_buf(), size, modified);
    let cache = CACHE.get_or_init(|| Mutex::new(Vec::new()));
    if let Some((_, hash)) = cache.lock().ok()?.iter().find(|(k, _)| *k == key) {
        return Some(hash.clone());
    }
    let mut hasher = Sha256::new();
    let mut file = std::fs::File::open(eboot).ok()?;
    std::io::copy(&mut file, &mut hasher).ok()?;
    let hash: String = hasher.finalize().iter().map(|b| format!("{b:02x}")).collect();
    let mut cache = cache.lock().ok()?;
    if cache.len() > 16 {
        cache.remove(0);
    }
    cache.push((key, hash.clone()));
    Some(hash)
}

impl GameFolder {
    pub fn read(p: &Path) -> Self {
        let values = if p.as_os_str().is_empty() {
            IndexMap::new()
        } else {
            read_sfo(&p.join(PARAM_SFO)).unwrap_or_default()
        };
        let root = if p.as_os_str().is_empty() {
            Path::new(".")
        } else {
            p
        };
        let eboot = root.join("eboot.bin");
        GameFolder {
            values,
            has_eboot: eboot.is_file(),
            eboot_version: eboot_version(&eboot),
        }
    }

    fn text(&self, key: &str) -> Option<&str> {
        match self.values.get(key) {
            Some(SfoValue::Text(text)) => Some(text),
            _ => None,
        }
    }

    pub fn title_id(&self) -> Option<&str> {
        self.text("TITLE_ID")
    }

    /// `APP_VER` ("01.02"); a known eboot that is newer than it wins (an update copied over the
    /// base game).
    pub fn app_version(&self) -> Option<&str> {
        let sfo = self.text("APP_VER");
        match (sfo, self.eboot_version) {
            (Some(sfo), Some(eboot)) if eboot > sfo => Some(eboot),
            (None, Some(eboot)) => Some(eboot),
            _ => sfo,
        }
    }

    /// The patch profile, by the title ID or the title: any region and version of a known game.
    pub fn profile(&self) -> Option<&'static GameProfile> {
        profile_for(&self.values)
    }

    /// Title and app version, or `None` without `eboot.bin`.
    pub fn info(&self) -> Option<GameInfo> {
        if !self.has_eboot {
            return None;
        }
        let title = self.text("TITLE").unwrap_or(APP_NAME);
        Some(GameInfo {
            title: strip_marks(title).trim().to_string(),
            version: self.app_version().unwrap_or("?").to_string(),
        })
    }

    /// Whether the folder has a readable `param.sfo`.
    fn has_sfo(&self) -> bool {
        !self.values.is_empty()
    }
}

fn profile_for(values: &IndexMap<String, SfoValue>) -> Option<&'static GameProfile> {
    let text = |key| match values.get(key) {
        Some(SfoValue::Text(text)) => Some(text.as_str()),
        _ => None,
    };
    game_profile(text("TITLE_ID"), text("TITLE")).or_else(|| {
        // A release under a title ID not listed whose title is in another script: the content ID
        // label is the same in every region (UP0002-CUSA00243_00-D3ULTIMATEEVIL00).
        let content = text("CONTENT_ID")?.to_uppercase();
        GAMES
            .iter()
            .find(|game| game.labels.iter().any(|label| content.contains(label)))
    })
}

/// Diablo III: the first update the port mounts add-ons with (GitHub #28). 1.00 has no
/// Necromancer, and some 1.00 eboots (CUSA00433) load a DLC loader of their own the port lacks.
/// 1.46 is the last European update, 1.47 the American and Brazilian one.
pub const ADDON_MIN_VERSION: &str = "01.46";

/// Whether add-ons need a newer update than `version` ("01.00"); an unreadable version says no.
pub fn addons_need_update(version: &str) -> bool {
    let number = |text: &str| -> Option<u32> {
        let (major, minor) = text.trim().split_once('.')?;
        Some(major.parse::<u32>().ok()? * 100 + minor.parse::<u32>().ok()?)
    };
    match (number(version), number(ADDON_MIN_VERSION)) {
        (Some(have), Some(need)) => have < need,
        _ => false,
    }
}

/// Whether add-ons are installed where the game looks for them (run.py: `addcont` beside the game
/// folder, and `Games/PS4/addcont` in the home and port folders): any folder inside.
pub fn has_addons(game_folder: &Path, port_dir: &Path) -> bool {
    let mut roots = vec![port_dir.join("Games").join("PS4").join("addcont")];
    if let Some(parent) = game_folder.parent() {
        roots.push(parent.join("addcont"));
    }
    if let Some(home) = std::env::var_os("HOME").or_else(|| std::env::var_os("USERPROFILE")) {
        roots.push(PathBuf::from(home).join("Games").join("PS4").join("addcont"));
    }
    any_addon(&roots)
}

/// Whether any of `roots` (`addcont` folders) holds a non-empty title folder.
fn any_addon(roots: &[PathBuf]) -> bool {
    roots.iter().any(|root| {
        std::fs::read_dir(root).is_ok_and(|entries| {
            entries.flatten().any(|title| {
                std::fs::read_dir(title.path()).is_ok_and(|mut inside| inside.next().is_some())
            })
        })
    })
}

/// A game the built-in patches know: its title IDs, patch file, metadata app version, and the
/// title that also recognises a release under an ID not listed.
#[derive(Debug, PartialEq, Eq)]
pub struct GameProfile {
    pub ids: &'static [&'static str],
    pub xml: &'static str,
    pub app_version: &'static str,
    pub title: &'static str,
    /// Content ID labels of the game in every region.
    pub labels: &'static [&'static str],
}

static GAMES: &[GameProfile] = &[GameProfile {
    ids: &["CUSA00243", "CUSA00242", "CUSA00434", "CUSA00433"],
    xml: "Diablo_III.xml",
    app_version: "01.47",
    title: "Diablo III",
    labels: &["D3ULTIMATEEVIL", "DIABLO3"],
}];

/// The profile of a known game, by title ID or by its title appearing in the SFO title. The IDs are
/// the releases seen so far; any other region is recognised by its title (or content ID).
pub fn game_profile(title_id: Option<&str>, title: Option<&str>) -> Option<&'static GameProfile> {
    let title = strip_marks(title.unwrap_or(""));
    GAMES.iter().find(|game| {
        title_id.is_some_and(|id| game.ids.contains(&id))
            || (!title.is_empty() && title.contains(game.title))
    })
}

fn same_name(a: &std::ffi::OsStr, b: &std::ffi::OsStr) -> bool {
    let (a, b) = (a.to_string_lossy(), b.to_string_lossy());
    if cfg!(windows) {
        a.to_lowercase() == b.to_lowercase()
    } else {
        a == b
    }
}

/// PKG extractors write the system files (`param.sfo`, `icon0.png`, `pic1.png`, `trophy/`) to
/// `Sc0/` beside the game files (`Image0/`), while the port reads them from `sce_sys/`: the Sc0
/// folder beside `folder` holding a SanctuaryPort `param.sfo`, while `folder` has none worth
/// keeping (an unreadable one is a different problem). For an update folder the title must be
/// that of `reference` (the base game); an Sc0 beside the base game itself (`base`) is
/// the base game's own unless the base game's `param.sfo` says another version.
pub fn system_files_beside(
    folder: impl AsRef<Path>,
    reference: Option<&GameFolder>,
    base: Option<&Path>,
) -> Option<PathBuf> {
    let reference = reference.filter(|r| r.has_sfo()).map(|r| &r.values);
    let folder = std::path::absolute(folder.as_ref()).ok()?;
    if matches!(
        sfo_state(&folder.join(PARAM_SFO)),
        FileState::Ok | FileState::Unreadable
    ) {
        return None;
    }
    let parent = folder.parent()?;
    let mut names = Vec::new();
    for entry in fs::read_dir(parent).ok()? {
        let entry = entry.ok()?;
        if entry.path().is_dir() && entry.file_name().to_string_lossy().to_lowercase() == "sc0" {
            names.push(entry.file_name());
        }
    }
    let [name] = names.as_slice() else {
        return None;
    };
    if same_name(name, folder.file_name()?) {
        return None;
    }
    let sibling = parent.join(name);
    let values = load_sfo(&sibling.join("param.sfo")).ok()?;
    profile_for(&values)?;
    if reference.is_some_and(|r| values.get("TITLE_ID") != r.get("TITLE_ID")) {
        return None;
    }
    if let Some(base) = base {
        let base_parent = std::path::absolute(base)
            .ok()?
            .parent()
            .map(Path::to_path_buf);
        let same_version = reference.is_none_or(|r| values.get("APP_VER") == r.get("APP_VER"));
        if base_parent.is_some_and(|p| same_folder(parent, p)) && same_version {
            return None;
        }
    }
    Some(sibling)
}

/// The files under `dir`, relative to `root`, as `/`-separated names. A folder that cannot
/// be listed is an error (a silent skip would adopt half a dump).
fn files_under(root: &Path, dir: &Path, out: &mut Vec<String>) -> io::Result<()> {
    for entry in fs::read_dir(dir)? {
        let entry = entry?;
        let path = entry.path();
        let kind = entry.file_type()?;
        if kind.is_dir() {
            files_under(root, &path, out)?;
        } else if kind.is_symlink() && path.is_dir() {
            continue;
        } else {
            let relative = path.strip_prefix(root).map_err(io::Error::other)?;
            let parts: Vec<_> = relative.iter().map(|p| p.to_string_lossy()).collect();
            out.push(parts.join("/"));
        }
    }
    Ok(())
}

fn copy_whole(from: &Path, to: &Path) -> io::Result<()> {
    let mut part = to.as_os_str().to_owned();
    part.push(".part");
    let part = PathBuf::from(part);
    let result = fs::copy(from, &part).and_then(|_| fs::rename(&part, to));
    if result.is_err() {
        let _ = fs::remove_file(&part);
    }
    result
}

/// Copies the files of `source` (an Sc0 folder) into `<game>/sce_sys`, keeping every usable file
/// the game already has there (empty ones, and a `param.sfo` that is not one, are replaced);
/// returns the relative names copied. Each file lands whole or not at all, and `param.sfo` goes
/// last: while it is missing, a copy that failed halfway is offered again.
pub fn adopt_system_files(source: &Path, game: &Path) -> io::Result<Vec<String>> {
    let target = game.join("sce_sys");
    let mut files = Vec::new();
    files_under(source, source, &mut files)?;
    files.sort_by(|a, b| ((a == "param.sfo"), a).cmp(&((b == "param.sfo"), b)));
    let mut copied = Vec::new();
    for relative in files {
        let destination = target.join(&relative);
        if destination.is_dir() {
            return Err(io::Error::other(format!(
                "{} is a folder: {} cannot replace it",
                destination.display(),
                source.join(&relative).display()
            )));
        }
        let state = if relative == "param.sfo" {
            sfo_state(&destination)
        } else {
            file_state(&destination)
        };
        if state == FileState::Ok {
            continue;
        }
        if let Some(parent) = destination.parent() {
            fs::create_dir_all(parent)?;
        }
        copy_whole(&source.join(&relative), &destination)?;
        copied.push(relative);
    }
    Ok(copied)
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::fs;

    fn sfo(entries: &[(&str, &str)]) -> Vec<u8> {
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

    fn diablo(version: &str) -> Vec<u8> {
        sfo(&[
            ("TITLE_ID", "CUSA00243"),
            ("APP_VER", version),
            ("TITLE", "Diablo\u{ae} III: Eternal Collection"),
        ])
    }

    fn put(root: &Path, name: &str, bytes: &[u8]) {
        let path = root.join(name);
        fs::create_dir_all(path.parent().unwrap()).unwrap();
        fs::write(path, bytes).unwrap();
    }

    fn complete_dump(root: &Path) {
        put(root, "eboot.bin", b"elf");
        put(root, "sce_sys/param.sfo", &diablo("01.00"));
        put(root, "sce_module/libc.prx", b"prx");
        put(root, "sce_module/libSceFios2.prx", b"prx");
    }

    #[test]
    fn an_empty_eboot_counts_as_missing() {
        let dir = tempfile::tempdir().unwrap();
        complete_dump(dir.path());
        put(dir.path(), "eboot.bin", b"");
        assert_eq!(
            missing_game_files(dir.path(), REQUIRED_GAME_FILES),
            vec![("eboot.bin".to_string(), FileState::Empty)]
        );
        assert_eq!(file_state(&dir.path().join("nothing")), FileState::Missing);
    }

    #[test]
    fn a_folder_named_param_sfo_is_reported_as_folder() {
        let dir = tempfile::tempdir().unwrap();
        complete_dump(dir.path());
        fs::remove_file(dir.path().join("sce_sys/param.sfo")).unwrap();
        fs::create_dir(dir.path().join("sce_sys/param.sfo")).unwrap();
        assert_eq!(
            missing_game_files(dir.path(), REQUIRED_GAME_FILES),
            vec![("sce_sys/param.sfo".to_string(), FileState::Folder)]
        );
    }

    #[test]
    fn a_param_sfo_that_is_not_one_is_invalid() {
        let dir = tempfile::tempdir().unwrap();
        complete_dump(dir.path());
        put(dir.path(), "sce_sys/param.sfo", b"definitely not an sfo");
        assert_eq!(
            sfo_state(&dir.path().join("sce_sys/param.sfo")),
            FileState::Invalid
        );
        assert_eq!(
            missing_game_files(dir.path(), REQUIRED_GAME_FILES),
            vec![("sce_sys/param.sfo".to_string(), FileState::Invalid)]
        );
    }

    #[test]
    fn an_update_copied_over_the_base_is_its_eboots_version() {
        // GitHub #30: the 1.47 eboot over a base whose param.sfo still says 01.00.
        let dir = tempfile::tempdir().unwrap();
        let eboot = fs::File::create(dir.path().join("eboot.bin")).unwrap();
        eboot.set_len(22_646_255).unwrap();
        put(dir.path(), "sce_sys/param.sfo", &sfo(&[("TITLE_ID", "CUSA00434"), ("APP_VER", "01.00")]));
        let folder = GameFolder::read(dir.path());
        assert_eq!(folder.app_version(), Some("01.47"));
        assert_eq!(folder.info().map(|info| info.version), Some("01.47".into()));
        // A real 1.00 eboot, or an eboot of an unknown size, keeps param.sfo's version.
        eboot.set_len(19_773_126).unwrap();
        assert_eq!(GameFolder::read(dir.path()).app_version(), Some("01.00"));
        eboot.set_len(5).unwrap();
        assert_eq!(GameFolder::read(dir.path()).app_version(), Some("01.00"));
    }

    #[test]
    fn an_eboot_of_an_unknown_size_is_known_by_its_hash() {
        // A file of a Diablo-sized length that is none of the known builds: hashed, not known,
        // so param.sfo's version stays; the hash is the file's real SHA-256.
        let dir = tempfile::tempdir().unwrap();
        let eboot = dir.path().join("eboot.bin");
        let file = fs::File::create(&eboot).unwrap();
        file.set_len(20_000_000).unwrap();
        let meta = fs::metadata(&eboot).unwrap();
        let hash = eboot_hash(&eboot, meta.len(), meta.modified().ok()).unwrap();
        // sha256 of 20,000,000 zero bytes.
        assert_eq!(hash.len(), 64);
        assert_eq!(eboot_hash(&eboot, meta.len(), meta.modified().ok()).unwrap(), hash, "cached");
        assert_eq!(eboot_version(&eboot), None);
        put(dir.path(), "sce_sys/param.sfo", &sfo(&[("TITLE_ID", "CUSA00433"), ("APP_VER", "01.00")]));
        assert_eq!(GameFolder::read(dir.path()).app_version(), Some("01.00"));
        assert!(DIABLO_EBOOT_HASHES.iter().any(|(_, v)| *v == "01.46"));
    }

    #[test]
    fn a_game_folder_reads_param_sfo_once() {
        let dir = tempfile::tempdir().unwrap();
        put(dir.path(), "eboot.bin", b"elf");
        put(
            dir.path(),
            "sce_sys/param.sfo",
            &sfo(&[("TITLE_ID", "CUSA00243"), ("APP_VER", "01.47")]),
        );
        let folder = GameFolder::read(dir.path());
        fs::remove_file(dir.path().join("sce_sys/param.sfo")).unwrap();
        assert_eq!(folder.title_id(), Some("CUSA00243"));
        assert_eq!(folder.app_version(), Some("01.47"));
        assert!(folder.profile().is_some());
        assert_eq!(folder.info().map(|info| info.version), Some("01.47".into()));
        let none = GameFolder::read(Path::new(""));
        assert_eq!(none.title_id(), None);
        assert!(none.profile().is_none());
    }

    #[test]
    fn game_info_strips_the_marks() {
        let dir = tempfile::tempdir().unwrap();
        assert_eq!(GameFolder::read(dir.path()).info(), None);
        put(dir.path(), "eboot.bin", b"elf");
        assert_eq!(
            GameFolder::read(dir.path()).info(),
            Some(GameInfo {
                title: "SanctuaryPort".into(),
                version: "?".into()
            })
        );
        put(
            dir.path(),
            "sce_sys/param.sfo",
            &sfo(&[
                ("TITLE", " Diablo\u{ae} III\u{2122} "),
                ("APP_VER", "01.02"),
            ]),
        );
        assert_eq!(
            GameFolder::read(dir.path()).info(),
            Some(GameInfo {
                title: "Diablo III".into(),
                version: "01.02".into()
            })
        );
    }

    #[test]
    fn profile_matches_by_id_or_title() {
        let by_id = game_profile(Some("CUSA00242"), None).unwrap();
        assert_eq!(by_id.xml, "Diablo_III.xml");
        assert_eq!(by_id.app_version, "01.47");
        let by_title = game_profile(
            Some("CUSA99999"),
            Some("Diablo\u{ae} III: Reaper of Souls \u{2013} Ultimate Evil Edition"),
        );
        assert_eq!(by_title.map(|p| p.title), Some("Diablo III"));
        assert!(game_profile(Some("CUSA99999"), Some("Another Game")).is_none());
        assert!(game_profile(None, None).is_none());

        let dir = tempfile::tempdir().unwrap();
        put(dir.path(), "sce_sys/param.sfo", &diablo("01.00"));
        let folder = GameFolder::read(dir.path());
        assert_eq!(folder.title_id(), Some("CUSA00243"));
        assert!(folder.profile().is_some());
        assert!(GameFolder::read(Path::new("")).profile().is_none());
    }

    #[test]
    fn sc0_beside_a_pkg_extraction_is_found_only_without_a_usable_param_sfo() {
        let root = tempfile::tempdir().unwrap();
        let image = root.path().join("Image0");
        put(&image, "eboot.bin", b"elf");
        put(root.path(), "Sc0/param.sfo", &diablo("01.00"));
        let sc0 = system_files_beside(&image, None, None).unwrap();
        assert_eq!(sc0.file_name().unwrap(), "Sc0");

        let reference_dir = root.path().join("Ref");
        put(&reference_dir, "sce_sys/param.sfo", &diablo("01.00"));
        let reference = GameFolder::read(&reference_dir);
        assert!(system_files_beside(&image, Some(&reference), None).is_some());
        let foreign_dir = root.path().join("Foreign");
        put(
            &foreign_dir,
            "sce_sys/param.sfo",
            &sfo(&[("TITLE_ID", "CUSA00001")]),
        );
        let foreign = GameFolder::read(&foreign_dir);
        assert!(system_files_beside(&image, Some(&foreign), None).is_none());

        // The base game's own Sc0 when its version matches the base game's.
        assert!(system_files_beside(&image, Some(&reference), Some(image.as_path())).is_none());

        // A usable param.sfo in the folder means nothing to adopt.
        put(&image, "sce_sys/param.sfo", &diablo("01.00"));
        assert!(system_files_beside(&image, None, None).is_none());
        // An unusable one does not.
        put(&image, "sce_sys/param.sfo", b"junk");
        assert!(system_files_beside(&image, None, None).is_some());

        // An Sc0 that is not a SanctuaryPort game is left alone.
        let other = sfo(&[("TITLE_ID", "CUSA00001"), ("TITLE", "Other")]);
        put(root.path(), "Sc0/param.sfo", &other);
        assert!(system_files_beside(&image, None, None).is_none());
    }

    #[test]
    fn adoption_copies_missing_files_keeps_usable_ones_and_writes_param_sfo_last() {
        let root = tempfile::tempdir().unwrap();
        let source = root.path().join("Sc0");
        put(&source, "param.sfo", &diablo("01.00"));
        put(&source, "icon0.png", b"new icon");
        put(&source, "pic1.png", b"new pic");
        put(&source, "trophy/trophy00.trp", b"trophy");
        let game = root.path().join("Image0");
        put(&game, "sce_sys/icon0.png", b"my icon");
        put(&game, "sce_sys/pic1.png", b"");

        let copied = adopt_system_files(&source, &game).unwrap();
        assert_eq!(copied, vec!["pic1.png", "trophy/trophy00.trp", "param.sfo"]);
        assert_eq!(
            fs::read(game.join("sce_sys/icon0.png")).unwrap(),
            b"my icon"
        );
        assert_eq!(fs::read(game.join("sce_sys/pic1.png")).unwrap(), b"new pic");
        assert_eq!(
            fs::read(game.join("sce_sys/trophy/trophy00.trp")).unwrap(),
            b"trophy"
        );
        assert_eq!(sfo_state(&game.join("sce_sys/param.sfo")), FileState::Ok);
        let leftovers = fs::read_dir(game.join("sce_sys"))
            .unwrap()
            .filter_map(Result::ok)
            .filter(|e| e.file_name().to_string_lossy().ends_with(".part"))
            .count();
        assert_eq!(leftovers, 0);

        // Everything is usable now: a second run copies nothing.
        assert!(adopt_system_files(&source, &game).unwrap().is_empty());

        // A folder where a file belongs is an error.
        fs::remove_file(game.join("sce_sys/pic1.png")).unwrap();
        fs::create_dir(game.join("sce_sys/pic1.png")).unwrap();
        assert!(adopt_system_files(&source, &game).is_err());
        assert!(adopt_system_files(&root.path().join("nope"), &game).is_err());
    }
}

#[cfg(test)]
mod addon_tests {
    use super::*;

    #[test]
    fn add_ons_need_1_46_or_later() {
        assert!(addons_need_update("01.00"));
        assert!(addons_need_update("01.45"));
        assert!(!addons_need_update("01.46"));
        assert!(!addons_need_update("01.47"));
        assert!(!addons_need_update("?"));
    }

    #[test]
    fn add_ons_are_found_beside_the_game() {
        let dir = tempfile::tempdir().unwrap();
        let roots = [dir.path().join("addcont")];
        assert!(!any_addon(&roots));
        std::fs::create_dir_all(dir.path().join("addcont").join("CUSA00433")).unwrap();
        assert!(!any_addon(&roots), "an empty title folder holds no add-on");
        let label = dir.path().join("addcont").join("CUSA00433").join("NECROMANCERPACK0");
        std::fs::create_dir_all(&label).unwrap();
        assert!(any_addon(&roots));
    }
}
