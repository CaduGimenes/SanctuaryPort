//! Everything the runner reads from the environment with a default of its own, read once into a
//! [`RunPlan`]. An empty value counts as unset; a relative path is from the port folder (`run.sh`
//! starts `run.py` in it).

use std::path::PathBuf;

use indexmap::IndexMap;

use crate::core::catalog;
use crate::core::mods;
use crate::core::paths::Paths;

/// `run.py`'s `--timeout` default (seconds; 0 is none).
const TIMEOUT: &str = "0";
/// `run.py`'s `GOW3_CONTENT_SKU` default.
const CONTENT_SKU: &str = "full";
/// Where the development builds find their CLANG64 DLLs (`run.py`'s `MSYS2_ROOT`).
const MSYS2_ROOT: &str = r"C:\msys64";
/// The game folder next to the port when `GOW3_GAME_DIR` is not set.
const GAME_FOLDER: &str = "CUSA01623";

/// One run's settings: a field for each variable the runner reads with its own default.
#[derive(Debug, Clone, PartialEq)]
pub struct RunPlan {
    /// The port folder, with its links followed.
    pub port: PathBuf,
    pub data: PathBuf,
    pub out: PathBuf,
    pub config: PathBuf,
    pub game: PathBuf,
    /// `GOW3_PATCH_DIR`: the game update to combine with the game.
    pub update: Option<PathBuf>,
    pub mods_dir: PathBuf,
    pub mods_config: PathBuf,
    /// `GOW3_MODS_ENABLED`: `"0"` skips the mods (step 4), another value than `"0"` or `"1"`
    /// ends the run with code 2 (step 9).
    pub mods_switch: String,
    pub patches_dir: PathBuf,
    pub patches_config: PathBuf,
    /// `GOW3_PATCHES` split at `;` (an unset list is one empty name, like `run.py`'s).
    pub chosen_patches: Vec<String>,
    pub patches_only: bool,
    pub user: PathBuf,
    /// `GOW3_PROBE` as given, else the probe in the port's `bin/` or `out/`.
    pub probe: Option<PathBuf>,
    /// `GOW3_ISA_REWRITE`: `1` forces the table, `0` disables it, else the CPU decides.
    pub isa_rewrite: Option<bool>,
    pub content_sku: String,
    pub timeout: String,
    pub stream_memory: String,
    pub language: Option<String>,
    pub fsr411_dir: Option<String>,
    pub msys2_root: PathBuf,
}

impl RunPlan {
    /// The plan of a run with `env`; the defaults of the paths come from `paths`, the other
    /// defaults from `catalog::run_default` and this module's consts.
    pub fn read(paths: &Paths, env: &IndexMap<String, String>) -> RunPlan {
        let value = |key: &str| {
            env.get(key)
                .map(String::as_str)
                .filter(|value| !value.is_empty())
        };
        let port = mods::resolve(&paths.port_dir).unwrap_or_else(|_| paths.port_dir.clone());
        let path = |key: &str, default: PathBuf| value(key).map_or(default, |v| port.join(v));
        let text = |key: &str, default: &str| value(key).unwrap_or(default).to_string();
        let catalog_text = |key: &str| text(key, catalog::run_default(key).unwrap_or_default());

        // Unset, the data folder is the resolved port (`paths.data_dir` is the unresolved one); a
        // given one, like every path here, joins to the port rather than using the `Paths` methods.
        let data = path("GOW3_DATA_DIR", port.clone());
        let in_data = |key: &str, name: &str| path(key, data.join(name));
        let parent = port.parent().unwrap_or(&port).to_path_buf();
        let probe = value("GOW3_PROBE")
            .map(PathBuf::from)
            .or_else(|| paths.find_tool("spartanport-probe"));
        RunPlan {
            out: data.join("out"),
            config: in_data("GOW3_CONFIG", "gow3.ini"),
            game: path("GOW3_GAME_DIR", parent.join(GAME_FOLDER)),
            update: value("GOW3_PATCH_DIR").map(|dir| port.join(dir)),
            mods_dir: in_data("GOW3_MODS_DIR", "mods"),
            mods_config: in_data("GOW3_MODS_CONFIG", "mods.json"),
            mods_switch: catalog_text("GOW3_MODS_ENABLED"),
            patches_dir: in_data("GOW3_PATCHES_DIR", "patches"),
            patches_config: in_data("GOW3_PATCHES_CONFIG", "patches.json"),
            chosen_patches: value("GOW3_PATCHES")
                .unwrap_or_default()
                .split(';')
                .map(str::to_string)
                .collect(),
            patches_only: value("GOW3_PATCHES_ONLY") == Some("1"),
            user: in_data("GOW3_USER_DIR", "user"),
            probe,
            isa_rewrite: match value("GOW3_ISA_REWRITE") {
                Some("1") => Some(true),
                Some("0") => Some(false),
                _ => None,
            },
            content_sku: text("GOW3_CONTENT_SKU", CONTENT_SKU),
            timeout: text("GOW3_TIMEOUT", TIMEOUT),
            stream_memory: catalog_text("GOW3_STREAM_MEMORY"),
            language: value("GOW3_LANGUAGE").map(str::to_string),
            fsr411_dir: value("GOW3_FSR411_DIR").map(str::to_string),
            msys2_root: PathBuf::from(text("MSYS2_ROOT", MSYS2_ROOT)),
            data,
            port,
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::path::Path;

    fn read(port: &Path, vars: &[(&str, &str)]) -> (Paths, RunPlan) {
        let paths = Paths::from_env(port, &|_| None);
        let env: IndexMap<String, String> = vars
            .iter()
            .map(|(k, v)| (k.to_string(), v.to_string()))
            .collect();
        let plan = RunPlan::read(&paths, &env);
        (paths, plan)
    }

    #[test]
    fn plan_reads_the_catalog_run_defaults() {
        let dir = tempfile::tempdir().unwrap();
        let (_, plan) = read(&dir.path().canonicalize().unwrap(), &[]);
        let default = |key| catalog::run_default(key).unwrap();
        assert_eq!(plan.mods_switch, default("GOW3_MODS_ENABLED"));
        assert_eq!(plan.stream_memory, default("GOW3_STREAM_MEMORY"));
    }

    #[test]
    fn defaults_are_the_paths_ones() {
        let dir = tempfile::tempdir().unwrap();
        // A port folder already in its resolved form, so Paths and the plan agree on it.
        let port = mods::resolve(dir.path()).unwrap();
        let (paths, plan) = read(&port, &[]);
        assert_eq!(plan.data, paths.data_dir);
        assert_eq!(plan.out, paths.data_dir.join("out"));
        assert_eq!(plan.config, paths.ini_path());
        assert_eq!(plan.mods_dir, paths.mods_dir(""));
        assert_eq!(plan.mods_config, paths.mods_json());
        assert_eq!(plan.patches_dir, paths.patches_dir(""));
        assert_eq!(plan.patches_config, paths.patches_json());
        assert_eq!(plan.user, paths.user_dir(""));
        assert_eq!(plan.game, plan.port.parent().unwrap().join("CUSA01623"));
        assert_eq!(plan.update, None);
        assert_eq!(plan.probe, None);
        assert_eq!(plan.isa_rewrite, None);
        assert_eq!(plan.language, None);
        assert_eq!(plan.mods_switch, "1");
        assert_eq!(plan.timeout, "0");
        assert_eq!(plan.content_sku, "full");
        assert!(!plan.patches_only);
        assert_eq!(plan.chosen_patches, vec![String::new()]);
        assert_eq!(plan.msys2_root, PathBuf::from(r"C:\msys64"));
    }

    #[test]
    fn variables_override_each_default() {
        let dir = tempfile::tempdir().unwrap();
        let abs = |name: &str| dir.path().join("elsewhere").join(name);
        let text = |name: &str| abs(name).to_string_lossy().into_owned();
        let vars = [
            ("GOW3_DATA_DIR", text("data")),
            ("GOW3_CONFIG", text("my.ini")),
            ("GOW3_GAME_DIR", text("game")),
            ("GOW3_PATCH_DIR", text("update")),
            ("GOW3_MODS_DIR", text("mods")),
            ("GOW3_MODS_CONFIG", text("mods.json")),
            ("GOW3_PATCHES_DIR", text("patches")),
            ("GOW3_PATCHES_CONFIG", text("patches.json")),
            ("GOW3_USER_DIR", text("user")),
            ("GOW3_PROBE", text("probe")),
            ("MSYS2_ROOT", text("msys")),
            ("GOW3_MODS_ENABLED", "0".to_string()),
            ("GOW3_PATCHES", "A;B".to_string()),
            ("GOW3_PATCHES_ONLY", "1".to_string()),
            ("GOW3_ISA_REWRITE", "0".to_string()),
            ("GOW3_CONTENT_SKU", "demo".to_string()),
            ("GOW3_TIMEOUT", "5".to_string()),
            ("GOW3_STREAM_MEMORY", "2".to_string()),
            ("GOW3_LANGUAGE", "3".to_string()),
            ("GOW3_FSR411_DIR", "fsr".to_string()),
        ];
        let vars: Vec<(&str, &str)> = vars.iter().map(|(k, v)| (*k, v.as_str())).collect();
        let (_, plan) = read(dir.path(), &vars);
        assert_eq!(plan.data, abs("data"));
        assert_eq!(plan.out, abs("data").join("out"));
        assert_eq!(plan.config, abs("my.ini"));
        assert_eq!(plan.game, abs("game"));
        assert_eq!(plan.update, Some(abs("update")));
        assert_eq!(plan.mods_dir, abs("mods"));
        assert_eq!(plan.mods_config, abs("mods.json"));
        assert_eq!(plan.patches_dir, abs("patches"));
        assert_eq!(plan.patches_config, abs("patches.json"));
        assert_eq!(plan.user, abs("user"));
        assert_eq!(plan.probe, Some(abs("probe")));
        assert_eq!(plan.msys2_root, abs("msys"));
        assert_eq!(plan.mods_switch, "0");
        assert_eq!(plan.chosen_patches, vec!["A".to_string(), "B".to_string()]);
        assert!(plan.patches_only);
        assert_eq!(plan.isa_rewrite, Some(false));
        assert_eq!(plan.content_sku, "demo");
        assert_eq!(plan.timeout, "5");
        assert_eq!(plan.stream_memory, "2");
        assert_eq!(plan.language.as_deref(), Some("3"));
        assert_eq!(plan.fsr411_dir.as_deref(), Some("fsr"));
        let (_, forced) = read(dir.path(), &[("GOW3_ISA_REWRITE", "1")]);
        assert_eq!(forced.isa_rewrite, Some(true));
    }

    #[test]
    fn relative_values_are_from_the_port() {
        let dir = tempfile::tempdir().unwrap();
        let (_, plan) = read(
            dir.path(),
            &[
                ("GOW3_DATA_DIR", "d"),
                ("GOW3_GAME_DIR", "g"),
                ("GOW3_PATCH_DIR", "u"),
                ("GOW3_USER_DIR", "saves"),
            ],
        );
        assert_eq!(plan.data, plan.port.join("d"));
        assert_eq!(plan.game, plan.port.join("g"));
        assert_eq!(plan.update, Some(plan.port.join("u")));
        assert_eq!(plan.user, plan.port.join("saves"));
        // The defaults that follow the data folder follow the one given.
        assert_eq!(plan.mods_config, plan.port.join("d").join("mods.json"));
        assert_eq!(plan.config, plan.port.join("d").join("gow3.ini"));
    }

    #[test]
    fn empty_values_count_as_unset() {
        let dir = tempfile::tempdir().unwrap();
        let (_, plain) = read(dir.path(), &[]);
        let names = [
            "GOW3_DATA_DIR",
            "GOW3_CONFIG",
            "GOW3_GAME_DIR",
            "GOW3_PATCH_DIR",
            "GOW3_MODS_DIR",
            "GOW3_MODS_CONFIG",
            "GOW3_PATCHES_DIR",
            "GOW3_PATCHES_CONFIG",
            "GOW3_USER_DIR",
            "GOW3_PROBE",
            "MSYS2_ROOT",
            "GOW3_MODS_ENABLED",
            "GOW3_PATCHES",
            "GOW3_PATCHES_ONLY",
            "GOW3_ISA_REWRITE",
            "GOW3_CONTENT_SKU",
            "GOW3_TIMEOUT",
            "GOW3_STREAM_MEMORY",
            "GOW3_GLYPH_STYLE",
            "GOW3_LANGUAGE",
            "GOW3_FSR411_DIR",
        ];
        let vars: Vec<(&str, &str)> = names.iter().map(|name| (*name, "")).collect();
        let (_, empty) = read(dir.path(), &vars);
        assert_eq!(format!("{empty:?}"), format!("{plain:?}"));
    }

    #[test]
    fn data_defaults_follow_the_resolved_port() {
        let dir = tempfile::tempdir().unwrap();
        std::fs::create_dir_all(dir.path().join("sub")).unwrap();
        std::fs::create_dir_all(dir.path().join("port")).unwrap();
        // The same folder by two names: Paths keeps the one it was given.
        let given = dir.path().join("sub").join("..").join("port");
        let (paths, plan) = read(&given, &[]);
        assert_eq!(paths.data_dir, given);
        assert_ne!(plan.port, given);
        assert_eq!(plan.data, plan.port);
        assert_eq!(plan.out, plan.port.join("out"));
        assert_eq!(plan.config, plan.port.join("gow3.ini"));
        assert_eq!(plan.mods_dir, plan.port.join("mods"));
        assert_eq!(plan.patches_config, plan.port.join("patches.json"));
        assert_eq!(plan.user, plan.port.join("user"));
    }
}
