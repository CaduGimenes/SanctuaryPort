// SPDX-FileCopyrightText: Copyright 2026 bbport contributors
// SPDX-License-Identifier: GPL-2.0-or-later
// sanctuary: the on-screen keyboard of the game's text dialog (ImeDialog, the character name),
// drawn by sanctuary_overlay.cpp over the frame. A pad moves the selection over the keys and Cross
// presses one; the layout and the selection live here so they can be tested without ImGui
// (tests/test_osk.cpp).
#pragma once

#include <cstdint>
#include <string>

namespace SanctuaryOverlay::Osk {

enum class Action : std::uint8_t { Char, Space, Backspace, Shift, Confirm, Cancel };

/// One key: the label shown, the label while Shift is on (nullptr: the same) and what pressing it
/// does. A Char key inserts its shown label's first byte (ASCII letters and digits).
struct Key {
    const char* label;
    const char* shifted;
    Action action;
};

struct Row {
    const Key* keys;
    int count;
};

inline constexpr Key kDigits[] = {
    {"1", nullptr, Action::Char}, {"2", nullptr, Action::Char}, {"3", nullptr, Action::Char},
    {"4", nullptr, Action::Char}, {"5", nullptr, Action::Char}, {"6", nullptr, Action::Char},
    {"7", nullptr, Action::Char}, {"8", nullptr, Action::Char}, {"9", nullptr, Action::Char},
    {"0", nullptr, Action::Char},
};
inline constexpr Key kRowQ[] = {
    {"Q", "q", Action::Char}, {"W", "w", Action::Char}, {"E", "e", Action::Char},
    {"R", "r", Action::Char}, {"T", "t", Action::Char}, {"Y", "y", Action::Char},
    {"U", "u", Action::Char}, {"I", "i", Action::Char}, {"O", "o", Action::Char},
    {"P", "p", Action::Char},
};
inline constexpr Key kRowA[] = {
    {"A", "a", Action::Char}, {"S", "s", Action::Char}, {"D", "d", Action::Char},
    {"F", "f", Action::Char}, {"G", "g", Action::Char}, {"H", "h", Action::Char},
    {"J", "j", Action::Char}, {"K", "k", Action::Char}, {"L", "l", Action::Char},
};
inline constexpr Key kRowZ[] = {
    {"Z", "z", Action::Char}, {"X", "x", Action::Char}, {"C", "c", Action::Char},
    {"V", "v", Action::Char}, {"B", "b", Action::Char}, {"N", "n", Action::Char},
    {"M", "m", Action::Char}, {",", nullptr, Action::Char}, {".", nullptr, Action::Char},
    {"'", nullptr, Action::Char},
};
inline constexpr Key kActions[] = {
    {"Space", nullptr, Action::Space},   {"Del", nullptr, Action::Backspace},
    {"Shift", nullptr, Action::Shift},   {"OK", nullptr, Action::Confirm},
    {"Cancel", nullptr, Action::Cancel},
};

inline constexpr Row kRows[] = {
    {kDigits, 10}, {kRowQ, 10}, {kRowA, 9}, {kRowZ, 10}, {kActions, 5},
};
inline constexpr int kRowCount = 5;

/// The key selected and whether the next letter is inserted in lower case.
struct State {
    int row = 0;
    int col = 0;
    bool shift = false;

    void Reset() {
        row = 0;
        col = 0;
        shift = false;
    }
};

inline const Key& At(const State& state) {
    return kRows[state.row].keys[state.col];
}

inline const char* Label(const State& state, const Key& key) {
    return state.shift && key.shifted ? key.shifted : key.label;
}

/// Moves the selection: horizontally within its row (wrapping), vertically between rows with the
/// column kept where the new row is long enough.
inline void Move(State& state, int drow, int dcol) {
    if (drow) {
        const int row = state.row + drow;
        if (row >= 0 && row < kRowCount) {
            state.row = row;
            const int count = kRows[row].count;
            if (state.col >= count) {
                state.col = count - 1;
            }
        }
    }
    if (dcol) {
        const int count = kRows[state.row].count;
        state.col = (state.col + dcol % count + count) % count;
    }
}

enum class Edit : std::uint8_t { None, Append, Backspace, Clear, Confirm, Cancel };

struct Result {
    Edit edit = Edit::None;
    std::string text;
};

/// Presses the selected key. Shift toggles and returns None; a Confirm key returns Confirm (the
/// dialog then ends as Enter does), Cancel likewise (Escape), and a character key returns Append
/// with the UTF-8 text for the dialog's field.
inline Result Activate(State& state) {
    const Key& key = At(state);
    switch (key.action) {
    case Action::Char:
        return {Edit::Append, std::string(1, Label(state, key)[0])};
    case Action::Space:
        return {Edit::Append, " "};
    case Action::Backspace:
        return {Edit::Backspace, {}};
    case Action::Shift:
        state.shift = !state.shift;
        return {};
    case Action::Confirm:
        return {Edit::Confirm, {}};
    case Action::Cancel:
        return {Edit::Cancel, {}};
    }
    return {};
}

} // namespace SanctuaryOverlay::Osk
