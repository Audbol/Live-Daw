#include "ui/StartupNotice.h"
#include "engine/AppPaths.h"
#include "ui/UiContext.h"
#include "ui/WingPopups.h"

namespace livedaw::ui::startupNotice
{

juce::String text()
{
    // Each point is checked against the code and docs/ARCHITECTURE.md ("Known
    // limitations"); keep it in step when something gets done.
    return juce::String (kAppName) + " is an independent hobby project, made for fun and practice. "
           "It is not affiliated with, endorsed by or supported by Behringer or Music Tribe. "
           "\"WING\" is their trademark; it is used here only to say which console " + kAppName + " imitates.\n"
           "\n"
           + kAppName + " tries its hardest to emulate the WING's interface and behaviour faithfully, "
           "but it is incomplete and can differ from the real console.\n"
           "\n"
           "## What is missing or different\n"
           "- Sound: the channel processing and FX models recreate each unit's controls and character "
           "from the WING's parameter set, not Behringer's exact algorithms.\n"
           "- Hardware: your audio interface's inputs and outputs take the place of the WING's. "
           "There is no StageConnect, AES50 or Dante, and no remote preamp control (HA REMOTE): "
           "source GAIN is digital and +48V is only stored.\n"
           "- Recording: instead of the WING-LIVE SD cards, a multitrack recorder writes to a folder on disk "
           "(with an editor); the USB recorder uses a folder of your choice instead of a USB stick. "
           "The MP3 encoder is basic (constant bit rate, a simple masking model).\n"
           "- Plugins (VST3, AU, LV2) run inside the mixer process, not in a sandbox: a plugin that crashes "
           "takes " + kAppName + " down (safe mode then restores the show without plugins). "
           "There is no plugin delay compensation.\n"
           "- WING remote apps: the WING network protocols cover 83.7 % of the WING 3.1 parameter tree "
           "(the rest is stored but does nothing). WING Edit and WING Copilot have not been tried, "
           "and exported WING snapshots have not been loaded into a console.\n"
           "- MIDI and DAW remote control (MCU / HUI) have not been tested with real hardware.\n"
           "- LIBRARY: scene and snippet files are " + kAppName + "'s own (.lsnap / .lsnip), not the WING's "
           ".snap / .snip, and the CLIP tab plays its clip on the USB player.\n"
           "- Drawn but disabled: the GPIO and SD functions of the custom controls (there is no GPIO "
           "socket or SD card), and settings that only make sense on the hardware.\n"
           "\n"
           "## Before you use it live\n"
           "Don't rely on " + kAppName + " for a critical live show without testing it thoroughly first, "
           "with your own computer, audio interface and show. It comes with no warranty.\n"
           "\n"
           "## Licence\n"
           + kAppName + " is free software under the GNU Affero General Public License, version 3 (AGPLv3), "
           "WITHOUT ANY WARRANTY. Source code, the licence and the third-party notices (JUCE, the VST3, LV2 "
           "and ASIO SDKs, the Barlow fonts, libwing's WING parameter data): https://github.com/Audbol/Live-Daw";
}

void show (juce::PropertiesFile& settings, juce::Component& host, bool atStartup)
{
    const juce::String version (JUCE_APPLICATION_VERSION_STRING);
    if (atStartup && settings.getValue (uiprefs::noticeDismissed) == version)
        return;

    popups::NoticeOptions options;
    options.checkText = "DON'T SHOW AGAIN UNTIL THE NEXT VERSION";
    options.checked = settings.getValue (uiprefs::noticeDismissed) == version;
    // Over the whole window (the window's content), so it stays readable
    // however small the main display is.
    juce::Component* cover = &host;
    while (cover->getParentComponent() != nullptr && dynamic_cast<juce::ResizableWindow*> (cover->getParentComponent()) == nullptr)
        cover = cover->getParentComponent();
    options.coverHost = true;
    options.onClose = [&settings, version] (bool dontShow)
    {
        settings.setValue (uiprefs::noticeDismissed, dontShow ? version : juce::String());
        settings.saveIfNeeded();
    };
    popups::notice (*cover, "ABOUT " + juce::String (kAppName).toUpperCase() + " " + version, text(), std::move (options));
}

} // namespace livedaw::ui::startupNotice
