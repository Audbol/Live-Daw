#include "ui/FaderSection.h"

namespace livedaw::ui
{

void toggleSpill (UiContext& ctx, std::optional<LayerSet::Section> origin, const LayerSet::Spill& spill)
{
    auto& layers = ctx.show.layers;
    if (const auto section = layers.findSpill (spill))
        layers.endSpill (*section); // the section's layer and position come back
    else
        layers.startSpill (LayerSet::spillTarget (origin), spill);
    notifyLayersChangedAsync (ctx);
}

bool isSpilling (UiContext& ctx, const LayerSet::Spill& spill)
{
    return ctx.show.layers.findSpill (spill).has_value();
}

FaderSection::FaderSection (UiContext& c, LayerSet::Section s) : ctx (c), section (s)
{
    // VIEW: the section's OVERVIEW; hold for CONFIG (manual table 4.2).
    viewKey.setTooltip ("VIEW: overview of this section (hold: CONFIG)");
    viewKey.onPress = [this]
    {
        ctx.viewSection = section;
        if (ctx.showScreen)
            ctx.showScreen (Screen::Overview);
    };
    viewKey.onHold = [this]
    {
        ctx.viewSection = section;
        if (ctx.showScreen)
            ctx.showScreen (Screen::Config);
    };
    viewKey.isActive = [this]
    {
        if (! ctx.getScreen || ctx.viewSection != section)
            return false;
        const auto screen = ctx.getScreen();
        return screen == Screen::Overview || screen == Screen::Config || screen == Screen::Faders || screen == Screen::Sends;
    };
    addAndMakeVisible (viewKey);

    // Shift keys: scroll by the shift step, lit while there's more to scroll to;
    // hold for the first / last faders of the layer.
    for (auto* k : { &shiftLeft, &shiftRight })
    {
        const int direction = k == &shiftLeft ? -1 : 1;
        k->arrow = direction;
        k->ledColour = Colours::keyWhite;
        k->onPress = [this, direction] { shift (direction); };
        k->onHold = [this, direction] { jumpToEnd (direction); };
        k->ledState = [this, direction]
        {
            const int total = (int) layer.faders.size();
            const bool more = direction < 0 ? offset > 0 : offset + strips.size() < total;
            return more ? SurfaceKey::Lit : SurfaceKey::Off;
        };
        addAndMakeVisible (k);
    }

    if (section == LayerSet::Center)
    {
        sofKey.ledColour = Colours::keyLayer; // red-orange, not SOLO's orange
        sofKey.backlit = true;
        sofKey.setTooltip ("SOF FLIP (Sends on Fader): select a bus, main or matrix to set every input's send to it "
                           "on the input faders, or select an input to set its sends on the bus, main and matrix faders");
        // SOF BUTTON (SETUP > SURFACE): AUTO lights during SOF and flashes in
        // the alternative SOF (a fixed destination); ON lights, FLASH flashes.
        sofKey.ledState = [this]
        {
            if (! ctx.sendsOnFader)
                return SurfaceKey::Off;
            const int mode = ctx.settings.getIntValue (uiprefs::sofButton, 0);
            if (mode == 2 || (mode == 0 && ctx.sofDestination.has_value()))
                return SurfaceKey::Blink;
            return SurfaceKey::Lit;
        };
        sofKey.onPress = [this]
        {
            ctx.sendsOnFader = ! ctx.sendsOnFader;
            ctx.sofDestination.reset();
            if (ctx.sofChanged)
                ctx.sofChanged();
        };
        addAndMakeVisible (sofKey);
    }
    startTimerHz (4);
}

void FaderSection::timerCallback()
{
    // A spill follows the console: a strip assigned to the spilled DCA (or a
    // send switched on to the spilled bus) joins it at once.
    if (layer.id != LayerSet::kSpillId)
        return;
    auto now = ctx.show.layers.getSelectedLayer (ctx.engine, section);
    if (now.id == layer.id && now.faders != layer.faders)
    {
        layer = std::move (now);
        refreshFaders();
    }
}

int FaderSection::shiftStep() const
{
    // SETUP > SURFACE: scroll 4, 8 or 12 faders at a time.
    const int step = ctx.settings.getIntValue ("ui.shiftStep", 4);
    return step == 8 || step == 12 ? step : 4;
}

void FaderSection::refresh()
{
    // The seven layer keys, then the user layers that have no USER key.
    keys = ctx.show.layers.getDefaultLayers (ctx.engine, section);
    numFixedKeys = (int) keys.size();
    const auto& user = ctx.show.layers.getUserLayers();
    for (size_t i = 0; i < user.size(); ++i)
    {
        if (i < 2)
            userIds[i] = user[i].id;
        else
            keys.push_back (user[i]);
    }
    for (size_t i = user.size(); i < 2; ++i)
        userIds[i] = {};

    layerKeys.clear();
    for (size_t i = 0; i < keys.size(); ++i)
    {
        auto* k = layerKeys.add (new SurfaceKey (LayerSet::keyLabel (keys[i])));
        k->ledColour = Colours::keyLayer;
        k->setTooltip (keys[i].name + (keys[i].user ? " (user layer)" : ""));
        k->onPress = [this, id = keys[i].id] { selectLayer (id); };
        k->ledState = [this, i] { return i < keys.size() && isKeyLit (keys[i]) ? SurfaceKey::Lit : SurfaceKey::Off; };
        addAndMakeVisible (k);
    }
    shiftLeft.setLabel (juce::String (shiftStep()));
    shiftRight.setLabel (juce::String (shiftStep()));

    // The scroll position lives in the layer set, so the end of a spill brings it back.
    layer = ctx.show.layers.getSelectedLayer (ctx.engine, section);
    offset = ctx.show.layers.getOffset (section);

    resized();
    refreshFaders();
}

bool FaderSection::isKeyLit (const Layer& key) const
{
    if (key.id == layer.id)
        return true;
    // A user layer selected by its id lights its USER key.
    for (int i = 0; i < 2; ++i)
        if (userIds[i].isNotEmpty() && layer.id == userIds[i] && key.id == "user-" + juce::String (i + 1))
            return true;
    return false;
}

void FaderSection::selectLayer (const juce::String& id)
{
    // A layer key ends a spill on the section (without going back).
    const bool wasSpilling = ctx.show.layers.getSpill (section).has_value();
    ctx.show.layers.setSelectedLayerId (section, id);
    offset = 0;
    layer = ctx.show.layers.getSelectedLayer (ctx.engine, section);
    refreshFaders();
    if (wasSpilling)
        notifyLayersChangedAsync (ctx); // the SPILL keys on the screens

    // USER LAYER LINK (SETUP > SURFACE): USER 1 / USER 2 switch the linked
    // sections (L/C, C/R) too.
    if (id == "user-1" || id == "user-2")
    {
        bool changed = false;
        auto link = [&] (int pref, LayerSet::Section a, LayerSet::Section b)
        {
            if (! ctx.settings.getBoolValue (uiprefs::userLayerLink[pref], false) || (section != a && section != b))
                return;
            const auto other = section == a ? b : a;
            if (ctx.show.layers.getSelectedLayerId (other) != id)
            {
                ctx.show.layers.setSelectedLayerId (other, id);
                changed = true;
            }
        };
        link (0, LayerSet::Left, LayerSet::Center);
        link (1, LayerSet::Center, LayerSet::Right);
        // L/C and C/R together chain all three.
        if (section != LayerSet::Center && ctx.settings.getBoolValue (uiprefs::userLayerLink[0], false)
            && ctx.settings.getBoolValue (uiprefs::userLayerLink[1], false))
        {
            const auto opposite = section == LayerSet::Left ? LayerSet::Right : LayerSet::Left;
            if (ctx.show.layers.getSelectedLayerId (opposite) != id)
            {
                ctx.show.layers.setSelectedLayerId (opposite, id);
                changed = true;
            }
        }
        if (changed)
            notifyLayersChangedAsync (ctx);
    }
}

void FaderSection::shift (int direction)
{
    // FULL FADER PAGING: a whole section width per press.
    const int step = ctx.settings.getBoolValue (uiprefs::fullFaderPaging, false) ? juce::jmax (1, strips.size()) : shiftStep();
    offset += direction * step;
    refreshFaders();
}

void FaderSection::jumpToEnd (int direction)
{
    offset = direction < 0 ? 0 : (int) layer.faders.size();
    refreshFaders();
}

void FaderSection::reveal (const state::FaderRef& fader)
{
    const auto id = ctx.show.layers.findDefaultLayerContaining (ctx.engine, section, fader);
    if (id.isEmpty())
        return;
    if (id != layer.id)
        selectLayer (id);

    const int n = strips.size();
    for (int i = 0; i < (int) layer.faders.size(); ++i)
    {
        if (layer.faders[(size_t) i] == fader)
        {
            if (i < offset || i >= offset + n)
            {
                const int step = shiftStep();
                offset = i / step * step;
                refreshFaders();
            }
            return;
        }
    }
}

void FaderSection::refreshFaders()
{
    const int n = juce::jmax (1, ctx.show.layers.getEffectiveWidth (section));
    if (strips.size() != n)
    {
        strips.clear();
        for (int i = 0; i < n; ++i)
            addAndMakeVisible (strips.add (new FaderStrip (ctx, section)));
        resized();
    }

    const int total = (int) layer.faders.size();
    offset = juce::jlimit (0, juce::jmax (0, total - n), offset);
    ctx.show.layers.setOffset (section, offset);

    for (int i = 0; i < n; ++i)
    {
        const int index = offset + i;
        strips[i]->setTarget (index < total ? std::optional<state::FaderRef> (layer.faders[(size_t) index]) : std::nullopt);
    }
    repaint();
}

void FaderSection::paint (juce::Graphics& g)
{
    const auto& sc = surfaceColours();
    g.fillAll (sc.surface);

    // The layer column's plate the height of the section (the black surface's
    // lighter grey; the silver surface has none).
    if (! sc.layerColumn.isTransparent())
    {
        const auto plate = column.reduced (4, 2).toFloat();
        g.setGradientFill (juce::ColourGradient (sc.layerColumn.brighter (0.12f), plate.getX(), 0.0f,
                                                 sc.layerColumn.darker (0.1f), plate.getRight(), 0.0f, false));
        g.fillRoundedRectangle (plate, 5.0f);
        g.setColour (juce::Colours::black.withAlpha (0.5f));
        g.drawRoundedRectangle (plate.reduced (0.5f), 5.0f, 1.0f);
    }

    // SENDS ON FADERS over the SOF FLIP key.
    if (section == LayerSet::Center && sofKey.isVisible())
        drawSilk (g, sofKey.getBounds().translated (0, -14).withHeight (12).expanded (6, 0).toFloat(), "SENDS ON FADERS",
                  juce::jlimit (7.0f, 9.0f, (float) column.getWidth() * 0.15f), juce::Justification::centred, sc.surfacePrint);

    // The printed line between the sections.
    g.setColour (sc.surfaceLine);
    g.fillRect (getWidth() - 1, 0, 1, getHeight());
}

void FaderSection::paintOverChildren (juce::Graphics& g)
{
    if (strips.isEmpty())
        return;

    // Strips come in modules of four (their LCD and meter panels run across the
    // module), with the meter scale printed between strips.
    const auto meter = strips[0]->getMeterArea();
    const int y0 = strips[0]->getY();
    for (int first = 0; first < strips.size(); first += 4)
    {
        const int last = juce::jmin (strips.size(), first + 4) - 1;
        if (last + 1 < strips.size())
        {
            g.setColour (surfaceColours().surfaceLine);
            g.fillRect (strips[last]->getRight(), 0, 1, getHeight());
        }

        const float rowH = (float) meter.getHeight() / (float) FaderStrip::kMeterRows;
        const float size = juce::jlimit (7.0f, 11.0f, rowH * 0.9f);
        for (int i = first; i < last; ++i)
        {
            const float x = (float) strips[i]->getRight();
            const float w = juce::jmin (22.0f, (float) strips[i]->getWidth() * 0.36f);
            auto row = [&] (int r) { return juce::Rectangle<float> (x - w * 0.5f, (float) (y0 + meter.getY()) + rowH * (float) r, w, rowH); };
            for (int r = 0; r < FaderStrip::kMeterSegments; ++r)
                drawSilk (g, row (r + 1), FaderStrip::kMeterLabels[r], size);

            // A dash either side of each label, as printed.
            const float dash = juce::jmin (5.0f, (float) strips[i]->getWidth() * 0.06f), gap = w * 0.5f + dash * 0.6f;
            g.setColour (surfaceColours().silk.withAlpha (0.8f));
            for (int r = 0; r < FaderStrip::kMeterRows && dash >= 2.0f; ++r)
                for (float side : { -1.0f, 1.0f })
                    g.fillRect (juce::Rectangle<float> (x + side * gap - (side < 0 ? dash : 0.0f), row (r).getCentreY() - 0.5f, dash, 1.0f));

            // DYN outlined, GATE inverse, as printed.
            auto dyn = row (0).reduced (0.0f, rowH * 0.06f);
            g.setColour (surfaceColours().silk);
            g.drawRoundedRectangle (dyn, 2.0f, 1.0f);
            drawSilk (g, dyn, "DYN", size * 0.9f);
            auto gate = row (FaderStrip::kMeterRows - 1).reduced (0.0f, rowH * 0.06f);
            g.setColour (surfaceColours().silk);
            g.fillRoundedRectangle (gate, 2.0f);
            drawSilk (g, gate, "GATE", size * 0.9f, juce::Justification::centred, juce::Colours::black);
        }
    }
}

void FaderSection::resized()
{
    auto b = getLocalBounds();
    column = b.removeFromLeft (kBankColumnWidth);

    const int n = strips.size();
    if (n > 0)
    {
        const int w = b.getWidth() / n;
        for (int i = 0; i < n; ++i)
        {
            strips[i]->setBounds (b.getX() + i * w, b.getY(), w, b.getHeight());
            strips[i]->setModuleEdges (i % 4 == 0, i % 4 == 3 || i == n - 1);
        }
    }

    // VIEW level with the scribble strips; the layer keys from the SELECT row
    // down (the center section's SOF FLIP first, under its caption).
    auto c = column.reduced (4, 0);
    const auto scribble = n > 0 ? strips[0]->getScribbleArea() : juce::Rectangle<int> (0, 4, 10, 56);
    const int viewSize = juce::jmin (juce::roundToInt ((float) c.getWidth() * 0.66f), 40);
    viewKey.setBounds (juce::Rectangle<int> (viewSize, viewSize).withCentre ({ c.getCentreX(), scribble.getCentreY() }));

    const int keyW = c.getWidth() - 6;
    c.setTop (scribble.getBottom() + 10);
    if (section == LayerSet::Center)
        c.removeFromTop (14); // the caption
    c.removeFromBottom (6);

    const int numKeys = layerKeys.size() + 2 + (section == LayerSet::Center ? 1 : 0);
    const int pitch = juce::jlimit (18, 44, c.getHeight() / juce::jmax (1, numKeys));
    const int keyH = juce::jmin (juce::roundToInt ((float) keyW * 0.66f), pitch - juce::jmax (4, pitch / 4));
    auto place = [&] (juce::Component& k)
    {
        const auto cell = c.removeFromTop (pitch);
        k.setVisible (cell.getBottom() <= column.getBottom() && cell.getHeight() == pitch);
        k.setBounds (cell.withSizeKeepingCentre (keyW, keyH));
    };
    if (section == LayerSet::Center)
        place (sofKey);
    // The seven keys, the shift keys, then further user layers.
    const int fixed = juce::jmin (layerKeys.size(), numFixedKeys);
    for (int i = 0; i < fixed; ++i)
        place (*layerKeys[i]);
    place (shiftLeft);
    place (shiftRight);
    for (int i = fixed; i < layerKeys.size(); ++i)
        place (*layerKeys[i]);
}

} // namespace livedaw::ui
