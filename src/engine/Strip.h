#pragma once

#include "engine/Parameters.h"
#include "engine/RustStrings.h"

#include <juce_graphics/juce_graphics.h>

namespace livedaw
{

/** The WING's strip types. */
enum class StripKind
{
    Channel = ld::kChannel, // input channel (40 on the console, more here if the CPU allows)
    Aux = ld::kAux,         // aux input channel (8)
    Bus = ld::kBus,         // stereo mix bus (16, more can be added)
    Main = ld::kMain,       // stereo main bus (4)
    Matrix = ld::kMatrix    // stereo matrix bus (8)
};

constexpr int kNumStripKinds = 5;

/** Name used in show files ("ch", "aux", "bus", "main", "mtx"). */
const char* toString (StripKind kind) noexcept;
StripKind stripKindFromString (const juce::String& s) noexcept;
/** Scribble strip label ("CH", "AUX", "BUS", "MAIN", "MTX"). */
const char* label (StripKind kind) noexcept;

inline bool isInputKind (StripKind k) noexcept { return k == StripKind::Channel || k == StripKind::Aux; }

/** The WING colour palette (index 1..18). */
juce::Colour wingColour (int index) noexcept;
juce::String wingColourName (int index);
/** ICON/COLOR's colour grid: the colour index at place 0..17 (row by row, three to a row). */
inline int wingColourAtGrid (int position) noexcept { return ld::ld_colour_at_grid (position); }

/** A strip's sends to every bus slot (storage in the Rust engine, grown on demand). */
class SendBank
{
public:
    explicit SendBank (ld::Strip* s) : strip (s) {}

    /** Message thread: the send for `slot`, allocated if needed. */
    SendParams& operator[] (size_t slot) const
    {
        auto* p = ld::ld_strip_send (strip, static_cast<int32_t> (slot));
        jassert (p != nullptr);
        return *p;
    }

    /** Any thread: the send for `slot` if it was ever configured, else nullptr. */
    SendParams* find (int slot) const noexcept { return ld::ld_strip_find_send (strip, slot); }

private:
    ld::Strip* strip;
};

/** Blocks that hold a model (see ld_strip_set_model). */
enum class ModelBlock : int32_t
{
    Gate = 0,
    Eq = 1,
    Dynamics = 2,
    FilterTool = 3
};

/** Where a strip's peak meters measure (Strip::peakMeter). */
enum class MeterPoint
{
    Input,    // the raw input (before trim)
    Trim,     // after trim, balance and polarity (the INPUT screen's "post TRIM" meter)
    PreFader, // at the PFL point (the METERS screen; "PRE" meter setting)
    Output    // after the fader and pan ("POST" meter setting)
};

/** C++ view of one strip of the Rust engine, processed as on the WING:
      input channels: INPUT -> FILTER -> GATE, EQ, DYNAMICS and INS 1 in the
        selected process order -> fader, DCAs, mute groups -> INS 2 (or automix)
        -> DELAY (POST) -> pan/width;
      aux channels: INPUT -> delay -> DYNAMICS -> EQ -> INS 1 -> fader (no INS 2);
      buses, mains, matrices: INPUT -> DYNAMICS -> INS 1 -> fader -> EQ -> INS 2
        -> delay -> pan/width;
    then sends to buses, mains and matrices. Aux, buses, mains and matrices tap
    TAP-mode sends after INS 1 (pre-fader).

    Stateless: parameters, meters and the name live in the engine, so a Strip
    can be recreated for the same handle at any time. Message thread only (the
    audio thread is in Rust). */
class Strip
{
public:
    Strip (ld::Engine* engine, ld::Strip* handle);

    StripKind getKind() const noexcept { return kind; }
    bool isInput() const noexcept { return isInputKind (kind); }
    bool isOutput() const noexcept { return ! isInput(); }
    bool isBus() const noexcept { return kind == StripKind::Bus; }

    /** Channels: a stable id. Aux, buses, mains and matrices: their 0-based slot. */
    int getId() const noexcept { return id; }

    /** Position-based number shown on the scribble strip (1-based, set by MixerEngine). */
    int getNumber() const noexcept { return number; }
    void setNumber (int n) noexcept { number = n; }
    /** "CH 3", "BUS 12", ... */
    juce::String getLabel() const { return juce::String (livedaw::label (kind)) + " " + juce::String (number); }

    ld::Strip* getHandle() const noexcept { return handle; }
    /** The engine the strip belongs to (its analysis: RTA, scope, LISTEN). */
    ld::Engine* getEngineHandle() const noexcept { return engineHandle; }

    //==============================================================================
    // Customization: name, colour and icon. With LINK CUSTOMIZATION TO SOURCE
    // on (input and aux channels, params.custLink) the strip shows those of
    // the source in use (MAIN or ALT); without a source, and with the link
    // off, its own. The setters change the strip's own.

    /** The name the strip shows (its source's while linked). */
    juce::String getName() const;
    /** The strip's own name. */
    juce::String getOwnName() const { return takeString (ld::ld_strip_name (handle)); }
    void setName (const juce::String& newName) const
    {
        const Utf8 u (newName);
        ld::ld_strip_set_name (handle, u.data, u.size);
    }

    /** The colour the strip shows (1..18; its source's while linked). */
    int getColourIndex() const noexcept;
    void setColourIndex (int index) const noexcept { params.colour.store (juce::jlimit (1, 18, index)); }
    juce::Colour getColour() const noexcept { return wingColour (getColourIndex()); }

    /** The WING icon the strip shows (0 = none; its source's while linked). */
    int getIcon() const noexcept;

    /** The source table index whose customization the strip shows: linked,
        an input kind and a source in use; else -1. */
    int linkedSourceIndex() const noexcept;
    ld::SourceParams* linkedSource() const noexcept;

    /** Selects a model for a block and loads its defaults. */
    void setModel (ModelBlock block, int model) const noexcept
    {
        ld::ld_strip_set_model (handle, static_cast<int32_t> (block), model);
    }

    /** The model table of this strip's EQ (1 = channel EQs, 2 = bus EQs). */
    int getEqTable() const noexcept { return isOutput() ? 2 : 1; }

    // What the WING gives each strip type (Operation Manual, chapter 6).
    /** INS 2 (the post insert): every type but aux. */
    bool hasPostInsert() const noexcept { return kind != StripKind::Aux; }
    /** SEL DBL CLICK = BUSFX (manual 5.5 p. 48): "opens the INS1 ... screen on
        Buses, Mains and Matrices (or INS2 if INS1 is unused)"; input and aux
        channels open Channel Home. 1 = INS 1, 2 = INS 2, 0 = Channel Home. */
    int busFxInsert() const noexcept
    {
        if (kind != StripKind::Bus && kind != StripKind::Main && kind != StripKind::Matrix)
            return 0;
        return params.preInsert.fx.load() < 0 ? 2 : 1;
    }
    /** Automix X/Y (in INS 2): the input channels only. */
    bool hasAutomix() const noexcept { return kind == StripKind::Channel; }
    /** The automix group the strip is a member of (0 X, 1 Y; -1 none): INS 2
        set to AUTO X / AUTO Y and ON, as the engine shares gain (`Strip::automix_group`). */
    int automixGroup() const noexcept
    {
        const int mode = params.postInsert.mode.load();
        return hasAutomix() && params.postInsert.on.load() && (mode == 1 || mode == 2) ? mode - 1 : -1;
    }
    /** A selectable TAP point and process order: input channels only (the
        others tap after INS 1, pre-fader, and have a fixed order). */
    bool hasSelectableTap() const noexcept { return kind == StripKind::Channel; }
    /** Buses, mains and matrices run their EQ after the fader. */
    bool hasPostFaderEq() const noexcept { return isOutput(); }
    /** The delay is post-fader everywhere but on aux channels. */
    bool hasPostFaderDelay() const noexcept { return kind != StripKind::Aux; }
    /** IGNORE CHANNEL MUTE applies to this strip's TAP-mode sends (input and aux channels). */
    bool canIgnoreMuteOnSends() const noexcept { return isInput(); }

    /** A peak meter (channel 0/1): "max since last read", so exactly one reader
        (the UI's meter bank) exchanges it with 0. */
    std::atomic<float>& peakMeter (MeterPoint point, int channel) const noexcept
    {
        const auto ch = (size_t) juce::jlimit (0, 1, channel);
        switch (point)
        {
            case MeterPoint::Trim:     return meters.trimPeak[ch];
            case MeterPoint::PreFader: return meters.pflPeak[ch];
            case MeterPoint::Output:   return meters.outputPeak[ch];
            case MeterPoint::Input:    break;
        }
        return meters.inputPeak[ch];
    }

    //==============================================================================
    // Input sources (input and aux channels). A source is a hardware input
    // (0-based) or a recorder playback channel (Recorder::playbackSource (n)).

    /** The MAIN input (-1 = none; the right one is used when stereo, -1 = left + 1). */
    int getInputLeft() const noexcept { return params.inputLeft.load(); }
    int getInputRight() const noexcept { return params.inputRight.load(); }

    /** The ALT input (WING INPUT SELECT): -1 = none. */
    int getAltInputLeft() const noexcept { return params.altInputLeft.load(); }
    int getAltInputRight() const noexcept { return params.altInputRight.load(); }
    bool hasAltInput() const noexcept { return isInput() && getAltInputLeft() >= 0; }
    void setAltInput (int left, int right = -1) const noexcept
    {
        params.altInputLeft.store (left < 0 ? -1 : left);
        params.altInputRight.store (right < 0 ? -1 : right);
    }

    /** The strip's own MAIN / ALT switch (it decides unless INPUT SELECT is global). */
    bool isAltSelected() const noexcept { return params.altSelected.load(); }
    void setAltSelected (bool alt) const noexcept { params.altSelected.store (alt); }

    /** True if the strip listens to its ALT input now: it has one and ALT is
        selected (MixerEngine::inputSelectGlobal ? globalAlt : isAltSelected()). */
    bool isUsingAltInput() const noexcept
    {
        if (! hasAltInput())
            return false;
        if (controls != nullptr && controls->inputSelectGlobal.load())
            return controls->globalAlt.load();
        return isAltSelected();
    }

    /** The source in use (MAIN or ALT). */
    int getActiveInputLeft() const noexcept { return isUsingAltInput() ? getAltInputLeft() : getInputLeft(); }
    int getActiveInputRight() const noexcept { return isUsingAltInput() ? getAltInputRight() : getInputRight(); }

    //==============================================================================
    // Console level (SOLO MODE, MUTEGROUP/SIP OVERRIDE, talkback)

    /** MUTE as the surface shows it (ld::MuteState): kMuteGroup = muted by a
        mute group, DCA or SIP (the key flashes). Updated every audio block. */
    int getMuteState() const noexcept { return ld::ld_strip_mute_state (handle); }
    /** Soloed in the last block: own solo, a soloed DCA or a TALK SOLO tag. */
    bool isSoloedNow() const noexcept { return ld::ld_strip_is_soloed (handle); }
    /** HOME > TAGS talk tags (ld::TalkTag bits). */
    uint32_t getTalkTags() const noexcept { return ld::ld_strip_talk_tags (handle); }
    void setTalkTags (uint32_t tags) const noexcept { ld::ld_strip_set_talk_tags (handle, tags); }

    StripParams& params;
    SendBank sends;
    ld::StripMeters& meters;

    int getNumChannels() const noexcept { return ld::ld_strip_num_channels (handle); }
    /** Live gain of band 0-1 of the GATE / DYNAMICS slot's dynamic EQ (dB, signed). */
    float getDynBandGain (bool gate, int band) const noexcept { return ld::ld_strip_dyn_band_gain (handle, gate, band); }
    /** The plugin latency of the strip's output in samples (its inserts plus
        what delay compensation aligned its sources to). */
    int getLatency() const noexcept { return ld::ld_strip_latency (handle); }
    bool isIdle() const noexcept { return ld::ld_strip_is_idle (handle); }

private:
    const ld::EngineControls* controls;
    ld::SourceParams* sources = nullptr;
    int numSources = 0;
    ld::Engine* engineHandle;
    ld::Strip* handle;
    StripKind kind;
    int id;
    int number = 1;

    JUCE_DECLARE_NON_COPYABLE (Strip)
};

} // namespace livedaw
