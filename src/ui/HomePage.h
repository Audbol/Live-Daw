#pragma once

#include "ui/DisplayPage.h"

#include <memory>

namespace livedaw::ui
{

/** The HOME screen of the selected strip: an overview of its processing chain
    and, in the left-hand column, the INPUT, FILTER, GATE, EQ, DYNAMICS, PRE INS,
    POST INS, MAIN, BUS SENDS and NAME/COLOR screens. */
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

    class Tab;

private:
    void rebuild();
    bool isAvailable (HomeTab, const Strip*) const;

    UiContext& ctx;
    HomeTab current = HomeTab::Home;
    Strip* strip = nullptr;
    juce::OwnedArray<juce::TextButton> tabButtons;
    std::unique_ptr<Tab> content;
};

} // namespace livedaw::ui
