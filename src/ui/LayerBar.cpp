#include "ui/LayerBar.h"
#include "ui/LookAndFeel.h"

namespace livedaw::ui
{

LayerBar::LayerBar (UiContext& c) : ctx (c)
{
    editButton.setTooltip ("Create and edit user layers");
    editButton.onClick = [this] { if (onEditLayers) onEditLayers(); };
    addAndMakeVisible (editButton);
    refresh();
}

void LayerBar::refresh()
{
    buttons.clear();

    const auto layers = ctx.show.layers.getAllLayers (ctx.engine);
    const auto current = ctx.show.layers.findLayer (ctx.engine, ctx.show.layers.selectedLayerId).id;
    numDefault = 0;

    for (size_t i = 0; i < layers.size(); ++i)
    {
        const auto& layer = layers[i];
        numDefault += layer.user ? 0 : 1;

        auto* b = buttons.add (new juce::TextButton (layer.name));
        b->setClickingTogglesState (false);
        b->setToggleState (layer.id == current, juce::dontSendNotification);
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

        addAndMakeVisible (b);
    }

    resized();
    repaint();
}

void LayerBar::paint (juce::Graphics& g)
{
    g.setColour (Colours::panel);
    g.fillRoundedRectangle (getLocalBounds().toFloat().reduced (1.0f), 4.0f);

    g.setColour (Colours::textDim);
    g.setFont (juce::FontOptions (10.0f, juce::Font::bold));
    g.drawText ("LAYERS", getLocalBounds().reduced (6, 4).removeFromTop (12), juce::Justification::centredLeft);

    if (numDefault < buttons.size())
    {
        auto* firstUser = buttons[numDefault];
        g.drawText ("USER", juce::Rectangle<int> (6, firstUser->getY() - 15, getWidth() - 12, 12),
                    juce::Justification::centredLeft);
    }
}

void LayerBar::resized()
{
    auto b = getLocalBounds().reduced (5);
    b.removeFromTop (16);
    editButton.setBounds (b.removeFromBottom (24));
    b.removeFromBottom (6);

    const int h = juce::jlimit (18, 30, b.getHeight() / juce::jmax (1, buttons.size() + 1));

    for (int i = 0; i < buttons.size(); ++i)
    {
        if (i == numDefault && i > 0)
            b.removeFromTop (18); // room for the "USER" caption

        buttons[i]->setBounds (b.removeFromTop (h).reduced (0, 1));
    }
}

} // namespace livedaw::ui
