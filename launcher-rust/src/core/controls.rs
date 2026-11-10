//! The control tables, key-name rules, `SANCTUARY_*_MAP` builders and the settings migration.
//!
//! Key names are compared case-insensitively with `to_lowercase()`; the names involved are
//! ASCII/Latin, where it matches the Python launcher's `casefold()`.

use std::collections::HashSet;

use indexmap::IndexMap;
use serde_json::{Map, Value};

/// A PS4 button, its default key and its default gamepad button (SDL names).
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct ControlRow {
    pub button: &'static str,
    pub key: &'static str,
    pub pad: Option<&'static str>,
}

const fn row(button: &'static str, key: &'static str, pad: Option<&'static str>) -> ControlRow {
    ControlRow { button, key, pad }
}

/// L2/R2 default to the analog triggers. The left touchpad click (photo mode) has no button of its
/// own by default, as L1 + the right click's button presses it, so its default input is empty.
pub static CONTROLS: [ControlRow; 17] = [
    row("cross", "Space", Some("a")),
    row("circle", "Left Shift", Some("b")),
    row("square", "E", Some("x")),
    row("triangle", "Q", Some("y")),
    row("l1", "1", Some("leftshoulder")),
    row("r1", "3", Some("rightshoulder")),
    row("l2", "R", Some("lefttrigger")),
    row("r2", "F", Some("righttrigger")),
    row("l3", "Z", Some("leftstick")),
    row("r3", "C", Some("rightstick")),
    row("options", "Return", Some("start")),
    row("touchpad_left", "Tab", Some("")),
    row("touchpad_right", "Backspace", Some("back")),
    row("up", "I", Some("dpup")),
    row("down", "K", Some("dpdown")),
    row("left", "J", Some("dpleft")),
    row("right", "L", Some("dpright")),
];

/// Gamepad buttons by SDL name, labelled as on a PlayStation, an Xbox or a Nintendo controller.
pub static PAD_STYLES: &[(&str, &[(&str, &str)])] = &[
    (
        "playstation",
        &[
            ("a", "Cross"),
            ("b", "Circle"),
            ("x", "Square"),
            ("y", "Triangle"),
            ("leftshoulder", "L1"),
            ("rightshoulder", "R1"),
            ("lefttrigger", "L2"),
            ("righttrigger", "R2"),
            ("leftstick", "L3"),
            ("rightstick", "R3"),
            ("start", "Options"),
            ("back", "Share / Create"),
            ("touchpad", "Touchpad click"),
            ("guide", "PS button"),
            ("dpup", "D-pad up"),
            ("dpdown", "D-pad down"),
            ("dpleft", "D-pad left"),
            ("dpright", "D-pad right"),
            ("misc1", "Mute button"),
        ],
    ),
    (
        "xbox",
        &[
            ("a", "A"),
            ("b", "B"),
            ("x", "X"),
            ("y", "Y"),
            ("leftshoulder", "LB"),
            ("rightshoulder", "RB"),
            ("lefttrigger", "LT"),
            ("righttrigger", "RT"),
            ("leftstick", "LS (left stick click)"),
            ("rightstick", "RS (right stick click)"),
            ("start", "Menu"),
            ("back", "View"),
            ("touchpad", "Touchpad click"),
            ("guide", "Xbox button"),
            ("dpup", "D-pad up"),
            ("dpdown", "D-pad down"),
            ("dpleft", "D-pad left"),
            ("dpright", "D-pad right"),
            ("misc1", "Share"),
        ],
    ),
    // By position, as printed on a Nintendo controller: the south button is B, the east one A.
    (
        "nintendo",
        &[
            ("a", "B"),
            ("b", "A"),
            ("x", "Y"),
            ("y", "X"),
            ("leftshoulder", "L"),
            ("rightshoulder", "R"),
            ("lefttrigger", "ZL"),
            ("righttrigger", "ZR"),
            ("leftstick", "Left stick click"),
            ("rightstick", "Right stick click"),
            ("start", "Plus (+)"),
            ("back", "Minus (\u{2212})"),
            ("touchpad", "Touchpad click"),
            ("guide", "Home button"),
            ("dpup", "D-pad up"),
            ("dpdown", "D-pad down"),
            ("dpleft", "D-pad left"),
            ("dpright", "D-pad right"),
            ("misc1", "Capture"),
        ],
    ),
];

/// Defaults of the keyboard & mouse table where they differ from the game's own keys: two face
/// buttons on the mouse, the shoulder buttons next to WASD, the d-pad on 1-4.
pub static KEY_DEFAULTS: &[(&str, &str)] = &[
    ("cross", "Space"),
    ("circle", "Left Shift"),
    ("square", "Mouse Left"),
    ("triangle", "Mouse Right"),
    ("l1", "Q"),
    ("r1", "E"),
    ("l2", "R"),
    ("r2", "Mouse Forward"),
    ("l3", "Z"),
    ("r3", "X"),
    ("options", "Tab"),
    ("touchpad_left", "P"),
    ("touchpad_right", "Escape"),
    ("up", "1"),
    ("right", "2"),
    ("down", "3"),
    ("left", "4"),
];

/// The left stick (`move_*`) and right stick (`look_*`) directions; their defaults are the game's
/// own keys, so only a changed one is sent.
pub static STICK_ROWS: [ControlRow; 8] = [
    row("move_up", "W", None),
    row("move_down", "S", None),
    row("move_left", "A", None),
    row("move_right", "D", None),
    row("look_up", "Up", None),
    row("look_down", "Down", None),
    row("look_left", "Left", None),
    row("look_right", "Right", None),
];
pub static KEY_ACTIONS: [&ControlRow; 8] = [
    &STICK_ROWS[0],
    &STICK_ROWS[1],
    &STICK_ROWS[2],
    &STICK_ROWS[3],
    &STICK_ROWS[4],
    &STICK_ROWS[5],
    &STICK_ROWS[6],
    &STICK_ROWS[7],
];
/// Settings layout: 4 = remappable keyboard stick directions, 5 = the layout this launcher writes
/// (`migrate_controls`).
pub const CONTROLS_LAYOUT: u64 = 5;

pub static MOUSE_NAMES: &[&str] = &[
    "Mouse Left",
    "Mouse Middle",
    "Mouse Right",
    "Mouse Back",
    "Mouse Forward",
    "Wheel Up",
    "Wheel Down",
];

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum Kind {
    Key,
    Pad,
}

impl Kind {
    fn name(self) -> &'static str {
        match self {
            Kind::Key => "key",
            Kind::Pad => "pad",
        }
    }
}

/// `CONTROLS` rows of a table, in display order; the keyboard one adds its actions.
pub fn table_rows(kind: Kind) -> Vec<&'static ControlRow> {
    if kind != Kind::Key {
        return CONTROLS.iter().collect();
    }
    let mut rows: Vec<&'static ControlRow> = STICK_ROWS.iter().collect();
    rows.extend(CONTROLS.iter());
    rows
}

pub fn is_mouse_name(name: &str) -> bool {
    let name = name.trim();
    MOUSE_NAMES.iter().any(|m| m.eq_ignore_ascii_case(name))
}

/// Default input of a table row (`None` when it has none).
pub fn default_input(row: &ControlRow, kind: Kind) -> Option<&'static str> {
    if kind == Kind::Pad {
        return row.pad;
    }
    if row.key.is_empty() {
        return None;
    }
    let default = KEY_DEFAULTS
        .iter()
        .find(|(button, _)| *button == row.button)
        .map_or(row.key, |(_, key)| *key);
    Some(default)
}

/// Whether every row of the `kind` table in `values` holds its default (an input without a
/// default holds nothing); case and outer spaces aside, as `control_changes` compares.
pub fn at_defaults(values: &IndexMap<String, String>, kind: Kind) -> bool {
    table_rows(kind).into_iter().all(|row| {
        let value = values.get(row.button).map_or("", |value| value.trim());
        value.eq_ignore_ascii_case(default_input(row, kind).unwrap_or_default())
    })
}

/// SDL name of a pressed key, from the neutral key names the keyboard layer reports, or `None`
/// when the key cannot be bound.
pub fn sdl_key_name(key: &str) -> Option<String> {
    const NAMED: &[&str] = &[
        "Space",
        "Return",
        "Tab",
        "Backspace",
        "Delete",
        "Escape",
        "Home",
        "End",
        "PageUp",
        "PageDown",
        "CapsLock",
        "Numlock",
        "ScrollLock",
        "Pause",
        "Application",
        "Up",
        "Down",
        "Left",
        "Right",
        "Left Shift",
        "Right Shift",
        "Left Ctrl",
        "Right Ctrl",
        "Left Alt",
        "Right Alt",
        "Equals",
        "Comma",
        "-",
        "[",
        "]",
        "\\",
        ";",
        "'",
        ".",
        "/",
        "`",
        "Keypad Enter",
        "Keypad +",
        "Keypad -",
        "Keypad *",
        "Keypad /",
        "Keypad .",
        "Keypad 0",
        "Keypad 1",
        "Keypad 2",
        "Keypad 3",
        "Keypad 4",
        "Keypad 5",
        "Keypad 6",
        "Keypad 7",
        "Keypad 8",
        "Keypad 9",
    ];
    // Comma and = are saved as Comma and Equals: their own names separate SANCTUARY_KEY_MAP entries.
    match key {
        "=" => return Some("Equals".to_string()),
        "," => return Some("Comma".to_string()),
        _ => {}
    }
    if let Some(name) = NAMED.iter().find(|n| **n == key) {
        return Some((*name).to_string());
    }
    let mut chars = key.chars();
    if let (Some(c), None) = (chars.next(), chars.next()) {
        if c.is_ascii_alphanumeric() {
            return Some(c.to_ascii_uppercase().to_string());
        }
    }
    if let Some(digits) = key.strip_prefix('F') {
        if !digits.is_empty() && digits.bytes().all(|b| b.is_ascii_digit()) {
            if let Ok(n) = digits.parse::<u32>() {
                if (1..=24).contains(&n) {
                    return Some(key.to_string());
                }
            }
        }
    }
    None
}

/// Which of a keyboard row's three inputs: the main one, or one of the two alternatives that press
/// the same button as well (`controls.key_alt`, `controls.key_alt2`).
#[derive(Debug, Clone, Copy, PartialEq, Eq, Hash)]
pub enum Slot {
    Main,
    Alternative,
    SecondAlternative,
}

impl Slot {
    const ALL: [Slot; 3] = [Slot::Main, Slot::Alternative, Slot::SecondAlternative];

    fn index(self) -> usize {
        match self {
            Slot::Main => 0,
            Slot::Alternative => 1,
            Slot::SecondAlternative => 2,
        }
    }

    /// The `controls` field that saves an alternative slot; the main inputs are saved in `key`.
    fn saved_field(self) -> Option<&'static str> {
        match self {
            Slot::Main => None,
            Slot::Alternative => Some("key_alt"),
            Slot::SecondAlternative => Some("key_alt2"),
        }
    }
}

/// Gives `name` to the `slot` input of `button`. `inputs` are the main inputs, the alternatives and
/// the second alternatives, by game button. The input that had the name (of another button, or
/// another one of this button) takes what `button` had in that slot, so no key presses two
/// buttons. "None" (no input) is never taken from another input. A main input left without a key
/// reads "None"; an alternative left without one is dropped from its table. Returns the input that
/// swapped.
pub fn bind_key(
    inputs: [&mut IndexMap<String, String>; 3],
    button: &str,
    slot: Slot,
    name: &str,
) -> Option<(String, Slot)> {
    let old = inputs[slot.index()]
        .get(button)
        .cloned()
        .unwrap_or_default();
    let wanted = name.trim().to_lowercase();
    let other = Slot::ALL.into_iter().find_map(|table_slot| {
        inputs[table_slot.index()]
            .iter()
            .find(|(other, key)| {
                (other.as_str() != button || table_slot != slot)
                    && !wanted.is_empty()
                    && wanted != "none"
                    && key.trim().to_lowercase() == wanted
            })
            .map(|(other, _)| (other.clone(), table_slot))
    });
    set_input(inputs[slot.index()], slot, button, name);
    let (other, other_slot) = other.filter(|(other, _)| !other.is_empty())?;
    set_input(inputs[other_slot.index()], other_slot, &other, &old);
    Some((other, other_slot))
}

fn set_input(table: &mut IndexMap<String, String>, slot: Slot, button: &str, value: &str) {
    if slot == Slot::Main {
        let value = if value.trim().is_empty() {
            "None"
        } else {
            value
        };
        table.insert(button.to_string(), value.to_string());
    } else if value.trim().is_empty() || value.trim().eq_ignore_ascii_case("none") {
        table.shift_remove(button);
    } else {
        table.insert(button.to_string(), value.to_string());
    }
}

/// Gives gamepad input `name` to `button`, as `bind_key` does. A button with no input of its own
/// has none to hand over, so the one that had `name` keeps it, and the page reports the conflict.
pub fn bind_pad(values: &mut IndexMap<String, String>, button: &str, name: &str) {
    if values.get(button).is_some_and(|old| old.is_empty()) {
        values.insert(button.to_string(), name.to_string());
    } else {
        bind_key(
            [values, &mut IndexMap::new(), &mut IndexMap::new()],
            button,
            Slot::Main,
            name,
        );
    }
}

/// The inputs that press more than one game button, in table order. "None" presses nothing.
pub fn conflicts<'a>(values: impl Iterator<Item = &'a String>) -> Vec<String> {
    let mut seen: Vec<String> = Vec::new();
    let mut twice = Vec::new();
    for value in values {
        let value = value.trim().to_lowercase();
        if value.is_empty() || value == "none" {
            continue;
        }
        if seen.contains(&value) && !twice.contains(&value) {
            twice.push(value.clone());
        }
        seen.push(value);
    }
    twice
}

/// The saved inputs (key) or gamepad names (pad) by game button, for the rows of that table.
/// `touchpad`, saved before the sides were split, is the left click.
pub fn control_values(controls: &Value, kind: Kind) -> IndexMap<String, String> {
    table_values(controls.get(kind.name()), kind)
}

/// The saved alternative inputs of `slot` by game button (`controls.key_alt` or `key_alt2`), for
/// the rows of the keyboard table. A row without one, an empty or "None" value and a button the
/// table lacks are left out; the main slot has none.
pub fn alternative_values(controls: &Value, slot: Slot) -> IndexMap<String, String> {
    let Some(Value::Object(saved)) = slot.saved_field().and_then(|field| controls.get(field))
    else {
        return IndexMap::new();
    };
    table_rows(Kind::Key)
        .into_iter()
        .filter_map(|row| {
            let Some(Value::String(text)) = saved.get(row.button) else {
                return None;
            };
            let text = text.trim();
            (!text.is_empty() && !text.eq_ignore_ascii_case("none"))
                .then(|| (row.button.to_string(), text.to_string()))
        })
        .collect()
}

/// `control_values` of one saved table (`controls.key` or `controls.pad`).
fn table_values(chosen: Option<&Value>, kind: Kind) -> IndexMap<String, String> {
    let Some(Value::Object(chosen)) = chosen else {
        return IndexMap::new();
    };
    let mut values: Map<String, Value> = chosen
        .iter()
        .filter(|(button, _)| button.as_str() != "touchpad")
        .map(|(b, v)| (b.clone(), v.clone()))
        .collect();
    // Only the keyboard table ever saved it: an old gamepad entry stays ignored.
    if let Some(touchpad) = chosen.get("touchpad").filter(|_| kind == Kind::Key) {
        if !values.contains_key("touchpad_left") {
            values.insert("touchpad_left".to_string(), touchpad.clone());
        }
    }
    let mut out = IndexMap::new();
    for row in table_rows(kind) {
        let has_input = match kind {
            Kind::Pad => row.pad.is_some(),
            Kind::Key => !row.key.is_empty(),
        };
        if !has_input {
            continue;
        }
        if let Some(Value::String(text)) = values.get(row.button) {
            if !text.trim().is_empty() {
                out.insert(row.button.to_string(), text.clone());
            }
        }
    }
    out
}

/// The buttons whose chosen key or SDL gamepad name differs from the default.
pub fn control_changes(values: &IndexMap<String, String>, kind: Kind) -> IndexMap<String, String> {
    let mut out = IndexMap::new();
    for row in table_rows(kind) {
        let Some(default) = default_input(row, kind) else {
            continue;
        };
        let Some(value) = values.get(row.button) else {
            continue;
        };
        let value = value.trim();
        if !value.is_empty() && value.to_lowercase() != default.to_lowercase() {
            out.insert(row.button.to_string(), value.to_string());
        }
    }
    out
}

/// SANCTUARY_PAD_MAP value: `cross=a,circle=b` for the changed buttons.
pub fn control_map(controls: &Value, kind: Kind) -> String {
    let chosen = control_values(controls, kind);
    CONTROLS
        .iter()
        .filter_map(|row| {
            let value = chosen.get(row.button)?;
            if value.contains([',', '=']) {
                return None;
            }
            Some(format!("{}={}", row.button, value.trim()))
        })
        .collect::<Vec<_>>()
        .join(",")
}

/// SANCTUARY_KEY_MAP of the keyboard & mouse table: each button on its chosen input, written where
/// that differs from the game's own key, or whenever it has an alternative (`cross=Space|K`: the
/// runtime reads every name of an entry as a key of the button).
pub fn keyboard_map(controls: &Value) -> String {
    let chosen = control_values(controls, Kind::Key);
    let alternatives =
        [Slot::Alternative, Slot::SecondAlternative].map(|slot| alternative_values(controls, slot));
    let mut entries = Vec::new();
    for row in table_rows(Kind::Key) {
        if row.key.is_empty() {
            continue;
        }
        let value = chosen
            .get(row.button)
            .map(String::as_str)
            .filter(|v| !v.is_empty())
            .or_else(|| default_input(row, Kind::Key))
            .unwrap_or("")
            .trim();
        if value.contains([',', '=', '|']) {
            continue;
        }
        // A repeated key presses the button once, and an alternative left empty is skipped, so
        // a second alternative alone is written right after the main key.
        let mut names = vec![value];
        for table in &alternatives {
            let alternative = table
                .get(row.button)
                .map(String::as_str)
                .filter(|alt| !alt.contains([',', '=', '|']));
            if let Some(alternative) = alternative {
                if !names
                    .iter()
                    .any(|name| name.eq_ignore_ascii_case(alternative))
                {
                    names.push(alternative);
                }
            }
        }
        if names.len() > 1 {
            // The entry replaces every key of the button: the main one is named even when it is the
            // game's own, and a button without a main key is left with the alternatives alone.
            names.retain(|name| !name.eq_ignore_ascii_case("none"));
            entries.push(format!("{}={}", row.button, names.join("|")));
            continue;
        }
        if value.to_lowercase() != row.key.to_lowercase() {
            entries.push(format!("{}={}", row.button, value));
        }
    }
    entries.join(",")
}

/// Python truthiness of a JSON value.
fn truthy(value: &Value) -> bool {
    match value {
        Value::Null => false,
        Value::Bool(b) => *b,
        Value::Number(n) => n.as_f64().is_some_and(|f| f != 0.0),
        Value::String(s) => !s.is_empty(),
        Value::Array(a) => !a.is_empty(),
        Value::Object(o) => !o.is_empty(),
    }
}

/// The inputs a keyboard table sends (saved or default), lowercase, leaving out the `skipped` rows.
fn inputs_in_use(chosen: &IndexMap<String, String>, skipped: &[&str]) -> HashSet<String> {
    table_rows(Kind::Key)
        .into_iter()
        .filter(|row| !row.key.is_empty() && !skipped.contains(&row.button))
        .filter_map(|row| {
            chosen
                .get(row.button)
                .map(String::as_str)
                .or_else(|| default_input(row, Kind::Key))
        })
        .map(|input| input.trim().to_lowercase())
        .collect()
}

/// Leaves the `actions` that a saved keyboard table predates without an input ("None") when their
/// default one is already used by another row, in any slot; the rest follow their defaults. A
/// table with nothing saved follows the defaults, which never collide.
fn claim_new_actions(controls: &mut Map<String, Value>, actions: &[ControlRow]) {
    let chosen = table_values(controls.get("key"), Kind::Key);
    if chosen.is_empty() {
        return;
    }
    let skipped: Vec<&str> = actions.iter().map(|row| row.button).collect();
    let mut taken = inputs_in_use(&chosen, &skipped);
    let saved = Value::Object(controls.clone());
    for slot in [Slot::Alternative, Slot::SecondAlternative] {
        taken.extend(
            alternative_values(&saved, slot)
                .iter()
                .filter(|(button, _)| !skipped.contains(&button.as_str()))
                .map(|(_, input)| input.to_lowercase()),
        );
    }
    // a saved table is an object: `chosen` came from it
    if let Some(Value::Object(key)) = controls.get_mut("key") {
        for action in actions {
            if taken.contains(&action.key.to_lowercase()) {
                key.insert(action.button.to_string(), Value::from("None"));
            }
        }
    }
}

/// Settings saved before the current keyboard & mouse table (`CONTROLS_LAYOUT`). A player who
/// saved keys in Keyboard Mapping keeps the whole layout they had (the game's keys for the rest);
/// one who used Native Translation keeps that table; later layouts keep their saved keys. New
/// directions whose default input is already taken stay without one. A layout newer than this
/// launcher's is left as it is by this migration (a Save still rebuilds `controls` from the page's
/// edits). Settings of the launcher this one came from (`rage_one_input`, `native_rage`) are dropped.
pub fn migrate_controls(saved: Map<String, Value>) -> Map<String, Value> {
    let previous = saved.get("controls_layout").and_then(Value::as_f64);
    if saved.is_empty() || previous.is_some_and(|n| n >= CONTROLS_LAYOUT as f64) {
        return saved;
    }
    let mut saved = saved;
    let mut controls = match saved.get("controls") {
        Some(Value::Object(map)) => map.clone(),
        _ => Map::new(),
    };
    let native = controls.shift_remove("native");
    let native_on = saved
        .shift_remove("native_input")
        .as_ref()
        .is_some_and(truthy);
    if previous.is_none_or(|n| n < 2.0) {
        match native {
            Some(Value::Object(native)) if native_on => {
                controls.insert("key".to_string(), Value::Object(native)); // its defaults are the new ones
            }
            _ if controls.get("key").is_some_and(truthy) => {
                let layout = rebuilt_layout(&controls);
                controls.insert("key".to_string(), Value::Object(layout));
            }
            _ => {}
        }
    }
    saved.shift_remove("native_rage");
    saved.shift_remove("rage_one_input");
    if previous.is_none_or(|n| n < 4.0) {
        claim_new_actions(&mut controls, &STICK_ROWS);
    }
    saved.insert("controls".to_string(), Value::Object(controls));
    saved.insert("controls_layout".to_string(), Value::from(CONTROLS_LAYOUT));
    saved
}

/// The whole keyboard table of a player who saved keys before the table had its own defaults: the
/// saved keys, the game's keys for the rest, and each action on the input it had, or "None" when a
/// button already uses that one.
fn rebuilt_layout(controls: &Map<String, Value>) -> Map<String, Value> {
    let kept = table_values(controls.get("key"), Kind::Key);
    let mut layout: IndexMap<String, String> = CONTROLS
        .iter()
        .filter(|row| !row.key.is_empty())
        .map(|row| {
            let value = kept
                .get(row.button)
                .filter(|v| !v.is_empty())
                .map_or(row.key, String::as_str);
            (row.button.to_string(), value.to_string())
        })
        .collect();
    let taken: HashSet<String> = layout
        .values()
        .map(|value| value.trim().to_lowercase())
        .collect();
    for action in KEY_ACTIONS {
        let default = action.key;
        let value = if taken.contains(&default.to_lowercase()) {
            "None"
        } else {
            default
        };
        layout.insert(action.button.to_string(), value.to_string());
    }
    layout
        .into_iter()
        .map(|(button, value)| (button, Value::String(value)))
        .collect()
}

#[cfg(test)]
mod tests {
    use super::*;
    use serde_json::json;

    fn map(pairs: &[(&str, &str)]) -> IndexMap<String, String> {
        pairs
            .iter()
            .map(|(k, v)| (k.to_string(), v.to_string()))
            .collect()
    }

    fn defaults_of(kind: Kind) -> IndexMap<String, String> {
        table_rows(kind)
            .into_iter()
            .map(|r| {
                (
                    r.button.to_string(),
                    default_input(r, kind).unwrap_or("").to_string(),
                )
            })
            .collect()
    }

    /// The main input of `button`, as the page binds it when no alternative is involved.
    fn bind_main(
        bindings: &mut IndexMap<String, String>,
        button: &str,
        name: &str,
    ) -> Option<String> {
        bind(bindings, &mut IndexMap::new(), button, Slot::Main, name).map(|(other, _)| other)
    }

    /// `bind_key` for the tests that have no second alternatives.
    fn bind(
        main: &mut IndexMap<String, String>,
        alternatives: &mut IndexMap<String, String>,
        button: &str,
        slot: Slot,
        name: &str,
    ) -> Option<(String, Slot)> {
        bind_key(
            [main, alternatives, &mut IndexMap::new()],
            button,
            slot,
            name,
        )
    }

    fn object(value: Value) -> Map<String, Value> {
        value.as_object().unwrap().clone()
    }

    #[test]
    fn changed_buttons_in_table_order() {
        let controls = json!({"key": {"circle": " Left Shift ", "cross": "Q", "square": ""},
                              "pad": {"cross": "b"}});
        assert_eq!(
            control_map(&controls, Kind::Key),
            "cross=Q,circle=Left Shift"
        );
        assert_eq!(control_map(&controls, Kind::Pad), "cross=b");
    }

    #[test]
    fn rejects_separators_unknown_buttons_and_bad_shapes() {
        let controls = json!({"key": {"cross": "a,b", "circle": "x=y", "jump": "Q", "square": 5}});
        assert_eq!(control_map(&controls, Kind::Key), "");
        assert_eq!(control_map(&json!([]), Kind::Key), "");
        assert_eq!(control_map(&json!({}), Kind::Pad), "");
    }

    #[test]
    fn only_changes_from_the_defaults_are_kept() {
        let mut keys = defaults_of(Kind::Key);
        keys.insert("cross".into(), "q".into());
        keys.insert("circle".into(), "LEFT SHIFT".into());
        keys.insert("square".into(), "  ".into());
        assert_eq!(control_changes(&keys, Kind::Key), map(&[("cross", "q")]));
        let mut pads: IndexMap<String, String> = CONTROLS
            .iter()
            .map(|r| (r.button.to_string(), r.pad.unwrap_or("").to_string()))
            .collect();
        pads.insert("cross".into(), "b".into());
        pads.insert("l2".into(), "a".into());
        assert_eq!(
            control_changes(&pads, Kind::Pad),
            map(&[("cross", "b"), ("l2", "a")])
        );
    }

    #[test]
    fn touchpad_sides_are_separate_buttons() {
        let row = |b: &str| CONTROLS.iter().find(|r| r.button == b);
        assert!(row("touchpad").is_none());
        let left = row("touchpad_left").unwrap();
        assert_eq!((left.key, left.pad), ("Tab", Some("")));
        let right = row("touchpad_right").unwrap();
        assert_eq!((right.key, right.pad), ("Backspace", Some("back")));
        assert_eq!(
            control_map(&json!({"pad": {"touchpad_right": "guide"}}), Kind::Pad),
            "touchpad_right=guide"
        );
        assert_eq!(
            control_map(&json!({"key": {"touchpad_right": "Delete"}}), Kind::Key),
            "touchpad_right=Delete"
        );
    }

    #[test]
    fn saved_touchpad_key_moves_to_the_left_side() {
        assert_eq!(
            control_map(&json!({"key": {"touchpad": "T"}}), Kind::Key),
            "touchpad_left=T"
        );
        assert_eq!(
            control_map(
                &json!({"key": {"touchpad": "T", "touchpad_left": "Y"}}),
                Kind::Key
            ),
            "touchpad_left=Y"
        );
        assert_eq!(
            control_values(&json!({"key": {"touchpad": "T"}}), Kind::Key),
            map(&[("touchpad_left", "T")])
        );
        assert!(control_values(&json!({"pad": {"touchpad": "misc1"}}), Kind::Pad).is_empty());
    }

    #[test]
    fn both_styles_name_every_default_gamepad_button() {
        assert_eq!(PAD_STYLES.len(), 3);
        for (_, labels) in PAD_STYLES {
            for pad in CONTROLS
                .iter()
                .filter_map(|r| r.pad)
                .filter(|p| !p.is_empty())
            {
                assert!(labels.iter().any(|(name, _)| *name == pad), "{pad}");
            }
            let mut shown: Vec<&str> = labels.iter().map(|(_, l)| *l).collect();
            shown.sort_unstable();
            shown.dedup();
            assert_eq!(shown.len(), labels.len());
        }
    }

    #[test]
    fn keyboard_defaults_put_the_attacks_on_the_mouse() {
        let text = keyboard_map(&json!({}));
        let mut pairs: IndexMap<String, String> = text
            .split(',')
            .map(|e| {
                let (k, v) = e.split_once('=').unwrap();
                (k.to_string(), v.to_string())
            })
            .collect();
        assert_eq!(pairs["square"], "Mouse Left");
        assert_eq!(pairs["triangle"], "Mouse Right");
        assert_eq!(pairs["r2"], "Mouse Forward");
        for row in &CONTROLS {
            let default = KEY_DEFAULTS
                .iter()
                .find(|(b, _)| *b == row.button)
                .map_or(row.key, |(_, k)| *k);
            if default.to_lowercase() != row.key.to_lowercase() {
                assert_eq!(pairs.shift_remove(row.button).unwrap(), default);
            }
        }
        // the stick directions default to the game's own keys
        assert!(pairs.is_empty(), "{pairs:?}");
        let controls = json!({"key": {"square": "Wheel Up", "cross": "Q", "triangle": "E"}});
        assert!(keyboard_map(&controls).contains("cross=Q,square=Wheel Up,triangle=E,"));
        let mut values = defaults_of(Kind::Key);
        assert!(control_changes(&values, Kind::Key).is_empty());
        values.insert("square".into(), "E".into());
        assert_eq!(control_changes(&values, Kind::Key), map(&[("square", "E")]));
    }

    #[test]
    fn saved_keyboard_binds_keep_their_layout() {
        let old = object(
            json!({"controls": {"key": {"cross": "Q"}, "pad": {"cross": "b"}}, "pad_style": "xbox"}),
        );
        let new = migrate_controls(old);
        let key = new["controls"]["key"].as_object().unwrap();
        assert_eq!(key["cross"], "Q");
        for row in &CONTROLS {
            if row.button != "cross" {
                // the game's keys, not the new defaults
                assert_eq!(key[row.button], row.key);
            }
        }
        assert_eq!(new["controls"]["pad"], json!({"cross": "b"}));
        assert_eq!(new["controls_layout"], json!(CONTROLS_LAYOUT));
        assert_eq!(migrate_controls(new.clone()), new); // once
        let game = keyboard_map(&new["controls"]);
        assert!(game.contains("cross=Q"));
        assert!(!game.contains("square=")); // still E, the game's own key
        let apart = migrate_controls(object(
            json!({"controls": {"key": {"cross": "Q"}}, "native_rage": false}),
        ));
        assert!(!apart.contains_key("native_rage"));
        assert!(!apart.contains_key("rage_one_input"));
    }


    #[test]
    fn stick_directions_are_saved_and_sent_like_other_keyboard_actions() {
        let mut defaults = defaults_of(Kind::Key);
        let directions = [
            ("move_up", "W"),
            ("move_down", "S"),
            ("move_left", "A"),
            ("move_right", "D"),
            ("look_up", "Up"),
            ("look_down", "Down"),
            ("look_left", "Left"),
            ("look_right", "Right"),
        ];
        for (button, key) in directions {
            assert_eq!(
                defaults.get(button).map(String::as_str),
                Some(key),
                "{button}"
            );
        }
        defaults.insert("move_up".into(), "T".into());
        defaults.insert("look_left".into(), "None".into());
        let changes = control_changes(&defaults, Kind::Key);
        assert_eq!(changes, map(&[("move_up", "T"), ("look_left", "None")]));
        let spec = keyboard_map(&json!({ "key": changes }));
        assert!(spec.contains("move_up=T"));
        assert!(spec.contains("look_left=None"));
    }

    #[test]
    fn new_directions_do_not_take_keys_from_saved_actions() {
        let new = migrate_controls(object(
            json!({"controls_layout": 3, "controls": {"key": {"cross": "W", "square": "Left"}}}),
        ));
        let key = &new["controls"]["key"];
        assert_eq!(key["cross"], "W");
        assert_eq!(key["move_up"], "None");
        assert_eq!(key["look_left"], "None");
        assert_eq!(migrate_controls(new.clone()), new);
    }

    #[test]
    fn unbinding_several_inputs_does_not_swap_their_old_keys() {
        let mut bindings = map(&[
            ("move_up", "W"),
            ("move_left", "None"),
            ("look_right", "Right"),
        ]);
        assert_eq!(bind_main(&mut bindings, "move_up", "None"), None);
        assert_eq!(bind_main(&mut bindings, "look_right", "None"), None);
        assert!(
            bindings.values().all(|value| value == "None"),
            "{bindings:?}"
        );
    }

    #[test]
    fn direction_conflicts_swap_and_invalid_entries_are_not_forwarded() {
        let mut bindings = defaults_of(Kind::Key);
        assert_eq!(
            bind_main(&mut bindings, "cross", "W"),
            Some("move_up".to_string())
        );
        assert_eq!(bindings["move_up"], "Space");
        let spec = keyboard_map(
            &json!({ "key": control_changes(&bindings, Kind::Key) }),
        );
        assert!(spec.contains("move_up=Space"));
        assert!(spec.contains("cross=W"));
        let invalid = json!({"key": {"move_up": "T,cross=H"}});
        assert!(!keyboard_map(&invalid).contains("move_up="));
    }

    #[test]
    fn key_actions_cover_every_keyboard_only_row() {
        // A row added to the keyboard table must also reach the rebuilt layout.
        let in_pad_table = |button: &str| CONTROLS.iter().any(|row| row.button == button);
        for row in table_rows(Kind::Key) {
            if !in_pad_table(row.button) {
                assert!(
                    KEY_ACTIONS.iter().any(|action| action.button == row.button),
                    "{} is missing from KEY_ACTIONS",
                    row.button
                );
            }
        }
        assert_eq!(
            KEY_ACTIONS.len(),
            table_rows(Kind::Key).len() - CONTROLS.len()
        );
    }

    // Rust only (not in the Python): the migration itself leaves a newer layout as it is.
    #[test]
    fn a_newer_layout_is_left_alone() {
        let saved = json!({"controls_layout": 6, "controls": {"keyboard": {"rage": "Q"}}});
        let map = saved.as_object().unwrap().clone();
        assert_eq!(migrate_controls(map.clone()), map);
    }

    #[test]
    fn native_translation_carries_over() {
        let old = object(json!({"controls": {"key": {"cross": "Q"},
                                             "native": {"circle": "Mouse Middle"}},
                                "native_input": true, "native_rage": true}));
        let new = migrate_controls(old);
        assert_eq!(new["controls"]["key"], json!({"circle": "Mouse Middle"}));
        let busy = migrate_controls(object(
            json!({"controls": {"native": {"circle": "Wheel Up"}}, "native_input": true}),
        ));
        assert_eq!(busy["controls"]["key"], json!({"circle": "Wheel Up"}));
        assert!(new["controls"].get("native").is_none());
        assert!(!new.contains_key("native_input"));
        assert!(!new.contains_key("native_rage"));
        let unused = migrate_controls(object(
            json!({"controls": {"native": {"circle": "Mouse Middle"}}, "native_input": false}),
        ));
        assert_eq!(unused["controls"], json!({}));
    }

    #[test]
    fn new_players_get_the_new_defaults() {
        assert!(migrate_controls(Map::new()).is_empty());
        let fresh = migrate_controls(object(json!({"game_dir": "D:/games"})));
        assert_eq!(fresh["controls"], json!({}));
        assert!(keyboard_map(&fresh["controls"]).contains("square=Mouse Left"));
    }

    #[test]
    fn binding_a_used_key_swaps_the_buttons() {
        let mut bindings = map(&[
            ("cross", "Space"),
            ("circle", "Left Shift"),
            ("square", "E"),
        ]);
        assert_eq!(
            bind_main(&mut bindings, "cross", "e"),
            Some("square".to_string())
        );
        assert_eq!(
            bindings,
            map(&[
                ("cross", "e"),
                ("circle", "Left Shift"),
                ("square", "Space")
            ])
        );
        assert_eq!(bind_main(&mut bindings, "circle", "Q"), None);
        assert_eq!(bindings["circle"], "Q");
    }

    #[test]
    fn an_alternative_key_is_written_after_the_main_key() {
        let controls = json!({
            "key": {"circle": "Left Ctrl"},
            "key_alt": {"cross": "K", "circle": "L", "square": "J", "triangle": "I"}
        });
        assert!(keyboard_map(&controls).contains(
            "cross=Space|K,circle=Left Ctrl|L,square=Mouse Left|J,triangle=Mouse Right|I,"
        ));
    }

    #[test]
    fn an_alternative_alone_keeps_the_main_default() {
        // Space is the game's own key, which a plain entry leaves out; the entry names it because
        // it replaces every key of the button.
        let controls = json!({"key_alt": {"cross": "K", "l1": "G"}});
        let spec = keyboard_map(&controls);
        assert!(spec.contains("cross=Space|K,"), "{spec}");
        assert!(spec.contains("l1=Q|G,"), "{spec}");
        assert!(!spec.contains("circle="), "{spec}");
    }

    #[test]
    fn a_button_without_a_main_key_keeps_only_its_alternative() {
        let controls = json!({"key": {"cross": "None"}, "key_alt": {"cross": "K"}});
        let spec = keyboard_map(&controls);
        assert!(spec.contains("cross=K,"), "{spec}");
        assert!(!spec.contains("None|") && !spec.contains("|None"), "{spec}");
    }

    #[test]
    fn unusable_alternatives_are_not_sent() {
        // Separators, an empty or "None" value, a repeat of the main key, a wrong type and a
        // button the table lacks.
        let controls = json!({"key_alt": {
            "cross": "a,b", "circle": "x=y", "square": "A|B", "triangle": "  ", "l1": 5,
            "l2": "R", "r2": "None", "jump": "K"
        }});
        assert_eq!(
            keyboard_map(&controls),
            keyboard_map(&json!({}))
        );
        assert!(!alternative_values(&controls, Slot::Alternative).contains_key("jump"));
        assert!(!alternative_values(&controls, Slot::Alternative).contains_key("r2"));
    }

    #[test]
    fn settings_without_alternatives_write_the_same_map() {
        let controls = json!({"key": {"cross": "Q", "square": "Wheel Up"}});
        let with_empty = json!({"key": {"cross": "Q", "square": "Wheel Up"}, "key_alt": {}});
        let wrong_shape = json!({"key": {"cross": "Q", "square": "Wheel Up"}, "key_alt": ["K"]});
        let expected = keyboard_map(&controls);
        assert_eq!(keyboard_map(&with_empty), expected);
        assert_eq!(keyboard_map(&wrong_shape), expected);
        assert!(alternative_values(&controls, Slot::Alternative).is_empty());
        assert!(alternative_values(&controls, Slot::SecondAlternative).is_empty());
        assert!(!expected.contains('|'));
    }

    #[test]
    fn the_pad_map_has_no_alternatives() {
        let controls = json!({"key_alt": {"cross": "K"}, "pad": {"cross": "b"}});
        assert_eq!(control_map(&controls, Kind::Pad), "cross=b");
    }

    #[test]
    fn migrating_keeps_the_alternatives() {
        let saved = object(json!({"controls_layout": 3,
                                  "controls": {"key": {"cross": "W"}, "key_alt": {"square": "J"}}}));
        let migrated = migrate_controls(saved);
        assert_eq!(migrated["controls"]["key_alt"], json!({"square": "J"}));
        let current = object(json!({"controls_layout": CONTROLS_LAYOUT,
                                    "controls": {"key_alt": {"square": "J"}}}));
        assert_eq!(migrate_controls(current.clone()), current);
    }

    #[test]
    fn an_alternative_used_by_another_button_swaps_with_it() {
        let mut main = map(&[("cross", "Space"), ("square", "Mouse Left"), ("l1", "Q")]);
        let mut alternatives = map(&[("l1", "J")]);
        // J is the alternative of L1: Square takes it, and L1 gets what Square had there (none).
        assert_eq!(
            bind(
                &mut main,
                &mut alternatives,
                "square",
                Slot::Alternative,
                "j"
            ),
            Some(("l1".to_string(), Slot::Alternative))
        );
        assert_eq!(alternatives, map(&[("square", "j")]));
        // Q is the main key of L1: Cross's alternative takes it, and L1 is left without a key.
        assert_eq!(
            bind(
                &mut main,
                &mut alternatives,
                "cross",
                Slot::Alternative,
                "Q"
            ),
            Some(("l1".to_string(), Slot::Main))
        );
        assert_eq!(main["l1"], "None");
        assert_eq!(alternatives, map(&[("square", "j"), ("cross", "Q")]));
    }

    #[test]
    fn a_main_key_used_as_an_alternative_swaps_with_it() {
        let mut main = map(&[("cross", "Space"), ("square", "Mouse Left")]);
        let mut alternatives = map(&[("square", "J")]);
        assert_eq!(
            bind(&mut main, &mut alternatives, "cross", Slot::Main, "J"),
            Some(("square".to_string(), Slot::Alternative))
        );
        assert_eq!(main["cross"], "J");
        assert_eq!(alternatives["square"], "Space");
    }

    #[test]
    fn the_two_inputs_of_a_button_swap_when_one_takes_the_other_key() {
        let mut main = map(&[("cross", "Space")]);
        let mut alternatives = map(&[("cross", "K")]);
        assert_eq!(
            bind(&mut main, &mut alternatives, "cross", Slot::Main, "K"),
            Some(("cross".to_string(), Slot::Alternative))
        );
        assert_eq!(
            (main["cross"].as_str(), alternatives["cross"].as_str()),
            ("K", "Space")
        );
        // Rebinding a slot to its own key changes nothing.
        assert_eq!(
            bind(&mut main, &mut alternatives, "cross", Slot::Main, "K"),
            None
        );
        assert_eq!(
            (main["cross"].as_str(), alternatives["cross"].as_str()),
            ("K", "Space")
        );
    }

    #[test]
    fn clearing_an_alternative_drops_it_and_takes_nothing() {
        let mut main = map(&[("cross", "Space"), ("square", "Mouse Left")]);
        let mut alternatives = map(&[("cross", "K")]);
        assert_eq!(
            bind(
                &mut main,
                &mut alternatives,
                "cross",
                Slot::Alternative,
                "None"
            ),
            None
        );
        assert!(alternatives.is_empty());
        assert_eq!(main, map(&[("cross", "Space"), ("square", "Mouse Left")]));
        // A free key becomes the alternative; nothing else moves.
        assert_eq!(
            bind(
                &mut main,
                &mut alternatives,
                "square",
                Slot::Alternative,
                "J"
            ),
            None
        );
        assert_eq!(alternatives, map(&[("square", "J")]));
    }

    #[test]
    fn both_alternatives_are_written_in_order_after_the_main_key() {
        let controls = json!({
            "key": {"circle": "Left Ctrl", "square": "Mouse Left", "triangle": "Mouse Right"},
            "key_alt": {"square": "7", "triangle": "8", "cross": "9", "circle": "0"},
            "key_alt2": {"square": "Keypad 4", "triangle": "Keypad 8", "cross": "Keypad 2", "circle": "Keypad 6"}
        });
        let spec = keyboard_map(&controls);
        assert!(
            spec.contains(
                "cross=Space|9|Keypad 2,circle=Left Ctrl|0|Keypad 6,square=Mouse Left|7|Keypad 4,triangle=Mouse Right|8|Keypad 8,"
            ),
            "{spec}"
        );
    }

    #[test]
    fn a_second_alternative_alone_keeps_the_main_key_and_writes_no_empty_name() {
        let controls = json!({"key_alt2": {"cross": "Keypad 2"}});
        let spec = keyboard_map(&controls);
        assert!(spec.contains("cross=Space|Keypad 2,"), "{spec}");
        assert!(!spec.contains("||") && !spec.contains("|,"), "{spec}");
        // Without a main key the two alternatives are what is left.
        let controls = json!({
            "key": {"cross": "None"}, "key_alt": {"cross": "K"}, "key_alt2": {"cross": "J"}
        });
        assert!(keyboard_map(&controls).contains("cross=K|J,"));
    }

    #[test]
    fn a_repeated_or_unusable_second_alternative_is_not_sent() {
        // The main key, the first alternative, a separator and "None" add nothing.
        let controls = json!({
            "key_alt": {"cross": "K", "circle": "L"},
            "key_alt2": {"cross": "k", "circle": "a,b", "square": "Mouse Left", "triangle": "None", "jump": "Q"}
        });
        let spec = keyboard_map(&controls);
        assert!(spec.contains("cross=Space|K,"), "{spec}");
        assert!(spec.contains("circle=Left Shift|L,"), "{spec}");
        assert!(!spec.contains("square=Mouse Left|"), "{spec}");
        assert!(!spec.contains("triangle=Mouse Right|"), "{spec}");
        assert!(alternative_values(&controls, Slot::SecondAlternative)
            .get("jump")
            .is_none());
        // And a wrong shape is read as no second alternatives.
        let wrong = json!({"key_alt2": ["J"]});
        assert_eq!(keyboard_map(&wrong), keyboard_map(&json!({})));
    }

    fn three(
        main: &[(&str, &str)],
        first: &[(&str, &str)],
        second: &[(&str, &str)],
    ) -> [IndexMap<String, String>; 3] {
        [map(main), map(first), map(second)]
    }

    fn bind_in(
        tables: &mut [IndexMap<String, String>; 3],
        button: &str,
        slot: Slot,
        name: &str,
    ) -> Option<(String, Slot)> {
        let [main, first, second] = tables;
        bind_key([main, first, second], button, slot, name)
    }

    #[test]
    fn a_key_used_in_any_of_the_three_boxes_swaps_with_the_one_that_takes_it() {
        let mut tables = three(
            &[("cross", "Space"), ("square", "Mouse Left")],
            &[("cross", "K")],
            &[("cross", "Keypad 2"), ("square", "J")],
        );
        // The second alternative of Square takes the main key of Cross, which gets what that box had.
        assert_eq!(
            bind_in(&mut tables, "square", Slot::SecondAlternative, "Space"),
            Some(("cross".to_string(), Slot::Main))
        );
        assert_eq!(tables[0]["cross"], "J");
        assert_eq!(tables[2]["square"], "Space");
        // A main key takes the first alternative of another button.
        assert_eq!(
            bind_in(&mut tables, "square", Slot::Main, "K"),
            Some(("cross".to_string(), Slot::Alternative))
        );
        assert_eq!(tables[1]["cross"], "Mouse Left");
        // A second alternative takes another second alternative.
        assert_eq!(
            bind_in(&mut tables, "cross", Slot::SecondAlternative, "Space"),
            Some(("square".to_string(), Slot::SecondAlternative))
        );
        assert_eq!(tables[2]["square"], "Keypad 2");
        assert_eq!(tables[2]["cross"], "Space");
    }

    #[test]
    fn the_three_boxes_of_a_button_swap_among_themselves() {
        let mut tables = three(
            &[("cross", "Space")],
            &[("cross", "K")],
            &[("cross", "Keypad 2")],
        );
        assert_eq!(
            bind_in(&mut tables, "cross", Slot::Main, "Keypad 2"),
            Some(("cross".to_string(), Slot::SecondAlternative))
        );
        assert_eq!(
            (
                tables[0]["cross"].as_str(),
                tables[1]["cross"].as_str(),
                tables[2]["cross"].as_str()
            ),
            ("Keypad 2", "K", "Space")
        );
        // Clearing the first alternative leaves the second where it is.
        assert_eq!(
            bind_in(&mut tables, "cross", Slot::Alternative, "None"),
            None
        );
        assert!(tables[1].is_empty());
        assert_eq!(tables[2]["cross"], "Space");
    }

    #[test]
    fn migrating_keeps_the_second_alternatives() {
        let saved = object(json!({"controls_layout": 3,
                                  "controls": {"key": {"cross": "W"}, "key_alt2": {"square": "J"}}}));
        assert_eq!(
            migrate_controls(saved)["controls"]["key_alt2"],
            json!({"square": "J"})
        );
    }

    #[test]
    fn sdl_names_and_mouse_names() {
        assert_eq!(sdl_key_name("q").as_deref(), Some("Q"));
        assert_eq!(sdl_key_name("7").as_deref(), Some("7"));
        assert_eq!(sdl_key_name("F12").as_deref(), Some("F12"));
        assert_eq!(sdl_key_name("Left Shift").as_deref(), Some("Left Shift"));
        assert_eq!(sdl_key_name("Keypad 5").as_deref(), Some("Keypad 5"));
        assert_eq!(sdl_key_name("=").as_deref(), Some("Equals"));
        for key in ["Insert", "Print", "F25", "F0", "eacute", ""] {
            assert_eq!(sdl_key_name(key), None, "{key}");
        }
        assert!(is_mouse_name(" wheel down "));
        assert!(!is_mouse_name("Left Shift"));
    }
}
