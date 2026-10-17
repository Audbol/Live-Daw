#include "ui/ShowPanel.h"

namespace livedaw::ui
{

namespace
{
    /** "N PARS": a snippet's size, strips x sections (the closest thing to the console's parameter count). */
    int parameterCount (const Show::Snippet& s)
    {
        int sections = 0;
        for (int i = 0; i < state::kNumSections; ++i)
            sections += (s.sections & state::sectionAt (i)) != 0 ? 1 : 0;
        return (int) s.strips.size() * sections;
    }
} // namespace

//==============================================================================
class ShowPanel::List final : public juce::ListBox, private juce::ListBoxModel
{
public:
    explicit List (ShowPanel& p) : panel (p)
    {
        setModel (this);
        setColour (backgroundColourId, juce::Colours::black);
        setColour (outlineColourId, juce::Colours::transparentBlack);
        getVerticalScrollBar().setColour (juce::ScrollBar::thumbColourId, Dark::key);
        getVerticalScrollBar().setColour (juce::ScrollBar::backgroundColourId, Dark::body);
    }

    ~List() override { setModel (nullptr); }

    std::vector<Item> rows;
    int filter = 0;

    void rebuild()
    {
        rows.clear();
        auto& show = panel.ctx.show;
        for (int i = 0; i < show.getNumScenes(); ++i)
        {
            rows.push_back ({ i, -1 });
            if (const auto scene = show.getScene (i))
                for (int j = 0; j < scene->numSnippets; ++j)
                    rows.push_back ({ i, j });
        }
        updateContent();
        repaint();
    }

    int rowOf (Item item) const
    {
        for (size_t i = 0; i < rows.size(); ++i)
            if (rows[i] == item)
                return (int) i;
        return -1;
    }

    Item itemAt (int row) const { return juce::isPositiveAndBelow (row, (int) rows.size()) ? rows[(size_t) row] : Item {}; }

private:
    int getNumRows() override { return (int) rows.size(); }

    /** Does a later snippet of the same scene (after row `row`) fire with it? */
    bool linkContinues (int row) const
    {
        const auto item = itemAt (row);
        for (int r = row + 1; r < (int) rows.size() && rows[(size_t) r].scene == item.scene; ++r)
            if (const auto sn = panel.ctx.show.getSnippet (item.scene, rows[(size_t) r].snippet); sn && sn->fireWithScene)
                return true;
        return false;
    }

    void paintListBoxItem (int row, juce::Graphics& g, int width, int height, bool rowSelected) override
    {
        const auto item = itemAt (row);
        if (! item.isValid())
            return;
        auto& show = panel.ctx.show;
        const auto scene = show.getScene (item.scene);
        if (! scene)
            return;
        const auto snippet = item.isSnippet() ? show.getSnippet (item.scene, item.snippet) : std::nullopt;
        if (item.isSnippet() && ! snippet)
            return;

        const bool normal = filter == 0 || (filter == 1 && ! item.isSnippet()) || (filter == 2 && item.isSnippet());
        auto r = juce::Rectangle<int> (width, height).toFloat();
        g.setColour (juce::Colour (0xff262626));
        g.fillRect (r.removeFromBottom (1.0f));
        if (rowSelected)
        {
            g.setColour (Dark::selectedRow);
            g.fillRect (r.reduced (1.0f));
            g.setColour (Dark::selectedOutline);
            g.drawRect (r.reduced (1.0f), 2.0f);
        }

        auto t = r.reduced (8.0f, 2.0f);
        auto bars = t.removeFromRight (12.0f); // the LINK bars, right of the chips
        if (item.isSnippet())
            t.removeFromLeft (24.0f);

        // The type chip with the scene number under it.
        const float chipW = juce::jmin (46.0f, t.getWidth() * 0.16f), chipH = juce::jlimit (12.0f, 18.0f, t.getHeight() * 0.4f);
        auto chipColumn = t.removeFromRight (chipW + 2.0f);
        const auto chipColour = item.isSnippet() ? FileColours::snip : FileColours::snap;
        auto chip = chipColumn.withHeight (chipH).withY (t.getY() + 2.0f);
        drawChip (g, chip, item.isSnippet() ? "SNIP" : "SNAP", chipColour.withMultipliedAlpha (normal ? 1.0f : 0.45f), juce::Colours::black);
        g.setColour (Dark::textDim);
        g.setFont (juce::FontOptions (juce::jlimit (9.0f, 13.0f, t.getHeight() * 0.3f), juce::Font::bold));
        const juce::String number = juce::String (item.scene + 1) + (item.isSnippet() ? "." + juce::String (item.snippet + 1) : juce::String());
        g.drawText (number, chipColumn.withTop (chip.getBottom()).toNearestInt(), juce::Justification::centredRight);

        // Snippets that fire with their scene: a green bar down the chip column (the console's LINK).
        const float barX = bars.getX() + 1.0f;
        g.setColour (FileColours::wav);
        if (! item.isSnippet() && linkContinues (row))
            g.fillRect (barX, r.getCentreY(), 4.0f, r.getBottom() + 1.0f - r.getCentreY());
        else if (item.isSnippet())
        {
            const bool linked = snippet->fireWithScene;
            const bool continues = linkContinues (row);
            if (linked || continues)
                g.fillRect (barX, r.getY(), 4.0f, (continues ? r.getBottom() + 1.0f : r.getCentreY()) - r.getY());
        }

        // A scene that LINKs to the scene below: a green bar joins them, and the
        // linked scene is indented.
        const auto previous = item.scene > 0 ? show.getScene (item.scene - 1) : std::nullopt;
        const bool linkedFromAbove = ! item.isSnippet() && previous && previous->linkNext;
        const float sceneBarX = bars.getX() + 7.0f;
        if (scene->linkNext && item.scene < show.getNumScenes() - 1)
            g.fillRect (sceneBarX, item.isSnippet() ? r.getY() : r.getCentreY(), 4.0f, item.isSnippet() ? r.getHeight() + 1.0f : r.getBottom() + 1.0f - r.getCentreY());
        if (linkedFromAbove)
        {
            g.fillRect (sceneBarX, r.getY(), 4.0f, r.getCentreY() - r.getY());
            t.removeFromLeft (16.0f);
        }

        if (item.isSnippet())
        {
            g.setColour (Dark::textDim.withMultipliedAlpha (normal ? 1.0f : 0.5f));
            g.setFont (juce::FontOptions (juce::jlimit (9.0f, 12.0f, t.getHeight() * 0.28f), juce::Font::bold));
            g.drawText (juce::String (parameterCount (*snippet)) + " PARS", t.removeFromRight (60.0f).withHeight (chipH + 4.0f).toNearestInt(),
                        juce::Justification::centredRight);
        }
        else if (scene->fadeSeconds > 0.0)
        {
            g.setColour (Dark::textDim.withMultipliedAlpha (normal ? 1.0f : 0.5f));
            g.setFont (juce::FontOptions (juce::jlimit (9.0f, 12.0f, t.getHeight() * 0.28f), juce::Font::bold));
            g.drawText ("FADE " + juce::String (scene->fadeSeconds, 1) + " s", t.removeFromRight (70.0f).withHeight (chipH + 4.0f).toNearestInt(),
                        juce::Justification::centredRight);
        }

        // The active (last recalled) scene.
        if (! item.isSnippet() && item.scene == show.getCurrentSceneIndex())
        {
            auto marker = t.removeFromLeft (16.0f);
            drawIcon (g, Icon::Play, marker.withSizeKeepingCentre (12.0f, 12.0f), FileColours::wav);
            t.removeFromLeft (4.0f);
        }

        if (! item.isSnippet())
        {
            // The scene tag (#1..#128) in a small blue chip at the top left.
            if (scene->tag > 0)
            {
                drawChip (g, juce::Rectangle<float> (t.getX(), t.getY() + 1.0f, 30.0f, juce::jlimit (10.0f, 14.0f, t.getHeight() * 0.32f)),
                          "#" + juce::String (scene->tag), Dark::choice.withMultipliedAlpha (normal ? 1.0f : 0.5f), juce::Colours::black);
                t.removeFromTop (juce::jlimit (6.0f, 10.0f, t.getHeight() * 0.22f));
            }
            // A skipped scene: the SKIP mark, and the name dimmed.
            if (scene->skip)
            {
                drawIcon (g, Icon::Skip, t.removeFromLeft (16.0f).withSizeKeepingCentre (12.0f, 12.0f), Dark::on.withMultipliedAlpha (normal ? 1.0f : 0.5f));
                t.removeFromLeft (4.0f);
            }
        }

        const bool dim = ! normal || (! item.isSnippet() && scene->skip);
        g.setColour (dim ? Dark::textDim.withAlpha (0.6f) : Dark::text);
        g.setFont (juce::FontOptions (juce::jlimit (12.0f, 24.0f, t.getHeight() * 0.62f), juce::Font::bold));
        g.drawFittedText ((item.isSnippet() ? snippet->name : scene->name).toUpperCase(), t.toNearestInt(), juce::Justification::centredLeft, 1, 0.8f);
    }

    void selectedRowsChanged (int) override
    {
        panel.updateKeys();
        if (panel.onSelectionChanged)
            panel.onSelectionChanged (panel.getSelected());
    }

    void listBoxItemDoubleClicked (int row, const juce::MouseEvent&) override
    {
        if (panel.onActivated)
            panel.onActivated (itemAt (row));
    }

    ShowPanel& panel;
};

//==============================================================================
ShowPanel::ShowPanel (UiContext& c) : ctx (c)
{
    list = std::make_unique<List> (*this);
    addAndMakeVisible (*list);

    // The file browser (internal storage, USB stick) doesn't exist yet: the
    // list always shows the open show.
    intKey.setEnabled (false);
    usbKey.setEnabled (false);
    showKey.litColour = FileColours::show;
    showKey.isLit = [] { return true; };
    showKey.caption = [this] { return juce::String (ctx.show.getNumScenes()) + " SCNS"; };
    skipKey.litColour = Dark::on;
    skipKey.isLit = [this]
    {
        const auto i = getSelected();
        const auto sc = i.isValid() && ! i.isSnippet() ? ctx.show.getScene (i.scene) : std::nullopt;
        return sc && sc->skip;
    };
    skipKey.onClick = [this] { skipSelected(); };

    removeKey.onClick = [this] { removeSelected(); };
    renameKey.onClick = [this] { renameSelected(); };
    linkKey.onClick = [this] { linkSelected(); };
    upKey.onClick = [this] { moveSelected (-1); };
    downKey.onClick = [this] { moveSelected (1); };

    for (auto* k : { &intKey, &usbKey, &showKey, &removeKey, &renameKey, &skipKey, &linkKey, &upKey, &downKey })
        addAndMakeVisible (k);
    refresh();
}

ShowPanel::~ShowPanel() = default;

ShowPanel::Item ShowPanel::getSelected() const
{
    return list->itemAt (list->getSelectedRow());
}

void ShowPanel::select (Item item)
{
    const int row = list->rowOf (item);
    if (row >= 0)
    {
        list->selectRow (row);
        list->scrollToEnsureRowIsOnscreen (row);
    }
    else
        list->deselectAllRows();
}

void ShowPanel::setTypeFilter (int filter)
{
    list->filter = filter;
    list->repaint();
}

void ShowPanel::refresh()
{
    const auto selected = getSelected();
    list->rebuild();
    int row = list->rowOf (selected);
    if (row < 0 && selected.isValid())
        row = list->rowOf ({ selected.scene, -1 }); // a removed snippet: its scene
    if (row < 0)
        row = list->rowOf ({ juce::jmin (selected.scene, ctx.show.getNumScenes() - 1), -1 });
    if (row < 0 && ctx.show.getCurrentSceneIndex() >= 0)
        row = list->rowOf ({ ctx.show.getCurrentSceneIndex(), -1 });
    if (row >= 0 && row != list->getSelectedRow())
        list->selectRow (row);
    updateKeys();
    repaint();
}

void ShowPanel::updateKeys()
{
    const auto item = getSelected();
    removeKey.setEnabled (item.isValid());
    renameKey.setEnabled (item.isValid());
    // SKIP: scenes. LINK: a scene to the scene below it, a snippet to its scene (fire with it).
    skipKey.setEnabled (item.isValid() && ! item.isSnippet());
    linkKey.setEnabled (item.isSnippet() || (item.isValid() && item.scene < ctx.show.getNumScenes() - 1));
    linkKey.isLit = [this]
    {
        const auto i = getSelected();
        if (i.isSnippet())
        {
            const auto sn = ctx.show.getSnippet (i.scene, i.snippet);
            return sn && sn->fireWithScene;
        }
        const auto sc = ctx.show.getScene (i.scene);
        return sc && sc->linkNext;
    };
    linkKey.litColour = FileColours::wav;
    upKey.setEnabled (item.isValid() && ! item.isSnippet() && item.scene > 0);
    downKey.setEnabled (item.isValid() && ! item.isSnippet() && item.scene < ctx.show.getNumScenes() - 1);
}

juce::Component& ShowPanel::popupHost()
{
    // The windows cover the whole LIBRARY screen, not just the list.
    auto* parent = getParentComponent();
    return parent != nullptr ? *parent : *this;
}

void ShowPanel::removeSelected()
{
    const auto item = getSelected();
    if (! item.isValid())
        return;
    const auto scene = ctx.show.getScene (item.scene);
    const auto snippet = item.isSnippet() ? ctx.show.getSnippet (item.scene, item.snippet) : std::nullopt;
    if (! scene || (item.isSnippet() && ! snippet))
        return;
    const auto name = item.isSnippet() ? snippet->name : scene->name;
    juce::Component::SafePointer<ShowPanel> safe (this);
    popups::confirm (popupHost(), "REMOVE", "Remove \"" + name + "\"" + (item.isSnippet() ? juce::String ("?") : juce::String (" and its snippets?")), "REMOVE",
                     [safe, item]
                     {
                         if (safe == nullptr)
                             return;
                         if (item.isSnippet())
                             safe->ctx.show.removeSnippet (item.scene, item.snippet);
                         else
                             safe->ctx.show.removeScene (item.scene);
                         safe->refresh();
                     },
                     true);
}

void ShowPanel::renameSelected()
{
    const auto item = getSelected();
    const auto scene = ctx.show.getScene (item.scene);
    const auto snippet = item.isSnippet() ? ctx.show.getSnippet (item.scene, item.snippet) : std::nullopt;
    if (! scene || (item.isSnippet() && ! snippet))
        return;
    popups::TextOptions options;
    options.maxLength = 32;
    juce::Component::SafePointer<ShowPanel> safe (this);
    popups::editText (popupHost(), item.isSnippet() ? "SNIPPET NAME" : "SCENE NAME", item.isSnippet() ? snippet->name : scene->name,
                      [safe, item] (const juce::String& text)
                      {
                          const auto name = text.trim();
                          if (safe == nullptr || name.isEmpty())
                              return;
                          if (item.isSnippet())
                              safe->ctx.show.setSnippetName (item.scene, item.snippet, name);
                          else
                              safe->ctx.show.setSceneName (item.scene, name);
                          safe->refresh();
                      },
                      options);
}

void ShowPanel::moveSelected (int delta)
{
    const auto item = getSelected();
    if (! item.isValid() || item.isSnippet())
        return;
    const int target = item.scene + delta;
    if (target < 0 || target >= ctx.show.getNumScenes())
        return;
    ctx.show.moveScene (item.scene, target);
    list->rebuild();
    select ({ target, -1 });
}

void ShowPanel::linkSelected()
{
    const auto item = getSelected();
    if (item.isSnippet())
    {
        if (const auto sn = ctx.show.getSnippet (item.scene, item.snippet))
            ctx.show.setSnippetFireWithScene (item.scene, item.snippet, ! sn->fireWithScene);
    }
    else if (const auto sc = ctx.show.getScene (item.scene))
        ctx.show.setSceneLink (item.scene, ! sc->linkNext);
    list->repaint();
    linkKey.repaint();
}

void ShowPanel::skipSelected()
{
    const auto item = getSelected();
    if (item.isSnippet())
        return;
    if (const auto sc = ctx.show.getScene (item.scene))
        ctx.show.setSceneSkip (item.scene, ! sc->skip);
    list->repaint();
    skipKey.repaint();
}

void ShowPanel::paint (juce::Graphics& g)
{
    g.fillAll (juce::Colours::black);
    g.setColour (Dark::header);
    g.fillRect (toolbar);
    g.setColour (Dark::body);
    g.fillRect (listArea);
}

void ShowPanel::resized()
{
    auto b = getLocalBounds();
    toolbar = b.removeFromTop (juce::jlimit (36, 52, getHeight() * 11 / 100));
    b.removeFromTop (2);
    listArea = b;
    list->setRowHeight (juce::jlimit (28, 44, getHeight() / 12));
    list->setBounds (listArea);

    auto t = toolbar.reduced (2, 2);
    const int w = juce::jmin (t.getHeight() + 14, t.getWidth() / 10);
    for (auto* k : { &intKey, &usbKey, &showKey })
        k->setBounds (t.removeFromLeft (w));
    t.removeFromLeft (juce::jmax (4, (t.getWidth() - 6 * w) / 2));
    for (auto* k : { &removeKey, &renameKey, &skipKey, &linkKey, &upKey, &downKey })
        k->setBounds (t.removeFromLeft (w));
}

} // namespace livedaw::ui
