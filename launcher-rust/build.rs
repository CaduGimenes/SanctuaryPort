//! Embeds `assets/sanctuaryport.ico` into the Windows executable, so Explorer, the taskbar and a
//! shortcut to the exe show the SanctuaryPort icon (R92). Nothing happens on other systems.
//! It also forwards the release tag (`GOW3_RELEASE_TAG`) into the binary.

fn main() {
    println!("cargo:rerun-if-env-changed=GOW3_RELEASE_TAG");
    if let Ok(tag) = std::env::var("GOW3_RELEASE_TAG") {
        if !tag.is_empty() {
            println!("cargo:rustc-env=GOW3_RELEASE_TAG={tag}");
        }
    }
    println!("cargo:rerun-if-changed=build.rs");
    println!("cargo:rerun-if-changed=assets/sanctuaryport.ico");
    #[cfg(windows)]
    embed_icon();
}

#[cfg(windows)]
fn embed_icon() {
    if std::env::var("CARGO_CFG_TARGET_OS").as_deref() != Ok("windows") {
        return;
    }
    let mut resource = winresource::WindowsResource::new();
    resource.set_icon("assets/sanctuaryport.ico");
    // Without the Windows SDK's resource compiler the exe is still built, without its icon.
    if let Err(error) = resource.compile() {
        println!("cargo:warning=the launcher is built without its icon: {error}");
    }
}
