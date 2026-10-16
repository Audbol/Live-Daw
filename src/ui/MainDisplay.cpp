#include "ui/MainDisplay.h"

namespace livedaw::ui
{

namespace
{
    constexpr Screen screens[] = { Screen::Home, Screen::Effects, Screen::Meters, Screen::Routing,
                                   Screen::Setup, Screen::Library, Screen::Utility };

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

    for (auto s : screens)
    {
        auto* p = page (s);
        p->knobsChanged = [this, s] { if (screen == s) updateKnobs(); };
        addChildComponent (p);

        auto* b = screenButtons.add (new juce::TextButton (screenName (s)));
        b->setColour (juce::TextButton::buttonOnColourId, Colours::accent);
        b->setColour (juce::TextButton::buttonColourId, Colours::panelLight.darker (0.3f));
        b->onClick = [this, s] { showScreen (s); };
        addAndMakeVisible (b);
    }

    for (auto& k : knobs)
        addAndMakeVisible (k);

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
    }
    return home.get();
}

void MainDisplay::showScreen (Screen s)
{
    screen = s;
    for (size_t i = 0; i < std::size (screens); ++i)
    {
        page (screens[i])->setVisible (screens[i] == s);
        screenButtons[(int) i]->setToggleState (screens[i] == s, juce::dontSendNotification);
    }
    page (s)->refresh();
    updateKnobs();
    repaint();
}

void MainDisplay::showHomeTab (HomeTab t)
{
    if (screen != Screen::Home)
    {
        screen = Screen::Home;
        for (size_t i = 0; i < std::size (screens); ++i)
        {
            page (screens[i])->setVisible (screens[i] == Screen::Home);
            screenButtons[(int) i]->setToggleState (screens[i] == Screen::Home, juce::dontSendNotification);
        }
    }
    home->showTab (t);
    updateKnobs();
}

void MainDisplay::refresh()
{
    page (screen)->refresh();
    // Pages that cache strips refresh when shown; HOME follows the selection always.
    if (screen != Screen::Home)
        home->refresh();
    updateKnobs();
}

void MainDisplay::refreshLibrary()
{
    library->refresh();
}

void MainDisplay::updateKnobs()
{
    auto bindings = page (screen)->getKnobs();
    for (size_t i = 0; i < 6; ++i)
        knobs[i].setBinding (i < bindings.size() ? bindings[i] : KnobBinding {});
}

void MainDisplay::timerCallback()
{
    juce::String left, right;
    juce::Colour colour = Colours::outline;

    if (auto* s = ctx.getSelectedStrip ? ctx.getSelectedStrip() : nullptr)
    {
        left = describeStrip (*s);
        colour = s->getColour();
    }

    const int scene = ctx.show.getCurrentSceneIndex();
    if (const auto sc = ctx.show.getScene (scene))
        right << "SCENE " << juce::String (scene + 1).paddedLeft ('0', 2) << " " << sc->name << "    ";
    right << "DSP " << juce::roundToInt (ctx.engine.getDspLoad() * 100.0f) << "%    " << juce::Time::getCurrentTime().formatted ("%H:%M");

    const int faults = ctx.engine.getNumFaultedFx();
    const bool noDevice = ctx.deviceManager.getCurrentAudioDevice() == nullptr;
    const bool newAlert = faults > 0 || noDevice;
    if (noDevice)
        right = "NO AUDIO DEVICE    " + right;
    else if (faults > 0)
        right = juce::String (faults) + " FX FAULT" + (faults > 1 ? "S" : "") + "    " + right;

    if (left != statusLeft || right != statusRight || colour != statusColour || newAlert != alert)
    {
        statusLeft = left;
        statusRight = right;
        statusColour = colour;
        alert = newAlert;
        repaint (statusArea);
    }
}

void MainDisplay::paint (juce::Graphics& g)
{
    g.fillAll (Colours::panel);

    // Bezel around the screen.
    auto screenArea = getLocalBounds().withTrimmedLeft (84).withTrimmedBottom (knobArea.getHeight()).reduced (2);
    g.setColour (juce::Colours::black);
    g.fillRoundedRectangle (screenArea.toFloat(), 4.0f);

    g.setColour (Colours::screenPanel);
    g.fillRect (statusArea);
    auto s = statusArea.reduced (8, 0);
    g.setColour (statusColour);
    g.fillRect (s.removeFromLeft (4).reduced (0, 5));
    s.removeFromLeft (6);
    g.setColour (Colours::text);
    g.setFont (juce::FontOptions (13.0f, juce::Font::bold));
    g.drawText (statusLeft.isNotEmpty() ? statusLeft : juce::String ("no strip selected"), s, juce::Justification::centredLeft);
    g.setColour (Colours::accent);
    g.drawText (screenName (screen), s, juce::Justification::centred);
    g.setColour (alert ? Colours::mute : Colours::textDim);
    g.setFont (juce::FontOptions (12.0f, juce::Font::bold));
    g.drawText (statusRight, s, juce::Justification::centredRight);

    g.setColour (Colours::panel.darker (0.2f));
    g.fillRect (knobArea);
}

void MainDisplay::resized()
{
    auto b = getLocalBounds();
    knobArea = b.removeFromBottom (juce::jlimit (70, 96, b.getHeight() / 6));
    auto nav = b.removeFromLeft (84).reduced (4, 6);
    const int h = juce::jmin (44, nav.getHeight() / (int) std::size (screens));
    for (auto* button : screenButtons)
        button->setBounds (nav.removeFromTop (h).reduced (0, 3));

    auto screenArea = b.reduced (4);
    statusArea = screenArea.removeFromTop (24);
    for (auto s : screens)
        page (s)->setBounds (screenArea);

    auto k = knobArea.withTrimmedLeft (84).reduced (8, 4);
    const int w = k.getWidth() / 6;
    for (auto& knob : knobs)
        knob.setBounds (k.removeFromLeft (w).reduced (6, 0));
}

} // namespace livedaw::ui
