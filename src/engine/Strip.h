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

/** C++ view of one strip of the Rust engine: INPUT -> FILTER -> GATE, EQ,
    DYNAMICS and PRE INSERT in the selected process order -> fader, DCAs, mute
    groups -> POST INSERT -> pan/width -> sends to buses, mains and matrices.

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

    juce::String getName() const { return takeString (ld::ld_strip_name (handle)); }
    void setName (const juce::String& newName) const
    {
        const Utf8 u (newName);
        ld::ld_strip_set_name (handle, u.data, u.size);
    }

    int getColourIndex() const noexcept { return params.colour.load(); }
    void setColourIndex (int index) const noexcept { params.colour.store (juce::jlimit (1, 18, index)); }
    juce::Colour getColour() const noexcept { return wingColour (getColourIndex()); }

    /** Selects a model for a block and loads its defaults. */
    void setModel (ModelBlock block, int model) const noexcept
    {
        ld::ld_strip_set_model (handle, static_cast<int32_t> (block), model);
    }

    /** The model table of this strip's EQ (1 = channel EQs, 2 = bus EQs). */
    int getEqTable() const noexcept { return isOutput() ? 2 : 1; }

    StripParams& params;
    SendBank sends;
    ld::StripMeters& meters;

    int getNumChannels() const noexcept { return ld::ld_strip_num_channels (handle); }
    bool isIdle() const noexcept { return ld::ld_strip_is_idle (handle); }

private:
    ld::Strip* handle;
    StripKind kind;
    int id;
    int number = 1;

    JUCE_DECLARE_NON_COPYABLE (Strip)
};

} // namespace livedaw
