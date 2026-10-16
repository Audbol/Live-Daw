#pragma once

#include "ui/DisplayPage.h"
#include "ui/HomeScreens.h"

#include <memory>

namespace livedaw::ui
{

/** The HOME screen of the selected strip, as on the WING (manual 5.1 and
    chapter 6). Left, the toolbar: HOME, then a thumbnail per processing slot at
    fixed rows (gaps where a strip type has no such slot), the fader bar with
    the TAP point, MAIN and SENDS. On Channel Home it also has a key beside each
    slot that switches it, and the wrench to drag the slots into another order.
    Channel Home has the OVERVIEW / ICON/COLOR / NAME / TAGS tabs and the SAFE
    key; the slot screens fill the rest of the display. INIT, COPY (and
    SETTINGS) sit in the status bar and open overlay screens. */
class HomePage final : public DisplayPage
{
public:
    explicit HomePage (UiContext&);
    ~HomePage() override;

    void showTab (HomeTab);
    HomeTab getTab() const noexcept { return current; }

    std::vector<KnobBinding> getKnobs() override;
    std::vector<KnobStyle> getKnobStyles() override;
    std::vector<StatusKey> getStatusKeys() override;
    void refresh() override;

    void paint (juce::Graphics&) override;
    void resized() override;

    class BlockColumn;

private:
    enum class OverlayKind
    {
        None,
        Init,
        Copy,
        Settings,
        Feed
    };

    void rebuild();
    void showSends (bool matrixPage);
    void openOverlay (OverlayKind);
    void closeOverlay();
    bool isAvailable (HomeTab, const Strip*) const;
    static bool isOverviewGroup (HomeTab t) { return t == HomeTab::Home || t == HomeTab::Icon || t == HomeTab::Name || t == HomeTab::Tags; }

    UiContext& ctx;
    HomeTab current = HomeTab::Home;
    bool matrixSends = false;
    Strip* strip = nullptr;
    std::unique_ptr<BlockColumn> column;
    juce::OwnedArray<juce::TextButton> topTabs;
    std::unique_ptr<juce::TextButton> safeKey;
    std::unique_ptr<HomeScreen> content;
    std::unique_ptr<home::OverlayScreen> overlay;
    OverlayKind overlayKind = OverlayKind::None;
};

} // namespace livedaw::ui
