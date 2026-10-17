#pragma once

#include "engine/Strip.h"

#include <array>

namespace livedaw
{

/** C++ view of the engine's analysis (rust/crates/livedaw-engine/src/analyser.rs):
    the RTA and the LR vector scope of the analysed strip, and LISTEN (BAND
    SOLO, KEY SOLO, XOVER SOLO), which replaces the solo bus for one strip at
    a time. Stateless like Strip: the settings are the engine's shared
    atomics, so a view can be made from any Strip (or the engine) at any time.
    Message thread. */
class Analysis
{
public:
    explicit Analysis (ld::Engine* e) : engine (e), controls (*ld::ld_engine_analysis (e)) {}
    explicit Analysis (const Strip& s) : Analysis (s.getEngineHandle()) {}

    static constexpr int numBands = ld::kRtaBands;
    /** Centre frequency of an RTA band: a piano key, E0 (20.6 Hz) .. D#10. */
    static float bandHz (int band) noexcept { return ld::ld_rta_band_hz (band); }
    /** The band's edges: half a semitone either side of its centre. */
    static float bandLowHz (int band) noexcept { return bandHz (band) * 0.9715319f; }
    static float bandHighHz (int band) noexcept { return bandHz (band) * 1.0293022f; }

    //==============================================================================
    // The analysed strip (the selected one) and the RTA

    void setAnalysedStrip (const Strip* s) noexcept
    {
        controls.stripKind = s != nullptr ? static_cast<int32_t> (s->getKind()) : -1;
        controls.stripId = s != nullptr ? s->getId() : -1;
    }
    bool isAnalysed (const Strip& s) const noexcept
    {
        return controls.stripKind.load() == static_cast<int32_t> (s.getKind()) && controls.stripId.load() == s.getId();
    }

    /** Shows the RTA of `s` in a view (ld::RtaView), or none (nullptr). */
    void showRta (const Strip* s, int view) noexcept
    {
        setAnalysedStrip (s);
        controls.view = view;
        controls.rtaOn = s != nullptr;
    }
    void hideRta() noexcept { controls.rtaOn = false; }

    bool isFrozen() const noexcept { return controls.rtaFreeze.load(); }
    void setFrozen (bool f) noexcept { controls.rtaFreeze = f; }

    /** The settings of a view: HOME (RTA SETTINGS) or EQ / FILTER (EQ/RTA SETTINGS). */
    ld::RtaViewSettings& settings (int view) const noexcept { return view == ld::kRtaViewHome ? controls.home : controls.eq; }

    struct Bands
    {
        std::array<float, ld::kRtaBands> levels {}, peaks {};
        uint32_t frame = 0;
    };
    /** The latest bands (dB; a sine of amplitude 1 reads 0 dB in its band). */
    void read (Bands& b) const noexcept { ld::ld_rta_read (engine, b.levels.data(), b.peaks.data(), ld::kRtaBands, &b.frame); }

    //==============================================================================
    // LR VECTOR SCOPE (the analysed strip, after its INPUT block, before pan)

    void showScope (const Strip* s) noexcept
    {
        setAnalysedStrip (s);
        controls.scopeOn = s != nullptr;
    }
    void hideScope() noexcept { controls.scopeOn = false; }
    /** The newest L/R pairs (interleaved); returns how many. */
    int readScope (float* lr, int maxPairs) const noexcept { return ld::ld_scope_read (engine, lr, maxPairs); }

    //==============================================================================
    // LISTEN

    bool isListening (const Strip& s, int mode) const noexcept
    {
        return controls.listenMode.load() == mode && controls.listenKind.load() == static_cast<int32_t> (s.getKind())
               && controls.listenId.load() == s.getId();
    }
    bool isListening() const noexcept { return controls.listenMode.load() != ld::kListenOff; }

    /** Plays `mode` (ld::ListenMode) of `s` on the solo bus instead of the
        soloed strips; BAND SOLO plays band `band` of the WING EQ. */
    void listen (const Strip& s, int mode, int band = 0) noexcept
    {
        controls.listenKind = static_cast<int32_t> (s.getKind());
        controls.listenId = s.getId();
        controls.listenBand = band;
        controls.listenMode = mode;
    }
    void stopListening() noexcept { controls.listenMode = ld::kListenOff; }
    /** LISTEN on, or off if `s` is already playing `mode`; returns whether it's on. */
    bool toggleListen (const Strip& s, int mode, int band = 0) noexcept
    {
        if (isListening (s, mode))
        {
            stopListening();
            return false;
        }
        listen (s, mode, band);
        return true;
    }
    /** BAND SOLO follows the selected band while it plays this strip. */
    void followBand (const Strip& s, int band) noexcept
    {
        if (isListening (s, ld::kListenBand))
            controls.listenBand = band;
    }

    ld::Engine* const engine;
    ld::AnalysisControls& controls;
};

} // namespace livedaw
