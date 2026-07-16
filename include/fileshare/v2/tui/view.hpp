#pragma once

// FTXUI rendering of the commander. render_commander() is a pure function of
// AppState (plus the active-panel focus), so it can be rendered to a string
// headlessly in tests -- catching layout regressions without a terminal.

#include <ftxui/dom/elements.hpp>

#include "fileshare/v2/tui/model.hpp"

namespace fileshare::v2::tui {

// The full commander screen: two panels, transfer line, op log, command line
// and the F-bar. `prompt` is the command-line text (e.g. "vit@vps:/incoming$ ").
[[nodiscard]] ftxui::Element render_commander(const AppState& app, bool admin,
                                              const std::string& prompt);

// Render one panel (exposed for focused tests).
[[nodiscard]] ftxui::Element render_panel(const Panel& p, bool active);

} // namespace fileshare::v2::tui
