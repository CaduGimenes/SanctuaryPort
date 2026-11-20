//! The offline season rules: which community events come with a season, and the season rotation.
//!
//! Every "Offline Seasons - Season N" patch already carries that season's own community event and the
//! Season Journey, plus the Altar of Rites from Season 28 and Visions of Enmity from Season 29, as
//! Blizzard added them. A few seasons (22, 33, 38, 39, 40) have
//! a theme the patch itself does not switch on; `THEME_GAPS` names the "Event: ..." patches that do
//! (Maxroll "Season Theme History" and d3resource.com/seasons).

use std::collections::BTreeSet;
use std::time::{SystemTime, UNIX_EPOCH};

/// The name prefix of the offline community event patches.
pub const EVENT_PREFIX: &str = "Event: ";

/// The name prefix of the offline season patches (patches/Diablo_III.xml).
pub const SEASON_PREFIX: &str = "Offline Seasons - Season ";

/// Days one season lasts when the rotation is on, unless the player picks another length.
pub const ROTATION_DAYS: i64 = 15;

/// The season lengths the Seasons page offers, in days: 90 is about Blizzard's usual season.
pub const ROTATION_LENGTHS: [i64; 4] = [15, 30, 60, 90];

/// The saved `season_rotate_days` as a length the page offers; anything else is `ROTATION_DAYS`.
pub fn rotation_days(saved: &str) -> i64 {
    saved
        .trim()
        .parse()
        .ok()
        .filter(|days| ROTATION_LENGTHS.contains(days))
        .unwrap_or(ROTATION_DAYS)
}

/// The anniversary event patch (the Diablo 1 dungeon in Old Tristram). It follows its own check box
/// and dates on the Seasons page, never a season.
pub const TRISTRAM_PATCH: &str = "Event: The Darkening of Tristram";

/// Event patches that are never switched by a season mode: they are the player's own choice.
const OWN_CHOICE: [&str; 3] = [
    "Event: Also in non-seasonal games",
    "Event: Challenge Rifts",
    TRISTRAM_PATCH,
];

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

/// Days since 1970-01-01 of a `YYYY-MM-DD` date; `None` for anything else.
pub fn parse_date(text: &str) -> Option<i64> {
    let mut parts = text.trim().split('-');
    let year: i64 = parts.next()?.parse().ok()?;
    let month: i64 = parts.next()?.parse().ok()?;
    let day: i64 = parts.next()?.parse().ok()?;
    if parts.next().is_some() || !(1..=12).contains(&month) || !(1..=31).contains(&day) {
        return None;
    }
    // Days from civil (Howard Hinnant's algorithm).
    let y = if month <= 2 { year - 1 } else { year };
    let era = y.div_euclid(400);
    let yoe = y - era * 400;
    let mp = (month + 9) % 12;
    let doy = (153 * mp + 2) / 5 + day - 1;
    let doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    let days = era * 146_097 + doe - 719_468;
    // Reject 31 February and the like: the date must come back as typed.
    let back = civil_from_days(days);
    (back == (year, month, day)).then_some(days)
}

fn civil_from_days(days: i64) -> (i64, i64, i64) {
    let z = days + 719_468;
    let era = z.div_euclid(146_097);
    let doe = z - era * 146_097;
    let yoe = (doe - doe / 1460 + doe / 36_524 - doe / 146_096) / 365;
    let doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    let mp = (5 * doy + 2) / 153;
    let day = doy - (153 * mp + 2) / 5 + 1;
    let month = if mp < 10 { mp + 3 } else { mp - 9 };
    let year = yoe + era * 400 + i64::from(month <= 2);
    (year, month, day)
}

/// What the dates of an event say about `today`.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum Window {
    /// Inside the dates (an empty date is no limit on that side).
    Open,
    /// Before the first day or after the last one.
    Closed,
    /// A date that is not `YYYY-MM-DD`.
    Invalid,
}

/// Whether `today` is on or between `start` and `end` (`YYYY-MM-DD`, both included). An empty
/// date leaves that side open, so two empty dates mean always.
pub fn event_window(start: &str, end: &str, today: i64) -> Window {
    let bound = |text: &str| {
        if text.trim().is_empty() { Ok(None) } else { parse_date(text).map(Some).ok_or(()) }
    };
    let (Ok(first), Ok(last)) = (bound(start), bound(end)) else { return Window::Invalid };
    if first.is_some_and(|first| today < first) || last.is_some_and(|last| today > last) {
        Window::Closed
    } else {
        Window::Open
    }
}

/// Ticks the Darkening of Tristram patch when the player wants it and `today` is inside its dates,
/// unticks it otherwise. Nothing happens when the patch file does not offer it.
pub fn apply_tristram(
    chosen: &mut BTreeSet<String>,
    available: &BTreeSet<String>,
    wanted: bool,
    start: &str,
    end: &str,
    today: i64,
) {
    if !available.contains(TRISTRAM_PATCH) {
        return;
    }
    if wanted && event_window(start, end, today) == Window::Open {
        chosen.insert(TRISTRAM_PATCH.to_string());
    } else {
        chosen.remove(TRISTRAM_PATCH);
    }
}

/// The season the rotation is on: it starts at `first` on day `start`, and every
/// `days` days moves to the next season of `numbers` (ascending), wrapping at the end.
/// A `first` that is not in the list starts at the first entry.
pub fn rotated_season(numbers: &[u32], first: u32, start: i64, today: i64, days: i64) -> Option<u32> {
    if numbers.is_empty() {
        return None;
    }
    let at = numbers.iter().position(|n| *n == first).unwrap_or(0) as i64;
    let steps = (today - start).max(0) / days.max(1);
    Some(numbers[((at + steps) % numbers.len() as i64) as usize])
}

/// The days left of the current season of the rotation (1 on its last day).
pub fn days_left(start: i64, today: i64, days: i64) -> i64 {
    let days = days.max(1);
    days - ((today - start).max(0) % days)
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
        assert_eq!(rotated_season(&numbers, 15, 100, 100, ROTATION_DAYS), Some(15));
        assert_eq!(rotated_season(&numbers, 15, 100, 114, ROTATION_DAYS), Some(15));
        assert_eq!(rotated_season(&numbers, 15, 100, 115, ROTATION_DAYS), Some(30));
        assert_eq!(rotated_season(&numbers, 15, 100, 130, ROTATION_DAYS), Some(14));
        assert_eq!(rotated_season(&numbers, 15, 100, 50, ROTATION_DAYS), Some(15), "a date before the start stays");
        assert_eq!(rotated_season(&[], 15, 100, 130, ROTATION_DAYS), None);
        assert_eq!(rotated_season(&numbers, 99, 100, 115, ROTATION_DAYS), Some(15), "unknown first season: the first entry");
    }

    #[test]
    fn the_rotation_follows_each_length() {
        let numbers = [14, 15, 30];
        for days in ROTATION_LENGTHS {
            let at = |today| rotated_season(&numbers, 15, 100, today, days);
            assert_eq!(at(100), Some(15), "{days}");
            assert_eq!(at(100 + days - 1), Some(15), "{days}: the last day of the first season");
            assert_eq!(at(100 + days), Some(30), "{days}: the next season");
            assert_eq!(at(100 + 2 * days), Some(14), "{days}: wraps to the first entry");
            assert_eq!(at(100 + 3 * days), Some(15), "{days}");
            assert_eq!(days_left(100, 100, days), days, "{days}");
            assert_eq!(days_left(100, 100 + days - 1, days), 1, "{days}");
            assert_eq!(days_left(100, 100 + days, days), days, "{days}");
        }
        // 90 days is about Blizzard's three months: day 89 is still the first season.
        assert_eq!(rotated_season(&numbers, 15, 100, 189, 90), Some(15));
        assert_eq!(rotated_season(&numbers, 15, 100, 190, 90), Some(30));
    }

    #[test]
    fn the_saved_length_is_one_the_page_offers() {
        assert_eq!(rotation_days(""), ROTATION_DAYS);
        assert_eq!(rotation_days("15"), 15);
        assert_eq!(rotation_days("30"), 30);
        assert_eq!(rotation_days(" 60 "), 60);
        assert_eq!(rotation_days("90"), 90);
        for bad in ["0", "-15", "7", "abc", "90.5"] {
            assert_eq!(rotation_days(bad), ROTATION_DAYS, "{bad}");
        }
    }

    #[test]
    fn days_left_counts_down_to_one() {
        assert_eq!(days_left(100, 100, ROTATION_DAYS), 15);
        assert_eq!(days_left(100, 114, ROTATION_DAYS), 1);
        assert_eq!(days_left(100, 115, ROTATION_DAYS), 15);
    }

    #[test]
    fn modes_round_trip_their_saved_value() {
        for mode in [SeasonMode::Manual, SeasonMode::WithEvents, SeasonMode::Full] {
            assert_eq!(SeasonMode::from_key(mode.key()), mode);
        }
        assert_eq!(SeasonMode::from_key("junk"), SeasonMode::Manual);
    }

    #[test]
    fn dates_are_read_as_year_month_day() {
        assert_eq!(parse_date("1970-01-01"), Some(0));
        assert_eq!(parse_date("2027-01-03"), Some(20_821));
        assert_eq!(parse_date(" 2027-01-31 "), Some(20_849));
        assert_eq!(parse_date("2024-02-29"), Some(19_782));
        for bad in ["", "2027-02-30", "2027-13-01", "2027-1", "03-01-2027", "2027-01-03-1", "abc"] {
            assert_eq!(parse_date(bad), None, "{bad}");
        }
    }

    #[test]
    fn an_event_runs_between_its_dates_and_empty_means_always() {
        let first = parse_date("2027-01-03").unwrap();
        let last = parse_date("2027-01-31").unwrap();
        let window = |today| event_window("2027-01-03", "2027-01-31", today);
        assert_eq!(window(first - 1), Window::Closed);
        assert_eq!(window(first), Window::Open);
        assert_eq!(window(last), Window::Open);
        assert_eq!(window(last + 1), Window::Closed);
        assert_eq!(event_window("", "", 1), Window::Open);
        assert_eq!(event_window("2027-01-03", "", last + 400), Window::Open);
        assert_eq!(event_window("", "2027-01-31", first), Window::Open);
        assert_eq!(event_window("junk", "", 1), Window::Invalid);
        assert_eq!(event_window("", "2027-02-30", 1), Window::Invalid);
    }

    #[test]
    fn the_season_modes_leave_the_tristram_event_alone() {
        let available: BTreeSet<String> = [TRISTRAM_PATCH, "Event: Soul Shards"]
            .iter()
            .map(|name| name.to_string())
            .collect();
        let mut chosen: BTreeSet<String> = [TRISTRAM_PATCH.to_string()].into();
        apply_mode(&mut chosen, &available, SeasonMode::WithEvents, Some(30));
        assert!(chosen.contains(TRISTRAM_PATCH), "a season mode must not untick it");
        let mut chosen = BTreeSet::new();
        apply_mode(&mut chosen, &available, SeasonMode::Full, Some(30));
        assert!(!chosen.contains(TRISTRAM_PATCH), "a full season must not tick it");
        assert!(chosen.contains("Event: Soul Shards"));
    }

    #[test]
    fn the_tristram_patch_follows_its_check_box_and_dates() {
        let available: BTreeSet<String> = [TRISTRAM_PATCH.to_string()].into();
        let (first, last) = (parse_date("2027-01-03").unwrap(), parse_date("2027-01-31").unwrap());
        let run = |wanted, today| {
            let mut chosen = BTreeSet::new();
            apply_tristram(&mut chosen, &available, wanted, "2027-01-03", "2027-01-31", today);
            chosen.contains(TRISTRAM_PATCH)
        };
        assert!(run(true, first) && run(true, last));
        assert!(!run(true, first - 1) && !run(true, last + 1));
        assert!(!run(false, first), "unticked stays off inside the dates");
        // Without the patch in the file nothing is added; an old tick is dropped when it ends.
        let mut none = BTreeSet::new();
        apply_tristram(&mut none, &BTreeSet::new(), true, "", "", 1);
        assert!(none.is_empty());
        let mut stale: BTreeSet<String> = [TRISTRAM_PATCH.to_string()].into();
        apply_tristram(&mut stale, &available, true, "2027-01-03", "2027-01-31", last + 9);
        assert!(stale.is_empty());
    }
}
