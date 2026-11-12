#![allow(clippy::unwrap_used, clippy::expect_used)]
//! The launcher -> runner contract: what `environment` writes for each catalog row, and what the
//! runner assumes when a direct `--run` leaves a variable out.

use indexmap::IndexMap;
use serde_json::{json, Value};
use sanctuaryport_launcher::core::catalog::{self, File, Kind, Setting, SETTINGS};
use sanctuaryport_launcher::core::env::{environment, game_environment};
use sanctuaryport_launcher::core::paths::Paths;
use sanctuaryport_launcher::core::settings::{app_defaults, Settings};

/// Variables built from more than one setting, or from a setting's content, so the catalog rows
/// (`game_patches`, `controls`) carry no `env`; they are not a 1:1 encoding.
const DERIVED: &[(&str, &str)] = &[
    ("SANCTUARY_PATCHES", "game_patches"),
    ("SANCTUARY_KEY_MAP", "controls"),
    ("SANCTUARY_PAD_MAP", "controls"),
];

/// Switches the launcher parks: whatever the setting says, the game gets this value. None now: the
/// upscaler switch is shown again and reaches the game like any other flag.
const PARKED: &[(&str, &str)] = &[];

fn world() -> (Paths, tempfile::TempDir) {
    let dir = tempfile::tempdir().unwrap();
    let data = dir.path().to_string_lossy().into_owned();
    let env = move |key: &str| (key == "SANCTUARY_DATA_DIR").then(|| data.clone());
    (Paths::from_env(dir.path(), &env), dir)
}

fn settings_with(key: &str, value: Value) -> Settings {
    let mut settings = Settings(app_defaults(&|_| None));
    settings.set(key, value);
    settings
}

fn env_of(settings: &Settings, paths: &Paths) -> IndexMap<String, String> {
    environment(settings, paths, "en", &IndexMap::new(), false)
}

fn env_rows() -> impl Iterator<Item = (&'static Setting, &'static str)> {
    SETTINGS
        .iter()
        .filter(|row| row.file == File::App)
        .filter_map(|row| row.env.map(|env| (row, env.name)))
}

#[test]
fn every_env_setting_round_trips() {
    let (paths, _dir) = world();
    let mut checked = 0;
    for (row, name) in env_rows() {
        if let Some((_, fixed)) = PARKED.iter().find(|(key, _)| *key == row.key) {
            for on in [true, false] {
                let env = env_of(&settings_with(row.key, json!(on)), &paths);
                assert_eq!(env[name], *fixed, "{}", row.key);
            }
            continue;
        }
        match row.kind {
            Kind::Flag => {
                let on = env_of(&settings_with(row.key, json!(true)), &paths);
                assert_eq!(on.get(name).map(String::as_str), Some("1"), "{}", row.key);
                let off = env_of(&settings_with(row.key, json!(false)), &paths);
                assert_ne!(off.get(name).map(String::as_str), Some("1"), "{}", row.key);
                checked += 1;
            }
            Kind::Choice(choices) => {
                for choice in choices {
                    let env = env_of(&settings_with(row.key, json!(choice.value)), &paths);
                    match env.get(name) {
                        // "System" menu language: the game gets the language the launcher resolved.
                        Some(value) if row.key == "ui_language" && choice.value.is_empty() => {
                            assert_eq!(value, "en");
                        }
                        Some(value) => assert_eq!(value, choice.value, "{}", row.key),
                        // Left out: the runner's own default says the same thing.
                        None => assert!(
                            choice.value.is_empty() || choice.value == row.env.unwrap().absent,
                            "{} = {:?} is not written",
                            row.key,
                            choice.value
                        ),
                    }
                    checked += 1;
                }
            }
            _ => {}
        }
    }
    assert!(checked > 30);
}

#[test]
fn hdr_is_always_one_or_zero() {
    // Python `'1' if s['hdr'] else '0'`: written either way, over an inherited value too.
    let (paths, _dir) = world();
    let inherited: IndexMap<String, String> = [("SANCTUARY_HDR".to_string(), "1".to_string())]
        .into_iter()
        .collect();
    for (value, sent) in [
        (json!(true), "1"),
        (json!(false), "0"),
        (json!("0"), "1"),
        (json!(""), "0"),
        (json!(null), "0"),
    ] {
        let settings = settings_with("hdr", value.clone());
        for base in [IndexMap::new(), inherited.clone()] {
            let env = environment(&settings, &paths, "en", &base, true);
            assert_eq!(
                env.get("SANCTUARY_HDR").map(String::as_str),
                Some(sent),
                "{value}"
            );
        }
    }
}

#[test]
fn derived_variables_have_no_catalog_row() {
    let (paths, _dir) = world();
    let mut settings = Settings(app_defaults(&|_| None));
    settings.set(
        "controls",
        json!({"key": {"cross": "Space"}, "pad": {"cross": "b"}}),
    );
    let env = env_of(&settings, &paths);
    for (name, source) in DERIVED {
        assert!(catalog::run_default(name).is_none(), "{name}");
        assert!(catalog::setting(source).unwrap().env.is_none(), "{source}");
    }
    assert!(env.contains_key("SANCTUARY_KEY_MAP"));
    assert!(env.contains_key("SANCTUARY_PAD_MAP"));
}

#[test]
fn runner_defaults_are_run_pys() {
    // Each literal is the default run.py passes to env.get(...) (or the launcher's own fallback).
    let expected = [
        ("SANCTUARY_MODS_ENABLED", "1"), // run.py: env.get('SANCTUARY_MODS_ENABLED', '1')
        ("SANCTUARY_STREAM_MEMORY", "cached"), // run.py: env.get('SANCTUARY_STREAM_MEMORY', 'cached')
        ("SANCTUARY_LANGUAGE", "1"),     // the English text and voice, as the game assumes
    ];
    for (name, literal) in expected {
        assert_eq!(catalog::run_default(name), Some(literal), "{name}");
    }
    // Every other variable is read as plain "absent" by run.py.
    for (_, name) in env_rows() {
        if !expected.iter().any(|(n, _)| *n == name) {
            assert_eq!(catalog::run_default(name), Some(""), "{name}");
        }
    }
}

#[test]
fn environment_writes_no_file() {
    let (paths, dir) = world();
    let settings = Settings(app_defaults(&|_| None));
    let pure = environment(&settings, &paths, "en", &IndexMap::new(), true);
    let full = game_environment(&settings, &paths, "en", &IndexMap::new(), true);
    assert!(std::fs::read_dir(dir.path()).unwrap().next().is_none());
    assert_eq!(pure, full);
}
