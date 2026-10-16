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

namespace livedaw::ui
{

/** The main display's screens (the buttons left of the WING's display). */
enum class Screen
{
    Home,
    Effects,
    Meters,
    Routing,
    Setup,
    Library,
    Utility
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
    std::function<void (juce::AudioProcessor*)> pluginRemoved;
    std::function<void()> showPluginScanner;

    // Structure and fader banks: call after changes so the surface rebuilds.
    std::function<void()> structureChanged;
    std::function<void()> layersChanged;

    // Show files
    std::function<void()> newShow, openShow;
    std::function<void (bool saveAs)> saveShow;

    /** Sends on Fader (SOF FLIP): with a bus selected the input faders show their
        sends to it; with an input selected the bus faders show its sends. */
    bool sendsOnFader = false;
    std::function<void()> sofChanged;
};

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

/** "CH 3 Kick" */
juce::String describeStrip (const Strip&);

/** The strip's name, or its label ("CH 3") when it has a default name. */
juce::String stripName (const Strip&);

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
