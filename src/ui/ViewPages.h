#pragma once

#include "ui/DisplayPage.h"
#include "ui/LayerDrop.h"
#include "ui/ScreenWidgets.h"
#include "ui/WingPopups.h"

namespace livedaw::ui
{

class MemberAssignView;
class DcaPanel;

/** A bank of the VIEW screens' bank keys: "1-8" ... "DCA 9-16" and its faders. */
struct ViewBank
{
    juce::String key, title;
    std::vector<state::FaderRef> faders;
    int group = 0; // the column leaves a gap between groups
};

/** The bank keys' banks: each strip kind in banks of eight ("1-8" ...
    "33-40", "AUX", "BUS 1-8", ...), the mains and matrices one group, then
    the DCAs (also the custom controls' OVERVIEW PAGE banks). */
std::vector<ViewBank> viewBanks (UiContext&);

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
    /** A custom control's OVERVIEW PAGE: section `s`, on layer key `bank`
        (0-8) or fixed bank `bank - cc::kFirstOverviewBank`. */
    void showOverviewBank (LayerSet::Section s, int bank);
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

    bool isCompact() const noexcept { return ctx.consoleModel == ConsoleModel::Compact; }
    /** The section shown: the WING COMPACT has its fader section (the left)
        and its VIRTUAL section (WING manual 5.7 p. 61, Fig. 55; the center). */
    LayerSet::Section section() const noexcept
    {
        if (ctx.consoleModel == ConsoleModel::Compact)
            return ctx.viewSection == LayerSet::kVirtualSection ? LayerSet::kVirtualSection : LayerSet::Left;
        return ctx.viewSection;
    }
    /** The COMPACT's VIRTUAL section is shown. */
    bool isVirtual() const noexcept { return isCompact() && section() == LayerSet::kVirtualSection; }
    juce::String currentLayerId() const;
    Layer currentLayer() const;
    using Bank = ViewBank;
    std::vector<Bank> banks() const { return viewBanks (ctx); }
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
    /** A DCA's panel over the page: its colour, light, icon, SOLO BUS and name. */
    void showDcaPanel (int dca);

    UiContext& ctx;
    const Screen screen;
    juce::Rectangle<int> content;
    int page = 0;

private:
    std::unique_ptr<LayerColumn> column;
    std::unique_ptr<DcaPanel> dcaPanel;
    juce::String browsed[LayerSet::kNumSections];
    int bank[LayerSet::kNumSections] { -1, -1, -1 };
    juce::String shownTitle;
    juce::uint64 shownHash = 0;
    juce::Component anchor;
};

/** OVERVIEW (fig. 53-54): each strip of the layer as a column of mini blocks
    in processing order. A click opens the strip's block on HOME. A DCA's
    column shows its members [WING] ("DCA" at its foot yellow while the DCA is
    soloed); a click on it opens the DCA's assign screen, whose title tile (in
    the DCA's colour) opens the DCA's panel. */
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
    bool titleClicked() override;
    void resized() override;

protected:
    /** Not on the full-size WING's left section; the COMPACT's (with COMPACT
        and VIRTUAL at its foot) has it. */
    bool hasLayerColumn() const override { return isCompact() || section() != LayerSet::Left; }
    void paintContent (juce::Graphics&) override;

private:
    std::unique_ptr<MemberAssignView> assign;
};

/** FADERS (fig. 56): a virtual fader for each strip of the layer. A DCA's
    name opens its panel; its label at the foot is yellow while it's soloed. */
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

/** CONFIG (fig. 58, [I10]): edits the fader layers, whose slots are fixed as
    on the WING: a slot can be blank (an inactive strip on the surface) and the
    same fader can sit in several slots. Pick items in the top grid and drag
    them onto a slot, or click a slot with items picked: they fill that slot
    (and the next ones for several items), replacing what was there; an item
    already in the layer gets another slot. Dropped near the boundary between
    two slots (a bright bar marks it) they're inserted there and the slots
    after it move right (refused when the layer's last slot is in use). Drag
    a slot onto another to swap the two (onto a blank slot: move it), between
    two to insert it there (its old slot turns blank), off the slots to blank
    it. Right-click or long-press a slot: CLEAR SLOT, INSERT EMPTY SLOT. CLEAR
    blanks the whole layer. Every layer key is editable: USER 1 / USER 2 hold
    user layers, the other keys keep an edited copy in the show (RESTORE gives
    back the generated one). The slots of the page shown sit in groups of four
    on blue plates, their numbers under them; the arrows page, one page past
    the end to append. */
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
    /** A press this long on a slot opens its menu (touch screens). */
    static constexpr int kLongPressMs = 600;

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
    /** Applies a slot edit (layerslots) to the layer shown; an edit that
        returns false is refused (the layer is full) and says so. */
    void editSlots (const std::function<bool (std::vector<state::FaderRef>&)>&);
    /** The picked items dropped or tapped on `target` (layerslots::dropItems). */
    void dropItemsAt (layerslots::DropTarget target);
    void clearSlot (int index);
    void insertEmptySlot (int index);
    /** CLEAR SLOT / INSERT EMPTY SLOT for slot `local` of the page shown. */
    void showSlotMenu (int local);
    /** Where a drop at `pos` lands (layerdrop::targetAt). */
    layerslots::DropTarget dropTargetAt (juce::Point<int>) const;
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
    /** SOURCE / BUS CUSTOMIZATION (manual 5.10 p. 66, Fig. 60): the send
        faders assigned show their destination's name, colour and icon. */
    bool busCustomization = false;
    juce::Rectangle<int> sourceCustomKey, busCustomKey;
    std::vector<state::FaderRef> selectedItems;
    std::vector<juce::Rectangle<int>> categoryKeys, sourceTiles, slotAreas, plates;
    juce::Rectangle<int> sourceArea, slotsArea, hintArea;
    juce::OwnedArray<DropdownField> fields;
    WingKey clearKey { "CLEAR", Icon::Trash }, renameKey { "RENAME", Icon::Edit }, newKey { "NEW", Icon::Plus }, deleteKey { "DELETE" };

    // Dragging: from the source grid (dragSlot < 0) or from a filled slot.
    // pressSlot: the page slot pressed (filled or blank), for the slot menu.
    bool dragActive = false;
    int dragSlot = -1, pressSlot = -1;
    juce::Point<int> dragPoint;
};

/** MUTE GROUPS (fig. 61, firmware 3.1): the eight groups' members as columns
    (red while the group is engaged) and the group keys on the right (a key
    engages its group). A column's header (MGRP.n over the number) opens the
    MUTE GROUP n window to rename the group [WING]; the rest of the column
    opens the group's assign screen. */
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
    void renameGroup (int);

    juce::Rectangle<int> columns[kNumMuteGroups], headers[kNumMuteGroups], groupKeys[kNumMuteGroups];
    std::unique_ptr<MemberAssignView> assign;
};

/** "OVERVIEW", "FADERS", ... */
const char* viewScreenName (Screen);

} // namespace livedaw::ui
