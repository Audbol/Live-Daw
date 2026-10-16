#pragma once

#include "ui/FaderStrip.h"

namespace livedaw::ui
{

/** One of the WING's three fader sections: a column of bank buttons (default
    banks + user banks), paging arrows for banks longer than the section, and
    the fader strips. The center section also has the SOF (Sends on Fader) button. */
class FaderSection final : public juce::Component
{
public:
    static constexpr int kMinStripWidth = 60;
    static constexpr int kBankColumnWidth = 92;

    FaderSection (UiContext&, LayerSet::Section, const juce::String& title);

    /** Rebuilds the bank buttons and re-resolves the faders (structure/bank changes). */
    void refresh();
    /** Re-resolves the faders only (selection / SOF changes). */
    void refreshFaders();

    /** Number of strips shown right now. */
    int getNumStrips() const noexcept { return (int) strips.size(); }
    /** Width needed for `n` strips of width `stripWidth`. */
    static int widthFor (int n, int stripWidth) { return kBankColumnWidth + n * stripWidth; }

    /** For Auto width: how many strips fit `width`. */
    static int fitStrips (int width) { return juce::jmax (1, (width - kBankColumnWidth) / kMinStripWidth); }

    void paint (juce::Graphics&) override;
    void resized() override;

private:
    void selectBank (const juce::String& id);
    void layoutBanks();

    UiContext& ctx;
    LayerSet::Section section;
    juce::String title;
    Layer layer;
    int page = 0;

    juce::Viewport bankViewport;
    juce::Component bankContent;
    juce::OwnedArray<juce::TextButton> bankButtons;
    juce::TextButton pageLeft { "<" }, pageRight { ">" }, sofButton { "SOF" };
    juce::Label pageLabel;
    juce::OwnedArray<FaderStrip> strips;
};

} // namespace livedaw::ui
