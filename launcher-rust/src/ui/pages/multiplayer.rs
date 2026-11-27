//! The Multiplayer page: the private PSN master and Demonware title server (the same fields as the
//! d3pc Python launcher). Values are saved in settings.json; PLAY gives them to the runtime as
//! BB_* environment variables (`core::multiplayer`).

use iced::widget::{button, column, text, text_input};
use iced::Element;
use serde_json::Value;

use crate::core::i18n::tr;
use crate::core::multiplayer;
use crate::ui::app::{Message, State};
use crate::ui::theme::{self, palette};
use crate::ui::widgets::row::actions;
use crate::ui::widgets::{checkbox, row, section, toggle_row, SELECT_MIN};

pub const INTRO: &str = "Match the private master server’s ports. The alternate NAT discovery port is supplied automatically by the server.";
pub const ONLINE_HINT: &str = "Connect to the private D3 master. Choose online play in the game.";
pub const USERNAME_HINT: &str = "3–16 ASCII letters, digits, underscores or hyphens; start with a letter. Each connected player needs a different name.";
pub const MASTER_HINT: &str = "Hostname, IPv4, or tcp://host / tls://host. An explicit :port overrides Master TCP port.";
pub const DEMONWARE_HINT: &str = "d3demonware host (title auth, lobby and NAT discovery). Blank uses the PSN master host.";
pub const MASTER_PORT_HINT: &str = "Identity connection. Default: 31313. Match the server’s -listen port.";
pub const WEBAPI_HINT: &str = "HTTP on the master host. Default: 31315. Match -web-listen (normally master TCP port + 2).";
pub const TITLEAUTH_HINT: &str = "HTTP on the Demonware host. Default: 31316. Match -auth-listen.";
pub const SIGNALING_HINT: &str = "Match signaling_listen (normally master TCP port + 1).";
pub const RELAY_HINT: &str = "Match relay. Used by the retained Matching2 backend.";
pub const LOBBY_HINT: &str = "Match the d3demonware server’s lobby_listen.";
pub const STUN_HINT: &str = "Match the d3demonware server’s stun_listen. The alternate port is advertised by the server.";
pub const P2P_HINT: &str = "Local Matching2 transport port. Choose a free port on this PC.";
pub const TLS_HINT: &str = "Enable only if the master uses a certificate. tcp:// and tls:// in the address override this switch.";
pub const RESET_HINT: &str = "Restore all multiplayer defaults, disable online mode and clear the username. Other launcher settings are kept.";

/// A text/number field of the page; disabled while online multiplayer is off.
fn text_row<'a>(
    state: &'a State,
    key: &'static str,
    label: &str,
    placeholder: &str,
    hint: &str,
    enabled: bool,
) -> Element<'a, Message> {
    let lang = state.lang.as_str();
    let value = state.store.text(key);
    let mut input = text_input(placeholder, &value)
        .padding([6, 10])
        .width(SELECT_MIN)
        .style(theme::text_input_dark);
    if enabled {
        input = input.on_input(move |value| Message::Set(key.to_string(), Value::String(value)));
    }
    row(
        state.rtl,
        &tr(lang, label),
        input.into(),
        Some(&tr(lang, hint)),
    )
}

pub fn view(state: &State) -> Element<'_, Message> {
    let lang = state.lang.as_str();
    let online = state.store.flag("online_enabled");
    let module = multiplayer::module_installed(&state.paths.port_dir);
    let mut page = column![
        text(tr(lang, INTRO)).size(13).style(theme::text_muted),
        section(state.rtl, &tr(lang, "Private server")),
        checkbox(
            state.rtl,
            "online_enabled",
            &tr(lang, "Enable online multiplayer"),
            online,
            Some(&tr(lang, ONLINE_HINT)),
        ),
        section(state.rtl, &tr(lang, "Identity")),
        text_row(
            state,
            "multiplayer_username",
            "Multiplayer username",
            "PlayerOne",
            USERNAME_HINT,
            online,
        ),
        section(state.rtl, &tr(lang, "PSN master")),
        text_row(
            state,
            "master_server",
            "PSN master URL / host",
            "127.0.0.1",
            MASTER_HINT,
            online,
        ),
        text_row(
            state,
            "master_port",
            "Master TCP port",
            "31313",
            MASTER_PORT_HINT,
            online,
        ),
        text_row(
            state,
            "webapi_port",
            "PSN WebAPI port",
            "31315",
            WEBAPI_HINT,
            online,
        ),
        text_row(
            state,
            "signaling_port",
            "Signaling UDP port",
            "31314",
            SIGNALING_HINT,
            online,
        ),
        text_row(
            state,
            "relay_port",
            "Relay UDP port",
            "3478",
            RELAY_HINT,
            online,
        ),
        if online {
            checkbox(
                state.rtl,
                "master_tls",
                &tr(lang, "Use TLS for master connection"),
                state.store.flag("master_tls"),
                Some(&tr(lang, TLS_HINT)),
            )
        } else {
            toggle_row(
                state.rtl,
                &tr(lang, "Use TLS for master connection"),
                state.store.flag("master_tls"),
                None::<fn(bool) -> Message>,
                Some(&tr(lang, TLS_HINT)),
            )
        },
        section(state.rtl, &tr(lang, "Demonware")),
        text_row(
            state,
            "demonware_server",
            "Demonware server host",
            "Same as the master",
            DEMONWARE_HINT,
            online,
        ),
        text_row(
            state,
            "titleauth_port",
            "Demonware title-auth port",
            "31316",
            TITLEAUTH_HINT,
            online,
        ),
        text_row(
            state,
            "lobby_port",
            "Demonware lobby TCP port",
            "3074",
            LOBBY_HINT,
            online,
        ),
        text_row(
            state,
            "stun_port",
            "NAT discovery UDP port",
            "31317",
            STUN_HINT,
            online,
        ),
        text_row(
            state,
            "p2p_port",
            "Local P2P UDP port",
            "3658",
            P2P_HINT,
            online,
        ),
        section(state.rtl, &tr(lang, "Defaults")),
        actions(
            state.rtl,
            "Reset defaults",
            button(text(tr(lang, "Reset defaults")))
                .padding([7, 14])
                .style(theme::button_flat)
                .on_press(Message::MultiplayerReset)
                .into(),
            Some(&tr(lang, RESET_HINT)),
        ),
    ]
    .spacing(2);
    if online && !module {
        page = page.push(
            text(format!(
                "{} {}",
                tr(lang, "The online module is not built:"),
                multiplayer::module_path(&state.paths.port_dir).display()
            ))
            .size(13)
            .color(palette::RED_HI),
        );
    }
    page.into()
}
