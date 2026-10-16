#pragma once

#include "ui/UiContext.h"
#include "ui/WingWidgets.h"

namespace livedaw::ui
{

/** Show > Scene > Snippet manager in the style of the console's library: a
    list on the left, the selected item's settings on the right and the GO /
    PREV / NEXT keys below. The SCENES view lists the show's scenes (recall
    scope, fade, notes); the SNIPPETS view the snippets of the selected scene. */
class ShowPanel final : public juce::Component
{
public:
    explicit ShowPanel (UiContext&);
    ~ShowPanel() override;

    enum class View
    {
        Scenes,
        Snippets
    };

    void setView (View);
    View getView() const noexcept { return view; }

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
    std::optional<Show::Scene> currentSceneObject() const;
    std::optional<Show::Snippet> currentSnippetObject() const;
    void updateSceneEditor();
    void updateSnippetEditor();
    /** Moves the selection of the current view by `delta`, recalling the result if asked. */
    void step (int delta, bool recall);

    UiContext& ctx;
    View view = View::Scenes;

    std::unique_ptr<SceneList> sceneList;
    WingKey storeSceneButton { "STORE", Icon::Disk }, newSceneButton { "NEW", Icon::Plus }, deleteSceneButton { "DELETE", Icon::Trash },
        upButton { "UP" }, downButton { "DOWN" };
    juce::Label sceneNameLabel, fadeLabel, notesLabel;
    juce::Slider fadeSlider;
    juce::TextEditor notesEditor;
    std::unique_ptr<SectionToggles> sceneScope;

    std::unique_ptr<SnippetList> snippetList;
    WingKey storeSnippetButton { "STORE", Icon::Disk }, newSnippetButton { "NEW", Icon::Plus }, deleteSnippetButton { "DELETE", Icon::Trash },
        autoFireKey { "FIRE WITH SCENE" };
    juce::Label snippetNameLabel;
    std::unique_ptr<SectionToggles> snippetSections;
    std::unique_ptr<StripChooser> stripChooser;

    WingKey goPrevButton { "GO\n+PREV" }, goNextButton { "GO\n+NEXT" }, prevButton { "PREV", Icon::ChevronLeft },
        nextButton { "NEXT", Icon::ChevronRight }, goButton { "GO", Icon::Play };

    juce::Rectangle<int> keyBar, listArea, editorArea, transportArea, keyBarTitle;
};

} // namespace livedaw::ui
