#include "dsp/DspUtils.h"
#include "engine/Models.h"
#include "ui/Faceplate.h"
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

    /** The control a device's rack tile shows on its ring (DECAY for reverbs, TIME for delays...). */
    int keyParam (const models::Model& m)
    {
        static const char* const ids[] = { "dcy", "time", "dly", "spd", "fbase", "semi", "semi1", "oct1", "int", "gin", "hrm",
                                           "wid", "body", "drv", "gain", "drive", "vol", "pre", "g", "TRIM", "thr", "1-thr", "hpf", "mix" };
        for (auto* id : ids)
            if (const int i = m.find (id); i >= 0 && m.params[(size_t) i].isContinuous())
                return i;
        for (size_t i = 0; i < m.params.size(); ++i)
            if (m.params[i].isContinuous())
                return (int) i;
        return -1;
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
        auto b = getLocalBounds().toFloat().reduced (1.5f);
        const int model = fx.getModel (slot);
        const bool device = model > ld::kFxExt;
        const bool loaded = fx.hasPlugin (slot) || device;
        const bool sel = page.selected == slot;

        g.setColour (juce::Colours::black);
        g.fillRect (b);
        g.setColour (sel ? Colours::yellow : Colours::screenLine);
        g.drawRect (b, sel ? 2.5f : 1.5f);

        // Slot number, bottom right.
        // (The box grows with the text: two digits didn't fit at larger screen sizes.)
        const auto font = wingFont (TypeScale::body, WingWeight::SemiBold);
        const juce::String text (slot + 1);
        const float w = juce::jmax (20.0f, juce::GlyphArrangement::getStringWidth (font, text) + 6.0f);
        auto number = juce::Rectangle<float> (b.getRight() - w - 2.0f, b.getBottom() - 18.0f, w, 16.0f);
        g.setColour (Dark::textDim);
        g.fillRect (number);
        g.setColour (juce::Colours::black);
        g.setFont (font);
        g.drawText (text, number, juce::Justification::centred);

        auto inner = b.reduced (6.0f);
        auto left = inner.removeFromLeft (inner.getWidth() * 0.28f);
        inner.removeFromLeft (6.0f);
        deviceArea = inner.removeFromTop (inner.getHeight() * 0.48f);
        meterArea = deviceArea.removeFromRight (juce::jmin (16.0f, deviceArea.getWidth() * 0.1f));
        deviceArea.removeFromRight (3.0f);

        if (! loaded)
        {
            // An empty slot: a blank rack unit with a "+".
            g.setColour (Colours::screenTile);
            g.fillRect (deviceArea);
            const float d = juce::jmin (deviceArea.getHeight() * 0.6f, 34.0f);
            auto circle = juce::Rectangle<float> (d, d).withCentre (deviceArea.getCentre());
            g.setColour (Dark::textDim);
            g.drawEllipse (circle, 2.0f);
            g.fillRect (juce::Rectangle<float> (d * 0.5f, 2.0f).withCentre (circle.getCentre()));
            g.fillRect (juce::Rectangle<float> (2.0f, d * 0.5f).withCentre (circle.getCentre()));
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
            g.drawFittedText (plugin != nullptr ? plugin->getName().toUpperCase() : juce::String ("FX " + juce::String (slot + 1)),
                              label.removeFromTop (label.getHeight() * 0.62f).toNearestInt(), juce::Justification::centredBottom, 1);
            g.setColour (juce::Colour (0xffc8c8c8));
            g.setFont (wingFontFit (TypeScale::caption, label.getHeight() * 0.7f));
            g.drawFittedText (plugin != nullptr ? plugin->getPluginDescription().manufacturerName.toUpperCase() : juce::String ("EXT"),
                              label.toNearestInt(), juce::Justification::centredTop, 1);
        }

        const auto user = findUser (ctx.engine, slot);
        const float level = user.strip != nullptr && ! params.bypass.load()
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

        // Source strip, PRE/POST and ON.
        const float chipH = juce::jlimit (12.0f, 22.0f, left.getHeight() * 0.13f);
        onArea = left.removeFromBottom (chipH);
        left.removeFromBottom (3.0f);
        prePostArea = left.removeFromBottom (chipH);
        left.removeFromBottom (4.0f);
        sourceArea = left;
        if (user.strip != nullptr)
        {
            // The destination: its name in a bar of its colour (plain when it has
            // its default name), its icon, and its default label under the icon.
            auto src = left;
            auto bar = src.removeFromTop (juce::jmax (10.0f, src.getHeight() * 0.18f));
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
            auto name = src.removeFromBottom (juce::jmin (18.0f, src.getHeight() * 0.32f));
            drawStripIcon (g, *user.strip, src.reduced (src.getWidth() * 0.22f, 3.0f), juce::Colours::white);
            g.setColour (juce::Colours::white);
            g.setFont (wingFontFit (TypeScale::body, name.getHeight(), WingWeight::SemiBold));
            g.drawFittedText (user.strip->getLabel(), name.toNearestInt(), juce::Justification::centred, 1, 0.7f);

            // PRE lit red, POST green, as on the console.
            auto pre = prePostArea.removeFromLeft (prePostArea.getWidth() / 2.0f);
            drawChip (g, pre, "PRE", user.post ? Colours::screenTile : Dark::pre, user.post ? Dark::textDim : juce::Colours::black);
            drawChip (g, prePostArea, "POST", user.post ? Dark::post : Colours::screenTile, user.post ? juce::Colours::black : Dark::textDim);
            prePostArea = pre.getUnion (prePostArea);
        }
        else
        {
            // Not inserted anywhere: the grey column with the FX logo (fig. 28).
            g.setColour (Colours::screenTile);
            g.fillRect (left);
            g.setColour (Colours::caption.withAlpha (0.55f));
            g.setFont (wingFontPx (juce::jmin (left.getWidth() * 0.32f, left.getHeight() * 0.3f), WingWeight::SemiBold).italicised());
            g.drawText ("FX", left, juce::Justification::centred, false);
        }
        const bool on = ! params.bypass.load();
        drawChip (g, onArea, "ON", on ? Colours::yellow : Colours::screenTile, on ? juce::Colours::black : Dark::textDim);

        // Ring and value: the device's key control (a plugin's: MIX).
        auto bottom = inner.withTrimmedTop (4.0f).withTrimmedRight (24.0f);
        knobArea = bottom.removeFromLeft (juce::jmin (bottom.getHeight(), bottom.getWidth() * 0.4f));
        ringParam = device ? keyParam (spec) : -1;
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
        const auto k = knobArea.reduced (knobArea.getWidth() * 0.12f);
        const float a0 = juce::MathConstants<float>::pi * 1.2f, a1 = juce::MathConstants<float>::pi * 2.8f;
        const float rad = k.getWidth() * 0.4f, thick = k.getWidth() * 0.18f;
        juce::Path track, value;
        track.addCentredArc (k.getCentreX(), k.getCentreY(), rad, rad, 0.0f, a0, a1, true);
        value.addCentredArc (k.getCentreX(), k.getCentreY(), rad, rad, 0.0f, a0, a0 + mix * (a1 - a0), true);
        g.setColour (Colours::screenTile);
        g.strokePath (track, juce::PathStrokeType (thick));
        g.setColour (on ? Colours::text : Dark::textDim);
        g.strokePath (value, juce::PathStrokeType (thick));
        bottom.removeFromLeft (6.0f);
        g.setColour (Colours::text);
        // [Fig. 28] the caption about a tenth of the tile tall, the value larger.
        g.setFont (wingFontPx (juce::jmin ((float) getHeight() * 0.1f, bottom.getHeight() * 0.3f)));
        g.drawFittedText (ringLabel, bottom.removeFromTop (bottom.getHeight() / 2.0f).toNearestInt(), juce::Justification::bottomLeft, 1, 0.8f);
        g.setFont (wingFontPx (juce::jmin ((float) getHeight() * 0.155f, bottom.getHeight() * 0.62f), WingWeight::SemiBold));
        g.drawFittedText (ringValue, bottom.toNearestInt(), juce::Justification::topLeft, 1, 0.8f);
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
        {
            // Open the slot: a device or plugin in the editor, or choose one for an empty slot.
            auto& fx = ctx.engine.getFx();
            if (fx.isLoaded (slot))
                page.editSlot (slot);
            else
                showFxDeviceMenu (ctx, slot, this, [this] { page.editSlot (slot); });
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
            << (int) fx.isFaulted (slot) << (int) fx.isDoubleAssigned (slot) << (int) (page.selected == slot);
        if (model > ld::kFxExt)
        {
            const auto& spec = models::model (models::Fx, model);
            if (const int key = keyParam (spec); key >= 0 && key < (int) spec.params.size())
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
        startTimerHz (6);
    }

    void setSlot (int s)
    {
        slot = juce::jlimit (0, kNumFx - 1, s);
        mix.setBinding (bindLinear ("FX MIX", ctx.engine.getFx().params (slot).mix, 0.0f, 100.0f, " %", 0, 100.0f));
        deviceField.setCaption ("FX " + juce::String (slot + 1) + (slot < 8 ? "  PREMIUM" : "  STANDARD"));
        rebind();
    }

    int getSlot() const noexcept { return slot; }

    std::vector<KnobBinding> knobs()
    {
        std::vector<KnobBinding> k;
        auto& fx = ctx.engine.getFx();
        const int model = fx.getModel (slot);
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
                info = plugin->getPluginDescription().manufacturerName.toUpperCase() + "   " + plugin->getPluginDescription().pluginFormatName
                       + "   LOAD " + juce::String (juce::roundToInt (fx.getLoad (slot) * 100.0f)) + " %";
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
    }

    void mouseUp (const juce::MouseEvent& e) override
    {
        if (ctx.engine.getFx().getModel (slot) == ld::kFxNone && body.contains (e.getPosition()))
            showFxDeviceMenu (ctx, slot, this, [this] { rebind(); });
    }

private:
    void rebind()
    {
        auto& fx = ctx.engine.getFx();
        shownModel = fx.getModel (slot);
        const bool device = shownModel > ld::kFxExt;
        face.setVisible (device);
        if (device)
            face.bind (models::Fx, shownModel, fx.params (slot).values, [this] { return ctx.engine.getFx().getMeter (slot); });
        else
            face.unbind();
        openKey.setVisible (shownModel == ld::kFxExt);
        pluginKey.setVisible (shownModel == ld::kFxExt);
        openKey.setEnabled (fx.hasPlugin (slot));
        deviceField.repaint();
        page6 = 0;
        page.notifyKnobsChanged();
        repaint();
    }

    void timerCallback() override
    {
        if (ctx.engine.getFx().getModel (slot) != shownModel)
            rebind();
        openKey.setEnabled (ctx.engine.getFx().hasPlugin (slot));
        repaint (header);
    }

    UiContext& ctx;
    EffectsPage& page;
    int slot = 0, shownModel = -1, page6 = 0;
    Faceplate face;
    WingKey backKey { "RACK", Icon::ChevronLeft }, onKey { "ON" }, openKey { "OPEN EDITOR", Icon::Edit },
        pluginKey { "LOAD / REPLACE", Icon::Folder };
    DropdownField deviceField { "FX 1",
                                [this]
                                {
                                    const int m = ctx.engine.getFx().getModel (slot);
                                    return m == ld::kFxNone ? juce::String ("SELECT DEVICE") : models::model (models::Fx, m).name.toUpperCase();
                                },
                                [this] (DropdownField& f) { showFxDeviceMenu (ctx, slot, &f, [this] { rebind(); }); } };
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
    {
        selected = slot;
        editor->setSlot (slot);
    }
    editor->setVisible (open);
    for (auto* t : tiles)
        t->setVisible (! open);
    notifyKnobsChanged();
    repaint();
}

EffectsPage::~EffectsPage() = default;

int EffectsPage::tapParam() const
{
    if (editor == nullptr || ! editor->isVisible())
        return -1;
    const auto& spec = models::model (models::Fx, ctx.engine.getFx().getModel (editor->getSlot()));
    for (size_t i = 0; i < spec.params.size(); ++i)
    {
        const auto& p = spec.params[i];
        if (p.unit == "ms" && p.isContinuous() && (p.id == "time" || p.id == "dly" || p.label.equalsIgnoreCase ("TIME")))
            return (int) i;
    }
    return -1;
}

void EffectsPage::blankKeyPressed()
{
    // Tap tempo: the time between two taps (0.1 to 3 s) becomes the delay time.
    const int param = tapParam();
    if (param < 0)
        return;
    const double now = juce::Time::getMillisecondCounterHiRes();
    const double interval = now - lastTap;
    lastTap = now;
    if (interval < 100.0 || interval > 3000.0)
        return;
    const int slot = editor->getSlot();
    const auto& p = models::model (models::Fx, ctx.engine.getFx().getModel (slot)).params[(size_t) param];
    ctx.engine.getFx().params (slot).values[param].store (juce::jlimit (p.min, p.max, (float) interval));
}

std::vector<KnobBinding> EffectsPage::getKnobs()
{
    if (editor != nullptr && editor->isVisible())
        return editor->knobs();
    std::vector<KnobBinding> k;
    const int row = selected / 4 * 4;
    for (int i = 0; i < 4; ++i)
        k.push_back (bindLinear ("FX" + juce::String (row + i + 1) + " MIX", ctx.engine.getFx().params (row + i).mix, 0.0f, 100.0f, "%", 0, 100.0f));
    return k;
}

void EffectsPage::paint (juce::Graphics& g)
{
    g.fillAll (Colours::screen);
    if (editor != nullptr && editor->isVisible())
        return;

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

    // [Fig. 28] the group captions are small and grey, printed on the rack.
    g.setColour (Colours::caption.withAlpha (0.7f));
    g.setFont (wingFontPx (juce::jmin (TypeScale::body * getWingScreenHeight(), premiumLabel.getHeight() * 0.5f)));
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
    editor->setBounds (getLocalBounds());
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
