#pragma once

#include "engine/ConsoleState.h"
#include "engine/Layers.h"
#include "engine/PluginHost.h"

#include <juce_events/juce_events.h>

#include <functional>
#include <memory>
#include <optional>
#include <vector>

namespace livedaw
{

/** A show: the console setup plus an ordered list of scenes, each of which can
    hold snippets. Implemented in Rust (crate livedaw-session); this class is the
    C++ interface and hosts the plugins the session refers to.

      Show     - the whole file: console structure (channels, buses, plugins,
                 patching) and every scene.
      Scene    - a snapshot of the console. Recalling it applies the sections in
                 its recall scope to every strip that isn't recall-safe. It can
                 glide faders, pans and send levels over a fade time.
      Snippet  - a partial snapshot that belongs to a scene: chosen strips x chosen
                 sections. Fire it on its own, or mark it to fire automatically
                 straight after its scene is recalled.

    Scene and snippet recall never create or destroy channels or plugins, so
    recalling during a show can't cause dropouts.

    Message thread only.
*/
class Show final : private juce::Timer
{
    MixerEngine& engine;
    PluginHost& host;
    ld::Show* handle; // declared before `layers`, which views the show's layer set

public:
    /** A copy of a scene's settings (change them with the setters below). */
    struct Scene
    {
        juce::String name, notes;
        juce::uint32 scope = 0;
        double fadeSeconds = 0.0;
        int numSnippets = 0;
    };

    /** A copy of a snippet's settings. */
    struct Snippet
    {
        juce::String name;
        juce::uint32 sections = 0;
        std::vector<state::StripRef> strips;
        bool fireWithScene = false;
    };

    Show (MixerEngine&, PluginHost&);
    ~Show() override;

    juce::String getName() const { return takeString (ld::ld_show_name (handle)); }
    void setName (const juce::String&);

    /** Fader layers (default banks + user layers), saved with the show. */
    LayerSet layers;

    //==============================================================================
    // Scenes
    int getNumScenes() const noexcept { return ld::ld_show_num_scenes (handle); }
    /** A copy (const, so that writing to it by mistake doesn't compile: use the setters). */
    const std::optional<Scene> getScene (int index) const;
    int getCurrentSceneIndex() const noexcept { return ld::ld_show_current_scene (handle); }

    // Editing a scene or snippet's settings doesn't call onChanged (the editor
    // that made the change updates itself).
    void setSceneName (int index, const juce::String&);
    void setSceneNotes (int index, const juce::String&);
    void setSceneScope (int index, juce::uint32 scope);
    void setSceneFadeSeconds (int index, double seconds);

    /** Captures the current console as a new scene (appended, or inserted at `index`). */
    int addScene (const juce::String& sceneName, int index = -1);
    void storeScene (int index);
    void removeScene (int index);
    void moveScene (int from, int to);

    /** Recalls a scene (and its auto-fire snippets). */
    state::ApplyResult recallScene (int index);
    state::ApplyResult recallNext();
    state::ApplyResult recallPrevious();

    //==============================================================================
    // Snippets
    const std::optional<Snippet> getSnippet (int sceneIndex, int snippetIndex) const;
    int addSnippet (int sceneIndex, const juce::String& snippetName, juce::uint32 sections,
                    const std::vector<state::StripRef>& strips);
    void storeSnippet (int sceneIndex, int snippetIndex);
    void removeSnippet (int sceneIndex, int snippetIndex);
    state::ApplyResult recallSnippet (int sceneIndex, int snippetIndex);

    void setSnippetName (int sceneIndex, int snippetIndex, const juce::String&);
    void setSnippetSections (int sceneIndex, int snippetIndex, juce::uint32 sections);
    void setSnippetStrips (int sceneIndex, int snippetIndex, const std::vector<state::StripRef>&);
    void setSnippetFireWithScene (int sceneIndex, int snippetIndex, bool fire);

    //==============================================================================
    bool isFading() const noexcept { return ld::ld_show_is_fading (handle); }
    void cancelFade();

    //==============================================================================
    // Files
    std::unique_ptr<juce::XmlElement> toXml() const;
    juce::String toXmlString() const;

    /** Loads a show (or a v1 session file). With loadPlugins false (safe mode) the
        console is restored without instantiating any plugins. */
    juce::Result loadFromXml (const juce::XmlElement&, bool loadPlugins = true);

    /** Writes atomically (temp file + rename) and keeps the previous version as .bak,
        so a crash or power cut during a save can never leave a corrupt show. */
    juce::Result saveToFile (const juce::File&) const;
    juce::Result loadFromFile (const juce::File&, bool loadPlugins = true);

    /** Clears scenes and resets the console to `numChannels` inputs. */
    void clear (int numChannels);

    /** Called after any change to the scene list or current scene. */
    std::function<void()> onChanged;

    static constexpr const char* fileExtension = ".lshow";

private:
    void timerCallback() override;
    void changed();
    state::ApplyResult afterRecall (ld::ApplyResultC&);
    juce::Result afterLoad (bool ok, ld::LdString* message);

    static bool captureInsert (void* ctx, const ld::Strip*, int32_t slot, ld::LdSink* description, ld::LdSink* state);
    static int32_t restoreInsert (void* ctx, const ld::Strip*, int32_t slot, int32_t mode,
                                  const char* description, size_t descriptionLen,
                                  const char* state, size_t stateLen, ld::LdSink* error);

    JUCE_DECLARE_NON_COPYABLE (Show)
};

} // namespace livedaw
