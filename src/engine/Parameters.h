#pragma once

// Strip parameters are owned by the Rust engine; these are its C++ views
// (see rust/crates/livedaw-ffi/include/livedaw_engine.h). The UI writes the
// atomics directly; the audio thread reads them once per block.

#include "livedaw_engine.h"

namespace livedaw
{

/** Upper bound on bus slots (send storage grows on demand; CPU is the real limit). */
constexpr int kMaxBuses = ld::kMaxBuses;

constexpr int kNumMains = ld::kNumMains;
constexpr int kNumMatrices = ld::kNumMatrices;
constexpr int kNumAux = ld::kNumAux;
constexpr int kNumDcas = ld::kNumDcas;
constexpr int kNumMuteGroups = ld::kNumMuteGroups;
constexpr int kNumFx = ld::kNumFx;
constexpr int kWingChannels = ld::kWingChannels;
constexpr int kWingBuses = ld::kWingBuses;
/** Buses 9+ can feed buses 1-8. */
constexpr int kBusToBusTargets = 8;
/** The filter frequencies of a new or INITed channel (the engine's defaults),
    which the LO CUT / HI CUT controls reset to: 100 Hz and 10 kHz on the
    console's steps (100.2 Hz, 10k02 Hz). */
constexpr float kLowCutDefaultHz = 100.2374f;
constexpr float kHighCutDefaultHz = 10018.26f;
/** The key filters' and the crossover's FREQ of a new channel (1k0 Hz on the
    console's steps), which their controls reset to. */
constexpr float kKeyFilterDefaultHz = 1002.374f;

using InputParams = ld::InputParams;
using FilterParams = ld::FilterParams;
using DynSlotParams = ld::DynSlotParams;
using EqBlockParams = ld::EqBlockParams;
using InsertParams = ld::InsertParams;
using MainSendParams = ld::MainSendParams;
using MatrixSendParams = ld::MatrixSendParams;
using StripParams = ld::StripParams;
using SendParams = ld::SendParams;
using DcaParams = ld::DcaParams;
using FxParams = ld::FxParams;

/** Bus send modes (list 4). Pre is the WING's TAP mode on input and aux
    channels (taken at the tap point; cut by the channel mute unless the send's
    ignoreMute is set) and pre-fader on buses. Group is post-fader and post-pan
    with no send level. */
enum class SendMode : int32_t
{
    Pre = 0,
    Post = 1,
    Group = 2
};

/** MAIN LINK (EngineControls::mainLink, list 12). */
enum class MainLink : int32_t
{
    Off = ld::kMainLinkOff,
    To2 = ld::kMainLink2,    // 1>2
    To3 = ld::kMainLink2_3,  // 1>2-3
    To4 = ld::kMainLink2_4   // 1>2-4
};

/** Post insert modes (list 6). */
enum class PostInsertMode : int32_t
{
    Fx = 0,
    AutomixX = 1,
    AutomixY = 2
};

} // namespace livedaw
