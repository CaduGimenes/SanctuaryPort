//! The private PSN master / Demonware multiplayer options: validation and the BB_* environment
//! the d3pc online module reads. The page and `settings.json` use the catalog rows.

use std::path::{Path, PathBuf};

use indexmap::IndexMap;
use serde_json::{json, Value};

use crate::core::catalog::{setting, Default};
use crate::core::settings::Settings;
use crate::core::store::Store;

/// The settings the Multiplayer page writes and Reset restores.
pub const KEYS: [&str; 13] = [
    "online_enabled",
    "multiplayer_username",
    "master_server",
    "demonware_server",
    "master_port",
    "webapi_port",
    "titleauth_port",
    "signaling_port",
    "relay_port",
    "lobby_port",
    "stun_port",
    "p2p_port",
    "master_tls",
];

/// A validated multiplayer configuration.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct Network {
    pub online: bool,
    pub name: String,
    pub host: String,
    pub demonware: String,
    pub master_port: u16,
    pub webapi_port: u16,
    pub titleauth_port: u16,
    pub signaling_port: u16,
    pub relay_port: u16,
    pub lobby_port: u16,
    pub stun_port: u16,
    pub p2p_port: u16,
    pub tls: bool,
}

/// Restores every multiplayer field to its catalog default (other settings are kept).
pub fn reset(store: &mut Store) {
    for key in KEYS {
        let Some(row) = setting(key) else { continue };
        let value = match row.default {
            Default::Bool(on) => Value::Bool(on),
            Default::Int(n) => json!(n),
            Default::Text(text) => Value::String(text.to_string()),
            Default::Object => Value::Object(serde_json::Map::new()),
        };
        store.set(key, value);
    }
}

/// The online module PLAY loads, for the source checkout and a packaged build.
pub fn module_path(port_dir: &Path) -> PathBuf {
    let file = if cfg!(windows) {
        "sanctuarynet.dll"
    } else {
        "libsanctuarynet.so"
    };
    let packaged = port_dir.join("bin").join("online").join(file);
    if packaged.is_file() {
        return packaged;
    }
    port_dir.join("out").join("online").join(file)
}

pub fn module_installed(port_dir: &Path) -> bool {
    module_path(port_dir).is_file()
}

fn host_valid(host: &str) -> bool {
    let bytes = host.as_bytes();
    !host.is_empty()
        && host.len() <= 253
        && bytes.first().is_some_and(u8::is_ascii_alphanumeric)
        && bytes.last().is_some_and(u8::is_ascii_alphanumeric)
        && host
            .bytes()
            .all(|c| c.is_ascii_alphanumeric() || c == b'.' || c == b'-')
}

fn name_valid(name: &str) -> bool {
    let bytes = name.as_bytes();
    (3..=16).contains(&bytes.len())
        && bytes[0].is_ascii_alphabetic()
        && bytes
            .iter()
            .all(|c| c.is_ascii_alphanumeric() || *c == b'_' || *c == b'-')
}

fn port(settings: &Settings, key: &str, label: &str) -> Result<u16, String> {
    let text = settings.text(key);
    if text.is_empty() || !text.bytes().all(|c| c.is_ascii_digit()) {
        return Err(format!("{label} must be a whole number from 1 to 65535."));
    }
    match text.parse::<u32>() {
        Ok(value) if (1..=65535).contains(&value) => Ok(value as u16),
        _ => Err(format!("{label} must be a whole number from 1 to 65535.")),
    }
}

/// Reads and validates the saved choices; an error is the text PLAY shows.
pub fn validate(settings: &Settings) -> Result<Network, String> {
    let online = settings.flag("online_enabled");
    if !online {
        return Ok(Network {
            online: false,
            name: String::new(),
            host: String::new(),
            demonware: String::new(),
            master_port: 0,
            webapi_port: 0,
            titleauth_port: 0,
            signaling_port: 0,
            relay_port: 0,
            lobby_port: 0,
            stun_port: 0,
            p2p_port: 0,
            tls: false,
        });
    }
    let name = settings.text("multiplayer_username").trim().to_string();
    if !name_valid(&name) {
        return Err("Multiplayer username must be 3–16 ASCII letters, digits, underscores or hyphens, starting with a letter.".into());
    }
    let raw = settings.text("master_server").trim().to_string();
    if raw.is_empty() {
        return Err("Master server must be a hostname, IPv4 address, or tcp://host / tls://host URL without a path.".into());
    }
    let (scheme, authority) = match raw.split_once("://") {
        Some((scheme, rest)) => (scheme, rest),
        None => ("", raw.as_str()),
    };
    if !matches!(scheme, "" | "tcp" | "tls") {
        return Err("Master server must be a hostname, IPv4 address, or tcp://host / tls://host URL without a path.".into());
    }
    let authority = authority.strip_suffix('/').unwrap_or(authority);
    if authority.is_empty()
        || authority.contains(['/', '?', '#', '@'])
        || authority.starts_with('[')
    {
        return Err("Master server must be a hostname, IPv4 address, or tcp://host / tls://host URL without a path.".into());
    }
    let (host, embedded) = match authority.rsplit_once(':') {
        Some((host, port)) if !port.is_empty() && port.bytes().all(|c| c.is_ascii_digit()) => {
            (host, Some(port.parse::<u32>().map_err(|_| "Invalid master server address or port.")?))
        }
        Some(_) => return Err("Invalid master server address or port.".into()),
        None => (authority, None),
    };
    if !host_valid(host) {
        return Err("Master server must be a hostname, IPv4 address, or tcp://host / tls://host URL without a path.".into());
    }
    let master_port = match embedded {
        Some(port) if (1..=65535).contains(&port) => port as u16,
        Some(_) => return Err("Master TCP port must be from 1 to 65535.".into()),
        None => port(settings, "master_port", "Master TCP port")?,
    };
    let demonware = {
        let value = settings.text("demonware_server").trim().to_string();
        if value.is_empty() { host.to_string() } else { value }
    };
    if !host_valid(&demonware) {
        return Err("Demonware server must be a hostname or IPv4 address (or blank for the master host).".into());
    }
    Ok(Network {
        online: true,
        name,
        host: host.to_string(),
        demonware,
        master_port,
        webapi_port: port(settings, "webapi_port", "PSN WebAPI port")?,
        titleauth_port: port(settings, "titleauth_port", "Demonware title-auth port")?,
        signaling_port: port(settings, "signaling_port", "Signaling UDP port")?,
        relay_port: port(settings, "relay_port", "Relay UDP port")?,
        lobby_port: port(settings, "lobby_port", "Demonware lobby TCP port")?,
        stun_port: port(settings, "stun_port", "NAT discovery UDP port")?,
        p2p_port: port(settings, "p2p_port", "Local P2P UDP port")?,
        tls: scheme == "tls" || (scheme != "tcp" && settings.flag("master_tls")),
    })
}

fn offline(env: &mut IndexMap<String, String>) {
    for key in ["BB_ONLINE", "BB_D3_MASTER", "BB_D3_DEMONWARE", "SANCTUARY_ONLINE"] {
        env.insert(key.into(), "0".into());
    }
    let stale: Vec<String> = env
        .keys()
        .filter(|key| key.starts_with("BB_SHADNET_") || key.starts_with("BB_D3_"))
        .cloned()
        .collect();
    for key in stale {
        env.shift_remove(&key);
    }
    for key in ["BB_NET_MODULE", "SHADPS4_P2P_RELAY_PORT", "SHADPS4_P2P_RELAY", "BB_P2P_PORT"] {
        env.shift_remove(key);
    }
}

/// Adds the online environment to `env`; an invalid saved choice is reported and leaves the game
/// offline (PLAY refuses before this, `--play` prints the reason).
pub fn apply(settings: &Settings, env: &mut IndexMap<String, String>) -> Result<(), String> {
    let network = match validate(settings) {
        Ok(network) => network,
        Err(error) => {
            offline(env);
            return Err(error);
        }
    };
    if !network.online {
        offline(env);
        return Ok(());
    }
    env.insert("SANCTUARY_ONLINE".into(), "1".into());
    env.insert("BB_ONLINE".into(), "1".into());
    env.insert("BB_D3_MASTER".into(), "1".into());
    env.insert("BB_D3_DEMONWARE".into(), "1".into());
    env.insert(
        "BB_SHADNET_SERVER".into(),
        format!("{}:{}", network.host, network.master_port),
    );
    env.insert("BB_SHADNET_NPID".into(), network.name.clone());
    env.insert("BB_USER_NAME".into(), network.name);
    env.insert(
        "BB_SHADNET_TLS".into(),
        if network.tls { "1" } else { "0" }.into(),
    );
    env.insert(
        "BB_SHADNET_WEBAPI".into(),
        format!("http://{}:{}", network.host, network.webapi_port),
    );
    env.insert("BB_D3_DEMONWARE_HOST".into(), network.demonware.clone());
    env.insert(
        "BB_D3_TITLEAUTH".into(),
        format!("http://{}:{}", network.demonware, network.titleauth_port),
    );
    env.insert("SHADPS4_P2P_RELAY".into(), "auto".into());
    env.insert(
        "BB_SHADNET_SIGNALING_PORT".into(),
        network.signaling_port.to_string(),
    );
    env.insert(
        "SHADPS4_P2P_RELAY_PORT".into(),
        network.relay_port.to_string(),
    );
    env.insert("BB_D3_LOBBY_PORT".into(), network.lobby_port.to_string());
    env.insert("BB_D3_STUN_PORT".into(), network.stun_port.to_string());
    env.insert("BB_P2P_PORT".into(), network.p2p_port.to_string());
    Ok(())
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::core::settings::{app_defaults, Settings};

    fn settings(changes: &[(&str, Value)]) -> Settings {
        let mut map = app_defaults(&|_| None);
        map.insert("online_enabled".into(), Value::Bool(true));
        map.insert("multiplayer_username".into(), json!("PlayerOne"));
        for (key, value) in changes {
            map.insert((*key).into(), value.clone());
        }
        Settings(map)
    }

    #[test]
    fn custom_identity_and_ports_reach_the_environment() {
        let s = settings(&[
            ("master_server", json!("master.example.com")),
            ("demonware_server", json!("dw.example.com")),
            ("master_port", json!("32000")),
            ("webapi_port", json!(32002)),
            ("titleauth_port", json!(32003)),
            ("signaling_port", json!(42001)),
            ("relay_port", json!(42002)),
            ("lobby_port", json!(42003)),
            ("stun_port", json!(42004)),
            ("p2p_port", json!(42005)),
        ]);
        let mut env = IndexMap::from([("BB_NET_MODULE".to_string(), "/custom".to_string())]);
        apply(&s, &mut env).unwrap();
        assert_eq!(env["BB_SHADNET_SERVER"], "master.example.com:32000");
        assert_eq!(env["BB_SHADNET_WEBAPI"], "http://master.example.com:32002");
        assert_eq!(env["BB_D3_DEMONWARE_HOST"], "dw.example.com");
        assert_eq!(env["BB_D3_TITLEAUTH"], "http://dw.example.com:32003");
        assert_eq!(env["BB_SHADNET_NPID"], "PlayerOne");
        assert_eq!(env["BB_USER_NAME"], "PlayerOne");
        assert_eq!(env["BB_SHADNET_SIGNALING_PORT"], "42001");
        assert_eq!(env["SHADPS4_P2P_RELAY_PORT"], "42002");
        assert_eq!(env["BB_D3_LOBBY_PORT"], "42003");
        assert_eq!(env["BB_D3_STUN_PORT"], "42004");
        assert_eq!(env["BB_P2P_PORT"], "42005");
        assert_eq!(env["SANCTUARY_ONLINE"], "1");
        assert_eq!(env["BB_NET_MODULE"], "/custom");
    }

    #[test]
    fn url_forms_and_tls() {
        for (server, tls, expected, secure) in [
            ("127.0.0.1", false, "127.0.0.1:31313", false),
            ("master.example.com", true, "master.example.com:31313", true),
            ("master.example.com:32000", false, "master.example.com:32000", false),
            ("tls://master.example.com:443/", false, "master.example.com:443", true),
            ("tcp://master.example.com", true, "master.example.com:31313", false),
        ] {
            let s = settings(&[
                ("master_server", json!(server)),
                ("master_tls", json!(tls)),
            ]);
            let mut env = IndexMap::new();
            apply(&s, &mut env).unwrap();
            assert_eq!(env["BB_SHADNET_SERVER"], expected);
            assert_eq!(env["BB_SHADNET_TLS"], if secure { "1" } else { "0" });
        }
    }

    #[test]
    fn invalid_values_are_refused() {
        for (key, value) in [
            ("multiplayer_username", json!("")),
            ("multiplayer_username", json!("1Player")),
            ("multiplayer_username", json!("Player One")),
            ("master_server", json!("bad host")),
            ("master_server", json!("https://example.com")),
            ("master_server", json!("tcp://user:pass@example.com")),
            ("master_server", json!("example.com?x=1")),
            ("demonware_server", json!("bad host")),
            ("demonware_server", json!("http://dw.example.com")),
            ("master_port", json!(0)),
            ("webapi_port", json!(65536)),
            ("titleauth_port", json!("abc")),
            ("signaling_port", json!(true)),
        ] {
            assert!(validate(&settings(&[(key, value)])).is_err(), "{key}");
        }
    }

    #[test]
    fn offline_clears_inherited_online_variables() {
        let mut s = settings(&[]);
        s.set("online_enabled", Value::Bool(false));
        let mut env = IndexMap::from([
            ("BB_ONLINE".to_string(), "1".to_string()),
            ("BB_SHADNET_SERVER".to_string(), "old:1".to_string()),
            ("BB_NET_MODULE".to_string(), "/old".to_string()),
        ]);
        apply(&s, &mut env).unwrap();
        assert_eq!(env["BB_ONLINE"], "0");
        assert!(!env.contains_key("BB_SHADNET_SERVER"));
        assert!(!env.contains_key("BB_NET_MODULE"));
    }
}
