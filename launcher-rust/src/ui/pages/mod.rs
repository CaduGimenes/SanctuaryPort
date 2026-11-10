//! The nine pages of the tab bar, the controller mapper's page (opened from Controls, with no tab)
//! and the drawer body of each.

pub mod advanced;
pub mod control_center;
pub mod controls;
pub mod game;
pub mod graphics;
pub mod log;
pub mod mapper;
pub mod performance;

use iced::widget::text;
use iced::Element;

use crate::ui::app::{Message, State};

/// A tab of the bottom bar. Home is the background; every other page opens in the drawer.
#[derive(Debug, Clone, Copy, PartialEq, Eq, Hash, Default)]
pub enum Page {
    #[default]
    Home,
    Graphics,
    Performance,
    Game,
    Controls,
    Advanced,
    Log,
    /// "Set up a controller", opened from the Controls page; it has no tab of its own.
    Mapper,
}

impl Page {
    /// Tab order, which is also the order of the arrow keys.
    pub const ALL: [Page; 7] = [
        Page::Home,
        Page::Graphics,
        Page::Performance,
        Page::Game,
        Page::Controls,
        Page::Advanced,
        Page::Log,
    ];

    /// The English tab label, which is also the translation key.
    pub fn label_en(self) -> &'static str {
        match self {
            Page::Home => "Home",
            Page::Graphics => "Graphics",
            Page::Performance => "Performance",
            Page::Game => "Game",
            Page::Controls => "Controls",
            Page::Advanced => "Advanced",
            Page::Log => "Log",
            Page::Mapper => "Set up a controller",
        }
    }

    /// The English subtitle under the drawer's title (the mockup's, without its sample counts),
    /// which is also the translation key; Home has no drawer.
    pub fn subtitle_en(self) -> &'static str {
        match self {
            Page::Home => "",
            Page::Graphics => {
                "Stored in sanctuary.ini; the in-game menu (Insert or R3+L2) changes the same values."
            }
            Page::Performance => {
                "Frame rate, shaders and GPU data paths. Applied when the game starts."
            }
            Page::Game => "Your game dump, saves and language.",
            Page::Controls => "Keyboard, mouse and gamepad mapping.",
            Page::Advanced => "Launcher options, diagnostics and system tools.",
            Page::Log => "Output of the last start. last_run.log holds the same text.",
            Page::Mapper => "Choose Configure next to a control. Each change is saved immediately.",
        }
    }

    /// Pages whose drawer takes 72 % of the window instead of 46 % (W1, R95).
    pub fn wide(self) -> bool {
        matches!(
            self,
            Page::Performance
                | Page::Controls
                | Page::Log
                | Page::Mapper
        )
    }

    /// The tab to the right, wrapping from Log to Home.
    pub fn next(self) -> Page {
        Page::ALL[(self.index() + 1) % Page::ALL.len()]
    }

    /// The tab to the left, wrapping from Home to Log.
    pub fn previous(self) -> Page {
        Page::ALL[(self.index() + Page::ALL.len() - 1) % Page::ALL.len()]
    }

    /// The tab that is active on this page: the controller mapper belongs to Controls.
    pub fn tab(self) -> Page {
        match self {
            Page::Mapper => Page::Controls,
            page => page,
        }
    }

    fn index(self) -> usize {
        Page::ALL
            .iter()
            .position(|page| *page == self.tab())
            .unwrap_or_default()
    }
}

/// The drawer body of `page`.
pub fn view(state: &State, page: Page) -> Element<'_, Message> {
    match page {
        Page::Home => text("…").into(),
        Page::Graphics => graphics::view(state),
        Page::Performance => performance::view(state),
        Page::Game => game::view(state),
        Page::Controls => controls::view(state),
        Page::Advanced => advanced::view(state),
        Page::Log => log::view(state),
        Page::Mapper => mapper::view(state),
    }
}
