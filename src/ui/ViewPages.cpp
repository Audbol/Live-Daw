#include "ui/ViewPages.h"
#include "ui/LookAndFeel.h"

namespace livedaw::ui
{

namespace
{
    constexpr Screen viewScreens[] = { Screen::Overview, Screen::Faders, Screen::Sends, Screen::Config, Screen::MuteGroups };
} // namespace

const char* viewScreenName (Screen s)
{
    switch (s)
    {
        case Screen::Overview:   return "OVERVIEW";
        case Screen::Faders:     return "FADERS";
        case Screen::Sends:      return "SENDS";
        case Screen::Config:     return "CONFIG";
        case Screen::MuteGroups: return "MUTEGRP";
        case Screen::Home:
        case Screen::Effects:
        case Screen::Meters:
        case Screen::Routing:
        case Screen::Setup:
        case Screen::Library:
        case Screen::Utility:
            break;
    }
    return "";
}

ViewPage::ViewPage (UiContext& c, Screen s) : ctx (c), screen (s) {}

std::vector<StatusKey> ViewPage::getStatusKeys()
{
    // The VIEW screens switch between each other from the status bar.
    std::vector<StatusKey> keys;
    for (auto s : viewScreens)
        keys.push_back ({ viewScreenName (s), std::nullopt, s == screen, [this, s] { if (ctx.showScreen) ctx.showScreen (s); } });
    return keys;
}

void ViewPage::paint (juce::Graphics& g)
{
    g.setColour (Colours::textDim);
    g.setFont (juce::FontOptions (22.0f, juce::Font::bold));
    g.drawText (viewScreenName (screen), getLocalBounds(), juce::Justification::centred);
}

} // namespace livedaw::ui
