#pragma once

#include <juce_graphics/juce_graphics.h>

#include <vector>

namespace livedaw::ui
{

/** Vector icons in the style of the WING's screen. */
enum class Icon
{
    Gear,
    Tools,
    Wrench,
    Xlr,        // input channel
    Jack,       // aux channel
    Bus,        // mix bus
    Speakers,   // main
    Matrix,
    Headphones,
    Speaker,
    LockOpen,
    LockClosed,
    Plus,
    LowCut,
    HighCut,
    Tilt,
    Gate,
    Compressor,
    Insert,
    Eq,
    Folder,
    Check,
    Reset,
    Edit,
    Trash,
    Play,
    Stop,
    ChevronLeft,
    ChevronRight,
    Disk,
    // Printed on the channel strip and monitoring keys
    GainArrows,  // ↕ (EQ GAIN)
    WidthArrows, // ↔ (EQ WIDTH)
    FreqMarker,  // ‹|› (EQ FREQ)
    Bell,        // EQ band legend
    LowShelf,
    HighShelf,
    Usb,         // USB trident
    Circle,      // the MONO key
    Copy,       // status key COPY
    Maxer,      // tool filters
    AllPass90,
    AllPass180,
    DeltaT,     // delay
    Invert,     // polarity
    Cancel,     // circled x: NONE
    UsbStick,   // USB stick (status bar)
    // SETUP, LIBRARY, VIEW and ROUTING screens
    Power,         // SHUTDOWN
    RemoveCircle,  // REMOVE (a dot in a ring)
    MinusCircle,   // REMOVE ITEMS
    ClearCircle,   // CLEAR / NONE (a cross in a ring)
    Info,          // the circled "i" after some labels
    Skip,          // SKIP (play + bar)
    Link,          // LINK (chain)
    Tag,           // EDIT TAG
    Storage,       // INT location (internal storage)
    ShowFile,      // SHOW location (film frame)
    GoPrev,        // GO PREV (play + <)
    GoNext,        // GO NEXT (play + >)
    SortDate,      // sort by date (calendar)
    Overview,      // VIEW: OVERVIEW (grid)
    Faders,        // VIEW: FADERS, ROUTING: CHANNELS
    Sends,         // VIEW: SENDS
    Layers,        // fader layer (status title of the VIEW screens)
    LayersGear,    // VIEW: CONFIG
    MuteGroup,     // VIEW: MUTEGRP (speaker with a cross)
    Monitors,      // SETUP: MONITORS
    CcEdit,        // SETUP: CC EDIT
    Pause,
    Record,
    SaveTray,      // SAVE (arrow down into a tray)
    LoadTray,      // LOAD (arrow up out of a tray)
    SourcesIn,     // ROUTING: SOURCES (arrow into a ring)
    OutputsOut,    // ROUTING: OUTPUTS (arrow out of a ring)
    ChevronUp,
    ChevronDown,
    // Recorder and multitrack editor
    ReturnToStart, // |◀
    Rewind,        // ◀◀
    FastForward,   // ▶▶
    Loop,          // two arrows in a loop
    MarkerAdd,     // +I (WING-LIVE add marker)
    MarkerPrev,    // I◀
    MarkerNext,    // ▶I
    SdCard,        // the WING-LIVE card (status bar)
    Waveform,      // the editor (EDIT, DAW)
    // HOME, CC EDIT and the insert screens
    Premium,       // the premium-effect diamond (FX slots 1-8)
    Sun,           // ICON/COLOR: LIGHT
    ListNone,      // NONE (a list of empty boxes)
    ListAll,       // ALL (a list of filled boxes)
    PaintBucket,   // CC EDIT: COLOR
    Warning,       // CC EDIT: CLR ALL (a triangle with "!")
    KeyboardEdit,  // a keyboard with a pencil: LINK CUSTOMIZATION, SET CUST
    Spill,         // SPILL (a fader with arrows)
    // The main display's frame
    Maximize,      // four corners pointing out: the screen fills the window
    Restore        // four corners pointing in: back to the surface
};

/** Draws `icon` fitted into `area` (stroked or filled as the icon needs). */
void drawIcon (juce::Graphics&, Icon, juce::Rectangle<float> area, juce::Colour);

/** Draws the WING channel icon `number` (/ch/n/icon: 1-14 general, the 100s
    microphones and vocals, 200s drums, 300s strings and winds, 400s keys, 500s
    speakers, 600s specials) as line art fitted into `area`. A number this set
    has no drawing of its own gets its category's icon. Returns false, drawing
    nothing, for 0 (no icon) and unknown numbers: draw the strip type's icon
    (stripIcon) then. */
bool drawWingIcon (juce::Graphics&, int number, juce::Rectangle<float> area, juce::Colour);

/** The console's name for icon `number` ("Kick In"), or empty. */
juce::String wingIconName (int number);

/** The icon numbers of a category (0 general, 1 microphones and vocals, 2 drums,
    3 strings and winds, 4 keys, 5 speakers, 6 specials), or all of them (-1). */
std::vector<int> wingIconNumbers (int category = -1);

} // namespace livedaw::ui
