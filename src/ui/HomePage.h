#pragma once

#include "ui/DisplayPage.h"
#include "ui/HomeScreens.h"

#include <memory>

namespace livedaw::ui
{

/** The HOME screen of the selected strip, as on the WING: a column of
    processing-block thumbnails in signal order (each with a key that switches
    the block, the wrench sets the process order), the OVERVIEW / ICON/COLOR /
    NAME / TAGS tabs with the SAFE key, and the block screens (INPUT, FILTER,
    GATE, EQ, DYNAMICS, PRE/POST INSERT, MAIN, BUS SENDS). */
class HomePage final : public DisplayPage
{
public:
    explicit HomePage (UiContext&);
    ~HomePage() override;

    void showTab (HomeTab);
    HomeTab getTab() const noexcept { return current; }

    std::vector<KnobBinding> getKnobs() override;
    void refresh() override;

    void paint (juce::Graphics&) override;
    void resized() override;

    class BlockColumn;

private:
    void rebuild();
    bool isAvailable (HomeTab, const Strip*) const;
    static bool isOverviewGroup (HomeTab t) { return t == HomeTab::Home || t == HomeTab::Icon || t == HomeTab::Name || t == HomeTab::Tags; }

    UiContext& ctx;
    HomeTab current = HomeTab::Home;
    Strip* strip = nullptr;
    std::unique_ptr<BlockColumn> column;
    juce::OwnedArray<juce::TextButton> topTabs;
    std::unique_ptr<juce::TextButton> safeKey;
    std::unique_ptr<HomeScreen> content;
    juce::Rectangle<int> titleArea;
};

} // namespace livedaw::ui
