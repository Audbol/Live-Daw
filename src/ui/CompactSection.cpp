#include "ui/CompactSection.h"
#include "ui/MidiRemoteUi.h"

#include <utility>

namespace livedaw::ui
{

namespace
{
    const char* const modeLabels[] = { "BUS", "DCA", "MAIN\nMATRIX", "USER" };
    constexpr compact::KeyMode modes[] = { compact::KeyMode::Bus, compact::KeyMode::Dca, compact::KeyMode::Main, compact::KeyMode::User };

    // The LCD's colours [photos of a WING COMPACT]: a colour screen, white on blue.
    const juce::Colour lcdBlue { 0xff1d7fd8 }, lcdDim { 0xff0b2c4c };

    /** The FADER SPILL target of MAIN `n` and MATRIX `n` (1-based; custom.rs: kinds 5, 6). */
    constexpr int mainSpillTarget (int n) { return 500 + n - 1; }
    constexpr int matrixSpillTarget (int n) { return 600 + n - 1; }
} // namespace

CompactSection::CompactSection (UiContext& c) : ctx (c), fader13 (c, LayerSet::Center)
{
    // Settings from before the COMPACT had one DCA spill setting: Sidecar's
    // DCA SELECT SPILL, on, carries over to DCA SPILL ON SELECT (the same thing).
    if (ctx.settings.containsKey (uiprefs::legacyCompactDcaSelectSpill))
    {
        if (ctx.settings.getBoolValue (uiprefs::legacyCompactDcaSelectSpill, false))
            ctx.settings.setValue (uiprefs::dcaSpill, true);
        ctx.settings.removeValue (uiprefs::legacyCompactDcaSelectSpill);
    }

    for (int i = 0; i < 4; ++i)
    {
        auto& k = modeKeys[i];
        k = std::make_unique<SurfaceKey> (modeLabels[i]);
        k->ledColour = Colours::keyLayer;
        // In DAW mode the four keys play the set DAW MODE KEYS (empty at first).
        k->onDownUp = [this, i] (bool down) { dawDownUp (compact::kNumKeys + i, down, true); };
        k->onPress = [this, i]
        {
            if (! dawTook[(size_t) (compact::kNumKeys + i)])
                setKeyMode (modes[i]);
        };
        k->ledState = [this, i]
        {
            if (dawKeys())
            {
                const auto f = modeKeyFunction (i);
                return f.isAssigned() && cc::isLit (ctx, f) ? SurfaceKey::Lit : SurfaceKey::Off;
            }
            return keyMode() == modes[i] ? SurfaceKey::Lit : SurfaceKey::Off;
        };
        addAndMakeVisible (*k);
    }
    modeKeys[0]->setTooltip ("BUS: the 16 keys are BUS 1-16 (press: sends on fader, hold: spill; BUS SPILL swaps them); fader 13 follows");
    modeKeys[1]->setTooltip ("DCA: the 16 keys are DCA 1-16 (press: spills the DCA and puts it on fader 13), "
                             "or the custom control sets D1-D4 (USE DCA BUTTONS 1-16 AS CC)");
    modeKeys[2]->setTooltip ("MAIN MATRIX: keys 1-4 MAIN 1-4, 5-8 the custom control set M/M, 9-16 MATRIX 1-8 (press and hold: "
                             "sends on fader or spill, as MAIN SPILL and MATRIX SPILL say); fader 13 follows");
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
            ctx.showCustomControls (cc::kFirstCompactUserLayer + (dawKeys() ? compact::kFirstDawSet : set));
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
        k->onPress = [this, i]
        {
            if (! dawTook[(size_t) i])
                pressKey (i, false);
        };
        k->onHold = [this, i]
        {
            if (! dawTook[(size_t) i])
                pressKey (i, true);
        };
        // DAW mode: the key's DAW control acts while held (DAW BUTTONS push).
        k->onDownUp = [this, i] (bool down) { dawDownUp (i, down, false); };
        k->ledState = [this, i] { return keyLit (i) ? SurfaceKey::Lit : SurfaceKey::Off; };
        addAndMakeVisible (k);
    }

    // Fader 13 has no scribble strip: the LCD names what it controls. Its
    // SELECT held after a DCA key: the DCA assignment (manual 4.2 p. 16).
    fader13.setScribbleShown (false);
    fader13.setSelectHeld ([this] (bool down)
    {
        const bool dcaKeys = keyMode() == compact::KeyMode::Dca && ! ctx.settings.getBoolValue (uiprefs::compactDcaCustom, false);
        ctx.compactDcaAssign = down && dcaKeys && juce::isPositiveAndBelow (assignDca, kNumDcas) ? assignDca : -1;
    });
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
    assignDca = -1;
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

bool CompactSection::dawKeys() const
{
    return midiui::dawModeOn (ctx);
}

cc::Function CompactSection::setFunction (const compact::KeyControl& kc) const
{
    if (ctx.customControls == nullptr)
        return {};
    return ctx.customControls->get (cc::kFirstCompactUserLayer + kc.set, kc.control).upper;
}

cc::Function CompactSection::modeKeyFunction (int modeKey) const
{
    const auto kc = compact::modeKeyControl (modeKey);
    return dawKeys() && kc ? setFunction (*kc) : cc::Function {};
}

bool CompactSection::dawDownUp (int index, bool down, bool modeKey)
{
    auto& held = dawHeld[(size_t) index];
    if (! down)
    {
        // Released whatever mode the press left (DAW LAYER ends DAW mode on the way down).
        const auto f = std::exchange (held, cc::Function {});
        if (f.isAssigned())
            cc::release (ctx, f);
        return dawTook[(size_t) index];
    }
    dawTook[(size_t) index] = dawKeys();
    if (! dawKeys())
        return false;
    const auto f = modeKey ? modeKeyFunction (index - compact::kNumKeys) : keyFunction (index);
    if (f.isAssigned())
    {
        // A key with a parameter gives it to the knob right of the display.
        juce::Component* key = modeKey ? static_cast<juce::Component*> (modeKeys[index - compact::kNumKeys].get()) : keys[index];
        if (auto b = cc::bindHeldKey (ctx, f); b.isValid() && ctx.setFineTarget)
            ctx.setFineTarget (b, key);
        held = f;
        cc::press (ctx, f);
    }
    return true;
}

void CompactSection::pressKey (int key, bool held)
{
    if (dawKeys())
        return; // dawDownUp plays the DAW sets
    const auto mf = keyMainFader (key);
    if (! mf)
    {
        // A custom control key (held or not). A key with a parameter gives
        // it to the knob right of the display.
        if (const auto f = keyFunction (key); f.isAssigned())
        {
            if (keyControl (key))
                if (auto b = cc::bindHeldKey (ctx, f); b.isValid() && ctx.setFineTarget)
                    ctx.setFineTarget (b, keys[key]);
            cc::press (ctx, f);
            cc::release (ctx, f);
        }
        return;
    }

    // A bus, main, matrix or DCA: the key selects it for fader 13; pressed
    // again it lets go (and ends what the press started). BUS keys send on a
    // press and spill on a hold, MAIN and MATRIX keys too, each as its BUS /
    // MAIN / MATRIX SPILL setting says. A DCA key spills its DCA as well,
    // held or not, and is the DCA fader 13's SELECT assigns to.
    const bool dca = keyMode() == compact::KeyMode::Dca;
    if (dca)
    {
        held = false; // a hold does what a press does
        assignDca = key;
    }
    const bool deselect = ! held && key == selectedKey;
    selectedKey = deselect ? -1 : key;
    const auto actions = dca ? compact::dcaActions()
                         : keyMode() == compact::KeyMode::Bus
                             ? compact::busActions (ctx.settings.getBoolValue (uiprefs::compactBusSpill, uiprefs::compactBusSpillDefault))
                             : compact::mainMatrixActions (key, ctx.settings.getBoolValue (uiprefs::compactMainSpill, uiprefs::compactMainSpillDefault),
                                                           ctx.settings.getBoolValue (uiprefs::compactMatrixSpill, uiprefs::compactMatrixSpillDefault));
    const auto f = actionFunction (key, held ? actions.hold : actions.press);
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
    switch (mf->group)
    {
        case compact::MainFader::Bus:    return keyFunction (key);
        case compact::MainFader::Main:   return { cc::BtnFaderSpill, mainSpillTarget (mf->number), 0 };
        case compact::MainFader::Matrix: return { cc::BtnFaderSpill, matrixSpillTarget (mf->number), 0 };
        case compact::MainFader::Dca:    return { cc::BtnFaderSpill, mf->number - 1, 0 }; // FADER SPILL kind 0: the DCA
        case compact::MainFader::Off:
        case compact::MainFader::Channel:
        case compact::MainFader::Aux:    break;
    }
    return {};
}

std::optional<compact::KeyControl> CompactSection::keyControl (int key) const
{
    return compact::keyControl (keyMode(), key, ctx.settings.getBoolValue (uiprefs::compactDcaCustom, false), dawKeys());
}

cc::Function CompactSection::keyFunction (int key) const
{
    if (const auto kc = keyControl (key))
    {
        auto f = setFunction (*kc);
        // DAW mode: an empty DAW key whose control in the mode's own set is
        // DAW LAYER still leaves DAW mode (the key that turned it on).
        if (dawKeys() && ! f.isAssigned())
            if (const auto own = compact::keyControl (keyMode(), key, ctx.settings.getBoolValue (uiprefs::compactDcaCustom, false)))
                if (const auto o = setFunction (*own); o.category == cc::BtnOther && o.option == cc::OtherDawLayer)
                    f = o;
        return f;
    }
    // A BUS key's spill (a DCA's is actionFunction's).
    if (keyMode() == compact::KeyMode::Bus)
    {
        const auto& buses = ctx.engine.getBuses();
        if (juce::isPositiveAndBelow (key, (int) buses.size()))
            return { cc::BtnFaderSpill, 100 + state::StripRef::of (*buses[(size_t) key]).id, 0 };
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
    if (dawKeys() || keyControl (key))
        return std::nullopt;
    const auto mf = compact::keyMainFader (keyMode(), key);
    if (mf && mf->group == compact::MainFader::Dca)
        return mf->number <= kNumDcas ? mf : std::nullopt;
    return mf && mainFaderStrip (*mf) != nullptr ? mf : std::nullopt;
}

bool CompactSection::keyLit (int key) const
{
    if (key == selectedKey && keyMainFader (key))
        return true;
    if (const auto f = keyFunction (key); f.isAssigned() && cc::isLit (ctx, f))
        return true;
    // A BUS, MAIN or MATRIX key lights while its sends or spill are on.
    if (keyMainFader (key))
        for (auto a : { compact::KeyAction::Spill, compact::KeyAction::Sends })
            if (const auto f = actionFunction (key, a); f.isAssigned() && cc::isLit (ctx, f))
                return true;
    return false;
}

juce::String CompactSection::keyName (int key) const
{
    if (const auto mf = keyMainFader (key))
    {
        if (mf->group == compact::MainFader::Dca)
            return ctx.engine.getDcaName (mf->number - 1);
        if (const auto* s = mainFaderStrip (*mf))
            return stripName (*s);
    }
    if (const auto kc = keyControl (key))
    {
        if (ctx.customControls == nullptr)
            return {};
        // A control's name, else what its key does (WING manual 4.8).
        const auto& c = ctx.customControls->get (cc::kFirstCompactUserLayer + kc->set, kc->control);
        if (c.name.isNotEmpty())
            return c.name.removeCharacters ("|*");
        if (c.upper.isAssigned())
            return cc::describe (ctx, cc::Slot::Upper, c.upper);
        // An empty DAW key that leaves DAW mode (keyFunction).
        if (const auto f = keyFunction (key); f.isAssigned())
            return cc::describe (ctx, cc::Slot::Upper, f);
    }
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
    juce::String s = compact::mainFaderToken (shownMainFader()) + "|" + compact::keyModeToken (keyMode()) + (dawKeys() ? "|DAW" : "");
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
    g.drawFittedText (dawKeys() ? juce::String ("DAW") : juce::String (modeNames[(int) keyMode()]), title.reduced (4.0f, 0.0f).toNearestInt(),
                      juce::Justification::centredRight, 1, 0.7f);

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
