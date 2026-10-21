#pragma once

#include "livedaw_engine.h"

#include <juce_data_structures/juce_data_structures.h>
#include <juce_graphics/juce_graphics.h>

#include <vector>

namespace livedaw
{

/** The web remote's server: browsers on the local network open
    http://<this computer>:<port>/ and get the screen mirror (the page, then a
    WebSocket carrying tiles of the window one way and the browser's input
    the other). The server runs on background threads in the Rust engine
    (livedaw-web); the window side is ui::WebMirror.

    Start, stop and status are message thread only; the screen and input
    calls may come from one other thread at a time (the tile encoder). */
class WebRemote final
{
public:
    WebRemote();
    ~WebRemote();

    /** The WEB REMOTE settings (SETUP > REMOTE), kept in the app settings. */
    struct Settings
    {
        bool enabled = false;
        int port = 8080;
        /** 4-8 digits; empty = no PIN. */
        juce::String pin;
        /** Browsers watch, their input is ignored. */
        bool viewOnly = false;

        static bool isValidPin (const juce::String&);
        static Settings load (juce::PropertiesFile&);
        void save (juce::PropertiesFile&) const;
        bool operator== (const Settings&) const;
        bool operator!= (const Settings& o) const { return ! operator== (o); }
    };

    /** Starts, restarts or stops the server to match `settings`. Returns the
        error if the port could not be opened. */
    juce::String apply (const Settings& settings);
    void stop();
    bool isRunning() const;

    struct Client
    {
        juce::String address;
        bool watching = false; // has entered the PIN (or none is needed)
        int seconds = 0;
    };

    struct Status
    {
        bool running = false;
        int port = -1; // -1: not listening
        juce::String error;
        std::vector<Client> clients;
        juce::int64 bytesSent = 0, framesSent = 0;

        int numWatching() const;
    };

    Status getStatus() const;

    /** The addresses to open in a browser: http://<IPv4>:<port>/ for each
        network interface (loopback last). */
    static juce::StringArray getUrls (int port);

    //==============================================================================
    // The screen mirror (see ld_web_*)

    /** Whether any browser watches: only then is the window snapshotted. */
    bool isWatched() const;
    /** A new screen size in pixels: all tiles unknown. Returns the generation
        `putTile` quotes. */
    juce::uint32 resetScreen (int width, int height, int tileSize);
    /** The candidate tiles whose pixels changed since they were last seen. */
    std::vector<juce::uint32> changedTiles (const juce::Image::BitmapData&, const std::vector<juce::uint32>& candidates);
    enum TileFormat
    {
        png = 0,
        jpeg = 1
    };
    bool putTile (juce::uint32 generation, juce::uint32 index, TileFormat, const juce::MemoryBlock& data);
    /** Sends the tiles put since the last call. */
    void publish();
    bool takeInput (ld::LdWebInput& event);

    /** Special keys in input events (characters are positive). */
    enum Key
    {
        keyEnter = -1,
        keyEscape = -2,
        keyBackspace = -3,
        keyTab = -4,
        keyDelete = -5,
        keyLeft = -6,
        keyRight = -7,
        keyUp = -8,
        keyDown = -9,
        keyHome = -10,
        keyEnd = -11,
        keyPageUp = -12,
        keyPageDown = -13,
        keyInsert = -14,
        keyF1 = -21, // F1..F12: -21..-32
        keyF12 = -32
    };

private:
    ld::LdWeb* handle;
    Settings running;

    JUCE_DECLARE_NON_COPYABLE (WebRemote)
};

} // namespace livedaw
