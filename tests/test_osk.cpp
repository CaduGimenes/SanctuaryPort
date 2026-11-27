// SPDX-FileCopyrightText: Copyright 2026 bbport contributors
// SPDX-License-Identifier: GPL-2.0-or-later
// The IME dialog's on-screen keyboard (gpu/shim/bbport_osk.h): selection movement, the Shift
// label, and what the selected key asks the dialog to do. No ImGui or window needed.
#include <cassert>
#include <cstdio>
#include <string>

#include "gpu/shim/sanctuary_osk.h"

using namespace SanctuaryOverlay::Osk;

int main() {
    State s;
    assert(Label(s, At(s)) == std::string("1"));

    // Horizontal movement wraps inside the row; a shorter row clamps the column when entered.
    Move(s, 0, 1);
    assert(Label(s, At(s)) == std::string("2"));
    Move(s, 0, -2);
    assert(Label(s, At(s)) == std::string("0"));
    Move(s, 0, 1);
    assert(Label(s, At(s)) == std::string("1"));

    // Down keeps the column and clamps it on a row that has none there.
    Move(s, 1, 0);
    assert(Label(s, At(s)) == std::string("Q"));
    Move(s, 0, 9);
    assert(Label(s, At(s)) == std::string("P"));
    Move(s, 1, 0); // A..L row: column 9 becomes L
    assert(Label(s, At(s)) == std::string("L"));
    Move(s, 1, 0); // Z..:' row: column 8
    assert(Label(s, At(s)) == std::string("."));
    Move(s, 1, 0); // actions row: Cancel (last)
    assert(Label(s, At(s)) == std::string("Cancel"));

    // The edges hold.
    Move(s, 1, 0);
    assert(Label(s, At(s)) == std::string("Cancel"));
    Move(s, 2, 0);
    assert(Label(s, At(s)) == std::string("Cancel"));
    s.Reset();
    Move(s, -1, 0);
    assert(Label(s, At(s)) == std::string("1"));

    // Characters: upper case by default, lower case after Shift, digits unchanged.
    s.Reset();
    Move(s, 1, 0); // Q
    Result r = Activate(s);
    assert(r.edit == Edit::Append && r.text == "Q");

    Move(s, 3, 0); // actions row, column 0: Space
    Move(s, 0, 2); // Shift
    assert(Label(s, At(s)) == std::string("Shift"));
    r = Activate(s);
    assert(r.edit == Edit::None);
    assert(s.shift);
    Move(s, 0, -2); // back to Space's column
    Move(s, -1, 0); // Z row
    assert(Label(s, At(s)) == std::string("z"));
    r = Activate(s);
    assert(r.edit == Edit::Append && r.text == "z");
    Move(s, -2, 0); // back to the Q row: still shifted
    assert(Label(s, At(s)) == std::string("q"));
    Move(s, -1, 0); // digits: Shift does not change them
    assert(Label(s, At(s)) == std::string("1"));
    r = Activate(s);
    assert(r.edit == Edit::Append && r.text == "1");

    // The action keys ask the dialog for the same endings as the keyboard.
    Move(s, 4, 0);
    r = Activate(s);
    assert(Label(s, At(s)) == std::string("Space"));
    assert(r.edit == Edit::Append && r.text == " ");
    Move(s, 0, 1);
    r = Activate(s); // Del
    assert(r.edit == Edit::Backspace && r.text.empty());
    Move(s, 0, 2); // OK
    r = Activate(s);
    assert(r.edit == Edit::Confirm);
    Move(s, 0, 1); // Cancel
    r = Activate(s);
    assert(r.edit == Edit::Cancel);

    // Reset returns to the first key.
    s.Reset();
    assert(Label(s, At(s)) == std::string("1") && !s.shift);

    std::puts("On-screen keyboard: PASS (selection, wrap/clamp, Shift, every key action)");
}
