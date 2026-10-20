#pragma once

#include "engine/MixerEngine.h"
#include "engine/Recorder.h"
#include "engine/UsbRecorder.h"

#include <vector>

namespace livedaw::ui::routing
{

/** The WING's port groups as ROUTING's group menus list them (firmware 3.1).
    Sidecar's own ports stand in for the hardware it doesn't have:

    - LOCAL IN / LOCAL OUT: the audio interface's inputs and outputs 1-48
      (LCL 1 ...), 24 input and 8 output tiles like the console, more when
      the interface has more. Tiles past the interface's channels read n/a
      but can still be patched (for a bigger interface).
    - AES50 B / C: interface ports 49-96 and 97-144, numbered as the WING
      remote protocol numbers them (B 1 = port 49). AES50 A is the remote's
      name for ports 1-48, which LOCAL IN already shows, so it reads n/a.
    - OSCILLATOR: OSC 1 / 2. WLIVE PLAY: the multitrack recorder's 64
      playback channels (PLAY 1 ...). USB PLAYER: the USB deck's 4 channels.
    - Outputs: RECORDER taps any signal for the USB recorder. BUS, MAIN,
      MATRIX and MONITOR feed outputs as on the console; USER SIGNAL holds
      the channels' and aux channels' own outputs.

    The other groups (AUX IN, AES/EBU, ST CONNECT, USB AUDIO, DANTE, ...) can
    be selected and show their tiles n/a, as the console shows a missing card. */
enum class Group
{
    LocalIn,
    AuxIn,
    AesIn,
    Oscillator,
    Aes50A,
    Aes50B,
    Aes50C,
    StConnect,
    UsbAudio,
    WlivePlay,
    Dante,
    UsbPlayer,
    Bus,
    Main,
    Matrix,
    UserSignal,
    Monitor,
    FxSend,
    LocalOut,
    AuxOut,
    AesOut,
    WliveRec,
    Recorder
};

/** A group's menu name, the prefix of its tile labels ("LCL 3") and its number of tiles. */
struct GroupInfo
{
    const char* name;
    const char* prefix;
    int size;
};

inline GroupInfo info (Group g)
{
    switch (g)
    {
        case Group::LocalIn:    return { "LOCAL IN", "LCL", 24 };
        case Group::AuxIn:      return { "AUX IN", "AUX", 8 };
        case Group::AesIn:      return { "AES/EBU IN", "AES", 2 };
        case Group::Oscillator: return { "OSCILLATOR", "OSC", ld::kNumOscillators };
        case Group::Aes50A:     return { "AES50 A", "A", 48 };
        case Group::Aes50B:     return { "AES50 B", "B", 48 };
        case Group::Aes50C:     return { "AES50 C", "C", 48 };
        case Group::StConnect:  return { "ST CONNECT", "SC", 32 };
        case Group::UsbAudio:   return { "USB AUDIO", "USB", 48 };
        case Group::WlivePlay:  return { "WLIVE PLAY", "PLAY", Recorder::kNumPlaybackChannels };
        case Group::Dante:      return { "DANTE", "DAN", 64 };
        case Group::UsbPlayer:  return { "USB PLAYER", "PLAYER", UsbRecorder::kNumChannels };
        case Group::Bus:        return { "BUS", "BUS", 32 };
        case Group::Main:       return { "MAIN", "MAIN", 8 };
        case Group::Matrix:     return { "MATRIX", "MTX", 16 };
        case Group::UserSignal: return { "USER SIGNAL", "USR", 24 };
        case Group::Monitor:    return { "MONITOR", "MON", 4 };
        case Group::FxSend:     return { "FX SEND", "FX", 32 };
        case Group::LocalOut:   return { "LOCAL OUT", "LCL", 8 };
        case Group::AuxOut:     return { "AUX OUT", "AUX", 8 };
        case Group::AesOut:     return { "AES/EBU OUT", "AES", 2 };
        case Group::WliveRec:   return { "WLIVE REC", "REC", 64 };
        case Group::Recorder:   return { "RECORDER", "REC", UsbRecorder::kNumChannels };
    }
    return { "", "", 0 };
}

/** Interface ports per group (LOCAL, AES50 B, AES50 C). */
constexpr int kBlock = 48;
/** Output index of the RECORDER group's first port: it taps a signal for the
    USB recorder (UsbRecorder::Tap) instead of patching it. */
constexpr int kRecorderPortBase = 0x20000;
constexpr bool isRecorderPort (int port) noexcept { return port >= kRecorderPortBase && port < kRecorderPortBase + UsbRecorder::kNumChannels; }

/** The tiles group `g` shows: LOCAL IN 24 and LOCAL OUT 8 like the console,
    in rows of 8 up to 48 when the interface has more ports (`numHardware`). */
inline int tileCount (Group g, int numHardware)
{
    const int size = info (g).size;
    if (g == Group::LocalIn || g == Group::LocalOut)
        return juce::jlimit (size, kBlock, (numHardware + 7) / 8 * 8);
    return size;
}

/** The source (a strip's input index) of tile `index` of an input group, or
    -1 when Sidecar has no such source (an n/a tile that can't be patched). */
inline int sourceAt (Group g, int index)
{
    if (index < 0)
        return -1;
    if (g == Group::LocalIn || g == Group::Aes50B || g == Group::Aes50C)
        return index < kBlock ? (g == Group::LocalIn ? 0 : g == Group::Aes50B ? kBlock : 2 * kBlock) + index : -1;
    if (g == Group::Oscillator)
        return index < ld::kNumOscillators ? MixerEngine::oscSource (index) : -1;
    if (g == Group::WlivePlay)
        return index < Recorder::kNumPlaybackChannels ? Recorder::playbackSource (index) : -1;
    if (g == Group::UsbPlayer)
        return index < UsbRecorder::kNumChannels ? UsbRecorder::playerSource (index) : -1;
    return -1;
}

/** The output port of tile `index` of an output group, or -1 (n/a). */
inline int outputAt (Group g, int index)
{
    if (index < 0)
        return -1;
    if (g == Group::LocalOut || g == Group::Aes50B || g == Group::Aes50C)
        return index < kBlock ? (g == Group::LocalOut ? 0 : g == Group::Aes50B ? kBlock : 2 * kBlock) + index : -1;
    if (g == Group::Recorder)
        return index < UsbRecorder::kNumChannels ? kRecorderPortBase + index : -1;
    return -1;
}

/** The input group a source belongs to. */
inline Group groupOfSource (int source)
{
    if (MixerEngine::isOscSource (source))
        return Group::Oscillator;
    if (Recorder::isPlaybackSource (source))
        return Group::WlivePlay;
    if (UsbRecorder::isPlayerSource (source))
        return Group::UsbPlayer;
    return source < kBlock ? Group::LocalIn : source < 2 * kBlock ? Group::Aes50B : Group::Aes50C;
}

/** The number of a source within its group (0-based). */
inline int numberInGroup (int source)
{
    if (const int n = MixerEngine::oscChannelOf (source); n >= 0)
        return n;
    if (const int n = Recorder::playbackChannelOf (source); n >= 0)
        return n;
    if (const int n = UsbRecorder::playerChannelOf (source); n >= 0)
        return n;
    return source % kBlock;
}

/** A source as its tile labels it: "LCL 3", "B 1", "OSC 1", "PLAY 5", "PLAYER 2" (empty for none). */
inline juce::String sourceLabel (int source)
{
    if (source < 0)
        return {};
    return juce::String (info (groupOfSource (source)).prefix) + " " + juce::String (numberInGroup (source) + 1);
}

/** A stereo pair as a channel tile labels it: "LCL 1/2", "PLAY 3/4". */
inline juce::String pairLabel (int left, int right)
{
    return sourceLabel (left) + "/" + juce::String (numberInGroup (right) + 1);
}

/** An output port as its tile numbers it (1-based within its group). */
inline int outputNumber (int port) { return (isRecorderPort (port) ? port - kRecorderPortBase : port % kBlock) + 1; }

/** The output group a port belongs to. */
inline Group groupOfOutput (int port)
{
    if (isRecorderPort (port))
        return Group::Recorder;
    return port < kBlock ? Group::LocalOut : port < 2 * kBlock ? Group::Aes50B : Group::Aes50C;
}

/** A group menu: rows of four tiles (-1 a gap), the fourth column set apart,
    and the rows that get extra space above them. */
struct Menu
{
    std::vector<std::vector<int>> rows;
    std::vector<int> spacedRows;
};

/** The SOURCE GROUP menu: CHANNELS (`forChannels`, which also lists BUS, MAIN
    and MATRIX) or SOURCES; with `monitor` (OUTPUTS) MONITOR and FX SEND follow. */
inline Menu sourceMenu (bool forChannels, bool monitor = false)
{
    auto id = [] (Group g) { return (int) g; };
    Menu m;
    m.rows = { { id (Group::LocalIn), id (Group::AuxIn), id (Group::AesIn), id (Group::Oscillator) },
               { id (Group::Aes50A), id (Group::Aes50B), id (Group::Aes50C), id (Group::StConnect) },
               { id (Group::UsbAudio), id (Group::WlivePlay), id (Group::Dante), id (Group::UsbPlayer) } };
    if (forChannels)
        m.rows.push_back ({ id (Group::Bus), id (Group::Main), id (Group::Matrix), id (Group::UserSignal) });
    else
        m.rows.push_back ({ -1, -1, -1, id (Group::UserSignal) });
    m.spacedRows = { 3 };
    if (monitor)
        m.rows.push_back ({ id (Group::Monitor), id (Group::FxSend), -1, -1 });
    return m;
}

/** The OUTPUT GROUP menu. */
inline Menu outputMenu()
{
    auto id = [] (Group g) { return (int) g; };
    return { { { id (Group::LocalOut), id (Group::AuxOut), id (Group::AesOut), -1 },
               { id (Group::Aes50A), id (Group::Aes50B), id (Group::Aes50C), id (Group::StConnect) },
               { id (Group::UsbAudio), id (Group::WliveRec), id (Group::Dante), id (Group::Recorder) } },
             {} };
}

} // namespace livedaw::ui::routing
