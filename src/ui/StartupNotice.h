#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

/** The notice Sidecar shows when it starts: what the project is (an independent
    hobby project, not Behringer's), how it differs from a real WING, and not to
    rely on it live without testing. "Don't show again until the next version"
    stores the version it was turned off for (uiprefs::noticeDismissed). SETUP >
    GENERAL > ABOUT and UTILITY (SETUP > LOG) > ABOUT show it again. */
namespace livedaw::ui::startupNotice
{

/** The notice's text (WingPopups notice markup). */
juce::String text();

/** Shows the notice over the window `host` is in (its content component).
    `atStartup`: only if it wasn't turned off for this version. */
void show (juce::PropertiesFile& settings, juce::Component& host, bool atStartup);

} // namespace livedaw::ui::startupNotice
