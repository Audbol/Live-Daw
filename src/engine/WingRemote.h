#pragma once

#include "livedaw_engine.h"

#include <juce_data_structures/juce_data_structures.h>

#include <functional>
#include <vector>

namespace livedaw
{

class MixerEngine;
class Show;

namespace state
{
struct ApplyResult;
}

/** The WING remote: Sidecar answers the Behringer WING's network protocols
    (discovery on UDP 2222, OSC on UDP 2223, the native protocol on TCP 2222
    with meters over UDP) so WING remote clients can control the console, and
    reads and writes WING snapshot files. The protocols run on background
    threads in the Rust engine (livedaw-wing); this class is message thread only.

    Owned by MixerEngine, which destroys it (stopping the servers) first. */
class WingRemote final : private juce::Timer
{
public:
    explicit WingRemote (MixerEngine&);
    ~WingRemote() override;

    /** The REMOTE settings (SETUP > REMOTE), kept in the app settings. */
    struct Settings
    {
        bool enabled = false;
        juce::String name { "LIVE-DAW" };
        int discoveryPort = 2222, oscPort = 2223, nativePort = 2222;
        bool discovery = true, osc = true, native = true;

        /** The console name is SETUP > GENERAL's CONSOLE NAME (network). */
        static constexpr const char* nameKey = "setup.consoleName";
        static Settings load (juce::PropertiesFile&);
        void save (juce::PropertiesFile&) const;
        bool operator== (const Settings&) const;
        bool operator!= (const Settings& o) const { return ! operator== (o); }
    };

    /** Starts, restarts or stops the servers to match `settings`. Returns the
        servers that could not start (the others run). */
    juce::StringArray apply (const Settings& settings);
    void stop();
    bool isRunning() const;
    /** Renames the console at once (discovery and connected clients). */
    void setName (const juce::String&);

    struct Client
    {
        bool native = false; // else OSC
        juce::String address;
        bool subscribed = false; // native: meters; OSC: holds the event subscription
        int seconds = 0;
    };

    struct Status
    {
        bool running = false;
        int discoveryPort = -1, oscPort = -1, nativePort = -1; // -1: not listening
        juce::StringArray errors;
        std::vector<Client> clients;
        juce::int64 oscMessages = 0, nativeBytes = 0;
    };

    Status getStatus() const;

    /** Applies a WING snapshot (.snap) to the console. `report` gets the summary
        and what was not applied (or why the file could not be read). */
    juce::Result importSnapshot (const juce::File&, juce::String& report);
    /** Writes the console as a WING snapshot (.snap). Experimental. */
    juce::Result exportSnapshot (const juce::File&);
    /** Adds the snapshot scenes of a WING .show file to `show`. */
    juce::Result importShow (Show&, const juce::File&, juce::String& report);

    /** Scene control for clients (/$ctl/lib): publishes `show`'s scenes and runs
        the recalls clients ask for (GO, GO NEXT / PREV, GO TAG), reporting each
        to `onRecalled`. nullptr detaches (detach before the show goes away). */
    void attachShow (Show*, std::function<void (const juce::String& what, const state::ApplyResult&)> onRecalled = {});

    /** What a client changed on the surface (`onSurfaceChanged`'s flags). */
    enum SurfaceChange
    {
        surfaceLayers = 1,  // the show's fader layers (selection, spill, user layers, edited keys)
        surfaceControls = 2 // the show's custom control assignments
    };

    /** Surface control for clients (/$ctl/layer, /$ctl/user): with a show
        attached, the layer and custom control changes clients make are
        applied to it and its layers and custom controls are published (with
        the upper custom control layer `ccBank` returns), 10 times a second.
        `onSurfaceChanged` gets the SurfaceChange flags of what a client
        changed and the custom control layer it chose (-1 none). */
    void setSurfaceCallbacks (std::function<int()> ccBank, std::function<void (int changes, int bank)> onSurfaceChanged);
    /** Runs the surface exchange now (the timer does it with a show attached). */
    void syncSurface();

    /** How much of the WING parameter tree maps onto Sidecar. */
    static juce::String coverageReport();

    ld::LdWing* getHandle() const noexcept { return handle; }

private:
    void timerCallback() override;
    void publishLibrary();

    MixerEngine& engine;
    ld::LdWing* handle;
    Settings running;
    Show* libraryShow = nullptr;
    std::function<void (const juce::String&, const state::ApplyResult&)> onRecalled;
    std::function<int()> surfaceBank;
    std::function<void (int, int)> onSurfaceChanged;

    JUCE_DECLARE_NON_COPYABLE (WingRemote)
};

} // namespace livedaw
