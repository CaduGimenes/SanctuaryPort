//! A section heading with a line under it; at the right edge right to left.

use iced::widget::{column, container, space, text};
use iced::{Background, Element, Fill, Left, Right};

use crate::ui::app::Message;
use crate::ui::theme::{palette, CINZEL_BOLD};

pub fn section<'a>(rtl: bool, title: &str) -> Element<'a, Message> {
    let heading = text(title.to_owned())
        .font(CINZEL_BOLD)
        .size(19)
        .color(palette::RED_HI);
    let rule = container(space())
        .width(Fill)
        .height(1)
        .style(|_| container::Style {
            background: Some(Background::Color(palette::LINE)),
            ..container::Style::default()
        });
    column![heading, rule]
        .spacing(4)
        .padding([12, 0])
        .align_x(if rtl { Right } else { Left })
        .into()
}

/// A section heading that opens and closes the rows under it (`State::open_sections`): an arrow
/// before the title, the whole heading a button. The caller shows the rows only while `open`.
pub fn section_fold<'a>(
    rtl: bool,
    title: &str,
    open: bool,
    name: &'static str,
) -> Element<'a, Message> {
    use iced::widget::{button, row};
    let arrow = text(if open { "▾" } else if rtl { "◂" } else { "▸" })
        .size(17)
        .color(palette::RED_HI);
    let heading = text(title.to_owned())
        .font(CINZEL_BOLD)
        .size(19)
        .color(palette::RED_HI);
    let label = if rtl {
        row![heading, arrow]
    } else {
        row![arrow, heading]
    }
    .spacing(8)
    .align_y(iced::Center);
    let toggle = button(label)
        .padding(0)
        .style(crate::ui::theme::button_flat)
        .on_press(Message::ToggleSection(name));
    let rule = container(space())
        .width(Fill)
        .height(1)
        .style(|_| container::Style {
            background: Some(Background::Color(palette::LINE)),
            ..container::Style::default()
        });
    column![toggle, rule]
        .spacing(4)
        .padding([12, 0])
        .align_x(if rtl { Right } else { Left })
        .into()
}
