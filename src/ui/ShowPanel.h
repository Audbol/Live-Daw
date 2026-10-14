#pragma once

#include "ui/UiContext.h"

namespace livedaw::ui
{

/** Show > Scene > Snippet manager: the scene list with recall scope, fade and
    notes, and the snippets that belong to the selected scene. */
class ShowPanel final : public juce::Component
{
public:
    explicit ShowPanel (UiContext&);
    ~ShowPanel() override;

    /** Re-reads the show (call after it changed). */
    void refresh();

    void paint (juce::Graphics&) override;
    void resized() override;

private:
    class SceneList;
    class SnippetList;
    class SectionToggles;
    class StripChooser;

    int selectedScene() const;
    int selectedSnippet() const;
    Show::Scene* currentSceneObject() const;
    Show::Snippet* currentSnippetObject() const;
    void updateSceneEditor();
    void updateSnippetEditor();

    UiContext& ctx;

    std::unique_ptr<SceneList> sceneList;
    juce::TextButton goButton { "GO" }, storeSceneButton { "Store" }, newSceneButton { "+ New" },
        deleteSceneButton { "Delete" }, upButton { "Up" }, downButton { "Down" };
    juce::Label sceneNameLabel, fadeLabel, notesLabel;
    juce::Slider fadeSlider;
    juce::TextEditor notesEditor;
    std::unique_ptr<SectionToggles> sceneScope;

    std::unique_ptr<SnippetList> snippetList;
    juce::TextButton recallSnippetButton { "Recall" }, storeSnippetButton { "Store" },
        newSnippetButton { "+ New" }, deleteSnippetButton { "Delete" };
    juce::ToggleButton autoFireToggle { "Fire with scene" };
    juce::Label snippetNameLabel;
    std::unique_ptr<SectionToggles> snippetSections;
    std::unique_ptr<StripChooser> stripChooser;

    juce::Rectangle<int> sceneArea, scopeArea, snippetArea;
};

} // namespace livedaw::ui
