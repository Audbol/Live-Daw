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

/** Bus send modes (list 4). */
enum class SendMode : int32_t
{
    Pre = 0,
    Post = 1,
    Group = 2
};

/** Post insert modes (list 6). */
enum class PostInsertMode : int32_t
{
    Fx = 0,
    AutomixX = 1,
    AutomixY = 2
};

} // namespace livedaw
