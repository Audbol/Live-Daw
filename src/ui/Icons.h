#pragma once

#include <juce_graphics/juce_graphics.h>

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
    Waveform       // the editor (EDIT, DAW)
};

/** Draws `icon` fitted into `area` (stroked or filled as the icon needs). */
void drawIcon (juce::Graphics&, Icon, juce::Rectangle<float> area, juce::Colour);

} // namespace livedaw::ui
