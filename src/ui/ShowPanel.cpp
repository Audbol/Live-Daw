#include "ui/ShowPanel.h"
#include "ui/LookAndFeel.h"

namespace livedaw::ui
{

namespace
{
    juce::String sectionsSummary (juce::uint32 sections)
    {
        if ((sections & state::AllSections) == state::AllSections)
            return "All";

        juce::StringArray names;
        for (int i = 0; i < state::kNumSections; ++i)
            if (sections & state::sectionAt (i))
                names.add (state::sectionName (state::sectionAt (i)));
        return names.isEmpty() ? juce::String ("Nothing") : names.joinIntoString (", ");
    }

    void styleCaption (juce::Label& l, const juce::String& text)
    {
        l.setText (text, juce::dontSendNotification);
        l.setFont (juce::FontOptions (15.0f, juce::Font::bold));
        l.setColour (juce::Label::textColourId, Colours::textDim);
    }
} // namespace

//==============================================================================
class ShowPanel::SceneList final : public juce::ListBox, private juce::ListBoxModel
{
public:
    explicit SceneList (ShowPanel& p) : panel (p)
    {
        setModel (this);
        setRowHeight (48);
        setColour (backgroundColourId, Colours::screenPanel);
    }

    std::function<void()> onSelectionChanged;

private:
    int getNumRows() override { return panel.ctx.show.getNumScenes(); }

    void paintListBoxItem (int row, juce::Graphics& g, int width, int height, bool rowSelected) override
    {
        const auto scene = panel.ctx.show.getScene (row);
        if (! scene)
            return;

        // A library row: black, the name large, type and info chips on the right.
        const bool current = row == panel.ctx.show.getCurrentSceneIndex();
        auto r = juce::Rectangle<int> (width, height).reduced (4, 3).toFloat();
        g.setColour (rowSelected ? juce::Colour (0xff6b5a17) : juce::Colours::black);
        g.fillRect (r);
        if (rowSelected)
        {
            g.setColour (Colours::yellow);
            g.drawRect (r, 2.0f);
        }

        auto t = r.reduced (10.0f, 4.0f);
        auto marker = t.removeFromLeft (22.0f);
        if (current)
            drawIcon (g, Icon::Play, marker.withSizeKeepingCentre (18.0f, 18.0f), Colours::green);
        g.setColour (Colours::textDim);
        g.setFont (juce::FontOptions (18.0f, juce::Font::bold));
        g.drawText (juce::String (row + 1).paddedLeft ('0', 2), t.removeFromLeft (34.0f), juce::Justification::centredLeft);

        auto chips = t.removeFromRight (juce::jmin (150.0f, t.getWidth() * 0.4f));
        auto top = chips.removeFromTop (chips.getHeight() * 0.56f).reduced (0.0f, 1.0f);
        drawChip (g, top.removeFromLeft (58.0f), "SCENE", Colours::amber, juce::Colours::black);
        top.removeFromLeft (4.0f);
        if (scene->numSnippets > 0)
            drawChip (g, top.removeFromLeft (juce::jmin (top.getWidth(), 70.0f)), juce::String (scene->numSnippets) + " SNIP", juce::Colour (0xffd9e8b8),
                      juce::Colours::black);
        g.setColour (Colours::textDim);
        g.setFont (juce::FontOptions (15.0f, juce::Font::bold));
        juce::String info;
        if (scene->fadeSeconds > 0.0)
            info << "FADE " << juce::String (scene->fadeSeconds, 1) << " s";
        g.drawText (info, chips.toNearestInt(), juce::Justification::centredLeft);

        g.setColour (current ? Colours::text : Colours::text.withAlpha (0.85f));
        g.setFont (juce::FontOptions (juce::jmin (26.0f, t.getHeight() * 0.8f), juce::Font::bold));
        g.drawFittedText (scene->name, t.toNearestInt(), juce::Justification::centredLeft, 1);
    }

    void selectedRowsChanged (int) override
    {
        if (onSelectionChanged)
            onSelectionChanged();
    }

    void listBoxItemDoubleClicked (int row, const juce::MouseEvent&) override
    {
        logRecall (panel.ctx, "scene " + juce::String (row + 1), panel.ctx.show.recallScene (row));
    }

    ShowPanel& panel;
};

//==============================================================================
class ShowPanel::SnippetList final : public juce::ListBox, private juce::ListBoxModel
{
public:
    explicit SnippetList (ShowPanel& p) : panel (p)
    {
        setModel (this);
        setRowHeight (48);
        setColour (backgroundColourId, Colours::screenPanel);
    }

    std::function<void()> onSelectionChanged;

private:
    int getNumRows() override
    {
        const auto scene = panel.currentSceneObject();
        return scene ? scene->numSnippets : 0;
    }

    void paintListBoxItem (int row, juce::Graphics& g, int width, int height, bool rowSelected) override
    {
        const auto found = panel.ctx.show.getSnippet (panel.selectedScene(), row);
        if (! found)
            return;

        const auto& snippet = *found;
        auto r = juce::Rectangle<int> (width, height).reduced (4, 3).toFloat();
        g.setColour (rowSelected ? juce::Colour (0xff6b5a17) : juce::Colours::black);
        g.fillRect (r);
        if (rowSelected)
        {
            g.setColour (Colours::yellow);
            g.drawRect (r, 2.0f);
        }

        auto t = r.reduced (10.0f, 4.0f);
        auto chips = t.removeFromRight (juce::jmin (170.0f, t.getWidth() * 0.45f));
        auto top = chips.removeFromTop (chips.getHeight() * 0.56f).reduced (0.0f, 1.0f);
        drawChip (g, top.removeFromLeft (52.0f), "SNIP", Colours::cyan, juce::Colours::black);
        top.removeFromLeft (4.0f);
        if (snippet.fireWithScene)
            drawChip (g, top.removeFromLeft (52.0f), "AUTO", Colours::yellow, juce::Colours::black);
        g.setColour (Colours::textDim);
        g.setFont (juce::FontOptions (14.0f, juce::Font::bold));
        g.drawFittedText (juce::String (snippet.strips.size()) + " STRIPS: " + sectionsSummary (snippet.sections).toUpperCase(), chips.toNearestInt(),
                          juce::Justification::centredLeft, 1);

        g.setColour (Colours::text);
        g.setFont (juce::FontOptions (juce::jmin (24.0f, t.getHeight() * 0.8f), juce::Font::bold));
        g.drawFittedText (snippet.name, t.toNearestInt(), juce::Justification::centredLeft, 1);
    }

    void selectedRowsChanged (int) override
    {
        if (onSelectionChanged)
            onSelectionChanged();
    }

    void listBoxItemDoubleClicked (int row, const juce::MouseEvent&) override
    {
        const int scene = panel.selectedScene();
        logRecall (panel.ctx, "snippet " + juce::String (row + 1), panel.ctx.show.recallSnippet (scene, row));
    }

    ShowPanel& panel;
};

//==============================================================================
/** A row of toggles, one per console section, editing a bit mask. */
class ShowPanel::SectionToggles final : public juce::Component
{
public:
    std::function<juce::uint32()> getMask;
    std::function<void (juce::uint32)> setMask;

    SectionToggles()
    {
        for (int i = 0; i < state::kNumSections; ++i)
        {
            auto* b = buttons.add (new juce::TextButton (juce::String (state::sectionName (state::sectionAt (i))).toUpperCase()));
            b->setClickingTogglesState (true);
            b->setColour (juce::TextButton::buttonOnColourId, Colours::yellow);
            b->setColour (juce::TextButton::textColourOnId, juce::Colours::black);
            b->onClick = [this, i, b]
            {
                if (! getMask || ! setMask)
                    return;
                auto mask = getMask();
                const auto bit = static_cast<juce::uint32> (state::sectionAt (i));
                mask = b->getToggleState() ? (mask | bit) : (mask & ~bit);
                setMask (mask);
            };
            addAndMakeVisible (b);
        }
    }

    void refresh()
    {
        const auto mask = getMask ? getMask() : 0u;
        for (int i = 0; i < buttons.size(); ++i)
        {
            buttons[i]->setEnabled (static_cast<bool> (getMask));
            buttons[i]->setToggleState ((mask & static_cast<juce::uint32> (state::sectionAt (i))) != 0,
                                        juce::dontSendNotification);
        }
    }

    void resized() override
    {
        // Five columns, two rows.
        auto b = getLocalBounds();
        constexpr int columns = 5;
        const int rows = (buttons.size() + columns - 1) / columns;
        const int h = juce::jmin (36, b.getHeight() / juce::jmax (1, rows));
        for (int r = 0; r < rows; ++r)
        {
            auto row = b.removeFromTop (h);
            const int w = row.getWidth() / columns;
            for (int c = 0; c < columns; ++c)
                if (auto* button = buttons[r * columns + c])
                    button->setBounds (row.removeFromLeft (w).reduced (1));
        }
    }

private:
    juce::OwnedArray<juce::TextButton> buttons;
};

//==============================================================================
/** Toggle list of every strip, editing a snippet's strip set. */
class ShowPanel::StripChooser final : public juce::Component
{
public:
    explicit StripChooser (UiContext& c) : ctx (c)
    {
        viewport.setViewedComponent (&content, false);
        viewport.setScrollBarsShown (true, false);
        addAndMakeVisible (viewport);
    }

    std::function<std::optional<Show::Snippet>()> getSnippet;
    std::function<void (const std::vector<state::StripRef>&)> setStrips;
    std::function<void()> onChanged;

    void refresh()
    {
        buttons.clear();
        const auto snippet = getSnippet ? getSnippet() : std::nullopt;

        auto add = [&] (Strip& s)
        {
            const auto ref = state::StripRef::of (s);
            auto* b = buttons.add (new juce::TextButton (s.getName().isNotEmpty() ? s.getName() : s.getLabel()));
            b->setTooltip (describeStrip (s));
            b->setClickingTogglesState (true);
            b->setColour (juce::TextButton::buttonOnColourId, s.getColour());
            b->setEnabled (snippet.has_value());

            if (snippet)
            {
                auto& strips = snippet->strips;
                b->setToggleState (std::find (strips.begin(), strips.end(), ref) != strips.end(), juce::dontSendNotification);
            }

            b->onClick = [this, ref, b]
            {
                auto sn = getSnippet ? getSnippet() : std::nullopt;
                if (! sn || ! setStrips)
                    return;

                auto strips = sn->strips;
                strips.erase (std::remove (strips.begin(), strips.end(), ref), strips.end());
                if (b->getToggleState())
                    strips.push_back (ref);
                setStrips (strips);

                if (onChanged)
                    onChanged();
            };
            content.addAndMakeVisible (b);
        };

        for (int k = 0; k < kNumStripKinds; ++k)
            for (auto& strip : ctx.engine.getStrips (static_cast<StripKind> (k)))
                add (*strip);

        resized();
    }

    void resized() override
    {
        viewport.setBounds (getLocalBounds());
        const int w = viewport.getWidth() - viewport.getScrollBarThickness();
        constexpr int cellW = 96, cellH = 32;
        const int columns = juce::jmax (1, w / cellW);

        for (int i = 0; i < buttons.size(); ++i)
            buttons[i]->setBounds ((i % columns) * cellW, (i / columns) * cellH, cellW - 2, cellH - 2);

        content.setSize (w, ((buttons.size() + columns - 1) / columns) * cellH);
    }

private:
    UiContext& ctx;
    juce::Viewport viewport;
    juce::Component content;
    juce::OwnedArray<juce::TextButton> buttons;
};

//==============================================================================
ShowPanel::ShowPanel (UiContext& c) : ctx (c)
{
    sceneList = std::make_unique<SceneList> (*this);
    sceneList->onSelectionChanged = [this] { updateSceneEditor(); };
    addAndMakeVisible (*sceneList);

    for (auto* b : { &storeSceneButton, &newSceneButton, &deleteSceneButton, &upButton, &downButton, &storeSnippetButton, &newSnippetButton,
                     &deleteSnippetButton, &goPrevButton, &goNextButton, &prevButton, &nextButton, &goButton })
        addAndMakeVisible (b);
    for (auto* b : { &storeSceneButton, &newSceneButton, &deleteSceneButton, &storeSnippetButton, &newSnippetButton, &deleteSnippetButton })
    {
        b->iconAbove = true;
        b->offColour = Colours::screenPanel;
    }

    goButton.setTooltip ("Recall the selected item (double-click in the list does the same)");
    goButton.onClick = [this] { step (0, true); };
    goPrevButton.onClick = [this] { step (-1, true); };
    goNextButton.onClick = [this] { step (1, true); };
    prevButton.onClick = [this] { step (-1, false); };
    nextButton.onClick = [this] { step (1, false); };

    storeSceneButton.setTooltip ("Overwrite the selected scene with the current console");
    storeSceneButton.onClick = [this]
    {
        const int index = selectedScene();
        if (const auto scene = currentSceneObject())
        {
            juce::AlertWindow::showOkCancelBox (juce::MessageBoxIconType::QuestionIcon, "Store scene",
                                                "Overwrite \"" + scene->name + "\" with the current console?",
                                                "Store", "Cancel", this,
                                                juce::ModalCallbackFunction::create ([this, index] (int result)
            {
                if (result != 0)
                {
                    ctx.show.storeScene (index);
                    ctx.log.info ("Stored scene " + juce::String (index + 1));
                }
            }));
        }
    };

    newSceneButton.setTooltip ("Capture the current console as a new scene after the selected one");
    newSceneButton.onClick = [this]
    {
        const int at = selectedScene() >= 0 ? selectedScene() + 1 : ctx.show.getNumScenes();
        const int index = ctx.show.addScene ("Scene " + juce::String (ctx.show.getNumScenes() + 1), at);
        sceneList->selectRow (index);
    };

    deleteSceneButton.onClick = [this]
    {
        const int index = selectedScene();
        if (const auto scene = currentSceneObject())
        {
            juce::AlertWindow::showOkCancelBox (juce::MessageBoxIconType::WarningIcon, "Delete scene",
                                                "Delete \"" + scene->name + "\" and its snippets?", "Delete", "Cancel", this,
                                                juce::ModalCallbackFunction::create ([this, index] (int result)
            {
                if (result != 0)
                    ctx.show.removeScene (index);
            }));
        }
    };

    upButton.onClick = [this]
    {
        const int i = selectedScene();
        if (i > 0)
        {
            ctx.show.moveScene (i, i - 1);
            sceneList->selectRow (i - 1);
        }
    };

    downButton.onClick = [this]
    {
        const int i = selectedScene();
        if (i >= 0 && i < ctx.show.getNumScenes() - 1)
        {
            ctx.show.moveScene (i, i + 1);
            sceneList->selectRow (i + 1);
        }
    };

    styleCaption (fadeLabel, "FADE (FADERS, PANS, SENDS)");
    styleCaption (notesLabel, "NOTES");
    addAndMakeVisible (fadeLabel);
    addAndMakeVisible (notesLabel);

    sceneNameLabel.setEditable (true);
    sceneNameLabel.setFont (juce::FontOptions (24.0f, juce::Font::bold));
    sceneNameLabel.setColour (juce::Label::backgroundColourId, juce::Colours::black.withAlpha (0.25f));
    sceneNameLabel.onTextChange = [this]
    {
        if (selectedScene() >= 0)
        {
            ctx.show.setSceneName (selectedScene(), sceneNameLabel.getText());
            sceneList->repaint();
        }
    };
    addAndMakeVisible (sceneNameLabel);

    fadeSlider.setSliderStyle (juce::Slider::LinearHorizontal);
    fadeSlider.setRange (0.0, 30.0, 0.1);
    fadeSlider.setSkewFactorFromMidPoint (3.0);
    fadeSlider.setTextValueSuffix (" s");
    fadeSlider.setTextBoxStyle (juce::Slider::TextBoxRight, false, 64, 26);
    fadeSlider.onValueChange = [this]
    {
        if (selectedScene() >= 0)
        {
            ctx.show.setSceneFadeSeconds (selectedScene(), fadeSlider.getValue());
            sceneList->repaint();
        }
    };
    addAndMakeVisible (fadeSlider);

    notesEditor.setMultiLine (true);
    notesEditor.setReturnKeyStartsNewLine (true);
    notesEditor.onTextChange = [this]
    {
        if (selectedScene() >= 0)
            ctx.show.setSceneNotes (selectedScene(), notesEditor.getText());
    };
    addAndMakeVisible (notesEditor);

    sceneScope = std::make_unique<SectionToggles>();
    addAndMakeVisible (*sceneScope);

    // Snippets
    snippetList = std::make_unique<SnippetList> (*this);
    snippetList->onSelectionChanged = [this] { updateSnippetEditor(); };
    addAndMakeVisible (*snippetList);

    storeSnippetButton.setTooltip ("Re-capture the selected snippet's strips and sections");
    storeSnippetButton.onClick = [this]
    {
        if (selectedSnippet() >= 0)
        {
            ctx.show.storeSnippet (selectedScene(), selectedSnippet());
            ctx.log.info ("Stored snippet " + juce::String (selectedSnippet() + 1));
        }
    };

    newSnippetButton.setTooltip ("New snippet in the selected scene (starts with the selected channel)");
    newSnippetButton.onClick = [this]
    {
        const auto scene = currentSceneObject();
        if (! scene)
            return;

        std::vector<state::StripRef> strips;
        if (auto* selected = ctx.getSelectedStrip ? ctx.getSelectedStrip() : nullptr)
            strips.push_back (state::StripRef::of (*selected));

        const int index = ctx.show.addSnippet (selectedScene(), "Snippet " + juce::String (scene->numSnippets + 1),
                                               state::Fader | state::Sends, std::move (strips));
        snippetList->updateContent();
        snippetList->selectRow (index);
    };

    deleteSnippetButton.onClick = [this]
    {
        if (selectedSnippet() >= 0)
            ctx.show.removeSnippet (selectedScene(), selectedSnippet());
    };

    autoFireKey.setTooltip ("Recall this snippet automatically right after its scene");
    autoFireKey.isOn = [this] { const auto sn = currentSnippetObject(); return sn && sn->fireWithScene; };
    autoFireKey.onClick = [this]
    {
        if (const auto sn = currentSnippetObject())
        {
            ctx.show.setSnippetFireWithScene (selectedScene(), selectedSnippet(), ! sn->fireWithScene);
            snippetList->repaint();
            autoFireKey.repaint();
        }
    };
    addAndMakeVisible (autoFireKey);

    snippetNameLabel.setEditable (true);
    snippetNameLabel.setFont (juce::FontOptions (24.0f, juce::Font::bold));
    snippetNameLabel.setColour (juce::Label::backgroundColourId, juce::Colours::black.withAlpha (0.25f));
    snippetNameLabel.onTextChange = [this]
    {
        if (selectedSnippet() >= 0)
        {
            ctx.show.setSnippetName (selectedScene(), selectedSnippet(), snippetNameLabel.getText());
            snippetList->repaint();
        }
    };
    addAndMakeVisible (snippetNameLabel);

    snippetSections = std::make_unique<SectionToggles>();
    addAndMakeVisible (*snippetSections);

    stripChooser = std::make_unique<StripChooser> (ctx);
    stripChooser->getSnippet = [this] { return currentSnippetObject(); };
    stripChooser->setStrips = [this] (const std::vector<state::StripRef>& strips)
    {
        if (selectedSnippet() >= 0)
            ctx.show.setSnippetStrips (selectedScene(), selectedSnippet(), strips);
    };
    stripChooser->onChanged = [this] { snippetList->repaint(); };
    addAndMakeVisible (*stripChooser);

    setView (View::Scenes);
    refresh();
}

ShowPanel::~ShowPanel() = default;

void ShowPanel::setView (View v)
{
    view = v;
    const bool scenes = v == View::Scenes;
    for (juce::Component* c : { (juce::Component*) sceneList.get(), (juce::Component*) &storeSceneButton, (juce::Component*) &newSceneButton,
                                (juce::Component*) &deleteSceneButton, (juce::Component*) &upButton, (juce::Component*) &downButton,
                                (juce::Component*) &sceneNameLabel, (juce::Component*) &fadeLabel, (juce::Component*) &fadeSlider,
                                (juce::Component*) &notesLabel, (juce::Component*) &notesEditor, (juce::Component*) sceneScope.get() })
        c->setVisible (scenes);
    for (juce::Component* c : { (juce::Component*) snippetList.get(), (juce::Component*) &storeSnippetButton, (juce::Component*) &newSnippetButton,
                                (juce::Component*) &deleteSnippetButton, (juce::Component*) &autoFireKey, (juce::Component*) &snippetNameLabel,
                                (juce::Component*) snippetSections.get(), (juce::Component*) stripChooser.get() })
        c->setVisible (! scenes);
    resized();
    repaint();
}

void ShowPanel::step (int delta, bool recall)
{
    if (view == View::Scenes)
    {
        const int n = ctx.show.getNumScenes();
        if (n == 0)
            return;
        const int current = selectedScene() >= 0 ? selectedScene() : juce::jmax (0, ctx.show.getCurrentSceneIndex());
        const int target = juce::jlimit (0, n - 1, current + delta);
        sceneList->selectRow (target);
        if (recall)
            logRecall (ctx, "scene " + juce::String (target + 1), ctx.show.recallScene (target));
        return;
    }

    const auto scene = currentSceneObject();
    if (! scene || scene->numSnippets == 0)
        return;
    const int target = juce::jlimit (0, scene->numSnippets - 1, juce::jmax (0, selectedSnippet()) + (selectedSnippet() >= 0 ? delta : 0));
    snippetList->selectRow (target);
    if (recall)
        logRecall (ctx, "snippet " + juce::String (target + 1), ctx.show.recallSnippet (selectedScene(), target));
}

int ShowPanel::selectedScene() const
{
    const int row = sceneList->getSelectedRow();
    return juce::isPositiveAndBelow (row, ctx.show.getNumScenes()) ? row : -1;
}

int ShowPanel::selectedSnippet() const
{
    const auto scene = currentSceneObject();
    const int row = snippetList->getSelectedRow();
    return scene && juce::isPositiveAndBelow (row, scene->numSnippets) ? row : -1;
}

std::optional<Show::Scene> ShowPanel::currentSceneObject() const
{
    return ctx.show.getScene (selectedScene());
}

std::optional<Show::Snippet> ShowPanel::currentSnippetObject() const
{
    const int row = snippetList != nullptr ? snippetList->getSelectedRow() : -1;
    return ctx.show.getSnippet (selectedScene(), row);
}

void ShowPanel::refresh()
{
    sceneList->updateContent();

    // Follow the current scene when nothing valid is selected.
    if (selectedScene() < 0 && ctx.show.getCurrentSceneIndex() >= 0)
        sceneList->selectRow (ctx.show.getCurrentSceneIndex());

    sceneList->repaint();
    updateSceneEditor();
}

void ShowPanel::updateSceneEditor()
{
    const auto scene = currentSceneObject();
    const bool has = scene.has_value();

    for (juce::Component* c : { (juce::Component*) &sceneNameLabel, (juce::Component*) &fadeSlider, (juce::Component*) &notesEditor,
                                (juce::Component*) &storeSceneButton,
                                (juce::Component*) &deleteSceneButton, (juce::Component*) &upButton,
                                (juce::Component*) &downButton, (juce::Component*) &newSnippetButton })
        c->setEnabled (has);

    sceneNameLabel.setText (has ? scene->name : juce::String ("No scene selected"), juce::dontSendNotification);
    fadeSlider.setValue (has ? scene->fadeSeconds : 0.0, juce::dontSendNotification);
    if (! notesEditor.hasKeyboardFocus (true))
        notesEditor.setText (has ? scene->notes : juce::String(), false);

    if (has)
    {
        sceneScope->getMask = [this] { const auto s = currentSceneObject(); return s ? s->scope : 0u; };
        sceneScope->setMask = [this] (juce::uint32 m) { if (selectedScene() >= 0) ctx.show.setSceneScope (selectedScene(), m); };
    }
    else
    {
        sceneScope->getMask = nullptr;
        sceneScope->setMask = nullptr;
    }
    sceneScope->refresh();

    snippetList->updateContent();
    snippetList->repaint();
    updateSnippetEditor();
    repaint();
}

void ShowPanel::updateSnippetEditor()
{
    const auto snippet = currentSnippetObject();
    const bool has = snippet.has_value();

    for (juce::Component* c : { (juce::Component*) &storeSnippetButton, (juce::Component*) &deleteSnippetButton, (juce::Component*) &autoFireKey,
                                (juce::Component*) &snippetNameLabel })
        c->setEnabled (has);

    snippetNameLabel.setText (has ? snippet->name : juce::String ("No snippet selected"), juce::dontSendNotification);
    autoFireKey.repaint();

    if (has)
    {
        snippetSections->getMask = [this] { const auto s = currentSnippetObject(); return s ? s->sections : 0u; };
        snippetSections->setMask = [this] (juce::uint32 m)
        {
            if (selectedSnippet() >= 0)
            {
                ctx.show.setSnippetSections (selectedScene(), selectedSnippet(), m);
                snippetList->repaint();
            }
        };
    }
    else
    {
        snippetSections->getMask = nullptr;
        snippetSections->setMask = nullptr;
    }

    snippetSections->refresh();
    stripChooser->refresh();
}

void ShowPanel::paint (juce::Graphics& g)
{
    g.fillAll (Colours::screen);

    // Key bar, list and transport sit on grey panels; the editor is a titled panel.
    for (auto r : { keyBar, listArea, transportArea })
    {
        g.setColour (Colours::screenPanel);
        g.fillRect (r);
    }

    const auto scene = currentSceneObject();
    g.setColour (Colours::text);
    g.setFont (juce::FontOptions (juce::jmin (18.0f, keyBarTitle.getHeight() * 0.42f), juce::Font::bold));
    if (view == View::Scenes)
    {
        g.drawFittedText (ctx.show.getName().toUpperCase() + "\n" + juce::String (ctx.show.getNumScenes()) + " SCENES", keyBarTitle,
                          juce::Justification::centredRight, 2);
        drawSectionBox (g, editorArea, scene ? "SCENE " + juce::String (selectedScene() + 1).paddedLeft ('0', 2) : juce::String ("SCENE"));
    }
    else
    {
        g.drawFittedText (scene ? "IN SCENE " + juce::String (selectedScene() + 1).paddedLeft ('0', 2) + "\n" + scene->name.toUpperCase()
                                : juce::String ("NO SCENE SELECTED"),
                          keyBarTitle, juce::Justification::centredRight, 2);
        drawSectionBox (g, editorArea, "SNIPPET");
    }
}

void ShowPanel::resized()
{
    auto b = getLocalBounds();
    auto left = b.removeFromLeft (b.getWidth() * 50 / 100);
    b.removeFromLeft (8);

    keyBar = left.removeFromTop (juce::jmin (56, left.getHeight() / 7));
    left.removeFromTop (6);
    listArea = left;

    transportArea = b.removeFromBottom (juce::jmin (124, b.getHeight() * 30 / 100));
    b.removeFromBottom (6);
    editorArea = b;

    // Key bar: icon keys, then a title.
    {
        auto k = keyBar.reduced (6);
        const int w = juce::jmin (76, k.getWidth() / 7);
        juce::Array<WingKey*> keys;
        if (view == View::Scenes)
            keys = { &newSceneButton, &storeSceneButton, &deleteSceneButton, &upButton, &downButton };
        else
            keys = { &newSnippetButton, &storeSnippetButton, &deleteSnippetButton };
        for (auto* key : keys)
        {
            key->setBounds (k.removeFromLeft (w).reduced (2, 0));
            k.removeFromLeft (2);
        }
        keyBarTitle = k.reduced (6, 0);
    }

    sceneList->setBounds (listArea.reduced (4));
    snippetList->setBounds (listArea.reduced (4));

    // Transport, like the console: GO+PREV / GO+NEXT, PREV over NEXT, GO.
    {
        auto t = transportArea.reduced (8);
        auto go = t.removeFromRight (t.getWidth() / 4);
        goButton.setBounds (go.reduced (3));
        t.removeFromRight (4);
        auto steps = t.removeFromRight (t.getWidth() / 3);
        prevButton.setBounds (steps.removeFromTop (steps.getHeight() / 2).reduced (3));
        nextButton.setBounds (steps.reduced (3));
        t.removeFromRight (t.getWidth() / 6);
        goPrevButton.setBounds (t.removeFromLeft (t.getWidth() / 2).reduced (3));
        goNextButton.setBounds (t.reduced (3));
    }

    auto e = sectionContent (editorArea, true);
    if (view == View::Scenes)
    {
        sceneNameLabel.setBounds (e.removeFromTop (36));
        e.removeFromTop (6);
        auto fade = e.removeFromTop (30);
        fadeLabel.setBounds (fade.removeFromLeft (juce::jmin (180, fade.getWidth() / 3)));
        fadeSlider.setBounds (fade);
        e.removeFromTop (6);
        sceneScope->setBounds (e.removeFromTop (juce::jmin (72, e.getHeight() / 2)));
        e.removeFromTop (6);
        notesLabel.setBounds (e.removeFromTop (16));
        notesEditor.setBounds (e);
    }
    else
    {
        auto top = e.removeFromTop (36);
        autoFireKey.setBounds (top.removeFromRight (juce::jmin (170, top.getWidth() / 3)));
        snippetNameLabel.setBounds (top.withTrimmedRight (8));
        e.removeFromTop (6);
        snippetSections->setBounds (e.removeFromTop (juce::jmin (72, e.getHeight() / 2)));
        e.removeFromTop (6);
        stripChooser->setBounds (e);
    }
}

} // namespace livedaw::ui
