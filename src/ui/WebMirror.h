#pragma once

#include "engine/EventLog.h"
#include "engine/WebRemote.h"

#include <juce_gui_basics/juce_gui_basics.h>

#include <memory>
#include <optional>
#include <vector>

namespace livedaw::ui
{

/** The web remote's screen mirror (SETUP > REMOTE > WEB REMOTE). While a
    browser watches, the window is painted into an image as it repaints (only
    the areas that changed, at most kMaxFps times a second), the tiles whose
    pixels changed go to a worker thread that encodes them (PNG; JPEG where
    that is much smaller) for the server, and the browsers' pointer, wheel and
    key input is played into the window as JUCE mouse and key events. The
    mirror shows the window's own screen (one shared view); menus and alerts
    over the window are drawn in too. With nobody watching it costs nothing.

    Owned by the main component. Message thread only. */
class WebMirror final : private juce::Timer
{
public:
    /** `content`: the window's content (the window is found when needed). */
    WebMirror (juce::Component& content, juce::PropertiesFile&, EventLog&);
    ~WebMirror() override;

    /** Starts, restarts or stops the server to match the settings. Returns
        the error if the port could not be opened. */
    juce::String apply (const WebRemote::Settings&);
    WebRemote::Status getStatus() const { return server.getStatus(); }

    static constexpr int kTileSize = 64;
    static constexpr int kMaxFps = 30;

private:
    class RepaintTracker;
    class Encoder;

    /** A window over the main window (menu, alert), drawn into the mirror. */
    struct Overlay
    {
        juce::Component::SafePointer<juce::Component> component;
        juce::Rectangle<int> bounds; // in the main window
    };

    void timerCallback() override;
    void attach();
    void detach();
    void track (juce::Component&);
    void untrack (juce::Component*);
    /** A tracked window repainted `area` (its own coordinates). */
    void invalidate (juce::Component&, juce::Rectangle<int> area);
    void updateOverlays();
    void capture();
    void playInput();
    void play (const ld::LdWebInput&);
    void logStats();

    juce::Component& content;
    EventLog& log;
    WebRemote server;
    std::unique_ptr<Encoder> encoder;

    juce::Component::SafePointer<juce::Component> window;
    std::vector<Overlay> overlays;
    juce::RectangleList<int> dirty; // in the window
    juce::Image image;
    float scale = 1.0f;
    juce::uint32 generation = 0;
    double lastFrame = 0;
    /** The window (or overlay) a press went to: the drag stays with it. */
    juce::Component::SafePointer<juce::Component> pressTarget;
    /** A press waiting for this computer's pointer to get there. */
    std::optional<ld::LdWebInput> heldPress;
    bool warped = false;

    // SIDECAR_WEB_STATS=1: a line of statistics per second on stdout.
    bool logStatsEnabled = false;
    double statsStart = 0, paintMs = 0, paintMaxMs = 0;
    int frames = 0, tiles = 0;
    juce::int64 bytesAtStart = 0;

    JUCE_DECLARE_NON_COPYABLE (WebMirror)
};

} // namespace livedaw::ui
