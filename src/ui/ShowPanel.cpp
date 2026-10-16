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
        l.setFont (juce::FontOptions (11.0f));
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
        setRowHeight (22);
        setColour (backgroundColourId, juce::Colours::black.withAlpha (0.3f));
    }

    std::function<void()> onSelectionChanged;

private:
    int getNumRows() override { return panel.ctx.show.getNumScenes(); }

    void paintListBoxItem (int row, juce::Graphics& g, int width, int height, bool rowSelected) override
    {
        const auto scene = panel.ctx.show.getScene (row);
        if (! scene)
            return;

        const bool current = row == panel.ctx.show.getCurrentSceneIndex();
        auto r = juce::Rectangle<int> (width, height);

        if (rowSelected)
            g.fillAll (Colours::accent.withAlpha (0.35f));

        if (current)
        {
            g.setColour (Colours::active);
            g.fillRect (r.removeFromLeft (4));
        }
        else
        {
            r.removeFromLeft (4);
        }

        g.setColour (current ? Colours::active : Colours::textDim);
        g.setFont (juce::FontOptions (13.0f, juce::Font::bold));
        g.drawText (juce::String (row + 1).paddedLeft ('0', 2), r.removeFromLeft (32), juce::Justification::centred);

        auto right = r.removeFromRight (110);
        g.setColour (Colours::text);
        g.setFont (juce::FontOptions (13.0f));
        g.drawText (scene->name, r, juce::Justification::centredLeft);

        g.setColour (Colours::textDim);
        g.setFont (juce::FontOptions (11.0f));
        juce::String info;
        if (scene->fadeSeconds > 0.0)
            info << "fade " << juce::String (scene->fadeSeconds, 1) << "s  ";
        if (scene->numSnippets > 0)
            info << scene->numSnippets << " snip";
        g.drawText (info, right.reduced (4, 0), juce::Justification::centredRight);
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
        setRowHeight (22);
        setColour (backgroundColourId, juce::Colours::black.withAlpha (0.3f));
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
        auto r = juce::Rectangle<int> (width, height).reduced (6, 0);

        if (rowSelected)
            g.fillAll (Colours::accent.withAlpha (0.35f));

        if (snippet.fireWithScene)
        {
            auto badge = r.removeFromRight (40).reduced (0, 4);
            g.setColour (Colours::solo);
            g.fillRoundedRectangle (badge.toFloat(), 3.0f);
            g.setColour (juce::Colours::black);
            g.setFont (juce::FontOptions (10.0f, juce::Font::bold));
            g.drawText ("AUTO", badge, juce::Justification::centred);
        }

        g.setColour (Colours::text);
        g.setFont (juce::FontOptions (13.0f));
        g.drawText (snippet.name, r.removeFromLeft (r.getWidth() / 2), juce::Justification::centredLeft);
        g.setColour (Colours::textDim);
        g.setFont (juce::FontOptions (11.0f));
        g.drawText (juce::String (snippet.strips.size()) + " strips: " + sectionsSummary (snippet.sections),
                    r, juce::Justification::centredLeft);
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
            auto* b = buttons.add (new juce::TextButton (state::sectionName (state::sectionAt (i))));
            b->setClickingTogglesState (true);
            b->setColour (juce::TextButton::buttonOnColourId, Colours::active);
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
        // Three columns.
        auto b = getLocalBounds();
        const int rows = (buttons.size() + 2) / 3;
        const int h = juce::jmin (24, b.getHeight() / juce::jmax (1, rows));
        for (int r = 0; r < rows; ++r)
        {
            auto row = b.removeFromTop (h);
            const int w = row.getWidth() / 3;
            for (int c = 0; c < 3; ++c)
                if (auto* button = buttons[r * 3 + c])
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
            auto* b = buttons.add (new juce::TextButton (s.getName()));
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

        for (auto& in : ctx.engine.getInputs())
            add (*in);
        for (auto& bus : ctx.engine.getBuses())
            add (*bus);
        add (ctx.engine.getMaster());

        resized();
    }

    void resized() override
    {
        viewport.setBounds (getLocalBounds());
        const int w = viewport.getWidth() - viewport.getScrollBarThickness();
        constexpr int cellW = 76, cellH = 22;
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

    for (auto* b : { &goButton, &storeSceneButton, &newSceneButton, &deleteSceneButton, &upButton, &downButton,
                     &recallSnippetButton, &storeSnippetButton, &newSnippetButton, &deleteSnippetButton })
        addAndMakeVisible (b);

    goButton.setColour (juce::TextButton::buttonColourId, Colours::active.darker (0.3f));
    goButton.setTooltip ("Recall the selected scene (double-click a scene does the same)");
    goButton.onClick = [this]
    {
        if (selectedScene() >= 0)
            logRecall (ctx, "scene " + juce::String (selectedScene() + 1), ctx.show.recallScene (selectedScene()));
    };

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

    styleCaption (fadeLabel, "Fade (faders, pans, sends)");
    styleCaption (notesLabel, "Notes");
    addAndMakeVisible (fadeLabel);
    addAndMakeVisible (notesLabel);

    sceneNameLabel.setEditable (true);
    sceneNameLabel.setFont (juce::FontOptions (15.0f, juce::Font::bold));
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
    fadeSlider.setTextBoxStyle (juce::Slider::TextBoxRight, false, 56, 20);
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

    recallSnippetButton.setColour (juce::TextButton::buttonColourId, Colours::active.darker (0.3f));
    recallSnippetButton.onClick = [this]
    {
        if (selectedSnippet() >= 0)
            logRecall (ctx, "snippet " + juce::String (selectedSnippet() + 1),
                       ctx.show.recallSnippet (selectedScene(), selectedSnippet()));
    };

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

    autoFireToggle.setTooltip ("Recall this snippet automatically right after its scene");
    autoFireToggle.onClick = [this]
    {
        if (selectedSnippet() >= 0)
        {
            ctx.show.setSnippetFireWithScene (selectedScene(), selectedSnippet(), autoFireToggle.getToggleState());
            snippetList->repaint();
        }
    };
    addAndMakeVisible (autoFireToggle);

    snippetNameLabel.setEditable (true);
    snippetNameLabel.setFont (juce::FontOptions (14.0f, juce::Font::bold));
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

    refresh();
}

ShowPanel::~ShowPanel() = default;

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
                                (juce::Component*) &goButton, (juce::Component*) &storeSceneButton,
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

    for (juce::Component* c : { (juce::Component*) &recallSnippetButton, (juce::Component*) &storeSnippetButton,
                                (juce::Component*) &deleteSnippetButton, (juce::Component*) &autoFireToggle,
                                (juce::Component*) &snippetNameLabel })
        c->setEnabled (has);

    snippetNameLabel.setText (has ? snippet->name : juce::String ("No snippet selected"), juce::dontSendNotification);
    autoFireToggle.setToggleState (has && snippet->fireWithScene, juce::dontSendNotification);

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
    g.fillAll (Colours::background.darker (0.2f));

    auto drawBox = [&g] (juce::Rectangle<int> r, const juce::String& title)
    {
        g.setColour (Colours::panel);
        g.fillRoundedRectangle (r.toFloat().reduced (2.0f), 5.0f);
        g.setColour (Colours::textDim);
        g.setFont (juce::FontOptions (12.0f, juce::Font::bold));
        g.drawText (title, r.reduced (10, 6).removeFromTop (14), juce::Justification::centredLeft);
    };

    const auto scene = currentSceneObject();
    drawBox (sceneArea, "SCENES - " + ctx.show.getName().toUpperCase());
    drawBox (scopeArea, "SCENE: RECALL SCOPE / FADE / NOTES");
    drawBox (snippetArea, "SNIPPETS" + (scene ? " IN \"" + scene->name.toUpperCase() + "\"" : juce::String()));
}

void ShowPanel::resized()
{
    auto b = getLocalBounds().reduced (2);
    sceneArea = b.removeFromLeft (juce::jmax (320, b.getWidth() * 30 / 100));
    scopeArea = b.removeFromLeft (juce::jmax (280, b.getWidth() * 35 / 100));
    snippetArea = b;

    // Scenes
    {
        auto r = sceneArea.reduced (10).withTrimmedTop (18);
        auto buttons = r.removeFromBottom (26);
        const int w = buttons.getWidth() / 6;
        for (auto* button : { &goButton, &storeSceneButton, &newSceneButton, &deleteSceneButton, &upButton, &downButton })
            button->setBounds (buttons.removeFromLeft (w).reduced (1, 0));
        r.removeFromBottom (4);
        sceneList->setBounds (r);
    }

    // Scene editor
    {
        auto r = scopeArea.reduced (10).withTrimmedTop (18);
        sceneNameLabel.setBounds (r.removeFromTop (24));
        r.removeFromTop (6);
        sceneScope->setBounds (r.removeFromTop (72));
        r.removeFromTop (6);
        fadeLabel.setBounds (r.removeFromTop (14));
        fadeSlider.setBounds (r.removeFromTop (24));
        r.removeFromTop (4);
        notesLabel.setBounds (r.removeFromTop (14));
        notesEditor.setBounds (r);
    }

    // Snippets
    {
        auto r = snippetArea.reduced (10).withTrimmedTop (18);
        auto left = r.removeFromLeft (r.getWidth() / 2);
        auto buttons = left.removeFromBottom (26);
        const int w = buttons.getWidth() / 4;
        for (auto* button : { &recallSnippetButton, &storeSnippetButton, &newSnippetButton, &deleteSnippetButton })
            button->setBounds (buttons.removeFromLeft (w).reduced (1, 0));
        left.removeFromBottom (4);
        snippetList->setBounds (left);

        r.removeFromLeft (8);
        auto top = r.removeFromTop (24);
        autoFireToggle.setBounds (top.removeFromRight (130));
        snippetNameLabel.setBounds (top);
        r.removeFromTop (6);
        snippetSections->setBounds (r.removeFromTop (72));
        r.removeFromTop (6);
        stripChooser->setBounds (r);
    }
}

} // namespace livedaw::ui
