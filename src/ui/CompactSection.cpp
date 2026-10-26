#include "ui/CompactSection.h"

namespace livedaw::ui
{

namespace
{
    const char* const modeLabels[] = { "BUS", "DCA", "MAIN\nMATRIX", "USER" };
    constexpr compact::KeyMode modes[] = { compact::KeyMode::Bus, compact::KeyMode::Dca, compact::KeyMode::Main, compact::KeyMode::User };

    // The LCD's colours [photos of a WING COMPACT]: a colour screen, white on blue.
    const juce::Colour lcdBlue { 0xff1d7fd8 }, lcdDim { 0xff0b2c4c };

    /** The FADER SPILL target of MAIN `n` (1-based; custom.rs: kind 5). */
    constexpr int mainSpillTarget (int n) { return 500 + n - 1; }
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
    modeKeys[1]->setTooltip ("DCA: the 16 keys are DCA 1-16 (press: spill the DCA), or the custom control sets D1-D4 (USE DCA BUTTONS 1-16 AS CC)");
    modeKeys[2]->setTooltip ("MAIN MATRIX: keys 1-4 MAIN 1-4 (press: sends on fader, hold: spill), 5-8 the custom control set M/M, "
                             "9-16 MATRIX 1-8 (hold: sends on fader); fader 13 follows");
    modeKeys[3]->setTooltip ("USER: the 16 keys are the custom control sets U1-U4");

    // VIEW: the FADERS screen; held, CUSTOM CONTROLS on the keys' set (WING manual Table 4.4).
    viewKey.setTooltip ("VIEW: FADERS (hold: CUSTOM CONTROLS for the keys' set)");
    viewKey.onPress = [this]
    {
        viewOpened = false;
        if (ctx.showScreen)
            ctx.showScreen (Screen::Faders);
    };
    viewKey.onHold = [this]
    {
        const auto m = keyMode();
        const int set = m == compact::KeyMode::Main ? compact::kSetMainMatrix : m == compact::KeyMode::Dca ? compact::kFirstDcaSet : 0;
        if (ctx.showCustomControls)
            ctx.showCustomControls (cc::kFirstCompactUserLayer + set);
        viewOpened = true;
    };
    viewKey.isActive = [this]
    {
        const auto screen = ctx.getScreen ? ctx.getScreen() : Screen::Home;
        viewOpened = viewOpened && screen == Screen::Setup;
        return viewOpened || screen == Screen::Faders;
    };
    addAndMakeVisible (viewKey);

    for (int i = 0; i < layout::compact::kNumKeys; ++i)
    {
        auto* k = keys.add (new SurfaceKey (juce::String (i + 1)));
        k->ledColour = Colours::keyLayer;
        k->onPress = [this, i] { pressKey (i, false); };
        k->onHold = [this, i] { pressKey (i, true); };
        k->ledState = [this, i] { return keyLit (i) ? SurfaceKey::Lit : SurfaceKey::Off; };
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
    selectedKey = -1;
    ctx.settings.setValue (uiprefs::compactKeyMode, compact::keyModeToken (m));
    refresh();
}

compact::MainFader CompactSection::getMainFader() const
{
    return compact::parseMainFader (ctx.settings.getValue (uiprefs::compactMainFader, "MAIN.1"));
}

compact::MainFader CompactSection::shownMainFader() const
{
    if (selectedKey >= 0 && ! ctx.settings.getBoolValue (uiprefs::compactKeepMainFader, false))
        if (const auto mf = keyMainFader (selectedKey))
            return *mf;
    return getMainFader();
}

void CompactSection::setStripWidth (int w)
{
    if (w != stripW)
    {
        stripW = w;
        resized();
    }
}

void CompactSection::pressKey (int key, bool held)
{
    const auto mf = keyMainFader (key);
    if (! mf)
    {
        // A DCA spill or a custom control key (held or not).
        if (const auto f = keyFunction (key); f.isAssigned())
        {
            cc::press (ctx, f);
            cc::release (ctx, f);
        }
        return;
    }

    // A bus, main or matrix: the key selects it for fader 13; pressed again it
    // lets go (and ends what the press started). BUS keys spill (held or not);
    // MAIN and MATRIX keys do what MAIN / MATRIX SPILL give press and hold.
    const bool deselect = ! held && key == selectedKey;
    selectedKey = deselect ? -1 : key;
    cc::Function f;
    if (keyMode() == compact::KeyMode::Bus)
        f = keyFunction (key);
    else
    {
        const auto actions = compact::mainMatrixActions (key, ctx.settings.getBoolValue (uiprefs::compactMainSpill, uiprefs::compactMainSpillDefault),
                                                         ctx.settings.getBoolValue (uiprefs::compactMatrixSpill, uiprefs::compactMatrixSpillDefault));
        f = actionFunction (key, held ? actions.hold : actions.press);
    }
    // A press turns its function on (off when deselecting); a hold toggles it.
    if (f.isAssigned() && (held || cc::isLit (ctx, f) == deselect))
    {
        cc::press (ctx, f);
        cc::release (ctx, f);
    }
    refresh();
}

cc::Function CompactSection::actionFunction (int key, compact::KeyAction action) const
{
    const auto mf = keyMainFader (key);
    if (! mf || action == compact::KeyAction::None)
        return {};
    if (action == compact::KeyAction::Sends)
    {
        if (const auto* s = mainFaderStrip (*mf))
            return { cc::BtnSendsOnFader, cc::encodeStrip (state::StripRef::of (*s)), 0 };
        return {};
    }
    // Sidecar spills mains, not matrices.
    return mf->group == compact::MainFader::Main ? cc::Function { cc::BtnFaderSpill, mainSpillTarget (mf->number), 0 } : cc::Function {};
}

std::optional<compact::KeyControl> CompactSection::keyControl (int key) const
{
    return compact::keyControl (keyMode(), key, ctx.settings.getBoolValue (uiprefs::compactDcaCustom, false));
}

cc::Function CompactSection::keyFunction (int key) const
{
    if (const auto kc = keyControl (key))
    {
        if (ctx.customControls == nullptr)
            return {};
        return ctx.customControls->get (cc::kFirstCompactUserLayer + kc->set, kc->control).upper;
    }
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
        case compact::KeyMode::Main:
        case compact::KeyMode::User:
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

bool CompactSection::keyLit (int key) const
{
    if (key == selectedKey && keyMainFader (key))
        return true;
    if (const auto f = keyFunction (key); f.isAssigned() && cc::isLit (ctx, f))
        return true;
    // A MAIN or MATRIX key lights while its sends or spill are on.
    if (keyMode() == compact::KeyMode::Main && keyMainFader (key))
        for (auto a : { compact::KeyAction::Spill, compact::KeyAction::Sends })
            if (const auto f = actionFunction (key, a); f.isAssigned() && cc::isLit (ctx, f))
                return true;
    return false;
}

juce::String CompactSection::keyName (int key) const
{
    if (const auto mf = keyMainFader (key))
        if (const auto* s = mainFaderStrip (*mf))
            return stripName (*s);
    if (const auto kc = keyControl (key))
    {
        if (ctx.customControls == nullptr)
            return {};
        // A control's name, else what its key does (WING manual 4.8).
        const auto& c = ctx.customControls->get (cc::kFirstCompactUserLayer + kc->set, kc->control);
        if (c.name.isNotEmpty())
            return c.name.removeCharacters ("|*");
        return c.upper.isAssigned() ? cc::describe (ctx, cc::Slot::Upper, c.upper) : juce::String();
    }
    const auto f = keyFunction (key);
    if (f.isAssigned() && keyMode() == compact::KeyMode::Dca)
        return ctx.engine.getDcaName (key);
    return {};
}

juce::Colour CompactSection::keyColour (int key) const
{
    // Each custom control has its own colour on the display (WING manual 4.8).
    if (const auto kc = keyControl (key); kc && ctx.customControls != nullptr)
        if (const int colour = ctx.customControls->get (cc::kFirstCompactUserLayer + kc->set, kc->control).colour; colour > 0)
            return wingColour (colour);
    return juce::Colours::transparentBlack;
}

void CompactSection::refresh()
{
    // Fader 13: the selected key's, else its own assignment, if the console has it.
    const auto mf = shownMainFader();
    std::optional<state::FaderRef> target;
    if (mf.group == compact::MainFader::Dca)
        target = state::FaderRef::ofDca (mf.number - 1);
    else if (auto* s = mainFaderStrip (mf))
        target = state::FaderRef::of (state::StripRef::of (*s));
    fader13.setTarget (target);
    fader13.refresh();
    shownFader = mf;

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
    juce::String s = compact::mainFaderToken (shownMainFader()) + "|" + compact::keyModeToken (keyMode());
    for (int i = 0; i < keys.size(); ++i)
        s << "|" << keyName (i) << (keys[i]->isShownLit() ? "*" : "") << keyColour (i).toString();
    return s;
}

void CompactSection::timerCallback()
{
    // The LCD follows names, the keys' states and the custom controls; fader
    // 13 follows its assignment (WING remote clients set /$ctl/cfg/mfdr too).
    if (const auto mf = shownMainFader(); ! (mf == shownFader))
        refresh();
    if (const auto state = lcdState(); state != shownLcd)
    {
        refresh();
        shownLcd = state;
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
    const auto mf = shownMainFader();
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
        if (assigned)
        {
            g.setColour (lit ? lcdBlue : lcdDim);
            g.fillRect (cell);
        }
        // A custom control's colour: a bar at the cell's left.
        if (const auto colour = keyColour (i); ! colour.isTransparent())
        {
            g.setColour (colour);
            g.fillRect (cell.removeFromLeft (juce::jmax (3.0f, cellW * 0.06f)));
        }
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
