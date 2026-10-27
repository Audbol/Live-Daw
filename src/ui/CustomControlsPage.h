#pragma once

#include "ui/DisplayPage.h"
#include "ui/ScreenWidgets.h"

#include <array>

/** Custom controls (the WING's CUSTOM CONTROLS section and CC EDIT): the
    assignments saved with the show, what each function does, and the CC EDIT
    screen. */
namespace livedaw::ui::cc
{

/** The upper part's 16 layers (UP / DOWN), then the lower part's USER layer,
    three legacy layers, the lower part's DAW 1-4 layers of DAW mode and the
    WING COMPACT's key sets (stored with the show, see livedaw-session/
    custom.rs). The 4-Channel section's F1-F3 play layers 1-3 (WING manual 5.5
    p. 48: "mirrors the Custom Control layers 1-3 ... F1 opens the 4 knobs and
    top row of 4 buttons of the first layer"); older shows kept them on the
    legacy layers 17-19, which loading moves to layers 1-3. */
constexpr int kNumLayers = 16, kUserLayer = 16, kLegacyFourChannelLayer = 17, kNumStoredLayers = 38, kNumControls = 4;
constexpr int kFirstDawLayer = 20, kNumDawLayers = 4;
/** The COMPACT's key sets (WING manual 4.8: "U1-U4: User layer buttons 1-16,
    M/M: Main Matrix layer buttons 5-8, D1-D4: DCA layer buttons 1-16";
    /$ctl/user/U1-U4, MM, D1-D4): four keys each, an upper key per control.
    Then the keys of DAW mode (DAW LAYER on): DAW 1-4 .. 13-16 the 16 keys,
    DAW MODE KEYS the four mode keys. A WING COMPACT's owner: in DAW mode
    they do nothing until the user assigns them, so they start empty (no
    remote node; Sidecar's own, saved with the show, custom.rs). */
constexpr int kFirstCompactUserLayer = 24, kCompactMainMatrixLayer = 28, kFirstCompactDcaLayer = 29, kFirstCompactDawLayer = 33,
              kCompactDawModeLayer = 37, kNumCompactLayers = 14;

/** The COMPACT's key sets: USER 1-4 .. 13-16, MAIN/MTX 5-8, DCA 1-4 .. 13-16, DAW 1-4 .. 13-16, DAW MODE KEYS. */
inline bool isCompactLayer (int layer) noexcept { return layer >= kFirstCompactUserLayer && layer < kFirstCompactUserLayer + kNumCompactLayers; }
/** The layers with keys only: the lower part's (USER, DAW 1-4) and the COMPACT's sets. */
inline bool isLowerLayer (int layer) noexcept { return layer == kUserLayer || layer >= kFirstDawLayer; }
/** The lower part's layers have a lower key under each upper one; the COMPACT's sets don't. */
inline bool hasLowerKeys (int layer) noexcept { return ! isCompactLayer (layer); }
/** "LAYER 3", "USER", "DAW 2", "USER 5-8", "MAIN/MTX 5-8", "DCA 1-4" (WING Edit's names);
    the COMPACT's DAW mode sets "DAW KEYS 1-4" .. and "DAW MODE KEYS". */
juce::String layerName (int layer);

/** The encoder of a control, or one of the two keys under it. */
enum class Slot
{
    Encoder,
    Upper,
    Lower
};

struct Function
{
    int category = 0, target = 0, option = 0;

    bool isAssigned() const noexcept { return category != 0; }
    bool operator== (const Function& o) const noexcept { return category == o.category && target == o.target && option == o.option; }
};

struct Control
{
    Function encoder, upper, lower;
    juce::String name;
    int colour = 0; // WING colour 1-18 with its light on; -18..-1 the colour with the light off; 0 = none

    Function& slot (Slot s) { return s == Slot::Encoder ? encoder : s == Slot::Upper ? upper : lower; }
    const Function& slot (Slot s) const { return s == Slot::Encoder ? encoder : s == Slot::Upper ? upper : lower; }
};

/** The show's assignments, cached: reload() after a show was loaded; every
    change is written back to the show at once. */
class Assignments
{
public:
    explicit Assignments (Show&);

    void reload();
    const Control& get (int layer, int index) const;
    void set (int layer, int index, const Control&);
    /** CLR ALL. */
    void clearLayer (int layer);
    /** Bumped by every change, so the surface knows to redraw. */
    int getRevision() const noexcept { return revision; }

    /** The upper part's layer (UP / DOWN; CC EDIT edits it too). */
    int currentLayer = 0;

private:
    void save();

    Show& show;
    std::array<std::array<Control, kNumControls>, kNumStoredLayers> layers {};
    int revision = 0;
};

// The function categories (Drew Brashler's sheet of the 3.x functions; the
// ones that need features this console doesn't have are listed disabled).
enum EncoderCategory
{
    EncNone,
    EncFader,
    EncPan,
    EncDcaFader,
    EncSelBusSend,
    EncFixedBusSend,
    EncFxParameter,
    EncMonitoring,
    EncDawVpot,
    EncMidiCc,
    EncLighting, // OTHER (the WING's lighting controls): the option is LAMP, GLOW, PATCH BRIGHTNESS
    kNumEncoderCategories
};

enum ButtonCategory
{
    BtnNone,
    BtnMute,
    BtnSolo,
    BtnDcaMute,
    BtnMuteGroup,
    BtnSendsOnFader,
    BtnChannelPage,
    BtnGlobalPage,
    BtnFaderPage,
    BtnShowControl,
    BtnSceneRecall,
    BtnOther,
    BtnFaderSpill,
    BtnDawButtons,
    BtnMidi,
    BtnUsbPlayer,
    BtnSdRecorder,
    BtnGpio,
    BtnCcBank,       // 3.0.6: the option is the upper part's layer (CC bank) to show
    BtnInsertPre,    // CH INSERT PRE ON/OFF (WING INS1): the target is the strip
    BtnInsertPost,   // CH INSERT POST ON/OFF (INS2)
    BtnFxParameter,  // FX PAR: the target is the FX slot, the option the parameter (-1: MIX)
    BtnOverviewPage, // OVERVIEW PAGE (VIEWPAGE): the target is the area, the option the bank
    BtnFaderStart,   // FADER START (FSTART): the target is the strip; lit while its fader is up
    BtnDawVpot,      // DAW V-POTS (DAWENC): the target is the rotary (custom.rs DAW_VPOT_KEY_CATEGORY)
    kNumButtonCategories
};

/** OTHER's functions (the option; custom.rs `other`). CC EDIT lists them in
    WING Edit's order (otherOrder()). */
enum OtherFunction
{
    OtherMonitorDim,
    OtherMonitorMono,
    OtherMonitorSwap,
    OtherMonitorMute,
    OtherFaderOff,
    OtherFaderMinus10,
    OtherFader0,
    OtherAutomixX,
    OtherAutomixY,
    OtherChannelPrev,
    OtherChannelNext,
    OtherCcBankUp,
    OtherCcBankDown,
    OtherSofOff,
    OtherTalkbackA,
    OtherTalkbackB,
    OtherGlobalMainAlt,
    OtherDawLayer,
    OtherMonitorPhones,
    OtherMonitorSpeakers,
    OtherMonitorPhonesSpeakers,
    OtherChSoloTap,  // CH, BUS, MAIN, MTX SOLO TAP: PFL / AFL
    OtherBusSoloTap,
    OtherMainSoloTap,
    OtherMtxSoloTap,
    OtherChMeterTap, // CH, BUS, MAIN, MTX, DCA METER TAP: PRE / POST
    OtherBusMeterTap,
    OtherMainMeterTap,
    OtherMtxMeterTap,
    OtherDcaMeterTap,
    kNumOtherFunctions
};

/** The OVERVIEW PAGE areas (the WING's L, C, R, CMPCT, RCK, EXT, VRT) and
    banks: 0-8 the section's layer keys, from kFirstOverviewBank its fixed
    banks (1-8 ... DCA 9-16; the WING's bank 1-21). */
constexpr int kNumOverviewAreas = 7, kFirstOverviewBank = 9;
/** DAW V-POTS keys' rotaries: M1P-M8P, E1P-E16P (pushed while held), M1-M8,
    E1-E16 (a step clockwise per press), JOG. */
constexpr int kNumDawVpotKeys = 49, kFirstDawVpotTurn = 24;
/** A DAW V-POTS key's rotary as the WING names it ("M1P", "E16", "JOG"). */
juce::String dawVpotKeyCode (int target);
/** The fader section an OVERVIEW PAGE area shows (VRT: the COMPACT's VIRTUAL). */
LayerSet::Section overviewSection (int area) noexcept;

int numCategories (Slot);
/** The FUNCTION list's rows, as categories: WING Edit's order (CH MUTE, CH
    SOLO, CH INSERT PRE ...), Sidecar's own (CC BANK) after OTHER. */
std::vector<int> categoryOrder (Slot);
/** OTHER's functions in WING Edit's order (TALKBACK A, TALKBACK B, GLOBAL
    MAIN/ALT, DAW LAYER, MONITOR PHONES ...), Sidecar's own at the end. */
const std::vector<int>& otherOrder();
juce::String categoryName (Slot, int category);
/** False for the functions this console can't do yet (drawn disabled). */
bool categoryAvailable (Slot, int category);
/** The FUNCTION list's row colours (bus sends dark red, FX parameters magenta, ...). */
juce::Colour categoryColour (Slot, int category);

/** A strip as a function target: kind * 100000 + id. */
int encodeStrip (const state::StripRef&) noexcept;
state::StripRef decodeStrip (int target) noexcept;

/** The knob binding of an encoder function (invalid when it does nothing). */
KnobBinding bindEncoder (UiContext&, const Function&);
/** What the encoder over a key sets while the key is held (WING manual 4.8
    p. 25: "hold down the button and turn the knob directly above it"; on
    the COMPACT the knob right of the main display): an FX PAR key's
    parameter; invalid for the other functions. */
KnobBinding bindHeldKey (UiContext&, const Function&);
/** A key press. */
void press (UiContext&, const Function&);
/** A key released (the push functions: DAW BUTTONS, MIDI CC / NOTE PUSH). */
void release (UiContext&, const Function&);
/** A key's LED as SurfaceKey::Led (0 off, 1 lit, 2 blinking: DAW LEDs
    blink). An FX PAR key on a delay's TIME taps the tempo and flashes with it
    as SETUP > GENERAL > TAP TEMPO FLASH says (OFF, 8X: eight beats after the
    TIME changes, ON). */
int ledState (UiContext&, const Function&);
/** TAP TEMPO FLASH (manual 5.5 p. 47): 0 OFF, 1 8X, 2 ON (the default). */
int tapTempoFlash (UiContext&);
/** Whether a key's LED is lit. */
bool isLit (UiContext&, const Function&);
/** "CH1 FADER", "SCN GO", "TALK A"... */
juce::String describe (UiContext&, Slot, const Function&);

} // namespace livedaw::ui::cc

namespace livedaw::ui
{

/** CUSTOM CONTROLS (CC EDIT, [D1]): opened by pressing SETUP twice, SETUP's
    CC EDIT key and the CC section's VIEW key. The ‹ › keys pick the layer; the
    LAYER panel maps its four encoders and eight keys (touch one to edit it);
    the FUNCTION list (rows coloured by category) and one or two lists for
    the function's target and option assign it; SETTINGS has COLOR and
    CLR ALL, CONTROL has NAME (a name starting with '*' shows the value, a
    word written "|word" is shown inverted on the LCD). */
class CustomControlsPage final : public DisplayPage, private juce::Timer
{
public:
    CustomControlsPage (UiContext&, cc::Assignments&, int layer = -1);

    bool usesKnobs() const override { return false; }
    StatusTitle getStatusTitle() override;
    void paint (juce::Graphics&) override;
    void resized() override;
    void mouseUp (const juce::MouseEvent&) override;
    void mouseWheelMove (const juce::MouseEvent&, const juce::MouseWheelDetails&) override;

private:
    /** A list column: its header, rows and the ▲ ▼ keys under it. */
    struct ListColumn
    {
        juce::String title;
        juce::StringArray rows;
        std::vector<juce::Colour> bars;     // a colour bar at the row's left (channels)
        std::vector<juce::Colour> fills;    // the row's background (FUNCTION)
        std::vector<bool> enabled;
        int selected = -1, scroll = 0;
        juce::Rectangle<int> header, body, upKey, downKey;
        std::vector<juce::Rectangle<int>> rowAreas;
    };

    void timerCallback() override;
    void rebuildLists();
    /** Scrolls each list to its selected row (touching a control). */
    void revealSelection();
    void layoutList (ListColumn&, juce::Rectangle<int> area);
    void paintList (juce::Graphics&, ListColumn&, bool focused);
    void paintLayer (juce::Graphics&);
    void choose (int list, int row);
    void setLayer (int);
    cc::Control control() const;
    cc::Function function() const;
    void store (const cc::Function&);
    /** The FUNCTION list's categories (categoryOrder()), the targets (list 1)
        and options (list 2) of the selected function. */
    std::vector<int> categoryCodes, targetCodes, optionCodes;
    bool targetIsOption = false; // the first sub-list holds the options (no targets)

    UiContext& ctx;
    cc::Assignments& assignments;
    int layer = 0, index = 0;
    cc::Slot slot = cc::Slot::Encoder;
    int focusedList = 0;
    ListColumn lists[3];
    juce::Rectangle<int> header, prevKey, nextKey, userKey, gpioKey, title, layerHeader, layerBody, settingsHeader, controlHeader, colourKey, clearKey, nameKey,
        colourBox, clearBox, nameBox;
    struct Element
    {
        juce::Rectangle<int> area;
        int index;
        cc::Slot slot;
    };
    std::vector<Element> elements;
    std::vector<juce::Rectangle<int>> colourBars;
    juce::Component anchor;
    int shownRevision = -1;
};

} // namespace livedaw::ui
