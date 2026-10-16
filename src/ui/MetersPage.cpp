#include "dsp/DspUtils.h"
#include "ui/MeterComponent.h"
#include "ui/Pages.h"

namespace livedaw::ui
{

MetersPage::MetersPage (UiContext& c) : ctx (c)
{
    groups = { { "CHANNELS", StripKind::Channel, false, LayerSet::Left, "in-1", {} },
               { "AUX", StripKind::Aux, false, LayerSet::Left, "", {} },
               { "BUSES", StripKind::Bus, false, LayerSet::Center, "bus-1", {} },
               { "DCA", StripKind::Bus, true, LayerSet::Center, "dca-1", {} },
               { "MAINS", StripKind::Main, false, LayerSet::Right, "out-1", {} },
               { "MATRICES", StripKind::Matrix, false, LayerSet::Right, "", {} } };
    startTimerHz (20);
}

int MetersPage::count (const Group& g) const
{
    return g.dca ? kNumDcas : (int) ctx.engine.getStrips (g.kind).size();
}

void MetersPage::timerCallback()
{
    // Re-layout when channels or buses were added or removed.
    std::vector<int> counts;
    for (auto& g : groups)
        counts.push_back (count (g));
    if (counts != laidOutCounts)
        resized();
    repaint();
}

void MetersPage::resized()
{
    laidOutCounts.clear();
    for (auto& g : groups)
        laidOutCounts.push_back (count (g));

    auto b = getLocalBounds().reduced (4);
    auto top = b.removeFromTop (b.getHeight() / 2);
    groups[0].area = top;

    // Bottom row: widths proportional to the number of meters.
    int total = 0;
    for (size_t i = 1; i < groups.size(); ++i)
        total += juce::jmax (4, count (groups[i]));
    const int width = b.getWidth();
    for (size_t i = 1; i < groups.size(); ++i)
    {
        const int w = width * juce::jmax (4, count (groups[i])) / juce::jmax (1, total);
        groups[i].area = b.removeFromLeft (i + 1 == groups.size() ? b.getWidth() : w);
    }
}

void MetersPage::paint (juce::Graphics& g)
{
    g.fillAll (Colours::screen);

    for (auto& group : groups)
    {
        drawSectionBox (g, group.area, group.title + "  (" + juce::String (count (group)) + ")");
        auto area = group.area.reduced (8, 6).withTrimmedTop (18);
        const int n = count (group);
        if (n == 0)
            continue;

        const float w = juce::jmax (2.0f, (float) area.getWidth() / (float) n);
        const auto labels = area.removeFromBottom (12);
        for (int i = 0; i < n; ++i)
        {
            auto bar = juce::Rectangle<float> ((float) area.getX() + (float) i * w, (float) area.getY(), juce::jmax (1.0f, w - 1.5f), (float) area.getHeight());
            float level = 0.0f;
            bool muted = false;
            juce::Colour colour = Colours::outline;

            if (group.dca)
            {
                auto& d = ctx.engine.getDca (i);
                level = fader::dbToPos (d.faderDb.load());
                muted = d.mute.load();
                colour = wingColour (d.colour.load());
            }
            else
            {
                auto& s = *ctx.engine.getStrips (group.kind)[(size_t) i];
                const float peak = juce::jmax (ctx.meters.get (&s, false, 0), ctx.meters.get (&s, false, 1));
                level = MeterComponent::dbToProportion (dsp::gainToDb (peak));
                muted = s.params.mute.load();
                colour = s.getColour();
            }

            g.setColour (juce::Colours::black.withAlpha (0.6f));
            g.fillRect (bar);
            auto cap = bar.removeFromTop (4.0f);
            g.setColour (muted ? Colours::mute : colour.withAlpha (0.5f));
            g.fillRect (cap);

            auto lit = bar.withTop (bar.getBottom() - bar.getHeight() * level);
            if (group.dca)
                g.setColour (Colours::accent.withAlpha (0.7f));
            else
                g.setColour (level > MeterComponent::dbToProportion (-6.0f) ? Colours::meterYellow : Colours::meterGreen);
            g.fillRect (lit);

            if (w >= 14.0f || i % juce::jmax (1, (int) std::ceil (14.0f / w)) == 0)
            {
                g.setColour (Colours::textDim);
                g.setFont (juce::FontOptions (9.0f));
                g.drawText (juce::String (i + 1), juce::Rectangle<float> (bar.getX() - 4.0f, (float) labels.getY(), w + 8.0f, 12.0f),
                            juce::Justification::centred);
            }
        }
    }
}

void MetersPage::mouseDown (const juce::MouseEvent& e)
{
    for (auto& group : groups)
    {
        if (! group.area.contains (e.getPosition()))
            continue;

        auto area = group.area.reduced (8, 6).withTrimmedTop (18);
        const int n = count (group);

        // Title: bring the group to its fader section.
        if (e.y < area.getY())
        {
            juce::String bank = group.bank;
            if (bank.isEmpty() && n > 0)
                bank = ctx.show.layers.findDefaultLayerContaining (ctx.engine, group.section,
                                                                   state::FaderRef::of (state::StripRef::of (*ctx.engine.getStrips (group.kind)[0])));
            if (bank.isNotEmpty())
            {
                ctx.show.layers.setSelectedLayerId (group.section, bank);
                if (ctx.layersChanged)
                    ctx.layersChanged();
            }
            return;
        }

        if (group.dca || n == 0)
            return;

        const int i = juce::jlimit (0, n - 1, (int) ((float) (e.x - area.getX()) / ((float) area.getWidth() / (float) n)));
        auto* s = ctx.engine.getStrips (group.kind)[(size_t) i].get();
        if (ctx.selectStrip)
            ctx.selectStrip (s);

        // ... and show it on the matching section.
        const auto bank = ctx.show.layers.findDefaultLayerContaining (ctx.engine, group.section, state::FaderRef::of (state::StripRef::of (*s)));
        if (bank.isNotEmpty() && bank != ctx.show.layers.getSelectedLayerId (group.section))
        {
            ctx.show.layers.setSelectedLayerId (group.section, bank);
            if (ctx.layersChanged)
                ctx.layersChanged();
        }
        return;
    }
}

} // namespace livedaw::ui
