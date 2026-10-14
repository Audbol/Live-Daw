#include "engine/Show.h"

namespace livedaw
{

namespace
{
    constexpr int kShowVersion = 2;

    juce::String refsToString (const std::vector<state::StripRef>& refs)
    {
        juce::StringArray parts;
        for (auto& r : refs)
            parts.add (r.toString());
        return parts.joinIntoString (",");
    }

    std::vector<state::StripRef> refsFromString (const juce::String& s)
    {
        std::vector<state::StripRef> refs;
        for (auto& part : juce::StringArray::fromTokens (s, ",", ""))
            if (part.isNotEmpty())
                refs.push_back (state::StripRef::fromString (part));
        return refs;
    }

    void mergeResult (state::ApplyResult& into, const state::ApplyResult& from)
    {
        into.stripsApplied += from.stripsApplied;
        into.stripsSkippedSafe += from.stripsSkippedSafe;
        into.stripsMissing += from.stripsMissing;
        into.warnings.addArray (from.warnings);
    }
} // namespace

Show::Show (MixerEngine& e, PluginHost& h) : engine (e), host (h) {}

Show::~Show()
{
    stopTimer();
}

void Show::changed()
{
    if (onChanged)
        onChanged();
}

//==============================================================================
Show::Scene* Show::getScene (int index) const noexcept
{
    return juce::isPositiveAndBelow (index, getNumScenes()) ? scenes[(size_t) index].get() : nullptr;
}

int Show::addScene (const juce::String& sceneName, int index)
{
    auto scene = std::make_unique<Scene>();
    scene->name = sceneName;
    scene->state = state::capture (engine, state::AllSections);

    index = juce::isPositiveAndBelow (index, getNumScenes() + 1) ? index : getNumScenes();
    scenes.insert (scenes.begin() + index, std::move (scene));
    currentScene = index;
    changed();
    return index;
}

void Show::storeScene (int index)
{
    if (auto* scene = getScene (index))
    {
        scene->state = state::capture (engine, state::AllSections);
        currentScene = index;
        changed();
    }
}

void Show::removeScene (int index)
{
    if (getScene (index) == nullptr)
        return;

    scenes.erase (scenes.begin() + index);

    if (currentScene == index)
        currentScene = -1;
    else if (currentScene > index)
        --currentScene;

    changed();
}

void Show::moveScene (int from, int to)
{
    if (getScene (from) == nullptr || ! juce::isPositiveAndBelow (to, getNumScenes()) || from == to)
        return;

    auto scene = std::move (scenes[(size_t) from]);
    scenes.erase (scenes.begin() + from);
    scenes.insert (scenes.begin() + to, std::move (scene));

    if (currentScene == from)
        currentScene = to;
    else if (from < currentScene && to >= currentScene)
        --currentScene;
    else if (from > currentScene && to <= currentScene)
        ++currentScene;

    changed();
}

state::ApplyResult Show::recallScene (int index)
{
    state::ApplyResult result;
    auto* scene = getScene (index);

    if (scene == nullptr || scene->state == nullptr)
        return result;

    cancelFade();

    std::vector<state::FadeTarget> targets;
    const bool fade = scene->fadeSeconds > 0.0;
    result = state::apply (engine, *scene->state, scene->scope, nullptr, true, fade ? &targets : nullptr);

    // Auto-fire snippets apply on top of the scene (they ignore recall safes, like
    // a manual snippet). Their continuous values override the scene's fade targets.
    for (auto& snippet : scene->snippets)
    {
        if (! snippet->fireWithScene || snippet->state == nullptr)
            continue;

        mergeResult (result, state::apply (engine, *snippet->state, snippet->sections,
                                           &snippet->strips, false, fade ? &targets : nullptr));
    }

    if (fade)
        startFade (std::move (targets), scene->fadeSeconds);

    currentScene = index;
    changed();
    return result;
}

state::ApplyResult Show::recallNext()
{
    return recallScene (juce::jmin (currentScene + 1, getNumScenes() - 1));
}

state::ApplyResult Show::recallPrevious()
{
    return recallScene (juce::jmax (0, currentScene - 1));
}

//==============================================================================
int Show::addSnippet (int sceneIndex, const juce::String& snippetName, juce::uint32 sections,
                      std::vector<state::StripRef> strips)
{
    auto* scene = getScene (sceneIndex);
    if (scene == nullptr)
        return -1;

    auto snippet = std::make_unique<Snippet>();
    snippet->name = snippetName;
    snippet->sections = sections;
    snippet->strips = std::move (strips);
    snippet->state = state::capture (engine, sections, &snippet->strips);
    scene->snippets.push_back (std::move (snippet));
    changed();
    return static_cast<int> (scene->snippets.size()) - 1;
}

void Show::storeSnippet (int sceneIndex, int snippetIndex)
{
    if (auto* scene = getScene (sceneIndex))
    {
        if (juce::isPositiveAndBelow (snippetIndex, static_cast<int> (scene->snippets.size())))
        {
            auto& snippet = *scene->snippets[(size_t) snippetIndex];
            snippet.state = state::capture (engine, snippet.sections, &snippet.strips);
            changed();
        }
    }
}

void Show::removeSnippet (int sceneIndex, int snippetIndex)
{
    if (auto* scene = getScene (sceneIndex))
    {
        if (juce::isPositiveAndBelow (snippetIndex, static_cast<int> (scene->snippets.size())))
        {
            scene->snippets.erase (scene->snippets.begin() + snippetIndex);
            changed();
        }
    }
}

state::ApplyResult Show::recallSnippet (int sceneIndex, int snippetIndex)
{
    if (auto* scene = getScene (sceneIndex))
    {
        if (juce::isPositiveAndBelow (snippetIndex, static_cast<int> (scene->snippets.size())))
        {
            auto& snippet = *scene->snippets[(size_t) snippetIndex];
            if (snippet.state != nullptr)
                return state::apply (engine, *snippet.state, snippet.sections, &snippet.strips, false);
        }
    }

    return {};
}

//==============================================================================
void Show::startFade (std::vector<state::FadeTarget> targets, double seconds)
{
    fades.clear();

    for (auto& t : targets)
    {
        // A later target for the same parameter (e.g. from a snippet) wins.
        auto existing = std::find_if (fades.begin(), fades.end(), [&] (const Fade& f) { return f.param == t.param; });
        if (existing != fades.end())
            existing->target = t.target;
        else
            fades.push_back ({ t.param, t.param->load(), t.target });
    }

    fadeStartMs = juce::Time::getMillisecondCounterHiRes();
    fadeDurationMs = seconds * 1000.0;
    startTimerHz (60);
}

void Show::cancelFade()
{
    // Jump to the end of a running fade so the previous scene is fully applied.
    for (auto& f : fades)
        f.param->store (f.target);

    fades.clear();
    stopTimer();
}

void Show::timerCallback()
{
    const auto t = juce::jlimit (0.0, 1.0, (juce::Time::getMillisecondCounterHiRes() - fadeStartMs)
                                               / juce::jmax (1.0, fadeDurationMs));

    for (auto& f : fades)
        f.param->store (f.start + static_cast<float> (t) * (f.target - f.start));

    if (t >= 1.0)
    {
        fades.clear();
        stopTimer();
    }
}

//==============================================================================
std::unique_ptr<juce::XmlElement> Show::toXml() const
{
    auto root = std::make_unique<juce::XmlElement> ("LiveDAWShow");
    root->setAttribute ("version", kShowVersion);
    root->setAttribute ("name", name);
    root->setAttribute ("currentScene", currentScene);
    root->addChildElement (state::saveConsole (engine).release());
    root->addChildElement (layers.toXml().release());

    auto* scenesXml = root->createNewChildElement ("Scenes");
    for (auto& scene : scenes)
    {
        auto* sx = scenesXml->createNewChildElement ("Scene");
        sx->setAttribute ("name", scene->name);
        sx->setAttribute ("notes", scene->notes);
        sx->setAttribute ("scope", static_cast<int> (scene->scope));
        sx->setAttribute ("fade", scene->fadeSeconds);
        if (scene->state != nullptr)
            sx->addChildElement (new juce::XmlElement (*scene->state));

        auto* snippetsXml = sx->createNewChildElement ("Snippets");
        for (auto& snippet : scene->snippets)
        {
            auto* nx = snippetsXml->createNewChildElement ("Snippet");
            nx->setAttribute ("name", snippet->name);
            nx->setAttribute ("sections", static_cast<int> (snippet->sections));
            nx->setAttribute ("strips", refsToString (snippet->strips));
            nx->setAttribute ("fireWithScene", snippet->fireWithScene ? 1 : 0);
            if (snippet->state != nullptr)
                nx->addChildElement (new juce::XmlElement (*snippet->state));
        }
    }

    return root;
}

juce::Result Show::loadFromXml (const juce::XmlElement& xml, bool loadPlugins)
{
    cancelFade();

    // Version 1 files were a bare console ("session").
    if (xml.hasTagName ("LiveDAWSession"))
    {
        scenes.clear();
        layers.reset();
        currentScene = -1;
        name = "Imported session";

        juce::XmlElement console ("Console");
        for (auto* child : xml.getChildIterator())
            console.addChildElement (new juce::XmlElement (*child));

        const auto result = state::loadConsole (engine, console, host, loadPlugins);
        changed();
        return result;
    }

    if (! xml.hasTagName ("LiveDAWShow"))
        return juce::Result::fail ("Not a Live DAW show file");

    if (xml.getIntAttribute ("version") > kShowVersion)
        return juce::Result::fail ("This show was saved by a newer version of Live DAW");

    auto* console = xml.getChildByName ("Console");
    if (console == nullptr)
        return juce::Result::fail ("The show file has no console section");

    scenes.clear();
    name = xml.getStringAttribute ("name", "Untitled show");

    if (auto* layersXml = xml.getChildByName ("Layers"))
        layers.fromXml (*layersXml);
    else
        layers.reset();

    const auto result = state::loadConsole (engine, *console, host, loadPlugins);

    if (auto* scenesXml = xml.getChildByName ("Scenes"))
    {
        for (auto* sx : scenesXml->getChildWithTagNameIterator ("Scene"))
        {
            auto scene = std::make_unique<Scene>();
            scene->name = sx->getStringAttribute ("name");
            scene->notes = sx->getStringAttribute ("notes");
            scene->scope = static_cast<juce::uint32> (sx->getIntAttribute ("scope", static_cast<int> (scene->scope)));
            scene->fadeSeconds = juce::jlimit (0.0, 60.0, sx->getDoubleAttribute ("fade"));
            if (auto* st = sx->getChildByName ("State"))
                scene->state = std::make_unique<juce::XmlElement> (*st);

            if (auto* snippetsXml = sx->getChildByName ("Snippets"))
            {
                for (auto* nx : snippetsXml->getChildWithTagNameIterator ("Snippet"))
                {
                    auto snippet = std::make_unique<Snippet>();
                    snippet->name = nx->getStringAttribute ("name");
                    snippet->sections = static_cast<juce::uint32> (nx->getIntAttribute ("sections"));
                    snippet->strips = refsFromString (nx->getStringAttribute ("strips"));
                    snippet->fireWithScene = nx->getBoolAttribute ("fireWithScene");
                    if (auto* st = nx->getChildByName ("State"))
                        snippet->state = std::make_unique<juce::XmlElement> (*st);
                    scene->snippets.push_back (std::move (snippet));
                }
            }

            scenes.push_back (std::move (scene));
        }
    }

    currentScene = juce::jlimit (-1, getNumScenes() - 1, xml.getIntAttribute ("currentScene", -1));
    changed();
    return result;
}

juce::Result Show::saveToFile (const juce::File& file) const
{
    const auto text = toXml()->toString();

    if (file.existsAsFile())
        file.copyFileTo (file.withFileExtension (file.getFileExtension() + ".bak"));

    juce::TemporaryFile temp (file);

    if (! temp.getFile().replaceWithText (text))
        return juce::Result::fail ("Could not write " + temp.getFile().getFullPathName());

    // Verify what hit the disk before it replaces the real file.
    if (juce::parseXML (temp.getFile()) == nullptr)
        return juce::Result::fail ("Verification of the saved show failed");

    if (! temp.overwriteTargetFileWithTemporary())
        return juce::Result::fail ("Could not replace " + file.getFullPathName());

    return juce::Result::ok();
}

juce::Result Show::loadFromFile (const juce::File& file, bool loadPlugins)
{
    auto xml = juce::parseXML (file);

    if (xml == nullptr)
    {
        // Fall back to the backup made by the previous save.
        const auto backup = file.withFileExtension (file.getFileExtension() + ".bak");
        xml = juce::parseXML (backup);

        if (xml == nullptr)
            return juce::Result::fail ("Could not read " + file.getFullPathName());
    }

    return loadFromXml (*xml, loadPlugins);
}

void Show::clear (int numChannels)
{
    cancelFade();
    scenes.clear();
    currentScene = -1;
    name = "Untitled show";
    layers.reset();
    engine.reset();
    engine.setNumInputs (numChannels);
    changed();
}

} // namespace livedaw
