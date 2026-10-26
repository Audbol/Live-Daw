#include "ui/CompactSection.h"

namespace livedaw::ui
{

namespace
{
    const char* const modeLabels[] = { "BUS", "DCA", "MAIN\nMATRIX", "USER" };
    constexpr compact::KeyMode modes[] = { compact::KeyMode::Bus, compact::KeyMode::Dca, compact::KeyMode::Main, compact::KeyMode::User };

    // The LCD's colours [photos of a WING COMPACT]: a colour screen, white on blue.
    const juce::Colour lcdBlue { 0xff1d7fd8 }, lcdDim { 0xff0b2c4c };
} // namespace

CompactSection::CompactSection (UiContext& c) : ctx (c), fader13 (c, LayerSet::Center)
{
    for (int i = 0; i < 4; ++i)
    {
        auto& k = modeKeys[i];
        k = std::make_unique<SurfaceKey> (modeLabels[i]);
        k->ledColour = Colours::keyLayer;
        k->onPress = [this, i] { setKeyMode (modes[i]); };
        k->ledState = [this, i] { return keyMode() == modes[i] ? SurfaceKey::Lit : SurfaceKey::Off; };
        addAndMakeVisible (*k);
    }
    modeKeys[0]->setTooltip ("BUS: the 16 keys are BUS 1-16 (press: spill the bus and put it on fader 13)");
    modeKeys[1]->setTooltip ("DCA: the 16 keys are DCA 1-16 (press: spill the DCA onto the faders)");
    modeKeys[2]->setTooltip ("MAIN MATRIX: keys 1-4 are MAIN 1-4, 9-16 MATRIX 1-8 (press: put it on fader 13)");
    modeKeys[3]->setTooltip ("USER: the 16 keys are custom control keys (1-8 the USER layer, 9-16 the custom control layer shown)");

    // VIEW: CUSTOM CONTROLS on the COMPACT's USER 1-4 layers; Sidecar's
    // closest is the USER layer (the one the USER keys 1-8 play).
    viewKey.setTooltip ("VIEW: CUSTOM CONTROLS (CC EDIT) on the USER layer");
    viewKey.onPress = [this]
    {
        if (ctx.showCustomControls)
            ctx.showCustomControls (cc::kUserLayer);
        viewOpened = true;
    };
    viewKey.isActive = [this]
    {
        viewOpened = viewOpened && ctx.getScreen && ctx.getScreen() == Screen::Setup;
        return viewOpened;
    };
    addAndMakeVisible (viewKey);

    for (int i = 0; i < layout::compact::kNumKeys; ++i)
    {
        auto* k = keys.add (new SurfaceKey (juce::String (i + 1)));
        k->ledColour = Colours::keyLayer;
        k->onPress = [this, i]
        {
            if (const auto f = keyFunction (i); f.isAssigned())
            {
                cc::press (ctx, f);
                cc::release (ctx, f);
            }
            // A bus, main or matrix goes on fader 13 too.
            if (const auto mf = keyMainFader (i))
            {
                ctx.settings.setValue (uiprefs::compactMainFader, compact::mainFaderToken (*mf));
                refresh();
            }
        };
        k->ledState = [this, i]
        {
            const auto f = keyFunction (i);
            const auto mf = keyMainFader (i);
            return (f.isAssigned() && cc::isLit (ctx, f)) || (mf && *mf == getMainFader()) ? SurfaceKey::Lit : SurfaceKey::Off;
        };
        addAndMakeVisible (k);
    }

    // Fader 13 has no scribble strip: the LCD names what it controls.
    fader13.setScribbleShown (false);
    addAndMakeVisible (fader13);
    startTimerHz (4);
}

compact::KeyMode CompactSection::keyMode() const
{
    return compact::keyModeFromToken (ctx.settings.getValue (uiprefs::compactKeyMode, "BUS"));
}

void CompactSection::setKeyMode (compact::KeyMode m)
{
    ctx.settings.setValue (uiprefs::compactKeyMode, compact::keyModeToken (m));
    refresh();
}

compact::MainFader CompactSection::getMainFader() const
{
    return compact::parseMainFader (ctx.settings.getValue (uiprefs::compactMainFader, "MAIN.1"));
}

void CompactSection::setStripWidth (int w)
{
    if (w != stripW)
    {
        stripW = w;
        resized();
    }
}

cc::Function CompactSection::keyFunction (int key) const
{
    switch (keyMode())
    {
        case compact::KeyMode::Bus:
        {
            const auto& buses = ctx.engine.getBuses();
            if (juce::isPositiveAndBelow (key, (int) buses.size()))
                return { cc::BtnFaderSpill, 100 + state::StripRef::of (*buses[(size_t) key]).id, 0 };
            break;
        }
        case compact::KeyMode::Dca:
            if (key < kNumDcas)
                return { cc::BtnFaderSpill, key, 0 };
            break;
        case compact::KeyMode::User:
        {
            if (ctx.customControls == nullptr)
                break;
            const int layer = key < 8 ? cc::kUserLayer : ctx.customControls->currentLayer;
            const auto& control = ctx.customControls->get (layer, (key % 8) / 2);
            return key % 2 == 0 ? control.upper : control.lower;
        }
        case compact::KeyMode::Main:
            break;
    }
    return {};
}

Strip* CompactSection::mainFaderStrip (const compact::MainFader& mf) const
{
    auto at = [this] (StripKind kind, int number) -> Strip*
    {
        const auto& list = ctx.engine.getStrips (kind);
        return juce::isPositiveAndBelow (number - 1, (int) list.size()) ? list[(size_t) number - 1].get() : nullptr;
    };
    switch (mf.group)
    {
        case compact::MainFader::Main:    return at (StripKind::Main, mf.number);
        case compact::MainFader::Matrix:  return at (StripKind::Matrix, mf.number);
        case compact::MainFader::Channel: return at (StripKind::Channel, mf.number);
        case compact::MainFader::Aux:     return at (StripKind::Aux, mf.number);
        case compact::MainFader::Bus:     return at (StripKind::Bus, mf.number);
        case compact::MainFader::Off:
        case compact::MainFader::Dca:     break;
    }
    return nullptr;
}

std::optional<compact::MainFader> CompactSection::keyMainFader (int key) const
{
    const auto mf = compact::keyMainFader (keyMode(), key);
    return mf && mainFaderStrip (*mf) != nullptr ? mf : std::nullopt;
}

juce::String CompactSection::keyName (int key) const
{
    if (const auto mf = keyMainFader (key))
        if (const auto* s = mainFaderStrip (*mf))
            return stripName (*s);
    const auto f = keyFunction (key);
    if (! f.isAssigned())
        return {};
    switch (keyMode())
    {
        case compact::KeyMode::Dca:  return ctx.engine.getDcaName (key);
        case compact::KeyMode::User: return cc::describe (ctx, cc::Slot::Upper, f);
        case compact::KeyMode::Bus:
        case compact::KeyMode::Main: break;
    }
    return {};
}

void CompactSection::refresh()
{
    // Fader 13: its assignment, if the console has it.
    const auto mf = getMainFader();
    std::optional<state::FaderRef> target;
    if (mf.group == compact::MainFader::Dca)
        target = state::FaderRef::ofDca (mf.number - 1);
    else if (auto* s = mainFaderStrip (mf))
        target = state::FaderRef::of (state::StripRef::of (*s));
    fader13.setTarget (target);
    fader13.refresh();

    for (int i = 0; i < keys.size(); ++i)
    {
        keys[i]->setEnabled (keyFunction (i).isAssigned() || keyMainFader (i).has_value());
        keys[i]->setTooltip (keyName (i));
    }
    shownLcd = {};
    repaint();
}

juce::String CompactSection::lcdState() const
{
    juce::String s = compact::mainFaderToken (getMainFader()) + "|" + compact::keyModeToken (keyMode());
    for (int i = 0; i < keys.size(); ++i)
        s << "|" << keyName (i) << (keys[i]->isShownLit() ? "*" : "");
    return s;
}

void CompactSection::timerCallback()
{
    // The LCD follows names, the keys' states and the custom controls; fader
    // 13 follows its assignment (WING remote clients set /$ctl/cfg/mfdr too).
    if (const auto mf = getMainFader(); ! (mf == shownMainFader))
    {
        shownMainFader = mf;
        refresh();
    }
    if (const auto state = lcdState(); state != shownLcd)
    {
        shownLcd = state;
        repaint (lcd);
    }
}

void CompactSection::paint (juce::Graphics& g)
{
    const auto& sc = surfaceColours();
    g.fillAll (sc.surface);

    // The section's printed frame [WING COMPACT], and the line between it and the faders.
    g.setColour (sc.surfaceLine);
    g.fillRect (0, 0, 1, getHeight());
    if (! sc.deckEdge.isTransparent())
    {
        g.setColour (sc.deckEdge);
        g.drawRoundedRectangle (getLocalBounds().toFloat().reduced (4.0f, 3.0f), 4.0f, 1.0f);
    }

    // The LCD: fader 13's assignment and the keys' mode in a blue title bar,
    // then the 16 keys in their rows, a lit key's cell filled.
    if (lcd.isEmpty())
        return;
    auto r = lcd.toFloat();
    g.setColour (juce::Colours::black);
    g.fillRoundedRectangle (r, 3.0f);
    r = r.reduced (3.0f);
    const float titleH = juce::jlimit (12.0f, 30.0f, r.getHeight() * 0.22f);
    auto title = r.removeFromTop (titleH);
    g.setColour (lcdBlue);
    g.fillRect (title);
    g.setColour (juce::Colours::white);
    const auto font = juce::Font (juce::FontOptions (juce::jmin (titleH * 0.62f, 14.0f), juce::Font::bold));
    g.setFont (font);
    static const char* const modeNames[] = { "BUS", "DCA", "MAIN / MTX", "USER" };
    // What fader 13 controls: "13 MAIN 1", with its name when it has one.
    const auto mf = getMainFader();
    auto fader = "13 " + compact::mainFaderName (mf);
    if (const auto* s = mainFaderStrip (mf); s != nullptr && stripName (*s) != s->getLabel())
        fader << " " << stripName (*s).toUpperCase();
    g.drawFittedText (fader, title.reduced (4.0f, 0.0f).toNearestInt(), juce::Justification::centredLeft, 1, 0.7f);
    g.drawFittedText (modeNames[(int) keyMode()], title.reduced (4.0f, 0.0f).toNearestInt(), juce::Justification::centredRight, 1, 0.7f);

    r.removeFromTop (2.0f);
    const float cellW = r.getWidth() / 2.0f, cellH = r.getHeight() / (float) (layout::compact::kNumKeys / 2);
    g.setFont (juce::Font (juce::FontOptions (juce::jlimit (6.0f, 12.0f, cellH * 0.62f))));
    for (int i = 0; i < keys.size(); ++i)
    {
        auto cell = juce::Rectangle<float> (r.getX() + cellW * (float) (i % 2), r.getY() + cellH * (float) (i / 2), cellW, cellH).reduced (1.0f, 0.5f);
        const bool lit = keys[i]->isShownLit();
        const bool assigned = keys[i]->isEnabled();
        g.setColour (lit ? lcdBlue : lcdDim);
        if (assigned)
            g.fillRect (cell);
        g.setColour (assigned ? juce::Colours::white : juce::Colours::white.withAlpha (0.3f));
        const auto name = keyName (i);
        g.drawFittedText ((name.isNotEmpty() ? name : juce::String (i + 1)).toUpperCase(), cell.reduced (2.0f, 0.0f).toNearestInt(),
                          juce::Justification::centredLeft, 1, 0.6f);
    }
}

void CompactSection::resized()
{
    const auto plan = layout::compact::planKeys (getLocalBounds(), stripW);
    for (int i = 0; i < 4; ++i)
        modeKeys[i]->setBounds (plan.modes[(size_t) i]);
    viewKey.setBounds (plan.view);
    lcd = plan.lcd;
    for (int i = 0; i < keys.size(); ++i)
        keys[i]->setBounds (plan.keys[(size_t) i]);
    fader13.setBounds (plan.strip);
}

} // namespace livedaw::ui
