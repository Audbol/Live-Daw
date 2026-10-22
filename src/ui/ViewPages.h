#pragma once

#include "ui/DisplayPage.h"
#include "ui/ScreenWidgets.h"
#include "ui/WingPopups.h"

namespace livedaw::ui
{

class MemberAssignView;

/** The VIEW screens (manual 5.7-5.11), opened by the fader sections' VIEW keys
    and by each other's status-bar keys: OVERVIEW (every strip's blocks),
    FADERS, SENDS, CONFIG (fader layers) and MUTE GROUPS. They show the fader
    section in ctx.viewSection; the layer column on the right picks its layer
    (blue) and the section (green), the bank keys left of it (1-8 ... DCA 9-16)
    show one bank of eight instead of the layer, the arrows under them page.
    With FADER/SCREEN LAYER LINK on (SETUP > SURFACE) a layer chosen here also
    switches the section's faders. Laid out in the WING's 1280 x 800 pixels
    (firmware 3.1). */
class ViewPage : public DisplayPage, protected juce::Timer
{
public:
    ViewPage (UiContext&, Screen);
    ~ViewPage() override;

    Screen getScreen() const noexcept { return screen; }

    bool usesKnobs() const override { return false; }
    std::vector<StatusKey> getStatusKeys() override;
    StatusTitle getStatusTitle() override;
    void refresh() override;
    void paint (juce::Graphics&) override;
    void resized() override;

    class LayerColumn;

protected:
    /** False for the left section's OVERVIEW and for MUTE GROUPS. */
    virtual bool hasLayerColumn() const { return true; }
    /** False for CONFIG (it edits the layer itself): a narrow column without bank keys. */
    virtual bool hasBankKeys() const { return true; }
    /** The last page the arrows reach (CONFIG: one more, to append to). */
    virtual int lastPage() const { return numPages() - 1; }
    virtual void layoutContent() {}
    virtual void paintContent (juce::Graphics&) {}
    virtual void layerChanged() {}

    void timerCallback() override;

    LayerSet::Section section() const noexcept { return ctx.viewSection; }
    juce::String currentLayerId() const;
    Layer currentLayer() const;
    /** A bank of the bank keys: "1-8" ... "DCA 9-16" and its faders. */
    struct Bank
    {
        juce::String key, title;
        std::vector<state::FaderRef> faders;
        int group = 0; // the column leaves a gap between groups
    };
    std::vector<Bank> banks() const;
    /** The bank shown instead of the layer (-1: the layer). */
    int shownBank() const noexcept { return bank[section()]; }
    /** Faders shown at once (the section's width). */
    int pageSize() const;
    int numPages() const;
    /** The faders of the current page of the layer. */
    std::vector<state::FaderRef> pageFaders() const;
    /** `count` strip columns from `area`'s top to its bottom, 96 WING px wide
        106 apart, right-aligned against the layer column. */
    std::vector<juce::Rectangle<int>> stripColumns (juce::Rectangle<int> area, int count) const;

    void selectLayer (const juce::String& id);
    void selectBank (int);
    void selectSection (LayerSet::Section);
    void stepPage (int delta);
    /** The invisible component the drop-down lists open under, moved over `area`. */
    juce::Component& anchorAt (juce::Rectangle<int> area);

    UiContext& ctx;
    const Screen screen;
    juce::Rectangle<int> content;
    int page = 0;

private:
    std::unique_ptr<LayerColumn> column;
    juce::String browsed[LayerSet::kNumSections];
    int bank[LayerSet::kNumSections] { -1, -1, -1 };
    juce::String shownTitle;
    juce::Component anchor;
};

/** OVERVIEW (fig. 53-54): each strip of the layer as a column of mini blocks
    in processing order. A click opens the strip's block on HOME. A DCA's
    column shows its members [WING]; a click on it opens the DCA's assign
    screen (the title tile in its colour). */
class OverviewViewPage final : public ViewPage
{
public:
    explicit OverviewViewPage (UiContext&);
    ~OverviewViewPage() override;
    void mouseUp (const juce::MouseEvent&) override;
    /** The VIEW keys, then EXCLUSIVE SOLO (WING 3.0.5: on the CHANNEL OVERVIEW
        page; the SETUP > AUDIO preference). */
    std::vector<StatusKey> getStatusKeys() override;
    StatusTitle getStatusTitle() override;
    void resized() override;

protected:
    bool hasLayerColumn() const override { return section() != LayerSet::Left; }
    void paintContent (juce::Graphics&) override;

private:
    std::unique_ptr<MemberAssignView> assign;
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

/** CONFIG (fig. 58, [I10]): edits the fader layers. Pick items in the top grid
    and drag them onto a slot (replaces it) or between two slots (a blue line:
    inserts and shifts the rest), or click a slot; drag a slot away to remove
    it, drag within the slots to reorder. Every layer key is editable: USER 1 /
    USER 2 hold user layers, the other keys keep an edited copy in the show
    (RESTORE gives back the generated one). The slots of the page shown sit in
    groups of four on blue plates, their numbers under them; the arrows page,
    one page past the end to append. */
class ConfigViewPage final : public ViewPage
{
public:
    explicit ConfigViewPage (UiContext& c);
    void mouseDown (const juce::MouseEvent&) override;
    void mouseDrag (const juce::MouseEvent&) override;
    void mouseUp (const juce::MouseEvent&) override;

protected:
    bool hasBankKeys() const override { return false; }
    int lastPage() const override { return numPages(); }
    void layoutContent() override;
    void paintContent (juce::Graphics&) override;
    void layerChanged() override;

private:
    /** Where a drop lands: on slot `index` (replace) or before it (insert). */
    struct DropTarget
    {
        int index = -1;
        bool insert = false;
    };

    std::vector<state::FaderRef> sourceItems() const;
    /** The destinations of SENDS (every bus, main and matrix) or BUS -> BUS
        (buses 1-8, mains, matrices). */
    std::vector<state::StripRef> destinations (bool busToBus) const;
    juce::String fieldText (int field) const;
    void chooseField (int field, DropdownField&);
    /** The user layer behind `layer` (USER 1 / 2: the first two user layers,
        created when `create`); empty for the default layers. */
    juce::String userLayerId (const Layer&, bool create);
    /** Stores the layer's faders (a user layer's, or the default key's edited copy). */
    void setLayerFaders (const Layer&, const std::vector<state::FaderRef>&);
    /** Puts `items` at layer position `index`, replacing what's there or inserting. */
    void placeAt (const std::vector<state::FaderRef>&, int index, bool insert);
    DropTarget dropTargetAt (juce::Point<int>) const;
    void updateKeys();

    /** MIDI CC: 32 CCs of one MIDI channel at a time ("CH 1  CC 0-31"). */
    static constexpr int kMidiCcsPerBank = 32, kMidiBanksPerChannel = 128 / kMidiCcsPerBank, kNumMidiBanks = 16 * kMidiBanksPerChannel;
    static juce::String midiBankName (int bank)
    {
        const int first = bank % kMidiBanksPerChannel * kMidiCcsPerBank;
        return "CH " + juce::String (bank / kMidiBanksPerChannel + 1) + "  CC " + juce::String (first) + "-" + juce::String (first + kMidiCcsPerBank - 1);
    }

    int category = 0; // CHANNELS, BUS/MAIN, DCA, then SENDS, BUS -> BUS, FX, MIDI CC (the fields)
    int sendDest = 0, busDest = 0, fxSlot = 0, midiBank = 0;
    std::vector<state::FaderRef> selectedItems;
    std::vector<juce::Rectangle<int>> categoryKeys, sourceTiles, slotAreas, plates;
    juce::Rectangle<int> sourceArea, slotsArea, hintArea;
    juce::OwnedArray<DropdownField> fields;
    WingKey clearKey { "CLEAR", Icon::Trash }, renameKey { "RENAME", Icon::Edit }, newKey { "NEW", Icon::Plus }, deleteKey { "DELETE" };

    // Dragging: from the source grid (dragSlot < 0) or from a slot.
    bool dragActive = false;
    int dragSlot = -1;
    juce::Point<int> dragPoint;
};

/** MUTE GROUPS (fig. 61, firmware 3.1): the eight groups' members as columns
    (red while the group is engaged) and the group keys on the right (a key
    engages its group). A click on a column opens the group's assign screen;
    there its tab touched again renames the group. */
class MuteGroupsViewPage final : public ViewPage
{
public:
    explicit MuteGroupsViewPage (UiContext& c);
    ~MuteGroupsViewPage() override;
    void mouseUp (const juce::MouseEvent&) override;

    /** A red muted-speaker icon and MUTE GROUPS (manual fig. 61). */
    StatusTitle getStatusTitle() override
    {
        StatusTitle t;
        t.kind = StatusTitle::Kind::Text;
        t.text = "MUTE GROUPS";
        t.icon = Icon::MuteGroup;
        t.ink = Colours::textDim;
        t.iconInk = juce::Colour (0xfff05050);
        return t;
    }

protected:
    bool hasLayerColumn() const override { return false; }
    void layoutContent() override;
    void paintContent (juce::Graphics&) override;

private:
    juce::Rectangle<int> columns[kNumMuteGroups], groupKeys[kNumMuteGroups];
    std::unique_ptr<MemberAssignView> assign;
};

/** "OVERVIEW", "FADERS", ... */
const char* viewScreenName (Screen);

} // namespace livedaw::ui
