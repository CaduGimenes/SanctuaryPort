#![allow(clippy::unwrap_used, clippy::expect_used)]

use std::path::{Path, PathBuf};

fn walk(root: impl AsRef<Path>) -> Vec<PathBuf> {
    let mut files = Vec::new();
    for entry in std::fs::read_dir(root).unwrap() {
        let path = entry.unwrap().path();
        if path.is_dir() {
            files.extend(walk(&path));
        } else {
            files.push(path);
        }
    }
    files
}

#[test]
fn core_never_mentions_iced() {
    for root in [concat!(env!("CARGO_MANIFEST_DIR"), "/src/core")] {
        for entry in walk(root) {
            // `iced_x86` (an x86 decoder) is not the UI toolkit: only that exact token is allowed.
            let text = std::fs::read_to_string(&entry)
                .unwrap()
                .replace("iced_x86", "");
            assert!(!text.contains("iced"), "{entry:?} imports iced");
        }
    }
}
