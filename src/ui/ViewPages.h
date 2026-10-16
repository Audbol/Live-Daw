#pragma once

#include "ui/DisplayPage.h"
#include "ui/ScreenWidgets.h"

namespace livedaw::ui
{

/** The VIEW screens (manual 5.7-5.11), opened by the fader sections' VIEW keys
    and by each other's status-bar keys: OVERVIEW (every strip's blocks),
    FADERS, SENDS, CONFIG (fader layers) and MUTE GROUPS. They show the fader
    section in ctx.viewSection; the layer column on the right picks its layer
    (blue), the section (green) and the page. With FADER/SCREEN LAYER LINK on
    (SETUP > SURFACE) a layer chosen here also switches the section's faders. */
class ViewPage : public DisplayPage, protected juce::Timer
{
public:
    ViewPage (UiContext&, Screen);
    ~ViewPage() override;

    Screen getScreen() const noexcept { return screen; }

    bool usesKnobs() const override { return false; }
    std::vector<StatusKey> getStatusKeys() override;
    void refresh() override;
    void paint (juce::Graphics&) override;
    void resized() override;

    class LayerColumn;

protected:
    /** False for the left section's OVERVIEW and for MUTE GROUPS. */
    virtual bool hasLayerColumn() const { return true; }
    virtual void layoutContent() {}
    virtual void paintContent (juce::Graphics&) {}
    virtual void layerChanged() {}

    void timerCallback() override;

    LayerSet::Section section() const noexcept { return ctx.viewSection; }
    juce::String currentLayerId() const;
    Layer currentLayer() const;
    /** Faders shown at once (the section's width). */
    int pageSize() const;
    int numPages() const;
    /** The faders of the current page of the layer. */
    std::vector<state::FaderRef> pageFaders() const;
    /** `count` strip columns in `area`, right-aligned against the layer column. */
    std::vector<juce::Rectangle<int>> stripColumns (juce::Rectangle<int> area, int count) const;

    void selectLayer (const juce::String& id);
    void selectSection (LayerSet::Section);
    void stepPage (int delta);

    UiContext& ctx;
    const Screen screen;
    juce::Rectangle<int> content;
    int page = 0;

private:
    std::unique_ptr<LayerColumn> column;
    juce::String browsed[LayerSet::kNumSections];
};

/** OVERVIEW (fig. 53-54): each strip of the layer as a column of mini blocks
    in processing order. A click opens the strip's block on HOME. */
class OverviewViewPage final : public ViewPage
{
public:
    explicit OverviewViewPage (UiContext& c) : ViewPage (c, Screen::Overview) {}
    void mouseUp (const juce::MouseEvent&) override;

protected:
    bool hasLayerColumn() const override { return section() != LayerSet::Left; }
    void paintContent (juce::Graphics&) override;
};

/** FADERS (fig. 56): a virtual fader for each strip of the layer. */
class FadersViewPage final : public ViewPage
{
public:
    explicit FadersViewPage (UiContext& c) : ViewPage (c, Screen::Faders) {}
    void mouseDown (const juce::MouseEvent&) override;
    void mouseDrag (const juce::MouseEvent&) override;
    void mouseUp (const juce::MouseEvent&) override;
    void mouseDoubleClick (const juce::MouseEvent&) override;
    void mouseWheelMove (const juce::MouseEvent&, const juce::MouseWheelDetails&) override;

protected:
    void paintContent (juce::Graphics&) override;

private:
    std::atomic<float>* faderAt (juce::Point<int>, juce::Rectangle<int>* columnOut = nullptr) const;
    std::atomic<float>* dragging = nullptr;
    juce::Rectangle<int> dragColumn;
    float dragPos = 0.0f;
    int lastY = 0;
};

/** SENDS (fig. 57): "virtual Sends On Faders": the layer's strips as send
    faders to one destination (a bus, main or matrix). */
class SendsViewPage final : public ViewPage
{
public:
    explicit SendsViewPage (UiContext& c) : ViewPage (c, Screen::Sends) {}
    void mouseDown (const juce::MouseEvent&) override;
    void mouseDrag (const juce::MouseEvent&) override;
    void mouseUp (const juce::MouseEvent&) override;
    void mouseDoubleClick (const juce::MouseEvent&) override;

protected:
    void layoutContent() override;
    void paintContent (juce::Graphics&) override;

private:
    struct Destination
    {
        StripKind kind = StripKind::Bus;
        int index = 0; // bus slot, main or matrix index
    };

    std::vector<Destination> destinations() const;
    Destination destination() const;
    Strip* destinationStrip() const;
    bool canSend (const Strip&) const;
    /** The send level and on flag from `source` to the destination (nullptr: can't send). */
    std::atomic<float>* sendLevel (Strip& source, bool create) const;
    std::atomic<bool>* sendOn (Strip& source, bool create) const;
    std::atomic<float>* faderAt (juce::Point<int>, juce::Rectangle<int>* columnOut = nullptr) const;

    int group = 0, chosen = 0;
    std::vector<juce::Rectangle<int>> groupKeys, destTiles;
    juce::Rectangle<int> strips;
    std::atomic<float>* dragging = nullptr;
    juce::Rectangle<int> dragColumn;
    float dragPos = 0.0f;
    int lastY = 0;
};

/** CONFIG (fig. 58): edits the fader layers. Pick items in the top grid and
    drag them to the layer's slots at the bottom (or click a slot), drag a slot
    away to remove it, drag within the slots to reorder. USER 1 / USER 2 and
    further user layers are editable; the other layer keys are read-only
    (session item E19): NEW makes a user layer from the shown one. */
class ConfigViewPage final : public ViewPage
{
public:
    explicit ConfigViewPage (UiContext& c);
    void mouseDown (const juce::MouseEvent&) override;
    void mouseDrag (const juce::MouseEvent&) override;
    void mouseUp (const juce::MouseEvent&) override;

protected:
    void layoutContent() override;
    void paintContent (juce::Graphics&) override;
    void layerChanged() override;

private:
    std::vector<state::FaderRef> sourceItems() const;
    /** User layers and the USER 1 / USER 2 keys can be edited. */
    static bool isEditable (const Layer&);
    /** The user layer behind `layer` (USER 1 / 2: the first two user layers,
        created when `create`); empty for the default layers. */
    juce::String userLayerId (const Layer&, bool create);
    void insertAt (const std::vector<state::FaderRef>&, int slot);
    void updateKeys();

    int category = 0;
    std::vector<state::FaderRef> selectedItems;
    std::vector<juce::Rectangle<int>> categoryKeys, sourceTiles, slotAreas;
    juce::Rectangle<int> sourceArea, slotsArea, hintArea;
    juce::OwnedArray<DropdownField> fields;
    WingKey clearKey { "CLEAR", Icon::Trash }, renameKey { "RENAME", Icon::Edit }, newKey { "NEW", Icon::Plus }, deleteKey { "DELETE" };

    // Dragging: from the source grid (dragSlot < 0) or from a slot.
    bool dragActive = false;
    int dragSlot = -1;
    juce::Point<int> dragPoint;
};

/** MUTE GROUPS (fig. 61): the eight groups' members (click a column, then its
    cells), names (click the header) and the group keys on the right. */
class MuteGroupsViewPage final : public ViewPage
{
public:
    explicit MuteGroupsViewPage (UiContext& c);
    void mouseUp (const juce::MouseEvent&) override;

protected:
    bool hasLayerColumn() const override { return false; }
    void layoutContent() override;
    void paintContent (juce::Graphics&) override;

private:
    struct Cell
    {
        juce::Rectangle<int> area;
        Strip* strip;
        int group;
    };

    int selectedGroup = 0;
    juce::Rectangle<int> columns[kNumMuteGroups], headers[kNumMuteGroups], groupKeys[kNumMuteGroups];
    std::vector<Cell> cells;
    std::vector<std::pair<juce::Rectangle<int>, juce::String>> captions;
    WingKey unassignKey { "UNASSIGN", Icon::ClearCircle };
};

/** "OVERVIEW", "FADERS", ... */
const char* viewScreenName (Screen);

} // namespace livedaw::ui
