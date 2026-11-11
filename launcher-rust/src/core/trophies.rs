//! Trophies: the icons in the game's trophy package, the unlocked flags and the tier of an icon.

use std::collections::{BTreeMap, BTreeSet};
use std::fs;
use std::io;
use std::path::Path;

use super::i18n::tr;

const MAGIC: [u8; 4] = [0xdc, 0xa2, 0x4d, 0x00];
const HEADER_SIZE: usize = 0x60;
const ENTRY_SIZE: usize = 0x30;
const MAX_ENTRIES: u32 = 4096;

fn be_u32(data: &[u8], at: usize) -> Option<u32> {
    let bytes = data.get(at..at.checked_add(4)?)?;
    Some(u32::from_be_bytes(bytes.try_into().ok()?))
}

fn be_u64(data: &[u8], at: usize) -> Option<u64> {
    let bytes = data.get(at..at.checked_add(8)?)?;
    Some(u64::from_be_bytes(bytes.try_into().ok()?))
}

/// The id of an entry named `TROP<3 digits>.PNG`.
fn icon_id(name: &[u8]) -> Option<u32> {
    let digits = name.strip_prefix(b"TROP")?.strip_suffix(b".PNG")?;
    if digits.len() != 3 || !digits.iter().all(u8::is_ascii_digit) {
        return None;
    }
    Some(digits.iter().fold(0, |n, d| n * 10 + u32::from(d - b'0')))
}

/// {trophy id: PNG bytes} from the game's `sce_sys/trophy/trophy00.trp` (empty when missing or odd).
///
/// The header is big endian: magic DC A2 4D 00, file count at 0x10, entry size at 0x14; the
/// entries start at 0x60 (32-byte name, offset, size). The icons are TROP000.PNG, TROP001.PNG...
/// The texts (TROP*.ESFM) are encrypted and not read here.
pub fn read_trp_icons(path: &Path) -> BTreeMap<u32, Vec<u8>> {
    let mut icons = BTreeMap::new();
    let Ok(data) = fs::read(path) else {
        return icons;
    };
    if data.len() < HEADER_SIZE || data[..4] != MAGIC {
        return icons;
    }
    let (Some(count), Some(element)) = (be_u32(&data, 0x10), be_u32(&data, 0x14)) else {
        return icons;
    };
    if !(0x30..=0x100).contains(&element) {
        return icons;
    }
    let element = element as usize;
    for index in 0..count.min(MAX_ENTRIES) as usize {
        let start = HEADER_SIZE + index * element;
        let Some(entry) = data.get(start..start + ENTRY_SIZE) else {
            break;
        };
        let name = entry[..0x20].split(|&b| b == 0).next().unwrap_or_default();
        let Some(id) = icon_id(name) else { continue };
        let (Some(offset), Some(size)) = (be_u64(entry, 0x20), be_u64(entry, 0x28)) else {
            continue;
        };
        let Some(end) = offset.checked_add(size) else {
            continue;
        };
        if size > 0 && end <= data.len() as u64 {
            if let Some(png) = data.get(offset as usize..end as usize) {
                icons.insert(id, png.to_vec());
            }
        }
    }
    icons
}

/// The file the port records unlocks in, inside the saves folder (`src/runtime_services.c`).
pub const UNLOCKS_FILE: &str = "trophies.txt";

/// The trophy ids the port recorded: `user/trophies.txt` holds one id per line, in unlock order.
pub fn read_unlocked_trophies(path: &Path) -> BTreeSet<u32> {
    let Ok(data) = fs::read_to_string(path) else {
        return BTreeSet::new();
    };
    data.split_whitespace()
        .filter_map(|word| word.parse::<u32>().ok())
        .filter(|&id| id < 128)
        .collect()
}

/// A trophy of the built-in list: its name and description as the PS4 shows them in English.
#[derive(Debug, Clone, serde::Deserialize)]
pub struct TrophyInfo {
    pub id: u32,
    pub tier: String,
    pub group: u32,
    pub name: String,
    pub description: String,
}

/// A trophy group (the base game, an add-on): its name and the ids it holds.
#[derive(Debug, Clone, serde::Deserialize)]
pub struct TrophyGroup {
    pub id: u32,
    pub name: String,
}

#[derive(Debug, Default, serde::Deserialize)]
struct TrophyList {
    #[serde(default)]
    groups: Vec<TrophyGroup>,
    #[serde(default)]
    trophies: Vec<TrophyInfo>,
}

/// The game's trophy texts are in its encrypted TROP*.ESFM; the port ships Diablo III's list
/// (`assets/trophies/Diablo_III.json`, matched to the package's icons by id).
fn trophy_list() -> &'static TrophyList {
    static LIST: std::sync::OnceLock<TrophyList> = std::sync::OnceLock::new();
    LIST.get_or_init(|| {
        serde_json::from_str(include_str!("../../../assets/trophies/Diablo_III.json"))
            .unwrap_or_default()
    })
}

/// The name, description, tier and group of trophy `id`, when the built-in list has it.
pub fn trophy_info(id: u32) -> Option<&'static TrophyInfo> {
    trophy_list().trophies.iter().find(|trophy| trophy.id == id)
}

/// The group a trophy belongs to (its name, for the list's headings).
pub fn trophy_group(id: u32) -> Option<&'static TrophyGroup> {
    trophy_list().groups.iter().find(|group| group.id == id)
}

/// The tier the built-in list gives trophy `id`.
pub fn listed_tier(id: u32) -> Option<Tier> {
    match trophy_info(id)?.tier.as_str() {
        "platinum" => Some(Tier::Platinum),
        "gold" => Some(Tier::Gold),
        "silver" => Some(Tier::Silver),
        "bronze" => Some(Tier::Bronze),
        _ => None,
    }
}

/// The grade of a trophy, told by the colour of its icon.
#[derive(Debug, Clone, Copy, PartialEq, Eq, PartialOrd, Ord)]
pub enum Tier {
    Platinum,
    Gold,
    Silver,
    Bronze,
}

/// The tier by the colour of the icon; `None` when the image cannot be decoded or is all dark.
///
/// The icon is cut down to 24 x 24 with a Catmull-Rom filter, the same cubic family as the
/// bicubic filter of the Python launcher, so the average colour can differ by a rounding step.
pub fn trophy_tier(png: &[u8]) -> Option<Tier> {
    let rgb = image::load_from_memory(png).ok()?.to_rgb8();
    let small = image::imageops::resize(&rgb, 24, 24, image::imageops::FilterType::CatmullRom);
    let (mut r, mut g, mut b, mut n) = (0.0f64, 0.0f64, 0.0f64, 0u32);
    for pixel in small.pixels().filter(|p| p.0.iter().any(|&c| c > 60)) {
        r += f64::from(pixel.0[0]);
        g += f64::from(pixel.0[1]);
        b += f64::from(pixel.0[2]);
        n += 1;
    }
    if n == 0 {
        return None;
    }
    let n = f64::from(n);
    let (r, g, b) = (r / n, g / n, b / n);
    let top = r.max(g).max(b);
    let spread = (top - r.min(g).min(b)) / top;
    Some(if b >= r {
        Tier::Platinum
    } else if spread < 0.4 {
        Tier::Silver
    } else if spread < 0.72 {
        Tier::Bronze
    } else {
        Tier::Gold
    })
}

/// Locks every trophy again: the file is moved to `<name>.bak`; `fs::rename` replaces an older copy
/// in one step on every platform.
/// Returns how many trophies were unlocked (0: nothing is changed). The game must be closed:
/// it keeps the trophies in memory while it runs.
pub fn reset_trophies(path: &Path) -> io::Result<usize> {
    let unlocked = read_unlocked_trophies(path).len();
    if unlocked > 0 {
        let mut name = path.file_name().unwrap_or_default().to_os_string();
        name.push(".bak");
        let backup = path.with_file_name(name);
        fs::rename(path, &backup)?;
    }
    Ok(unlocked)
}

/// The name of a tier in `lang`.
pub fn tier_label(tier: Tier, lang: &str) -> String {
    let en = match tier {
        Tier::Platinum => "Platinum",
        Tier::Gold => "Gold",
        Tier::Silver => "Silver",
        Tier::Bronze => "Bronze",
    };
    tr(lang, en)
}

/// The colour a tier is drawn in.
pub fn tier_color(tier: Tier) -> &'static str {
    match tier {
        Tier::Platinum => "#8fd3e8",
        Tier::Gold => "#e0b43a",
        Tier::Silver => "#c4c4c8",
        Tier::Bronze => "#cd7f32",
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::io::Cursor;
    use std::path::PathBuf;

    fn trp_with(entries: &[(&str, &[u8])], magic: [u8; 4], element: usize) -> Vec<u8> {
        let header_size = 0x60;
        let offset = header_size + element * entries.len();
        let mut table = Vec::new();
        let mut body = Vec::new();
        for (name, data) in entries {
            let mut entry = vec![0u8; element.max(0x30)];
            entry[..name.len()].copy_from_slice(name.as_bytes());
            entry[0x20..0x28].copy_from_slice(&((offset + body.len()) as u64).to_be_bytes());
            entry[0x28..0x30].copy_from_slice(&(data.len() as u64).to_be_bytes());
            entry.truncate(element);
            table.extend(entry);
            body.extend_from_slice(data);
        }
        let mut out = magic.to_vec();
        out.extend(3u32.to_be_bytes());
        out.extend(((header_size + table.len() + body.len()) as u64).to_be_bytes());
        out.extend((entries.len() as u32).to_be_bytes());
        out.extend((element as u32).to_be_bytes());
        out.resize(header_size, 0);
        out.extend(table);
        out.extend(body);
        out
    }

    fn trp(entries: &[(&str, &[u8])]) -> Vec<u8> {
        trp_with(entries, [0xdc, 0xa2, 0x4d, 0x00], 0x40)
    }

    fn write(dir: &tempfile::TempDir, name: &str, data: &[u8]) -> PathBuf {
        let path = dir.path().join(name);
        fs::write(&path, data).unwrap();
        path
    }

    fn solid_png(rgb: [u8; 3]) -> Vec<u8> {
        let img = image::RgbImage::from_pixel(32, 32, image::Rgb(rgb));
        let mut out = Cursor::new(Vec::new());
        img.write_to(&mut out, image::ImageFormat::Png).unwrap();
        out.into_inner()
    }

    #[test]
    fn icons_are_read_by_id() {
        let dir = tempfile::tempdir().unwrap();
        let path = write(
            &dir,
            "trophy00.trp",
            &trp(&[
                ("TROPCONF.ESFM", b"encrypted"),
                ("TROP000.PNG", b"\x89PNG-fake0"),
                ("TROP034.PNG", b"\x89PNG-fake34"),
                ("TROP_00.ESFM", b"x"),
                ("ICON0.PNG", b"icon"),
                ("TROP0001.PNG", b"y"),
                ("TROP00A.PNG", b"z"),
            ]),
        );
        let icons = read_trp_icons(&path);
        assert_eq!(icons.len(), 2);
        assert_eq!(icons[&0], b"\x89PNG-fake0");
        assert_eq!(icons[&34], b"\x89PNG-fake34");
    }

    #[test]
    fn bad_packages_give_nothing() {
        let dir = tempfile::tempdir().unwrap();
        let good = trp(&[("TROP001.PNG", b"abc")]);
        let none = BTreeMap::new();
        assert_eq!(read_trp_icons(&dir.path().join("missing.trp")), none);
        assert_eq!(read_trp_icons(&write(&dir, "a.trp", b"")), none);
        let mut wrong = good.clone();
        wrong[..4].copy_from_slice(&[0, 0, 0, 0]);
        assert_eq!(read_trp_icons(&write(&dir, "b.trp", &wrong)), none);
        let small = trp_with(&[("TROP001.PNG", b"abc")], [0xdc, 0xa2, 0x4d, 0x00], 8);
        assert_eq!(read_trp_icons(&write(&dir, "c.trp", &small)), none);
        let big = trp_with(&[("TROP001.PNG", b"abc")], [0xdc, 0xa2, 0x4d, 0x00], 0x101);
        assert_eq!(read_trp_icons(&write(&dir, "e.trp", &big)), none);
        assert_eq!(
            read_trp_icons(&write(&dir, "d.trp", &good[..good.len() - 2])),
            none
        );
    }

    #[test]
    fn a_huge_count_never_reads_past_the_file() {
        let dir = tempfile::tempdir().unwrap();
        let mut data = trp(&[("TROP001.PNG", b"abc"), ("TROP002.PNG", b"def")]);
        data[0x10..0x14].copy_from_slice(&100_000u32.to_be_bytes());
        data.resize(1024, 0);
        let icons = read_trp_icons(&write(&dir, "huge.trp", &data));
        assert_eq!(icons.keys().copied().collect::<Vec<_>>(), vec![1, 2]);

        // An entry whose offset + size overflows u64 is dropped, the valid one stays.
        let mut data = trp(&[("TROP001.PNG", b"abc"), ("TROP002.PNG", b"def")]);
        let second = 0x60 + 0x40;
        data[second + 0x20..second + 0x28].copy_from_slice(&u64::MAX.to_be_bytes());
        data[second + 0x28..second + 0x30].copy_from_slice(&16u64.to_be_bytes());
        let icons = read_trp_icons(&write(&dir, "overflow.trp", &data));
        assert_eq!(icons.keys().copied().collect::<Vec<_>>(), vec![1]);
    }

    #[test]
    fn unlocked_ids_one_per_line() {
        let dir = tempfile::tempdir().unwrap();
        let path = write(&dir, UNLOCKS_FILE, b"5\n0\n31\n32\n5\n127\n128\nx\n");
        assert_eq!(
            read_unlocked_trophies(&path),
            BTreeSet::from([0, 5, 31, 32, 127])
        );
        assert!(read_unlocked_trophies(&dir.path().join("none.txt")).is_empty());
    }

    #[test]
    fn the_built_in_list_names_every_diablo_trophy() {
        for id in 0..54 {
            let info = trophy_info(id).unwrap();
            assert_eq!(info.id, id);
            assert!(!info.name.is_empty() && !info.description.is_empty());
            assert!(listed_tier(id).is_some());
            assert!(trophy_group(info.group).is_some());
        }
        assert_eq!(listed_tier(0), Some(Tier::Platinum));
        assert_eq!(trophy_info(1).unwrap().name, "Deep Pockets");
        assert_eq!(
            trophy_group(trophy_info(53).unwrap().group).unwrap().name,
            "Diablo III: Rise of the Necromancer"
        );
        assert!(trophy_info(54).is_none());
    }

    #[test]
    fn tier_follows_the_colour_of_the_icon() {
        let cases = [
            (Tier::Platinum, [139, 167, 167]),
            (Tier::Bronze, [137, 89, 52]),
            (Tier::Silver, [136, 119, 104]),
            (Tier::Gold, [141, 104, 27]),
        ];
        for (tier, rgb) in cases {
            assert_eq!(trophy_tier(&solid_png(rgb)), Some(tier));
        }
        assert_eq!(trophy_tier(b"not a png"), None);
        assert_eq!(trophy_tier(&solid_png([0, 0, 0])), None);
    }

    #[test]
    fn reset_moves_the_file_and_counts() {
        let dir = tempfile::tempdir().unwrap();
        let path = write(&dir, UNLOCKS_FILE, b"0\n2\n");
        write(&dir, "trophies.txt.bak", b"old");
        assert_eq!(reset_trophies(&path).unwrap(), 2);
        assert!(!path.exists());
        let bak = dir.path().join("trophies.txt.bak");
        assert_eq!(fs::read(&bak).unwrap(), b"0\n2\n".to_vec());

        let empty = write(&dir, "empty.txt", b"");
        assert_eq!(reset_trophies(&empty).unwrap(), 0);
        assert!(empty.exists());
    }

    #[test]
    fn tier_labels_and_colors_exist() {
        assert_eq!(tier_label(Tier::Gold, "en"), "Gold");
        assert_eq!(tier_color(Tier::Platinum), "#8fd3e8");
    }
}
