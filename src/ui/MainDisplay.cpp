#include "ui/MainDisplay.h"
#include "ui/Icons.h"

namespace livedaw::ui
{

namespace
{
    // The keys left of the display.
    constexpr Screen screens[] = { Screen::Home, Screen::Effects, Screen::Meters, Screen::Routing,
                                   Screen::Setup, Screen::Library, Screen::Utility };
    // Every page, including the VIEW screens.
    constexpr Screen allScreens[] = { Screen::Home,    Screen::Effects,  Screen::Meters, Screen::Routing,
                                      Screen::Setup,   Screen::Library,  Screen::Utility, Screen::Overview,
                                      Screen::Faders,  Screen::Sends,    Screen::Config,  Screen::MuteGroups };

    const char* screenName (Screen s)
    {
        switch (s)
        {
            case Screen::Home:    return "HOME";
            case Screen::Effects: return "EFFECTS";
            case Screen::Meters:  return "METERS";
            case Screen::Routing: return "ROUTING";
            case Screen::Setup:   return "SETUP";
            case Screen::Library: return "LIBRARY";
            case Screen::Utility: return "UTILITY";
            case Screen::Overview:
            case Screen::Faders:
            case Screen::Sends:
            case Screen::Config:
            case Screen::MuteGroups:
                return viewScreenName (s);
        }
        return "";
    }
} // namespace

MainDisplay::MainDisplay (UiContext& c) : ctx (c)
{
    home = std::make_unique<HomePage> (ctx);
    effects = std::make_unique<EffectsPage> (ctx);
    meters = std::make_unique<MetersPage> (ctx);
    routing = std::make_unique<RoutingPage> (ctx);
    setup = std::make_unique<SetupPage> (ctx);
    library = std::make_unique<LibraryPage> (ctx);
    utility = std::make_unique<UtilityPage> (ctx);
    overview = std::make_unique<OverviewViewPage> (ctx);
    faders = std::make_unique<FadersViewPage> (ctx);
    sends = std::make_unique<SendsViewPage> (ctx);
    config = std::make_unique<ConfigViewPage> (ctx);
    muteGroups = std::make_unique<MuteGroupsViewPage> (ctx);

    for (auto s : allScreens)
    {
        auto* p = page (s);
        p->knobsChanged = [this, s] { if (screen == s) updateKnobs(); };
        p->statusKeysChanged = [this, s] { if (screen == s) updateFrame(); };
        addChildComponent (p);
    }

    for (auto s : screens)
    {
        // The keys left of the display: grey hardware keys that light up.
        auto* b = screenButtons.add (new juce::TextButton (screenName (s)));
        b->setColour (juce::TextButton::buttonOnColourId, Colours::text);
        b->setColour (juce::TextButton::textColourOnId, juce::Colours::black);
        b->setColour (juce::TextButton::buttonColourId, juce::Colour (0xff2c2e32));
        b->onClick = [this, s] { showScreen (s); };
        addAndMakeVisible (b);
    }

    for (auto& slot : slots)
        addAndMakeVisible (slot);

    showScreen (Screen::Home);
    startTimerHz (4);
}

MainDisplay::~MainDisplay() = default;

DisplayPage* MainDisplay::page (Screen s) const
{
    switch (s)
    {
        case Screen::Home:    return home.get();
        case Screen::Effects: return effects.get();
        case Screen::Meters:  return meters.get();
        case Screen::Routing: return routing.get();
        case Screen::Setup:   return setup.get();
        case Screen::Library: return library.get();
        case Screen::Utility: return utility.get();
        case Screen::Overview: return overview.get();
        case Screen::Faders: return faders.get();
        case Screen::Sends: return sends.get();
        case Screen::Config: return config.get();
        case Screen::MuteGroups: return muteGroups.get();
    }
    return home.get();
}

void MainDisplay::showScreen (Screen s)
{
    screen = s;
    for (auto sc : allScreens)
        page (sc)->setVisible (sc == s);
    for (size_t i = 0; i < std::size (screens); ++i)
        screenButtons[(int) i]->setToggleState (screens[i] == s, juce::dontSendNotification);
    page (s)->refresh();
    updateFrame();
    updateKnobs();
    repaint();
}

void MainDisplay::showHomeTab (HomeTab t)
{
    if (screen != Screen::Home)
        showScreen (Screen::Home);
    home->showTab (t);
    updateKnobs();
    repaint();
}

void MainDisplay::refresh()
{
    page (screen)->refresh();
    if (screen != Screen::Home)
        home->refresh();
    updateKnobs();
    repaint (statusArea);
}

void MainDisplay::refreshLibrary()
{
    library->refresh();
    repaint (statusArea);
}

void MainDisplay::updateFrame()
{
    statusKeys = page (screen)->getStatusKeys();
    if (statusKeys.empty())
    {
        statusKeys.push_back ({ "TOOLS", Icon::Tools, screen == Screen::Utility, [this] { showScreen (Screen::Utility); } });
        statusKeys.push_back ({ "SETTINGS", Icon::Gear, screen == Screen::Setup, [this] { showScreen (Screen::Setup); } });
    }
    resized();
    repaint();
}

void MainDisplay::updateKnobs()
{
    auto bindings = page (screen)->getKnobs();
    for (size_t i = 0; i < 6; ++i)
        slots[i].setBinding (i < bindings.size() ? bindings[i] : KnobBinding {});
}

void MainDisplay::timerCallback()
{
    const auto time = juce::Time::getCurrentTime().formatted ("%H:%M:%S");
    // Repaint the status bar when anything it shows changes.
    // Unsigned, so the mixing may wrap around.
    juce::uint32 hash = 0;
    if (auto* s = ctx.getSelectedStrip ? ctx.getSelectedStrip() : nullptr)
        hash = (juce::uint32) juce::DefaultHashFunctions::generateHash (describeStrip (*s) + juce::String (s->getColourIndex()), 1 << 30);
    hash = hash * 31u + (juce::uint32) ctx.show.getCurrentSceneIndex();
    hash = hash * 31u + (juce::uint32) juce::roundToInt (ctx.engine.getDspLoad() * 100.0f);
    hash = hash * 31u + (juce::uint32) ctx.engine.getNumFaultedFx();
    hash = hash * 31u + (ctx.deviceManager.getCurrentAudioDevice() != nullptr ? 1u : 0u);
    if (time != shownTime || hash != shownHash)
    {
        shownTime = time;
        shownHash = hash;
        repaint (statusArea);
    }
}

void MainDisplay::paintStatusBar (juce::Graphics& g)
{
    g.setColour (Colours::screen);
    g.fillRect (statusArea);

    // Selected channel tab: amber, with the number in a black box.
    auto* strip = ctx.getSelectedStrip ? ctx.getSelectedStrip() : nullptr;
    auto tab = channelTab.toFloat();
    g.setColour (Colours::amber);
    g.fillRoundedRectangle (tab, 3.0f);
    auto t = tab.reduced (5.0f, 5.0f);
    const float h = t.getHeight();
    auto numberBox = t.removeFromLeft (h * 1.05f);
    g.setColour (juce::Colours::black);
    g.fillRect (numberBox);
    if (strip != nullptr)
    {
        g.setColour (Colours::amber);
        g.setFont (juce::FontOptions (h * (strip->getNumber() >= 100 ? 0.62f : 0.8f), juce::Font::bold));
        const auto prefix = strip->isInput() ? juce::String() : juce::String (strip->getLabel().substring (0, 1));
        g.drawFittedText (prefix + juce::String (strip->getNumber()), numberBox.toNearestInt(), juce::Justification::centred, 1);
        t.removeFromLeft (h * 0.25f);
        drawIcon (g, stripIcon (strip->getKind()), t.removeFromLeft (h).reduced (h * 0.08f), juce::Colours::black);
        t.removeFromLeft (h * 0.2f);
        g.setColour (juce::Colours::black);
        g.setFont (juce::FontOptions (h * 0.92f, juce::Font::bold));
        g.drawFittedText (stripName (*strip), t.toNearestInt(), juce::Justification::centredLeft, 1);
    }

    // Status chips: sample rate, clock source, DSP load, protection.
    auto tile = [&g] (juce::Rectangle<int> r)
    {
        g.setColour (Colours::screenHeader);
        g.fillRoundedRectangle (r.toFloat(), 3.0f);
    };
    tile (chipsTile);
    auto chips = chipsTile.reduced (8, 7).toFloat();
    const float chipH = (chips.getHeight() - 4.0f) / 2.0f;
    auto row1 = chips.removeFromTop (chipH);
    chips.removeFromTop (4.0f);
    auto row2 = chips;
    auto* device = ctx.deviceManager.getCurrentAudioDevice();
    const auto rate = device != nullptr ? juce::String (juce::roundToInt (device->getCurrentSampleRate() / 1000.0)) + "K" : juce::String ("--");
    const float cw = row1.getWidth() / 2.0f - 2.0f;
    drawChip (g, row1.removeFromLeft (cw), rate, device != nullptr ? Colours::green : Colours::mute, juce::Colours::black);
    row1.removeFromLeft (4.0f);
    drawChip (g, row1, device != nullptr ? "INT" : "NO DEV", device != nullptr ? Colours::green : Colours::mute, juce::Colours::black);
    const int faults = ctx.engine.getNumFaultedFx();
    drawChip (g, row2.removeFromLeft (cw), "DSP " + juce::String (juce::roundToInt (ctx.engine.getDspLoad() * 100.0f)) + "%",
              juce::Colours::white.withAlpha (0.85f), juce::Colours::black);
    row2.removeFromLeft (4.0f);
    drawChip (g, row2, faults > 0 ? "FX FLT" : "PROT", faults > 0 ? Colours::mute : (ctx.engine.outputProtection.load() ? Colours::green : Colours::screenTile),
              juce::Colours::black);

    // Current scene.
    tile (sceneTile);
    auto sc = sceneTile.reduced (10, 6);
    g.setColour (Colours::textDim);
    g.setFont (juce::FontOptions (13.0f, juce::Font::bold));
    g.drawText ("SCENE", sc.removeFromTop (15), juce::Justification::centredLeft);
    const int index = ctx.show.getCurrentSceneIndex();
    const auto scene = ctx.show.getScene (index);
    g.setColour (scene ? Colours::text : Colours::textDim);
    g.setFont (juce::FontOptions (juce::jmin (22.0f, (float) sc.getHeight()), juce::Font::bold));
    g.drawFittedText (scene ? juce::String (index + 1).paddedLeft ('0', 2) + "  " + scene->name : juce::String ("NONE"), sc,
                      juce::Justification::centredLeft, 1);

    // The screen's keys.
    for (size_t i = 0; i < statusKeys.size() && i < statusKeyAreas.size(); ++i)
    {
        const auto& key = statusKeys[i];
        const auto area = statusKeyAreas[i];
        g.setColour (key.lit ? Colours::text : Colours::screenHeader);
        g.fillRoundedRectangle (area.toFloat(), 3.0f);
        const auto ink = key.lit ? juce::Colours::black : Colours::text;
        auto k = area.toFloat().reduced (3.0f);
        g.setColour (ink);
        g.setFont (juce::FontOptions (13.0f, juce::Font::bold));
        if (key.icon)
        {
            auto label = k.removeFromBottom (14.0f);
            drawIcon (g, *key.icon, k, ink);
            g.drawFittedText (key.text, label.toNearestInt(), juce::Justification::centred, 1);
        }
        else
        {
            g.drawFittedText (key.text, k.toNearestInt(), juce::Justification::centred, 2);
        }
    }

    // Clock.
    tile (clockTile);
    auto c = clockTile.reduced (8, 4);
    const auto now = juce::Time::getCurrentTime();
    g.setColour (Colours::text);
    g.setFont (juce::FontOptions ((float) c.getHeight() * 0.56f, juce::Font::bold));
    g.drawFittedText (now.formatted ("%H:%M:%S"), c.removeFromTop (c.getHeight() * 6 / 10), juce::Justification::centred, 1);
    g.setColour (Colours::textDim);
    g.setFont (juce::FontOptions ((float) c.getHeight() * 0.9f, juce::Font::bold));
    g.drawFittedText (now.formatted ("%Y-%m-%d"), c, juce::Justification::centred, 1);
}

void MainDisplay::paint (juce::Graphics& g)
{
    g.fillAll (Colours::panel);

    // The screen, framed by a bezel.
    g.setColour (juce::Colours::black);
    g.fillRect (screenArea.expanded (3));
    g.setColour (Colours::outline);
    g.drawRect (screenArea.expanded (4), 1);
    g.setColour (Colours::screen);
    g.fillRect (screenArea);

    paintStatusBar (g);
}

void MainDisplay::editFx (int slot)
{
    showScreen (Screen::Effects);
    effects->editSlot (slot);
}

void MainDisplay::mouseUp (const juce::MouseEvent& e)
{
    const auto p = e.getPosition();
    for (size_t i = 0; i < statusKeys.size() && i < statusKeyAreas.size(); ++i)
        if (statusKeyAreas[i].contains (p))
        {
            if (auto f = statusKeys[i].onClick)
                f(); // a copy: the click may change the keys
            return;
        }
    if (sceneTile.contains (p))
        showScreen (Screen::Library);
    else if (channelTab.contains (p))
        showHomeTab (HomeTab::Home);
}

void MainDisplay::resized()
{
    auto b = getLocalBounds();
    auto nav = b.removeFromLeft (92).reduced (8, 12);
    const int h = juce::jmin (52, nav.getHeight() / (int) std::size (screens));
    for (auto* button : screenButtons)
        button->setBounds (nav.removeFromTop (h).reduced (0, 4));

    screenArea = b.reduced (8, 8);
    auto s = screenArea;
    const int statusH = juce::jlimit (44, 64, s.getHeight() / 12);
    statusArea = s.removeFromTop (statusH);
    const bool knobs = page (screen)->usesKnobs();
    slotArea = knobs ? s.removeFromBottom (juce::jlimit (36, 48, s.getHeight() / 13)) : juce::Rectangle<int>();
    s.removeFromTop (4);
    if (! knobs)
        s.removeFromBottom (4);

    for (auto sc : allScreens)
        page (sc)->setBounds (s);

    layoutStatusBar();

    for (auto& slot : slots)
        slot.setVisible (knobs);
    auto k = slotArea.reduced (8, 0);
    const int w = k.getWidth() / 6;
    for (auto& slot : slots)
        slot.setBounds (k.removeFromLeft (w).reduced (8, 0));
}

void MainDisplay::layoutStatusBar()
{
    auto st = statusArea.reduced (4, 4);
    clockTile = st.removeFromRight (juce::jmin (150, st.getWidth() / 7));
    st.removeFromRight (6);
    statusKeyAreas.clear();
    const int keyW = juce::jmin (78, st.getWidth() / 10);
    for (size_t i = 0; i < statusKeys.size(); ++i)
    {
        statusKeyAreas.insert (statusKeyAreas.begin(), st.removeFromRight (keyW));
        st.removeFromRight (6);
    }
    channelTab = st.removeFromLeft (st.getWidth() * 45 / 100);
    st.removeFromLeft (6);
    chipsTile = st.removeFromLeft (juce::jmin (120, st.getWidth() / 2));
    st.removeFromLeft (6);
    sceneTile = st;
}

} // namespace livedaw::ui
