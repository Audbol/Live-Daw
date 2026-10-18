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

      Show     - the whole file: console structure (channels, buses, FX slot
                 plugins, patching), DCAs, mute groups, fader banks and every scene.
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
        int tag = 0;           // EDIT TAG #1..#128 (0 = none)
        bool skip = false;     // SKIP: GO PREV/NEXT and recalls pass over it
        bool linkNext = false; // LINK: recalling it also recalls the scene below
    };

    static constexpr int maxSceneTag = 128;

    /** A copy of a snippet's settings. */
    struct Snippet
    {
        juce::String name;
        juce::uint32 sections = 0;
        std::vector<state::StripRef> strips;
        bool fireWithScene = false;
        /** Its whole scope (strips, sections, DCAs, mute groups, FX, console). */
        state::Scope scope = state::Scope::none();
        /** The parameters REC recorded, else the values it holds. */
        int parameterCount = 0;
    };

    Show (MixerEngine&, PluginHost&);
    ~Show() override;

    juce::String getName() const { return takeString (ld::ld_show_name (handle)); }
    void setName (const juce::String&);

    /** Fader banks of the three sections (default + user banks), saved with the show. */
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
    /** 1..128; anything else clears the tag. */
    void setSceneTag (int index, int tag);
    void setSceneSkip (int index, bool skip);
    /** The link follows the scene: after a move it links to its new neighbour below. */
    void setSceneLink (int index, bool linkNext);
    /** The first scene tagged `tag` that isn't skipped, or -1. */
    int findSceneByTag (int tag) const noexcept { return ld::ld_show_find_scene_by_tag (handle, tag); }

    /** Captures the current console as a new scene (appended, or inserted at `index`). */
    int addScene (const juce::String& sceneName, int index = -1);
    void storeScene (int index);
    void removeScene (int index);
    void moveScene (int from, int to);

    /** Recalls a scene (and its auto-fire snippets), then the scenes it links to.
        A skipped scene is not recalled. The current scene becomes the last one recalled. */
    state::ApplyResult recallScene (int index);
    /** GO NEXT / GO PREV: the next / previous scene that isn't skipped. */
    state::ApplyResult recallNext();
    state::ApplyResult recallPrevious();
    /** Recalls the first non-skipped scene tagged `tag` (nothing if none). */
    state::ApplyResult recallSceneByTag (int tag);

    //==============================================================================
    // Scopes beyond sections, GLOBAL safes, PARTIAL LOAD / UPDATE, RESTORE

    /** A scene's whole recall scope (its sections are Scene::scope). */
    state::Scope getSceneRecallScope (int index) const;
    void setSceneRecallScope (int index, const state::Scope&);
    /** PARTIAL UPDATE: stores only `scope` into the scene. */
    void storeScenePartial (int index, const state::Scope&);
    /** PARTIAL LOAD: recalls only `scope` of the scene (noGlobal: NO GLOB, the
        safes are ignored). No links, auto-fire snippets or fade. */
    state::ApplyResult recallScenePartial (int index, const state::Scope&, bool noGlobal);
    /** RESTORE: the whole scene regardless of its scope, safes included. */
    state::ApplyResult restoreScene (int index);
    /** The scope PARTIAL LOAD / UPDATE last used (they keep it). */
    state::Scope getPartialScope() const;
    /** GLOBAL safes beyond the strips' recall safe (strips are not used). */
    state::Scope getGlobalSafe() const;
    void setGlobalSafe (const state::Scope&);
    bool hasGlobalSafes() const;

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
    /** A snippet of what `scope` covers (strips, sections, DCAs, mute groups, FX, console). */
    int addSnippet (int sceneIndex, const juce::String& snippetName, const state::Scope&);
    /** ADD ITEMS / REMOVE ITEMS: no strips in `items` means every strip of the snippet. */
    void addSnippetItems (int sceneIndex, int snippetIndex, const state::Scope& items);
    void removeSnippetItems (int sceneIndex, int snippetIndex, const state::Scope& items);
    /** PARTIAL LOAD of a snippet: only what is in both its scope and `limit`. */
    state::ApplyResult recallSnippetPartial (int sceneIndex, int snippetIndex, const state::Scope& limit);

    // Snippet REC / STOP: the parameters changed between REC and STOP.
    void startSnippetRecording();
    /** Returns the number of changed parameters. */
    int stopSnippetRecording();
    void clearSnippetRecording();
    bool isRecordingSnippet() const;
    /** STOPped with changes that SAVE hasn't stored yet. */
    bool hasRecordedSnippet() const;
    /** The parameters changed so far (recording) or by the last STOP. */
    int getRecordedParameterCount() const;
    /** SAVE after STOP: a snippet of the recorded parameters within `focus` (REC
        FOCUS; nullptr: all). -1 if nothing was recorded. */
    int addRecordedSnippet (int sceneIndex, const juce::String& snippetName, const state::Scope* focus);

    //==============================================================================
    // Custom controls (CC EDIT): the assignments as a <CustomControls> element,
    // saved with the show (the UI interprets the functions).
    juce::String getCustomControlsXml() const;
    bool setCustomControlsXml (const juce::String&);

    //==============================================================================
    // LIBRARY presets: an FX slot's device (FX tab) or a strip's settings (CHAN tab) as files.
    struct PresetInfo
    {
        enum class Kind
        {
            None,
            Fx,
            Channel
        };
        Kind kind = Kind::None;
        juce::String name, detail; // detail: the FX device, or the strip kind
        juce::uint32 scope = 0;    // a channel preset's sections
        std::optional<StripKind> stripKind;
    };

    static constexpr const char* fxPresetExtension = ".lfx";
    static constexpr const char* channelPresetExtension = ".lchn";

    static PresetInfo readPreset (const juce::File&);
    juce::Result saveFxPreset (int slot, const juce::File&, const juce::String& presetName);
    /** Fails with the warnings when part of it didn't load (a plugin, a premium device in 9-16). */
    juce::Result loadFxPreset (int slot, const juce::File&);
    juce::Result saveChannelPreset (const state::StripRef&, juce::uint32 scope, const juce::File&, const juce::String& presetName);
    /** With its own scope, or `scope`. */
    juce::Result loadChannelPreset (const state::StripRef&, const juce::File&, std::optional<juce::uint32> scope = std::nullopt);
    /** UPDATE SCOPE. */
    static bool setPresetScope (const juce::File&, juce::uint32 scope);

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

    /** Adds the snapshot scenes of a WING .show file (see WingRemote::importShow);
        `report` gets what was skipped. The console only grows to 40 channels. */
    juce::Result importWingShow (ld::LdWing*, const juce::File&, juce::String& report);

    /** Clears scenes and resets the console to `numChannels` channels (the WING has 40). */
    void clear (int numChannels);

    /** Called after any change to the scene list or current scene. */
    std::function<void()> onChanged;

    /** Called before a show load empties FX slot `slot` (close its plugin editor here). */
    std::function<void (int slot)> onFxCleared;

    static constexpr const char* fileExtension = ".lshow";

private:
    void timerCallback() override;
    void changed();
    state::ApplyResult afterRecall (ld::ApplyResultC&);
    juce::Result afterLoad (bool ok, ld::LdString* message);

    static bool captureFx (void* ctx, int32_t slot, ld::LdSink* description, ld::LdSink* state);
    static int32_t restoreFx (void* ctx, int32_t slot, int32_t mode, const char* description, size_t descriptionLen,
                              const char* state, size_t stateLen, ld::LdSink* error);
    static void clearFx (void* ctx, int32_t slot);

    JUCE_DECLARE_NON_COPYABLE (Show)
};

} // namespace livedaw
