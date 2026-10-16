#include "engine/Show.h"

namespace livedaw
{

namespace
{
    ld::Show* createShow (Show* owner, ld::CaptureInsertFn capture, ld::RestoreInsertFn restore)
    {
        const ld::PluginBridge bridge { owner, capture, restore };
        auto* s = ld::ld_show_create (&bridge);
        if (s == nullptr)
            std::abort();
        return s;
    }

    void restorePluginState (juce::AudioPluginInstance& plugin, const juce::String& base64)
    {
        if (base64.isEmpty())
            return;

        juce::MemoryBlock block;
        if (block.fromBase64Encoding (base64))
            plugin.setStateInformation (block.getData(), static_cast<int> (block.getSize()));
    }
} // namespace

Show::Show (MixerEngine& e, PluginHost& h)
    : engine (e),
      host (h),
      handle (createShow (this, &Show::captureInsert, &Show::restoreInsert)),
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

bool Show::captureInsert (void* ctx, const ld::Strip* strip, int32_t slot, ld::LdSink* description, ld::LdSink* state)
{
    auto& self = *static_cast<Show*> (ctx);

    try
    {
        const PluginChain chain (self.engine.getHandle(), const_cast<ld::Strip*> (strip));
        auto* plugin = chain.getPlugin (slot);
        if (plugin == nullptr)
            return false;

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

int32_t Show::restoreInsert (void* ctx, const ld::Strip* strip, int32_t slot, int32_t mode,
                             const char* descriptionUtf8, size_t descriptionLen,
                             const char* stateUtf8, size_t stateLen, ld::LdSink* error)
{
    auto& self = *static_cast<Show*> (ctx);

    try
    {
        juce::PluginDescription desc;
        const auto descXml = juce::parseXML (juce::String::fromUTF8 (descriptionUtf8, (int) descriptionLen));
        if (descXml == nullptr || ! desc.loadFromXml (*descXml))
        {
            appendTo (error, "invalid plugin description");
            return 2;
        }

        const auto state = juce::String::fromUTF8 (stateUtf8, (int) stateLen);
        PluginChain chain (self.engine.getHandle(), const_cast<ld::Strip*> (strip));

        if (mode == 0) // recall: only into the same plugin already loaded
        {
            auto* current = chain.getPlugin (slot);
            if (current == nullptr
                || current->getPluginDescription().createIdentifierString() != desc.createIdentifierString())
                return 1;

            restorePluginState (*current, state);
            return 0;
        }

        // Show load: create the plugin.
        juce::String message;
        auto instance = self.host.createInstance (desc, self.engine.getSampleRate(), self.engine.getBlockSize(), message);
        if (instance == nullptr)
        {
            appendTo (error, message.isNotEmpty() ? message : juce::String ("could not be created"));
            return 2;
        }

        restorePluginState (*instance, state);
        chain.setPlugin (slot, std::move (instance));
        return 0;
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
    return s;
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
    return s;
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
    ld::ld_show_cancel_fade (handle);
    stopTimer();
}

void Show::timerCallback()
{
    if (! ld::ld_show_tick (handle))
        stopTimer();
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

void Show::clear (int numChannels)
{
    stopTimer();
    ld::ld_show_clear (handle, engine.getHandle(), numChannels);
    engine.syncStrips();
    changed();
}

} // namespace livedaw
