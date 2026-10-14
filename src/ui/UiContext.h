#pragma once

#include "engine/EventLog.h"
#include "engine/MixerEngine.h"
#include "engine/PluginHost.h"
#include "engine/Show.h"

#include <juce_gui_basics/juce_gui_basics.h>

#include <functional>

namespace livedaw::ui
{

/** What a strip-level view needs: the engine plus app-level actions. */
struct UiContext
{
    MixerEngine& engine;
    PluginHost& host;
    Show& show;
    EventLog& log;

    std::function<void (Strip&, int slot)> openPluginEditor;
    std::function<void (juce::AudioProcessor*)> pluginRemoved;
    std::function<void()> showPluginScanner;
    std::function<void (Strip*)> selectStrip;
    std::function<Strip*()> getSelectedStrip;
};

/** Logs the outcome of a scene/snippet recall. */
void logRecall (UiContext&, const juce::String& what, const state::ApplyResult&);

/** The insert slot menu: load / replace / remove / bypass / open editor. */
void showInsertSlotMenu (UiContext&, Strip&, int slot, juce::Component* target);

/** Loads `desc` into a slot (closing any editor of the plugin it replaces). */
bool loadPluginIntoSlot (UiContext&, Strip&, int slot, const juce::PluginDescription& desc);

/** Removes the plugin in a slot (closing its editor first). */
void clearSlot (UiContext&, Strip&, int slot);

/** Default click on a slot: open the editor if loaded, otherwise show the menu. */
void clickInsertSlot (UiContext&, Strip&, int slot, juce::Component* target);

} // namespace livedaw::ui
