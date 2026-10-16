// C++ view of the Live DAW Rust engine (crate livedaw-ffi).
//
// The structs below mirror the #[repr(C)] blocks in livedaw-engine/src/params.rs
// and sends.rs field for field. The Rust side owns the memory; C++ reads and
// writes the atomics directly. std::atomic<float/bool/int> are lock-free and have
// the same size and bit layout as Rust's AtomicU32 (f32 bits) / AtomicBool /
// AtomicI32. ld::verifyLayout() compares both sides at startup.
//
// If you change a struct here, change it in Rust too (and bump LD_ABI_VERSION).

#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>

namespace ld
{

static_assert (sizeof (std::atomic<float>) == 4 && std::atomic<float>::is_always_lock_free);
static_assert (sizeof (std::atomic<bool>) == 1 && std::atomic<bool>::is_always_lock_free);
static_assert (sizeof (std::atomic<int32_t>) == 4 && std::atomic<int32_t>::is_always_lock_free);

constexpr int kAbiVersion = 1;
constexpr int kMaxBuses = 16384;
constexpr int kNumInsertSlots = 8;
constexpr int kNumEqBands = 4;

enum StripKindId : int32_t
{
    kInput = 0,
    kAux = 1,
    kGroup = 2,
    kMaster = 3
};

struct GateParams
{
    std::atomic<bool> enabled;
    std::atomic<float> thresholdDb, rangeDb, attackMs, holdMs, releaseMs;
};

struct CompParams
{
    std::atomic<bool> enabled;
    std::atomic<float> thresholdDb, ratio, attackMs, releaseMs, kneeDb, makeupDb;
};

struct EqBandParams
{
    std::atomic<int32_t> type;
    std::atomic<float> frequency, gainDb, q;
    std::atomic<bool> enabled;
};

struct EqParams
{
    std::atomic<bool> enabled;
    EqBandParams bands[kNumEqBands];
};

struct StripParams
{
    // Input stage
    std::atomic<float> trimDb;
    std::atomic<bool> polarityInvert;
    std::atomic<bool> hpfEnabled;
    std::atomic<float> hpfFrequency;

    // Processing
    GateParams gate;
    EqParams eq;
    CompParams comp;

    // Output stage
    std::atomic<float> faderDb;
    std::atomic<float> pan;
    std::atomic<bool> mute;
    std::atomic<bool> solo;
    std::atomic<bool> mainAssign;

    std::atomic<bool> stereo;
    std::atomic<int32_t> inputLeft, inputRight;
    std::atomic<int32_t> outputLeft, outputRight;

    std::atomic<bool> recallSafe;
};

struct StripMeters
{
    std::atomic<float> inputPeak[2];
    std::atomic<float> outputPeak[2];
    std::atomic<float> gateGain;
    std::atomic<float> compReductionDb;
    std::atomic<int32_t> currentWidth;
    std::atomic<int32_t> dspFaults;
};

struct SendParams
{
    std::atomic<bool> enabled;
    std::atomic<float> levelDb;
    std::atomic<bool> preFader;
    float lastGain_; // owned by the audio thread: do not touch
};

struct EngineControls
{
    std::atomic<bool> soloAfl;
    std::atomic<bool> anySoloActive;
    std::atomic<bool> outputProtection;
    std::atomic<float> monitorLevelDb;
    std::atomic<int32_t> monitorOutputLeft, monitorOutputRight;
    std::atomic<float> outputCeilingDb;
    std::atomic<int32_t> outputClipEvents;
    std::atomic<float> monitorPeak[2];
    std::atomic<float> dspLoad;
    std::atomic<int32_t> engineFaults;
};

struct LayoutInfo
{
    int32_t abiVersion, stripParamsSize, gateOffset, eqOffset, compOffset, faderOffset, stereoOffset,
        outputRightOffset, recallSafeOffset, gateSize, eqSize, compSize, metersSize, metersDspFaultsOffset,
        sendSize, sendPreFaderOffset, controlsSize, controlsMonitorPeakOffset, controlsEngineFaultsOffset;
};

struct EqBandSnapshot
{
    int32_t type;
    float frequency, gainDb, q;
    int32_t enabled;
};

struct EqSnapshot
{
    int32_t enabled;
    EqBandSnapshot bands[kNumEqBands];
};

/// Processes numChannels (1 or 2) channels in place; return 0 on success.
using InsertProcessFn = int32_t (*) (void* ctx, float* const* channels, int32_t numChannels, int32_t numSamples);
/// Called on the message thread once the engine can no longer call process.
using InsertReleaseFn = void (*) (void* ctx);

struct InsertCallbacks
{
    InsertProcessFn process;
    InsertReleaseFn release;
    void* ctx;
};

struct Engine; // opaque
struct Strip;  // opaque

extern "C"
{
    void ld_layout_info (LayoutInfo* out);

    Engine* ld_engine_create (int32_t numWorkers);
    void ld_engine_destroy (Engine*);
    int32_t ld_engine_num_workers (const Engine*);
    void ld_engine_run_worker (const Engine*, int32_t index);
    void ld_engine_shutdown_workers (const Engine*);

    void ld_engine_prepare (const Engine*, double sampleRate, int32_t blockSize,
                            const int32_t* inMap, int32_t numHwInputs, const int32_t* outMap, int32_t numHwOutputs);
    void ld_engine_set_device_running (const Engine*, bool running);
    void ld_engine_process (const Engine*, const float* const* ins, int32_t numIns,
                            float* const* outs, int32_t numOuts, int32_t numSamples);
    double ld_engine_sample_rate (const Engine*);
    int32_t ld_engine_block_size (const Engine*);
    EngineControls* ld_engine_controls (const Engine*);

    void ld_engine_collect (const Engine*);
    void ld_engine_collect_all_now (const Engine*);
    int32_t ld_engine_pending_garbage (const Engine*);

    Strip* ld_engine_add_input (const Engine*, int32_t requestedId);
    void ld_engine_set_num_inputs (const Engine*, int32_t count);
    int32_t ld_engine_num_inputs (const Engine*);
    Strip* ld_engine_input_at (const Engine*, int32_t index);
    Strip* ld_engine_add_bus (const Engine*, int32_t kind, int32_t requestedSlot);
    int32_t ld_engine_num_buses (const Engine*);
    Strip* ld_engine_bus_at (const Engine*, int32_t index);
    Strip* ld_engine_bus_in_slot (const Engine*, int32_t slot);
    Strip* ld_engine_master (const Engine*);
    bool ld_engine_remove_strip (const Engine*, const Strip*);
    void ld_engine_reset (const Engine*);
    int32_t ld_engine_total_dsp_faults (const Engine*);
    int32_t ld_engine_num_faulted_plugins (const Engine*);

    int32_t ld_strip_kind (const Strip*);
    int32_t ld_strip_id (const Strip*);
    StripParams* ld_strip_params (const Strip*);
    StripMeters* ld_strip_meters (const Strip*);
    int32_t ld_strip_num_channels (const Strip*);
    bool ld_strip_is_idle (const Strip*);
    SendParams* ld_strip_send (const Strip*, int32_t slot);
    SendParams* ld_strip_find_send (const Strip*, int32_t slot);

    void ld_strip_set_insert (const Engine*, const Strip*, int32_t slot, const InsertCallbacks* callbacks);
    bool ld_strip_insert_is_bypassed (const Strip*, int32_t slot);
    void ld_strip_insert_set_bypassed (const Strip*, int32_t slot, bool bypassed);
    bool ld_strip_insert_is_faulted (const Strip*, int32_t slot);
    void ld_strip_insert_clear_fault (const Strip*, int32_t slot);
    float ld_strip_insert_load (const Strip*, int32_t slot);

    double ld_eq_response_db (const EqSnapshot* settings, double freq, double sampleRate);
}

/// Returns nullptr if the C++ and Rust layouts agree, else a description of the mismatch.
inline const char* verifyLayout()
{
    LayoutInfo info {};
    ld_layout_info (&info);

    if (info.abiVersion != kAbiVersion)                                        return "ABI version";
    if (info.stripParamsSize != (int) sizeof (StripParams))                    return "StripParams size";
    if (info.gateOffset != (int) offsetof (StripParams, gate))                 return "StripParams.gate";
    if (info.eqOffset != (int) offsetof (StripParams, eq))                     return "StripParams.eq";
    if (info.compOffset != (int) offsetof (StripParams, comp))                 return "StripParams.comp";
    if (info.faderOffset != (int) offsetof (StripParams, faderDb))             return "StripParams.faderDb";
    if (info.stereoOffset != (int) offsetof (StripParams, stereo))             return "StripParams.stereo";
    if (info.outputRightOffset != (int) offsetof (StripParams, outputRight))   return "StripParams.outputRight";
    if (info.recallSafeOffset != (int) offsetof (StripParams, recallSafe))     return "StripParams.recallSafe";
    if (info.gateSize != (int) sizeof (GateParams))                            return "GateParams size";
    if (info.eqSize != (int) sizeof (EqParams))                                return "EqParams size";
    if (info.compSize != (int) sizeof (CompParams))                            return "CompParams size";
    if (info.metersSize != (int) sizeof (StripMeters))                         return "StripMeters size";
    if (info.metersDspFaultsOffset != (int) offsetof (StripMeters, dspFaults)) return "StripMeters.dspFaults";
    if (info.sendSize != (int) sizeof (SendParams))                            return "SendParams size";
    if (info.sendPreFaderOffset != (int) offsetof (SendParams, preFader))      return "SendParams.preFader";
    if (info.controlsSize != (int) sizeof (EngineControls))                    return "EngineControls size";
    if (info.controlsMonitorPeakOffset != (int) offsetof (EngineControls, monitorPeak))   return "EngineControls.monitorPeak";
    if (info.controlsEngineFaultsOffset != (int) offsetof (EngineControls, engineFaults)) return "EngineControls.engineFaults";
    return nullptr;
}

} // namespace ld
