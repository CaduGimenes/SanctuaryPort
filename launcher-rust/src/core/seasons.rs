//! The offline season rules: which community events come with a season, and the 15 day rotation.
//!
//! Every "Offline Seasons - Season N" patch already carries that season's own community event, the
//! Altar of Rites, Visions of Enmity and the Season Journey. A few seasons (22, 33, 38, 39, 40) have
//! a theme the patch itself does not switch on; `THEME_GAPS` names the "Event: ..." patches that do
//! (Maxroll "Season Theme History" and d3resource.com/seasons).

use std::collections::BTreeSet;
use std::time::{SystemTime, UNIX_EPOCH};

/// The name prefix of the offline community event patches.
pub const EVENT_PREFIX: &str = "Event: ";

/// The name prefix of the offline season patches (patches/Diablo_III.xml).
pub const SEASON_PREFIX: &str = "Offline Seasons - Season ";

/// Days one season lasts when the rotation is on.
pub const ROTATION_DAYS: i64 = 15;

/// Event patches that are never switched by a season mode: they are the player's own choice.
const OWN_CHOICE: [&str; 2] = ["Event: Also in non-seasonal games", "Event: Challenge Rifts"];

/// The seasons whose theme the season patch does not carry, with the event patches that do.
const THEME_GAPS: [(u32, &[&str]); 4] = [
    (22, &["Event: Shadow Clones", "Event: Fourth Kanai's Cube Slot"]),
    (33, &["Event: Shadow Clones", "Event: Fourth Kanai's Cube Slot"]),
    (38, &["Event: Ethereal Items"]),
    (39, &["Event: Shadow Clones", "Event: Fourth Kanai's Cube Slot"]),
];

/// Season 40 and later reuse the rotation of Season 30 on: Light's Calling follows Season 39.
const LIGHTS_CALLING: &str = "Event: Sanctified Items";

/// How the community events follow the season.
#[derive(Clone, Copy, Debug, PartialEq, Eq, Default)]
pub enum SeasonMode {
    /// The event check boxes are the player's.
    #[default]
    Manual,
    /// Blizzard style: the season brings its own events and no others.
    WithEvents,
    /// The season and every event together.
    Full,
}

impl SeasonMode {
    /// The value saved in the settings.
    pub fn key(self) -> &'static str {
        match self {
            SeasonMode::Manual => "manual",
            SeasonMode::WithEvents => "season",
            SeasonMode::Full => "full",
        }
    }

    /// The mode of a saved value; anything unknown is `Manual`.
    pub fn from_key(key: &str) -> Self {
        match key {
            "season" => SeasonMode::WithEvents,
            "full" => SeasonMode::Full,
            _ => SeasonMode::Manual,
        }
    }
}

/// The event patches the season's theme needs on top of its own patch.
fn theme_patches(season: u32) -> Vec<&'static str> {
    if let Some((_, names)) = THEME_GAPS.iter().find(|(n, _)| *n == season) {
        return names.to_vec();
    }
    if season >= 40 && (season - 40).is_multiple_of(5) {
        return vec![LIGHTS_CALLING];
    }
    Vec::new()
}

/// Applies `mode` to the ticked patches for `season` (None: no season, nothing changes).
/// `available` are the names the patch file offers; only those are ticked.
pub fn apply_mode(
    chosen: &mut BTreeSet<String>,
    available: &BTreeSet<String>,
    mode: SeasonMode,
    season: Option<u32>,
) {
    let Some(season) = season else { return };
    let is_event = |name: &str| name.starts_with(EVENT_PREFIX) && !OWN_CHOICE.contains(&name);
    match mode {
        SeasonMode::Manual => {}
        SeasonMode::WithEvents => {
            chosen.retain(|name| !is_event(name));
            for name in theme_patches(season) {
                if available.contains(name) {
                    chosen.insert(name.to_string());
                }
            }
        }
        SeasonMode::Full => {
            for name in available.iter().filter(|name| is_event(name)) {
                chosen.insert(name.clone());
            }
        }
    }
}

/// Days since 1970-01-01 (UTC) now.
pub fn today() -> i64 {
    let secs = SystemTime::now()
        .duration_since(UNIX_EPOCH)
        .map_or(0, |since| since.as_secs());
    (secs / 86_400) as i64
}

/// The season the rotation is on: it starts at `first` on day `start`, and every
/// `ROTATION_DAYS` days moves to the next season of `numbers` (ascending), wrapping at the end.
/// A `first` that is not in the list starts at the first entry.
pub fn rotated_season(numbers: &[u32], first: u32, start: i64, today: i64) -> Option<u32> {
    if numbers.is_empty() {
        return None;
    }
    let at = numbers.iter().position(|n| *n == first).unwrap_or(0) as i64;
    let steps = (today - start).max(0) / ROTATION_DAYS;
    Some(numbers[((at + steps) % numbers.len() as i64) as usize])
}

/// The days left of the current season of the rotation (1 on its last day).
pub fn days_left(start: i64, today: i64) -> i64 {
    ROTATION_DAYS - ((today - start).max(0) % ROTATION_DAYS)
}

#[cfg(test)]
mod tests {
    use super::*;

    fn set(names: &[&str]) -> BTreeSet<String> {
        names.iter().map(|n| n.to_string()).collect()
    }

    #[test]
    fn manual_changes_nothing() {
        let mut chosen = set(&["Event: Pandemonium"]);
        apply_mode(&mut chosen, &set(&["Event: Soul Shards"]), SeasonMode::Manual, Some(30));
        assert_eq!(chosen, set(&["Event: Pandemonium"]));
    }

    #[test]
    fn with_events_keeps_only_the_season_theme_and_the_players_own_choices() {
        let all = set(&[
            "Event: Pandemonium",
            "Event: Shadow Clones",
            "Event: Fourth Kanai's Cube Slot",
            "Event: Challenge Rifts",
            "Event: Double Bounty Bags",
        ]);
        let mut chosen = set(&["Event: Pandemonium", "Event: Challenge Rifts", "Offline: no sign-in warning"]);
        apply_mode(&mut chosen, &all, SeasonMode::WithEvents, Some(33));
        assert_eq!(
            chosen,
            set(&[
                "Event: Challenge Rifts",
                "Event: Shadow Clones",
                "Event: Fourth Kanai's Cube Slot",
                "Offline: no sign-in warning",
            ])
        );
    }

    #[test]
    fn a_season_with_its_own_event_needs_no_extra_patch() {
        let all = set(&["Event: Soul Shards", "Event: Kanai Powers"]);
        let mut chosen = set(&["Event: Kanai Powers"]);
        apply_mode(&mut chosen, &all, SeasonMode::WithEvents, Some(30));
        assert!(chosen.is_empty());
    }

    #[test]
    fn season_40_brings_lights_calling_and_45_again() {
        assert_eq!(theme_patches(40), ["Event: Sanctified Items"]);
        assert_eq!(theme_patches(45), ["Event: Sanctified Items"]);
        assert!(theme_patches(41).is_empty());
    }

    #[test]
    fn full_ticks_every_event_but_the_players_own_choices() {
        let all = set(&["Event: Pandemonium", "Event: Soul Shards", "Event: Also in non-seasonal games"]);
        let mut chosen = BTreeSet::new();
        apply_mode(&mut chosen, &all, SeasonMode::Full, Some(30));
        assert_eq!(chosen, set(&["Event: Pandemonium", "Event: Soul Shards"]));
    }

    #[test]
    fn no_season_leaves_the_events_alone() {
        let mut chosen = set(&["Event: Pandemonium"]);
        apply_mode(&mut chosen, &set(&["Event: Soul Shards"]), SeasonMode::Full, None);
        assert_eq!(chosen, set(&["Event: Pandemonium"]));
    }

    #[test]
    fn the_rotation_moves_every_15_days_and_wraps() {
        let numbers = [14, 15, 30];
        assert_eq!(rotated_season(&numbers, 15, 100, 100), Some(15));
        assert_eq!(rotated_season(&numbers, 15, 100, 114), Some(15));
        assert_eq!(rotated_season(&numbers, 15, 100, 115), Some(30));
        assert_eq!(rotated_season(&numbers, 15, 100, 130), Some(14));
        assert_eq!(rotated_season(&numbers, 15, 100, 50), Some(15), "a date before the start stays");
        assert_eq!(rotated_season(&[], 15, 100, 130), None);
        assert_eq!(rotated_season(&numbers, 99, 100, 115), Some(15), "unknown first season: the first entry");
    }

    #[test]
    fn days_left_counts_down_to_one() {
        assert_eq!(days_left(100, 100), 15);
        assert_eq!(days_left(100, 114), 1);
        assert_eq!(days_left(100, 115), 15);
    }

    #[test]
    fn modes_round_trip_their_saved_value() {
        for mode in [SeasonMode::Manual, SeasonMode::WithEvents, SeasonMode::Full] {
            assert_eq!(SeasonMode::from_key(mode.key()), mode);
        }
        assert_eq!(SeasonMode::from_key("junk"), SeasonMode::Manual);
    }
}
