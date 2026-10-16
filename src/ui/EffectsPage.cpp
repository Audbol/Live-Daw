#include "ui/Pages.h"

namespace livedaw::ui
{

class EffectsPage::Tile final : public juce::Component, private juce::Timer
{
public:
    Tile (UiContext& c, int s, EffectsPage& p) : ctx (c), slot (s), page (p)
    {
        bypass.setClickingTogglesState (true);
        bypass.setColour (juce::TextButton::buttonOnColourId, Colours::mute);
        bypass.onClick = [this] { ctx.engine.getFx().params (slot).bypass = bypass.getToggleState(); };
        addAndMakeVisible (bypass);
        mix.setCompact (true);
        mix.setBinding (bindLinear ("MIX", ctx.engine.getFx().params (slot).mix, 0.0f, 100.0f, "%", 0, 100.0f));
        addAndMakeVisible (mix);
        startTimerHz (5);
        timerCallback();
    }

    void paint (juce::Graphics& g) override
    {
        auto& fx = ctx.engine.getFx();
        auto b = getLocalBounds().toFloat().reduced (3.0f);
        const bool loaded = fx.isLoaded (slot);
        const bool sel = page.selected == slot;
        g.setColour (Colours::screenPanel);
        g.fillRoundedRectangle (b, 5.0f);
        g.setColour (sel ? Colours::accent : (loaded ? Colours::block.withAlpha (0.6f) : Colours::screenLine));
        g.drawRoundedRectangle (b, 5.0f, sel ? 2.0f : 1.0f);

        auto t = getLocalBounds().reduced (10, 8);
        auto top = t.removeFromTop (16);
        g.setColour (Colours::accent);
        g.setFont (juce::FontOptions (13.0f, juce::Font::bold));
        g.drawText ("FX " + juce::String (slot + 1), top, juce::Justification::centredLeft);
        g.setColour (Colours::textDim);
        g.setFont (juce::FontOptions (9.5f));
        g.drawText (slot < 8 ? "PREMIUM" : "STANDARD", top, juce::Justification::centredRight);

        g.setColour (loaded ? Colours::text : Colours::textDim);
        g.setFont (juce::FontOptions (15.0f, juce::Font::bold));
        auto* plugin = fx.getPlugin (slot);
        g.drawFittedText (plugin != nullptr ? plugin->getName() : juce::String ("EMPTY - click to load"), t.removeFromTop (38),
                          juce::Justification::centredLeft, 2);

        g.setFont (juce::FontOptions (11.0f));
        juce::String info;
        if (loaded)
        {
            info << "users " << fx.getNumUsers (slot) << "   load " << juce::roundToInt (fx.getLoad (slot) * 100.0f) << "%";
            if (plugin != nullptr && plugin->getLatencySamples() > 0)
                info << "   lat " << plugin->getLatencySamples();
        }
        g.drawText (info, t.removeFromTop (16), juce::Justification::centredLeft);

        if (fx.isFaulted (slot) || fx.isDoubleAssigned (slot))
        {
            g.setColour (Colours::mute);
            g.setFont (juce::FontOptions (11.0f, juce::Font::bold));
            g.drawText (fx.isFaulted (slot) ? "FAULT - AUTO BYPASSED" : "DOUBLE ASSIGNED", t.removeFromTop (16), juce::Justification::centredLeft);
        }
    }

    void resized() override
    {
        auto b = getLocalBounds().reduced (10, 8);
        auto bottom = b.removeFromBottom (juce::jmin (64, b.getHeight() / 2));
        mix.setBounds (bottom.removeFromRight (70));
        bypass.setBounds (bottom.removeFromBottom (24).removeFromLeft (70));
    }

    void mouseUp (const juce::MouseEvent& e) override
    {
        page.selected = slot;
        page.notifyKnobsChanged();
        page.repaint();
        for (auto* t : page.tiles)
            t->repaint();
        if (e.mods.isPopupMenu())
            showFxSlotMenu (ctx, slot, this);
        else
            clickFxSlot (ctx, slot, this);
    }

private:
    void timerCallback() override
    {
        bypass.setToggleState (ctx.engine.getFx().params (slot).bypass.load(), juce::dontSendNotification);
        repaint();
    }

    UiContext& ctx;
    int slot;
    EffectsPage& page;
    juce::TextButton bypass { "BYPASS" };
    DisplayKnob mix;
};

EffectsPage::EffectsPage (UiContext& c) : ctx (c)
{
    for (int i = 0; i < kNumFx; ++i)
        addAndMakeVisible (tiles.add (new Tile (ctx, i, *this)));
}

EffectsPage::~EffectsPage() = default;

std::vector<KnobBinding> EffectsPage::getKnobs()
{
    std::vector<KnobBinding> k;
    const int row = selected / 4 * 4;
    for (int i = 0; i < 4; ++i)
        k.push_back (bindLinear ("FX" + juce::String (row + i + 1) + " MIX", ctx.engine.getFx().params (row + i).mix, 0.0f, 100.0f, "%", 0, 100.0f));
    return k;
}

void EffectsPage::paint (juce::Graphics& g)
{
    g.fillAll (Colours::screen);
}

void EffectsPage::resized()
{
    auto b = getLocalBounds().reduced (4);
    const int w = b.getWidth() / 4, h = b.getHeight() / 4;
    for (int i = 0; i < tiles.size(); ++i)
        tiles[i]->setBounds (b.getX() + (i % 4) * w, b.getY() + (i / 4) * h, w, h);
}

} // namespace livedaw::ui
