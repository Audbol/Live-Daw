#pragma once

#include "dsp/Compressor.h"
#include "dsp/NoiseGate.h"
#include "dsp/ParametricEq.h"

#include <array>
#include <atomic>

#include <juce_core/juce_core.h>

namespace livedaw
{

/** Upper bound on bus slots. Send storage grows in pages as buses are added, so
    this ceiling costs nothing until it's used (2 KB of page pointers per strip);
    in practice the number of buses is limited by CPU, like the number of channels. */
constexpr int kMaxBuses = 16384;

/** Number of insert slots per strip for VST/AU/LV2 plugins. */
constexpr int kNumInsertSlots = 8;

using AF = std::atomic<float>;
using AB = std::atomic<bool>;
using AI = std::atomic<int>;

/** All parameters are atomics: the UI thread writes them, the audio thread reads
    a snapshot at the start of every block. No locks are involved. */
struct GateParams
{
    AB enabled { false };
    AF thresholdDb { -50.0f }, rangeDb { -80.0f }, attackMs { 0.5f }, holdMs { 50.0f }, releaseMs { 150.0f };

    dsp::NoiseGate::Settings snapshot() const noexcept
    {
        dsp::NoiseGate::Settings s;
        s.enabled = enabled.load (std::memory_order_relaxed);
        s.thresholdDb = thresholdDb.load (std::memory_order_relaxed);
        s.rangeDb = rangeDb.load (std::memory_order_relaxed);
        s.attackMs = attackMs.load (std::memory_order_relaxed);
        s.holdMs = holdMs.load (std::memory_order_relaxed);
        s.releaseMs = releaseMs.load (std::memory_order_relaxed);
        return s;
    }
};

struct CompParams
{
    AB enabled { false };
    AF thresholdDb { -20.0f }, ratio { 4.0f }, attackMs { 10.0f }, releaseMs { 120.0f }, kneeDb { 6.0f }, makeupDb { 0.0f };

    dsp::Compressor::Settings snapshot() const noexcept
    {
        dsp::Compressor::Settings s;
        s.enabled = enabled.load (std::memory_order_relaxed);
        s.thresholdDb = thresholdDb.load (std::memory_order_relaxed);
        s.ratio = ratio.load (std::memory_order_relaxed);
        s.attackMs = attackMs.load (std::memory_order_relaxed);
        s.releaseMs = releaseMs.load (std::memory_order_relaxed);
        s.kneeDb = kneeDb.load (std::memory_order_relaxed);
        s.makeupDb = makeupDb.load (std::memory_order_relaxed);
        return s;
    }
};

struct EqBandParams
{
    AI type { 0 };
    AF frequency { 1000.0f }, gainDb { 0.0f }, q { 0.707f };
    AB enabled { true };
};

struct EqParams
{
    AB enabled { true };
    std::array<EqBandParams, dsp::ParametricEq::kNumBands> bands;

    EqParams()
    {
        const auto defaults = dsp::ParametricEq::Settings::defaultBands();
        for (size_t i = 0; i < bands.size(); ++i)
        {
            bands[i].type = static_cast<int> (defaults[i].type);
            bands[i].frequency = defaults[i].frequency;
            bands[i].gainDb = defaults[i].gainDb;
            bands[i].q = defaults[i].q;
            bands[i].enabled = defaults[i].enabled;
        }
    }

    dsp::ParametricEq::Settings snapshot() const noexcept
    {
        dsp::ParametricEq::Settings s;
        s.enabled = enabled.load (std::memory_order_relaxed);
        for (size_t i = 0; i < bands.size(); ++i)
        {
            auto& b = s.bands[i];
            b.type = static_cast<dsp::ParametricEq::BandType> (bands[i].type.load (std::memory_order_relaxed));
            b.frequency = bands[i].frequency.load (std::memory_order_relaxed);
            b.gainDb = bands[i].gainDb.load (std::memory_order_relaxed);
            b.q = bands[i].q.load (std::memory_order_relaxed);
            b.enabled = bands[i].enabled.load (std::memory_order_relaxed);
        }
        return s;
    }
};

struct SendParams
{
    AB enabled { false };
    AF levelDb { 0.0f };
    AB preFader { true };

    /** Gain ramp state, owned by the audio task that sums the destination bus. */
    float lastGain = 0.0f;
};

/** A strip's sends to every bus slot, allocated in pages as buses are created.

    Pages are only ever added (by the message thread) and are freed with the strip,
    so the audio thread can read them lock-free: a slot whose page doesn't exist
    yet simply has no send. */
class SendBank
{
public:
    static constexpr int kPageSize = 64;
    static constexpr int kNumPages = (kMaxBuses + kPageSize - 1) / kPageSize;

    SendBank()
    {
        for (auto& p : pages)
            p.store (nullptr, std::memory_order_relaxed);
    }

    ~SendBank()
    {
        for (auto& p : pages)
            delete p.load();
    }

    SendBank (const SendBank&) = delete;
    SendBank& operator= (const SendBank&) = delete;

    /** Audio thread (or any thread): the send for `slot`, or nullptr if it was never touched. */
    SendParams* find (int slot) const noexcept
    {
        if (slot < 0 || slot >= kMaxBuses)
            return nullptr;

        auto* page = pages[(size_t) (slot / kPageSize)].load (std::memory_order_acquire);
        return page != nullptr ? &page->sends[(size_t) (slot % kPageSize)] : nullptr;
    }

    /** Message thread: the send for `slot`, allocating its page if needed. */
    SendParams& operator[] (size_t slot)
    {
        jassert (slot < (size_t) kMaxBuses);
        auto& pagePtr = pages[slot / kPageSize];
        auto* page = pagePtr.load (std::memory_order_acquire);

        if (page == nullptr)
        {
            page = new Page();
            pagePtr.store (page, std::memory_order_release);
        }

        return page->sends[slot % kPageSize];
    }

private:
    struct Page
    {
        std::array<SendParams, kPageSize> sends;
    };

    std::array<std::atomic<Page*>, kNumPages> pages;
};

struct StripParams
{
    // Input stage
    AF trimDb { 0.0f };
    AB polarityInvert { false };
    AB hpfEnabled { false };
    AF hpfFrequency { 80.0f };

    // Processing
    GateParams gate;
    EqParams eq;
    CompParams comp;

    // Output stage
    AF faderDb { 0.0f };
    AF pan { 0.0f };
    AB mute { false };
    AB solo { false };
    AB mainAssign { true };

    // Channel width: mono or stereo (input channels and buses; the master is always stereo).
    AB stereo { true };

    // Input routing (input strips): hardware input channel numbers, -1 = none.
    // inputRight is only used when the channel is stereo.
    AI inputLeft { -1 }, inputRight { -1 };

    // Direct hardware outputs (buses, master and input direct outs), -1 = none.
    AI outputLeft { -1 }, outputRight { -1 };

    // Sends from this strip to each bus slot (input strips only).
    SendBank sends;

    // Scene recalls skip this strip entirely (snippets still apply).
    AB recallSafe { false };
};

} // namespace livedaw
