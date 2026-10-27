#include "dsp/DspUtils.h"
#include "ui/Pages.h"

namespace livedaw::ui
{

using routing::drawGroupIcon;
using routing::Group;

namespace
{
    constexpr int kTilesPerRow = 8, kTargetsPerPage = 48, kSignalsPerPage = 64;
    // An oscillator's LEVEL (dB). A source's GAIN takes its group's range
    // (MixerEngine::sourceGainRange).
    constexpr float kOscLevelMin = -40.0f, kOscLevelMax = -6.0f;

    /** The GAIN range of input port `port`'s source group. */
    MixerEngine::GainRange gainRangeOf (int port)
    {
        return port >= 0 ? MixerEngine::sourceGainRange (MixerEngine::sourceIndex (port)) : MixerEngine::GainRange();
    }

    // ROUTING's own colours (sampled from the console's screen, firmware 3.1):
    // port labels blue for mono and olive for stereo, n/a tiles in dim blue and
    // grey, unpatched tiles darker than patched ones, the locked padlock red.
    const juce::Colour monoLabel = Colours::choice, stereoLabel = Dark::olive, naLabel { 0xff203860 }, naText = Colours::chipOff,
        tileOff { 0xff181818 }, numberGrey { 0xffb8b8b8 }, iconGrey { 0xffc8c8c8 }, greyKey = Colours::chipOff, lockRed { 0xfff85c58 },
        autoYellow = KnobColours::yellow, playbackStrip (0xff2f5a32);

    /** `v` px of the WING's 1280 x 800 screen in px of this screen. */
    int px (float v) { return juce::roundToInt (wingPx (v)); }

    /** A rectangle given in WING px from the page's top left (the page starts
        under the status bar, at the console's y = 80). */
    juce::Rectangle<int> wingRect (float x, float y, float w, float h)
    {
        return { px (x), px (y), px (x + w) - px (x), px (y + h) - px (y) };
    }

    /** The console's fonts by the height of their capitals in WING px. */
    juce::Font capFont (float capWingPx, WingWeight weight = WingWeight::Medium)
    {
        return wingFontPx (juce::jmax (5.0f, wingPx (capWingPx)), weight);
    }

    /** A channel or source without a source: its colour at a quarter (the console's dim strips). */
    juce::Colour dimmed (juce::Colour c) { return c.withMultipliedBrightness (0.25f); }

    bool hasCustomName (const Strip& s)
    {
        return stripName (s) != s.getLabel();
    }
} // namespace

/** The icons of the group menus and selectors (line art like the console's). */
void routing::drawGroupIcon (juce::Graphics& g, Group group, juce::Rectangle<float> area, juce::Colour c)
{
    const float s = juce::jmin (area.getWidth(), area.getHeight());
    const auto r = area.withSizeKeepingCentre (s, s);
    auto P = [&r, s] (float u, float v) { return juce::Point<float> (r.getX() + u * s, r.getY() + v * s); };
    const float w = juce::jmax (1.2f, s * 0.055f);
    const juce::PathStrokeType stroke (w, juce::PathStrokeType::curved, juce::PathStrokeType::rounded);
    auto ring = [&] (float u, float v, float radius)
    {
        juce::Path p;
        p.addEllipse (P (u, v).x - radius * s, P (u, v).y - radius * s, radius * 2.0f * s, radius * 2.0f * s);
        g.strokePath (p, stroke);
    };
    auto text = [&] (const juce::String& t, float u, float v, float cap, float width)
    {
        g.setFont (wingFontPx (juce::jmax (4.0f, cap * s), WingWeight::SemiBold));
        g.drawFittedText (t, juce::Rectangle<float> (width * s, cap * s * 1.6f).withCentre (P (u, v)).toNearestInt(), juce::Justification::centred, 1, 0.6f);
    };
    // A round connector shell with its latch at the top (XLR, AES50).
    auto shell = [&]
    {
        ring (0.5f, 0.54f, 0.34f);
        g.fillEllipse (juce::Rectangle<float> (s * 0.12f, s * 0.12f).withCentre (P (0.5f, 0.2f)));
    };
    auto xlrPins = [&]
    {
        for (auto [u, v] : { std::pair { 0.38f, 0.48f }, { 0.62f, 0.48f }, { 0.5f, 0.68f } })
            ring (u, v, 0.055f);
    };
    // An RJ45 socket: a box with its latch notch and contacts.
    auto rj45 = [&] (juce::Rectangle<float> box)
    {
        juce::Path p;
        p.addRoundedRectangle (box, s * 0.03f);
        g.strokePath (p, stroke);
        const float pin = box.getWidth() / 9.0f;
        for (int i = 0; i < 4; ++i)
            g.fillRect (juce::Rectangle<float> (pin * 0.8f, box.getHeight() * 0.22f).withPosition (box.getX() + pin * (1.5f + 1.7f * (float) i), box.getY() + box.getHeight() * 0.14f));
    };
    g.setColour (c);
    juce::Path p;
    switch (group)
    {
        case Group::LocalIn:
        case Group::LocalOut:
            shell();
            xlrPins();
            return;
        case Group::AesIn:
        case Group::AesOut:
            shell();
            xlrPins();
            // The square wave of a digital signal.
            p.startNewSubPath (P (0.66f, 0.16f));
            for (int i = 0; i < 3; ++i)
            {
                const float u = 0.66f + 0.08f * (float) i;
                p.lineTo (P (u, 0.08f));
                p.lineTo (P (u + 0.04f, 0.08f));
                p.lineTo (P (u + 0.04f, 0.16f));
                p.lineTo (P (u + 0.08f, 0.16f));
            }
            g.strokePath (p, juce::PathStrokeType (w * 0.7f));
            return;
        case Group::AuxIn:
        case Group::AuxOut:
            ring (0.5f, 0.5f, 0.24f);
            ring (0.5f, 0.5f, 0.13f);
            return;
        case Group::Oscillator:
            ring (0.5f, 0.5f, 0.34f);
            p.startNewSubPath (P (0.32f, 0.52f));
            p.cubicTo (P (0.4f, 0.32f), P (0.46f, 0.36f), P (0.5f, 0.5f));
            p.cubicTo (P (0.54f, 0.64f), P (0.6f, 0.68f), P (0.68f, 0.48f));
            g.strokePath (p, stroke);
            return;
        case Group::Aes50A:
        case Group::Aes50B:
        case Group::Aes50C:
            shell();
            rj45 (juce::Rectangle<float> (P (0.33f, 0.38f), P (0.67f, 0.66f)));
            text (group == Group::Aes50A ? "A" : group == Group::Aes50B ? "B" : "C", 0.5f, 0.61f, 0.09f, 0.2f);
            return;
        case Group::StConnect:
            text ("SC", 0.52f, 0.4f, 0.24f, 0.6f);
            p.startNewSubPath (P (0.22f, 0.66f));
            p.cubicTo (P (0.34f, 0.66f), P (0.38f, 0.58f), P (0.5f, 0.6f));
            p.lineTo (P (0.86f, 0.6f));
            g.strokePath (p, stroke);
            g.fillEllipse (juce::Rectangle<float> (s * 0.08f, s * 0.08f).withCentre (P (0.18f, 0.66f)));
            p.clear();
            p.addTriangle (P (0.9f, 0.6f), P (0.82f, 0.55f), P (0.82f, 0.65f));
            g.fillPath (p);
            return;
        case Group::UsbAudio:
            p.addRoundedRectangle (juce::Rectangle<float> (P (0.2f, 0.2f), P (0.8f, 0.8f)), s * 0.06f);
            g.strokePath (p, stroke);
            p.clear();
            // The USB-B socket with its chip.
            p.startNewSubPath (P (0.3f, 0.7f));
            p.lineTo (P (0.3f, 0.4f));
            p.lineTo (P (0.38f, 0.3f));
            p.lineTo (P (0.62f, 0.3f));
            p.lineTo (P (0.7f, 0.4f));
            p.lineTo (P (0.7f, 0.7f));
            p.closeSubPath();
            g.strokePath (p, stroke);
            p.clear();
            p.addRectangle (juce::Rectangle<float> (P (0.4f, 0.42f), P (0.6f, 0.6f)));
            g.strokePath (p, juce::PathStrokeType (w * 0.7f));
            return;
        case Group::WlivePlay:
        case Group::WliveRec:
            p.addRectangle (juce::Rectangle<float> (P (0.1f, 0.4f), P (0.9f, 0.62f)));
            g.strokePath (p, stroke);
            g.fillRect (juce::Rectangle<float> (s * 0.1f, s * 0.09f).withCentre (P (0.32f, 0.51f)));
            for (int i = 0; i < 4; ++i)
                g.fillEllipse (juce::Rectangle<float> (s * 0.05f, s * 0.05f).withCentre (P (0.48f + 0.1f * (float) i, 0.51f)));
            return;
        case Group::Dante:
            p.addRoundedRectangle (juce::Rectangle<float> (P (0.22f, 0.2f), P (0.78f, 0.8f)), s * 0.06f);
            g.strokePath (p, stroke);
            rj45 (juce::Rectangle<float> (P (0.33f, 0.33f), P (0.67f, 0.63f)));
            return;
        case Group::UsbPlayer:
        case Group::Recorder:
            p.addRoundedRectangle (juce::Rectangle<float> (P (0.34f, 0.38f), P (0.66f, 0.86f)), s * 0.06f);
            p.addRectangle (juce::Rectangle<float> (P (0.39f, 0.14f), P (0.61f, 0.38f)));
            g.strokePath (p, stroke);
            g.fillRect (juce::Rectangle<float> (P (0.44f, 0.22f), P (0.47f, 0.28f)));
            g.fillRect (juce::Rectangle<float> (P (0.53f, 0.22f), P (0.56f, 0.28f)));
            return;
        case Group::Bus:
        case Group::Main:
        case Group::Matrix:
        {
            // A return arrow with the group's name in its loop.
            p.startNewSubPath (P (0.24f, 0.36f));
            p.cubicTo (P (0.24f, 0.28f), P (0.28f, 0.26f), P (0.34f, 0.26f));
            p.lineTo (P (0.7f, 0.26f));
            p.cubicTo (P (0.84f, 0.26f), P (0.86f, 0.5f), P (0.7f, 0.5f));
            p.lineTo (P (0.16f, 0.5f));
            p.startNewSubPath (P (0.28f, 0.4f));
            p.lineTo (P (0.16f, 0.5f));
            p.lineTo (P (0.28f, 0.6f));
            g.strokePath (p, stroke);
            g.fillEllipse (juce::Rectangle<float> (s * 0.07f, s * 0.07f).withCentre (P (0.24f, 0.36f)));
            const auto name = group == Group::Bus ? "BUS" : group == Group::Main ? "MAIN" : "MATRIX";
            const auto band = juce::Rectangle<float> (s * 0.42f, s * 0.12f).withCentre (P (0.53f, 0.26f));
            g.saveState();
            g.setColour (Colours::screenTile);
            g.fillRect (band);
            g.restoreState();
            g.setColour (c);
            text (name, 0.53f, 0.26f, 0.08f, 0.44f);
            return;
        }
        case Group::UserSignal:
        {
            p.startNewSubPath (P (0.72f, 0.24f));
            p.lineTo (P (0.24f, 0.24f));
            p.lineTo (P (0.24f, 0.48f));
            p.lineTo (P (0.74f, 0.48f));
            p.lineTo (P (0.74f, 0.72f));
            p.lineTo (P (0.32f, 0.72f));
            juce::Path dashed;
            const float dashes[] = { s * 0.06f, s * 0.04f };
            juce::PathStrokeType (w).createDashedStroke (dashed, p, dashes, 2);
            g.fillPath (dashed);
            ring (0.78f, 0.24f, 0.06f);
            ring (0.26f, 0.72f, 0.06f);
            g.fillEllipse (juce::Rectangle<float> (s * 0.05f, s * 0.05f).withCentre (P (0.78f, 0.24f)));
            g.fillEllipse (juce::Rectangle<float> (s * 0.05f, s * 0.05f).withCentre (P (0.26f, 0.72f)));
            return;
        }
        case Group::Monitor:
            p.addRoundedRectangle (juce::Rectangle<float> (P (0.46f, 0.2f), P (0.76f, 0.8f)), s * 0.03f);
            g.strokePath (p, stroke);
            ring (0.61f, 0.34f, 0.05f);
            ring (0.61f, 0.6f, 0.1f);
            p.clear();
            p.addCentredArc (P (0.4f, 0.6f).x, P (0.4f, 0.6f).y, s * 0.14f, s * 0.14f, 0.0f, -juce::MathConstants<float>::halfPi * 1.2f, 0.0f, true);
            g.strokePath (p, stroke);
            g.fillRoundedRectangle (juce::Rectangle<float> (P (0.24f, 0.6f), P (0.32f, 0.74f)), s * 0.02f);
            return;
        case Group::FxSend:
            p.addRoundedRectangle (juce::Rectangle<float> (P (0.22f, 0.2f), P (0.78f, 0.78f)), s * 0.05f);
            g.strokePath (p, stroke);
            text ("FX", 0.5f, 0.5f, 0.26f, 0.5f);
            return;
    }
}

namespace
{

    /** The console's icon of a source: a WING icon (SOURCES > ICON/COLOR) or its group's. */
    void drawSourceIcon (juce::Graphics& g, int source, int wingIcon, juce::Rectangle<float> r, juce::Colour c)
    {
        if (wingIcon > 0 && drawWingIcon (g, wingIcon, r, c))
            return;
        const auto group = routing::groupOfSource (source);
        if (group == Group::WlivePlay)
            drawIcon (g, Icon::Waveform, r, c);
        else
            drawGroupIcon (g, group == Group::Aes50B || group == Group::Aes50C ? Group::LocalIn : group, r, c);
    }

    /** A key with centred text: grey (#585858) with black print, or lit. */
    void drawKey (juce::Graphics& g, juce::Rectangle<int> r, const juce::String& text, bool on, juce::Colour onColour, float cap = 14.0f)
    {
        if (r.isEmpty())
            return;
        g.setColour (on ? onColour : greyKey);
        g.fillRect (r);
        g.setColour (juce::Colours::black);
        g.setFont (fontFittingWidth (capFont (cap, WingWeight::SemiBold), text, (float) r.getWidth() - 4.0f, 0.75f));
        g.drawFittedText (text, r.reduced (2, 0), juce::Justification::centred, 2, 0.75f);
    }

    /** A black caption bar over a pair of keys (CHANNEL INPUT, COPY CUSTOMIZATION). */
    void drawCaptionBar (juce::Graphics& g, juce::Rectangle<int> r, const juce::String& text)
    {
        if (r.isEmpty())
            return;
        g.setColour (juce::Colours::black);
        g.fillRect (r);
        g.setColour (Dark::caption);
        g.setFont (fontFittingWidth (capFont (9.0f), text, (float) r.getWidth() - 4.0f, 0.75f));
        g.drawText (text, r, juce::Justification::centred);
    }

    /** An outlined key [I8]: a frame on the panel, an optional icon at the left
        (INITIALIZE); filled when lit. */
    void drawOutlinedKey (juce::Graphics& g, juce::Rectangle<int> r, std::optional<Icon> icon, const juce::String& text, bool on, juce::Colour onColour,
                          juce::Colour frame = Dark::caption)
    {
        if (r.isEmpty())
            return;
        auto f = r.toFloat();
        g.setColour (on ? onColour : Colours::screenTile);
        g.fillRect (f);
        g.setColour (on ? onColour : frame);
        g.drawRect (f, juce::jmax (1.0f, wingPx (1.5f)));
        const auto ink = on ? juce::Colours::black : Dark::caption;
        if (icon.has_value())
        {
            auto ib = f.removeFromLeft (f.getHeight()).reduced (f.getHeight() * 0.22f);
            drawIcon (g, *icon, ib.translated (wingPx (6.0f), 0.0f), ink);
        }
        g.setColour (ink);
        g.setFont (capFont (13.0f));
        g.drawFittedText (text, f.toNearestInt().reduced (4, 0), juce::Justification::centred, 1, 0.8f);
    }

    /** A square key with an icon and its caption under it (+48V, POL); a held
        key (`hold` 0 .. 1) fills with the colour it switches to. */
    void drawIconKeyBelow (juce::Graphics& g, juce::Rectangle<int> key, Icon icon, const juce::String& caption, bool on, juce::Colour onColour,
                           float hold = 0.0f)
    {
        if (key.isEmpty())
            return;
        g.setColour (juce::Colours::black);
        g.fillRect (key.expanded (px (2.0f)));
        g.setColour (on ? onColour : Colours::screenTile);
        g.fillRect (key);
        paintHoldProgress (g, key.toFloat(), hold, on ? Colours::screenTile : onColour);
        drawIcon (g, icon, key.toFloat().reduced ((float) key.getHeight() * 0.24f), on ? juce::Colours::black : Dark::caption);
        g.setColour (on ? onColour : Dark::caption.withAlpha (0.8f));
        g.setFont (capFont (13.0f));
        g.drawText (caption, key.withY (key.getBottom() + px (6.0f)).withHeight (px (24.0f)).expanded (px (16.0f), 0), juce::Justification::centred);
    }

    /** An icon over a small caption (RENAME, NONE). */
    void drawIconKey (juce::Graphics& g, juce::Rectangle<int> r, Icon icon, const juce::String& text, bool enabled = true)
    {
        if (r.isEmpty())
            return;
        const auto ink = enabled ? Dark::caption : Dark::textDim.withAlpha (0.5f);
        auto f = r.toFloat();
        auto label = f.removeFromBottom (f.getHeight() * 0.36f);
        drawIcon (g, icon, f.reduced (f.getHeight() * 0.1f), ink);
        g.setColour (ink);
        g.setFont (capFont (9.0f));
        g.drawFittedText (text, label.toNearestInt(), juce::Justification::centred, 1);
    }

    /** A group selector [ROUTING header]: in a black frame the group's icon,
        the caption over the group's name, and the drop-down arrow key. */
    void drawSelector (juce::Graphics& g, juce::Rectangle<int> r, Group group, const juce::String& caption)
    {
        if (r.isEmpty())
            return;
        g.setColour (juce::Colours::black);
        g.fillRect (r);
        r = r.reduced (px (2.0f));
        auto arrow = r.removeFromRight (px (30.0f));
        g.setColour (Colours::screenTile);
        g.fillRect (arrow);
        juce::Path tri;
        const auto c = arrow.getCentre().toFloat();
        const float tw = wingPx (8.0f), th = wingPx (8.0f);
        tri.addTriangle (c.x - tw, c.y - th * 0.5f, c.x + tw, c.y - th * 0.5f, c.x, c.y + th * 0.5f);
        g.setColour (Dark::caption);
        g.fillPath (tri);
        r.removeFromRight (px (2.0f));
        g.setColour (Colours::screenPanel);
        g.fillRect (r);
        auto ib = r.removeFromLeft (r.getHeight()).reduced (px (6.0f));
        drawGroupIcon (g, group, ib.toFloat(), Dark::caption);
        r.removeFromLeft (px (4.0f));
        g.setColour (Dark::caption);
        const auto name = juce::String (routing::info (group).name);
        g.setFont (fontFittingWidth (capFont (12.5f), caption, (float) r.getWidth(), 0.8f));
        g.drawText (caption, r.withHeight (r.getHeight() * 44 / 100).translated (0, px (2.0f)), juce::Justification::bottomLeft);
        g.setColour (Dark::text);
        g.setFont (fontFittingWidth (capFont (19.0f, WingWeight::SemiBold), name, (float) r.getWidth(), 0.7f));
        g.drawText (name, r.withTrimmedTop (r.getHeight() * 44 / 100), juce::Justification::centredLeft);
    }

    /** The mini meter of a tile: a column of segments, `pos` (0..1) lit. */
    void drawMiniMeter (juce::Graphics& g, juce::Rectangle<float> meter, float pos)
    {
        const int segments = juce::jmax (4, (int) (meter.getHeight() / juce::jmax (2.0f, wingPx (3.0f))));
        const float segH = meter.getHeight() / (float) segments;
        for (int i = 0; i < segments; ++i)
        {
            const bool lit = pos > (float) i / (float) segments;
            g.setColour (lit ? (i == segments - 1 ? Colours::meterRed : Colours::meterGreen) : Colours::screenWell);
            g.fillRect (meter.getX(), meter.getBottom() - (float) (i + 1) * segH, meter.getWidth(), juce::jmax (1.0f, segH - 1.0f));
        }
    }

    /** The source meter's scale [SOURCES]: CLIP, -6 ... -60 equally spaced. */
    constexpr float kScaleMarks[] = { 0.0f, -6.0f, -12.0f, -24.0f, -36.0f, -48.0f, -60.0f };
    float scalePosition (float db)
    {
        db = juce::jlimit (-60.0f, 0.0f, db);
        for (int i = 0; i + 1 < (int) std::size (kScaleMarks); ++i)
            if (db >= kScaleMarks[i + 1])
                return ((float) i + (kScaleMarks[i] - db) / (kScaleMarks[i] - kScaleMarks[i + 1])) / 6.0f;
        return 1.0f;
    }

    /** An n/a tile: the dim strip over a black body with "n/a" and its label. */
    void paintNaTile (juce::Graphics& g, juce::Rectangle<int> area, juce::Colour strip, const juce::String& label, float stripShare, bool meter,
                      bool selected)
    {
        auto r = area.toFloat();
        g.setColour (dimmed (strip));
        g.fillRect (r.removeFromTop (r.getHeight() * stripShare));
        if (meter)
            drawMiniMeter (g, r.withTrimmedLeft (r.getWidth() * 0.9f).withTrimmedRight (r.getWidth() * 0.04f).reduced (0.0f, wingPx (4.0f)), 0.0f);
        auto labelRow = r.removeFromBottom (r.getHeight() * 0.34f);
        g.setColour (naText);
        g.setFont (capFont (13.0f));
        g.drawText ("n/a", r.withTrimmedRight (meter ? r.getWidth() * 0.1f : 0.0f).toNearestInt(), juce::Justification::centred);
        g.setColour (naLabel);
        g.setFont (capFont (9.5f));
        g.drawFittedText (label, labelRow.withTrimmedRight (meter ? labelRow.getWidth() * 0.1f : 0.0f).toNearestInt(), juce::Justification::centredTop, 1, 0.7f);
        if (selected)
        {
            g.setColour (Colours::gold);
            g.drawRect (area.toFloat().expanded (wingPx (4.0f)), wingPx (3.0f));
        }
    }

    /** False for an interface port the interface doesn't have (drawn n/a). */
    bool sourceAvailable (MixerEngine& engine, int source)
    {
        if (source < 0)
            return false;
        const auto group = routing::groupOfSource (source);
        if (group == Group::LocalIn || group == Group::Aes50B || group == Group::Aes50C)
            return source < engine.getHardwareInputNames().size();
        return true;
    }

    /** A source's settings: a stereo or mid/side pair runs on its first source's. */
    ld::SourceParams* sourceSettings (MixerEngine& engine, int port)
    {
        const int index = MixerEngine::sourceIndex (port);
        auto* src = engine.getSource (index);
        if (src != nullptr && src->mode.load() != ld::kSourceMono)
            return engine.getSource (index & ~1);
        return src;
    }

    void drawSelection (juce::Graphics& g, juce::Rectangle<int> area)
    {
        g.setColour (Colours::gold);
        g.drawRect (area.toFloat().expanded (wingPx (4.0f)), juce::jmax (2.0f, wingPx (3.0f)));
    }

    /** The strip a bus, main or matrix signal comes from (nullptr for other sources, a missing bus). */
    Strip* stripOfSignal (MixerEngine& engine, int source, int* side = nullptr)
    {
        StripKind kind = StripKind::Bus;
        int number = 0, s = 0;
        if (! routing::stripOfSource (source, kind, number, s))
            return nullptr;
        if (side != nullptr)
            *side = s;
        for (auto& strip : engine.getStrips (kind))
            if (strip->getNumber() - 1 == number) // a bus by its slot
                return strip.get();
        return nullptr;
    }

    /** A source's colour: its own (SOURCES > ICON/COLOR), a strip signal's strip's. */
    juce::Colour sourceColour (MixerEngine& engine, int source)
    {
        if (auto* s = stripOfSignal (engine, source))
            return stripDisplayColour (*s);
        const auto* src = sourceSettings (engine, source);
        return wingColour (src != nullptr ? src->colour.load() : 1);
    }

    /** A source is one side of a stereo signal: a pair (STEREO, MID/SIDE), a strip's L or R. */
    bool sourcePaired (MixerEngine& engine, int source)
    {
        if (MixerEngine::isStripSource (source))
            return true;
        const auto* src = sourceSettings (engine, source);
        return src != nullptr && src->mode.load() != ld::kSourceMono;
    }
} // namespace

//==============================================================================
/** A group menu [ROUTING]: the groups as tiles of four (icon over name) under
    the selector, over the dimmed page; the current group framed gold. */
class RoutingPage::GroupMenu final : public juce::Component
{
public:
    GroupMenu (routing::Menu m, int current, juce::Point<int> origin, std::function<void (int)> chosen)
        : menu (std::move (m)), selected (current), topLeft (origin), onChosen (std::move (chosen))
    {
    }

    /** What dims under the menu (3.1: only the side the menu is for); empty: everything. */
    juce::Rectangle<int> dimArea;

    void paint (juce::Graphics& g) override
    {
        g.setColour (Colours::scrim);
        g.fillRect (dimArea.isEmpty() ? getLocalBounds() : dimArea);
        g.setColour (Colours::screenPanel);
        g.fillRect (frame());
        for (auto& [area, id] : tiles())
            routing::paintGroupTile (g, area, (Group) id, id == selected, id == pressed);
    }

    void mouseDown (const juce::MouseEvent& e) override
    {
        pressed = tileAt (e.getPosition());
        repaint();
    }

    void mouseUp (const juce::MouseEvent& e) override
    {
        const int was = std::exchange (pressed, -1);
        const int id = tileAt (e.getPosition());
        auto chosen = onChosen;
        if (was >= 0 && id == was)
            chosen (id);
        else if (! frame().contains (e.getPosition()))
            chosen (-1);
        else
            repaint();
    }

private:
    // [WING px] tiles 88 square on a pitch of 91, the fourth column and the
    // spaced rows 8 further apart, 4 of padding.
    juce::Rectangle<int> frame() const
    {
        const int rows = (int) menu.rows.size();
        return wingRect (0.0f, 0.0f, 8.0f + 3.0f * 91.0f + 8.0f + 88.0f, 8.0f + (float) (rows - 1) * 91.0f + 8.0f * (float) menu.spacedRows.size() + 88.0f)
            .translated (topLeft.x, topLeft.y);
    }

    std::vector<std::pair<juce::Rectangle<int>, int>> tiles() const
    {
        std::vector<std::pair<juce::Rectangle<int>, int>> out;
        float y = 4.0f;
        for (size_t row = 0; row < menu.rows.size(); ++row)
        {
            if (std::find (menu.spacedRows.begin(), menu.spacedRows.end(), (int) row) != menu.spacedRows.end())
                y += 8.0f;
            for (size_t col = 0; col < menu.rows[row].size(); ++col)
                if (const int id = menu.rows[row][col]; id >= 0)
                    out.push_back ({ wingRect (4.0f + (float) col * 91.0f + (col == 3 ? 8.0f : 0.0f), y, 88.0f, 88.0f).translated (topLeft.x, topLeft.y), id });
            y += 91.0f;
        }
        return out;
    }

    int tileAt (juce::Point<int> p) const
    {
        for (auto& [area, id] : tiles())
            if (area.contains (p))
                return id;
        return -1;
    }

    routing::Menu menu;
    int selected, pressed = -1;
    juce::Point<int> topLeft;
    std::function<void (int)> onChosen;
};

//==============================================================================
RoutingPage::RoutingPage (UiContext& c) : ctx (c)
{
    // Where the drop-down lists of the hand-drawn fields open.
    anchor.setInterceptsMouseClicks (false, false);
    addAndMakeVisible (anchor);
    startTimerHz (10); // the source meters
}

RoutingPage::~RoutingPage() = default;

void RoutingPage::setLocked (bool shouldLock)
{
    if (lock.set (shouldLock))
        repaint();
}

StatusTitle RoutingPage::getStatusTitle()
{
    // SOURCES and OUTPUTS show the selected source or output (grey tile, its
    // number, icon and group: "1 LOCAL IN"), CHANNELS the selected channel.
    StatusTitle t;
    if (mode == Mode::Sources)
    {
        t.kind = StatusTitle::Kind::Source;
        const auto group = routing::groupOfSource (selectedSource);
        t.prefix = juce::String (routing::numberInGroup (selectedSource) + 1);
        const auto custom = ctx.engine.getSourceName (MixerEngine::sourceIndex (selectedSource));
        t.text = custom.isNotEmpty() ? custom.toUpperCase() : juce::String (routing::info (group).name);
        t.icon = group == Group::Oscillator ? Icon::Sine : group == Group::WlivePlay ? Icon::Waveform : group == Group::UsbPlayer ? Icon::UsbStick : Icon::Xlr;
    }
    else if (mode == Mode::Outputs)
    {
        t.kind = StatusTitle::Kind::Source;
        const auto group = routing::groupOfOutput (selectedPort);
        t.prefix = juce::String (routing::outputNumber (selectedPort));
        t.text = routing::info (group).name;
        t.icon = group == Group::Recorder ? Icon::UsbStick : Icon::Xlr;
    }
    else
        t.plainNumber = true; // 3.1: the channel's number unboxed, in black
    return t;
}

std::vector<StatusKey> RoutingPage::getStatusKeys()
{
    return { { "CHANNELS", Icon::Faders, mode == Mode::Channels, [this] { setMode (Mode::Channels); } },
             { "SOURCES", Icon::SourcesIn, mode == Mode::Sources, [this] { setMode (Mode::Sources); } },
             { "OUTPUTS", Icon::OutputsOut, mode == Mode::Outputs, [this] { setMode (Mode::Outputs); } } };
}

std::vector<Strip*> RoutingPage::channelTargets() const
{
    std::vector<Strip*> list;
    for (auto k : { StripKind::Channel, StripKind::Aux })
        for (auto& s : ctx.engine.getStrips (k))
            list.push_back (s.get());
    return list;
}

namespace
{
    /** The MONITOR source group (fig. 32): PHONES L / R (0, 1), SPEAKERS L / R (2, 3). */
    std::atomic<int32_t>& monitorOutput (MixerEngine& e, int channel)
    {
        switch (channel)
        {
            case 0: return e.phones.outputLeft;
            case 1: return e.phones.outputRight;
            case 2: return e.monitorOutputLeft;
            default: return e.monitorOutputRight;
        }
    }
    constexpr int kMonitorSignals = 4;
    juce::String monitorSignalName (int channel) { return channel < 2 ? "PHONES" : "SPEAKERS"; }
    /** "1L" ... "2R": monitor 1 is the PHONES, 2 the SPEAKERS (the WING's /cfg/mon/1, 2). */
    juce::String monitorSignalNumber (int channel) { return juce::String (channel / 2 + 1) + (channel % 2 == 0 ? "L" : "R"); }
} // namespace

std::vector<RoutingPage::Signal> RoutingPage::signals() const
{
    std::vector<Signal> list;
    auto addStrips = [&] (StripKind kind)
    {
        for (auto& s : ctx.engine.getStrips (kind))
        {
            list.push_back ({ s.get(), 0 });
            if (s->isOutput() || s->params.stereo.load())
                list.push_back ({ s.get(), 1 });
        }
    };
    if (signalGroup == Group::Bus)
        addStrips (StripKind::Bus);
    else if (signalGroup == Group::Main)
        addStrips (StripKind::Main);
    else if (signalGroup == Group::Matrix)
        addStrips (StripKind::Matrix);
    else if (signalGroup == Group::Monitor)
        for (int ch = 0; ch < kMonitorSignals; ++ch)
            list.push_back ({ nullptr, ch });
    else if (signalGroup == Group::FxSend) // no FX sends here
        list.assign ((size_t) routing::info (signalGroup).size, Signal { nullptr, -1 });
    else
    {
        // Any source straight to an output (manual 5.4 OUTPUTS, chapter 2:
        // "sent directly from any input to any output"); USER SIGNAL's
        // user signals and user patches too. Sidecar's missing ports are n/a.
        const int count = routing::tileCount (signalGroup, numHardware (false));
        for (int i = 0; i < count; ++i)
        {
            const int source = routing::sourceAt (signalGroup, i);
            list.push_back (source >= 0 ? Signal { nullptr, 0, source } : Signal { nullptr, -1 });
        }
    }
    return list;
}

int RoutingPage::numTargetPages() const
{
    return juce::jmax (1, ((int) channelTargets().size() + kTargetsPerPage - 1) / kTargetsPerPage);
}

int RoutingPage::numHardware (bool outputs) const
{
    return (outputs ? ctx.engine.getHardwareOutputNames() : ctx.engine.getHardwareInputNames()).size();
}

bool RoutingPage::sourcePresent (int source) const
{
    return sourceAvailable (ctx.engine, source);
}

Strip* RoutingPage::selectedStrip() const
{
    auto& strips = ctx.engine.getStrips (selectedKind);
    return juce::isPositiveAndBelow (selectedIndex, (int) strips.size()) ? strips[(size_t) selectedIndex].get() : nullptr;
}

int RoutingPage::patchedSourceIndex() const
{
    const auto* s = selectedStrip();
    const int port = s == nullptr ? -1 : (editAlt ? s->getAltInputLeft() : s->getInputLeft());
    return port >= 0 ? MixerEngine::sourceIndex (port) : -1;
}

void RoutingPage::select (Strip* s)
{
    if (s == nullptr || ! s->isInput())
        return;
    selectedKind = s->getKind();
    auto& strips = ctx.engine.getStrips (selectedKind);
    for (size_t i = 0; i < strips.size(); ++i)
        if (strips[i].get() == s)
            selectedIndex = (int) i;

    // Keep the selection on screen.
    const auto list = channelTargets();
    for (size_t i = 0; i < list.size(); ++i)
        if (list[i] == s && (int) i / kTargetsPerPage != targetPage)
        {
            targetPage = (int) i / kTargetsPerPage;
            layoutTiles();
        }
    repaint();
}

void RoutingPage::setMode (Mode m)
{
    mode = m;
    groupMenu.reset();
    if (auto* s = selectedStrip(); s == nullptr || ! s->isInput())
    {
        selectedKind = StripKind::Channel;
        selectedIndex = 0;
    }
    resized();
    repaint();
    notifyStatusKeysChanged();
    notifyKnobsChanged(); // SOURCES: the source's GAIN
}

void RoutingPage::refresh()
{
    if (auto* s = ctx.getSelectedStrip ? ctx.getSelectedStrip() : nullptr; s != nullptr && s->isInput())
        select (s);
    layoutTiles();
    repaint();
}

void RoutingPage::timerCallback()
{
    if (ctx.engine.getStructureVersion() != layoutVersion)
        layoutTiles();
    // INITIALIZE CONSOLE (or a reset console) locks the patch again, as the console's.
    if (lock.update())
        repaint();

    // The source meters (the page is their only reader): the group's tiles and
    // the selection. Only the meters that move are repainted: a whole page of
    // tiles ten times a second can keep a slow machine's message thread busy.
    if (mode != Mode::Outputs)
    {
        auto moved = [] (float a, float b)
        { return std::abs (juce::Decibels::gainToDecibels (a, -60.0f) - juce::Decibels::gainToDecibels (b, -60.0f)) > 0.5f; };
        const auto& grid = mode == Mode::Channels ? rightPorts : leftPorts;
        sourcePeaks.resize (grid.size(), 0.0f);
        for (size_t i = 0; i < grid.size(); ++i)
        {
            const int source = routing::sourceAt (inputGroup, (int) i);
            auto* src = source >= 0 ? ctx.engine.getSource (MixerEngine::sourceIndex (source)) : nullptr;
            const float peak = src != nullptr ? src->peak.exchange (0.0f) : 0.0f;
            auto& shown = sourcePeaks[i];
            const float before = shown;
            shown = juce::jmax (peak, shown * 0.5f);
            if (moved (before, shown))
                repaint (grid[i]);
            if (source == selectedSource && source >= 0)
            {
                if (moved (selectedSourcePeak, shown) && mode == Mode::Sources)
                    repaint (sourceMeter);
                selectedSourcePeak = shown;
            }
        }
    }
    // Everything else twice a second (patches, names and keys changed elsewhere).
    if (++refreshTicks % 5 == 0)
        repaint();
}

void RoutingPage::resized()
{
    // [WING px] Each side: a header box 80 tall, an 8 px gap, then the tiles in
    // a framed black box. CHANNELS and OUTPUTS split the page 544 | 712,
    // SOURCES 712 | 544 (the settings on the right).
    for (auto* r : { &inputCaption, &mainKey, &altKey, &monoKey, &stereoKey, &midSideKey, &autoKey, &lockFrame, &lockKey, &unlockKey, &prevPage, &nextPage, &leftSelector,
                     &rightSelector, &noneKey, &copyCaption, &srcToChKey, &chToSrcKey, &renameKey, &srcSoloKey, &meterLegend, &prevSignals, &nextSignals })
        *r = {};

    const float split = mode == Mode::Sources ? 712.0f : 544.0f;
    leftPanel = wingRect (8.0f, 0.0f, split, 800.0f).withBottom (getHeight());
    rightPanel = wingRect (16.0f + split, 0.0f, 1264.0f - 8.0f - split, 800.0f).withBottom (getHeight());
    leftHeader = leftPanel.withHeight (px (80.0f));
    rightHeader = rightPanel.withHeight (px (80.0f));

    if (mode == Mode::Sources)
    {
        leftSelector = wingRect (16.0f, 8.0f, 264.0f, 64.0f);
        renameKey = wingRect (648.0f, 12.0f, 64.0f, 60.0f);
        // SRC SOLO (fig. 36), while SOURCE SOLO is on (SETUP > AUDIO).
        srcSoloKey = wingRect (520.0f, 20.0f, 104.0f, 44.0f);
    }
    else
    {
        lockFrame = wingRect (428.0f, 12.0f, 112.0f, 56.0f);
        lockKey = wingRect (430.0f, 14.0f, 54.0f, 52.0f);
        unlockKey = wingRect (486.0f, 14.0f, 52.0f, 52.0f);
        autoKey = wingRect (322.0f, 14.0f, 76.0f, 52.0f);
        if (mode == Mode::Channels)
        {
            inputCaption = wingRect (16.0f, 8.0f, 162.0f, 16.0f);
            mainKey = wingRect (16.0f, 24.0f, 80.0f, 48.0f);
            altKey = wingRect (98.0f, 24.0f, 80.0f, 48.0f);
            // The page keys of consoles with more than 40 channels (Sidecar's).
            if (numTargetPages() > 1)
            {
                prevPage = wingRect (188.0f, 24.0f, 40.0f, 48.0f);
                nextPage = wingRect (230.0f, 24.0f, 40.0f, 48.0f);
            }
            // M | ST | M/S: the pairing of the channel's source, beside NONE
            // (as SELECT MAIN INPUT has them); shown only while unlocked.
            monoKey = wingRect (842.0f, 18.0f, 56.0f, 48.0f);
            stereoKey = wingRect (900.0f, 18.0f, 56.0f, 48.0f);
            midSideKey = wingRect (958.0f, 18.0f, 56.0f, 48.0f);
        }
        else
            leftSelector = wingRect (16.0f, 8.0f, 264.0f, 64.0f);

        rightSelector = wingRect (568.0f, 8.0f, 264.0f, 64.0f);
        // NONE clears the patch (shown only while unlocked, as on the console).
        noneKey = wingRect (mode == Mode::Channels ? 1024.0f : 848.0f, 12.0f, 64.0f, 60.0f);
        if (mode == Mode::Channels)
        {
            copyCaption = wingRect (1102.0f, 8.0f, 162.0f, 16.0f);
            srcToChKey = wingRect (1102.0f, 24.0f, 80.0f, 48.0f);
            chToSrcKey = wingRect (1184.0f, 24.0f, 80.0f, 48.0f);
        }
        else
        {
            meterLegend = wingRect (1068.0f, 22.0f, 196.0f, 40.0f);
            if (signals().size() > (size_t) kSignalsPerPage)
            {
                prevSignals = wingRect (924.0f, 24.0f, 40.0f, 48.0f);
                nextSignals = wingRect (968.0f, 24.0f, 40.0f, 48.0f);
            }
        }
    }
    layoutSourcePanel();
    layoutTiles();
}

void RoutingPage::layoutTiles()
{
    layoutVersion = ctx.engine.getStructureVersion();
    targetPage = juce::jlimit (0, numTargetPages() - 1, targetPage);
    channelTiles.clear();
    leftPorts.clear();
    rightPorts.clear();
    signalTiles.clear();
    separator = leftGrid = rightGrid = {};
    if (leftPanel.isEmpty())
        return;

    // [WING px] Tiles start 8 inside the box's 4 px frame. Channel tiles are
    // 59 x 77 on a pitch of 67 x 84, output tiles 59 x 69 (pitch 67 x 76),
    // source and signal tiles 80 x 68 (pitch 88 x 76). Rows that don't fit
    // the page get closer.
    const float bottom = (float) getHeight() * 800.0f / juce::jmax (1.0f, getWingScreenHeight()) - 8.0f;
    auto grid = [&] (juce::Rectangle<int>& box, float boxX, float boxW, float pitchX, float tileW, float pitchY, float tileH, int count,
                     std::vector<juce::Rectangle<int>>& out)
    {
        const int rows = juce::jmax (1, (count + kTilesPerRow - 1) / kTilesPerRow);
        const float scale = juce::jmin (1.0f, (bottom - 96.0f - 8.0f + (pitchY - tileH)) / ((float) rows * pitchY));
        pitchY *= scale;
        tileH = pitchY - (pitchY / scale - tileH);
        for (int i = 0; i < count; ++i)
            out.push_back (wingRect (boxX + 8.0f + (float) (i % kTilesPerRow) * pitchX, 96.0f + (float) (i / kTilesPerRow) * pitchY, tileW, tileH));
        box = wingRect (boxX, 88.0f, boxW, 16.0f + (float) (rows - 1) * pitchY + tileH);
    };

    const float rightX = mode == Mode::Sources ? 720.0f : 560.0f;
    const float rightW = 1272.0f - rightX;
    if (mode == Mode::Channels)
    {
        // Up to 48 channels; the aux channels on a row of their own under a line.
        const auto list = channelTargets();
        const int first = targetPage * kTargetsPerPage;
        const int count = juce::jmin (kTargetsPerPage, (int) list.size() - first);
        int channelRows = 0, auxCount = 0;
        for (int i = 0; i < count; ++i)
            auxCount += list[(size_t) (first + i)]->getKind() == StripKind::Aux ? 1 : 0;
        channelRows = (count - auxCount + kTilesPerRow - 1) / kTilesPerRow;
        const int auxRows = (auxCount + kTilesPerRow - 1) / kTilesPerRow;
        const bool split = auxCount > 0 && auxCount < count;
        float pitchY = 84.0f, tileH = 77.0f, gap = split ? 24.0f : 0.0f;
        const float needed = (float) (channelRows + auxRows) * pitchY + gap;
        if (const float avail = bottom - 96.0f - 8.0f + (pitchY - tileH); needed > avail)
        {
            const float scale = avail / needed;
            pitchY *= scale;
            tileH = pitchY - 7.0f * scale;
            gap *= scale;
        }
        int ch = 0, aux = 0;
        float lastBottom = 96.0f;
        for (int i = 0; i < count; ++i)
        {
            auto* s = list[(size_t) (first + i)];
            const bool isAux = s->getKind() == StripKind::Aux;
            const int n = isAux ? aux++ : ch++;
            const float y = 96.0f + (float) ((isAux ? channelRows : 0) + n / kTilesPerRow) * pitchY + (isAux ? gap : 0.0f);
            channelTiles.push_back ({ wingRect (16.0f + (float) (n % kTilesPerRow) * 67.0f, y, 59.0f, tileH), s });
            lastBottom = juce::jmax (lastBottom, y + tileH);
        }
        if (split)
            separator = wingRect (24.0f, 96.0f + (float) channelRows * pitchY - (pitchY - tileH) + (pitchY - tileH + gap) / 2.0f - 1.5f, 512.0f, 3.0f);
        leftGrid = wingRect (8.0f, 88.0f, 544.0f, lastBottom + 8.0f - 88.0f);
        grid (rightGrid, rightX, rightW, 88.0f, 80.0f, 76.0f, 68.0f, routing::tileCount (inputGroup, numHardware (false)), rightPorts);
    }
    else if (mode == Mode::Sources)
        grid (leftGrid, 8.0f, 712.0f, 88.0f, 80.0f, 76.0f, 68.0f, routing::tileCount (inputGroup, numHardware (false)), leftPorts);
    else
    {
        grid (leftGrid, 8.0f, 544.0f, 67.0f, 59.0f, 76.0f, 69.0f, routing::tileCount (outputGroup, numHardware (true)), leftPorts);
        const auto list = signals();
        const int pages = juce::jmax (1, ((int) list.size() + kSignalsPerPage - 1) / kSignalsPerPage);
        signalPage = juce::jlimit (0, pages - 1, signalPage);
        const int first = signalPage * kSignalsPerPage;
        const int count = juce::jmin (kSignalsPerPage, (int) list.size() - first);
        std::vector<juce::Rectangle<int>> areas;
        grid (rightGrid, rightX, rightW, 88.0f, 80.0f, 76.0f, 68.0f, count, areas);
        for (int i = 0; i < count; ++i)
            signalTiles.push_back ({ areas[(size_t) i], list[(size_t) (first + i)] });
    }
}

std::vector<std::pair<Strip*, bool>> RoutingPage::sourceUsers (int port) const
{
    std::vector<std::pair<Strip*, bool>> users;
    for (auto* s : channelTargets())
    {
        const bool stereo = s->params.stereo.load();
        const int l = s->getInputLeft(), r = s->getInputRight();
        if (l == port || (stereo && (r == port || (r < 0 && l >= 0 && l + 1 == port))))
            users.push_back ({ s, false });
        const int al = s->getAltInputLeft(), ar = s->getAltInputRight();
        if (al == port || (stereo && (ar == port || (ar < 0 && al >= 0 && al + 1 == port))))
            users.push_back ({ s, true });
    }
    return users;
}

juce::String RoutingPage::sourceName (int source) const
{
    if (source < 0)
        return "NONE";
    // A source renamed on SOURCES shows its name everywhere in ROUTING.
    if (const int index = MixerEngine::sourceIndex (source); index >= 0)
        if (auto name = ctx.engine.getSourceName (index); name.isNotEmpty())
            return name;
    return routing::sourceLabel (source);
}

RoutingPage::Signal RoutingPage::feederOf (int port) const
{
    if (routing::isRecorderPort (port))
    {
        // A RECORDER port taps a strip's output (the USB recorder's own patch).
        const auto tap = ctx.engine.getUsbRecorder().getTap (port - routing::kRecorderPortBase);
        if (tap.kind == UsbRecorder::Tap::Kind::Source && tap.source >= 0)
            return { nullptr, 0, tap.source };
        if (tap.kind != UsbRecorder::Tap::Kind::Strip)
            return { nullptr, -1 };
        auto& strips = ctx.engine.getStrips (tap.stripKind);
        for (size_t i = 0; i < strips.size(); ++i)
        {
            const int number = strips[i]->isBus() ? strips[i]->getId() + 1 : (int) i + 1;
            if (number == tap.stripNumber)
                return { strips[i].get(), tap.channel };
        }
        return { nullptr, -1 };
    }
    if (const int source = ctx.engine.getOutputSource (port); source >= 0)
        return { nullptr, 0, source };
    for (int k = 0; k < kNumStripKinds; ++k)
        for (auto& s : ctx.engine.getStrips (static_cast<StripKind> (k)))
        {
            if (s->params.outputLeft.load() == port)
                return { s.get(), 0 };
            if (s->params.outputRight.load() == port)
                return { s.get(), 1 };
        }
    for (int ch = 0; ch < kMonitorSignals; ++ch)
        if (monitorOutput (ctx.engine, ch).load() == port)
            return { nullptr, ch };
    return { nullptr, -1 };
}

juce::String RoutingPage::signalLabel (const Signal& sig) const
{
    if (sig.channel < 0)
        return {};
    if (sig.source >= 0) // a source: its name, else its label ("LCL 3", "USR 2")
        return sourceName (sig.source);
    if (sig.strip == nullptr) // PHONES L / R, SPKR L / R: separate outputs
        return juce::String (sig.channel < 2 ? "PHONES" : "SPKR") + (sig.channel % 2 == 0 ? " L" : " R");
    // "BUS 1L", "MAIN 1R", "MTX 2L", "CH 3", "AUX 1L".
    const bool stereo = sig.strip->isOutput() || sig.strip->params.stereo.load();
    const char* kind = sig.strip->isBus() ? "BUS " : sig.strip->getKind() == StripKind::Main ? "MAIN " : sig.strip->getKind() == StripKind::Matrix ? "MTX "
                       : sig.strip->getKind() == StripKind::Aux                                ? "AUX "
                                                                                               : "CH ";
    return kind + juce::String (sig.strip->getNumber()) + (stereo ? (sig.channel == 0 ? "L" : "R") : juce::String());
}

//==============================================================================
void RoutingPage::paintChannelTile (juce::Graphics& g, juce::Rectangle<int> area, Strip& s, bool selected) const
{
    // [WING 59 x 77] The channel's colour strip (17 px, its name in black) over
    // the body: a patched channel shows its icon and its source's label (blue
    // mono, olive stereo), an unpatched one only its number on a darker body.
    // The input shown is the one being patched (MAIN or ALT); a channel
    // listening to its ALT input carries an orange ALT chip.
    const int port = editAlt ? s.getAltInputLeft() : s.getInputLeft();
    const int portRight = editAlt ? s.getAltInputRight() : s.getInputRight();
    const bool stereo = s.params.stereo.load();

    auto r = area.toFloat();
    auto strip = r.removeFromTop (r.getHeight() * 17.0f / 77.0f);
    g.setColour (stripHasSource (s) ? s.getColour() : dimmed (s.getColour()));
    g.fillRect (strip);
    if (hasCustomName (s))
    {
        g.setColour (juce::Colours::black);
        g.setFont (fontFittingWidth (capFont (9.0f), stripName (s).toUpperCase(), strip.getWidth() - 4.0f, 0.7f));
        g.drawFittedText (stripName (s).toUpperCase(), strip.toNearestInt().reduced (2, 0), juce::Justification::centred, 1, 0.7f);
    }
    g.setColour (port >= 0 ? Colours::screenTile : tileOff);
    g.fillRect (r);

    auto label = r.removeFromBottom (r.getHeight() * 18.0f / 60.0f);
    g.setFont (capFont (port >= 0 ? 9.5f : 10.5f));
    if (port >= 0)
    {
        drawStripIcon (g, s, r.reduced (r.getWidth() * 0.18f, r.getHeight() * 0.08f).translated (0.0f, r.getHeight() * 0.04f), iconGrey);
        auto text = sourceName (port);
        if (stereo && sourceName (port) == routing::sourceLabel (port))
            text = routing::pairLabel (port, portRight >= 0 ? portRight : port + 1);
        g.setColour (! sourcePresent (port) ? naLabel : stereo ? stereoLabel : monoLabel);
        g.setFont (fontFittingWidth (g.getCurrentFont(), text, label.getWidth() - 2.0f, 0.7f));
        g.drawFittedText (text, label.toNearestInt().reduced (1, 0), juce::Justification::centredTop, 1, 0.7f);
    }
    else
    {
        g.setColour (numberGrey);
        g.drawText (shortStripLabel (s), label.toNearestInt().translated (0, -px (2.0f)), juce::Justification::centredTop);
    }
    if (s.isUsingAltInput())
        drawChip (g, juce::Rectangle<float> (wingPx (24.0f), wingPx (12.0f)).withPosition (r.getRight() - wingPx (26.0f), r.getY() + wingPx (2.0f)), "ALT",
                  Colours::alt, juce::Colours::black);
    if (selected)
        drawSelection (g, area);
}

void routing::paintGroupTile (juce::Graphics& g, juce::Rectangle<int> area, Group group, bool selected, bool pressed)
{
    g.setColour (selected ? Colours::gold : juce::Colours::black);
    g.fillRect (area);
    const auto inner = area.reduced (juce::jmax (2, px (selected ? 3.0f : 2.0f)));
    g.setColour (pressed ? Colours::chipOff : Colours::screenTile);
    g.fillRect (inner);
    auto f = inner.toFloat();
    auto caption = f.removeFromBottom (f.getHeight() * 0.3f);
    drawGroupIcon (g, group, f.expanded (f.getHeight() * 0.04f).translated (0.0f, f.getHeight() * 0.06f), iconGrey);
    const auto name = juce::String (routing::info (group).name);
    g.setColour (Dark::caption);
    g.setFont (fontFittingWidth (capFont (10.0f), name, caption.getWidth() - 2.0f, 0.75f));
    g.drawText (name, caption.toNearestInt(), juce::Justification::centred);
}

void RoutingPage::paintSourceTile (juce::Graphics& g, juce::Rectangle<int> area, Group group, int index, bool marked, int slot) const
{
    const float peak = juce::isPositiveAndBelow (slot, (int) sourcePeaks.size()) ? sourcePeaks[(size_t) slot] : 0.0f;
    routing::paintSourceTile (g, area, ctx.engine, group, index, peak, marked, mode == Mode::Sources);
}

void routing::paintSourceTile (juce::Graphics& g, juce::Rectangle<int> area, MixerEngine& engine, Group group, int index, float peak, bool marked,
                               bool numbered)
{
    // [WING 80 x 68] The source's colour strip (16 px, its name) over the body:
    // its icon, the 48 (phantom) and M (mute) chips beside a mini meter, and
    // the label (CHANNELS: "LCL 3", blue or olive for a pair; SOURCES: the
    // number in blue and the gain). A port the interface lacks reads n/a.
    const int port = routing::sourceAt (group, index);
    if (int side = 0; auto* strip = stripOfSignal (engine, port, &side))
    {
        // A bus, main or matrix signal (manual 2.6): the strip's colour, name
        // and icon, its output meter, "BUS 12L" (olive: one side of a stereo signal).
        auto r = area.toFloat();
        auto band = r.removeFromTop (r.getHeight() * 16.0f / 68.0f);
        g.setColour (stripDisplayColour (*strip));
        g.fillRect (band);
        if (hasCustomName (*strip))
        {
            g.setColour (juce::Colours::black);
            g.setFont (fontFittingWidth (capFont (9.0f), stripName (*strip).toUpperCase(), band.getWidth() - 4.0f, 0.7f));
            g.drawFittedText (stripName (*strip).toUpperCase(), band.toNearestInt().reduced (2, 0), juce::Justification::centred, 1, 0.7f);
        }
        g.setColour (Colours::screenTile);
        g.fillRect (r);
        const float u = r.getWidth() / 80.0f, v = r.getHeight() / 52.0f;
        drawMiniMeter (g, juce::Rectangle<float> (r.getX() + 72.0f * u, r.getY() + 2.0f * v, 4.0f * u, 46.0f * v), 0.0f);
        const auto iconArea = juce::Rectangle<float> (r.getX() + 6.0f * u, r.getY() + 4.0f * v, 30.0f * u, 30.0f * v);
        drawStripIcon (g, *strip, iconArea.withSizeKeepingCentre (juce::jmin (iconArea.getWidth(), iconArea.getHeight()), juce::jmin (iconArea.getWidth(), iconArea.getHeight())),
                       iconGrey);
        auto bottom = juce::Rectangle<float> (r.getX() + 4.0f * u, r.getY() + 36.0f * v, 66.0f * u, 14.0f * v);
        g.setFont (capFont (9.5f));
        g.setColour (stereoLabel);
        const auto label = numbered ? juce::String (index + 1) : routing::sourceLabel (port);
        g.setFont (fontFittingWidth (g.getCurrentFont(), label, bottom.getWidth(), 0.7f));
        g.drawText (label, bottom.toNearestInt(), numbered ? juce::Justification::centredLeft : juce::Justification::centred);
        juce::ignoreUnused (side);
        if (marked)
            drawSelection (g, area);
        return;
    }
    const auto* src = port >= 0 ? sourceSettings (engine, port) : nullptr;
    const int colour = src != nullptr ? src->colour.load() : 1;
    const bool playback = group == Group::WlivePlay || group == Group::UsbPlayer;
    const auto stripColour = playback && colour == 1 ? playbackStrip : wingColour (colour);
    if (port < 0 || src == nullptr || ! sourceAvailable (engine, port))
    {
        const auto label = port >= 0 ? routing::sourceLabel (port) : juce::String (routing::info (group).prefix) + " " + juce::String (index + 1);
        if (! numbered)
        {
            paintNaTile (g, area, stripColour, label, 16.0f / 68.0f, true, marked);
            return;
        }
        paintNaTile (g, area, stripColour, {}, 16.0f / 68.0f, true, marked);
        auto bottom = area.toFloat().withTrimmedTop (area.getHeight() * 0.66f).withTrimmedRight (area.getWidth() * 0.12f).reduced (wingPx (6.0f), 0.0f);
        g.setColour (naLabel);
        g.setFont (capFont (9.5f));
        g.drawText (juce::String (index + 1), bottom.toNearestInt(), juce::Justification::centredLeft);
        if (const auto range = gainRangeOf (port); src != nullptr && range.hasGain())
        {
            g.setColour (naText);
            g.drawText (juce::String (range.clamp (src->gainDb.load()), 1) + " dB", bottom.toNearestInt(), juce::Justification::centredRight);
        }
        return;
    }

    const int pairing = src->mode.load();
    auto r = area.toFloat();
    auto strip = r.removeFromTop (r.getHeight() * 16.0f / 68.0f);
    g.setColour (stripColour);
    g.fillRect (strip);
    if (const int idx = MixerEngine::sourceIndex (port); idx >= 0)
        if (const auto name = engine.getSourceName (idx).toUpperCase(); name.isNotEmpty())
        {
            g.setColour (juce::Colours::black);
            g.setFont (fontFittingWidth (capFont (9.0f), name, strip.getWidth() - 4.0f, 0.7f));
            g.drawFittedText (name, strip.toNearestInt().reduced (2, 0), juce::Justification::centred, 1, 0.7f);
        }
    g.setColour (Colours::screenTile);
    g.fillRect (r);

    // The mini meter at the right edge (the group's tiles keep their peaks).
    const float u = r.getWidth() / 80.0f, v = r.getHeight() / 52.0f;
    drawMiniMeter (g, juce::Rectangle<float> (r.getX() + 72.0f * u, r.getY() + 2.0f * v, 4.0f * u, 46.0f * v),
                   juce::jlimit (0.0f, 1.0f, (juce::Decibels::gainToDecibels (peak, -60.0f) + 60.0f) / 60.0f));

    // The icon at the left, 48 over M (grey, lit red) beside the meter.
    const auto iconArea = juce::Rectangle<float> (r.getX() + 6.0f * u, r.getY() + 4.0f * v, 30.0f * u, 30.0f * v);
    drawSourceIcon (g, port, src->icon.load(), iconArea.withSizeKeepingCentre (juce::jmin (iconArea.getWidth(), iconArea.getHeight()), juce::jmin (iconArea.getWidth(), iconArea.getHeight())),
                    iconGrey);
    if (area.getHeight() >= 30)
    {
        const auto chip48 = juce::Rectangle<float> (r.getX() + 52.0f * u, r.getY() + 4.0f * v, 17.0f * u, 12.0f * v);
        // An oscillator has no preamp: its chip shows SIN, PNK or WHT (a pair's, OSC 1's).
        if (group == Group::Oscillator)
        {
            static const char* const types[] = { "SIN", "PNK", "WHT" };
            const auto* o = engine.getOscillator (pairing != ld::kSourceMono ? 0 : MixerEngine::oscChannelOf (port));
            drawChip (g, chip48.withLeft (chip48.getX() - 6.0f * u), types[juce::jlimit (0, 2, o != nullptr ? (int) o->mode.load() : 0)], Colours::chipOff,
                      juce::Colours::black);
        }
        else if (group != Group::UserSignal) // the USR sources have no +48 V
            drawChip (g, chip48, "48", src->phantom.load() ? Colours::mute : Colours::chipOff, juce::Colours::black);
        drawChip (g, chip48.translated (0.0f, 14.0f * v), "M", src->mute.load() ? Colours::mute : Colours::chipOff, juce::Colours::black);
    }

    // A stereo or mid/side pair: the link glyph (o-o; o|o for mid/side) under the icon.
    if (pairing != ld::kSourceMono)
    {
        const float d = 6.0f * u;
        auto link = juce::Rectangle<float> (d * 3.2f, d).withPosition (r.getX() + 38.0f * u, r.getY() + 4.0f * v);
        g.setColour (stereoLabel);
        g.drawEllipse (link.withWidth (d), 1.2f);
        g.drawEllipse (link.withTrimmedLeft (link.getWidth() - d), 1.2f);
        if (pairing == ld::kSourceStereo)
            g.drawLine (link.getX() + d, link.getCentreY(), link.getRight() - d, link.getCentreY(), 1.2f);
        else
            g.drawLine (link.getCentreX(), link.getY(), link.getCentreX(), link.getBottom(), 1.2f);
    }

    auto bottom = juce::Rectangle<float> (r.getX() + 4.0f * u, r.getY() + 36.0f * v, 66.0f * u, 14.0f * v);
    g.setFont (capFont (9.5f));
    if (numbered)
    {
        g.setColour (pairing != ld::kSourceMono ? stereoLabel : monoLabel);
        g.drawText (juce::String (routing::numberInGroup (port) + 1), bottom.toNearestInt(), juce::Justification::centredLeft);
        g.setColour (Colours::textDim);
        if (const auto range = gainRangeOf (port); range.hasGain())
            g.drawText (juce::String (range.clamp (src->gainDb.load()), 1) + " dB", bottom.toNearestInt(), juce::Justification::centredRight);
    }
    else
    {
        const auto label = routing::sourceLabel (port);
        g.setColour (pairing != ld::kSourceMono ? stereoLabel : monoLabel);
        g.setFont (fontFittingWidth (g.getCurrentFont(), label, bottom.getWidth(), 0.7f));
        g.drawText (label, bottom.toNearestInt(), juce::Justification::centred);
    }
    if (marked)
        drawSelection (g, area);
}

void RoutingPage::paintOutputTile (juce::Graphics& g, juce::Rectangle<int> area, Group group, int index, bool selected) const
{
    // [WING 59 x 69] The feeding signal's colour strip (14 px) over the body:
    // a mini level bar, the output's number at the right and the signal's
    // label (olive: one side of a stereo signal). Unpatched: dim strip, darker body.
    const int port = routing::outputAt (group, index);
    const auto feeder = port >= 0 ? feederOf (port) : Signal { nullptr, -1 };
    const bool present = port >= 0 && (routing::isRecorderPort (port) || port < numHardware (true));
    const auto colour = feeder.channel < 0         ? wingColour (1)
                        : feeder.strip != nullptr  ? feeder.strip->getColour()
                        : feeder.source >= 0       ? sourceColour (ctx.engine, feeder.source)
                                                   : Colours::lime;
    if (! present)
    {
        paintNaTile (g, area, colour, feeder.channel >= 0 ? signalLabel (feeder) : juce::String (routing::info (group).prefix) + " " + juce::String (index + 1),
                     14.0f / 69.0f, false, selected);
        return;
    }
    auto r = area.toFloat();
    auto strip = r.removeFromTop (r.getHeight() * 14.0f / 69.0f);
    g.setColour (feeder.channel < 0 ? dimmed (colour) : colour);
    g.fillRect (strip);
    if (feeder.strip != nullptr && hasCustomName (*feeder.strip))
    {
        g.setColour (juce::Colours::black);
        g.setFont (fontFittingWidth (capFont (8.0f), stripName (*feeder.strip).toUpperCase(), strip.getWidth() - 4.0f, 0.7f));
        g.drawFittedText (stripName (*feeder.strip).toUpperCase(), strip.toNearestInt().reduced (2, 0), juce::Justification::centred, 1, 0.7f);
    }
    g.setColour (feeder.channel < 0 ? tileOff : Colours::screenTile);
    g.fillRect (r);
    const float u = r.getWidth() / 59.0f, v = r.getHeight() / 55.0f;
    g.setColour (Colours::screenWell);
    g.fillRect (juce::Rectangle<float> (r.getX() + 36.0f * u, r.getY() + 3.0f * v, 20.0f * u, 5.0f * v));
    g.setColour (numberGrey);
    g.setFont (capFont (13.0f));
    g.drawText (juce::String (routing::outputNumber (port)), juce::Rectangle<float> (r.getX(), r.getY() + 12.0f * v, 56.0f * u, 22.0f * v).toNearestInt(),
                juce::Justification::centredRight);
    if (feeder.channel >= 0)
    {
        const bool stereo = feeder.source >= 0 ? sourcePaired (ctx.engine, feeder.source)
                                               : feeder.strip == nullptr || feeder.strip->isOutput() || feeder.strip->params.stereo.load();
        const auto text = signalLabel (feeder);
        auto label = juce::Rectangle<float> (r.getX(), r.getY() + 38.0f * v, r.getWidth(), 15.0f * v);
        g.setColour (stereo ? stereoLabel : monoLabel);
        g.setFont (fontFittingWidth (capFont (9.5f), text, label.getWidth() - 2.0f, 0.7f));
        g.drawText (text, label.toNearestInt(), juce::Justification::centred);
    }
    if (selected)
        drawSelection (g, area);
}

void RoutingPage::paintSignal (juce::Graphics& g, juce::Rectangle<int> area, const Signal& sig, int index, bool marked) const
{
    // [WING 80 x 68] The signal's colour strip over its number ("1L"), a mini
    // meter, the stereo link glyph and the group chip in the signal's colour.
    if (sig.channel < 0)
    {
        paintNaTile (g, area, wingColour (1), juce::String (routing::info (signalGroup).prefix) + " " + juce::String (index + 1), 16.0f / 68.0f, true,
                     false);
        return;
    }
    // A source (an input group, USER SIGNAL): its tile as CHANNELS shows it.
    if (sig.source >= 0)
    {
        routing::paintSourceTile (g, area, ctx.engine, signalGroup, index, 0.0f, marked, false);
        return;
    }
    const auto colour = sig.strip != nullptr ? stripDisplayColour (*sig.strip) : Colours::lime;
    auto r = area.toFloat();
    auto strip = r.removeFromTop (r.getHeight() * 16.0f / 68.0f);
    g.setColour (colour);
    g.fillRect (strip);
    const auto name = sig.strip != nullptr ? (hasCustomName (*sig.strip) ? stripName (*sig.strip).toUpperCase() : juce::String()) : monitorSignalName (sig.channel);
    if (name.isNotEmpty())
    {
        g.setColour (juce::Colours::black);
        g.setFont (fontFittingWidth (capFont (9.0f), name, strip.getWidth() - 4.0f, 0.7f));
        g.drawFittedText (name, strip.toNearestInt().reduced (2, 0), juce::Justification::centred, 1, 0.7f);
    }
    g.setColour (Colours::screenTile);
    g.fillRect (r);
    const float u = r.getWidth() / 80.0f, v = r.getHeight() / 52.0f;
    drawMiniMeter (g, juce::Rectangle<float> (r.getX() + 70.0f * u, r.getY() + 4.0f * v, 4.0f * u, 26.0f * v), 0.0f);

    const bool stereo = sig.strip == nullptr || sig.strip->isOutput() || sig.strip->params.stereo.load();
    juce::String number;
    if (sig.strip == nullptr)
        number = monitorSignalNumber (sig.channel);
    else
        number = (sig.strip->getKind() == StripKind::Aux ? "A" : "") + juce::String (sig.strip->getNumber()) + (stereo ? (sig.channel == 0 ? "L" : "R") : "");
    g.setColour (numberGrey);
    g.setFont (fontFittingWidth (capFont (14.0f), number, 60.0f * u, 0.7f));
    g.drawText (number, juce::Rectangle<float> (r.getX(), r.getY() + 6.0f * v, 66.0f * u, 24.0f * v).toNearestInt(), juce::Justification::centredRight);

    // o-o for a stereo signal, o for a mono one.
    const float d = 7.0f * u, y = r.getY() + 41.0f * v;
    g.setColour (Colours::chipOff);
    g.drawEllipse (juce::Rectangle<float> (d, d).withCentre ({ r.getX() + 10.0f * u, y }), juce::jmax (1.0f, wingPx (1.5f)));
    if (stereo)
    {
        g.drawEllipse (juce::Rectangle<float> (d, d).withCentre ({ r.getX() + 32.0f * u, y }), juce::jmax (1.0f, wingPx (1.5f)));
        g.drawLine (r.getX() + 10.0f * u + d / 2.0f, y, r.getX() + 32.0f * u - d / 2.0f, y, juce::jmax (1.0f, wingPx (1.5f)));
    }
    const char* chip = sig.strip == nullptr                            ? "MON"
                       : sig.strip->isBus()                            ? "BUS"
                       : sig.strip->getKind() == StripKind::Main       ? "MAIN"
                       : sig.strip->getKind() == StripKind::Matrix     ? "MTX"
                       : sig.strip->getKind() == StripKind::Aux        ? "AUX"
                                                                       : "CH";
    drawChip (g, juce::Rectangle<float> (r.getX() + 46.0f * u, r.getY() + 35.0f * v, 30.0f * u, 12.0f * v), chip, colour, juce::Colours::black);
    if (marked)
        drawSelection (g, area);
}

//==============================================================================
ld::SourceParams* RoutingPage::sourceParams (int port) const
{
    return sourceSettings (ctx.engine, port);
}

void RoutingPage::layoutSourcePanel()
{
    // [WING px, page coordinates] The SOURCES settings (fig. 31): the tabs, the
    // banner with the channels using the source, then the boxes: INITIALIZE;
    // the source (group, number, MONO / STEREO / MID/SIDE); REMOTE and RCV
    // CUST; the meter with +48V, POL, MUTE and the GAIN fader.
    sourceColourCells.clear();
    sourceIconCells.clear();
    sourceTagCells.clear();
    const float bottom = juce::jmin (672.0f, (float) getHeight() * 800.0f / juce::jmax (1.0f, getWingScreenHeight()) - 2.0f);
    sourceTabBar = wingRect (728.0f, 0.0f, 544.0f, 58.0f);
    sourceTabKeys[0] = wingRect (728.0f, 0.0f, 158.0f, 56.0f);
    sourceTabKeys[1] = wingRect (888.0f, 0.0f, 158.0f, 56.0f);
    sourceTabKeys[2] = wingRect (1048.0f, 0.0f, 158.0f, 56.0f);
    sourceBanner = wingRect (728.0f, 72.0f, 544.0f, 76.0f);
    sourceBody = wingRect (728.0f, 160.0f, 544.0f, bottom - 160.0f);

    initBox = wingRect (728.0f, 160.0f, 220.0f, 144.0f);
    sourceInitKey = wingRect (744.0f, 204.0f, 186.0f, 56.0f);
    sourceBox = wingRect (728.0f, 312.0f, 220.0f, bottom - 312.0f);
    const float modeTop = juce::jmin (464.0f, bottom - 16.0f - 192.0f);
    modeBox = wingRect (744.0f, modeTop, 188.0f, 192.0f);
    for (int i = 0; i < 3; ++i)
        sourceModeKeys[(size_t) i] = wingRect (744.0f, modeTop + 64.0f * (float) i, 188.0f, 64.0f);

    remoteBox = wingRect (956.0f, 160.0f, 82.0f, 290.0f);
    for (int i = 0; i < 4; ++i)
        remoteKeys[(size_t) i] = wingRect (964.0f, 180.0f + 66.0f * (float) i, 66.0f, 64.0f);
    rcvBox = wingRect (956.0f, bottom - 147.0f, 82.0f, 82.0f);
    rcvKey = rcvBox.reduced (px (8.0f));

    meterBox = wingRect (1046.0f, 160.0f, 226.0f, bottom - 160.0f);
    sourceMeter = wingRect (1060.0f, 200.0f, 56.0f, juce::jmin (360.0f, bottom - 200.0f - 120.0f));
    sourcePhantomKey = wingRect (1200.0f, 184.0f, 48.0f, 48.0f);
    sourcePolKey = wingRect (1200.0f, 288.0f, 48.0f, 48.0f);
    sourceMuteKey = wingRect (1062.0f, bottom - 72.0f, 84.0f, 56.0f);
    sourceGainFader = wingRect (1192.0f, juce::jmax (388.0f, bottom - 284.0f), 64.0f, juce::jmin (268.0f, bottom - 16.0f - 388.0f));
    // An OSC source: SINE / PINK / WHITE in the REMOTE column, FREQ above the
    // LEVEL fader in place of +48V and POL.
    for (int i = 0; i < 3; ++i)
        oscModeKeys[(size_t) i] = remoteKeys[(size_t) i + 1];
    oscFreqField = wingRect (1140.0f, 184.0f, 122.0f, 64.0f);
    oscLevelFader = sourceGainFader;
    // A USR source (no gain, no +48V): SOURCE in place of +48V; a user signal
    // has L+R / L / R in the REMOTE column and TAP (PRE / POST) in place of RCV CUST.
    for (int i = 0; i < 3; ++i)
        userSideKeys[(size_t) i] = remoteKeys[(size_t) i + 1];
    userTapKey = rcvKey;
    userSourceField = wingRect (1126.0f, 184.0f, 138.0f, 64.0f);

    // TAGS: the 8 mute groups the source belongs to (2 x 4).
    {
        auto tags = sourceBody.withTrimmedTop (px (40.0f));
        tags = tags.withHeight (juce::jmin (tags.getHeight(), px (200.0f)));
        const int cw = tags.getWidth() / 4, chh = tags.getHeight() / 2;
        for (int i = 0; i < kNumMuteGroups; ++i)
            sourceTagCells.push_back ({ juce::Rectangle<int> (tags.getX() + (i % 4) * cw, tags.getY() + (i / 4) * chh, cw, chh).reduced (px (4.0f)), i });
    }

    // ICON/COLOR: the 18 colours (6 x 3) and a row of icons.
    auto look = sourceBody.reduced (px (8.0f));
    auto colours = look.removeFromTop (look.getHeight() / 2);
    const int cw = colours.getWidth() / 6, ch = colours.getHeight() / 3;
    for (int i = 0; i < 18; ++i)
        sourceColourCells.push_back ({ juce::Rectangle<int> (colours.getX() + (i % 6) * cw, colours.getY() + (i / 6) * ch, cw, ch).reduced (px (4.0f)), i + 1 });
    look.removeFromTop (px (10.0f));
    static constexpr int icons[] = { 0, 1, 2, 100, 102, 103, 107, 113, 114, 200, 202, 205, 300, 304, 308, 400, 402, 506, 602, 605 };
    const int iw = look.getWidth() / 10, ih = juce::jmin (look.getHeight() / 2, iw);
    for (int i = 0; i < (int) std::size (icons); ++i)
        sourceIconCells.push_back ({ juce::Rectangle<int> (look.getX() + (i % 10) * iw, look.getY() + (i / 10) * ih, iw, ih).reduced (px (3.0f)), icons[i] });
}

void RoutingPage::paintSourcePanel (juce::Graphics& g) const
{
    // The tabs: the selected one #303030 with a gold top line and big gold
    // print, the others #202020 with grey print, black lines between them.
    g.setColour (Colours::screenPanel);
    g.fillRect (sourceTabBar);
    const char* names[] = { "SETTINGS", "ICON/COLOR", "TAGS" };
    for (int i = 0; i < 3; ++i)
    {
        auto t = sourceTabKeys[(size_t) i];
        const bool on = i == sourceTab;
        g.setColour (juce::Colours::black);
        g.fillRect (t.expanded (px (2.0f), 0));
        g.setColour (on ? Colours::screenTile : Colours::screenPanel);
        g.fillRect (t);
        if (on)
        {
            g.setColour (Colours::gold);
            g.fillRect (t.withHeight (px (3.0f)));
        }
        g.setColour (on ? Colours::gold : Dark::caption);
        g.setFont (capFont (on ? 19.0f : 14.0f, on ? WingWeight::SemiBold : WingWeight::Medium));
        g.drawText (names[i], t, juce::Justification::centred);
    }

    // Where the source goes: an arrow and the channels patched to it as tiles.
    g.setColour (Colours::screenPanel);
    g.fillRect (sourceBanner);
    {
        auto b = sourceBanner.reduced (px (6.0f), px (4.0f));
        auto arrowArea = b.removeFromLeft (px (28.0f)).toFloat();
        juce::Path arrow;
        const auto c = arrowArea.getCentre();
        const float a = wingPx (10.0f);
        arrow.addArrow ({ c.x - a, c.y, c.x + a, c.y }, wingPx (6.0f), wingPx (16.0f), wingPx (10.0f));
        g.setColour (Colours::chipOff);
        g.fillPath (arrow);
        auto users = sourceUsers (selectedSource);
        if (users.empty())
        {
            g.setColour (Colours::textDim);
            g.setFont (capFont (13.0f));
            g.drawText ("NO CONNECTIONS", b.withTrimmedLeft (px (8.0f)), juce::Justification::centredLeft);
        }
        // A soloed source also goes to the SOURCE SOLO channel.
        if (auto* host = ctx.engine.getSourceSoloHost(); host != nullptr && ctx.engine.isSourceSoloed (selectedSource))
            users.push_back ({ host, false });
        const int tileW = px (104.0f);
        for (size_t i = 0; i < users.size() && b.getWidth() >= tileW; ++i)
        {
            auto [s, alt] = users[i];
            auto tile = b.removeFromLeft (tileW);
            b.removeFromLeft (px (4.0f));
            g.setColour (juce::Colours::black);
            g.fillRect (tile);
            auto in = tile.reduced (px (2.0f)).toFloat();
            g.setColour (s->getColour());
            g.fillRect (in.removeFromTop (in.getHeight() * 0.36f));
            g.setColour (Colours::screenTile);
            g.fillRect (in);
            g.setColour (Colours::textDim);
            g.setFont (capFont (14.0f));
            g.drawText (shortStripLabel (*s), in.reduced (wingPx (6.0f), 0.0f).toNearestInt(), juce::Justification::centredLeft);
            drawStripIcon (g, *s, in.removeFromRight (in.getHeight()).reduced (in.getHeight() * 0.18f), Colours::textDim);
            if (alt)
                drawChip (g, juce::Rectangle<float> (wingPx (24.0f), wingPx (12.0f)).withCentre (in.getCentre()), "ALT", Colours::alt, juce::Colours::black);
        }
    }

    if (sourceTab == 1)
        paintSourceLook (g);
    else if (sourceTab == 2)
        paintSourceTags (g);
    else
        paintSourceSettings (g);
}

void RoutingPage::paintSourceSettings (juce::Graphics& g) const
{
    for (auto box : { initBox, sourceBox, remoteBox, rcvBox, meterBox })
    {
        g.setColour (Colours::screenPanel);
        g.fillRect (box);
    }
    const auto* src = sourceParams (selectedSource);
    if (src == nullptr)
    {
        g.setColour (Dark::textDim);
        g.setFont (capFont (13.0f));
        g.drawFittedText ("THIS PORT HAS NO SOURCE SETTINGS", sourceBox.reduced (px (8.0f)), juce::Justification::centredTop, 2);
        return;
    }

    drawOutlinedKey (g, sourceInitKey, Icon::Reset, "INITIALIZE", false, Dark::choice);

    // The source: its group, its number in a gold ring between corner marks.
    {
        const auto group = routing::groupOfSource (selectedSource);
        g.setColour (Dark::caption);
        g.setFont (capFont (13.0f));
        g.drawText (routing::info (group).name, sourceBox.withHeight (px (28.0f)).translated (0, px (10.0f)), juce::Justification::centred);
        const auto centre = juce::Point<float> ((float) sourceBox.getCentreX(), (float) sourceBox.getY() + wingPx (86.0f));
        const float rad = wingPx (21.0f);
        g.setColour (Colours::gold);
        g.drawEllipse (juce::Rectangle<float> (rad * 2.0f, rad * 2.0f).withCentre (centre), wingPx (2.5f));
        for (auto off : { juce::Point<float> (-0.8f, -0.8f), juce::Point<float> (0.8f, 0.8f) })
            g.drawEllipse (juce::Rectangle<float> (wingPx (7.0f), wingPx (7.0f)).withCentre (centre + off * rad), wingPx (2.0f));
        g.setColour (Dark::text);
        g.setFont (capFont (12.0f, WingWeight::SemiBold));
        g.drawText (juce::String (routing::numberInGroup (selectedSource) + 1), juce::Rectangle<float> (rad * 2.0f, rad * 2.0f).withCentre (centre).toNearestInt(),
                    juce::Justification::centred);
        // The corner marks.
        g.setColour (Colours::chipOff);
        const float t = wingPx (3.0f), len = wingPx (16.0f), h = wingPx (36.0f);
        const float left = (float) sourceBox.getX() + wingPx (16.0f), right = (float) sourceBox.getRight() - wingPx (16.0f), top = centre.y;
        g.fillRect (left, top, len, t);
        g.fillRect (left, top, t, h);
        g.fillRect (right - len, top, len, t);
        g.fillRect (right - t, top, t, h);
    }

    // MONO / STEREO / MID-SIDE (a pair: ports 1-2, 3-4, ...): in a black box,
    // the chosen one lit blue, each with its glyph.
    {
        const char* modes[] = { "MONO", "STEREO", "MID/SIDE" };
        g.setColour (juce::Colours::black);
        g.fillRect (modeBox);
        for (int i = 0; i < 3; ++i)
        {
            const bool on = src->mode.load() == i;
            auto k = sourceModeKeys[(size_t) i];
            if (on)
            {
                g.setColour (Colours::choice);
                g.fillRect (k);
            }
            const auto ink = on ? juce::Colours::black : Dark::text;
            auto glyph = k.toFloat().withWidth (wingPx (96.0f)).withSizeKeepingCentre (wingPx (60.0f), wingPx (30.0f));
            const float d = glyph.getHeight(), lw = wingPx (2.5f);
            g.setColour (ink);
            if (i == 0)
                g.drawEllipse (juce::Rectangle<float> (d, d).withCentre (glyph.getCentre()).reduced (lw / 2.0f), lw);
            else
            {
                const auto a = juce::Rectangle<float> (d, d).withCentre (glyph.getCentre().translated (-d * 0.45f, 0.0f)).reduced (lw / 2.0f);
                const auto b = juce::Rectangle<float> (d, d).withCentre (glyph.getCentre().translated (d * 0.45f, 0.0f)).reduced (lw / 2.0f);
                if (i == 1)
                {
                    g.setColour (on ? juce::Colours::black : Dark::text);
                    g.fillEllipse (a);
                    g.drawEllipse (b, lw);
                }
                else
                {
                    g.drawEllipse (a.translated (-d * 0.1f, 0.0f), lw);
                    g.drawEllipse (b.translated (d * 0.1f, 0.0f), lw);
                    g.fillRect (juce::Rectangle<float> (lw, d).withCentre (glyph.getCentre()));
                }
            }
            g.setFont (capFont (13.0f));
            g.drawText (modes[i], k.withTrimmedLeft (px (96.0f)), juce::Justification::centredLeft);
        }
    }

    // The meter (after gain, mute and polarity): the scale beside a column of segments.
    {
        auto m = sourceMeter.toFloat();
        auto bar = m.removeFromRight (wingPx (12.0f)).reduced (0.0f, wingPx (4.0f));
        const float db = juce::Decibels::gainToDecibels (selectedSourcePeak, -60.0f);
        const int segments = juce::jmax (8, (int) (bar.getHeight() / juce::jmax (2.0f, wingPx (4.0f))));
        const float segH = bar.getHeight() / (float) segments;
        const float lit = 1.0f - scalePosition (db);
        for (int i = 0; i < segments; ++i)
        {
            const float at = (float) i / (float) segments;
            const bool on = selectedSourcePeak > 0.0f && lit > at;
            g.setColour (on ? (at > 0.95f ? Colours::meterRed : at > 0.66f ? Colours::meterYellow : Colours::meterGreen) : Colours::screenWell);
            g.fillRect (bar.getX(), bar.getBottom() - (float) (i + 1) * segH, bar.getWidth(), juce::jmax (1.0f, segH - 1.0f));
        }
        g.setColour (Dark::caption);
        g.setFont (capFont (8.0f));
        const char* labels[] = { "CLIP", "-6", "-12", "-24", "-36", "-48", "-60" };
        for (int i = 0; i < 7; ++i)
        {
            const float y = bar.getY() + bar.getHeight() * (float) i / 6.0f;
            g.drawText (labels[i], juce::Rectangle<float> (m.getX(), y - wingPx (8.0f), m.getWidth() - wingPx (4.0f), wingPx (16.0f)), juce::Justification::centredRight, false);
        }
        g.drawText ("IN", juce::Rectangle<float> (bar.getX() - wingPx (8.0f), bar.getBottom() + wingPx (2.0f), bar.getWidth() + wingPx (16.0f), wingPx (16.0f)),
                    juce::Justification::centred, false);
    }

    // REMOTE (the AES50 stage boxes' preamps) and RCV CUST don't exist here:
    // OFF stays lit, the other keys are drawn dim. An oscillator puts its type
    // (SINE / PINK / WHITE) in this column, a user signal its side (L+R / L /
    // R) and its TAP; the USR sources have no preamp.
    const auto* osc = selectedOscillator();
    const auto* user = selectedUserSignal();
    const bool usr = MixerEngine::isUsrSource (selectedSource);
    g.setColour (Dark::caption);
    g.setFont (capFont (9.0f));
    g.drawText (osc != nullptr ? "TYPE" : user != nullptr ? "L / R" : usr ? "" : "REMOTE", remoteBox.withHeight (px (20.0f)), juce::Justification::centred);
    if (usr)
    {
        if (user != nullptr)
        {
            const char* sides[] = { "L+R", "L", "R" };
            for (int i = 0; i < 3; ++i)
            {
                const bool on = user->side.load() == i;
                auto k = userSideKeys[(size_t) i];
                g.setColour (on ? Colours::choice : juce::Colours::black);
                g.fillRect (k);
                g.setColour (on ? juce::Colours::black : Dark::text);
                g.setFont (capFont (11.0f));
                g.drawText (sides[i], k, juce::Justification::centred);
            }
            // TAP: PRE (the strip's tap point) or POST, manual 2.6.
            const bool post = user->post.load();
            g.setColour (Colours::choice);
            g.fillRect (userTapKey);
            g.setColour (juce::Colours::black);
            g.setFont (capFont (8.0f));
            g.drawText ("TAP", userTapKey.withHeight (userTapKey.getHeight() / 3), juce::Justification::centred);
            g.setFont (capFont (12.0f, WingWeight::SemiBold));
            g.drawText (post ? "POST" : "PRE", userTapKey.withTrimmedTop (userTapKey.getHeight() / 3), juce::Justification::centred);
        }
    }
    else if (osc != nullptr)
    {
        const char* types[] = { "SINE", "PINK", "WHITE" };
        for (int i = 0; i < 3; ++i)
        {
            const bool on = osc->mode.load() == i;
            auto k = oscModeKeys[(size_t) i];
            g.setColour (on ? Colours::choice : juce::Colours::black);
            g.fillRect (k);
            g.setColour (on ? juce::Colours::black : Dark::text);
            g.setFont (capFont (10.0f));
            g.drawText (types[i], k, juce::Justification::centred);
        }
    }
    else
    {
        const char* remote[] = { "OFF", "AES50 A", "AES50 B", "AES50 C" };
        for (int i = 0; i < 4; ++i)
        {
            auto k = remoteKeys[(size_t) i];
            g.setColour (i == 0 ? Colours::screenLine : juce::Colours::black);
            g.fillRect (k);
            auto f = k.toFloat();
            auto caption = f.removeFromBottom (f.getHeight() * 0.3f);
            const auto ink = i == 0 ? Dark::text : Colours::textDim;
            if (i == 0)
                drawIcon (g, Icon::ClearCircle, f.reduced (f.getHeight() * 0.24f), ink);
            else
                drawGroupIcon (g, i == 1 ? Group::Aes50A : i == 2 ? Group::Aes50B : Group::Aes50C, f.reduced (f.getHeight() * 0.06f), ink);
            g.setColour (ink);
            g.setFont (capFont (9.0f));
            g.drawText (remote[i], caption.toNearestInt(), juce::Justification::centredTop);
        }
    }
    if (! usr)
    {
        g.setColour (juce::Colours::black);
        g.fillRect (rcvKey);
        auto f = rcvKey.toFloat();
        auto caption = f.removeFromBottom (f.getHeight() * 0.3f);
        drawIcon (g, Icon::KeyboardEdit, f.reduced (f.getHeight() * 0.16f), Colours::textDim);
        g.setColour (Colours::textDim);
        g.setFont (capFont (9.0f));
        g.drawText ("RCV CUST", caption.toNearestInt(), juce::Justification::centredTop);
    }

    if (usr)
    {
        // What the user signal or patch copies (manual 2.6); POL and MUTE as
        // any source's, no gain or +48V (the WING's USR group has none).
        g.setColour (Colours::screenWell);
        g.fillRect (userSourceField);
        auto f = userSourceField.reduced (px (6.0f), px (4.0f));
        g.setColour (Dark::caption);
        g.setFont (capFont (9.0f));
        g.drawText ("SOURCE", f.removeFromTop (f.getHeight() / 3), juce::Justification::centredLeft);
        const auto text = userSourceText();
        g.setColour (text == "OFF" ? Dark::textDim : Dark::text);
        g.setFont (fontFittingWidth (capFont (14.0f, WingWeight::SemiBold), text, (float) f.getWidth(), 0.7f));
        g.drawText (text, f, juce::Justification::centredLeft);
        drawIconKeyBelow (g, sourcePolKey, Icon::Invert, "POL", src->invert.load(), Colours::gold);
        drawOutlinedKey (g, sourceMuteKey, std::nullopt, "MUTE", src->mute.load(), Colours::mute, juce::Colours::black);
        return;
    }

    if (osc != nullptr)
    {
        // The oscillator [WING: ROUTING > SOURCES, OSC group]: FREQ (a sine's)
        // and LEVEL; MUTE switches it off. "250 Hz", "1.00 kHz"; dim while a noise plays.
        const auto hz = formatHz (osc->freqHz.load());
        g.setColour (Colours::screenWell);
        g.fillRect (oscFreqField);
        auto f = oscFreqField.reduced (px (6.0f), px (4.0f));
        g.setColour (Dark::caption);
        g.setFont (capFont (9.0f));
        g.drawText ("FREQ", f.removeFromTop (f.getHeight() / 3), juce::Justification::centredLeft);
        g.setColour (osc->mode.load() == ld::kOscSine ? Dark::text : Dark::textDim);
        g.setFont (capFont (14.0f, WingWeight::SemiBold));
        g.drawText (hz.endsWithChar ('k') ? hz.dropLastCharacters (1) + " kHz" : hz + " Hz", f, juce::Justification::centredLeft);
        drawOutlinedKey (g, sourceMuteKey, std::nullopt, "MUTE", src->mute.load(), Colours::mute, juce::Colours::black);
        paintLevelFader (g, oscLevelFader, osc->levelDb.load(), kOscLevelMin, kOscLevelMax);
        return;
    }

    drawIconKeyBelow (g, sourcePhantomKey, Icon::Power, "+48V", src->phantom.load(), Colours::mute, phantomHold.progress());
    drawIconKeyBelow (g, sourcePolKey, Icon::Invert, "POL", src->invert.load(), Colours::gold);
    drawOutlinedKey (g, sourceMuteKey, std::nullopt, "MUTE", src->mute.load(), Colours::mute, juce::Colours::black);
    // GAIN in the group's range (0 dB near the bottom, as on the WING); none
    // for a group the WING gives no gain.
    if (const auto range = gainRangeOf (selectedSource); range.hasGain())
        paintLevelFader (g, sourceGainFader, range.clamp (src->gainDb.load()), range.min, range.max);
}

void RoutingPage::paintLevelFader (juce::Graphics& g, juce::Rectangle<int> area, float value, float min, float max) const
{
    // GAIN (the group's range) or an oscillator's LEVEL (-40 .. -6 dB): in a black
    // frame a dark fader with a black slot, the white value flag on its gold handle.
    g.setColour (juce::Colours::black);
    g.fillRect (area);
    auto f = area.toFloat().reduced (wingPx (2.0f));
    g.setColour (Colours::screenWell);
    g.fillRect (f);
    const float pos = juce::jlimit (0.0f, 1.0f, (value - min) / (max - min));
    auto slot = f.withSizeKeepingCentre (wingPx (10.0f), f.getHeight() - wingPx (20.0f));
    g.setColour (juce::Colours::black);
    g.fillRect (slot);
    const float y = slot.getBottom() - slot.getHeight() * pos;
    g.setColour (Colours::gold);
    g.fillRect (juce::Rectangle<float> (wingPx (12.0f), wingPx (6.0f)).withCentre ({ f.getCentreX(), juce::jlimit (f.getY() + wingPx (14.0f), f.getBottom() - wingPx (3.0f), y + wingPx (10.0f)) }));
    auto flag = juce::Rectangle<float> (juce::jmin (f.getWidth() + wingPx (6.0f), wingPx (60.0f)), wingPx (22.0f))
                    .withCentre ({ f.getCentreX(), juce::jlimit (f.getY() + wingPx (11.0f), f.getBottom() - wingPx (14.0f), y) });
    g.setColour (juce::Colours::black);
    g.fillRect (flag.expanded (1.0f));
    g.setColour (Colours::text);
    g.fillRect (flag);
    g.setColour (juce::Colours::black);
    g.setFont (capFont (10.0f));
    g.drawFittedText (juce::String (value, 1) + " dB", flag.toNearestInt(), juce::Justification::centred, 1, 0.6f);
}

void RoutingPage::paintSourceLook (juce::Graphics& g) const
{
    g.setColour (Colours::screenPanel);
    g.fillRect (sourceBody);
    const auto* src = sourceParams (selectedSource);
    if (src == nullptr)
        return;
    for (auto& [cell, colour] : sourceColourCells)
    {
        g.setColour (wingColour (colour));
        g.fillRect (cell);
        if (colour == src->colour.load())
        {
            g.setColour (Colours::gold);
            g.drawRect (cell.expanded (3), 3);
        }
    }
    for (auto& [cell, icon] : sourceIconCells)
    {
        const bool on = icon == src->icon.load();
        g.setColour (on ? Dark::choice : Dark::key);
        g.fillRect (cell);
        const auto ink = on ? Dark::choiceInk : Dark::text;
        if (icon == 0)
        {
            g.setColour (ink);
            g.setFont (capFont (9.0f));
            g.drawText ("NONE", cell, juce::Justification::centred);
        }
        else
            drawWingIcon (g, icon, cell.toFloat().reduced ((float) cell.getHeight() * 0.15f), ink);
    }
}

void RoutingPage::paintSourceTags (juce::Graphics& g) const
{
    // The mute groups (manual p. 40): while one of them is engaged, the source is muted.
    g.setColour (Colours::screenPanel);
    g.fillRect (sourceBody);
    const auto* src = sourceParams (selectedSource);
    if (src == nullptr)
        return;
    auto title = sourceBody.withHeight (px (34.0f));
    g.setColour (Dark::caption);
    g.setFont (panelTitleFont (title.getHeight()));
    g.drawText ("MUTE GROUPS", title, juce::Justification::centred);
    const auto mask = src->muteGroups.load();
    const auto engaged = ctx.engine.muteGroups.load();
    for (auto& [cell, i] : sourceTagCells)
    {
        const bool on = (mask >> i) & 1u;
        g.setColour (on ? Dark::muteGroup : Dark::key);
        g.fillRoundedRectangle (cell.toFloat(), 2.0f);
        if ((engaged >> i) & 1u)
        {
            g.setColour (Dark::muteGroup);
            g.drawRoundedRectangle (cell.toFloat().reduced (1.0f), 2.0f, 2.0f);
        }
        auto r = cell.reduced (4);
        g.setColour (on ? juce::Colours::black : Dark::caption);
        g.setFont (capFont (9.0f));
        g.drawText ("MGRP " + juce::String (i + 1), r.removeFromTop (r.getHeight() / 3), juce::Justification::centred);
        g.setColour (on ? juce::Colours::black : Dark::text);
        g.setFont (capFont (12.0f));
        g.drawFittedText (ctx.engine.getMuteGroupName (i).toUpperCase(), r, juce::Justification::centred, 1, 0.7f);
    }
    // The source's state.
    g.setColour (Dark::textDim);
    g.setFont (capFont (10.0f));
    const auto state = src->mute.load() ? "MUTED" : ((mask & engaged) != 0 ? "MUTED BY A MUTE GROUP" : "");
    g.drawText (state, sourceBody.withTrimmedTop (sourceTagCells.empty() ? 0 : sourceTagCells.back().first.getBottom() - sourceBody.getY() + 10).withHeight (px (24.0f)),
                juce::Justification::centred);
}

void RoutingPage::selectSource (int port)
{
    selectedSource = port;
    selectedSources.clear(); // a touch selects one source; a drag selects more
    // A source solo follows the selection (firmware 1.12).
    if (ctx.engine.isSourceSoloActive())
        ctx.engine.setSourceSoloPort (port);
}

void RoutingPage::renameSource()
{
    const int index = MixerEngine::sourceIndex (selectedSource);
    if (index < 0)
        return;
    popups::TextOptions options;
    options.actionText = "RENAME";
    juce::Component::SafePointer<RoutingPage> safe (this);
    popups::editText (*this, "RENAME SOURCE", ctx.engine.getSourceName (index), [safe, index] (const juce::String& text)
    {
        if (safe == nullptr)
            return;
        safe->ctx.engine.setSourceName (index, text.trim());
        safe->repaint();
        safe->notifyStatusKeysChanged();
    }, options);
}

void RoutingPage::setSourceGainFromY (int y)
{
    const auto fader = levelFader();
    auto slot = fader.toFloat().reduced (wingPx (2.0f)).withSizeKeepingCentre (wingPx (10.0f), (float) fader.getHeight() - wingPx (24.0f));
    const float pos = juce::jlimit (0.0f, 1.0f, (slot.getBottom() - (float) y) / juce::jmax (1.0f, slot.getHeight()));
    if (auto* osc = selectedOscillator())
        osc->levelDb = std::round ((kOscLevelMin + pos * (kOscLevelMax - kOscLevelMin)) * 2.0f) / 2.0f;
    else if (const auto range = gainRangeOf (selectedSource); range.hasGain())
    {
        const float gain = range.fromProportion (pos); // on the group's steps
        // Every selected source, in its own group's range.
        forSelectedSources ([gain] (int i, ld::SourceParams& s) { s.gainDb = MixerEngine::sourceGainRange (i).clamp (gain); });
    }
    repaint();
}

juce::Rectangle<int> RoutingPage::levelFader() const
{
    return MixerEngine::isOscSource (selectedSource) ? oscLevelFader : sourceGainFader;
}

ld::OscParams* RoutingPage::selectedOscillator() const
{
    const int n = MixerEngine::oscChannelOf (selectedSource);
    if (n < 0)
        return nullptr;
    // A pair runs on OSC 1's settings.
    const auto* src = ctx.engine.getSource (MixerEngine::sourceIndex (selectedSource));
    return ctx.engine.getOscillator (src != nullptr && src->mode.load() != ld::kSourceMono ? 0 : n);
}

ld::UserSignalParams* RoutingPage::selectedUserSignal() const
{
    return ctx.engine.getUserSignal (MixerEngine::usrChannelOf (selectedSource));
}

ld::UserPatchParams* RoutingPage::selectedUserPatch() const
{
    const int n = MixerEngine::usrChannelOf (selectedSource);
    return n >= ld::kNumUserSignals ? ctx.engine.getUserPatch (n - ld::kNumUserSignals) : nullptr;
}

namespace
{
    /** A user signal's SOURCE groups (/io/in/USR/n/user/grp) and their strips. */
    constexpr StripKind kUserKinds[] = { StripKind::Channel, StripKind::Aux, StripKind::Bus, StripKind::Main, StripKind::Matrix };
    const char* const kUserGroups[] = { "OFF", "CH", "AUX", "BUS", "MAIN", "MTX" };
    /** The groups a user patch copies from (the WING's LCL ... AES that Sidecar has). */
    constexpr routing::Group kPatchGroups[] = { Group::LocalIn, Group::Aes50B, Group::Aes50C, Group::WlivePlay, Group::UsbPlayer };
} // namespace

juce::String RoutingPage::userSourceText() const
{
    if (const auto* u = selectedUserSignal())
    {
        const int group = u->group.load();
        if (! juce::isPositiveAndBelow (group - 1, (int) std::size (kUserKinds)))
            return "OFF";
        return juce::String (kUserGroups[group]) + " " + juce::String (u->number.load());
    }
    if (const auto* p = selectedUserPatch())
        return p->source.load() >= 0 ? sourceName (p->source.load()) : juce::String ("OFF");
    return {};
}

void RoutingPage::showUserSourceMenu()
{
    // Two lists, as the WING's SOURCE GROUP then its number: a user signal's
    // group (OFF, CH ... MTX) then the strip; a user patch's group then the port.
    juce::Component::SafePointer<RoutingPage> safe (this);
    auto& anchorComp = anchorAt (userSourceField);
    if (auto* u = selectedUserSignal())
    {
        juce::StringArray groups;
        for (auto* gname : kUserGroups)
            groups.add (gname);
        popups::ListOptions options;
        options.caption = "SOURCE GROUP";
        popups::chooseFrom (anchorComp, groups, u->group.load(), [safe, u] (int group)
        {
            if (safe == nullptr || group < 0)
                return;
            if (group == 0)
            {
                u->group = 0;
                safe->repaint();
                return;
            }
            juce::MessageManager::callAsync ([safe, u, group]
            {
                if (safe == nullptr)
                    return;
                const auto kind = kUserKinds[(size_t) group - 1];
                juce::StringArray items;
                std::vector<int> numbers;
                for (auto& s : safe->ctx.engine.getStrips (kind))
                {
                    items.add (shortStripLabel (*s));
                    numbers.push_back (s->getNumber());
                }
                const int current = u->group.load() == group ? (int) (std::find (numbers.begin(), numbers.end(), u->number.load()) - numbers.begin()) : -1;
                popups::ListOptions list;
                list.columns = items.size() > 30 ? 4 : items.size() > 12 ? 2 : 1;
                list.caption = kUserGroups[group];
                popups::chooseFrom (safe->anchorAt (safe->userSourceField), items, current, [safe, u, group, numbers] (int i)
                {
                    if (safe == nullptr || ! juce::isPositiveAndBelow (i, (int) numbers.size()))
                        return;
                    u->number = numbers[(size_t) i];
                    u->group = group;
                    safe->repaint();
                }, list);
            });
        }, options);
        return;
    }
    const int n = MixerEngine::usrChannelOf (selectedSource) - ld::kNumUserSignals;
    if (selectedUserPatch() == nullptr)
        return;
    juce::StringArray groups { "OFF" };
    for (auto g : kPatchGroups)
        groups.add (routing::info (g).name);
    popups::ListOptions options;
    options.caption = "SOURCE GROUP";
    const int patched = selectedUserPatch()->source.load();
    int current = 0;
    for (int i = 0; i < (int) std::size (kPatchGroups); ++i)
        if (patched >= 0 && routing::groupOfSource (patched) == kPatchGroups[(size_t) i])
            current = i + 1;
    popups::chooseFrom (anchorComp, groups, current, [safe, n] (int group)
    {
        if (safe == nullptr || group < 0)
            return;
        if (group == 0)
        {
            safe->ctx.engine.setUserPatch (n, -1);
            safe->repaint();
            return;
        }
        juce::MessageManager::callAsync ([safe, n, group]
        {
            if (safe == nullptr)
                return;
            const auto g = kPatchGroups[(size_t) group - 1];
            juce::StringArray items;
            std::vector<int> ports;
            for (int i = 0; i < routing::tileCount (g, safe->numHardware (false)); ++i)
                if (const int port = routing::sourceAt (g, i); port >= 0)
                {
                    items.add (safe->sourceName (port));
                    ports.push_back (port);
                }
            const auto* p = safe->ctx.engine.getUserPatch (n);
            const int now = p != nullptr ? (int) (std::find (ports.begin(), ports.end(), p->source.load()) - ports.begin()) : -1;
            popups::ListOptions list;
            list.columns = items.size() > 30 ? 4 : items.size() > 12 ? 2 : 1;
            list.caption = routing::info (g).name;
            popups::chooseFrom (safe->anchorAt (safe->userSourceField), items, now, [safe, n, ports] (int i)
            {
                if (safe == nullptr || ! juce::isPositiveAndBelow (i, (int) ports.size()))
                    return;
                safe->ctx.engine.setUserPatch (n, ports[(size_t) i]);
                safe->repaint();
            }, list);
        });
    }, options);
}

bool RoutingPage::isSelectedSource (int port) const
{
    return port == selectedSource || std::find (selectedSources.begin(), selectedSources.end(), port) != selectedSources.end();
}

void RoutingPage::forSelectedSources (const std::function<void (int, ld::SourceParams&)>& set)
{
    // The selected source and the others a drag selected (manual 5.4:
    // "Parameter changes are applied to all selected Sources"); a pair's
    // partner with each, as one source's settings.
    std::vector<int> ports { selectedSource };
    for (int p : selectedSources)
        if (p != selectedSource)
            ports.push_back (p);
    std::vector<int> done;
    for (int port : ports)
    {
        const int index = MixerEngine::sourceIndex (port);
        auto* src = ctx.engine.getSource (index);
        if (src == nullptr)
            continue;
        const bool paired = src->mode.load() != ld::kSourceMono;
        for (int i : { paired ? (index & ~1) : index, paired ? (index | 1) : -1 })
            if (auto* s = ctx.engine.getSource (i); s != nullptr && std::find (done.begin(), done.end(), i) == done.end())
            {
                done.push_back (i);
                set (i, *s);
            }
    }
}

void RoutingPage::dragSelectTo (int tile)
{
    if (dragStartTile < 0 || tile < 0)
        return;
    selectedSources.clear();
    for (int i = juce::jmin (dragStartTile, tile); i <= juce::jmax (dragStartTile, tile); ++i)
        if (const int port = routing::sourceAt (inputGroup, i); port >= 0)
            selectedSources.push_back (port);
    dragSelected = dragSelected || tile != dragStartTile;
    repaint();
}

void RoutingPage::showOscFreqMenu()
{
    // The usual test frequencies; the knob and the wheel set any other.
    static constexpr float freqs[] = { 20.0f, 31.5f, 50.0f, 63.0f, 100.0f, 125.0f, 250.0f, 440.0f, 500.0f, 1000.0f,
                                       2000.0f, 4000.0f, 8000.0f, 10000.0f, 12500.0f, 16000.0f, 20000.0f };
    auto* osc = selectedOscillator();
    if (osc == nullptr)
        return;
    juce::StringArray items;
    int current = -1;
    for (int i = 0; i < (int) std::size (freqs); ++i)
    {
        const auto hz = formatHz (freqs[i]);
        items.add (hz.endsWithChar ('k') ? hz.dropLastCharacters (1) + " kHz" : hz + " Hz");
        if (std::abs (osc->freqHz.load() - freqs[i]) < 0.01f)
            current = i;
    }
    popups::ListOptions options;
    options.columns = 4;
    options.caption = "FREQ";
    juce::Component::SafePointer<RoutingPage> safe (this);
    popups::chooseFrom (anchorAt (oscFreqField), items, current, [safe] (int i)
    {
        if (safe == nullptr || ! juce::isPositiveAndBelow (i, (int) std::size (freqs)))
            return;
        if (auto* o = safe->selectedOscillator())
            o->freqHz = freqs[i];
        safe->repaint();
    }, options);
}

std::vector<KnobBinding> RoutingPage::getKnobs()
{
    if (mode != Mode::Sources)
        return {};
    auto* src = sourceParams (selectedSource);
    if (src == nullptr)
        return {};
    if (auto* osc = selectedOscillator())
    {
        // An oscillator: LEVEL, FREQ and the type.
        auto level = bindLinear ("LEVEL", osc->levelDb, kOscLevelMin, kOscLevelMax, " dB", 1, -20.0f);
        level.steps = 69.0f; // 0.5 dB steps, as the WING
        return { level, bindFreq ("FREQ", osc->freqHz, 20.0f, 20000.0f, 1000.0f), bindChoice ("TYPE", osc->mode, { "SINE", "PINK", "WHITE" }) };
    }
    // A stereo / mid-side pair has one gain (the engine plays the first
    // source's): the knob sets both, as the fader and the wheel do.
    // The group's range and steps; a group the WING gives no gain has no knob.
    const auto range = gainRangeOf (selectedSource);
    if (! range.hasGain())
        return {};
    auto gain = bindLinear ("GAIN", src->gainDb, range.min, range.max, " dB", 1, 0.0f);
    gain.get = [src, range] { return range.toProportion (src->gainDb.load()); };
    // Every selected source takes it (manual 5.4), each in its group's range.
    juce::Component::SafePointer<RoutingPage> safe (this);
    gain.set = [safe, range] (float p)
    {
        const float g = range.fromProportion (p);
        if (safe != nullptr)
            safe->forSelectedSources ([g] (int i, ld::SourceParams& s) { s.gainDb = MixerEngine::sourceGainRange (i).clamp (g); });
    };
    gain.text = [src, range] { return juce::String (range.clamp (src->gainDb.load()), 1) + " dB"; };
    gain.steps = (float) range.steps(); // as the WING: 20 (LOCAL IN) or 98 (AES50) values
    gain.bipolar = false;               // a level: a thumb on the bar, not a selector's segments
    gain.reset = [safe]
    {
        if (safe != nullptr)
            safe->forSelectedSources ([] (int, ld::SourceParams& s) { s.gainDb = 0.0f; });
    };
    return { gain };
}

void RoutingPage::mouseDown (const juce::MouseEvent& e)
{
    draggingGain = mode == Mode::Sources && sourceTab == 0 && levelFader().contains (e.getPosition()) && sourceParams (selectedSource) != nullptr;
    if (draggingGain)
        setSourceGainFromY (e.y);
    // SOURCES: a drag over the tiles selects several sources (manual 5.4).
    dragStartTile = -1;
    dragSelected = false;
    if (mode == Mode::Sources)
        for (size_t i = 0; i < leftPorts.size(); ++i)
            if (leftPorts[i].contains (e.getPosition()) && routing::sourceAt (inputGroup, (int) i) >= 0)
                dragStartTile = (int) i;
    // +48V switches when held (kPhantomHoldSeconds), as on the console; an
    // oscillator has FREQ there.
    if (mode == Mode::Sources && sourceTab == 0 && selectedOscillator() == nullptr && ! MixerEngine::isUsrSource (selectedSource)
        && sourceParams (selectedSource) != nullptr && sourcePhantomKey.contains (e.getPosition()))
        phantomHold.press();
}

void RoutingPage::mouseDrag (const juce::MouseEvent& e)
{
    if (draggingGain)
        setSourceGainFromY (e.y);
    if (dragStartTile >= 0)
        for (size_t i = 0; i < leftPorts.size(); ++i)
            if (leftPorts[i].contains (e.getPosition()) && ((int) i != dragStartTile || dragSelected))
            {
                if (! dragSelected)
                    selectSource (routing::sourceAt (inputGroup, dragStartTile)); // the first tile's settings show
                dragSelectTo ((int) i);
                notifyStatusKeysChanged();
                notifyKnobsChanged();
            }
    if (! sourcePhantomKey.contains (e.getPosition()))
        phantomHold.release();
}

void RoutingPage::switchPhantom()
{
    if (const auto* src = sourceParams (selectedSource))
    {
        const bool on = ! src->phantom.load();
        forSelectedSources ([on] (int, ld::SourceParams& s) { s.phantom = on; });
    }
    repaint();
    notifyStatusKeysChanged();
}

void RoutingPage::mouseWheelMove (const juce::MouseEvent& e, const juce::MouseWheelDetails& w)
{
    if (auto* osc = selectedOscillator(); osc != nullptr && mode == Mode::Sources && sourceTab == 0)
    {
        // LEVEL in 0.5 dB steps, FREQ in 1/12 octave.
        const float dir = w.deltaY > 0.0f ? 1.0f : -1.0f;
        if (oscLevelFader.contains (e.getPosition()))
            osc->levelDb = juce::jlimit (kOscLevelMin, kOscLevelMax, osc->levelDb.load() + dir * 0.5f);
        else if (oscFreqField.contains (e.getPosition()))
            osc->freqHz = juce::jlimit (20.0f, 20000.0f, osc->freqHz.load() * std::pow (2.0f, dir / 12.0f));
        repaint();
        return;
    }
    if (mode == Mode::Sources && sourceTab == 0 && sourceGainFader.contains (e.getPosition()))
        if (auto* src = sourceParams (selectedSource); src != nullptr && gainRangeOf (selectedSource).hasGain())
        {
            // One of the group's steps.
            const auto range = gainRangeOf (selectedSource);
            const float gain = range.clamp (range.clamp (src->gainDb.load()) + (w.deltaY > 0.0f ? range.step : -range.step));
            forSelectedSources ([gain] (int i, ld::SourceParams& s) { s.gainDb = MixerEngine::sourceGainRange (i).clamp (gain); });
            repaint();
        }
}

//==============================================================================
void RoutingPage::paintHeaders (juce::Graphics& g) const
{
    g.setColour (Colours::screenPanel);
    g.fillRect (leftHeader);
    if (mode != Mode::Sources)
        g.fillRect (rightHeader);

    if (mode == Mode::Channels)
    {
        // CHANNEL INPUT: which input of the channel the source grid patches.
        drawCaptionBar (g, inputCaption, "CHANNEL INPUT");
        drawKey (g, mainKey, "MAIN", ! editAlt, Colours::valueBlue);
        drawKey (g, altKey, "ALT", editAlt, Colours::valueBlue);
        if (routing::showsPatchKeys (lock))
        {
            // The pairing of the source the channel's input (MAIN or ALT) is on.
            const auto* src = ctx.engine.getSource (patchedSourceIndex());
            const int pairing = src != nullptr ? (int) src->mode.load() : -1;
            drawKey (g, monoKey, "M", pairing == ld::kSourceMono, Colours::valueBlue);
            drawKey (g, stereoKey, "ST", pairing == ld::kSourceStereo, Colours::valueBlue);
            drawKey (g, midSideKey, "M/S", pairing == ld::kSourceMidSide, Colours::valueBlue);
        }
        drawKey (g, prevPage, "<", false, Colours::valueBlue);
        drawKey (g, nextPage, ">", false, Colours::valueBlue);
    }
    if (mode == Mode::Sources)
    {
        drawSelector (g, leftSelector, inputGroup, "SOURCE GROUP");
        drawIconKey (g, renameKey, Icon::Edit, "RENAME", sourceParams (selectedSource) != nullptr);
        if (ctx.engine.monitor.sourceSolo.load() != ld::kSourceSoloOff)
            drawKey (g, srcSoloKey, "SRC SOLO", ctx.engine.isSourceSoloed (selectedSource), Colours::gold);
        return;
    }

    if (mode == Mode::Outputs)
        drawSelector (g, leftSelector, outputGroup, "OUTPUT GROUP");
    // +1 AUTO: grey print, lit yellow.
    if (autoAdvance)
    {
        g.setColour (autoYellow);
        g.fillRect (autoKey);
    }
    g.setColour (autoAdvance ? juce::Colours::black : Dark::caption);
    g.setFont (capFont (13.0f));
    g.drawText ("+1", autoKey.withHeight (autoKey.getHeight() / 2).translated (0, px (4.0f)), juce::Justification::centred);
    g.drawText ("AUTO", autoKey.withTrimmedTop (autoKey.getHeight() / 2), juce::Justification::centred);
    // The lock pair in a black frame: locked lights the closed lock red,
    // unlocked the open one green; the other stays #303030.
    g.setColour (juce::Colours::black);
    g.fillRect (lockFrame);
    const bool locked = lock.isLocked();
    g.setColour (locked ? lockRed : Colours::screenTile);
    g.fillRect (lockKey);
    drawIcon (g, Icon::LockClosed, lockKey.toFloat().reduced ((float) lockKey.getHeight() * 0.2f), locked ? juce::Colours::black : Colours::textDim);
    g.setColour (! locked ? Dark::post : Colours::screenTile);
    g.fillRect (unlockKey);
    drawIcon (g, Icon::LockOpen, unlockKey.toFloat().reduced ((float) unlockKey.getHeight() * 0.2f), ! locked ? juce::Colours::black : Colours::textDim);

    drawSelector (g, rightSelector, mode == Mode::Channels ? inputGroup : signalGroup, "SOURCE GROUP");
    if (routing::showsPatchKeys (lock))
        drawIconKey (g, noneKey, Icon::ClearCircle, "NONE");
    if (mode == Mode::Channels)
    {
        // Source customization doesn't exist yet.
        drawCaptionBar (g, copyCaption, "COPY CUSTOMIZATION");
        drawKey (g, srcToChKey, "SRC->CH", false, Colours::valueBlue);
        drawKey (g, chToSrcKey, "CH->SRC", false, Colours::valueBlue);
    }
    else
    {
        // The tiles' meter scale.
        auto legend = meterLegend;
        auto labels = legend.removeFromBottom (legend.getHeight() / 2);
        const char* marks[] = { "-60", "-48", "-36", "-24", "-12", "-6", "CLIP" };
        const int segments = 18;
        const float segW = (float) legend.getWidth() / (float) segments;
        g.setColour (Colours::screenTile);
        for (int i = 0; i < segments; ++i)
            g.fillRect (juce::Rectangle<float> ((float) legend.getX() + (float) i * segW, (float) legend.getY() + (float) legend.getHeight() * 0.3f,
                                                juce::jmax (1.0f, segW - wingPx (3.0f)), (float) legend.getHeight() * 0.6f));
        g.setColour (Dark::caption);
        g.setFont (capFont (8.0f));
        const float step = (float) labels.getWidth() / 7.0f;
        for (int i = 0; i < 7; ++i)
            g.drawText (marks[i], juce::Rectangle<float> ((float) labels.getX() + step * (float) i, (float) labels.getY(), step, (float) labels.getHeight()).toNearestInt(),
                        juce::Justification::centred);
        drawKey (g, prevSignals, "<", false, Colours::valueBlue);
        drawKey (g, nextSignals, ">", false, Colours::valueBlue);
    }
}

void RoutingPage::paint (juce::Graphics& g)
{
    g.fillAll (juce::Colours::black);
    paintHeaders (g);

    // The tile boxes: black inside a 4 px #202020 frame.
    for (auto box : { leftGrid, rightGrid })
        if (! box.isEmpty())
        {
            g.setColour (Colours::screenPanel);
            g.fillRect (box);
            g.setColour (juce::Colours::black);
            g.fillRect (box.reduced (px (4.0f)));
        }
    if (! separator.isEmpty())
    {
        g.setColour (Colours::screenPanel);
        g.fillRect (separator);
    }

    auto* sel = selectedStrip();
    if (mode == Mode::Channels)
    {
        for (auto& [area, strip] : channelTiles)
            paintChannelTile (g, area, *strip, strip == sel);
        for (size_t i = 0; i < rightPorts.size(); ++i)
        {
            const int port = routing::sourceAt (inputGroup, (int) i);
            bool mine = false;
            if (sel != nullptr && port >= 0)
            {
                const int l = editAlt ? sel->getAltInputLeft() : sel->getInputLeft(), r = editAlt ? sel->getAltInputRight() : sel->getInputRight();
                mine = l == port || (sel->params.stereo.load() && (r == port || (r < 0 && l >= 0 && l + 1 == port)));
            }
            paintSourceTile (g, rightPorts[i], inputGroup, (int) i, mine, (int) i);
        }
    }
    else if (mode == Mode::Sources)
    {
        for (size_t i = 0; i < leftPorts.size(); ++i)
        {
            const int port = routing::sourceAt (inputGroup, (int) i);
            paintSourceTile (g, leftPorts[i], inputGroup, (int) i, port >= 0 && isSelectedSource (port), (int) i);
        }
        paintSourcePanel (g);
    }
    else
    {
        for (size_t i = 0; i < leftPorts.size(); ++i)
            paintOutputTile (g, leftPorts[i], outputGroup, (int) i, routing::outputAt (outputGroup, (int) i) == selectedPort);
        const auto feeder = feederOf (selectedPort);
        for (size_t i = 0; i < signalTiles.size(); ++i)
        {
            const auto& [area, sig] = signalTiles[i];
            const bool feeds = sig.channel >= 0 && feeder.channel >= 0 && feeder.strip == sig.strip && feeder.channel == sig.channel && feeder.source == sig.source;
            paintSignal (g, area, sig, signalPage * kSignalsPerPage + (int) i, feeds);
        }
    }

    if (lock.isLocked() && mode != Mode::Sources)
    {
        g.setColour (juce::Colours::black.withAlpha (0.25f));
        g.fillRect (rightGrid);
    }
}

//==============================================================================
void RoutingPage::assignInput (int port, bool right)
{
    auto* s = selectedStrip();
    if (s == nullptr || lock.isLocked())
        return;
    auto& p = s->params;
    if (editAlt)
    {
        // The ALT input (WING INPUT SELECT), e.g. a recorder playback channel.
        if (right)
            s->setAltInput (s->getAltInputLeft(), port);
        else
            s->setAltInput (port);
    }
    else if (right)
        p.inputRight = port;
    else if (MixerEngine::isStripSource (port))
    {
        // A bus, main or matrix is a stereo source (the WING's $BUS... mode ST):
        // the channel takes its L and R, in stereo.
        const int first = port - (port - ld::kStripSourceBase) % 2;
        p.inputLeft = first;
        p.inputRight = first + 1;
        p.stereo = true;
    }
    else if (auto* src = sourceParams (port); src != nullptr && src->mode.load() != ld::kSourceMono)
    {
        // A stereo or mid/side source: the channel takes the pair, in stereo.
        const int first = MixerEngine::sourcePort (MixerEngine::sourceIndex (port) & ~1);
        p.inputLeft = first;
        p.inputRight = first + 1;
        p.stereo = true;
    }
    else
    {
        p.inputLeft = port;
        p.inputRight = -1; // stereo channels take the next input
    }

    if (autoAdvance && ! right)
    {
        const auto list = channelTargets();
        for (size_t i = 0; i + 1 < list.size(); ++i)
            if (list[i] == s)
            {
                select (list[i + 1]);
                break;
            }
    }
    repaint();
}

void RoutingPage::clearOutput (int port)
{
    if (routing::isRecorderPort (port))
    {
        ctx.engine.getUsbRecorder().setTap (port - routing::kRecorderPortBase, {});
        ctx.engine.getUsbRecorder().saveSettings (ctx.settings);
        return;
    }
    // Strips, the monitor paths and a source patched there.
    ctx.engine.clearOutput (port);
}

void RoutingPage::assignOutput (int port, const Signal& sig)
{
    if (lock.isLocked() || port < 0 || sig.channel < 0)
        return;
    // One signal per output. A strip has one output pair, so patching it to a
    // new output moves it there (engine item E4: many outputs per signal).
    // The RECORDER ports tap the signal instead, leaving its outputs alone.
    if (routing::isRecorderPort (port))
    {
        UsbRecorder::Tap tap;
        if (sig.source >= 0)
        {
            tap.kind = UsbRecorder::Tap::Kind::Source;
            tap.source = sig.source;
        }
        else if (sig.strip != nullptr)
        {
            tap.kind = UsbRecorder::Tap::Kind::Strip;
            tap.stripKind = sig.strip->getKind();
            auto& strips = ctx.engine.getStrips (tap.stripKind);
            for (size_t i = 0; i < strips.size(); ++i)
                if (strips[i].get() == sig.strip)
                    tap.stripNumber = sig.strip->isBus() ? sig.strip->getId() + 1 : (int) i + 1;
            tap.channel = sig.channel;
        }
        ctx.engine.getUsbRecorder().setTap (port - routing::kRecorderPortBase, tap);
        ctx.engine.getUsbRecorder().saveSettings (ctx.settings);
    }
    else if (sig.source >= 0)
        ctx.engine.routeOutputSource (port, sig.source); // replaces what fed the output
    else
    {
        clearOutput (port);
        if (sig.strip == nullptr)
            monitorOutput (ctx.engine, sig.channel) = port;
        else
            (sig.channel == 0 ? sig.strip->params.outputLeft : sig.strip->params.outputRight) = port;
    }
    // +1 AUTO: on to the group's next output.
    if (autoAdvance)
        for (int i = 0; i + 1 < (int) leftPorts.size(); ++i)
            if (routing::outputAt (outputGroup, i) == port && routing::outputAt (outputGroup, i + 1) >= 0)
            {
                selectedPort = routing::outputAt (outputGroup, i + 1);
                notifyStatusKeysChanged();
                break;
            }
    repaint();
}

juce::Component& RoutingPage::anchorAt (juce::Rectangle<int> area)
{
    anchor.setBounds (area);
    return anchor;
}

void RoutingPage::showGroupMenu (bool rightSide)
{
    // The menu opens over the tile box under its selector.
    routing::Menu menu;
    Group current = inputGroup;
    if (mode == Mode::Outputs)
    {
        const bool compact = ctx.consoleModel == ConsoleModel::Compact;
        menu = rightSide ? routing::sourceMenu (true, true, compact) : routing::outputMenu (compact);
        current = rightSide ? signalGroup : outputGroup;
    }
    else
        menu = routing::sourceMenu (mode == Mode::Channels, false, ctx.consoleModel == ConsoleModel::Compact);
    const auto box = rightSide ? rightGrid : leftGrid;
    const auto origin = juce::Point<int> ((rightSide ? rightPanel : leftPanel).getX() + px (8.0f), box.getY());
    juce::Component::SafePointer<RoutingPage> safe (this);
    groupMenu = std::make_unique<GroupMenu> (std::move (menu), (int) current, origin, [safe, rightSide] (int group)
    {
        // Closing the menu deletes it: finish on the message loop.
        juce::MessageManager::callAsync ([safe, rightSide, group]
        {
            if (safe == nullptr)
                return;
            safe->groupMenu.reset();
            if (group >= 0)
                safe->chooseGroup (rightSide, group);
            safe->repaint();
        });
    });
    {
        const auto side = rightSide ? rightPanel : leftPanel;
        groupMenu->dimArea = getLocalBounds().withLeft (side.getX() - px (4.0f)).withRight (side.getRight() + px (4.0f));
    }
    addAndMakeVisible (*groupMenu);
    groupMenu->setBounds (getLocalBounds());
}

void RoutingPage::chooseGroup (bool rightSide, int group)
{
    const auto g = (Group) group;
    if (mode == Mode::Outputs && rightSide)
    {
        signalGroup = g;
        signalPage = 0;
        resized(); // the page keys
    }
    else if (mode == Mode::Outputs)
    {
        outputGroup = g;
        if (const int first = routing::outputAt (g, 0); first >= 0)
            selectedPort = first;
    }
    else
    {
        inputGroup = g;
        // SOURCES: the group's first source.
        if (const int first = routing::sourceAt (g, 0); mode == Mode::Sources && first >= 0)
        {
            selectSource (first);
            notifyKnobsChanged();
        }
    }
    layoutTiles();
    notifyStatusKeysChanged();
}

void RoutingPage::mouseUp (const juce::MouseEvent& e)
{
    const auto pos = e.getPosition();
    phantomHold.release();
    if (draggingGain)
    {
        draggingGain = false;
        return;
    }
    // The end of a drag over the source tiles: the selection stays (a touch selects one again).
    const bool dragged = dragSelected;
    dragStartTile = -1;
    dragSelected = false;
    if (dragged)
    {
        repaint();
        return;
    }
    if (mode == Mode::Sources && rightPanel.contains (pos))
    {
        // The SOURCES settings panel.
        for (int i = 0; i < 3; ++i)
            if (sourceTabKeys[(size_t) i].contains (pos))
            {
                sourceTab = i;
                repaint();
                return;
            }
        auto* src = sourceParams (selectedSource);
        if (src == nullptr)
            return;
        // A stereo or mid/side pair shares its settings: set both sources;
        // and every selected source (manual 5.4).
        auto flip = [this, src] (std::atomic<bool> ld::SourceParams::*field)
        {
            const bool on = ! (src->*field).load();
            forSelectedSources ([field, on] (int, ld::SourceParams& s) { (s.*field) = on; });
        };
        // The selected sources' table indices (each source once, not its pair partner).
        auto selectedIndices = [this]
        {
            std::vector<int> out { MixerEngine::sourceIndex (selectedSource) };
            for (int p : selectedSources)
                if (p != selectedSource)
                    out.push_back (MixerEngine::sourceIndex (p));
            return out;
        };
        if (sourceTab == 0)
        {
            auto* osc = selectedOscillator();
            auto* user = selectedUserSignal();
            const bool usr = MixerEngine::isUsrSource (selectedSource);
            if (sourceInitKey.contains (pos))
            {
                for (int i : selectedIndices())
                    ctx.engine.initializeSource (i);
            }
            else if (usr && userSourceField.contains (pos))
                showUserSourceMenu();
            else if (user != nullptr && userTapKey.contains (pos))
                user->post = ! user->post.load();
            else if (user != nullptr && std::any_of (userSideKeys.begin(), userSideKeys.end(), [pos] (auto k) { return k.contains (pos); }))
            {
                for (int i = 0; i < 3; ++i)
                    if (userSideKeys[(size_t) i].contains (pos))
                        user->side = i;
            }
            else if (osc != nullptr && oscFreqField.contains (pos))
                showOscFreqMenu();
            else if (osc != nullptr && ! sourceMuteKey.contains (pos))
            {
                for (int m = 0; m < 3; ++m)
                    if (oscModeKeys[(size_t) m].contains (pos))
                        osc->mode = m;
            }
            else if (sourcePolKey.contains (pos))
                flip (&ld::SourceParams::invert);
            else if (sourceMuteKey.contains (pos))
                flip (&ld::SourceParams::mute);
            for (int m = 0; m < 3; ++m)
                if (sourceModeKeys[(size_t) m].contains (pos))
                    for (int i : selectedIndices())
                        ctx.engine.setSourceMode (i, m);
        }
        else if (sourceTab == 2)
        {
            for (auto& [cell, group] : sourceTagCells)
                if (cell.contains (pos))
                {
                    // The group on (or off) for every selected source, as for the first.
                    const auto bit = 1u << group;
                    const bool on = (src->muteGroups.load() & bit) == 0;
                    forSelectedSources ([bit, on] (int, ld::SourceParams& s) { s.muteGroups = on ? (s.muteGroups.load() | bit) : (s.muteGroups.load() & ~bit); });
                }
        }
        else
        {
            for (auto& [cell, colour] : sourceColourCells)
                if (cell.contains (pos))
                    forSelectedSources ([c = colour] (int, ld::SourceParams& s) { s.colour = c; });
            for (auto& [cell, icon] : sourceIconCells)
                if (cell.contains (pos))
                    forSelectedSources ([i = icon] (int, ld::SourceParams& s) { s.icon = i; });
        }
        repaint();
        notifyStatusKeysChanged();
        return;
    }
    if (renameKey.contains (pos))
        renameSource();
    else if (srcSoloKey.contains (pos) && mode == Mode::Sources && ctx.engine.monitor.sourceSolo.load() != ld::kSourceSoloOff)
        ctx.engine.setSourceSolo (selectedSource, ! ctx.engine.isSourceSoloed (selectedSource));
    else if (autoKey.contains (pos))
        autoAdvance = ! autoAdvance;
    else if (lockKey.contains (pos))
        setLocked (true);
    else if (unlockKey.contains (pos))
        setLocked (false);
    else if (prevPage.contains (pos) || nextPage.contains (pos))
    {
        targetPage = (targetPage + (nextPage.contains (pos) ? 1 : numTargetPages() - 1)) % numTargetPages();
        layoutTiles();
    }
    else if (prevSignals.contains (pos) || nextSignals.contains (pos))
    {
        const int pages = juce::jmax (1, ((int) signals().size() + kSignalsPerPage - 1) / kSignalsPerPage);
        signalPage = (signalPage + (nextSignals.contains (pos) ? 1 : pages - 1)) % pages;
        layoutTiles();
    }
    else if (mainKey.contains (pos) || altKey.contains (pos))
        editAlt = altKey.contains (pos); // CHANNEL INPUT: which input of the channel the source grid patches
    else if (leftSelector.contains (pos))
        showGroupMenu (false);
    else if (rightSelector.contains (pos))
        showGroupMenu (true);
    else if (mode == Mode::Channels && (monoKey.contains (pos) || stereoKey.contains (pos) || midSideKey.contains (pos)))
    {
        // The channel's source pairing (its channels follow); hidden while locked.
        if (const int index = patchedSourceIndex(); index >= 0 && routing::showsPatchKeys (lock))
            ctx.engine.setSourceMode (index, monoKey.contains (pos) ? ld::kSourceMono : stereoKey.contains (pos) ? ld::kSourceStereo : ld::kSourceMidSide);
    }
    else if (noneKey.contains (pos))
    {
        if (lock.isLocked())
            return;
        if (mode == Mode::Outputs)
            clearOutput (selectedPort);
        else if (auto* s = selectedStrip())
        {
            if (editAlt)
            {
                s->setAltInput (-1);
            }
            else
            {
                s->params.inputLeft = -1;
                s->params.inputRight = -1;
            }
        }
    }
    else
    {
        for (auto& [area, strip] : channelTiles)
            if (area.contains (pos))
            {
                select (strip);
                if (ctx.selectStrip)
                    ctx.selectStrip (strip);
                return;
            }
        for (size_t i = 0; i < rightPorts.size(); ++i)
            if (rightPorts[i].contains (pos))
            {
                // Right click or shift: the right input of a stereo channel.
                if (const int source = routing::sourceAt (inputGroup, (int) i); source >= 0)
                    assignInput (source, e.mods.isPopupMenu() || e.mods.isShiftDown());
                return;
            }
        for (size_t i = 0; i < leftPorts.size(); ++i)
            if (leftPorts[i].contains (pos))
            {
                if (mode == Mode::Sources)
                {
                    if (const int source = routing::sourceAt (inputGroup, (int) i); source >= 0)
                        selectSource (source);
                }
                else if (const int port = routing::outputAt (outputGroup, (int) i); port >= 0)
                    selectedPort = port;
                repaint();
                notifyStatusKeysChanged(); // the title tile shows the selection
                notifyKnobsChanged();      // SOURCES: its GAIN
                return;
            }
        for (auto& [area, sig] : signalTiles)
            if (area.contains (pos))
            {
                assignOutput (selectedPort, sig);
                return;
            }
    }
    repaint();
}

} // namespace livedaw::ui
