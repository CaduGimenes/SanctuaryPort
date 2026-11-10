//! Writing start-up options into `sanctuary.ini` while keeping the rest of the file: the launcher
//! uses it where the in-game menu's file must change in place.

use std::io;
use std::path::Path;

use indexmap::IndexMap;

use crate::core::ini::{write_lines, HEADER};

/// Rewrites `updates` in place and appends missing keys, keeping every other line. The header
/// goes only in place of a file that cannot be read: an empty file (a save cut short) gets none,
/// unlike the launcher's `save_ini`.
pub fn write_keys(path: &Path, updates: &IndexMap<String, String>) -> io::Result<()> {
    let lines = match std::fs::read(path) {
        Ok(bytes) => String::from_utf8_lossy(&bytes)
            .lines()
            .map(str::to_string)
            .collect(),
        Err(_) => vec![HEADER.to_string()],
    };
    write_lines(path, updates, &lines)
}

#[cfg(test)]
#[allow(clippy::unwrap_used, clippy::expect_used)]
mod tests {
    use super::*;
    use std::fs;

    fn map(pairs: &[(&str, &str)]) -> IndexMap<String, String> {
        pairs
            .iter()
            .map(|(k, v)| (k.to_string(), v.to_string()))
            .collect()
    }

    #[test]
    fn write_keys_on_an_empty_file_adds_no_header() {
        let dir = tempfile::tempdir().unwrap();
        let ini = dir.path().join("sanctuary.ini");
        fs::write(&ini, "").unwrap();
        write_keys(&ini, &map(&[("hdr", "1")])).unwrap();
        assert_eq!(fs::read_to_string(&ini).unwrap(), "hdr=1\n");
        // A missing file starts with the header.
        fs::remove_file(&ini).unwrap();
        write_keys(&ini, &map(&[("hdr", "1")])).unwrap();
        assert_eq!(
            fs::read_to_string(&ini).unwrap(),
            "# sanctuary settings (in-game menu: Insert / R3+L2)\nhdr=1\n"
        );
    }

    #[test]
    fn write_keys_keeps_other_lines() {
        let dir = tempfile::tempdir().unwrap();
        let ini = dir.path().join("sanctuary.ini");
        fs::write(&ini, "# comment\nshow_fps=1\nengine_fps=60\n").unwrap();
        write_keys(&ini, &map(&[("engine_fps", "240"), ("vsync", "0")])).unwrap();
        assert_eq!(
            fs::read_to_string(&ini).unwrap(),
            "# comment\nshow_fps=1\nengine_fps=240\nvsync=0\n"
        );
    }
}
