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
    modeKeys[0]->setTooltip ("BUS: the 16 keys are BUS 1-16 (press: the bus's sends on the faders; hold: spill the bus)");
    modeKeys[1]->setTooltip ("DCA: the 16 keys are DCA 1-16 (press: spill the DCA onto the faders)");
    modeKeys[2]->setTooltip ("MAIN MATRIX: keys 1-4 are MAIN 1-4, 5-12 MATRIX 1-8 (press: their sends on the faders)");
    modeKeys[3]->setTooltip ("USER: the 16 keys are custom control keys (1-8 the USER layer, 9-16 the custom control layer shown)");

    // VIEW: CUSTOM CONTROLS (CC EDIT) on the layer the USER keys show.
    viewKey.setTooltip ("VIEW: CUSTOM CONTROLS (CC EDIT)");
    viewKey.onPress = [this]
    {
        if (ctx.showCustomControls)
            ctx.showCustomControls (keyMode() == compact::KeyMode::User || ctx.customControls == nullptr ? cc::kUserLayer : ctx.customControls->currentLayer);
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
            if (const auto f = keyFunctions (i).first; f.isAssigned())
            {
                cc::press (ctx, f);
                cc::release (ctx, f);
            }
        };
        k->onHold = [this, i]
        {
            if (const auto f = keyFunctions (i).second; f.isAssigned())
                cc::press (ctx, f);
        };
        k->ledState = [this, i]
        {
            const auto [push, hold] = keyFunctions (i);
            return (push.isAssigned() && cc::isLit (ctx, push)) || (hold.isAssigned() && cc::isLit (ctx, hold)) ? SurfaceKey::Lit : SurfaceKey::Off;
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

std::pair<cc::Function, cc::Function> CompactSection::keyFunctions (int key) const
{
    auto stripAt = [this] (StripKind kind, int index) -> Strip*
    {
        const auto& list = ctx.engine.getStrips (kind);
        return juce::isPositiveAndBelow (index, (int) list.size()) ? list[(size_t) index].get() : nullptr;
    };
    auto sof = [] (Strip* s) { return s != nullptr ? cc::Function { cc::BtnSendsOnFader, cc::encodeStrip (state::StripRef::of (*s)), 0 } : cc::Function {}; };

    switch (keyMode())
    {
        case compact::KeyMode::Bus:
            if (auto* bus = stripAt (StripKind::Bus, key))
                return { sof (bus), cc::Function { cc::BtnFaderSpill, 100 + state::StripRef::of (*bus).id, 0 } };
            break;
        case compact::KeyMode::Main:
            return { sof (key < kNumMains ? stripAt (StripKind::Main, key) : stripAt (StripKind::Matrix, key - kNumMains)), {} };
        case compact::KeyMode::Dca:
            if (key < kNumDcas)
                return { cc::Function { cc::BtnFaderSpill, key, 0 }, {} };
            break;
        case compact::KeyMode::User:
        {
            if (ctx.customControls == nullptr)
                break;
            const int layer = key < 8 ? cc::kUserLayer : ctx.customControls->currentLayer;
            const auto& control = ctx.customControls->get (layer, (key % 8) / 2);
            return { key % 2 == 0 ? control.upper : control.lower, {} };
        }
    }
    return {};
}

juce::String CompactSection::keyName (int key) const
{
    const auto f = keyFunctions (key).first;
    if (! f.isAssigned())
        return {};
    switch (keyMode())
    {
        case compact::KeyMode::Bus:
        case compact::KeyMode::Main:
            if (const auto* s = state::findStrip (ctx.engine, cc::decodeStrip (f.target)))
                return stripName (*s);
            break;
        case compact::KeyMode::Dca:  return ctx.engine.getDcaName (key);
        case compact::KeyMode::User: return cc::describe (ctx, cc::Slot::Upper, f);
    }
    return {};
}

void CompactSection::refresh()
{
    // Fader 13: its assignment, if the console has it.
    const auto mf = getMainFader();
    std::optional<state::FaderRef> target;
    auto strip = [this] (StripKind kind, int number) -> std::optional<state::FaderRef>
    {
        const auto& list = ctx.engine.getStrips (kind);
        if (juce::isPositiveAndBelow (number - 1, (int) list.size()))
            return state::FaderRef::of (state::StripRef::of (*list[(size_t) number - 1]));
        return std::nullopt;
    };
    switch (mf.group)
    {
        case compact::MainFader::Off:     break;
        case compact::MainFader::Main:    target = strip (StripKind::Main, mf.number); break;
        case compact::MainFader::Matrix:  target = strip (StripKind::Matrix, mf.number); break;
        case compact::MainFader::Channel: target = strip (StripKind::Channel, mf.number); break;
        case compact::MainFader::Aux:     target = strip (StripKind::Aux, mf.number); break;
        case compact::MainFader::Bus:     target = strip (StripKind::Bus, mf.number); break;
        case compact::MainFader::Dca:     target = state::FaderRef::ofDca (mf.number - 1); break;
    }
    fader13.setTarget (target);
    fader13.refresh();

    for (int i = 0; i < keys.size(); ++i)
    {
        const auto [push, hold] = keyFunctions (i);
        keys[i]->setEnabled (push.isAssigned() || hold.isAssigned());
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
    // The LCD follows names, the keys' states and the custom controls.
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
    g.drawFittedText (compact::mainFaderName (getMainFader()), title.reduced (4.0f, 0.0f).toNearestInt(), juce::Justification::centredLeft, 1, 0.7f);
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

void CompactSection::mouseUp (const juce::MouseEvent& e)
{
    // Touching the LCD's title: what fader 13 controls.
    if (lcd.withHeight (juce::jmax (16, lcd.getHeight() / 4)).contains (e.getPosition()))
        showMainFaderMenu();
}

void CompactSection::showMainFaderMenu()
{
    using MF = compact::MainFader;
    const auto current = getMainFader();
    juce::PopupMenu menu;
    menu.addSectionHeader ("FADER 13");
    auto add = [&] (juce::PopupMenu& m, MF f) { m.addItem (mainFaderName (f), true, f == current, [this, f]
                                                          {
                                                              ctx.settings.setValue (uiprefs::compactMainFader, compact::mainFaderToken (f));
                                                              refresh();
                                                          }); };
    add (menu, { MF::Off, 1 });
    for (int g = MF::Main; g <= MF::Bus; ++g)
    {
        const auto group = static_cast<MF::Group> (g);
        juce::PopupMenu sub;
        const int n = group == MF::Channel ? juce::jmin (compact::groupSize (group), (int) ctx.engine.getChannels().size()) : compact::groupSize (group);
        for (int i = 1; i <= n; ++i)
            add (sub, { group, i });
        menu.addSubMenu (compact::groupToken (group), sub, n > 0, juce::Image(), current.group == group);
    }
    menu.showMenuAsync (juce::PopupMenu::Options().withTargetScreenArea (localAreaToGlobal (lcd)));
}

} // namespace livedaw::ui
