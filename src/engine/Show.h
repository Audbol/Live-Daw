#pragma once

#include "engine/ConsoleState.h"

#include <juce_events/juce_events.h>

#include <functional>
#include <memory>
#include <vector>

namespace livedaw
{

/** A show: the console setup plus an ordered list of scenes, each of which can
    hold snippets.

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
public:
    struct Snippet
    {
        juce::String name;
        juce::uint32 sections = state::Fader | state::Sends;
        std::vector<state::StripRef> strips;
        bool fireWithScene = false;
        std::unique_ptr<juce::XmlElement> state;
    };

    struct Scene
    {
        juce::String name, notes;
        juce::uint32 scope = state::AllSections & ~state::Routing;
        double fadeSeconds = 0.0;
        std::unique_ptr<juce::XmlElement> state;
        std::vector<std::unique_ptr<Snippet>> snippets;
    };

    Show (MixerEngine&, PluginHost&);
    ~Show() override;

    juce::String name { "Untitled show" };

    //==============================================================================
    // Scenes
    int getNumScenes() const noexcept { return static_cast<int> (scenes.size()); }
    Scene* getScene (int index) const noexcept;
    int getCurrentSceneIndex() const noexcept { return currentScene; }

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
    int addSnippet (int sceneIndex, const juce::String& snippetName, juce::uint32 sections,
                    std::vector<state::StripRef> strips);
    void storeSnippet (int sceneIndex, int snippetIndex);
    void removeSnippet (int sceneIndex, int snippetIndex);
    state::ApplyResult recallSnippet (int sceneIndex, int snippetIndex);

    //==============================================================================
    bool isFading() const noexcept { return ! fades.empty(); }
    void cancelFade();

    //==============================================================================
    // Files
    std::unique_ptr<juce::XmlElement> toXml() const;

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
    void startFade (std::vector<state::FadeTarget>, double seconds);
    void changed();

    MixerEngine& engine;
    PluginHost& host;

    std::vector<std::unique_ptr<Scene>> scenes;
    int currentScene = -1;

    struct Fade
    {
        std::atomic<float>* param;
        float start, target;
    };

    std::vector<Fade> fades;
    double fadeStartMs = 0.0, fadeDurationMs = 0.0;

    JUCE_DECLARE_NON_COPYABLE (Show)
};

} // namespace livedaw
