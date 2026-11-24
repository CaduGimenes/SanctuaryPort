//! Patches of the game's eboot as the launcher sees them: the built-in XML's metadata, the patches
//! it offers, and the report of the last start (`patch_status.json`, written by
//! `scripts/patches.py`, which compiles and applies them).

use std::io;
use std::path::{Path, PathBuf};

use indexmap::IndexMap;
use serde::{Deserialize, Serialize};

use super::game::GameProfile;
use super::options::{
    COOP_EFFECTS_PATCH, FIXED_SCALE_PATCH, FRAME_LIMITER_PATCH_PREFIX, FRAME_RATE_PATCH_PREFIX,
    GAME_MSAA_PATCH_PREFIX, PC_CAMERA_ZOOM_PATCH, RENDER_TARGETS_PATCH_PREFIX,
    RESOLUTION_PATCH_PREFIX,
};
use super::paths::Paths;

/// The cheats ("Cheat: ..." patches): offered on the Game page under Cheats, off unless ticked.
pub const CHEAT_PREFIX: &str = "Cheat: ";

/// Fixes with nothing to choose: not offered in the launcher and always applied.
const ALWAYS_ON: &[&str] = &["Bug Fix - Startup Black Screen"];

#[derive(Debug, thiserror::Error)]
pub enum PatchError {
    #[error(transparent)]
    Io(#[from] io::Error),
    #[error("{0}")]
    Xml(String),
}

/// One `Metadata` element of a patch file.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct PatchMeta {
    pub name: String,
    pub author: Option<String>,
    pub note: Option<String>,
    pub enabled_by_default: bool,
}

/// The report `patches.py` writes after a start (`patch_status.json`), in its key order.
#[derive(Debug, Clone, Default, PartialEq, Eq, Serialize, Deserialize)]
pub struct PatchStatus {
    /// SHA-256 of the game's `eboot.bin` (`null` when it could not be read).
    #[serde(default)]
    pub sha256: Option<String>,
    #[serde(default)]
    pub applied: Vec<String>,
    /// Patch name to the reason it was left out.
    #[serde(default)]
    pub skipped: IndexMap<String, String>,
}

/// The built-in patch file of a game profile.
pub fn builtin_xml(paths: &Paths, profile: &GameProfile) -> PathBuf {
    paths.port_dir.join("patches").join(profile.xml)
}

/// Parses a patch file. One nested deeper than [`MAX_DEPTH`] is refused first: roxmltree's
/// parser recurses once per level and would overflow the stack.
fn parse(text: &str) -> Result<roxmltree::Document<'_>, PatchError> {
    let depth = nesting_bound(text);
    if depth > MAX_DEPTH {
        return Err(PatchError::Xml(format!(
            "elements may nest {depth} deep (at most {MAX_DEPTH})"
        )));
    }
    let options = roxmltree::ParsingOptions {
        allow_dtd: true,
        ..Default::default()
    };
    roxmltree::Document::parse_with_options(text, options)
        .map_err(|error| PatchError::Xml(error.to_string()))
}

/// Deeper than any patch file (they nest four deep) and shallow enough for roxmltree's
/// recursion, about 8 KiB a level in a debug build.
const MAX_DEPTH: usize = 32;

/// An upper bound of the element nesting roxmltree reaches on `text`. Without entity
/// declarations, the depth of the tags outside comments, CDATA sections, processing
/// instructions and quoted attribute values; an entity may expand to markup, so with one every
/// `<` counts.
fn nesting_bound(text: &str) -> usize {
    let bytes = text.as_bytes();
    if text.contains("<!ENTITY") {
        return bytes.iter().filter(|&&b| b == b'<').count();
    }
    // The offset just past the first `end` at or after `from` (the end of the text without one).
    let past = |from: usize, end: &[u8]| {
        bytes
            .get(from..)
            .and_then(|rest| rest.windows(end.len()).position(|w| w == end))
            .map_or(bytes.len(), |at| from + at + end.len())
    };
    let (mut depth, mut deepest, mut at) = (0usize, 0usize, 0usize);
    while let Some(start) = bytes
        .get(at..)
        .and_then(|rest| rest.iter().position(|&b| b == b'<'))
        .map(|i| at + i)
    {
        let tag = bytes.get(start..).unwrap_or_default();
        at = if tag.starts_with(b"<!--") {
            past(start + 4, b"-->")
        } else if tag.starts_with(b"<![CDATA[") {
            past(start + 9, b"]]>")
        } else if tag.starts_with(b"<?") {
            past(start + 2, b"?>")
        } else if tag.starts_with(b"<!") {
            // A declaration: what follows is scanned like content, which only counts more.
            start + 2
        } else if tag.starts_with(b"</") {
            depth = depth.saturating_sub(1);
            start + 2
        } else {
            // A start tag, to its `>` outside quotes; `/>` closes it at once.
            let mut quote = None;
            let mut end = None;
            for (i, &b) in tag.iter().enumerate().skip(1) {
                match quote {
                    Some(q) if b == q => quote = None,
                    Some(_) => {}
                    None if b == b'"' || b == b'\'' => quote = Some(b),
                    None if b == b'>' => {
                        end = Some(i);
                        break;
                    }
                    None => {}
                }
            }
            let empty = end.is_some_and(|i| tag.get(i - 1) == Some(&b'/'));
            if !empty {
                depth += 1;
                deepest = deepest.max(depth);
            }
            end.map_or(bytes.len(), |i| start + i + 1)
        };
    }
    deepest
}

fn non_empty(value: Option<&str>) -> Option<String> {
    value.filter(|v| !v.is_empty()).map(str::to_string)
}

fn meta_of(node: roxmltree::Node) -> PatchMeta {
    PatchMeta {
        name: node.attribute("Name").unwrap_or_default().to_string(),
        author: non_empty(node.attribute("Author")),
        note: non_empty(node.attribute("Note")),
        enabled_by_default: node
            .attribute("isEnabled")
            .is_some_and(|v| v.to_lowercase() == "true"),
    }
}

fn metadata<'a, 'input>(
    doc: &'a roxmltree::Document<'input>,
) -> impl Iterator<Item = roxmltree::Node<'a, 'input>> {
    doc.descendants().filter(|n| n.has_tag_name("Metadata"))
}

/// A Preset of a parameterised patch, as the entry it stands for: its name and note, the patch's author.
fn preset_of(patch: roxmltree::Node, preset: roxmltree::Node) -> PatchMeta {
    PatchMeta {
        name: preset.attribute("Name").unwrap_or_default().to_string(),
        author: non_empty(patch.attribute("Author")),
        note: non_empty(preset.attribute("Note")).or_else(|| non_empty(patch.attribute("Note"))),
        enabled_by_default: false,
    }
}

/// The patches the XML has for this app version, in file order. A parameterised patch (Params=, whose
/// values run.py works out or a Preset names, scripts/patches.py) is listed as its Presets: the names
/// that can be ticked ("Offline Seasons - Season 30"), which select the patch with those values.
pub fn game_patch_metas(xml: &Path, app_version: &str) -> Result<Vec<PatchMeta>, PatchError> {
    let text = std::fs::read_to_string(xml)?;
    let doc = parse(&text).map_err(|e| PatchError::Xml(e.to_string()))?;
    Ok(metadata(&doc)
        .filter(|n| n.attribute("AppVer") == Some(app_version))
        .flat_map(|n| {
            if n.attribute("Params").is_some_and(|p| !p.trim().is_empty()) {
                n.descendants()
                    .filter(|c| c.has_tag_name("Preset"))
                    .map(|preset| preset_of(n, preset))
                    .collect()
            } else {
                vec![meta_of(n)]
            }
        })
        .collect())
}

/// The patches the launcher offers: without the resolution, frame rate, fixed scale, MSAA and
/// Effects ones (they follow the Graphics page) and the always-on fixes.
pub fn selectable(metas: &[PatchMeta]) -> Vec<PatchMeta> {
    metas
        .iter()
        .filter(|m| {
            // Diablo III's raised second frame limiter follows the 200 FPS frame rate (run.py).
            !(m.name == crate::core::options::FOREGROUND_CAP_PATCH
                || m.name.starts_with(RESOLUTION_PATCH_PREFIX)
                || m.name.starts_with(FRAME_RATE_PATCH_PREFIX)
                || m.name.starts_with(FRAME_LIMITER_PATCH_PREFIX)
                || m.name == FIXED_SCALE_PATCH
                || m.name.starts_with(GAME_MSAA_PATCH_PREFIX)
                || m.name.starts_with(RENDER_TARGETS_PATCH_PREFIX)
                || m.name == COOP_EFFECTS_PATCH
                || m.name == PC_CAMERA_ZOOM_PATCH
                || ALWAYS_ON.contains(&m.name.as_str()))
        })
        .cloned()
        .collect()
}

/// The last start's patch report in `out_dir`; `None` when missing or not valid.
pub fn patch_status(out_dir: &Path) -> Option<PatchStatus> {
    let bytes = std::fs::read(out_dir.join("patch_status.json")).ok()?;
    serde_json::from_slice(&bytes).ok()
}

#[cfg(test)]
#[allow(clippy::unwrap_used, clippy::expect_used)]
mod tests {
    use super::*;
    use crate::core::game::game_profile;

    fn diablo_xml() -> PathBuf {
        Path::new(env!("CARGO_MANIFEST_DIR")).join("../patches/Diablo_III.xml")
    }

    #[test]
    fn the_built_in_file_is_diablo_iiis() {
        let profile = game_profile(Some("CUSA00243"), None).unwrap();
        let paths = Paths::from_env(Path::new("/port"), &|_| None);
        assert_eq!(
            builtin_xml(&paths, profile),
            Path::new("/port/patches/Diablo_III.xml")
        );
        let metas = game_patch_metas(&diablo_xml(), profile.app_version).unwrap();
        assert!(!metas.is_empty());
        // The resolution, frame limiter and fixed scale patches follow the Graphics page; the offline
        // seasons and events (GitHub PR #31) are what the Game page offers.
        assert!(metas.iter().any(|meta| meta.name == FIXED_SCALE_PATCH));
        let offered = selectable(&metas);
        assert!(!offered.is_empty());
        assert!(offered.iter().all(|meta| !meta.name.starts_with(RESOLUTION_PATCH_PREFIX)
            && !meta.name.starts_with(FRAME_LIMITER_PATCH_PREFIX)
            && meta.name != FIXED_SCALE_PATCH
            && !meta.name.starts_with(GAME_MSAA_PATCH_PREFIX)
            && !meta.name.starts_with(RENDER_TARGETS_PATCH_PREFIX)
            && meta.name != COOP_EFFECTS_PATCH
            && meta.name != PC_CAMERA_ZOOM_PATCH));
        assert!(metas.iter().any(|meta| meta.name == "Game MSAA - 4x"));
        assert!(metas.iter().any(|meta| meta.name == "PC Render Targets - Shadow Map 2048"));
        assert!(metas.iter().any(|meta| meta.name == COOP_EFFECTS_PATCH));
        assert!(metas.iter().any(|meta| meta.name == PC_CAMERA_ZOOM_PATCH));
        assert!(offered.iter().any(|meta| meta.name.starts_with("Offline Seasons - Season ")));
        assert!(offered.iter().any(|meta| meta.name.starts_with("Event: ")));
        // The raised foreground cap is in the file but follows the 200 FPS frame rate (run.py).
        let cap = crate::core::options::FOREGROUND_CAP_PATCH;
        assert!(metas.iter().any(|meta| meta.name == cap));
        assert!(offered.iter().all(|meta| meta.name != cap));
        assert!(game_patch_metas(&diablo_xml(), "01.00").unwrap().is_empty());
    }

    #[test]
    fn metadata_and_selection() {
        let dir = tempfile::tempdir().unwrap();
        let xml = dir.path().join("p.xml");
        std::fs::write(
            &xml,
            r#"<Patch><Metadata Title="T" Name="Skip Intro" Author="A" Note="" isEnabled="True" AppVer="01.47"/>
<Metadata Name="Bug Fix - Startup Black Screen" AppVer="01.47"/>
<Metadata Name="Other version" AppVer="01.00"/></Patch>"#,
        )
        .unwrap();
        let metas = game_patch_metas(&xml, "01.47").unwrap();
        assert_eq!(
            metas[0],
            PatchMeta {
                name: "Skip Intro".into(),
                author: Some("A".into()),
                note: None,
                enabled_by_default: true,
            }
        );
        assert_eq!(metas.len(), 2);
        assert_eq!(selectable(&metas), metas[..1]);
    }

    #[test]
    fn a_parameterised_patch_is_listed_as_its_presets() {
        let dir = tempfile::tempdir().unwrap();
        let xml = dir.path().join("p.xml");
        std::fs::write(
            &xml,
            r#"<Patch><Metadata Name="Offline Seasons - Season" Params="season" Author="A" Note="Any season" AppVer="01.47">
<PatchList><Line Type="bytes" Address="0x1" Value="{param:season:a2}"/></PatchList>
<Presets><Preset Name="Offline Seasons - Season 14" Note="Season 14" season="14"/>
<Preset Name="Offline Seasons - Season 30" season="30"/></Presets></Metadata>
<Metadata Name="Offline Seasons - Season" Params="season" AppVer="01.47"/>
<Metadata Name="Event: Pandemonium" AppVer="01.47"/></Patch>"#,
        )
        .unwrap();
        let metas = game_patch_metas(&xml, "01.47").unwrap();
        let names: Vec<&str> = metas.iter().map(|m| m.name.as_str()).collect();
        assert_eq!(
            names,
            ["Offline Seasons - Season 14", "Offline Seasons - Season 30", "Event: Pandemonium"]
        );
        assert_eq!(metas[0].note.as_deref(), Some("Season 14"));
        assert_eq!(metas[1].note.as_deref(), Some("Any season"));
        assert_eq!(metas[1].author.as_deref(), Some("A"));
        // The built-in file: every season of the former one-entry-per-season patches, in order.
        let profile = game_profile(Some("CUSA00243"), None).unwrap();
        let seasons: Vec<String> = game_patch_metas(&diablo_xml(), profile.app_version)
            .unwrap()
            .into_iter()
            .filter_map(|m| m.name.strip_prefix("Offline Seasons - Season ").map(str::to_string))
            .collect();
        let expected: Vec<String> = (14..=40).map(|n: u32| n.to_string()).collect();
        assert_eq!(seasons, expected);
    }

    #[test]
    fn a_hostile_patch_file_is_an_error() {
        let dir = tempfile::tempdir().unwrap();
        let xml = dir.path().join("deep.xml");
        std::fs::write(&xml, "<a>".repeat(MAX_DEPTH + 1)).unwrap();
        assert!(matches!(game_patch_metas(&xml, "01.47"), Err(PatchError::Xml(_))));
        std::fs::write(&xml, "<Patch><Metadata").unwrap();
        assert!(matches!(game_patch_metas(&xml, "01.47"), Err(PatchError::Xml(_))));
        assert!(matches!(
            game_patch_metas(&dir.path().join("missing.xml"), "01.47"),
            Err(PatchError::Io(_))
        ));
        assert_eq!(nesting_bound("<a><!-- <b> --><c/><d x='>'></d></a>"), 2);
    }

    #[test]
    fn the_status_of_the_last_start() {
        let dir = tempfile::tempdir().unwrap();
        assert_eq!(patch_status(dir.path()), None);
        std::fs::write(
            dir.path().join("patch_status.json"),
            r#"{"sha256": null, "applied": ["Resolution Patch - 1440p"], "skipped": {"X": "not found"}}"#,
        )
        .unwrap();
        let status = patch_status(dir.path()).unwrap();
        assert_eq!(status.applied, ["Resolution Patch - 1440p"]);
        assert_eq!(status.skipped["X"], "not found");
        std::fs::write(dir.path().join("patch_status.json"), "{").unwrap();
        assert_eq!(patch_status(dir.path()), None);
    }
}
