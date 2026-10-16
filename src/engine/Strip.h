#pragma once

#include "engine/Parameters.h"
#include "engine/PluginChain.h"
#include "engine/RustStrings.h"

#include <juce_graphics/juce_graphics.h>

namespace livedaw
{

enum class StripKind
{
    Input = ld::kInput,  // fed by hardware inputs
    Aux = ld::kAux,      // fed by channel sends, usually sent to a hardware output
    Group = ld::kGroup,  // fed by channel sends, usually routed to the master
    Master = ld::kMaster // main L/R
};

const char* toString (StripKind kind) noexcept;
StripKind stripKindFromString (const juce::String& s) noexcept;

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

/** C++ view of one console strip of the Rust engine: trim -> polarity -> HPF ->
    gate -> 4-band EQ -> compressor -> inserts -> mute -> fader -> pan.

    Stateless: parameters, meters, name, colour and plugin slots all live in the
    engine, so a Strip can be recreated for the same handle at any time.
    Message thread only (the audio thread is in Rust). */
class Strip
{
public:
    Strip (ld::Engine* engine, ld::Strip* handle);

    StripKind getKind() const noexcept { return kind; }
    bool isBus() const noexcept { return kind == StripKind::Aux || kind == StripKind::Group; }

    /** Inputs: a stable id. Buses: the bus slot sends refer to. Master: 0. */
    int getId() const noexcept { return id; }

    ld::Strip* getHandle() const noexcept { return handle; }

    juce::String getName() const { return takeString (ld::ld_strip_name (handle)); }
    void setName (const juce::String& newName) const
    {
        const Utf8 u (newName);
        ld::ld_strip_set_name (handle, u.data, u.size);
    }

    juce::Colour getColour() const noexcept { return juce::Colour (ld::ld_strip_colour (handle)); }
    void setColour (juce::Colour c) const noexcept { ld::ld_strip_set_colour (handle, c.getARGB()); }

    StripParams& params;
    SendBank sends;
    PluginChain inserts;

    // Meters (peaks are "max since last read": exchange with 0 to read)
    std::atomic<float> (&inputPeak)[2];
    std::atomic<float> (&outputPeak)[2];
    std::atomic<float>& gateGain;
    std::atomic<float>& compReductionDb;
    std::atomic<int32_t>& currentWidth;
    std::atomic<int32_t>& dspFaults;

    int getNumChannels() const noexcept { return ld::ld_strip_num_channels (handle); }
    bool isIdle() const noexcept { return ld::ld_strip_is_idle (handle); }

private:
    ld::Strip* handle;
    StripKind kind;
    int id;
    ld::StripMeters& meters;

    JUCE_DECLARE_NON_COPYABLE (Strip)
};

} // namespace livedaw
