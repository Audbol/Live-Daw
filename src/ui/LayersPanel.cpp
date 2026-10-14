#include "ui/LayersPanel.h"
#include "ui/LookAndFeel.h"

namespace livedaw::ui
{

namespace
{
    void paintStripRow (juce::Graphics& g, Strip* s, const juce::String& fallback, int width, int height, bool selected)
    {
        if (selected)
            g.fillAll (Colours::accent.withAlpha (0.35f));

        auto r = juce::Rectangle<int> (width, height).reduced (6, 0);
        g.setColour (s != nullptr ? s->colour : Colours::outline);
        g.fillRect (r.removeFromLeft (4).reduced (0, 4));
        r.removeFromLeft (6);

        g.setFont (juce::FontOptions (13.0f));
        g.setColour (s != nullptr ? Colours::text : Colours::textDim);
        g.drawText (s != nullptr ? s->name : fallback, r, juce::Justification::centredLeft);

        if (s != nullptr)
        {
            g.setColour (Colours::textDim);
            g.setFont (juce::FontOptions (11.0f));
            g.drawText (juce::String (toString (s->getKind())).toUpperCase(), r, juce::Justification::centredRight);
        }
    }

    void styleList (juce::ListBox& l)
    {
        l.setRowHeight (22);
        l.setColour (juce::ListBox::backgroundColourId, juce::Colours::black.withAlpha (0.3f));
    }
} // namespace

//==============================================================================
class LayersPanel::LayerList final : public juce::ListBox, private juce::ListBoxModel
{
public:
    explicit LayerList (LayersPanel& p) : panel (p) { setModel (this); styleList (*this); }

private:
    int getNumRows() override { return static_cast<int> (panel.layers.size()); }

    void paintListBoxItem (int row, juce::Graphics& g, int width, int height, bool rowSelected) override
    {
        if (! juce::isPositiveAndBelow (row, getNumRows()))
            return;

        const auto& layer = panel.layers[(size_t) row];
        if (rowSelected)
            g.fillAll (Colours::accent.withAlpha (0.35f));

        auto r = juce::Rectangle<int> (width, height).reduced (8, 0);
        g.setFont (juce::FontOptions (13.0f, layer.user ? juce::Font::bold : juce::Font::plain));
        g.setColour (layer.user ? Colours::solo : Colours::text);
        g.drawText (layer.name, r, juce::Justification::centredLeft);
        g.setColour (Colours::textDim);
        g.setFont (juce::FontOptions (11.0f));
        g.drawText ((layer.user ? "user, " : "auto, ") + juce::String (layer.strips.size()) + " strips", r,
                    juce::Justification::centredRight);
    }

    void selectedRowsChanged (int row) override
    {
        if (! juce::isPositiveAndBelow (row, getNumRows()))
            return;

        // The edited layer is the one on the mixer.
        if (panel.ctx.show.layers.selectedLayerId != panel.layers[(size_t) row].id)
        {
            panel.ctx.show.layers.selectedLayerId = panel.layers[(size_t) row].id;
            panel.changed();
        }
    }

    LayersPanel& panel;
};

//==============================================================================
class LayersPanel::MemberList final : public juce::ListBox, private juce::ListBoxModel
{
public:
    explicit MemberList (LayersPanel& p) : panel (p) { setModel (this); styleList (*this); }

private:
    int getNumRows() override { return static_cast<int> (panel.currentLayer().strips.size()); }

    void paintListBoxItem (int row, juce::Graphics& g, int width, int height, bool rowSelected) override
    {
        const auto layer = panel.currentLayer();
        if (! juce::isPositiveAndBelow (row, static_cast<int> (layer.strips.size())))
            return;

        const auto& ref = layer.strips[(size_t) row];
        paintStripRow (g, state::findStrip (panel.ctx.engine, ref), ref.toString() + " (removed)", width, height, rowSelected);
    }

    void deleteKeyPressed (int) override { panel.removeButton.triggerClick(); }

    LayersPanel& panel;
};

//==============================================================================
class LayersPanel::BoardList final : public juce::ListBox, private juce::ListBoxModel
{
public:
    explicit BoardList (LayersPanel& p) : panel (p)
    {
        setModel (this);
        styleList (*this);
        setMultipleSelectionEnabled (true);
    }

    std::vector<Strip*> strips;

    void rebuild()
    {
        strips.clear();
        for (auto& s : panel.ctx.engine.getInputs())
            strips.push_back (s.get());
        for (auto& s : panel.ctx.engine.getBuses())
            strips.push_back (s.get());
        strips.push_back (&panel.ctx.engine.getMaster());
        updateContent();
        repaint();
    }

private:
    int getNumRows() override { return static_cast<int> (strips.size()); }

    void paintListBoxItem (int row, juce::Graphics& g, int width, int height, bool rowSelected) override
    {
        if (juce::isPositiveAndBelow (row, getNumRows()))
            paintStripRow (g, strips[(size_t) row], {}, width, height, rowSelected);
    }

    void listBoxItemDoubleClicked (int, const juce::MouseEvent&) override { panel.addSelectedBoardStrips(); }

    LayersPanel& panel;
};

//==============================================================================
LayersPanel::LayersPanel (UiContext& c) : ctx (c)
{
    layerList = std::make_unique<LayerList> (*this);
    memberList = std::make_unique<MemberList> (*this);
    boardList = std::make_unique<BoardList> (*this);
    addAndMakeVisible (*layerList);
    addAndMakeVisible (*memberList);
    addAndMakeVisible (*boardList);

    for (auto* b : { &newButton, &duplicateButton, &deleteButton, &layerUpButton, &layerDownButton,
                     &removeButton, &memberUpButton, &memberDownButton, &addButton })
        addAndMakeVisible (b);

    newButton.setTooltip ("New user layer (starts with the selected channel, if any)");
    newButton.onClick = [this]
    {
        std::vector<state::StripRef> strips;
        if (auto* s = ctx.getSelectedStrip ? ctx.getSelectedStrip() : nullptr)
            strips.push_back (state::StripRef::of (*s));

        const auto id = ctx.show.layers.addUserLayer ("User " + juce::String (ctx.show.layers.getUserLayers().size() + 1),
                                                      std::move (strips));
        ctx.show.layers.selectedLayerId = id;
        changed();
    };

    duplicateButton.setTooltip ("Copy the selected layer into a new user layer");
    duplicateButton.onClick = [this]
    {
        const auto layer = currentLayer();
        ctx.show.layers.selectedLayerId = ctx.show.layers.addUserLayer (layer.name + " copy", layer.strips);
        changed();
    };

    deleteButton.onClick = [this]
    {
        if (auto* layer = currentUserLayer())
        {
            ctx.show.layers.removeUserLayer (layer->id);
            ctx.show.layers.selectedLayerId = "in-1";
            changed();
        }
    };

    layerUpButton.onClick = [this] { if (auto* l = currentUserLayer()) { ctx.show.layers.moveUserLayer (l->id, -1); changed(); } };
    layerDownButton.onClick = [this] { if (auto* l = currentUserLayer()) { ctx.show.layers.moveUserLayer (l->id, 1); changed(); } };

    removeButton.onClick = [this]
    {
        auto* layer = currentUserLayer();
        if (layer == nullptr)
            return;

        const auto rows = memberList->getSelectedRows();
        std::vector<state::StripRef> toRemove;
        for (int i = 0; i < rows.size(); ++i)
            if (juce::isPositiveAndBelow (rows[i], static_cast<int> (layer->strips.size())))
                toRemove.push_back (layer->strips[(size_t) rows[i]]);

        for (auto& ref : toRemove)
            ctx.show.layers.removeFromUserLayer (layer->id, ref);

        memberList->deselectAllRows();
        changed();
    };

    auto moveMember = [this] (int delta)
    {
        auto* layer = currentUserLayer();
        const int row = memberList->getSelectedRow();
        if (layer == nullptr || row < 0)
            return;

        ctx.show.layers.moveWithinUserLayer (layer->id, row, delta);
        memberList->selectRow (juce::jlimit (0, static_cast<int> (layer->strips.size()) - 1, row + delta));
        changed();
    };
    memberUpButton.onClick = [moveMember] { moveMember (-1); };
    memberDownButton.onClick = [moveMember] { moveMember (1); };

    addButton.setTooltip ("Add the selected board strips to the user layer (or double-click)");
    addButton.onClick = [this] { addSelectedBoardStrips(); };

    nameLabel.setEditable (true);
    nameLabel.setFont (juce::FontOptions (14.0f, juce::Font::bold));
    nameLabel.setColour (juce::Label::backgroundColourId, juce::Colours::black.withAlpha (0.25f));
    nameLabel.onTextChange = [this]
    {
        if (auto* layer = currentUserLayer())
        {
            layer->name = nameLabel.getText();
            changed();
        }
    };
    addAndMakeVisible (nameLabel);

    bankLabel.setText ("Bank size", juce::dontSendNotification);
    bankLabel.setColour (juce::Label::textColourId, Colours::textDim);
    bankLabel.setFont (juce::FontOptions (11.0f));
    addAndMakeVisible (bankLabel);

    for (int size : { 8, 12, 16, 24, 32, 48 })
        bankSize.addItem (juce::String (size) + " strips", size);
    bankSize.setTooltip ("How many strips each default layer holds");
    bankSize.onChange = [this]
    {
        if (bankSize.getSelectedId() > 0 && bankSize.getSelectedId() != ctx.show.layers.getBankSize())
        {
            ctx.show.layers.setBankSize (bankSize.getSelectedId());
            changed();
        }
    };
    addAndMakeVisible (bankSize);

    hintLabel.setColour (juce::Label::textColourId, Colours::textDim);
    hintLabel.setFont (juce::FontOptions (12.0f));
    hintLabel.setJustificationType (juce::Justification::topLeft);
    addAndMakeVisible (hintLabel);

    refresh();
}

LayersPanel::~LayersPanel() = default;

Layer LayersPanel::currentLayer() const
{
    return ctx.show.layers.findLayer (ctx.engine, ctx.show.layers.selectedLayerId);
}

Layer* LayersPanel::currentUserLayer() const
{
    return ctx.show.layers.getUserLayer (currentLayer().id);
}

void LayersPanel::changed()
{
    if (ctx.layersChanged)
        ctx.layersChanged(); // rebuilds the mixer and calls refresh()
    else
        refresh();
}

void LayersPanel::addSelectedBoardStrips()
{
    auto* layer = currentUserLayer();
    if (layer == nullptr)
        return;

    const auto rows = boardList->getSelectedRows();
    for (int i = 0; i < rows.size(); ++i)
        if (juce::isPositiveAndBelow (rows[i], static_cast<int> (boardList->strips.size())))
            ctx.show.layers.addToUserLayer (layer->id, state::StripRef::of (*boardList->strips[(size_t) rows[i]]));

    changed();
}

void LayersPanel::refresh()
{
    layers = ctx.show.layers.getAllLayers (ctx.engine);
    const auto current = currentLayer();

    layerList->updateContent();
    for (size_t i = 0; i < layers.size(); ++i)
        if (layers[i].id == current.id)
            layerList->selectRow (static_cast<int> (i), false, true);
    layerList->repaint();

    memberList->updateContent();
    memberList->repaint();
    boardList->rebuild();

    const bool user = current.user;
    nameLabel.setText (current.name, juce::dontSendNotification);
    nameLabel.setEditable (user);
    for (auto* b : { &deleteButton, &layerUpButton, &layerDownButton, &removeButton, &memberUpButton,
                     &memberDownButton, &addButton })
        b->setEnabled (user);
    boardList->setEnabled (user);

    bankSize.setSelectedId (ctx.show.layers.getBankSize(), juce::dontSendNotification);

    hintLabel.setText (user ? "User layer: add any channels, buses or the master from the board, in any order. "
                              "Double-click a board strip to add it."
                            : "Default layers follow the console automatically. "
                              "Duplicate one, or press + New, to make a user layer.",
                       juce::dontSendNotification);
}

void LayersPanel::paint (juce::Graphics& g)
{
    g.fillAll (Colours::background.darker (0.2f));

    for (auto [area, title] : { std::pair { layersArea, "LAYERS" }, std::pair { membersArea, "STRIPS ON THIS LAYER" },
                                std::pair { boardArea, "BOARD" } })
    {
        g.setColour (Colours::panel);
        g.fillRoundedRectangle (area.toFloat().reduced (2.0f), 5.0f);
        g.setColour (Colours::textDim);
        g.setFont (juce::FontOptions (12.0f, juce::Font::bold));
        g.drawText (title, area.reduced (10, 6).removeFromTop (14), juce::Justification::centredLeft);
    }
}

void LayersPanel::resized()
{
    auto b = getLocalBounds().reduced (2);
    layersArea = b.removeFromLeft (juce::jmax (300, b.getWidth() * 32 / 100));
    boardArea = b.removeFromRight (juce::jmax (260, b.getWidth() * 40 / 100));
    membersArea = b;

    {
        auto r = layersArea.reduced (10).withTrimmedTop (18);
        auto bank = r.removeFromBottom (24);
        bankLabel.setBounds (bank.removeFromLeft (70));
        bankSize.setBounds (bank.removeFromLeft (120));
        r.removeFromBottom (4);
        auto buttons = r.removeFromBottom (26);
        const int w = buttons.getWidth() / 5;
        for (auto* button : { &newButton, &duplicateButton, &deleteButton, &layerUpButton, &layerDownButton })
            button->setBounds (buttons.removeFromLeft (w).reduced (1, 0));
        r.removeFromBottom (4);
        layerList->setBounds (r);
    }

    {
        auto r = membersArea.reduced (10).withTrimmedTop (18);
        nameLabel.setBounds (r.removeFromTop (24));
        r.removeFromTop (4);
        hintLabel.setBounds (r.removeFromBottom (34));
        auto buttons = r.removeFromBottom (26);
        const int w = buttons.getWidth() / 3;
        for (auto* button : { &removeButton, &memberUpButton, &memberDownButton })
            button->setBounds (buttons.removeFromLeft (w).reduced (1, 0));
        r.removeFromBottom (4);
        memberList->setBounds (r);
    }

    {
        auto r = boardArea.reduced (10).withTrimmedTop (18);
        addButton.setBounds (r.removeFromBottom (26).removeFromLeft (120));
        r.removeFromBottom (4);
        boardList->setBounds (r);
    }
}

} // namespace livedaw::ui
