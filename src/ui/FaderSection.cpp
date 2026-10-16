#include "ui/FaderSection.h"

namespace livedaw::ui
{

FaderSection::FaderSection (UiContext& c, LayerSet::Section s, const juce::String& t) : ctx (c), section (s), title (t)
{
    bankViewport.setViewedComponent (&bankContent, false);
    bankViewport.setScrollBarsShown (true, false);
    bankViewport.setScrollBarThickness (6);
    addAndMakeVisible (bankViewport);

    for (auto* b : { &pageLeft, &pageRight })
        addAndMakeVisible (b);
    pageLeft.onClick = [this] { if (page > 0) { --page; refreshFaders(); } };
    pageRight.onClick = [this] { ++page; refreshFaders(); };
    pageLabel.setJustificationType (juce::Justification::centred);
    pageLabel.setFont (juce::FontOptions (10.0f));
    pageLabel.setColour (juce::Label::textColourId, Colours::textDim);
    addAndMakeVisible (pageLabel);

    if (section == LayerSet::Center)
    {
        sofButton.setClickingTogglesState (true);
        sofButton.setColour (juce::TextButton::buttonOnColourId, Colours::solo);
        sofButton.setTooltip ("Sends on Fader: select a bus to set every input's send to it on the input faders, "
                              "or select an input to set its sends on the bus faders");
        sofButton.onClick = [this]
        {
            ctx.sendsOnFader = sofButton.getToggleState();
            if (ctx.sofChanged)
                ctx.sofChanged();
        };
        addAndMakeVisible (sofButton);
    }
}

void FaderSection::refresh()
{
    bankButtons.clear();
    const auto layers = ctx.show.layers.getAllLayers (ctx.engine, section);

    for (auto& l : layers)
    {
        auto* b = bankButtons.add (new juce::TextButton (l.name));
        b->setClickingTogglesState (false);
        b->setColour (juce::TextButton::buttonOnColourId, l.user ? Colours::block : Colours::accent);
        b->setColour (juce::TextButton::buttonColourId, Colours::panelLight.darker (0.25f));
        b->setTooltip (l.name + (l.user ? " (user bank)" : ""));
        const auto id = l.id;
        b->getProperties().set ("bank", id);
        b->onClick = [this, id] { selectBank (id); };
        bankContent.addAndMakeVisible (b);
    }

    layer = ctx.show.layers.getSelectedLayer (ctx.engine, section);
    for (auto* b : bankButtons)
        b->setToggleState (b->getProperties()["bank"].toString() == layer.id, juce::dontSendNotification);

    layoutBanks();
    refreshFaders();
}

void FaderSection::selectBank (const juce::String& id)
{
    ctx.show.layers.setSelectedLayerId (section, id);
    page = 0;
    layer = ctx.show.layers.getSelectedLayer (ctx.engine, section);
    for (auto* b : bankButtons)
        b->setToggleState (b->getProperties()["bank"].toString() == layer.id, juce::dontSendNotification);
    refreshFaders();
}

void FaderSection::refreshFaders()
{
    sofButton.setToggleState (ctx.sendsOnFader, juce::dontSendNotification);

    const int n = juce::jmax (1, ctx.show.layers.getEffectiveWidth (section));
    if (strips.size() != n)
    {
        strips.clear();
        for (int i = 0; i < n; ++i)
            addAndMakeVisible (strips.add (new FaderStrip (ctx)));
        resized();
    }

    const int total = (int) layer.faders.size();
    const int pages = juce::jmax (1, (total + n - 1) / n);
    page = juce::jlimit (0, pages - 1, page);
    pageLeft.setEnabled (page > 0);
    pageRight.setEnabled (page < pages - 1);
    pageLabel.setText (pages > 1 ? juce::String (page + 1) + "/" + juce::String (pages) : juce::String(), juce::dontSendNotification);

    for (int i = 0; i < n; ++i)
    {
        const int index = page * n + i;
        strips[i]->setTarget (index < total ? std::optional<state::FaderRef> (layer.faders[(size_t) index]) : std::nullopt);
    }
}

void FaderSection::paint (juce::Graphics& g)
{
    g.setColour (Colours::background);
    g.fillAll();
    auto bank = getLocalBounds().removeFromLeft (kBankColumnWidth);
    g.setColour (Colours::panel.darker (0.2f));
    g.fillRect (bank.reduced (1, 0));
    g.setColour (Colours::textDim);
    g.setFont (juce::FontOptions (11.0f, juce::Font::bold));
    g.drawText (title, bank.removeFromTop (20).reduced (6, 0), juce::Justification::centredLeft);
    g.setColour (Colours::outline);
    g.fillRect (getWidth() - 1, 0, 1, getHeight());
}

void FaderSection::layoutBanks()
{
    const int w = bankViewport.getWidth() - (bankViewport.isVerticalScrollBarShown() ? bankViewport.getScrollBarThickness() : 0);
    const int h = 26;
    for (int i = 0; i < bankButtons.size(); ++i)
        bankButtons[i]->setBounds (2, i * h, juce::jmax (10, w - 4), h - 3);
    bankContent.setSize (juce::jmax (10, w), bankButtons.size() * h);
}

void FaderSection::resized()
{
    auto b = getLocalBounds();
    auto bank = b.removeFromLeft (kBankColumnWidth).reduced (4, 0);
    bank.removeFromTop (20);

    auto bottom = bank.removeFromBottom (section == LayerSet::Center ? 64 : 34);
    if (section == LayerSet::Center)
        sofButton.setBounds (bottom.removeFromBottom (28).reduced (0, 2));
    auto pager = bottom.removeFromTop (28);
    pageLeft.setBounds (pager.removeFromLeft (26).reduced (0, 2));
    pageRight.setBounds (pager.removeFromRight (26).reduced (0, 2));
    pageLabel.setBounds (pager);

    bankViewport.setBounds (bank);
    layoutBanks();

    const int n = strips.size();
    if (n == 0)
        return;
    const int w = b.getWidth() / n;
    for (int i = 0; i < n; ++i)
        strips[i]->setBounds (b.getX() + i * w, b.getY(), w, b.getHeight());
}

} // namespace livedaw::ui
