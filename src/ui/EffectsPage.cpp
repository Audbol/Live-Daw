#include "dsp/DspUtils.h"
#include "ui/Pages.h"

namespace livedaw::ui
{

namespace
{
    /** The strip whose PRE or POST insert uses `slot` (the first one found). */
    struct FxUser
    {
        Strip* strip = nullptr;
        bool post = false;
    };

    FxUser findUser (MixerEngine& engine, int slot)
    {
        for (auto kind : { StripKind::Channel, StripKind::Aux, StripKind::Bus, StripKind::Main, StripKind::Matrix })
            for (auto& s : engine.getStrips (kind))
            {
                if (s->params.preInsert.fx.load() == slot)
                    return { s.get(), false };
                if (s->params.postInsert.fx.load() == slot && s->params.postInsert.mode.load() == 0)
                    return { s.get(), true };
            }
        return {};
    }

    void segmentMeter (juce::Graphics& g, juce::Rectangle<float> r, float level)
    {
        const int segments = juce::jmax (6, (int) (r.getHeight() / 4.0f));
        const float h = r.getHeight() / (float) segments;
        for (int col = 0; col < 2; ++col)
        {
            const float w = (r.getWidth() - 2.0f) / 2.0f;
            for (int i = 0; i < segments; ++i)
            {
                const float pos = ((float) i + 0.5f) / (float) segments;
                g.setColour (level >= pos ? (pos > 0.85f ? Colours::meterYellow : Colours::meterGreen) : juce::Colour (0xff3a3a3a));
                g.fillRect (r.getX() + (float) col * (w + 2.0f), r.getBottom() - (float) (i + 1) * h + 0.5f, w, juce::jmax (1.0f, h - 1.5f));
            }
        }
    }
} // namespace

class EffectsPage::Tile final : public juce::Component, private juce::Timer
{
public:
    Tile (UiContext& c, int s, EffectsPage& p) : ctx (c), slot (s), page (p) { startTimerHz (10); }

    void paint (juce::Graphics& g) override
    {
        auto& fx = ctx.engine.getFx();
        auto& params = fx.params (slot);
        auto b = getLocalBounds().toFloat().reduced (1.5f);
        const bool loaded = fx.isLoaded (slot);
        const bool sel = page.selected == slot;

        g.setColour (juce::Colours::black);
        g.fillRect (b);
        g.setColour (sel ? juce::Colours::white : juce::Colour (0xff9a9a9a));
        g.drawRect (b, sel ? 2.5f : 1.5f);

        // Slot number, bottom right.
        auto number = juce::Rectangle<float> (b.getRight() - 22.0f, b.getBottom() - 18.0f, 20.0f, 16.0f);
        g.setColour (juce::Colour (0xff6a6a6a));
        g.fillRect (number);
        g.setColour (juce::Colours::black);
        g.setFont (juce::FontOptions (13.0f, juce::Font::bold));
        g.drawText (juce::String (slot + 1), number, juce::Justification::centred);

        auto inner = b.reduced (6.0f);
        auto left = inner.removeFromLeft (inner.getWidth() * 0.28f);
        inner.removeFromLeft (6.0f);
        auto device = inner.removeFromTop (inner.getHeight() * 0.48f);
        meterArea = device.removeFromRight (juce::jmin (16.0f, device.getWidth() * 0.1f));
        device.removeFromRight (3.0f);
        deviceArea = device;

        if (! loaded)
        {
            // An empty slot: a blank rack unit with a "+".
            g.setColour (juce::Colour (0xff2a2a2a));
            g.fillRect (device);
            segmentMeter (g, meterArea, 0.0f);
            const float d = juce::jmin (device.getHeight() * 0.6f, 34.0f);
            auto circle = juce::Rectangle<float> (d, d).withCentre (device.getCentre());
            g.setColour (juce::Colour (0xff7a7a7a));
            g.drawEllipse (circle, 2.0f);
            g.fillRect (juce::Rectangle<float> (d * 0.5f, 2.0f).withCentre (circle.getCentre()));
            g.fillRect (juce::Rectangle<float> (2.0f, d * 0.5f).withCentre (circle.getCentre()));
            sourceArea = onArea = prePostArea = knobArea = {};
            return;
        }

        // The device: a faceplate with the plugin's name.
        auto* plugin = fx.getPlugin (slot);
        juce::ColourGradient face (juce::Colour (0xff5a5d62), device.getX(), device.getY(), juce::Colour (0xff2c2e31), device.getX(), device.getBottom(), false);
        g.setGradientFill (face);
        g.fillRect (device);
        g.setColour (juce::Colour (0xff1a1a1a));
        g.drawRect (device, 1.0f);
        for (float x : { device.getX() + 5.0f, device.getRight() - 5.0f })
            for (float y : { device.getY() + 5.0f, device.getBottom() - 5.0f })
            {
                g.setColour (juce::Colour (0xff8a8a8a));
                g.fillEllipse (juce::Rectangle<float> (3.0f, 3.0f).withCentre ({ x, y }));
            }
        auto label = device.reduced (12.0f, 6.0f);
        g.setColour (juce::Colours::white);
        g.setFont (juce::FontOptions (juce::jlimit (12.0f, 20.0f, label.getHeight() * 0.42f), juce::Font::bold));
        g.drawFittedText (plugin != nullptr ? plugin->getName().toUpperCase() : juce::String ("FX " + juce::String (slot + 1)),
                          label.removeFromTop (label.getHeight() * 0.62f).toNearestInt(), juce::Justification::centredBottom, 1);
        g.setColour (juce::Colour (0xffc8c8c8));
        g.setFont (juce::FontOptions (juce::jlimit (9.0f, 13.0f, label.getHeight() * 0.7f), juce::Font::bold));
        g.drawFittedText (plugin != nullptr ? plugin->getPluginDescription().manufacturerName.toUpperCase() : juce::String(), label.toNearestInt(),
                          juce::Justification::centredTop, 1);

        const auto user = findUser (ctx.engine, slot);
        const float level = user.strip != nullptr && ! params.bypass.load()
                                ? juce::jlimit (0.0f, 1.0f, (dsp::gainToDb (ctx.meters.get (user.strip, false, 0)) + 60.0f) / 60.0f)
                                : 0.0f;
        segmentMeter (g, meterArea, level);

        if (fx.isFaulted (slot) || fx.isDoubleAssigned (slot))
        {
            auto warn = device.withSizeKeepingCentre (device.getWidth(), 20.0f);
            g.setColour (Colours::mute);
            g.fillRect (warn);
            g.setColour (juce::Colours::black);
            g.setFont (juce::FontOptions (14.0f, juce::Font::bold));
            g.drawFittedText (fx.isFaulted (slot) ? "FAULT - BYPASSED" : "DOUBLE ASSIGNED", warn.toNearestInt(), juce::Justification::centred, 1);
        }

        // Source strip, PRE/POST and ON.
        const float chipH = juce::jlimit (12.0f, 22.0f, left.getHeight() * 0.13f);
        onArea = left.removeFromBottom (chipH);
        left.removeFromBottom (3.0f);
        prePostArea = left.removeFromBottom (chipH);
        left.removeFromBottom (4.0f);
        sourceArea = left;
        if (user.strip != nullptr)
        {
            auto src = left;
            auto bar = src.removeFromTop (juce::jmax (10.0f, src.getHeight() * 0.18f));
            g.setColour (user.strip->getColour());
            g.fillRect (bar);
            g.setColour (juce::Colour (0xff5a5a5a));
            g.fillRect (src);
            auto name = src.removeFromBottom (juce::jmin (18.0f, src.getHeight() * 0.32f));
            drawIcon (g, stripIcon (user.strip->getKind()), src.reduced (src.getWidth() * 0.22f, 3.0f), juce::Colours::white);
            g.setColour (juce::Colours::white);
            g.setFont (juce::FontOptions (juce::jmin (14.0f, name.getHeight()), juce::Font::bold));
            g.drawFittedText (stripName (*user.strip).toUpperCase(), name.toNearestInt(), juce::Justification::centred, 1);

            auto pre = prePostArea.removeFromLeft (prePostArea.getWidth() / 2.0f);
            drawChip (g, pre, "PRE", user.post ? juce::Colour (0xff4a4a4a) : Colours::pink, user.post ? Colours::textDim : juce::Colours::black);
            drawChip (g, prePostArea, "POST", user.post ? Colours::green : juce::Colour (0xff4a4a4a), user.post ? juce::Colours::black : Colours::textDim);
            prePostArea = pre.getUnion (prePostArea);
        }
        const bool on = ! params.bypass.load();
        drawChip (g, onArea, "ON", on ? Colours::yellow : juce::Colour (0xff4a4a4a), on ? juce::Colours::black : Colours::textDim);

        // Mix ring and value.
        auto bottom = inner.withTrimmedTop (4.0f).withTrimmedRight (24.0f);
        knobArea = bottom.removeFromLeft (juce::jmin (bottom.getHeight(), bottom.getWidth() * 0.4f));
        const float mix = juce::jlimit (0.0f, 1.0f, params.mix.load() / 100.0f);
        const auto k = knobArea.reduced (knobArea.getWidth() * 0.12f);
        const float a0 = juce::MathConstants<float>::pi * 1.2f, a1 = juce::MathConstants<float>::pi * 2.8f;
        const float rad = k.getWidth() * 0.4f, thick = k.getWidth() * 0.18f;
        juce::Path track, value;
        track.addCentredArc (k.getCentreX(), k.getCentreY(), rad, rad, 0.0f, a0, a1, true);
        value.addCentredArc (k.getCentreX(), k.getCentreY(), rad, rad, 0.0f, a0, a0 + mix * (a1 - a0), true);
        g.setColour (juce::Colour (0xff3a3a3a));
        g.strokePath (track, juce::PathStrokeType (thick));
        g.setColour (on ? juce::Colour (0xffd8d8d8) : Colours::textDim);
        g.strokePath (value, juce::PathStrokeType (thick));
        bottom.removeFromLeft (6.0f);
        g.setColour (Colours::text);
        g.setFont (juce::FontOptions (juce::jlimit (12.0f, 20.0f, bottom.getHeight() * 0.3f), juce::Font::bold));
        g.drawText ("MIX", bottom.removeFromTop (bottom.getHeight() / 2.0f).toNearestInt(), juce::Justification::bottomLeft);
        g.setFont (juce::FontOptions (juce::jlimit (14.0f, 26.0f, bottom.getHeight() * 0.62f), juce::Font::bold));
        g.drawText (juce::String (juce::roundToInt (params.mix.load())) + " %", bottom.toNearestInt(), juce::Justification::topLeft);
    }

    void mouseDown (const juce::MouseEvent& e) override
    {
        dragStart = ctx.engine.getFx().params (slot).mix.load();
        draggingMix = knobArea.contains (e.position);
    }

    void mouseDrag (const juce::MouseEvent& e) override
    {
        if (draggingMix)
            ctx.engine.getFx().params (slot).mix = juce::jlimit (0.0f, 100.0f, dragStart - (float) e.getDistanceFromDragStartY() * 0.5f);
    }

    void mouseUp (const juce::MouseEvent& e) override
    {
        page.selected = slot;
        page.notifyKnobsChanged();
        for (auto* t : page.tiles)
            t->repaint();

        auto& params = ctx.engine.getFx().params (slot);
        if (e.mods.isPopupMenu())
            showFxSlotMenu (ctx, slot, this);
        else if (onArea.contains (e.position))
            params.bypass = ! params.bypass.load();
        else if (sourceArea.contains (e.position) || prePostArea.contains (e.position))
        {
            if (auto user = findUser (ctx.engine, slot); user.strip != nullptr && ctx.selectStrip)
            {
                ctx.selectStrip (user.strip);
                if (ctx.showHomeTab)
                    ctx.showHomeTab (user.post ? HomeTab::PostInsert : HomeTab::PreInsert);
            }
        }
        else if (! (draggingMix && e.mouseWasDraggedSinceMouseDown()))
            clickFxSlot (ctx, slot, this);
        draggingMix = false;
    }

    void mouseDoubleClick (const juce::MouseEvent& e) override
    {
        if (knobArea.contains (e.position))
            ctx.engine.getFx().params (slot).mix = 100.0f;
    }

private:
    void timerCallback() override { repaint(); }

    UiContext& ctx;
    int slot;
    EffectsPage& page;
    juce::Rectangle<float> deviceArea, meterArea, sourceArea, prePostArea, onArea, knobArea;
    float dragStart = 0.0f;
    bool draggingMix = false;
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

    // The rack: dark brushed metal with screws.
    juce::ColourGradient metal (juce::Colour (0xff2b2c2d), (float) rack.getX(), (float) rack.getY(), juce::Colour (0xff161718), (float) rack.getRight(),
                                (float) rack.getBottom(), false);
    metal.addColour (0.45, juce::Colour (0xff222324));
    g.setGradientFill (metal);
    g.fillRect (rack);
    juce::Random random (7);
    for (int i = 0; i < 90; ++i)
    {
        const float x = (float) rack.getX() + random.nextFloat() * (float) rack.getWidth();
        const float y = (float) rack.getY() + random.nextFloat() * (float) rack.getHeight();
        g.setColour (juce::Colours::white.withAlpha (0.03f + random.nextFloat() * 0.04f));
        g.drawLine (x, y, x + random.nextFloat() * 40.0f - 20.0f, y + random.nextFloat() * 6.0f - 3.0f, 1.0f);
    }
    for (auto p : { rack.getTopLeft().toFloat() + juce::Point<float> (14.0f, 14.0f), rack.getTopRight().toFloat() + juce::Point<float> (-14.0f, 14.0f),
                    rack.getBottomLeft().toFloat() + juce::Point<float> (14.0f, -14.0f), rack.getBottomRight().toFloat() + juce::Point<float> (-14.0f, -14.0f) })
    {
        g.setColour (juce::Colour (0xff4a4a4a));
        g.fillEllipse (juce::Rectangle<float> (12.0f, 12.0f).withCentre (p));
        g.setColour (juce::Colour (0xff1a1a1a));
        g.drawLine (p.x - 4.0f, p.y - 4.0f, p.x + 4.0f, p.y + 4.0f, 1.5f);
    }
    g.setColour (juce::Colour (0xff8a8a8a));
    g.setFont (juce::FontOptions (juce::jmin (30.0f, rack.getHeight() * 0.06f), juce::Font::bold | juce::Font::italic));
    g.drawText ("FX", rack.getX() + 36, rack.getY() + 4, 80, juce::jmin (32, rack.getHeight() / 14), juce::Justification::centredLeft);

    g.setColour (Colours::text);
    g.setFont (juce::FontOptions (juce::jmin (20.0f, premiumLabel.getHeight() * 0.8f), juce::Font::bold));
    auto premium = premiumLabel;
    const int textW = juce::GlyphArrangement::getStringWidthInt (g.getCurrentFont(), "PREMIUM FX");
    auto diamond = premium.withSizeKeepingCentre (textW + 30, premium.getHeight()).removeFromLeft (22).toFloat().reduced (2.0f, 4.0f);
    juce::Path gem;
    gem.startNewSubPath (diamond.getX(), diamond.getY() + diamond.getHeight() * 0.35f);
    gem.lineTo (diamond.getX() + diamond.getWidth() * 0.25f, diamond.getY());
    gem.lineTo (diamond.getRight() - diamond.getWidth() * 0.25f, diamond.getY());
    gem.lineTo (diamond.getRight(), diamond.getY() + diamond.getHeight() * 0.35f);
    gem.lineTo (diamond.getCentreX(), diamond.getBottom());
    gem.closeSubPath();
    g.strokePath (gem, juce::PathStrokeType (1.5f));
    g.drawText ("PREMIUM FX", premium.withTrimmedLeft (26), juce::Justification::centred);
    g.drawText ("STANDARD FX", standardLabel, juce::Justification::centred);
}

void EffectsPage::resized()
{
    rack = getLocalBounds().reduced (2);
    auto b = rack.reduced (juce::jmin (40, rack.getWidth() / 24), 0);
    b.removeFromTop (juce::jmin (40, rack.getHeight() / 12));
    auto labels = b.removeFromBottom (juce::jmin (34, rack.getHeight() / 14));
    b.removeFromBottom (4);

    const int gap = 12;
    const int w = (b.getWidth() - gap) / 4, h = b.getHeight() / 4;
    premiumLabel = labels.withWidth (w * 2);
    standardLabel = labels.withX (b.getX() + w * 2 + gap).withWidth (w * 2);
    for (int i = 0; i < tiles.size(); ++i)
    {
        // Slots run down the columns: 1-4 in the first, 5-8 in the second ...
        const int col = i / 4, row = i % 4;
        tiles[i]->setBounds (b.getX() + col * w + (col >= 2 ? gap : 0), b.getY() + row * h, w, h);
    }
}

} // namespace livedaw::ui
