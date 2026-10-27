#include "dsp/DspUtils.h"
#include "engine/Models.h"
#include "engine/PluginHost.h"
#include "ui/Faceplate.h"
#include "ui/FaceplateDesign.h"
#include "ui/HomeWidgets.h"
#include "ui/Pages.h"
#include "ui/WingWidgets.h"

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

    /** The rack's scratched dark metal [3.1]: grain, fine light scratches and
        a few long dark ones, the same every time. */
    juce::Image scratchedMetal (int w, int h, float scale)
    {
        juce::Image image (juce::Image::RGB, juce::jmax (1, w), juce::jmax (1, h), false);
        juce::Random random (7);
        {
            juce::Image::BitmapData data (image, juce::Image::BitmapData::writeOnly);
            for (int y = 0; y < image.getHeight(); ++y)
                for (int x = 0; x < image.getWidth(); ++x)
                {
                    const auto v = (juce::uint8) (22 + random.nextInt (12));
                    data.setPixelColour (x, y, juce::Colour (v, (juce::uint8) (v + 1), v));
                }
        }
        juce::Graphics g (image);
        const float fw = (float) image.getWidth(), fh = (float) image.getHeight();
        for (int i = 0; i < 260; ++i)
        {
            const float x = random.nextFloat() * fw, y = random.nextFloat() * fh;
            const float angle = random.nextFloat() * juce::MathConstants<float>::pi, length = (8.0f + random.nextFloat() * 110.0f) * scale;
            g.setColour (juce::Colours::white.withAlpha (0.04f + random.nextFloat() * 0.12f));
            g.drawLine (x, y, x + std::cos (angle) * length, y + std::sin (angle) * length, juce::jmax (0.6f, scale * 0.8f));
        }
        for (int i = 0; i < 24; ++i)
        {
            const float x = random.nextFloat() * fw, y = random.nextFloat() * fh;
            const float angle = random.nextFloat() * juce::MathConstants<float>::pi, length = (60.0f + random.nextFloat() * 260.0f) * scale;
            g.setColour (juce::Colours::black.withAlpha (0.35f + random.nextFloat() * 0.3f));
            g.drawLine (x, y, x + std::cos (angle) * length, y + std::sin (angle) * length, juce::jmax (1.0f, scale * 1.4f));
        }
        // Darker towards the edges.
        g.setGradientFill (juce::ColourGradient (juce::Colours::transparentBlack, fw * 0.5f, fh * 0.5f, juce::Colours::black.withAlpha (0.35f), 0.0f, 0.0f, true));
        g.fillAll();
        return image;
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
                g.setColour (level >= pos ? (pos > 0.85f ? Colours::meterYellow : Colours::meterGreen) : Colours::screenTile);
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
        const auto b = getLocalBounds().toFloat();
        const int model = fx.getModel (slot);
        const bool device = model > ld::kFxExt;
        const bool outboard = fx.isOutboard (slot);
        const bool loaded = fx.hasPlugin (slot) || device || outboard;
        const auto user = loaded ? findUser (ctx.engine, slot) : FxUser {};
        const bool inUse = user.strip != nullptr;

        // [3.1] WING pixels of the 282 x 140 tile.
        const float sx = b.getWidth() / 282.0f, sy = b.getHeight() / 140.0f;
        auto at = [&b, sx, sy] (float x, float y, float w, float h) { return juce::Rectangle<float> (b.getX() + x * sx, b.getY() + y * sy, w * sx, h * sy); };

        // A slot in use has a #707070 outline and number box; an empty slot,
        // or one whose device is inserted nowhere, #383838 on black.
        const juce::Colour dimGrey (0xff383838);
        g.setColour (juce::Colours::black);
        g.fillRect (b);
        g.setColour (inUse ? Colours::textDim : dimGrey);
        g.drawRect (b, juce::jmax (1.0f, 2.0f * sy));

        // Slot number, bottom right: 16 of the tile's 140 px (wider for 10-16).
        const float numberH = juce::jmax (12.0f, 16.0f * sy);
        const auto numberFont = wingFontPx (numberH * 0.6f, WingWeight::SemiBold);
        const float numberW = juce::jmax (numberH, juce::GlyphArrangement::getStringWidth (numberFont, juce::String (slot + 1)) + numberH * 0.35f);
        auto number = juce::Rectangle<float> (b.getRight() - numberW - 2.0f * sx, b.getBottom() - numberH - 2.0f * sy, numberW, numberH);
        g.setColour (inUse ? Colours::textDim : dimGrey);
        g.fillRect (number);
        g.setColour (juce::Colours::black);
        g.setFont (numberFont);
        g.drawText (juce::String (slot + 1), number, juce::Justification::centred);

        // The unit: a #202020 plate 196 x 60 at 78, 8 with the device's
        // face 6 in and its meter at the right.
        const auto plate = at (78.0f, 8.0f, 196.0f, 60.0f);
        meterArea = at (246.0f, 12.0f, 24.0f, 52.0f);
        deviceArea = at (84.0f, 14.0f, 158.0f, 48.0f);
        g.setColour (loaded ? Colours::screenPanel : Colours::screenDark);
        g.fillRect (plate);

        if (! loaded)
        {
            // An empty slot: a blank unit with its unlit meter and a circled "+".
            const auto meter = meterArea.reduced (0.0f, 2.0f);
            const int segments = juce::jmax (6, (int) (meter.getHeight() / 4.0f));
            const float segH = meter.getHeight() / (float) segments, segW = (meter.getWidth() - 2.0f) / 2.0f;
            g.setColour (juce::Colour (0xff080808));
            for (int col = 0; col < 2; ++col)
                for (int i = 0; i < segments; ++i)
                    g.fillRect (meter.getX() + (float) col * (segW + 2.0f), meter.getY() + (float) i * segH, segW, juce::jmax (1.0f, segH - 1.5f));
            const auto unit = plate.withTrimmedRight (plate.getRight() - meterArea.getX());
            const float d = plate.getHeight() * 0.65f, stroke = juce::jmax (1.5f, d * 0.07f);
            auto circle = juce::Rectangle<float> (d, d).withCentre (unit.getCentre());
            g.setColour (dimGrey);
            g.drawEllipse (circle.reduced (stroke * 0.5f), stroke);
            g.fillRect (juce::Rectangle<float> (d * 0.4f, stroke * 1.4f).withCentre (circle.getCentre()));
            g.fillRect (juce::Rectangle<float> (stroke * 1.4f, d * 0.4f).withCentre (circle.getCentre()));
            sourceArea = onArea = prePostArea = knobArea = {};
            return;
        }

        // The device: its front panel, or for a plugin a faceplate with its name.
        auto* plugin = fx.getPlugin (slot);
        const auto& spec = models::model (models::Fx, model);
        if (device)
        {
            Faceplate::paintThumbnail (g, deviceArea, models::Fx, model, spec.name.toUpperCase());
        }
        else
        {
            juce::ColourGradient face (juce::Colour (0xff5a5d62), deviceArea.getX(), deviceArea.getY(), juce::Colour (0xff2c2e31), deviceArea.getX(),
                                       deviceArea.getBottom(), false);
            g.setGradientFill (face);
            g.fillRect (deviceArea);
            g.setColour (juce::Colour (0xff1a1a1a));
            g.drawRect (deviceArea, 1.0f);
            for (float x : { deviceArea.getX() + 5.0f, deviceArea.getRight() - 5.0f })
                for (float y : { deviceArea.getY() + 5.0f, deviceArea.getBottom() - 5.0f })
                {
                    g.setColour (juce::Colour (0xff8a8a8a));
                    g.fillEllipse (juce::Rectangle<float> (3.0f, 3.0f).withCentre ({ x, y }));
                }
            auto label = deviceArea.reduced (12.0f, 6.0f);
            g.setColour (juce::Colours::white);
            g.setFont (wingFontFit (TypeScale::value, label.getHeight() * 0.42f, WingWeight::SemiBold));
            // An outboard insert (EXTERNAL EFFECT) shows its RETURN.
            const auto title = plugin != nullptr ? plugin->getName().toUpperCase()
                               : outboard        ? juce::String ("OUTBOARD")
                                                 : juce::String ("FX " + juce::String (slot + 1));
            const auto subtitle = plugin != nullptr ? PluginHost::describe (*plugin).manufacturerName.toUpperCase()
                                  : outboard        ? "RETURN " + fx.returnLabel (slot)
                                                    : juce::String ("EXT");
            g.drawFittedText (title, label.removeFromTop (label.getHeight() * 0.62f).toNearestInt(), juce::Justification::centredBottom, 1);
            g.setColour (juce::Colour (0xffc8c8c8));
            g.setFont (wingFontFit (TypeScale::caption, label.getHeight() * 0.7f));
            g.drawFittedText (subtitle, label.toNearestInt(), juce::Justification::centredTop, 1);
        }

        const float level = inUse && ! params.bypass.load()
                                ? juce::jlimit (0.0f, 1.0f, (dsp::gainToDb (ctx.meters.get (user.strip, false, 0)) + 60.0f) / 60.0f)
                                : 0.0f;
        segmentMeter (g, meterArea, level);

        if (fx.isFaulted (slot) || fx.isDoubleAssigned (slot))
        {
            auto warn = deviceArea.withSizeKeepingCentre (deviceArea.getWidth(), 20.0f);
            g.setColour (Colours::mute);
            g.fillRect (warn);
            g.setColour (juce::Colours::black);
            g.setFont (wingFont (TypeScale::body, WingWeight::SemiBold));
            g.drawFittedText (fx.isFaulted (slot) ? "FAULT - BYPASSED" : "DOUBLE ASSIGNED", warn.toNearestInt(), juce::Justification::centred, 1);
        }
        else if (const int faulted = fx.getNumFaultedChainMembers (slot); faulted > 0)
        {
            // A chain keeps running without its faulted plugins.
            auto warn = deviceArea.withSizeKeepingCentre (deviceArea.getWidth(), 20.0f);
            g.setColour (Colours::amber);
            g.fillRect (warn);
            g.setColour (juce::Colours::black);
            g.setFont (wingFont (TypeScale::body, WingWeight::SemiBold));
            g.drawFittedText (juce::String (faulted) + (faulted == 1 ? " PLUGIN FAULTED" : " PLUGINS FAULTED"), warn.toNearestInt(), juce::Justification::centred, 1);
        }

        // The destination (in use only): its name in a bar of its colour (plain
        // when it has its default name) over its icon and default label, then
        // PRE / POST and ON, in the column 8..72.
        sourceArea = at (8.0f, 8.0f, 64.0f, 76.0f);
        prePostArea = at (8.0f, 89.0f, 64.0f, 20.0f);
        onArea = at (8.0f, 112.0f, 64.0f, 20.0f);
        const bool on = ! params.bypass.load();
        if (inUse)
        {
            auto src = sourceArea;
            auto bar = src.removeFromTop (16.0f * sy);
            g.setColour (stripDisplayColour (*user.strip));
            g.fillRect (bar);
            if (stripName (*user.strip) != user.strip->getLabel())
            {
                g.setColour (juce::Colours::black);
                g.setFont (wingFontFit (TypeScale::body, bar.getHeight() * 0.9f, WingWeight::SemiBold));
                g.drawFittedText (stripName (*user.strip).toUpperCase(), bar.toNearestInt().reduced (2, 0), juce::Justification::centred, 1, 0.6f);
            }
            g.setColour (Colours::screenTile);
            g.fillRect (src);
            auto name = src.removeFromBottom (juce::jmin (18.0f * sy, src.getHeight() * 0.32f));
            drawStripIcon (g, *user.strip, src.reduced (src.getWidth() * 0.22f, 3.0f), juce::Colours::white);
            g.setColour (juce::Colours::white);
            g.setFont (wingFontFit (TypeScale::body, name.getHeight(), WingWeight::SemiBold));
            g.drawFittedText (user.strip->getLabel(), name.toNearestInt(), juce::Justification::centred, 1, 0.7f);

            // PRE lit red, POST green, as on the console.
            auto pre = prePostArea.withWidth (prePostArea.getWidth() / 2.0f);
            drawChip (g, pre, "PRE", user.post ? Colours::screenTile : Dark::pre, user.post ? Dark::textDim : juce::Colours::black);
            drawChip (g, prePostArea.withTrimmedLeft (pre.getWidth()), "POST", user.post ? Dark::post : Colours::screenTile,
                      user.post ? juce::Colours::black : Dark::textDim);
            drawChip (g, onArea, "ON", on ? Colours::yellow : Colours::screenTile, on ? juce::Colours::black : Dark::textDim);
        }
        else
        {
            sourceArea = prePostArea = onArea = {};
        }

        // Ring and value: the device's key control (a plugin's: MIX), the
        // ring 48 px at 90, 80, the caption and value from 151.
        knobArea = at (90.0f, 80.0f, 48.0f, 48.0f);
        ringParam = device ? fp::rackTileParam (spec) : -1;
        float mix = juce::jlimit (0.0f, 1.0f, params.mix.load() / 100.0f);
        juce::String ringLabel ("MIX"), ringValue (juce::String (juce::roundToInt (params.mix.load())) + " %");
        if (ringParam >= 0)
        {
            const auto& p = spec.params[(size_t) ringParam];
            const float v = params.values[ringParam].load();
            mix = p.toNormalised (v);
            ringLabel = p.label.toUpperCase();
            ringValue = p.display (v);
        }
        const auto k = knobArea;
        const float a0 = juce::MathConstants<float>::pi * 1.2f, a1 = juce::MathConstants<float>::pi * 2.8f;
        const float rad = k.getWidth() * 0.4f, thick = k.getWidth() * 0.19f;
        juce::Path track, value;
        track.addCentredArc (k.getCentreX(), k.getCentreY(), rad, rad, 0.0f, a0, a1, true);
        value.addCentredArc (k.getCentreX(), k.getCentreY(), rad, rad, 0.0f, a0, a0 + mix * (a1 - a0), true);
        g.setColour (Colours::screenLine);
        g.strokePath (track, juce::PathStrokeType (thick));
        g.setColour (on || ! inUse ? Colours::text : Dark::textDim);
        g.strokePath (value, juce::PathStrokeType (thick));
        g.setColour (Colours::text);
        g.setFont (wingFontPx (13.0f * sy));
        g.drawFittedText (ringLabel, at (151.0f, 80.0f, 110.0f, 20.0f).toNearestInt(), juce::Justification::bottomLeft, 1, 0.8f);
        g.setFont (wingFontPx (18.0f * sy, WingWeight::SemiBold));
        g.drawFittedText (ringValue, at (151.0f, 101.0f, 110.0f, 28.0f).toNearestInt(), juce::Justification::centredLeft, 1, 0.8f);

        // [3.1] a device inserted nowhere is shown at half brightness.
        if (! inUse)
        {
            g.setColour (juce::Colours::black.withAlpha (0.5f));
            g.fillRect (b.reduced (2.0f * sx, 2.0f * sy).withTrimmedBottom (numberH));
            g.fillRect (b.reduced (2.0f * sx, 2.0f * sy).withTop (number.getY()).withRight (number.getX()));
        }
    }

    void mouseDown (const juce::MouseEvent& e) override
    {
        auto& params = ctx.engine.getFx().params (slot);
        const auto& spec = models::model (models::Fx, ctx.engine.getFx().getModel (slot));
        dragStart = ringParam >= 0 ? spec.params[(size_t) ringParam].toNormalised (params.values[ringParam].load()) : params.mix.load() / 100.0f;
        draggingMix = knobArea.contains (e.position);
    }

    void mouseDrag (const juce::MouseEvent& e) override
    {
        if (! draggingMix)
            return;
        auto& params = ctx.engine.getFx().params (slot);
        const float pos = juce::jlimit (0.0f, 1.0f, dragStart - (float) e.getDistanceFromDragStartY() / 200.0f);
        const auto& spec = models::model (models::Fx, ctx.engine.getFx().getModel (slot));
        if (ringParam >= 0 && ringParam < (int) spec.params.size())
            params.values[ringParam].store (spec.params[(size_t) ringParam].fromNormalised (pos));
        else
            params.mix = pos * 100.0f;
    }

    void mouseUp (const juce::MouseEvent& e) override
    {
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
        {
            // Open the slot: a device or plugin in the editor, or choose one for an empty slot.
            auto& fx = ctx.engine.getFx();
            if (fx.isLoaded (slot))
                page.editSlot (slot);
            else
                showFxDeviceMenu (ctx, slot, this, [this] { page.editSlot (slot); }, &page);
        }
        draggingMix = false;
    }

    void mouseDoubleClick (const juce::MouseEvent& e) override
    {
        if (! knobArea.contains (e.position))
            return;
        auto& params = ctx.engine.getFx().params (slot);
        const auto& spec = models::model (models::Fx, ctx.engine.getFx().getModel (slot));
        if (ringParam >= 0 && ringParam < (int) spec.params.size())
            params.values[ringParam].store (spec.params[(size_t) ringParam].defaultValue);
        else
            params.mix = 100.0f;
    }

private:
    void timerCallback() override
    {
        // Repaint only when what the tile shows changes: sixteen tiles redrawn
        // ten times a second cost a lot on a big (maximized) screen.
        if (auto now = signature(); now != shown)
        {
            shown = now;
            repaint();
        }
    }

    /** What paint() draws, in short. */
    juce::String signature() const
    {
        auto& fx = ctx.engine.getFx();
        auto& params = fx.params (slot);
        const int model = fx.getModel (slot);
        juce::String sig;
        sig << model << '|' << (juce::pointer_sized_int) fx.getPlugin (slot) << '|' << (int) params.bypass.load() << '|' << params.mix.load() << '|'
            << (int) fx.isFaulted (slot) << (int) fx.isDoubleAssigned (slot) << fx.getNumFaultedChainMembers (slot);
        if (fx.isOutboard (slot))
            sig << "|OB " << fx.returnLabel (slot);
        if (model > ld::kFxExt)
        {
            const auto& spec = models::model (models::Fx, model);
            if (const int key = fp::rackTileParam (spec); key >= 0 && key < (int) spec.params.size())
                sig << '|' << params.values[key].load();
        }
        const auto user = findUser (ctx.engine, slot);
        if (user.strip != nullptr)
        {
            const float level = params.bypass.load() ? 0.0f : dsp::gainToDb (ctx.meters.get (user.strip, false, 0));
            sig << '|' << (juce::pointer_sized_int) user.strip << (int) user.post << stripName (*user.strip) << '|'
                << (juce::int64) stripDisplayColour (*user.strip).getARGB() << '|' << juce::roundToInt (juce::jlimit (-61.0f, 0.0f, level));
        }
        return sig;
    }

    UiContext& ctx;
    int slot;
    EffectsPage& page;
    juce::String shown;
    juce::Rectangle<float> deviceArea, meterArea, sourceArea, prePostArea, onArea, knobArea;
    float dragStart = 0.0f;
    int ringParam = -1;
    bool draggingMix = false;
};

//==============================================================================
/** A slot opened from the rack: the device's front panel with its controls (or
    the plugin of an EXT slot), the device selector, source, ON and MIX. */
class EffectsPage::Editor final : public juce::Component, private juce::Timer
{
public:
    Editor (UiContext& c, EffectsPage& p) : ctx (c), page (p)
    {
        backKey.onClick = [this] { page.editSlot (-1); };
        deviceField.arrowKey = true;
        onKey.isOn = [this] { return ! ctx.engine.getFx().params (slot).bypass.load(); };
        onKey.onClick = [this]
        {
            auto& fx = ctx.engine.getFx();
            fx.clearFlags (slot);
            fx.params (slot).bypass = ! fx.params (slot).bypass.load();
        };
        openKey.onClick = [this] { if (ctx.openPluginEditor) ctx.openPluginEditor (slot); };
        pluginKey.onClick = [this] { showFxSlotMenu (ctx, slot, &pluginKey); };
        face.onTouched = [this] (int param)
        {
            if (param / 6 != page6)
            {
                page6 = param / 6;
                page.notifyKnobsChanged();
            }
        };
        for (juce::Component* comp : { (juce::Component*) &backKey, (juce::Component*) &deviceField, (juce::Component*) &onKey,
                                       (juce::Component*) &mix, (juce::Component*) &face, (juce::Component*) &openKey, (juce::Component*) &pluginKey })
            addAndMakeVisible (comp);
        for (juce::Component* comp : { (juce::Component*) &sourceKeys, (juce::Component*) &trimBox, (juce::Component*) &latencyBox })
            addChildComponent (comp);
        startTimerHz (6);
    }

    void setSlot (int s)
    {
        slot = juce::jlimit (0, kNumFx - 1, s);
        mix.setBinding (bindLinear ("FX MIX", ctx.engine.getFx().params (slot).mix, 0.0f, 100.0f, " %", 0, 100.0f));
        auto& v = ctx.engine.getFx().params (slot).values;
        trimBox.setBinding (bindLinear ("TRIM", v[ld::ext::kTrim], -18.0f, 18.0f, " dB", 1, 0.0f));
        trimBox.centredRange = true;
        auto lat = bindLinear ("LATENCY", v[ld::ext::kLatency], 0.0f, 800.0f, " smp", 0, 0.0f);
        lat.steps = 801.0f;
        latencyBox.setBinding (lat);
        deviceField.setCaption ("FX " + juce::String (slot + 1) + (slot < 8 ? "  PREMIUM" : "  STANDARD"));
        rebind();
    }

    int getSlot() const noexcept { return slot; }

    std::vector<KnobBinding> knobs()
    {
        std::vector<KnobBinding> k;
        auto& fx = ctx.engine.getFx();
        const int model = fx.getModel (slot);
        if (fx.isOutboard (slot))
        {
            // The EXTERNAL EFFECT's continuous controls: TRIM, LATENCY and the slot's MIX.
            auto& v = fx.params (slot).values;
            k.push_back (bindLinear ("TRIM", v[ld::ext::kTrim], -18.0f, 18.0f, " dB", 1, 0.0f));
            auto lat = bindLinear ("LATENCY", v[ld::ext::kLatency], 0.0f, 800.0f, " smp", 0, 0.0f);
            lat.steps = 801.0f;
            k.push_back (lat);
            k.push_back (bindLinear ("FX" + juce::String (slot + 1) + " MIX", fx.params (slot).mix, 0.0f, 100.0f, "%", 0, 100.0f));
            return k;
        }
        if (model <= ld::kFxExt)
        {
            k.push_back (bindLinear ("FX" + juce::String (slot + 1) + " MIX", fx.params (slot).mix, 0.0f, 100.0f, "%", 0, 100.0f));
            return k;
        }
        const auto& spec = models::model (models::Fx, model);
        for (int i = page6 * 6; i < juce::jmin ((int) spec.params.size(), page6 * 6 + 6); ++i)
            k.push_back (bindModelParam (fx.params (slot).values[i], spec.params[(size_t) i]));
        return k;
    }

    void paint (juce::Graphics& g) override
    {
        g.fillAll (Colours::screen);
        auto& fx = ctx.engine.getFx();
        const int model = fx.getModel (slot);

        // The header row as on the channel's INS screen [D8].
        g.setColour (Colours::screenPanel);
        g.fillRect (header);

        // Who uses the slot.
        juce::String users;
        for (auto kind : { StripKind::Channel, StripKind::Aux, StripKind::Bus, StripKind::Main, StripKind::Matrix })
            for (auto& s : ctx.engine.getStrips (kind))
            {
                const bool pre = s->params.preInsert.fx.load() == slot;
                const bool post = s->params.postInsert.fx.load() == slot && s->params.postInsert.mode.load() == 0;
                if ((pre || post) && users.isEmpty())
                    users = stripName (*s).toUpperCase() + (pre ? "  PRE" : "  POST");
            }
        // INSERTED ON: a caption over the value, like a field without a key.
        {
            auto u = sourceArea;
            g.setColour (Colours::caption);
            g.setFont (wingFont (TypeScale::body));
            g.drawText ("INSERTED ON", u.removeFromTop (u.getHeight() * 2 / 5), juce::Justification::centredLeft, false);
            g.setColour (users.isEmpty() ? Colours::textDim : Colours::text);
            g.setFont (wingFont (TypeScale::value));
            g.drawFittedText (users.isEmpty() ? juce::String ("NOT ASSIGNED") : users, u, juce::Justification::centredLeft, 1, 0.7f);
        }

        if (model == ld::kFxNone)
        {
            g.setColour (Colours::screenPanel);
            g.fillRect (body);
            const float d = juce::jmin (90.0f, body.getHeight() * 0.4f);
            auto circle = juce::Rectangle<float> (d, d).withCentre (body.getCentre().toFloat().translated (0.0f, -20.0f));
            g.setColour (Colours::textDim);
            g.drawEllipse (circle, 3.0f);
            g.fillRect (juce::Rectangle<float> (d * 0.5f, 3.0f).withCentre (circle.getCentre()));
            g.fillRect (juce::Rectangle<float> (3.0f, d * 0.5f).withCentre (circle.getCentre()));
            g.setFont (wingFont (TypeScale::value * 1.2f, WingWeight::SemiBold));
            g.drawText ("SELECT A DEVICE", body.withTop ((int) circle.getBottom() + 10).withHeight (30), juce::Justification::centredTop);
        }
        else if (model == ld::kFxExt && fx.isOutboard (slot))
        {
            paintOutboard (g);
        }
        else if (model == ld::kFxExt)
        {
            g.setColour (Colours::screenPanel);
            g.fillRect (body);
            auto* plugin = fx.getPlugin (slot);
            auto r = body.reduced (24).withHeight (juce::jmin (body.getHeight() - 48, 120));
            g.setColour (Colours::text);
            g.setFont (wingFont (TypeScale::statusTitle, WingWeight::SemiBold));
            g.drawFittedText (plugin != nullptr ? plugin->getName().toUpperCase() : juce::String ("NO PLUGIN LOADED"), r.removeFromTop (40),
                              juce::Justification::centredLeft, 1);
            g.setColour (Colours::textDim);
            g.setFont (wingFont (TypeScale::value));
            juce::String info ("EXT DEVICE: A VST3 / AU / LV2 PLUGIN");
            if (plugin != nullptr)
            {
                info = PluginHost::describe (*plugin).manufacturerName.toUpperCase() + "   " + PluginHost::describe (*plugin).pluginFormatName
                       + "   LOAD " + juce::String (juce::roundToInt (fx.getLoad (slot) * 100.0f)) + " %";
                if (fx.isSandboxed (slot)) // the plugin's latency includes the sandbox's block (compensated)
                    info << "   SANDBOX, LATENCY " << fx.getLatency (slot) << " SAMPLES";
            }
            g.drawFittedText (info, r.removeFromTop (28), juce::Justification::centredLeft, 1);
        }
    }

    void resized() override
    {
        auto b = getLocalBounds();
        // [D8] the INS screen's 80 px row (of the 670 px page): 64 px fields
        // and the 48 px ON key, laid out on the WING's 1160 px page width.
        header = b.removeFromTop (juce::jlimit (32, 72, b.getHeight() * 12 / 100));
        const float u = (float) header.getWidth() / 1160.0f;
        auto h = header.withSizeKeepingCentre (header.getWidth(), header.getHeight() * 4 / 5);
        auto span = [&h, u, this] (float x0, float x1)
        { return juce::Rectangle<int> (header.getX() + juce::roundToInt (x0 * u), h.getY(), juce::roundToInt ((x1 - x0) * u), h.getHeight()); };
        backKey.setBounds (span (16.0f, 160.0f));
        onKey.setBounds (span (176.0f, 272.0f).withSizeKeepingCentre (juce::roundToInt (96.0f * u), header.getHeight() * 3 / 5));
        deviceField.setBounds (span (288.0f, 640.0f));
        sourceArea = span (672.0f, 900.0f);
        mix.setBounds (span (944.0f, 1144.0f));
        b.removeFromTop (6);
        body = b;
        face.setBounds (body);
        auto keys = body.reduced (24).withTrimmedTop (juce::jmin (body.getHeight() - 48, 120) + 12).withHeight (48);
        openKey.setBounds (keys.removeFromLeft (220));
        keys.removeFromLeft (10);
        pluginKey.setBounds (keys.removeFromLeft (220));
        layoutOutboard();
    }

    void mouseUp (const juce::MouseEvent& e) override
    {
        auto& fx = ctx.engine.getFx();
        if (fx.getModel (slot) == ld::kFxNone && body.contains (e.getPosition()))
            showFxDeviceMenu (ctx, slot, this, [this] { rebind(); }, this);
        else if (fx.isOutboard (slot) && sendArea.contains (e.getPosition()) && ctx.showFxSendOutputs)
            ctx.showFxSendOutputs (slot); // [Fig. 169] SEND opens ROUTING > OUTPUTS
        else if (fx.isOutboard (slot) && returnArea.contains (e.getPosition()))
            chooseReturn();
        else if (fx.isOutboard (slot) && modeArea.contains (e.getPosition()))
        {
            // FX MODE: a row per mode.
            const int row = juce::jlimit (0, 2, (e.y - modeArea.getY()) * 3 / juce::jmax (1, modeArea.getHeight()));
            fx.params (slot).values[ld::ext::kMode] = (float) row;
            repaint (modeArea);
        }
    }

private:
    void rebind()
    {
        auto& fx = ctx.engine.getFx();
        shownModel = fx.getModel (slot);
        const bool device = shownModel > ld::kFxExt;
        face.setVisible (device);
        if (device)
            face.bind (
                models::Fx, shownModel, fx.params (slot).values, [this] { return ctx.engine.getFx().getMeter (slot); },
                [this] (int band) { return ctx.engine.getFx().getBandGain (slot, band); });
        else
            face.unbind();
        shownOutboard = fx.isOutboard (slot);
        openKey.setVisible (shownModel == ld::kFxExt && ! shownOutboard);
        pluginKey.setVisible (shownModel == ld::kFxExt && ! shownOutboard);
        openKey.setEnabled (fx.hasPlugin (slot));
        sourceKeys.setVisible (shownModel == ld::kFxExt);
        trimBox.setVisible (shownOutboard);
        latencyBox.setVisible (shownOutboard);
        layoutOutboard();
        deviceField.repaint();
        page6 = 0;
        page.notifyKnobsChanged();
        repaint();
    }

    void timerCallback() override
    {
        auto& fx = ctx.engine.getFx();
        if (fx.getModel (slot) != shownModel || fx.isOutboard (slot) != shownOutboard)
            rebind();
        openKey.setEnabled (fx.hasPlugin (slot));
        repaint (header);
        if (shownOutboard)
            repaint (body); // the SEND and RETURN meters, the patch
    }

    //==============================================================================
    // The EXTERNAL EFFECT page (manual 11.11 p. 164, Fig. 169): SEND (the
    // outputs patched to the slot's FX SEND, with TRIM), FX MODE, RETURN (the
    // source the gear comes back on) and LATENCY, with the meters either side.

    /** A rectangle of the page's faceplate frame (1156 x 479, Fig. 169's panel at 30, 70)
        on the body, scaled and centred as the drawn panels are. */
    juce::Rectangle<int> onFrame (float x, float y, float w, float h) const
    {
        const float scale = juce::jmin ((float) body.getWidth() / 1156.0f, (float) body.getHeight() / 479.0f);
        const float ox = (float) body.getCentreX() - 578.0f * scale, oy = (float) body.getCentreY() - 239.5f * scale;
        return juce::Rectangle<float> (ox + x * scale, oy + y * scale, w * scale, h * scale).toNearestInt();
    }

    float frameScale() const { return juce::jmin ((float) body.getWidth() / 1156.0f, (float) body.getHeight() / 479.0f); }

    void layoutOutboard()
    {
        // Measured from Fig. 169 (frame = figure + (30, 70)); SOURCE and LATENCY are Sidecar's.
        sourceKeys.setBounds (onFrame (40, 76, 300, 34));
        sendArea = onFrame (200, 192, 160, 90);
        trimBox.setBounds (onFrame (192, 296, 174, 62));
        modeArea = onFrame (480, 192, 200, 190);
        returnArea = onFrame (827, 220, 110, 110);
        latencyBox.setBounds (onFrame (798, 368, 170, 50));
    }

    /** The outputs patched to FX SEND side `side`, as the console names them ("LCL 7"). */
    juce::String sendOutput (int side) const
    {
        auto& p = ctx.engine.getFx().params (slot);
        const int port = (side == 0 ? p.sendLeft : p.sendRight).load();
        if (port < 0)
            return "-";
        const int block = port / 48, n = port % 48 + 1;
        return juce::String (block == 0 ? "LCL" : block == 1 ? "B" : block == 2 ? "C" : "OUT") + " " + juce::String (block <= 2 ? n : port + 1);
    }

    void paintOutboard (juce::Graphics& g)
    {
        auto& fx = ctx.engine.getFx();
        g.setColour (Colours::screenPanel);
        g.fillRect (body);
        const float k = frameScale();
        const auto font = [k] (float px, bool semi = true) { return wingFontPx (px * k * 0.7f, semi ? WingWeight::SemiBold : WingWeight::Medium); }; // px: text height on the frame
        g.setColour (Colours::text.withAlpha (0.85f));
        g.setFont (font (28.0f));
        g.drawText ("EXTERNAL EFFECT", onFrame (380, 86, 400, 34), juce::Justification::centred, false);

        // [Fig. 169] the sections framed by corner marks, their titles on top.
        const struct
        {
            const char* title;
            float x0, x1;
        } sections[] = { { "SEND", 70, 388 }, { "FX MODE", 478, 800 }, { "RETURN", 768, 1090 } };
        for (const auto& sec : sections)
        {
            const auto r = onFrame (sec.x0, 152, sec.x1 - sec.x0, 20).toFloat();
            const float c = 16.0f * k;
            g.setColour (Colours::textDim);
            g.drawLine (r.getX(), r.getY(), r.getX() + c, r.getY(), 2.0f);
            g.drawLine (r.getX(), r.getY(), r.getX(), r.getY() + c, 2.0f);
            g.drawLine (r.getRight() - c, r.getY(), r.getRight(), r.getY(), 2.0f);
            g.drawLine (r.getRight(), r.getY(), r.getRight(), r.getY() + c, 2.0f);
            g.setColour (Colours::text.withAlpha (0.8f));
            g.setFont (font (19.0f, false));
            g.drawText (sec.title, onFrame ((sec.x0 + sec.x1) * 0.5f - 100, 138, 200, 26), juce::Justification::centred, false);
        }

        // The SEND and RETURN meters: two columns of segments with their scale.
        for (const float x : { 100.0f, 1020.0f })
        {
            const char* marks[] = { "CLIP", "-6", "-12", "-24", "-36", "-48", "-60" };
            g.setFont (font (11.0f, false));
            for (int i = 0; i < 7; ++i)
            {
                g.setColour (Colours::textDim);
                g.drawText (marks[i], onFrame (x - 46, 192.0f + 27.0f * (float) i, 40, 14), juce::Justification::centredRight, false);
            }
            for (int col = 0; col < 2; ++col)
                for (int seg = 0; seg < 24; ++seg)
                {
                    g.setColour (juce::Colour (0xff3a3a3a));
                    g.fillRect (onFrame (x + 18.0f * (float) col, 194.0f + 7.0f * (float) seg, 12, 5));
                }
        }

        // SEND: the two outputs the FX SEND is patched to (tap: ROUTING > OUTPUTS).
        {
            for (int side = 0; side < 2; ++side)
            {
                const float cx = 238.0f + 80.0f * (float) side;
                const auto out = sendOutput (side);
                const bool patched = out != "-";
                g.setColour (patched ? Colours::text : Colours::textDim);
                g.setFont (font (20.0f, false));
                g.drawText (patched ? out.upToFirstOccurrenceOf (" ", false, false) : juce::String ("-"), onFrame (cx - 40, 186, 80, 24),
                            juce::Justification::centred, false);
                const auto circle = onFrame (cx - 22, 228, 44, 44).toFloat();
                g.setColour (Colours::screen);
                g.fillEllipse (circle);
                g.setColour (patched ? Colours::text : Colours::textDim);
                g.drawEllipse (circle, 2.0f);
                for (const auto dot : { juce::Point<float> (0.0f, -1.0f), juce::Point<float> (-0.8f, 0.6f), juce::Point<float> (0.8f, 0.6f) })
                    g.drawEllipse (juce::Rectangle<float> (8.0f * k, 8.0f * k)
                                       .withCentre (circle.getCentre() + dot * circle.getWidth() * 0.55f),
                                   1.5f);
                g.setFont (font (20.0f));
                g.drawText (patched ? out.fromFirstOccurrenceOf (" ", false, false) : juce::String ("-"), circle.toNearestInt(),
                            juce::Justification::centred, false);
            }
        }

        // FX MODE: MONO, STEREO, MID/SIDE rows, the one in use filled.
        {
            g.setColour (juce::Colours::black);
            g.fillRect (modeArea);
            const int mode = juce::roundToInt (fx.params (slot).values[ld::ext::kMode].load());
            const char* names[] = { "MONO", "STEREO", "MID/SIDE" };
            for (int i = 0; i < 3; ++i)
            {
                const auto row = onFrame (480, 192.0f + 63.0f * (float) i, 200, 63).toFloat();
                if (i == mode)
                {
                    g.setColour (juce::Colour (0xff8a9a10));
                    g.fillRect (row);
                }
                // The mode's symbol: one circle, two overlapping, two with a bar.
                const auto sym = juce::Rectangle<float> (row.getX() + 22.0f * k, row.getCentreY() - 15.0f * k, 30.0f * k, 30.0f * k);
                g.setColour (Colours::text);
                if (i == 0)
                    g.drawEllipse (sym, 2.0f);
                else if (i == 1)
                {
                    g.fillEllipse (sym);
                    g.setColour (i == mode ? juce::Colour (0xff8a9a10) : juce::Colours::black);
                    g.fillEllipse (sym.translated (20.0f * k, 0.0f));
                    g.setColour (Colours::text);
                    g.drawEllipse (sym.translated (20.0f * k, 0.0f), 2.0f);
                }
                else
                {
                    g.drawEllipse (sym.translated (-6.0f * k, 0.0f), 2.0f);
                    g.drawEllipse (sym.translated (24.0f * k, 0.0f), 2.0f);
                    g.fillRect (juce::Rectangle<float> (3.0f * k, 34.0f * k).withCentre (sym.getCentre().translated (9.0f * k, 0.0f)));
                }
                g.setColour (Colours::text);
                g.setFont (font (19.0f, false));
                g.drawText (names[i], row.withTrimmedLeft (88.0f * k).toNearestInt(), juce::Justification::centredLeft, false);
            }
        }

        // RETURN: the source tile (tap: choose the inputs).
        {
            g.setColour (juce::Colour (0xff2a2a2a).darker (0.2f));
            g.fillRect (onFrame (798, 192, 170, 166));
            auto r = returnArea.toFloat();
            g.setColour (juce::Colours::black);
            g.fillRect (r.expanded (4.0f * k));
            g.setColour (Colours::screenTile);
            g.fillRect (r);
            g.setColour (fx.hasReturn (slot) ? Colours::valueBlue : Colours::textDim);
            g.fillRect (r.withHeight (r.getHeight() * 0.18f));
            drawIcon (g, Icon::Xlr, juce::Rectangle<float> (r.getX() + 8.0f * k, r.getY() + 34.0f * k, 38.0f * k, 38.0f * k), juce::Colours::white);
            g.setColour (fx.hasReturn (slot) ? Colours::gold : Colours::textDim);
            g.setFont (font (18.0f));
            g.drawFittedText (fx.returnLabel (slot), r.removeFromBottom (r.getHeight() * 0.28f).toNearestInt(), juce::Justification::centred, 1);
            g.setColour (Colours::caption);
            g.setFont (font (12.0f, false));
            g.drawFittedText ("ROUND TRIP " + juce::String (fx.getLatency (slot)) + " SAMPLES", onFrame (798, 420, 170, 16), juce::Justification::centred, 1);
        }
    }

    /** RETURN: the console inputs the outboard gear comes back on. */
    void chooseReturn()
    {
        auto& v = ctx.engine.getFx().params (slot).values;
        juce::PopupMenu menu;
        menu.addSectionHeader ("RETURN (FX " + juce::String (slot + 1) + ")");
        menu.addItem ("OFF", [&v] { v[ld::ext::kGroup] = 0.0f; });
        // The groups that are Sidecar's inputs (the others share their port numbers).
        struct Group
        {
            const char* name;
            int index, count;
        };
        static constexpr Group groups[] = { { "LCL", 1, 48 }, { "A", 3, 48 }, { "B", 4, 48 }, { "C", 5, 48 }, { "CRD", 8, 64 }, { "PLAY", 10, 4 } };
        const int current = juce::roundToInt (v[ld::ext::kGroup].load()), in = juce::roundToInt (v[ld::ext::kInput].load());
        for (const auto& grp : groups)
        {
            juce::PopupMenu sub;
            for (int n = 1; n <= grp.count; ++n)
                sub.addItem (juce::String (grp.name) + " " + juce::String (n), true, current == grp.index && in == n, [&v, gi = grp.index, n]
                {
                    v[ld::ext::kGroup] = (float) gi;
                    v[ld::ext::kInput] = (float) n;
                });
            menu.addSubMenu (grp.name, sub, true, nullptr, current == grp.index);
        }
        menu.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (this).withTargetScreenArea (localAreaToGlobal (returnArea)));
    }

    UiContext& ctx;
    EffectsPage& page;
    int slot = 0, shownModel = -1, page6 = 0;
    bool shownOutboard = false;
    juce::Rectangle<int> sendArea, returnArea, modeArea;
    /** SOURCE: a plugin in the slot, or the console's outboard insert. */
    home::Segmented sourceKeys { juce::StringArray { "PLUGIN", "OUTBOARD" }, [this] { return ctx.engine.getFx().isOutboard (slot) ? 1 : 0; },
                                 [this] (int i)
                                 {
                                     auto& fx = ctx.engine.getFx();
                                     if (i == 1 && ! fx.isOutboard (slot))
                                     {
                                         if (fx.hasPlugin (slot))
                                             clearFx (ctx, slot); // closes its editor first
                                         fx.setOutboard (slot);
                                     }
                                     else if (i == 0 && fx.isOutboard (slot))
                                     {
                                         fx.params (slot).values[ld::ext::kSource] = 0.0f;
                                         showFxSlotMenu (ctx, slot, &sourceKeys); // load one
                                     }
                                     rebind();
                                 } };
    home::KnobBox trimBox { KnobBinding {} }, latencyBox { KnobBinding {} };
    Faceplate face;
    WingKey backKey { "RACK", Icon::ChevronLeft }, onKey { "ON" }, openKey { "OPEN EDITOR", Icon::Edit },
        pluginKey { "LOAD / REPLACE", Icon::Folder };
    DropdownField deviceField { "FX 1",
                                [this]
                                {
                                    const int m = ctx.engine.getFx().getModel (slot);
                                    return m == ld::kFxNone ? juce::String ("SELECT DEVICE") : models::model (models::Fx, m).name.toUpperCase();
                                },
                                [this] (DropdownField& f) { showFxDeviceMenu (ctx, slot, &f, [this] { rebind(); }, this); } };
    home::KnobBox mix { KnobBinding {} };
    juce::Rectangle<int> header, sourceArea, body;
};

EffectsPage::EffectsPage (UiContext& c) : ctx (c)
{
    for (int i = 0; i < kNumFx; ++i)
        addAndMakeVisible (tiles.add (new Tile (ctx, i, *this)));
    editor = std::make_unique<Editor> (ctx, *this);
    addChildComponent (*editor);
}

void EffectsPage::editSlot (int slot)
{
    const bool open = juce::isPositiveAndBelow (slot, kNumFx);
    if (open)
        editor->setSlot (slot);
    editor->setVisible (open);
    for (auto* t : tiles)
        t->setVisible (! open);
    notifyKnobsChanged();
    notifyStatusKeysChanged(); // the rack has no parameter bar
    repaint();
}

EffectsPage::~EffectsPage() = default;

int EffectsPage::tapParam() const
{
    if (editor == nullptr || ! editor->isVisible())
        return -1;
    return TapTempo::timeParam (ctx.engine.getFx().getModel (editor->getSlot()));
}

void EffectsPage::blankKeyPressed()
{
    // Tap tempo: the time between two taps (0.1 to 3 s) becomes the delay time.
    const int param = tapParam();
    if (param < 0)
        return;
    const auto interval = tapTempo.tap (juce::Time::getMillisecondCounterHiRes());
    if (! interval)
        return;
    const int slot = editor->getSlot();
    const auto& p = models::model (models::Fx, ctx.engine.getFx().getModel (slot)).params[(size_t) param];
    ctx.engine.getFx().params (slot).values[param].store (juce::jlimit (p.min, p.max, *interval));
}

std::optional<bool> EffectsPage::blankKeyBlink (double nowMs) const
{
    const int param = tapParam();
    if (param < 0)
        return std::nullopt;
    return tapTempo.ledLit (nowMs, ctx.engine.getFx().params (editor->getSlot()).values[param].load());
}

std::vector<KnobBinding> EffectsPage::getKnobs()
{
    if (editor != nullptr && editor->isVisible())
        return editor->knobs();
    return {}; // [3.1] the rack has no knobs, touched slot or not
}

bool EffectsPage::usesKnobs() const { return editor != nullptr && editor->isVisible(); }

void EffectsPage::paint (juce::Graphics& g)
{
    g.fillAll (Colours::screen);
    if (editor != nullptr && editor->isVisible())
        return;

    // The rack: scratched dark metal (drawn once per size) with screws.
    if (metal.getWidth() != rack.getWidth() || metal.getHeight() != rack.getHeight())
        metal = scratchedMetal (rack.getWidth(), rack.getHeight(), (float) getHeight() / 720.0f);
    g.drawImageAt (metal, rack.getX(), rack.getY());
    // [3.1] the screws 28 px in from the rack's corners, the embossed FX mark at its top left.
    const float ux = (float) getWidth() / 1280.0f, uy = (float) getHeight() / 720.0f;
    for (auto p : { juce::Point<float> (38.0f, 43.0f), juce::Point<float> (1238.0f, 43.0f), juce::Point<float> (38.0f, 682.0f), juce::Point<float> (1238.0f, 682.0f) })
    {
        const auto c = juce::Point<float> (p.x * ux, p.y * uy);
        const float d = 22.0f * juce::jmin (ux, uy);
        const auto head = juce::Rectangle<float> (d, d).withCentre (c);
        g.setGradientFill (juce::ColourGradient (juce::Colour (0xff6a6a62), head.getX(), head.getY(), juce::Colour (0xff1c1c1a), head.getRight(),
                                                 head.getBottom(), false));
        g.fillEllipse (head);
        g.setColour (juce::Colour (0xff0c0c0c));
        g.drawEllipse (head, 1.0f);
        // The cross slot.
        g.drawLine (c.x - d * 0.28f, c.y - d * 0.1f, c.x + d * 0.28f, c.y + d * 0.1f, juce::jmax (1.5f, d * 0.12f));
        g.drawLine (c.x - d * 0.1f, c.y + d * 0.28f, c.x + d * 0.1f, c.y - d * 0.28f, juce::jmax (1.5f, d * 0.12f));
    }
    const auto mark = juce::Rectangle<float> (140.0f * ux, 22.0f * uy, 60.0f * ux, 40.0f * uy);
    g.setFont (wingFontPx (mark.getHeight() * 0.72f, WingWeight::SemiBold).italicised());
    g.setColour (juce::Colour (0xff7a7c7a));
    g.drawText ("FX", mark.translated (-1.0f, -1.0f), juce::Justification::centredLeft, false);
    g.setColour (juce::Colour (0xff262826));
    g.drawText ("FX", mark, juce::Justification::centredLeft, false);

    // The group captions, #707070 and centred under their two columns.
    const auto font = wingFontPx (15.0f * uy);
    g.setFont (font);
    g.setColour (Colours::textDim);
    const float textW = juce::GlyphArrangement::getStringWidth (font, "PREMIUM FX");
    const float gemW = 26.0f * ux, gap = 8.0f * ux;
    const auto premium = premiumLabel.toFloat();
    const float x0 = premium.getCentreX() - (gemW + gap + textW) * 0.5f;
    drawIcon (g, Icon::Premium, juce::Rectangle<float> (x0, premium.getY(), gemW, premium.getHeight()).withSizeKeepingCentre (gemW, 18.0f * uy), Colours::textDim);
    g.drawText ("PREMIUM FX", juce::Rectangle<float> (x0 + gemW + gap, premium.getY(), textW + 4.0f, premium.getHeight()), juce::Justification::centredLeft, false);
    g.drawText ("STANDARD FX", standardLabel, juce::Justification::centred);
}

void EffectsPage::resized()
{
    editor->setBounds (getLocalBounds());
    // The 3.1 rack, in WING pixels of the 1280 x 720 page (no parameter bar):
    // the rack 10 px in; the slots 282 x 140 px from 63, 75, 287 px across and
    // 145 px down, the standard pair 12 px further right; the captions under them.
    const float ux = (float) getWidth() / 1280.0f, uy = (float) getHeight() / 720.0f;
    auto at = [ux, uy] (float x, float y, float w, float h)
    { return juce::Rectangle<float> (x * ux, y * uy, w * ux, h * uy).toNearestInt(); };
    rack = at (10.0f, 10.0f, 1260.0f, 700.0f);
    premiumLabel = at (63.0f, 657.0f, 569.0f, 30.0f);
    standardLabel = at (649.0f, 657.0f, 569.0f, 30.0f);
    static constexpr float columns[] = { 63.0f, 350.0f, 649.0f, 936.0f };
    for (int i = 0; i < tiles.size(); ++i)
    {
        // Slots run down the columns: 1-4 in the first, 5-8 in the second ...
        const int col = i / 4, row = i % 4;
        tiles[i]->setBounds (at (columns[col], 75.0f + 145.0f * (float) row, 282.0f, 140.0f));
    }
}

} // namespace livedaw::ui
