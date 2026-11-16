//! SanctuaryPort: after a start that ended in an error, the log is kept as `last_crash.log` (the
//! next start overwrites `last_run.log`) and the player can open a pre-filled GitHub issue in the
//! browser. Nothing is sent by the launcher: the browser shows the issue and the player submits it.
//! User names in paths are replaced by `~` first.

use std::path::Path;

/// The repository whose bug form the report opens.
pub const NEW_ISSUE: &str = "https://github.com/CaduGimenes/SanctuaryPort/issues/new";
/// The log kept from the last failed start, beside `last_run.log`.
pub const CRASH_LOG: &str = "last_crash.log";
/// Lines of the log's end that go into the issue (the rest is attached by the player).
const TAIL_LINES: usize = 40;
const LINE_CHARS: usize = 220;

/// Copies `last_run.log` to `last_crash.log` in the same folder.
pub fn keep_crash_log(run_log: &Path) -> std::io::Result<()> {
    let target = run_log.with_file_name(CRASH_LOG);
    std::fs::copy(run_log, target).map(|_| ())
}

/// `text` with the user's home folder and any `/home/<name>`, `/Users/<name>` or
/// `C:\Users\<name>` prefix shown as `~`.
pub fn redact(text: &str, home: Option<&str>) -> String {
    let mut out = text.to_string();
    if let Some(home) = home.filter(|h| h.len() > 3) {
        out = out.replace(home, "~");
        if home.contains('\\') {
            out = out.replace(&home.replace('\\', "/"), "~");
        }
    }
    for prefix in ["/home/", "/Users/"] {
        out = redact_prefix(&out, prefix, &['/'], false);
    }
    // Windows user names often hold spaces: the name ends at the next separator only.
    redact_prefix(&out, ":\\Users\\", &['\\', '/'], true)
}

/// Replaces `<prefix><name>` (up to the next separator or end; on Unix also a space or a quote)
/// with `~`; `drive` also eats the drive letter in front of `:\Users\`.
fn redact_prefix(text: &str, prefix: &str, separators: &[char], drive: bool) -> String {
    let mut out = String::with_capacity(text.len());
    let lower = text.to_ascii_lowercase();
    let wanted = prefix.to_ascii_lowercase();
    let mut rest = 0;
    let mut from = 0;
    while let Some(found) = lower[from..].find(&wanted) {
        let at = from + found;
        let start = if drive && at > 0 { at - 1 } else { at };
        let name_start = at + prefix.len();
        let name_end = text[name_start..]
            .find(|c: char| {
                separators.contains(&c) || (!drive && (c.is_whitespace() || c == '"' || c == '\''))
            })
            .map_or(text.len(), |n| name_start + n);
        if name_end == name_start {
            from = name_start;
            continue;
        }
        out.push_str(&text[rest..start]);
        out.push('~');
        rest = name_end;
        from = name_end;
    }
    out.push_str(&text[rest..]);
    out
}

/// What goes into the issue's log field: the GPU line and the end of the log, without the
/// address-space chatter, each line cut to a sensible length.
pub fn excerpt(log: &str) -> String {
    let useful = |line: &&str| !line.starts_with("Runtime: reserved virtual range");
    let gpu = log.lines().find(|line| line.starts_with("GPU: using ")).unwrap_or("");
    let lines: Vec<&str> = log.lines().filter(useful).collect();
    let tail = &lines[lines.len().saturating_sub(TAIL_LINES)..];
    let mut out = String::new();
    if !gpu.is_empty() && !tail.contains(&gpu) {
        out.push_str(gpu);
        out.push_str("\n...\n");
    }
    for line in tail {
        let cut: String = line.chars().take(LINE_CHARS).collect();
        out.push_str(&cut);
        out.push('\n');
    }
    out
}

/// The GPU named by the log (`GPU: using <name>`), or empty.
pub fn gpu_name(log: &str) -> String {
    log.lines()
        .find_map(|line| line.strip_prefix("GPU: using "))
        .unwrap_or("")
        .trim()
        .to_string()
}

/// The pre-filled bug form for a start that ended with `code`.
pub fn issue_url(version: &str, code: i32, log: &str, home: Option<&str>) -> String {
    let log = redact(log, home);
    let description = format!(
        "The game closed with an error (exit code {code}).\n\n\
         <!-- What were you doing when it happened? Please also attach user/{CRASH_LOG}. -->"
    );
    let fields = [
        ("template", "bug_report.yml".to_string()),
        ("title", format!("Crash (exit code {code})")),
        ("version", format!("v{version}")),
        ("gpu", gpu_name(&log)),
        ("description", description),
        ("log", excerpt(&log)),
    ];
    let query: Vec<String> = fields
        .iter()
        .map(|(key, value)| format!("{key}={}", encode(value)))
        .collect();
    format!("{NEW_ISSUE}?{}", query.join("&"))
}

/// Percent-encoding for a query value (RFC 3986 unreserved characters kept).
fn encode(value: &str) -> String {
    let mut out = String::with_capacity(value.len() * 3);
    for byte in value.bytes() {
        match byte {
            b'A'..=b'Z' | b'a'..=b'z' | b'0'..=b'9' | b'-' | b'_' | b'.' | b'~' => {
                out.push(byte as char)
            }
            _ => out.push_str(&format!("%{byte:02X}")),
        }
    }
    out
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn user_names_in_paths_become_tilde() {
        assert_eq!(
            redact("Settings: /home/deck/.local/share/x.ini", None),
            "Settings: ~/.local/share/x.ini"
        );
        assert_eq!(
            redact(r"Settings: D:\EMU\x.ini, C:\Users\Stavros K\x", None),
            r"Settings: D:\EMU\x.ini, ~\x"
        );
        assert_eq!(redact(r"c:\users\ana\AppData", None), r"~\AppData");
        assert_eq!(redact("/Users/bob/Games", None), "~/Games");
        assert_eq!(
            redact("Z:\\data\\Fulano\\port", Some("Z:\\data\\Fulano")),
            "~\\port"
        );
        assert_eq!(redact("no paths here", Some("/home/x")), "no paths here");
    }

    #[test]
    fn the_excerpt_keeps_the_end_and_the_gpu() {
        let mut log = String::from("GPU: using AMD Radeon RX 9060 XT\n");
        for i in 0..100 {
            log.push_str(&format!("line {i}\n"));
            log.push_str("Runtime: reserved virtual range 0x1, size=1\n");
        }
        log.push_str("Fault: read access to 0000000000000060 at amdvlk64.dll+0x2b5f3f0\n");
        let text = excerpt(&log);
        assert!(text.starts_with("GPU: using AMD Radeon RX 9060 XT\n...\n"));
        assert!(text.contains("line 99\n") && !text.contains("line 50\n"));
        assert!(!text.contains("reserved virtual range"));
        assert!(text.ends_with("amdvlk64.dll+0x2b5f3f0\n"));
    }

    #[test]
    fn the_issue_url_fills_the_bug_form() {
        let log = "GPU: using NVIDIA GeForce RTX 4070\nSettings: /home/csab/x.ini\nHost fault (signal 11)\n";
        let url = issue_url("0.6.5", 139, log, None);
        assert!(url.starts_with(&format!("{NEW_ISSUE}?template=bug_report.yml&title=Crash")));
        assert!(url.contains("&version=v0.6.5&"));
        assert!(url.contains("&gpu=NVIDIA%20GeForce%20RTX%204070&"));
        assert!(url.contains("exit%20code%20139"));
        assert!(!url.contains("csab"));
        assert!(url.len() < 8000);
    }

    #[test]
    fn the_crash_log_is_kept_beside_the_run_log() {
        let dir = tempfile::tempdir().unwrap();
        let run = dir.path().join("last_run.log");
        std::fs::write(&run, "boom").unwrap();
        keep_crash_log(&run).unwrap();
        assert_eq!(std::fs::read_to_string(dir.path().join(CRASH_LOG)).unwrap(), "boom");
    }
}
