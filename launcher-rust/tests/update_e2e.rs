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
