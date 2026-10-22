#include "engine/Show.h"

namespace livedaw
{

namespace
{
    ld::Show* createShow (Show* owner, ld::CaptureFxFn capture, ld::RestoreFxFn restore, ld::ClearFxFn clear)
    {
        const ld::PluginBridge bridge { owner, capture, restore, clear };
        auto* s = ld::ld_show_create (&bridge);
        if (s == nullptr)
            std::abort();
        return s;
    }

    /** A saved FX slot's plugins: a <PLUGINCHAIN>, or a <PLUGIN> with its state (a plugin on its own). */
    bool parseSavedFx (const juce::String& description, const juce::String& state, std::vector<PluginChain::Saved>& saved)
    {
        const auto xml = juce::parseXML (description);
        if (xml == nullptr)
            return false;
        if (xml->hasTagName (PluginChain::kXmlTag))
            return PluginChain::parseXml (*xml, saved);

        PluginChain::Saved single;
        if (! single.description.loadFromXml (*xml))
            return false;
        if (state.isNotEmpty())
            single.state.fromBase64Encoding (state);
        saved = { std::move (single) };
        return true;
    }

    void restoreState (juce::AudioPluginInstance& plugin, const juce::MemoryBlock& state)
    {
        if (state.getSize() > 0)
            plugin.setStateInformation (state.getData(), static_cast<int> (state.getSize()));
    }
} // namespace

Show::Show (MixerEngine& e, PluginHost& h)
    : engine (e),
      host (h),
      handle (createShow (this, &Show::captureFx, &Show::restoreFx, &Show::clearFx)),
      layers (ld::ld_show_layers (handle))
{
}

Show::~Show()
{
    stopTimer();
    ld::ld_show_destroy (handle);
}

void Show::changed()
{
    if (onChanged)
        onChanged();
}

void Show::setName (const juce::String& newName)
{
    const Utf8 u (newName);
    ld::ld_show_set_name (handle, u.data, u.size);
}

//==============================================================================
// Plugin bridge: the Rust session asks for plugin state and plugin creation here.

bool Show::captureFx (void* ctx, int32_t slot, ld::LdSink* description, ld::LdSink* state)
{
    auto& self = *static_cast<Show*> (ctx);

    try
    {
        auto& fx = self.engine.getFx();
        auto* plugin = fx.getPlugin (slot);
        if (plugin == nullptr)
            return false;

        // A chain is saved whole; a chain of one (not bypassed) like a plugin on
        // its own, so such shows open in earlier versions too.
        if (auto* chain = fx.getChain (slot); chain != nullptr && (chain->size() > 1 || chain->isBypassed (0)))
        {
            appendTo (description, chain->createXml()->toString (juce::XmlElement::TextFormat().singleLine().withoutHeader()));
            return true;
        }
        if (fx.getChain (slot) != nullptr)
            plugin = fx.getChainMember (slot, 0);

        const auto desc = plugin->getPluginDescription().createXml();
        appendTo (description, desc->toString (juce::XmlElement::TextFormat().singleLine().withoutHeader()));

        juce::MemoryBlock data;
        plugin->getStateInformation (data);
        appendTo (state, data.toBase64Encoding());
        return true;
    }
    catch (...)
    {
        return false; // a plugin that throws from getStateInformation is saved without state
    }
}

int32_t Show::restoreFx (void* ctx, int32_t slot, int32_t mode, const char* descriptionUtf8, size_t descriptionLen,
                         const char* stateUtf8, size_t stateLen, ld::LdSink* error)
{
    auto& self = *static_cast<Show*> (ctx);

    try
    {
        std::vector<PluginChain::Saved> saved;
        if (! parseSavedFx (juce::String::fromUTF8 (descriptionUtf8, (int) descriptionLen),
                            juce::String::fromUTF8 (stateUtf8, (int) stateLen), saved))
        {
            appendTo (error, "invalid plugin description");
            return 2;
        }

        auto& fx = self.engine.getFx();

        if (mode == 0) // recall: only into the same plugins already loaded, in the same order
        {
            const int n = fx.getNumChainMembers (slot);
            if (n != (int) saved.size())
                return 1;
            for (int i = 0; i < n; ++i)
                if (fx.getChainMember (slot, i)->getPluginDescription().createIdentifierString()
                    != saved[(size_t) i].description.createIdentifierString())
                    return 1;

            for (int i = 0; i < n; ++i)
            {
                restoreState (*fx.getChainMember (slot, i), saved[(size_t) i].state);
                if (saved[(size_t) i].bypassed != fx.isChainMemberBypassed (slot, i))
                    fx.setChainMemberBypassed (slot, i, saved[(size_t) i].bypassed);
            }
            return 0;
        }

        // Show load: create the plugins (each recorded for the crash blacklist,
        // or in the sandbox). One that can't be created is left out and reported.
        std::vector<std::pair<std::unique_ptr<juce::AudioPluginInstance>, bool>> created; // with bypass
        juce::StringArray failures;
        for (const auto& s : saved)
        {
            juce::String message;
            auto instance = self.host.createInstance (s.description, self.engine.getSampleRate(), self.engine.getBlockSize(), message);
            if (instance == nullptr)
            {
                failures.add ((saved.size() > 1 ? s.description.name + ": " : juce::String())
                              + (message.isNotEmpty() ? message : juce::String ("could not be created")));
                continue;
            }
            restoreState (*instance, s.state);
            created.emplace_back (std::move (instance), s.bypassed);
        }

        if (created.size() == 1 && ! created.front().second)
        {
            fx.setPlugin (slot, std::move (created.front().first)); // a plugin on its own
        }
        else if (! created.empty())
        {
            std::vector<PluginChain::MemberPtr> members;
            for (auto& [instance, bypassed] : created)
                members.push_back (PluginChain::makeMember (std::move (instance), bypassed));
            fx.setPlugin (slot, std::make_unique<PluginChain> (std::move (members)));
        }

        if (failures.isEmpty())
            return 0;
        appendTo (error, failures.joinIntoString ("; "));
        return 2;
    }
    catch (const std::exception& e)
    {
        appendTo (error, e.what());
    }
    catch (...)
    {
        appendTo (error, "the plugin threw an exception");
    }
    return 2;
}

void Show::clearFx (void* ctx, int32_t slot)
{
    auto& self = *static_cast<Show*> (ctx);
    if (self.onFxCleared)
        self.onFxCleared (slot);
    self.engine.getFx().setPlugin (slot, nullptr);
}

//==============================================================================
const std::optional<Show::Scene> Show::getScene (int index) const
{
    if (! ld::ld_scene_exists (handle, index))
        return std::nullopt;

    Scene s;
    s.name = takeString (ld::ld_scene_name (handle, index));
    s.notes = takeString (ld::ld_scene_notes (handle, index));
    s.scope = ld::ld_scene_scope (handle, index);
    s.fadeSeconds = ld::ld_scene_fade_seconds (handle, index);
    s.numSnippets = ld::ld_scene_num_snippets (handle, index);
    s.tag = ld::ld_scene_tag (handle, index);
    s.skip = ld::ld_scene_skip (handle, index);
    s.linkNext = ld::ld_scene_link (handle, index);
    s.midi = takeString (ld::ld_scene_midi (handle, index));
    return s;
}

void Show::setSceneTag (int index, int tag)
{
    ld::ld_scene_set_tag (handle, index, tag);
}

void Show::setSceneSkip (int index, bool skip)
{
    ld::ld_scene_set_skip (handle, index, skip);
}

void Show::setSceneLink (int index, bool linkNext)
{
    ld::ld_scene_set_link (handle, index, linkNext);
}

void Show::setSceneMidi (int index, const juce::String& text)
{
    const Utf8 u (text);
    ld::ld_scene_set_midi (handle, index, u.data, u.size);
}

void Show::setSceneName (int index, const juce::String& text)
{
    const Utf8 u (text);
    ld::ld_scene_set_name (handle, index, u.data, u.size);
}

void Show::setSceneNotes (int index, const juce::String& text)
{
    const Utf8 u (text);
    ld::ld_scene_set_notes (handle, index, u.data, u.size);
}

void Show::setSceneScope (int index, juce::uint32 scope)
{
    ld::ld_scene_set_scope (handle, index, scope);
}

void Show::setSceneFadeSeconds (int index, double seconds)
{
    ld::ld_scene_set_fade_seconds (handle, index, seconds);
}

int Show::addScene (const juce::String& sceneName, int index)
{
    const Utf8 u (sceneName);
    const int result = ld::ld_show_add_scene (handle, engine.getHandle(), u.data, u.size, index);
    changed();
    return result;
}

void Show::storeScene (int index)
{
    ld::ld_show_store_scene (handle, engine.getHandle(), index);
    changed();
}

void Show::removeScene (int index)
{
    ld::ld_show_remove_scene (handle, index);
    changed();
}

void Show::moveScene (int from, int to)
{
    ld::ld_show_move_scene (handle, from, to);
    changed();
}

state::ApplyResult Show::afterRecall (ld::ApplyResultC& r)
{
    auto result = state::ApplyResult::fromC (r);

    // SEND MIDI of the scenes recalled.
    if (const auto midi = takeString (ld::ld_show_take_midi (handle)); midi.isNotEmpty() && onSceneMidi)
    {
        juce::StringArray lines;
        lines.addLines (midi);
        onSceneMidi (lines);
    }

    if (isFading())
        startTimerHz (60);
    else
        stopTimer();

    changed();
    return result;
}

state::ApplyResult Show::recallScene (int index)
{
    ld::ApplyResultC r {};
    ld::ld_show_recall_scene (handle, engine.getHandle(), 0, index, &r);
    return afterRecall (r);
}

state::ApplyResult Show::recallNext()
{
    ld::ApplyResultC r {};
    ld::ld_show_recall_scene (handle, engine.getHandle(), 1, 0, &r);
    return afterRecall (r);
}

state::ApplyResult Show::recallPrevious()
{
    ld::ApplyResultC r {};
    ld::ld_show_recall_scene (handle, engine.getHandle(), 2, 0, &r);
    return afterRecall (r);
}

state::ApplyResult Show::recallSceneByTag (int tag)
{
    ld::ApplyResultC r {};
    ld::ld_show_recall_scene (handle, engine.getHandle(), 3, tag, &r);
    return afterRecall (r);
}

//==============================================================================
namespace
{
    /** Reads a scope through `get (out, strips, capacity) -> count`. */
    template <typename Getter>
    state::Scope readScope (Getter&& get)
    {
        ld::ScopeC c {};
        std::vector<ld::StripRefC> refs ((size_t) juce::jmax (0, get (&c, nullptr, 0)));
        get (&c, refs.data(), (int32_t) refs.size());
        return state::Scope::fromC (c, refs);
    }
} // namespace

state::Scope Show::getSceneRecallScope (int index) const
{
    return readScope ([&] (ld::ScopeC* out, ld::StripRefC* strips, int32_t cap) { return ld::ld_scene_get_scope (handle, index, out, strips, cap); });
}

void Show::setSceneRecallScope (int index, const state::Scope& scope)
{
    const auto refs = state::toC (scope.strips);
    const auto c = scope.toC (refs);
    ld::ld_scene_set_recall_scope (handle, index, &c);
}

void Show::storeScenePartial (int index, const state::Scope& scope)
{
    const auto refs = state::toC (scope.strips);
    const auto c = scope.toC (refs);
    ld::ld_show_store_scene_partial (handle, engine.getHandle(), index, &c);
    changed();
}

state::ApplyResult Show::recallScenePartial (int index, const state::Scope& scope, bool noGlobal)
{
    const auto refs = state::toC (scope.strips);
    const auto c = scope.toC (refs);
    ld::ApplyResultC r {};
    ld::ld_show_recall_scene_with (handle, engine.getHandle(), index, &c, noGlobal, &r);
    return afterRecall (r);
}

state::ApplyResult Show::restoreScene (int index)
{
    ld::ApplyResultC r {};
    ld::ld_show_recall_scene_with (handle, engine.getHandle(), index, nullptr, true, &r);
    return afterRecall (r);
}

state::Scope Show::getPartialScope() const
{
    return readScope ([&] (ld::ScopeC* out, ld::StripRefC* strips, int32_t cap) { return ld::ld_show_partial_scope (handle, out, strips, cap); });
}

state::Scope Show::getGlobalSafe() const
{
    ld::ScopeC c {};
    ld::ld_show_global_safe (handle, &c);
    return state::Scope::fromC (c, {});
}

void Show::setGlobalSafe (const state::Scope& safe)
{
    const std::vector<ld::StripRefC> none;
    const auto c = safe.toC (none);
    ld::ld_show_set_global_safe (handle, &c);
}

bool Show::hasGlobalSafes() const
{
    const auto g = getGlobalSafe();
    return g.sections != 0 || g.dcas != 0 || g.muteGroups != 0 || g.fx != 0 || g.console;
}

//==============================================================================
const std::optional<Show::Snippet> Show::getSnippet (int sceneIndex, int snippetIndex) const
{
    if (! ld::ld_snippet_exists (handle, sceneIndex, snippetIndex))
        return std::nullopt;

    Snippet s;
    s.name = takeString (ld::ld_snippet_name (handle, sceneIndex, snippetIndex));
    s.sections = ld::ld_snippet_sections (handle, sceneIndex, snippetIndex);
    s.fireWithScene = ld::ld_snippet_fire_with_scene (handle, sceneIndex, snippetIndex);

    std::vector<ld::StripRefC> refs ((size_t) ld::ld_snippet_strips (handle, sceneIndex, snippetIndex, nullptr, 0));
    ld::ld_snippet_strips (handle, sceneIndex, snippetIndex, refs.data(), (int32_t) refs.size());
    for (auto r : refs)
        s.strips.push_back (state::StripRef::fromC (r));
    s.scope = readScope ([&] (ld::ScopeC* out, ld::StripRefC* strips, int32_t cap)
                         { return ld::ld_snippet_get_scope (handle, sceneIndex, snippetIndex, out, strips, cap); });
    s.parameterCount = ld::ld_snippet_parameter_count (handle, sceneIndex, snippetIndex);
    return s;
}

int Show::addSnippet (int sceneIndex, const juce::String& snippetName, const state::Scope& scope)
{
    const Utf8 u (snippetName);
    const auto refs = state::toC (scope.strips);
    const auto c = scope.toC (refs);
    const int result = ld::ld_show_add_snippet_scoped (handle, engine.getHandle(), sceneIndex, u.data, u.size, &c);
    changed();
    return result;
}

void Show::addSnippetItems (int sceneIndex, int snippetIndex, const state::Scope& items)
{
    const auto refs = state::toC (items.strips);
    const auto c = items.toC (refs);
    ld::ld_show_snippet_items (handle, engine.getHandle(), sceneIndex, snippetIndex, true, &c);
    changed();
}

void Show::removeSnippetItems (int sceneIndex, int snippetIndex, const state::Scope& items)
{
    const auto refs = state::toC (items.strips);
    const auto c = items.toC (refs);
    ld::ld_show_snippet_items (handle, engine.getHandle(), sceneIndex, snippetIndex, false, &c);
    changed();
}

state::ApplyResult Show::recallSnippetPartial (int sceneIndex, int snippetIndex, const state::Scope& limit)
{
    const auto refs = state::toC (limit.strips);
    const auto c = limit.toC (refs);
    ld::ApplyResultC r {};
    ld::ld_show_recall_snippet_with (handle, engine.getHandle(), sceneIndex, snippetIndex, &c, &r);
    return state::ApplyResult::fromC (r);
}

void Show::startSnippetRecording()
{
    ld::ld_show_snippet_rec (handle, engine.getHandle(), 0);
}

int Show::stopSnippetRecording()
{
    return ld::ld_show_snippet_rec (handle, engine.getHandle(), 1);
}

void Show::clearSnippetRecording()
{
    ld::ld_show_snippet_rec (handle, engine.getHandle(), 2);
}

int Show::getRecordedParameterCount() const
{
    return ld::ld_show_snippet_rec (handle, engine.getHandle(), 3);
}

bool Show::isRecordingSnippet() const
{
    return ld::ld_show_snippet_rec (handle, engine.getHandle(), 4) != 0;
}

bool Show::hasRecordedSnippet() const
{
    return ld::ld_show_snippet_rec (handle, engine.getHandle(), 5) != 0;
}

int Show::addRecordedSnippet (int sceneIndex, const juce::String& snippetName, const state::Scope* focus)
{
    const Utf8 u (snippetName);
    std::vector<ld::StripRefC> refs;
    ld::ScopeC c {};
    if (focus != nullptr)
    {
        refs = state::toC (focus->strips);
        c = focus->toC (refs);
    }
    const int result = ld::ld_show_add_recorded_snippet (handle, engine.getHandle(), sceneIndex, u.data, u.size, focus != nullptr ? &c : nullptr);
    changed();
    return result;
}

int Show::addSnippet (int sceneIndex, const juce::String& snippetName, juce::uint32 sections,
                      const std::vector<state::StripRef>& strips)
{
    const Utf8 u (snippetName);
    const auto refs = state::toC (strips);
    const int result = ld::ld_show_add_snippet (handle, engine.getHandle(), sceneIndex, u.data, u.size, sections,
                                                refs.data(), (int32_t) refs.size());
    changed();
    return result;
}

void Show::storeSnippet (int sceneIndex, int snippetIndex)
{
    ld::ld_show_store_snippet (handle, engine.getHandle(), sceneIndex, snippetIndex);
    changed();
}

void Show::removeSnippet (int sceneIndex, int snippetIndex)
{
    ld::ld_show_remove_snippet (handle, sceneIndex, snippetIndex);
    changed();
}

state::ApplyResult Show::recallSnippet (int sceneIndex, int snippetIndex)
{
    ld::ApplyResultC r {};
    ld::ld_show_recall_snippet (handle, engine.getHandle(), sceneIndex, snippetIndex, &r);
    return state::ApplyResult::fromC (r);
}

void Show::setSnippetName (int sceneIndex, int snippetIndex, const juce::String& text)
{
    const Utf8 u (text);
    ld::ld_snippet_set_name (handle, sceneIndex, snippetIndex, u.data, u.size);
}

void Show::setSnippetSections (int sceneIndex, int snippetIndex, juce::uint32 sections)
{
    ld::ld_snippet_set_sections (handle, sceneIndex, snippetIndex, sections);
}

void Show::setSnippetStrips (int sceneIndex, int snippetIndex, const std::vector<state::StripRef>& strips)
{
    const auto refs = state::toC (strips);
    ld::ld_snippet_set_strips (handle, sceneIndex, snippetIndex, refs.data(), (int32_t) refs.size());
}

void Show::setSnippetFireWithScene (int sceneIndex, int snippetIndex, bool fire)
{
    ld::ld_snippet_set_fire_with_scene (handle, sceneIndex, snippetIndex, fire);
}

//==============================================================================
void Show::cancelFade()
{
    ld::ld_show_cancel_fade (handle, engine.getHandle());
    stopTimer();
}

void Show::timerCallback()
{
    if (! ld::ld_show_tick (handle, engine.getHandle()))
        stopTimer();
}

//==============================================================================
juce::String Show::getCustomControlsXml() const
{
    return takeString (ld::ld_show_custom_controls (handle));
}

bool Show::setCustomControlsXml (const juce::String& xml)
{
    const Utf8 u (xml);
    return ld::ld_show_set_custom_controls (handle, u.data, u.size);
}

//==============================================================================
Show::PresetInfo Show::readPreset (const juce::File& file)
{
    const Utf8 u (file.getFullPathName());
    ld::PresetInfoC c {};
    PresetInfo info;
    if (! ld::ld_preset_info (u.data, u.size, &c))
        return info;
    info.kind = c.kind == 1 ? PresetInfo::Kind::Fx : PresetInfo::Kind::Channel;
    info.name = takeString (c.name);
    info.detail = takeString (c.detail);
    info.scope = c.scope;
    if (juce::isPositiveAndBelow (c.stripKind, kNumStripKinds))
        info.stripKind = static_cast<StripKind> (c.stripKind);
    return info;
}

namespace
{
    juce::Result presetResult (bool ok, ld::LdString* message, const juce::String& what)
    {
        const auto text = takeString (message);
        if (! ok)
            return juce::Result::fail (text.isNotEmpty() ? text : what + " failed");
        return text.isEmpty() ? juce::Result::ok() : juce::Result::fail (text);
    }
} // namespace

juce::Result Show::saveFxPreset (int slot, const juce::File& file, const juce::String& presetName)
{
    const Utf8 p (file.getFullPathName()), n (presetName);
    ld::LdString* message = nullptr;
    const bool ok = ld::ld_show_save_fx_preset (handle, engine.getHandle(), slot, p.data, p.size, n.data, n.size, &message);
    return presetResult (ok, message, "Saving the FX preset");
}

juce::Result Show::loadFxPreset (int slot, const juce::File& file)
{
    const Utf8 p (file.getFullPathName());
    ld::LdString* message = nullptr;
    const bool ok = ld::ld_show_load_fx_preset (handle, engine.getHandle(), slot, p.data, p.size, &message);
    return presetResult (ok, message, "Loading the FX preset");
}

juce::Result Show::saveChannelPreset (const state::StripRef& strip, juce::uint32 scope, const juce::File& file, const juce::String& presetName)
{
    const Utf8 p (file.getFullPathName()), n (presetName);
    ld::LdString* message = nullptr;
    const bool ok = ld::ld_show_save_channel_preset (handle, engine.getHandle(), strip.toC(), scope, p.data, p.size, n.data, n.size, &message);
    return presetResult (ok, message, "Saving the channel preset");
}

juce::Result Show::loadChannelPreset (const state::StripRef& strip, const juce::File& file, std::optional<juce::uint32> scope)
{
    const Utf8 p (file.getFullPathName());
    ld::LdString* message = nullptr;
    const bool ok = ld::ld_show_load_channel_preset (handle, engine.getHandle(), strip.toC(), p.data, p.size, scope.has_value(), scope.value_or (0),
                                                     &message);
    return presetResult (ok, message, "Loading the channel preset");
}

bool Show::setPresetScope (const juce::File& file, juce::uint32 scope)
{
    const Utf8 p (file.getFullPathName());
    return ld::ld_preset_set_scope (p.data, p.size, scope);
}

//==============================================================================
Show::LibraryFileInfo Show::readLibraryFile (const juce::File& file)
{
    const Utf8 u (file.getFullPathName());
    ld::LibraryInfoC c {};
    LibraryFileInfo info;
    if (! ld::ld_library_info (u.data, u.size, &c))
        return info;
    info.kind = c.kind == 1 ? LibraryFileInfo::Kind::Scene : LibraryFileInfo::Kind::Snippet;
    info.name = takeString (c.name);
    info.count = c.count;
    return info;
}

juce::Result Show::exportScene (int index, const juce::File& file) const
{
    const Utf8 p (file.getFullPathName());
    ld::LdString* message = nullptr;
    const bool ok = ld::ld_show_export_library (handle, index, -1, p.data, p.size, &message);
    return presetResult (ok, message, "Saving the scene");
}

juce::Result Show::exportSnippet (int sceneIndex, int snippetIndex, const juce::File& file) const
{
    if (snippetIndex < 0)
        return juce::Result::fail ("No such snippet");
    const Utf8 p (file.getFullPathName());
    ld::LdString* message = nullptr;
    const bool ok = ld::ld_show_export_library (handle, sceneIndex, snippetIndex, p.data, p.size, &message);
    return presetResult (ok, message, "Saving the snippet");
}

int Show::importScene (const juce::File& file, int index, juce::String& error)
{
    if (readLibraryFile (file).kind != LibraryFileInfo::Kind::Scene)
    {
        error = "Not a Sidecar scene file";
        return -1;
    }
    const Utf8 p (file.getFullPathName());
    ld::LdString* message = nullptr;
    const int result = ld::ld_show_import_library (handle, p.data, p.size, index, &message);
    error = takeString (message);
    changed();
    return result;
}

int Show::importSnippet (const juce::File& file, int sceneIndex, juce::String& error)
{
    if (readLibraryFile (file).kind != LibraryFileInfo::Kind::Snippet)
    {
        error = "Not a Sidecar snippet file";
        return -1;
    }
    const Utf8 p (file.getFullPathName());
    ld::LdString* message = nullptr;
    const int result = ld::ld_show_import_library (handle, p.data, p.size, sceneIndex, &message);
    error = takeString (message);
    changed();
    return result;
}

//==============================================================================
juce::String Show::toXmlString() const
{
    return takeString (ld::ld_show_to_xml (handle, engine.getHandle()));
}

std::unique_ptr<juce::XmlElement> Show::toXml() const
{
    return juce::parseXML (toXmlString());
}

juce::Result Show::afterLoad (bool ok, ld::LdString* message)
{
    const auto text = takeString (message);
    stopTimer();
    engine.syncStrips();
    changed();

    if (! ok)
        return juce::Result::fail (text);
    if (text.isNotEmpty())
        return juce::Result::fail ("Some plugins could not be loaded:\n" + text);
    return juce::Result::ok();
}

juce::Result Show::loadFromXml (const juce::XmlElement& xml, bool loadPlugins)
{
    const auto text = xml.toString();
    const Utf8 u (text);
    ld::LdString* message = nullptr;
    const bool ok = ld::ld_show_load_xml (handle, engine.getHandle(), u.data, u.size, loadPlugins, &message);
    return afterLoad (ok, message);
}

juce::Result Show::saveToFile (const juce::File& file) const
{
    const auto path = file.getFullPathName();
    const Utf8 u (path);
    ld::LdString* error = nullptr;

    if (ld::ld_show_save_file (handle, engine.getHandle(), u.data, u.size, &error))
        return juce::Result::ok();

    return juce::Result::fail (takeString (error));
}

juce::Result Show::loadFromFile (const juce::File& file, bool loadPlugins)
{
    const auto path = file.getFullPathName();
    const Utf8 u (path);
    ld::LdString* message = nullptr;
    const bool ok = ld::ld_show_load_file (handle, engine.getHandle(), u.data, u.size, loadPlugins, &message);
    return afterLoad (ok, message);
}

juce::Result Show::importWingShow (ld::LdWing* wing, const juce::File& file, juce::String& report)
{
    const Utf8 u (file.getFullPathName());
    ld::LdString* message = nullptr;
    const bool ok = ld::ld_wing_import_show (wing, handle, u.data, u.size, &message);
    report = takeString (message);
    engine.syncStrips();
    changed();
    return ok ? juce::Result::ok() : juce::Result::fail (report);
}

juce::uint32 Show::syncWingSurface (ld::LdWing* wing, int ccBank, int& requestedBank)
{
    int32_t bank = -1;
    const auto changes = ld::ld_wing_sync_surface (wing, handle, ccBank, &bank);
    requestedBank = bank;
    return changes;
}

void Show::clear (int numChannels)
{
    stopTimer();
    ld::ld_show_clear (handle, engine.getHandle(), numChannels);
    engine.syncStrips();
    changed();
}

} // namespace livedaw
