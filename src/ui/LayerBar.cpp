#include "ui/LayerBar.h"
#include "ui/LookAndFeel.h"

namespace livedaw::ui
{

LayerBar::LayerBar (UiContext& c) : ctx (c)
{
    editButton.setTooltip ("Create and edit user layers");
    editButton.onClick = [this] { if (onEditLayers) onEditLayers(); };
    addAndMakeVisible (editButton);

    viewport.setViewedComponent (&list, false);
    viewport.setScrollBarsShown (true, false);
    viewport.setScrollBarThickness (8);
    addAndMakeVisible (viewport);

    userCaption.setText ("USER", juce::dontSendNotification);
    userCaption.setFont (juce::FontOptions (10.0f, juce::Font::bold));
    userCaption.setColour (juce::Label::textColourId, Colours::textDim);
    list.addChildComponent (userCaption);

    refresh();
}

LayerBar::~LayerBar()
{
    viewport.setViewedComponent (nullptr, false);
}

void LayerBar::refresh()
{
    buttons.clear();

    const auto layers = ctx.show.layers.getAllLayers (ctx.engine);
    const auto current = ctx.show.layers.findLayer (ctx.engine, ctx.show.layers.selectedLayerId).id;
    numDefault = 0;
    selectedIndex = -1;

    for (size_t i = 0; i < layers.size(); ++i)
    {
        const auto& layer = layers[i];
        numDefault += layer.user ? 0 : 1;

        auto* b = buttons.add (new juce::TextButton (layer.name));
        b->setClickingTogglesState (false);
        b->setToggleState (layer.id == current, juce::dontSendNotification);
        if (layer.id == current)
            selectedIndex = static_cast<int> (i);
        b->setColour (juce::TextButton::buttonOnColourId, layer.user ? Colours::solo : Colours::accent);

        juce::String tip = layer.name + ": " + juce::String (layer.strips.size()) + " strips";
        if (i < 9)
            tip << "  (Ctrl+" << juce::String ((int) i + 1) << ")";
        b->setTooltip (tip);

        b->onClick = [this, id = layer.id]
        {
            ctx.show.layers.selectedLayerId = id;
            notifyLayersChangedAsync (ctx); // the rebuild deletes this button
        };

        list.addAndMakeVisible (b);
    }

    resized();

    // Keep the selected layer in view.
    if (auto* b = buttons[selectedIndex])
    {
        const auto area = viewport.getViewArea();
        if (b->getY() < area.getY() || b->getBottom() > area.getBottom())
            viewport.setViewPosition (0, juce::jmax (0, b->getY() - area.getHeight() / 2));
    }

    repaint();
}

void LayerBar::paint (juce::Graphics& g)
{
    g.setColour (Colours::panel);
    g.fillRoundedRectangle (getLocalBounds().toFloat().reduced (1.0f), 4.0f);

    g.setColour (Colours::textDim);
    g.setFont (juce::FontOptions (10.0f, juce::Font::bold));
    g.drawText ("LAYERS", getLocalBounds().reduced (6, 4).removeFromTop (12), juce::Justification::centredLeft);
}

void LayerBar::resized()
{
    auto b = getLocalBounds().reduced (5);
    b.removeFromTop (16);
    editButton.setBounds (b.removeFromBottom (24));
    b.removeFromBottom (6);
    viewport.setBounds (b);
    layoutButtons();
}

void LayerBar::layoutButtons()
{
    constexpr int buttonHeight = 24, captionHeight = 16;
    const int width = viewport.getWidth() - viewport.getScrollBarThickness() - 2;
    int y = 0;

    userCaption.setVisible (numDefault < buttons.size());

    for (int i = 0; i < buttons.size(); ++i)
    {
        if (i == numDefault && i > 0)
        {
            y += 4;
            userCaption.setBounds (0, y, width, captionHeight);
            y += captionHeight;
        }

        buttons[i]->setBounds (0, y + 1, width, buttonHeight - 2);
        y += buttonHeight;
    }

    list.setSize (juce::jmax (1, width), y);
}

} // namespace livedaw::ui
