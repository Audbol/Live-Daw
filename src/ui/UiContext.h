#pragma once

#include "engine/EventLog.h"
#include "engine/MixerEngine.h"
#include "engine/PluginHost.h"
#include "engine/Show.h"
#include "ui/Icons.h"
#include "ui/MeterBank.h"

#include <juce_audio_devices/juce_audio_devices.h>
#include <juce_gui_basics/juce_gui_basics.h>

#include <functional>
#include <optional>

namespace livedaw::daw
{
class Session;
}

namespace livedaw
{
class MidiRemote;
}

namespace livedaw::ui
{

struct KnobBinding;

namespace cc
{
    class Assignments;
}

/** The main display's screens: the seven keys left of the WING's display, then
    the VIEW screens the fader sections' VIEW keys open (manual 5.7-5.11). */
enum class Screen
{
    Home,
    Effects,
    Meters,
    Routing,
    Setup,
    Library,
    Utility,
    Overview,
    Faders,
    Sends,
    Config,
    MuteGroups,
    Recorder,    // the WING-LIVE style recorder (the status bar's LIVE card icon)
    UsbRecorder  // the USB recorder / player (the status bar's USB icon, monitoring VIEW held)
};

/** The HOME screen's tabs for the selected strip: the overview group
    (OVERVIEW, ICON/COLOR, NAME, TAGS) and the processing blocks. */
enum class HomeTab
{
    Home,
    Icon,
    Tags,
    Input,
    Filter,
    Gate,
    Eq,
    Dynamics,
    PreInsert,
    PostInsert,
    Main,
    Sends,
    Name
};

/** What the views share: the engine, the show, app-level actions, the
    selected strip and the Sends-on-Fader state. Message thread only. */
struct UiContext
{
    UiContext (MixerEngine& e, PluginHost& h, Show& s, EventLog& l, juce::AudioDeviceManager& dm, juce::PropertiesFile& p, MeterBank& m)
        : engine (e), host (h), show (s), log (l), deviceManager (dm), settings (p), meters (m)
    {
    }

    MixerEngine& engine;
    PluginHost& host;
    Show& show;
    EventLog& log;
    juce::AudioDeviceManager& deviceManager;
    juce::PropertiesFile& settings;
    MeterBank& meters;

    // Selection (one SELECT at a time). The strip may disappear: resolve every time.
    std::function<void (Strip*)> selectStrip;
    std::function<Strip*()> getSelectedStrip;

    // Navigation
    std::function<void (Screen)> showScreen;
    std::function<void (HomeTab)> showHomeTab;

    // FX slots / plugins
    std::function<void (int slot)> openPluginEditor;
    /** Opens FX slot `slot` on the EFFECTS screen's device editor. */
    std::function<void (int slot)> editFx;
    std::function<void (juce::AudioProcessor*)> pluginRemoved;
    std::function<void()> showPluginScanner;

    // Structure and fader banks: call after changes so the surface rebuilds.
    std::function<void()> structureChanged;
    std::function<void()> layersChanged;

    // Show files
    std::function<void()> newShow, openShow;
    std::function<void (bool saveAs)> saveShow;
    /** The LIBRARY's file list: open a show file, save the show as a file. */
    std::function<void (const juce::File&)> openShowFile, saveShowTo;
    /** A show file was renamed in the LIBRARY (the open show follows it). */
    std::function<void (const juce::File& from, const juce::File& to)> showFileRenamed;

    /** Sends on Fader (SOF FLIP): with a bus selected the input faders show their
        sends to it; with an input selected the bus faders show its sends. */
    bool sendsOnFader = false;
    std::function<void()> sofChanged;
    /** ALTERNATIVE SOF MODE: the strip whose SELECT was held stays the SOF
        strip whatever is selected afterwards (nullopt: the selected strip). */
    std::optional<state::StripRef> sofDestination;

    /** The screen the main display shows (the VIEW keys light while theirs is up). */
    std::function<Screen()> getScreen;
    /** The fader section whose VIEW key opened the current VIEW screen
        (OVERVIEW / CONFIG show that section). */
    LayerSet::Section viewSection = LayerSet::Left;
    /** Brings `fader` onto a section: selects the layer key holding it and
        scrolls it into view. */
    std::function<void (LayerSet::Section, const state::FaderRef&)> revealFader;

    // Shortcuts (manual chapter 10)
    /** Saves the main display as a PNG in Documents/Sidecar/screens (on the
        console: hold CLR SOLO, press UTILITY). */
    std::function<void()> takeScreenshot;
    /** Surface lock (on the console: hold HOME): every control but the main
        display ignores input. */
    std::function<void (bool locked)> setSurfaceLocked;
    std::function<bool()> isSurfaceLocked;

    // Multitrack recording: the session the recorder screen, the editor and
    // the surface's MULTI TRANSP keys share (owned by the main component).
    daw::Session* recording = nullptr;
    /** Shows the multitrack editor in place of the console (false: back). */
    std::function<void (bool)> showEditor;
    std::function<bool()> isEditorShown;
    /** Only the main display (its screen keys, screen, knobs and right-hand
        column), as large as the window allows (F10, the maximize key under the
        display's right column); false: the whole surface again. */
    std::function<void (bool)> setScreenMaximized;
    std::function<bool()> isScreenMaximized;

    // Set by the SETUP page while it exists.
    /** Opens SETUP on a tab (0 GENERAL, 1 AUDIO, 2 SURFACE, 3 REMOTE, 4 DAW,
        5 PLUGINS, 6 LOG): the status bar's chips tile opens AUDIO. */
    std::function<void (int tab)> showSetupTab;
    /** Opens CUSTOM CONTROLS (CC EDIT) on SETUP, at a layer when >= 0 (SETUP
        pressed twice, the CC section's VIEW key). */
    std::function<void (int layer)> showCustomControls;
    /** Opens the MONITORS screen on SETUP (its MONITORS key, the monitoring VIEW key). */
    std::function<void()> showMonitors;

    /** The CC section's cursor keys and wheel with USE CRSR/WHEEL on: the main
        display moves its focus frame (dx, dy) or turns the focused control.
        Set by the main display (the focus model). */
    std::function<void (int dx, int dy)> moveScreenFocus;
    std::function<void (int steps)> turnScreenFocus;

    /** TOUCH FADER SELECT: an on-screen fader that was touched becomes the
        target of the knob right of the display (set by the main display). */
    std::function<void (const KnobBinding&, juce::Component* target)> setFineTarget;

    /** The custom control assignments of the show (owned by the main component). */
    cc::Assignments* customControls = nullptr;
    /** After the assignments or the custom control layer changed: the CC section redraws. */
    std::function<void()> customControlsChanged;

    /** MIDI remote control and DAW remote control (owned by the main component;
        SETUP > REMOTE and DAW, the surface's DAW key). */
    MidiRemote* midi = nullptr;
};

/** SETUP settings that the status bar, the surface and the pages share. */
namespace uiprefs
{
    constexpr const char* clock24 = "ui.clock24";       // bool, default true (SETUP > GENERAL > TIME)
    constexpr const char* dateFormat = "ui.dateFormat"; // "YYYY-MM-DD" (default) or "DD-MM-YYYY" (DATE)

    inline bool use24h (juce::PropertiesFile& s) { return s.getBoolValue (clock24, true); }
    inline bool dayFirst (juce::PropertiesFile& s) { return s.getValue (dateFormat, "YYYY-MM-DD") == "DD-MM-YYYY"; }

    /** "16:51:19", or "4:51:19 PM" in 12-hour mode. */
    inline juce::String formatTime (juce::PropertiesFile& s, juce::Time t)
    {
        if (use24h (s))
            return t.formatted ("%H:%M:%S");
        const int h = t.getHours() % 12;
        return juce::String (h == 0 ? 12 : h) + t.formatted (":%M:%S ") + (t.getHours() < 12 ? "AM" : "PM");
    }

    inline juce::String formatDate (juce::PropertiesFile& s, juce::Time t) { return t.formatted (dayFirst (s) ? "%d-%m-%Y" : "%Y-%m-%d"); }

    // The window (MainComponent).
    constexpr const char* screenMaximized = "ui.screenMaximized";   // bool: only the main display (F10)
    constexpr const char* fullScreen = "ui.fullScreen";             // bool: the window covers the screen (F11)
    constexpr const char* noticeDismissed = "ui.noticeDismissedFor"; // the app version the start-up notice was turned off for

    // SOLO SYSTEM (SETUP > AUDIO).
    constexpr const char* exclusiveSolo = "ui.soloExclusive";
    constexpr const char* soloFollowsSelect = "ui.soloFollowsSelect";
    constexpr const char* selectFollowsSolo = "ui.selectFollowsSolo";
    constexpr const char* sofActivatesSolo = "ui.sofActivatesSolo";

    // GENERAL (SETUP > GENERAL).
    constexpr const char* alwaysShowScene = "ui.alwaysShowScene";    // the status bar's title tile shows the active scene
    constexpr const char* useCursorWheel = "ui.useCursorWheel";      // the CC section's arrows and wheel edit the screen
    constexpr const char* touchFaderSelect = "ui.touchFaderSelect";  // a touched on-screen fader goes on the knob right of the display

    // SURFACE (SETUP > SURFACE).
    constexpr const char* sofButton = "ui.sofButton";                // SOF FLIP LED: 0 AUTO, 1 ON, 2 FLASH
    constexpr const char* showSofFrame = "ui.showSofFrame";          // a frame and SOF badge on the title tile during SOF
    constexpr const char* sourceOnScribble = "ui.sourceOnScribble";  // default on: the source tag ("IN5") on the scribble strips; off "MONO" / "ST"
    constexpr const char* faderValueOnScribble = "ui.faderValueOnScribble";  // default off: a moved fader's level replaces the icon for 1.5 s
    constexpr const char* screenFollowsStrip = "ui.screenFollowsChStrip";
    constexpr const char* stripTouchSelect = "ui.chStripTouchSelect";
    constexpr const char* faderSpeed = "ui.faderSpeed";              // 0 SLOW, 1 MED (default), 2 FAST
    constexpr const char* fullFaderPaging = "ui.fullFaderPaging";
    constexpr const char* alternativeSof = "ui.alternativeSof";
    constexpr const char* rightSectionSof = "ui.rightSectionSof";    // default on (ALL); off: the left and center sections only
    constexpr const char* userLayerLink[2] = { "ui.userLayerLinkLC", "ui.userLayerLinkCR" };
    constexpr const char* dcaSpill = "ui.dcaSpill"; // bool: SELECT on a DCA spills it (SETUP > SURFACE)
    constexpr const char* autoSelect[3] = { "ui.autoSelectL", "ui.autoSelectC", "ui.autoSelectR" };
    constexpr const char* f1f3AsCustom = "ui.f1f3AsCustomControls";

    // PLUGINS (SETUP > PLUGINS).
    constexpr const char* latencyCompensation = "engine.latencyCompensation"; // bool, default true: plugin delay compensation

    /** How long the faders take to travel to a new position (FADER SPEED). */
    inline int faderGlideMs (juce::PropertiesFile& s)
    {
        const int v = s.getIntValue (faderSpeed, 1);
        return v == 0 ? 400 : v == 2 ? 80 : 200;
    }

    /** A LIGHTS tile of SETUP > GENERAL as a brightness factor: 1 at the
        tile's default, dimmer below it and up to about 1.3 above. */
    inline float brightness (juce::PropertiesFile& s, const char* id, int defaultPercent)
    {
        auto level = [] (int percent) { return 0.25f + 0.75f * (float) juce::jlimit (0, 100, percent) / 100.0f; };
        return level (s.getIntValue (juce::String ("setup.brightness.") + id, defaultPercent)) / level (defaultPercent);
    }
} // namespace uiprefs

/** SPILL: ends `spill` where it shows, else puts it on the section a spill
    from `origin` goes to (the center for the right section, else the left;
    no origin: the left), and refreshes the fader sections. */
void toggleSpill (UiContext&, std::optional<LayerSet::Section> origin, const LayerSet::Spill&);
/** Whether a section spills `spill`. */
bool isSpilling (UiContext&, const LayerSet::Spill&);

/** Calls ctx.layersChanged asynchronously: safe to use from a callback of a
    component that the rebuild will delete. */
inline void notifyLayersChangedAsync (UiContext& ctx)
{
    juce::MessageManager::callAsync ([&ctx] { if (ctx.layersChanged) ctx.layersChanged(); });
}

/** Logs the outcome of a scene/snippet recall. */
void logRecall (UiContext&, const juce::String& what, const state::ApplyResult&);

/** The FX slot menu: load / replace / remove / bypass / open editor. */
void showFxSlotMenu (UiContext&, int slot, juce::Component* target);

/** Loads `desc` into an FX slot (closing any editor of the plugin it replaces). */
bool loadPluginIntoFx (UiContext&, int slot, const juce::PluginDescription& desc);

/** Removes the plugin in an FX slot (closing its editor first). */
void clearFx (UiContext&, int slot);

/** Default click on a slot: open the editor if loaded, otherwise show the menu. */
void clickFxSlot (UiContext&, int slot, juce::Component* target);

/** The device menu of a slot: premium (slots 1-8), standard and channel-strip
    devices, a plugin (EXT) or none. `chosen` runs after a device was selected. */
void showFxDeviceMenu (UiContext&, int slot, juce::Component* target, std::function<void()> chosen = {});

/** "CH 3 Kick" */
juce::String describeStrip (const Strip&);

/** The strip's name, or its label ("CH 3") when it has a default name. */
juce::String stripName (const Strip&);

/** A channel input index as shown: "IN 3" ("IN 03" with pad = 2), "PLAY 5"
    for a multitrack playback channel, "PLAYER 1" for the USB player, "OSC 1" for an oscillator. */
inline juce::String inputPortName (int input, int pad = 0)
{
    if (const int ch = UsbRecorder::playerChannelOf (input); ch >= 0)
        return UsbRecorder::playerSourceName (ch);
    if (const int n = MixerEngine::oscChannelOf (input); n >= 0)
        return MixerEngine::oscSourceName (n);
    if (const int ch = Recorder::playbackChannelOf (input); ch >= 0)
        return Recorder::playbackSourceName (ch);
    return "IN " + juce::String (input + 1).paddedLeft ('0', pad);
}

/** False for an input or aux channel with neither a MAIN nor an ALT source:
    the WING then switches its colour LED off ("Channel color LED switched off
    when no source is selected", firmware 1.10) and the screens draw its
    colour at half brightness. Buses, mains, matrices (and DCAs) always have
    one. */
inline bool stripHasSource (const Strip& s)
{
    return s.isOutput() || s.getInputLeft() >= 0 || s.getAltInputLeft() >= 0;
}

/** The strip's colour as colour bars, tiles and name strips draw it. */
inline juce::Colour stripDisplayColour (const Strip& s)
{
    return stripHasSource (s) ? s.getColour() : s.getColour().withMultipliedBrightness (0.5f);
}

/** The glyph of a source without a WING icon: a sine for an oscillator, a
    waveform for a playback channel, else the XLR connector. */
inline Icon sourceIcon (int port)
{
    if (MixerEngine::isOscSource (port))
        return Icon::Sine;
    if (Recorder::isPlaybackSource (port) || UsbRecorder::isPlayerSource (port))
        return Icon::Waveform;
    return Icon::Xlr;
}

/** Draws the strip's icon: its WING icon (Strip::getIcon) when set, else its kind's. */
void drawStripIcon (juce::Graphics&, const Strip&, juce::Rectangle<float> area, juce::Colour);

/** The icon of a kind of strip (XLR for channels, jack for aux, ...). */
inline Icon stripIcon (StripKind k)
{
    switch (k)
    {
        case StripKind::Channel: return Icon::Xlr;
        case StripKind::Aux:     return Icon::Jack;
        case StripKind::Bus:     return Icon::Bus;
        case StripKind::Main:    return Icon::Speakers;
        case StripKind::Matrix:  return Icon::Matrix;
    }
    return Icon::Xlr;
}

} // namespace livedaw::ui
