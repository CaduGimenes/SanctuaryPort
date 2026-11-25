//! The update path against a local server (`SANCTUARY_E2E_SERVER`, ignored otherwise): the list,
//! the newer release, the checksum and the AppImage swap.

use sanctuaryport_launcher::core::release::{newer, ReleaseId};
use sanctuaryport_launcher::core::update::{list_releases, replace_appimage};

#[test]
#[ignore]
fn an_appimage_updates_from_a_published_list() {
    let server = std::env::var("SANCTUARY_E2E_SERVER").expect("SANCTUARY_E2E_SERVER");
    std::env::set_var("SANCTUARY_RELEASES_API", format!("{server}/releases.json"));
    let list = list_releases(false).expect("list");
    let installed = ReleaseId::parse("v0.6.7", false).unwrap();
    let release = newer(&list, &installed).expect("a newer release");
    assert_eq!(release.id.version, "0.6.8");
    let dir = tempfile::tempdir().unwrap();
    let target = dir.path().join("SanctuaryPort.AppImage");
    std::fs::write(&target, b"old").unwrap();
    replace_appimage(&release.url, &target, &mut |_| {}).expect("replace");
    assert_eq!(std::fs::read(&target).unwrap(), b"NEW APPIMAGE 0.6.8\n");
    // A package whose checksum does not match is refused and the old file stays.
    std::fs::write(&target, b"old").unwrap();
    let bad = release.url.replace("x86_64", "bad");
    assert!(replace_appimage(&bad, &target, &mut |_| {}).is_err());
    assert_eq!(std::fs::read(&target).unwrap(), b"old");
}

/// A local server that answers each request with the next of `answers` and sends back the
/// requests' `If-None-Match` (None when absent).
fn scripted_server(
    answers: Vec<&'static str>,
) -> (String, std::sync::mpsc::Receiver<Option<String>>) {
    use std::io::{BufRead, BufReader, Write};
    let listener = std::net::TcpListener::bind("127.0.0.1:0").unwrap();
    let url = format!("http://{}/releases", listener.local_addr().unwrap());
    let (sent, seen) = std::sync::mpsc::channel();
    std::thread::spawn(move || {
        for answer in answers {
            let (stream, _) = listener.accept().unwrap();
            let mut reader = BufReader::new(stream.try_clone().unwrap());
            let mut etag = None;
            loop {
                let mut line = String::new();
                if reader.read_line(&mut line).unwrap() == 0 || line == "\r\n" {
                    break;
                }
                if let Some((name, value)) = line.split_once(':') {
                    if name.eq_ignore_ascii_case("if-none-match") {
                        etag = Some(value.trim().to_string());
                    }
                }
            }
            sent.send(etag).unwrap();
            let mut stream = stream;
            stream.write_all(answer.as_bytes()).unwrap();
        }
    });
    (url, seen)
}

/// #105: the list's ETag is sent back, a 304 is "not modified", and GitHub's rate-limit 403
/// becomes a wait instead of a bare "http status 403". No real GitHub call.
#[test]
fn the_list_honours_etag_and_githubs_rate_limit() {
    use sanctuaryport_launcher::core::update::{fetch_list, Fetched};
    use std::time::Duration;
    let body = "[]";
    let ok = Box::leak(
        format!(
            "HTTP/1.1 200 OK\r\nETag: \"v1\"\r\nContent-Length: {}\r\nConnection: close\r\n\r\n{body}",
            body.len()
        )
        .into_boxed_str(),
    );
    let not_modified = "HTTP/1.1 304 Not Modified\r\nETag: \"v1\"\r\nConnection: close\r\n\r\n";
    let limited = "HTTP/1.1 403 Forbidden\r\nX-RateLimit-Remaining: 0\r\nRetry-After: 420\r\nContent-Length: 0\r\nConnection: close\r\n\r\n";
    let forbidden = "HTTP/1.1 403 Forbidden\r\nX-RateLimit-Remaining: 30\r\nContent-Length: 0\r\nConnection: close\r\n\r\n";
    let (url, seen) = scripted_server(vec![ok, not_modified, limited, forbidden]);

    let first = fetch_list(&url, None).expect("the list");
    assert_eq!(
        first,
        Fetched::List {
            text: body.to_string(),
            etag: Some("\"v1\"".to_string())
        }
    );
    assert_eq!(seen.recv().unwrap(), None);
    assert_eq!(fetch_list(&url, Some("\"v1\"")).unwrap(), Fetched::NotModified);
    assert_eq!(seen.recv().unwrap().as_deref(), Some("\"v1\""));
    assert_eq!(
        fetch_list(&url, None).unwrap(),
        Fetched::Limited {
            wait: Duration::from_secs(420)
        }
    );
    let error = fetch_list(&url, None).unwrap_err().to_string();
    assert_eq!(error, "http status 403");
}
