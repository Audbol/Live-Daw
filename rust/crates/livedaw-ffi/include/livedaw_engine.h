// C++ view of the Live DAW Rust engine (crate livedaw-ffi).
//
// The structs below mirror the #[repr(C)] blocks in livedaw-engine/src/params.rs
// and sends.rs field for field. The Rust side owns the memory; C++ reads and
// writes the atomics directly. std::atomic<float/bool/int> are lock-free and have
// the same size and bit layout as Rust's AtomicU32 (f32 bits) / AtomicBool /
// AtomicI32. ld::verifyLayout() compares both sides at startup.
//
// The layout follows the Behringer WING: every strip has an INPUT block, a
// FILTER, a GATE slot and a DYNAMICS slot (each holding any of the 32 dynamics
// models), an EQ (one of the EQ models), PRE and POST inserts (FX slots),
// sends to buses, the 4 mains and the 8 matrices, DCA and mute group masks.
// Model parameters are stored by index in the model's table; ld_model_* below
// describes the tables (names, ranges, units, choices) for the UI.
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

constexpr int kAbiVersion = 8;
constexpr int kMaxBuses = 16384;
constexpr int kNumMains = 4;
constexpr int kNumMatrices = 8;
constexpr int kNumAux = 8;
constexpr int kNumDcas = 16;
constexpr int kNumMuteGroups = 8;
constexpr int kNumFx = 16;
constexpr int kWingChannels = 40;
constexpr int kWingBuses = 16;
constexpr int kMaxDynParams = 20;
constexpr int kMaxEqParams = 32;
constexpr int kMaxToolParams = 2;
constexpr int kMaxFxParams = 36;
/** FX slot models (indices into model table 4): an empty slot, and EXT (a plugin). */
constexpr int kFxNone = 0;
constexpr int kFxExt = 1;
/** The recorder's mono playback channels: input index kPlaybackSourceBase + n
    (in inputLeft/Right, altInputLeft/Right, record ports) is playback channel n. */
constexpr int kNumPlaybackChannels = 64;
constexpr int32_t kPlaybackSourceBase = 0x10000;
constexpr int kMaxRecordTracks = 64;
constexpr int kSamplesPerPeak = 256;
/** TAP EQ bands (low shelf, peak, high shelf). */
constexpr int kTapEqBands = 3;
/** RTA bands: one per semitone, E0 (20.6 Hz) .. D#10 (19.9 kHz), see ld_rta_band_hz. */
constexpr int kRtaBands = 120;

enum StripKindId : int32_t
{
    kChannel = 0,
    kAux = 1,
    kBus = 2,
    kMain = 3,
    kMatrix = 4
};

/// Fader reference kinds beyond the strip kinds (0..4): DCAs and FX parameters.
constexpr int32_t kFaderDca = 5;
constexpr int32_t kFaderFxParam = 6;

/// Tap points (index into list 1). WING names: INPUT, FILTER, TAP 3-5, PRE FDR,
/// POST FDR, POST PROC. Only input channels choose theirs; aux, buses, mains and
/// matrices always tap at kTapPfl (after INS 1, pre-fader).
enum TapPoint : int32_t
{
    kTapIn = 0,
    kTapFilter = 1,
    kTapPfl = 5,
    kTapAfl = 6,
    kTapPost = 7
};

/// MAIN LINK (EngineControls::mainLink, list 12): Main 1's fader also moves Mains 2..N.
enum MainLinkMode : int32_t
{
    kMainLinkOff = 0,
    kMainLink2 = 1,   // 1>2
    kMainLink2_3 = 2, // 1>2-3
    kMainLink2_4 = 3  // 1>2-4
};

struct InputParams
{
    std::atomic<float> trimDb, balanceDb;
    std::atomic<bool> invert, delayOn;
    std::atomic<int32_t> delayMode; // list 5: M, FT, MS, SMP
    std::atomic<float> delay;
};

struct FilterParams
{
    std::atomic<bool> lowCut, highCut, tool;
    std::atomic<float> lowCutFreq;
    std::atomic<int32_t> lowCutSlope; // list 8
    std::atomic<float> highCutFreq;
    std::atomic<int32_t> highCutSlope; // list 9
    std::atomic<int32_t> toolModel;    // table 3
    std::atomic<float> toolParams[kMaxToolParams];
};

struct DynSlotParams
{
    std::atomic<bool> on;
    std::atomic<int32_t> model; // table 0 (use ld_strip_set_model to change it)
    std::atomic<float> values[kMaxDynParams];
    std::atomic<float> mix, gain;
    std::atomic<int32_t> scFilter; // list 2
    std::atomic<float> scFreq, scQ;
    std::atomic<int32_t> scSource, scTap; // source -1 = self, else channel index
    std::atomic<int32_t> xoKind; // list 3
    std::atomic<float> xoFreq, xoQ, xoDepth;
};

struct EqBlockParams
{
    std::atomic<bool> on;
    std::atomic<int32_t> model; // table 1 (channels, aux) or 2 (buses, mains, matrices)
    std::atomic<float> mix;
    std::atomic<float> values[kMaxEqParams];
};

struct InsertParams
{
    std::atomic<bool> on;
    std::atomic<int32_t> fx;   // FX slot, -1 = none
    std::atomic<int32_t> mode; // post insert: list 6 (FX, AUTO_X, AUTO_Y)
    std::atomic<float> weight; // automix weight, dB
};

struct MainSendParams
{
    std::atomic<bool> on, pre; // pre: from the tap point (the strip's mute still cuts it)
    std::atomic<float> levelDb;
};

struct MatrixSendParams
{
    std::atomic<bool> on, pre;
    std::atomic<bool> ignoreMute; // IGNORE CHANNEL MUTE (input/aux channels, pre/TAP mode only)
    std::atomic<float> levelDb, pan;
};

/// TAP EQ (WING /ch/n/peq): band 0 low shelf, 1 peak, 2 high shelf.
struct TapEqBand
{
    std::atomic<float> gainDb; // -15..15
    std::atomic<float> freq;   // 20..20000 Hz
    std::atomic<float> q;      // 0.44..10
};

struct TapEqParams
{
    std::atomic<bool> on;
    TapEqBand bands[kTapEqBands];
};

struct StripParams
{
    InputParams input;
    FilterParams filter;
    DynSlotParams gate;
    EqBlockParams eq;
    DynSlotParams dynamics;
    InsertParams preInsert, postInsert; // aux channels have no post insert (INS 2)
    std::atomic<int32_t> processOrder; // list 0 (input channels only)
    std::atomic<int32_t> tap;          // list 1 (input channels only)

    std::atomic<float> faderDb;
    std::atomic<float> pan;   // -100..100
    std::atomic<float> width; // -150..150 %
    std::atomic<bool> mute, solo, soloSafe;
    std::atomic<int32_t> monitorBus; // list 7

    std::atomic<bool> stereo, mono;
    std::atomic<int32_t> inputLeft, inputRight;
    std::atomic<int32_t> outputLeft, outputRight;

    MainSendParams mains[kNumMains];
    MatrixSendParams matrix[kNumMatrices];

    std::atomic<uint32_t> dcaMask, muteGroupMask;

    // Matrices: direct input from a channel.
    std::atomic<bool> directOn, directInvert, directPre;
    std::atomic<int32_t> directSource;
    std::atomic<float> directLevelDb;

    std::atomic<int32_t> colour; // WING colour 1..18 (ld_colour_rgb)
    std::atomic<int32_t> icon;
    std::atomic<bool> led;
    std::atomic<bool> recallSafe;

    // ALT input (WING INPUT SELECT; input and aux channels): used instead of
    // inputLeft/Right while ALT is selected (-1 = none; right used when stereo).
    std::atomic<int32_t> altInputLeft, altInputRight;
    std::atomic<bool> altSelected; // the strip's MAIN / ALT switch (unless INPUT SELECT is global)

    // ABI 7, input channels: the signal TAP-mode sends take.
    std::atomic<float> tapWidth; // TAP WIDTH, -150..150 % (stereo channels)
    TapEqParams tapEq;           // TAP EQ
};

struct StripMeters
{
    std::atomic<float> inputPeak[2];
    std::atomic<float> outputPeak[2];
    std::atomic<float> gateReductionDb, dynReductionDb;
    std::atomic<bool> gateActive, dynActive;
    std::atomic<int32_t> currentWidth;
    std::atomic<int32_t> dspFaults;
    std::atomic<float> automixDb;
    std::atomic<float> trimPeak[2]; // after trim/balance/polarity ("post TRIM")
    std::atomic<float> pflPeak[2];  // pre-fader, at the PFL point (METERS screen)
};

struct SendParams
{
    std::atomic<bool> enabled, panLink;
    std::atomic<bool> ignoreMute;  // IGNORE CHANNEL MUTE (input/aux channels, PRE/TAP mode only)
    std::atomic<bool> panReversed; // with panLink: LINK REVERSED (mirrored channel pan)
    std::atomic<int32_t> mode; // list 4: PRE (TAP), POST, GRP (GROUP: no send level)
    std::atomic<float> levelDb, pan;
    float lastGain_[2]; // owned by the audio thread: do not touch
};

struct DcaParams
{
    std::atomic<float> faderDb;
    std::atomic<bool> mute, solo;
    std::atomic<int32_t> colour;
};

struct FxParams
{
    std::atomic<bool> bypass;
    std::atomic<float> mix;
    /** Index into the FX table (kFxNone, kFxExt or a built-in device). */
    std::atomic<int32_t> model;
    std::atomic<float> values[kMaxFxParams];
};

// ---- console level (ABI 7) ---------------------------------------------------

/** SOLO MODE (EngineControls::soloMode). */
enum SoloMode : int32_t
{
    kSoloLive = 0,   // each strip's SOLO BUS (monitorBus) chooses the path
    kSoloStudio = 1, // solos go to the selected path (EngineControls::soloBus)
    kSoloSip = 2     // as STUDIO, and inputs neither soloed nor solo safe are muted
};

/** A strip's MUTE as the surface shows it (ld_strip_mute_state). */
enum MuteState : int32_t
{
    kMuteOff = 0,
    kMuteOn = 1,         // its own MUTE
    kMuteGroup = 2,      // muted by a mute group, a DCA mute or SIP (the key flashes)
    kMuteOverridden = 3  // such a mute, unmuted by MUTEGROUP/SIP OVERRIDE
};

/** Talk key SWITCH MODE (TalkPathParams::mode). */
enum TalkMode : int32_t
{
    kTalkAuto = 0, // held = push, a short press latches
    kTalkPush = 1,
    kTalkLatch = 2
};

/** HOME > TAGS talk tags (ld_strip_talk_tags bits). */
enum TalkTag : uint32_t
{
    kTalkSolo = 1u << 0,
    kTalkOn = 1u << 1,
    kTalkASolo = 1u << 2,
    kTalkAOn = 1u << 3,
    kTalkBSolo = 1u << 4,
    kTalkBOn = 1u << 5
};

/** Source pairing (SourceParams::mode). */
enum SourceMode : int32_t
{
    kSourceMono = 0,
    kSourceStereo = 1,
    kSourceMidSide = 2
};

/** Sources: hardware ports 0..kNumHwSources-1, then the playback channels. */
constexpr int kNumHwSources = 576;
constexpr int kNumSources = kNumHwSources + kNumPlaybackChannels;
/** Monitor SOURCE choices: MAIN.1-4, MTX.1-8, BUS.1-16, AUX.1-8. */
constexpr int kNumMonitorSources = kNumMains + kNumMatrices + kWingBuses + kNumAux;
/** TalkParams::assignKind without a talkback channel. */
constexpr int32_t kTalkOff = -1;

/** One talk path (TALK A or TALK B). */
struct TalkPathParams
{
    std::atomic<bool> on;         // the key is on (the host applies AUTO / PUSH / LATCH)
    std::atomic<bool> individual; // USE SEND LEVELS: the talkback channel's send levels
    std::atomic<int32_t> mode;    // TalkMode
    std::atomic<float> monDimDb, busDimDb; // AUTO DIM (attenuations, dB)
    std::atomic<uint32_t> buses, matrices, mains; // TALK TO BUS 1-16 / MATRIX 1-8 / MAIN 1-4 (bit n = n+1)
};

/** Talkback: the talkback channel (kTalkOff, or kChannel / kAux and its 0-based
    number: CH40 = (kChannel, 39), AUX8 = (kAux, 7)), TALK LEVEL and the paths. */
struct TalkParams
{
    std::atomic<int32_t> assignKind, assignIndex;
    std::atomic<float> levelDb;
    TalkPathParams paths[2];
};

/** A monitor path's SOURCE (0..kNumMonitorSources-1), SRC LEVEL, SRC MIX
    (the source's level while a solo reaches the path; -144 = off) and PFL DIM. */
struct MonitorSourceParams
{
    std::atomic<int32_t> source;
    std::atomic<float> sourceLevelDb, sourceMixDb, pflDimDb;
};

/** The PHONES monitor path. The SPEAKERS path is monitorLevelDb, monitorDimDb,
    monitorOutputLeft/Right, monitorPeak plus EngineControls::speakers. */
struct MonitorPathParams
{
    MonitorSourceParams source;
    std::atomic<float> levelDb, dimDb;
    std::atomic<int32_t> outputLeft, outputRight;
    std::atomic<float> peak[2];
};

/** A source (ROUTING > SOURCES): gain, mute, polarity, +48 V (stored only),
    pairing with its neighbour (a pair shares the first source's settings),
    colour and icon, and its peak since the last read. ld_engine_sources. */
struct SourceParams
{
    std::atomic<float> gainDb;
    std::atomic<bool> mute, invert, phantom;
    std::atomic<int32_t> mode; // SourceMode
    std::atomic<int32_t> colour, icon;
    std::atomic<float> peak;
    std::atomic<uint32_t> muteGroups; // ABI 8, SOURCES > TAGS: bit n = mute group n+1 mutes the source
};

// ---- monitor path processing, SOURCE SOLO (ABI 8; livedaw-engine monitor.rs) ----

/** Monitor EQ bands: L (low shelf), 1-6 (PEQ), H (high shelf). */
constexpr int kMonitorEqBands = 8;
/** DIRECT IN choices: CH.1-40, AUX.1-8, BUS.1-16, MAIN.1-4, MTX.1-8 (-1 = OFF). */
constexpr int kNumDirectSources = kWingChannels + kNumAux + kWingBuses + kNumMains + kNumMatrices;

/** One monitor EQ band (the shelves have no Q). */
struct MonitorEqBand
{
    std::atomic<float> gainDb; // -15..15
    std::atomic<float> freq;   // L 20..2000, 1-6 20..20000, H 200..20000 Hz
    std::atomic<float> q;      // 0.44..10
};

/** A monitor path's processing (MONITORS > PHONES / SPEAKERS SETTINGS, the
    MONITORS EQ and LEVEL): solo bus + SOURCE + DIRECT IN -> EQ -> PAN / WIDTH
    -> INVERT -> DELAY -> level, DIM, MUTE, MONO / SWAP -> LIMITER. */
struct MonitorDspParams
{
    std::atomic<int32_t> directSource; // DIRECT IN (0..kNumDirectSources-1, -1 = OFF), ld_monitor_direct_name
    std::atomic<float> directLevelDb;  // its level
    std::atomic<float> bandSoloDb;     // BAND SOLO boost of an EQ band LISTEN on this path, 0..24 (default 12)
    std::atomic<bool> eqOn, invert, delayOn;
    MonitorEqBand eq[kMonitorEqBands];
    std::atomic<float> pan;         // -100..100
    std::atomic<float> width;       // -150..150 %
    std::atomic<float> delayM;      // DIST 0.1..100 m (ld_monitor_delay_ms)
    std::atomic<float> limiterDb;   // LIMITER threshold -40..0 dB
    std::atomic<float> limiterGrDb; // meter: largest gain reduction since the last read (exchange with 0)
};

/** SOURCE SOLO (MonitorControls::sourceSolo): the channel hosting a soloed source. */
enum SourceSoloHost : int32_t
{
    kSourceSoloOff = 0,
    kSourceSoloCh39 = 1,
    kSourceSoloAux7 = 2
};

/** EngineControls::monitor: both paths' processing and SOURCE SOLO. */
struct MonitorControls
{
    MonitorDspParams paths[2];             // 0 = SPEAKERS, 1 = PHONES
    std::atomic<int32_t> sourceSolo;       // SourceSoloHost (SETUP > AUDIO)
    std::atomic<bool> sourceSoloOn;        // SRC SOLO on ROUTING > SOURCES (not saved)
    std::atomic<int32_t> sourceSoloPort;   // the soloed source's input port
};

struct EngineControls
{
    std::atomic<bool> soloAfl; // CHANNEL SOLO (input and aux channels): AFL
    std::atomic<bool> anySoloActive;
    std::atomic<bool> outputProtection;
    std::atomic<float> monitorLevelDb;
    std::atomic<int32_t> monitorOutputLeft, monitorOutputRight;
    std::atomic<float> outputCeilingDb;
    std::atomic<int32_t> outputClipEvents;
    std::atomic<float> monitorPeak[2];
    std::atomic<float> dspLoad;
    std::atomic<int32_t> engineFaults;
    std::atomic<uint32_t> muteGroups;
    DcaParams dcas[kNumDcas];
    FxParams fx[kNumFx];

    std::atomic<bool> busSoloAfl, mainSoloAfl, matrixSoloAfl; // BUS / MAIN / MATRIX SOLO: AFL (default)
    std::atomic<bool> monitorDim, monitorMute, monitorMono, monitorSwap;
    std::atomic<bool> dcaMuteGroups;   // DCA MUTEGROUPS (default on)
    std::atomic<bool> startupMainMute; // STARTUP MAIN MUTE (ld_engine_apply_startup_main_mute)
    std::atomic<bool> automixEnable[2]; // X ENABLE, Y ENABLE
    std::atomic<float> monitorDimDb;    // DIM amount (attenuation in dB, default 20)
    std::atomic<int32_t> mainLink;      // MainLinkMode
    // INPUT SELECT: global = every input strip follows globalAlt, else each
    // strip's altSelected. A strip uses ALT only if it has an ALT source.
    std::atomic<bool> inputSelectGlobal;
    std::atomic<bool> globalAlt;

    // ABI 7: console level
    std::atomic<int32_t> soloMode; // SoloMode
    /** The solo bus / selected monitoring path, as list 7 (0 = A PHONES,
        1 = B SPEAKERS, 2 = A+B): STUDIO and SIP solo there; DIM, MUTE, MONO,
        SWAP and the talk MON DIM act on it. */
    std::atomic<int32_t> soloBus;
    std::atomic<bool> soloBusOverride; // LIVE: soloBus overrides the strips until no solo is left
    std::atomic<bool> muteOverride;    // MUTEGROUP/SIP OVERRIDE
    std::atomic<bool> sipActive;       // status: SIP is muting (the SIP badge)
    TalkParams talk;
    MonitorSourceParams speakers; // the SPEAKERS path's SOURCE etc.
    MonitorPathParams phones;     // the PHONES path

    // ABI 8: monitor path processing and SOURCE SOLO
    MonitorControls monitor;
};

/// Where the RTA shows (AnalysisControls::view): its tap and settings follow.
enum RtaView : int32_t
{
    kRtaViewHome = 0,  // HOME > OVERVIEW: RTA SETTINGS (source IN | EQ | POST)
    kRtaViewEq = 1,    // EQ screen: EQ/RTA SETTINGS (source PRE | POST the EQ)
    kRtaViewFilter = 2 // FILTER SETUP: source PRE | POST the filter (filterTap), the EQ view's settings
};

/// LISTEN (AnalysisControls::listenMode): replaces the solo bus, one strip at a time.
enum ListenMode : int32_t
{
    kListenOff = 0,
    kListenBand = 1,    // BAND SOLO / LISTEN: the selected WING EQ band (listenBand)
    kListenGateKey = 2, // KEY SOLO of the GATE slot (after the key filter)
    kListenDynKey = 3,  // KEY SOLO of the DYNAMICS slot
    kListenXover = 4    // XOVER SOLO: the DYNAMICS slot's crossover band
};

/// RTA SETTINGS of one view (the WING's cfg/rta nodes).
struct RtaViewSettings
{
    std::atomic<int32_t> display;  // SIZE/MODE. HOME: OFF, 1/3, FULL. EQ: OFF, 1/4, 1/3, 1/2, OVL/3, OVL
    std::atomic<int32_t> colour;   // COLOR: RD25, RD50, RD75, AM25, AM50, AM75, BL25, BL50, BL75
    std::atomic<int32_t> tap;      // RTA SOURCE. HOME: IN, EQ, POST. EQ: PRE, POST
    std::atomic<int32_t> range;    // RTA RANGE: 30 dB, 60 dB
    std::atomic<int32_t> decay;    // RTA DECAY: SLOW, MED, FAST
    std::atomic<int32_t> detector; // RTA DETECTOR: PEAK, RMS, AVG
    std::atomic<bool> autogain;    // RTA AUTOGAIN (applied by the UI)
    std::atomic<float> gainDb;     // FIXED GAIN, -5..50 dB
};

/// RTA, LR vector scope and LISTEN (ld_engine_analysis). Strips are named by
/// kind and id (Strip::getId()); -1 = none.
struct AnalysisControls
{
    std::atomic<int32_t> stripKind, stripId; // the analysed (selected) strip
    std::atomic<bool> rtaOn;                 // an RTA is shown (ld_rta_read starts the analysis thread)
    std::atomic<bool> rtaFreeze;             // FREEZE
    std::atomic<bool> scopeOn;               // the LR VECTOR SCOPE is shown (ld_scope_read)
    std::atomic<int32_t> view;               // RtaView
    std::atomic<int32_t> listenMode;         // ListenMode
    std::atomic<int32_t> listenKind, listenId;
    std::atomic<int32_t> listenBand;         // index into the WING EQ's bands (l, 1..n, h)
    std::atomic<float> bandSoloTrimDb;       // unused since ABI 8: MonitorDspParams::bandSoloDb per path
    RtaViewSettings home, eq;
    std::atomic<int32_t> filterTap;          // FILTER SETUP's RTA SOURCE: PRE, POST
};

struct LayoutInfo
{
    int32_t abiVersion, stripParams, input, filter, dynSlot, eqBlock, insert, mainSend, matrixSend, meters, send, dca,
        fx, controls, offFilter, offGate, offEq, offDynamics, offPostInsert, offFader, offStereo, offMains, offMatrix,
        offDcaMask, offDirectLevel, offColour, offRecallSafe, offDynValues, offDynXoDepth, offEqValues, offSendPan,
        offMetersAutomix, offControlsDcas, offControlsFx, offFxValues, offSendMode, offMatrixSendLevel,
        offMetersTrimPeak, offMetersPflPeak, offControlsBusSoloAfl, offControlsAutomixEnable, offControlsDimDb,
        offControlsMainLink,
        // ABI 6
        offAltInputLeft, offAltSelected, offControlsInputSelectGlobal, offControlsGlobalAlt, recClip,
        recPlaybackTrack, recRecordTrack, recTake,
        // ABI 7
        offTapWidth, offTapEq, tapEq, analysis, offAnalysisEq, offAnalysisFilterTap,
        talk, talkPath, monitorSource, monitorPath, source, offControlsSoloMode, offControlsTalk, offControlsSpeakers,
        offControlsPhones, offTalkPaths, offMonitorPathPeak, offSourcePeak,
        // ABI 8 (monitors)
        monitorDsp, monitorControls, offControlsMonitor, offMonitorDspEq, offMonitorDspPan, offMonitorControlsSourceSolo,
        offSourceMuteGroups;
};

struct ParamInfo
{
    int32_t kind; // 0 linear, 1 log, 2 choice, 3 values, 4 toggle, 5 integer
    float min, max;
    int32_t steps;
    float defaultValue;
    int32_t numChoices;
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

struct Engine;    // opaque
struct Strip;     // opaque
struct LdString;  // opaque owned UTF-8 string: ld_string_data/len, free with ld_string_free
struct LdSink;    // opaque string the host appends to (ld_sink_append) from callbacks
struct Show;      // opaque
struct LayerSet;  // opaque
struct LayerList; // opaque
struct LdWing;    // opaque WING remote (servers + snapshot files), see ld_wing_*

struct StripRefC
{
    int32_t kind, id;
};

/// A fader on the surface: kind 0..4 = a strip, or with toKind 2..4 the send
/// level from it to bus / main / matrix `toId`; kFaderDca = DCA (id 0..15);
/// kFaderFxParam = FX slot `id`'s parameter `toId`. toKind is -1 when unused.
struct FaderRefC
{
    int32_t kind, id;
    int32_t toKind = -1, toId = 0;
};

/// A spill: kind 0 = DCA `index`, 1 = the sources sending to bus slot `index`,
/// 2 = FX slot `index`'s parameters, 3 = automix group `index` (0 X, 1 Y),
/// 4 = the sends of `strip`.
struct SpillC
{
    int32_t kind, index;
    StripRefC strip;
};

/// A LIBRARY preset file's header: kind 0 = not a preset, 1 = FX, 2 = channel;
/// `name` and `detail` (the FX device or the strip kind) are the caller's to free.
struct PresetInfoC
{
    int32_t kind;
    uint32_t scope;
    int32_t stripKind;
    LdString* name;
    LdString* detail;
};

struct ApplyResultC
{
    int32_t stripsApplied, stripsSkippedSafe, stripsMissing;
    LdString* warnings; // '\n'-separated, or null; caller frees
};

/// A recall scope: strip sections, strips (allStrips: every strip), DCAs, mute
/// groups and FX slots (bit n = item n+1) and the console-wide settings. Going
/// in, `strips` points at `numStrips` refs; coming out it is null and the
/// strips go to a separate buffer.
struct ScopeC
{
    uint32_t sections, dcas, muteGroups, fx;
    bool console, allStrips;
    const StripRefC* strips;
    int32_t numStrips;
};

/** Writes the <PLUGIN> description XML and base64 state of FX slot `slot`; false if empty. */
using CaptureFxFn = bool (*) (void* ctx, int32_t slot, LdSink* description, LdSink* state);
/** mode 0 = restore state into the plugin already loaded, 1 = create the plugin.
    Returns 0 ok, 1 different plugin loaded (mode 0), 2 failed (message in `error`). */
using RestoreFxFn = int32_t (*) (void* ctx, int32_t slot, int32_t mode, const char* description, size_t descriptionLen,
                                 const char* state, size_t stateLen, LdSink* error);
/** Empties FX slot `slot` (show load). */
using ClearFxFn = void (*) (void* ctx, int32_t slot);

struct PluginBridge
{
    void* ctx;
    CaptureFxFn capture;
    RestoreFxFn restore;
    ClearFxFn clear;
};

// Recorder (ld_rec_*)

/** A clip of the playlist: file frame fileOffset plays at timeline `start`. */
struct RecClipC
{
    const char* path;
    size_t pathLen;
    int64_t fileOffset, start, length, fadeIn, fadeOut;
    float gain;
    int32_t fadeInShape, fadeOutShape; // 0 Linear, 1 EqualPower, 2 SCurve
};

/** A playback track; its clips are clips[firstClip .. firstClip + numClips). */
struct RecPlaybackTrackC
{
    int32_t channel, numChannels;
    float gain;
    bool mute;
    int32_t firstClip, numClips;
};

struct RecRecordTrackC
{
    int32_t track;
    int32_t sourceKind; // 0 hardware input (port), 1 strip pre-fader, 2 strip post-fader
    int32_t port;
    int32_t stripKind, stripNumber; // as shown: CH 3 = (kChannel, 3), BUS 12 = (kBus, 12)
    int32_t numChannels;
    const char* path;
    size_t pathLen;
};

/** A finished take; free `path` with ld_string_free. */
struct RecTakeC
{
    int32_t track, numChannels;
    int64_t start, length;
    LdString* path;
};

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
    /** Once at start-up: STARTUP MAIN MUTE mutes every main if the preference is on. */
    bool ld_engine_apply_startup_main_mute (const Engine*);

    void ld_engine_collect (const Engine*);
    void ld_engine_collect_all_now (const Engine*);
    int32_t ld_engine_pending_garbage (const Engine*);

    // Structure (message thread)
    int32_t ld_engine_num_strips (const Engine*, int32_t kind);
    Strip* ld_engine_strip_at (const Engine*, int32_t kind, int32_t index);
    Strip* ld_engine_add_channel (const Engine*, int32_t requestedId);
    void ld_engine_set_num_channels (const Engine*, int32_t count);
    /** Adds a bus beyond the console's 16 (null if no slot is free). */
    Strip* ld_engine_add_bus (const Engine*, int32_t requestedSlot);
    Strip* ld_engine_bus_in_slot (const Engine*, int32_t slot);
    /** Removes a channel or an added bus (the console's 16 buses stay). */
    bool ld_engine_remove_strip (const Engine*, const Strip*);
    void ld_engine_reset (const Engine*);
    int32_t ld_engine_total_dsp_faults (const Engine*);
    int32_t ld_engine_num_faulted_fx (const Engine*);
    /** group: 0 = DCA, 1 = mute group. */
    LdString* ld_engine_group_name (const Engine*, int32_t group, int32_t index);
    void ld_engine_set_group_name (const Engine*, int32_t group, int32_t index, const char* utf8, size_t len);

    // FX slots (message thread)
    void ld_fx_set (const Engine*, int32_t slot, const InsertCallbacks* callbacks);
    void* ld_fx_ctx (const Engine*, int32_t slot);
    /** what: 0 = a plugin is loaded, 1 = faulted (auto-bypassed), 2 = double-assigned,
        3 = a built-in device is running. */
    bool ld_fx_state (const Engine*, int32_t slot, int32_t what);
    /** Selects a slot's device (FX table index), creating it now; `defaults` resets its
        settings. False if not allowed there (premium devices need slots 1-8). */
    bool ld_fx_set_model (const Engine*, int32_t slot, int32_t model, bool defaults);
    /** Gain reduction of a slot's device in dB (<= 0). */
    float ld_fx_meter (const Engine*, int32_t slot);
    void ld_fx_clear_flags (const Engine*, int32_t slot);
    float ld_fx_load (const Engine*, int32_t slot);
    int32_t ld_fx_num_users (const Engine*, int32_t slot);

    // Strips
    int32_t ld_strip_kind (const Strip*);
    int32_t ld_strip_id (const Strip*);
    StripParams* ld_strip_params (const Strip*);
    StripMeters* ld_strip_meters (const Strip*);
    int32_t ld_strip_num_channels (const Strip*);
    bool ld_strip_is_idle (const Strip*);
    SendParams* ld_strip_send (const Strip*, int32_t slot);
    SendParams* ld_strip_find_send (const Strip*, int32_t slot);
    LdString* ld_strip_name (const Strip*);
    void ld_strip_set_name (const Strip*, const char* utf8, size_t len);
    /** HOME > TAGS: the strip's free tags, comma-separated. */
    LdString* ld_strip_tags (const Strip*);
    void ld_strip_set_tags (const Strip*, const char* utf8, size_t len);
    /** block: 0 = gate slot, 1 = EQ, 2 = dynamics slot, 3 = filter tool. Loads the model's defaults. */
    void ld_strip_set_model (const Strip*, int32_t block, int32_t model);
    /** Response in dB of the EQ (block 1), FILTER (block 3) or TAP EQ (block 4) at `count` frequencies. */
    void ld_strip_response (const Strip*, int32_t block, const double* freqs, double* outDb, int32_t count, double sampleRate);

    // Model tables: 0 = dynamics models, 1 = channel EQ models, 2 = bus EQ models, 3 = filter tools.
    int32_t ld_model_count (int32_t table);
    /** what: 0 = token ("COMP"), 1 = display name. */
    LdString* ld_model_text (int32_t table, int32_t model, int32_t what);
    /** 0 gate, 1 compressor, 2 special, 3 EQ, 4 filter. */
    int32_t ld_model_category (int32_t table, int32_t model);
    int32_t ld_model_find (int32_t table, const char* utf8, size_t len);
    int32_t ld_model_num_params (int32_t table, int32_t model);
    bool ld_model_param_info (int32_t table, int32_t model, int32_t param, ParamInfo* out);
    /** what: 0 = id ("thr"), 1 = label ("THR"), 2 = unit ("dB"). */
    LdString* ld_model_param_text (int32_t table, int32_t model, int32_t param, int32_t what);
    LdString* ld_model_param_choice (int32_t table, int32_t model, int32_t param, int32_t index);
    LdString* ld_model_param_display (int32_t table, int32_t model, int32_t param, float value);
    float ld_model_param_to_normalised (int32_t table, int32_t model, int32_t param, float value);
    float ld_model_param_from_normalised (int32_t table, int32_t model, int32_t param, float position);

    // Analysis: RTA, LR vector scope, LISTEN
    AnalysisControls* ld_engine_analysis (const Engine*);
    int32_t ld_rta_num_bands();
    /** Centre frequency of an RTA band (a piano key), 0 if out of range. */
    float ld_rta_band_hz (int32_t band);
    /** Message thread: the RTA bands (dB) and peak holds (either may be null); returns the bands
        written. Starts the analysis thread while rtaOn. `frame` (may be null) moves on new bands. */
    int32_t ld_rta_read (const Engine*, float* levels, float* peaks, int32_t count, uint32_t* frame);
    /** The newest L/R pairs of the LR vector scope (interleaved); returns how many. */
    int32_t ld_scope_read (const Engine*, float* lr, int32_t maxPairs);
    /** Scope pairs per second. */
    float ld_scope_rate (const Engine*);

    // Lists: 0 process orders, 1 tap points, 2 sidechain filters, 3 crossover types, 4 send modes,
    // 5 delay units, 6 post insert modes, 7 monitor buses, 8 low-cut slopes, 9 high-cut slopes,
    // 10 aux dynamics models, 11 colours, 12 MAIN LINK modes.
    int32_t ld_list_count (int32_t list);
    LdString* ld_list_item (int32_t list, int32_t index);
    uint32_t ld_colour_rgb (int32_t index);

    // Strings
    const char* ld_string_data (const LdString*);
    size_t ld_string_len (const LdString*);
    void ld_string_free (LdString*);
    void ld_sink_append (LdSink*, const char* utf8, size_t len);

    // Shows (message thread)
    Show* ld_show_create (const PluginBridge*);
    void ld_show_destroy (Show*);
    LdString* ld_show_name (Show*);
    void ld_show_set_name (Show*, const char* utf8, size_t len);
    int32_t ld_show_num_scenes (Show*);
    int32_t ld_show_current_scene (Show*);
    int32_t ld_show_add_scene (Show*, const Engine*, const char* name, size_t len, int32_t index);
    void ld_show_store_scene (Show*, const Engine*, int32_t index);
    void ld_show_remove_scene (Show*, int32_t index);
    void ld_show_move_scene (Show*, int32_t from, int32_t to);
    /** which: 0 = scene `index`, 1 = next (GO NEXT), 2 = previous (GO PREV), 3 = the first scene
        tagged `index` (#1..#128). Skipped scenes are passed over; a linked scene also recalls the
        one below it. */
    void ld_show_recall_scene (Show*, const Engine*, int32_t which, int32_t index, ApplyResultC* out);
    /** The first non-skipped scene tagged `tag`, or -1. */
    int32_t ld_show_find_scene_by_tag (Show*, int32_t tag);

    bool ld_scene_exists (Show*, int32_t scene);
    LdString* ld_scene_name (Show*, int32_t scene);
    LdString* ld_scene_notes (Show*, int32_t scene);
    uint32_t ld_scene_scope (Show*, int32_t scene);
    double ld_scene_fade_seconds (Show*, int32_t scene);
    int32_t ld_scene_num_snippets (Show*, int32_t scene);
    void ld_scene_set_name (Show*, int32_t scene, const char* utf8, size_t len);
    void ld_scene_set_notes (Show*, int32_t scene, const char* utf8, size_t len);
    void ld_scene_set_scope (Show*, int32_t scene, uint32_t scope);
    void ld_scene_set_fade_seconds (Show*, int32_t scene, double seconds);
    /** EDIT TAG #1..#128 (0 = none; out-of-range values clear it), SKIP, LINK (to the scene below). */
    int32_t ld_scene_tag (Show*, int32_t scene);
    bool ld_scene_skip (Show*, int32_t scene);
    bool ld_scene_link (Show*, int32_t scene);
    void ld_scene_set_tag (Show*, int32_t scene, int32_t tag);
    void ld_scene_set_skip (Show*, int32_t scene, bool skip);
    void ld_scene_set_link (Show*, int32_t scene, bool link);

    int32_t ld_show_add_snippet (Show*, const Engine*, int32_t scene, const char* name, size_t len,
                                 uint32_t sections, const StripRefC* strips, int32_t numStrips);
    void ld_show_store_snippet (Show*, const Engine*, int32_t scene, int32_t snippet);
    void ld_show_remove_snippet (Show*, int32_t scene, int32_t snippet);
    void ld_show_recall_snippet (Show*, const Engine*, int32_t scene, int32_t snippet, ApplyResultC* out);
    bool ld_snippet_exists (Show*, int32_t scene, int32_t snippet);
    LdString* ld_snippet_name (Show*, int32_t scene, int32_t snippet);
    uint32_t ld_snippet_sections (Show*, int32_t scene, int32_t snippet);
    bool ld_snippet_fire_with_scene (Show*, int32_t scene, int32_t snippet);
    int32_t ld_snippet_strips (Show*, int32_t scene, int32_t snippet, StripRefC* out, int32_t capacity);
    void ld_snippet_set_name (Show*, int32_t scene, int32_t snippet, const char* utf8, size_t len);
    void ld_snippet_set_sections (Show*, int32_t scene, int32_t snippet, uint32_t sections);
    void ld_snippet_set_fire_with_scene (Show*, int32_t scene, int32_t snippet, bool fire);
    void ld_snippet_set_strips (Show*, int32_t scene, int32_t snippet, const StripRefC* strips, int32_t n);

    // Scopes, GLOBAL safes, PARTIAL LOAD / UPDATE, RESTORE, snippet REC
    /** A scene's recall scope; returns the strip count (strips copied up to `capacity`). */
    int32_t ld_scene_get_scope (Show*, int32_t scene, ScopeC* out, StripRefC* strips, int32_t capacity);
    void ld_scene_set_recall_scope (Show*, int32_t scene, const ScopeC*);
    /** PARTIAL UPDATE: stores only `scope` into the scene. */
    void ld_show_store_scene_partial (Show*, const Engine*, int32_t index, const ScopeC* scope);
    /** PARTIAL LOAD (`scope`; ignoreSafes = NO GLOB) or RESTORE (scope null: all of it, safes included). */
    void ld_show_recall_scene_with (Show*, const Engine*, int32_t index, const ScopeC* scope, bool ignoreSafes, ApplyResultC* out);
    int32_t ld_show_partial_scope (Show*, ScopeC* out, StripRefC* strips, int32_t capacity);
    /** GLOBAL safes beyond strips (no strips; the strips' own recall safe stays on them). */
    void ld_show_global_safe (Show*, ScopeC* out);
    void ld_show_set_global_safe (Show*, const ScopeC*);
    int32_t ld_snippet_get_scope (Show*, int32_t scene, int32_t snippet, ScopeC* out, StripRefC* strips, int32_t capacity);
    int32_t ld_snippet_parameter_count (Show*, int32_t scene, int32_t snippet);
    int32_t ld_show_add_snippet_scoped (Show*, const Engine*, int32_t scene, const char* name, size_t len, const ScopeC* scope);
    /** ADD ITEMS (add) or REMOVE ITEMS; no strips = every strip of the snippet. */
    void ld_show_snippet_items (Show*, const Engine*, int32_t scene, int32_t snippet, bool add, const ScopeC* items);
    /** PARTIAL LOAD of a snippet (limit null: all of it). */
    void ld_show_recall_snippet_with (Show*, const Engine*, int32_t scene, int32_t snippet, const ScopeC* limit, ApplyResultC* out);
    /** op: 0 REC, 1 STOP (returns the changed parameters), 2 CLEAR, 3 count, 4 recording?, 5 recorded and not saved? */
    int32_t ld_show_snippet_rec (Show*, const Engine*, int32_t op);
    /** SAVE after STOP: a snippet of the recorded parameters within `focus` (null: all); -1 if none. */
    int32_t ld_show_add_recorded_snippet (Show*, const Engine*, int32_t scene, const char* name, size_t len, const ScopeC* focus);

    // LIBRARY presets: FX (.lfx) and channel (.lchn) files
    bool ld_preset_info (const char* path, size_t len, PresetInfoC* out);
    bool ld_show_save_fx_preset (Show*, const Engine*, int32_t slot, const char* path, size_t len, const char* name, size_t nameLen,
                                 LdString** message);
    /** Warnings (a plugin that isn't loaded there, a premium device in 9-16) in `message`. */
    bool ld_show_load_fx_preset (Show*, const Engine*, int32_t slot, const char* path, size_t len, LdString** message);
    bool ld_show_save_channel_preset (Show*, const Engine*, StripRefC strip, uint32_t scope, const char* path, size_t len,
                                      const char* name, size_t nameLen, LdString** message);
    /** useScope false: the preset's own scope. */
    bool ld_show_load_channel_preset (Show*, const Engine*, StripRefC strip, const char* path, size_t len, bool useScope,
                                      uint32_t scope, LdString** message);
    /** UPDATE SCOPE of a channel preset file. */
    bool ld_preset_set_scope (const char* path, size_t len, uint32_t scope);

    bool ld_show_is_fading (Show*);
    void ld_show_cancel_fade (Show*);
    bool ld_show_tick (Show*);

    LdString* ld_show_to_xml (Show*, const Engine*);
    /** False if nothing was loaded (error in *message); true may still set *message (warnings). */
    bool ld_show_load_xml (Show*, const Engine*, const char* utf8, size_t len, bool loadPlugins, LdString** message);
    bool ld_show_save_file (Show*, const Engine*, const char* path, size_t len, LdString** error);
    bool ld_show_load_file (Show*, const Engine*, const char* path, size_t len, bool loadPlugins, LdString** message);
    void ld_show_clear (Show*, const Engine*, int32_t numChannels);
    /** The CUSTOM CONTROLS assignments as a <CustomControls> element, and back. */
    LdString* ld_show_custom_controls (Show*);
    bool ld_show_set_custom_controls (Show*, const char* utf8, size_t len);

    // Layers (message thread). Sections: 0 = left (12 faders), 1 = center (8), 2 = right (4).
    LayerSet* ld_show_layers (Show*);
    LayerSet* ld_layers_create();
    void ld_layers_destroy (LayerSet*);
    /** 0 = Auto (fit the window). */
    int32_t ld_layers_width (LayerSet*, int32_t section);
    void ld_layers_set_width (LayerSet*, int32_t section, int32_t width);
    void ld_layers_set_fitted_width (LayerSet*, int32_t section, int32_t width);
    int32_t ld_layers_effective_width (LayerSet*, int32_t section);
    LdString* ld_layers_selected (LayerSet*, int32_t section);
    void ld_layers_set_selected (LayerSet*, int32_t section, const char* utf8, size_t len);
    int32_t ld_layers_offset (LayerSet*, int32_t section);
    void ld_layers_set_offset (LayerSet*, int32_t section, int32_t offset);
    /** The section a spill from `origin` goes to (origin -1: none). */
    int32_t ld_layers_spill_target (int32_t origin);
    void ld_layers_start_spill (LayerSet*, int32_t section, SpillC spill);
    bool ld_layers_end_spill (LayerSet*, int32_t section);
    bool ld_layers_spill (LayerSet*, int32_t section, SpillC* out);
    LdString* ld_layers_find_default_containing (LayerSet*, const Engine*, int32_t section, FaderRefC ref);
    /** which: 0 default, 1 default + user, 2 user only, 3 the layer `id` (or the section's first). */
    LayerList* ld_layers_list (LayerSet*, const Engine*, int32_t section, int32_t which, const char* id, size_t idLen);
    void ld_layer_list_free (LayerList*);
    int32_t ld_layer_list_len (const LayerList*);
    LdString* ld_layer_list_id (const LayerList*, int32_t i);
    LdString* ld_layer_list_name (const LayerList*, int32_t i);
    bool ld_layer_list_is_user (const LayerList*, int32_t i);
    int32_t ld_layer_list_faders (const LayerList*, int32_t i, FaderRefC* out, int32_t capacity);
    LdString* ld_layers_add_user (LayerSet*, const char* name, size_t len, const FaderRefC* faders, int32_t n);
    void ld_layers_remove_user (LayerSet*, const char* id, size_t len);
    void ld_layers_rename_user (LayerSet*, const char* id, size_t len, const char* name, size_t nameLen);
    void ld_layers_move_user (LayerSet*, const char* id, size_t len, int32_t delta);
    void ld_layers_add_to_user (LayerSet*, const char* id, size_t len, FaderRefC ref);
    void ld_layers_remove_from_user (LayerSet*, const char* id, size_t len, FaderRefC ref);
    void ld_layers_move_within_user (LayerSet*, const char* id, size_t len, int32_t index, int32_t delta);
    void ld_layers_set_override (LayerSet*, int32_t section, const char* id, size_t len, const FaderRefC* faders, int32_t n);
    void ld_layers_clear_override (LayerSet*, int32_t section, const char* id, size_t len);
    bool ld_layers_has_override (LayerSet*, int32_t section, const char* id, size_t len);
    LdString* ld_layers_to_xml (LayerSet*);
    bool ld_layers_from_xml (LayerSet*, const char* utf8, size_t len);
    void ld_layers_reset (LayerSet*);

    // Recorder (message thread; the audio side runs in ld_engine_process)
    /** 0 Stopped, 1 Starting, 2 Playing, 3 Recording (commands not yet taken by the
        audio thread already show: play reads Starting at once). */
    int32_t ld_rec_state (const Engine*);
    int64_t ld_rec_position (const Engine*);
    void ld_rec_play (const Engine*);
    void ld_rec_stop (const Engine*);
    void ld_rec_locate (const Engine*, int64_t position);
    void ld_rec_set_loop (const Engine*, int64_t start, int64_t end, bool enabled);
    /** Writes the loop range (pointers may be null); returns whether it is enabled. */
    bool ld_rec_loop (const Engine*, int64_t* start, int64_t* end);
    /** Copies the playlist (the arrays may be freed afterwards). */
    void ld_rec_set_playlist (const Engine*, const RecPlaybackTrackC* tracks, int32_t numTracks,
                              const RecClipC* clips, int32_t numClips);
    /** Creates the files and starts recording; false sets *error (may be null; free it). */
    bool ld_rec_record (const Engine*, const RecRecordTrackC* tracks, int32_t numTracks, LdString** error);
    /** The next finalized take; false if none. */
    bool ld_rec_pop_take (const Engine*, RecTakeC* out);
    /** Linear peak of a recording track's channel since the last call. */
    float ld_rec_record_level (const Engine*, int32_t track, int32_t channel);
    /** minMax holds maxPeaks * 2 * ld_rec_record_track_channels() floats. */
    int32_t ld_rec_recording_peaks (const Engine*, int32_t track, int64_t firstPeak, int32_t maxPeaks, float* minMax);
    int32_t ld_rec_record_track_channels (const Engine*, int32_t track);
    int64_t ld_rec_recorded_length (const Engine*);
    int32_t ld_rec_dropouts (const Engine*);
    float ld_rec_disk_load (const Engine*);
    /** Waits until the disk threads caught up (offline use, faster than real time). */
    bool ld_rec_wait_for_disk (const Engine*, int32_t timeoutMs);

    // WING remote (message thread). Destroy it before the engine.
    LdWing* ld_wing_create (const Engine*);
    void ld_wing_destroy (LdWing*);
    /** Ports: 0 = any free port. enable: 1 discovery, 2 OSC, 4 Native. Restarts if running;
        false if an enabled server could not start (the others run; see ld_wing_status). */
    bool ld_wing_start (LdWing*, const char* name, size_t len, int32_t discoveryPort, int32_t oscPort,
                        int32_t nativePort, uint32_t enable);
    void ld_wing_stop (LdWing*);
    bool ld_wing_running (LdWing*);
    void ld_wing_set_name (LdWing*, const char* name, size_t len);
    /** Bound port of server 0 discovery, 1 OSC, 2 Native; -1 if not running. */
    int32_t ld_wing_port (LdWing*, int32_t which);
    /** Tab-separated lines: "error\tmsg", "client\tnative|osc\taddr\tsubscribed\tseconds",
        "traffic\toscMessages\tnativeBytes". */
    LdString* ld_wing_status (LdWing*);
    /** Applies a WING .snap; *report gets the summary and what was not applied (or the error). */
    bool ld_wing_import_snap (LdWing*, const char* path, size_t len, LdString** report);
    /** Writes a WING .snap (experimental); false sets *message. */
    bool ld_wing_export_snap (LdWing*, const char* path, size_t len, LdString** message);
    /** Adds a WING .show's snapshot scenes to the show (the console only grows to 40 channels). */
    bool ld_wing_import_show (LdWing*, Show*, const char* path, size_t len, LdString** report);
    LdString* ld_wing_coverage();
    /** MIDI SYSEX CONTROL on the WING tree: cmd 2 executes a node string
        ("/ch.1.fdr=-1,mute=0"), 3 dumps a node, 5 describes one. True with
        *out the data (empty for 2), false with *out the error text. */
    bool ld_wing_sysex (LdWing*, int32_t cmd, const char* text, size_t len, LdString** out);

    // Console level: sources, talk tags, MUTE state (message thread unless noted)
    /** The source table (*count entries: hardware ports, then playback channels). */
    SourceParams* ld_engine_sources (const Engine*, int32_t* count);
    /** Source table index of an input port (hardware input or playback source), -1 if none. */
    int32_t ld_source_index (int32_t port);
    int32_t ld_source_port (int32_t index);
    LdString* ld_source_name (const Engine*, int32_t index);
    void ld_source_set_name (const Engine*, int32_t index, const char* utf8, size_t len);
    /** SourceMode for the source and its pair partner (a new pair takes the first one's settings). */
    void ld_source_set_mode (const Engine*, int32_t index, int32_t mode);
    /** INITIALIZE (the pair's settings and names; the pairing stays). */
    void ld_source_initialize (const Engine*, int32_t index);
    /** Any thread: TalkTag bits. */
    uint32_t ld_strip_talk_tags (const Strip*);
    void ld_strip_set_talk_tags (const Strip*, uint32_t tags);
    /** Any thread: MuteState of the last block. */
    int32_t ld_strip_mute_state (const Strip*);
    /** Any thread: soloed in the last block (own solo, a soloed DCA or TALK SOLO). */
    bool ld_strip_is_soloed (const Strip*);
    /** The MUTE key, MUTEGROUP/SIP OVERRIDE aware. */
    void ld_engine_press_mute (const Engine*, const Strip*);
    /** Monitor paths (bit 0 = SPEAKERS, bit 1 = PHONES) DCA `dca` controls (named MON A: PHONES, MON B: SPEAKERS, MON LVL). */
    uint32_t ld_engine_dca_monitor_paths (const Engine*, int32_t dca);

    // Monitor path processing and SOURCE SOLO (ABI 8; settings in EngineControls::monitor)
    /** DIRECT IN name ("CH.12", "AUX.6", "MTX.3"; "OFF"). */
    LdString* ld_monitor_direct_name (int32_t index);
    /** Delay in ms for a distance in meters. */
    float ld_monitor_delay_ms (float meters);
    /** dB response of path `path`'s EQ (0 SPEAKERS, 1 PHONES): the whole EQ (band < 0, flat while off) or one band. */
    void ld_monitor_eq_response (const Engine*, int32_t path, int32_t band, const float* freqs, float* outDb, int32_t n);
    /** The SOURCE SOLO host (CH39 / AUX7), nullptr when OFF. */
    const Strip* ld_engine_source_solo_host (const Engine*);
}

/// Returns nullptr if the C++ and Rust layouts agree, else a description of the mismatch.
inline const char* verifyLayout()
{
    LayoutInfo info {};
    ld_layout_info (&info);

    struct Check
    {
        int32_t rust;
        size_t cpp;
        const char* what;
    };

    const Check checks[] = {
        { info.stripParams, sizeof (StripParams), "StripParams size" },
        { info.input, sizeof (InputParams), "InputParams size" },
        { info.filter, sizeof (FilterParams), "FilterParams size" },
        { info.dynSlot, sizeof (DynSlotParams), "DynSlotParams size" },
        { info.eqBlock, sizeof (EqBlockParams), "EqBlockParams size" },
        { info.insert, sizeof (InsertParams), "InsertParams size" },
        { info.mainSend, sizeof (MainSendParams), "MainSendParams size" },
        { info.matrixSend, sizeof (MatrixSendParams), "MatrixSendParams size" },
        { info.meters, sizeof (StripMeters), "StripMeters size" },
        { info.send, sizeof (SendParams), "SendParams size" },
        { info.dca, sizeof (DcaParams), "DcaParams size" },
        { info.fx, sizeof (FxParams), "FxParams size" },
        { info.controls, sizeof (EngineControls), "EngineControls size" },
        { info.offFilter, offsetof (StripParams, filter), "StripParams.filter" },
        { info.offGate, offsetof (StripParams, gate), "StripParams.gate" },
        { info.offEq, offsetof (StripParams, eq), "StripParams.eq" },
        { info.offDynamics, offsetof (StripParams, dynamics), "StripParams.dynamics" },
        { info.offPostInsert, offsetof (StripParams, postInsert), "StripParams.postInsert" },
        { info.offFader, offsetof (StripParams, faderDb), "StripParams.faderDb" },
        { info.offStereo, offsetof (StripParams, stereo), "StripParams.stereo" },
        { info.offMains, offsetof (StripParams, mains), "StripParams.mains" },
        { info.offMatrix, offsetof (StripParams, matrix), "StripParams.matrix" },
        { info.offDcaMask, offsetof (StripParams, dcaMask), "StripParams.dcaMask" },
        { info.offDirectLevel, offsetof (StripParams, directLevelDb), "StripParams.directLevelDb" },
        { info.offColour, offsetof (StripParams, colour), "StripParams.colour" },
        { info.offRecallSafe, offsetof (StripParams, recallSafe), "StripParams.recallSafe" },
        { info.offDynValues, offsetof (DynSlotParams, values), "DynSlotParams.values" },
        { info.offDynXoDepth, offsetof (DynSlotParams, xoDepth), "DynSlotParams.xoDepth" },
        { info.offEqValues, offsetof (EqBlockParams, values), "EqBlockParams.values" },
        { info.offSendPan, offsetof (SendParams, pan), "SendParams.pan" },
        { info.offMetersAutomix, offsetof (StripMeters, automixDb), "StripMeters.automixDb" },
        { info.offControlsDcas, offsetof (EngineControls, dcas), "EngineControls.dcas" },
        { info.offControlsFx, offsetof (EngineControls, fx), "EngineControls.fx" },
        { info.offFxValues, offsetof (FxParams, values), "FxParams.values" },
        { info.offSendMode, offsetof (SendParams, mode), "SendParams.mode" },
        { info.offMatrixSendLevel, offsetof (MatrixSendParams, levelDb), "MatrixSendParams.levelDb" },
        { info.offMetersTrimPeak, offsetof (StripMeters, trimPeak), "StripMeters.trimPeak" },
        { info.offMetersPflPeak, offsetof (StripMeters, pflPeak), "StripMeters.pflPeak" },
        { info.offControlsBusSoloAfl, offsetof (EngineControls, busSoloAfl), "EngineControls.busSoloAfl" },
        { info.offControlsAutomixEnable, offsetof (EngineControls, automixEnable), "EngineControls.automixEnable" },
        { info.offControlsDimDb, offsetof (EngineControls, monitorDimDb), "EngineControls.monitorDimDb" },
        { info.offControlsMainLink, offsetof (EngineControls, mainLink), "EngineControls.mainLink" },
        { info.offAltInputLeft, offsetof (StripParams, altInputLeft), "StripParams.altInputLeft" },
        { info.offAltSelected, offsetof (StripParams, altSelected), "StripParams.altSelected" },
        { info.offControlsInputSelectGlobal, offsetof (EngineControls, inputSelectGlobal), "EngineControls.inputSelectGlobal" },
        { info.offControlsGlobalAlt, offsetof (EngineControls, globalAlt), "EngineControls.globalAlt" },
        { info.recClip, sizeof (RecClipC), "RecClipC size" },
        { info.recPlaybackTrack, sizeof (RecPlaybackTrackC), "RecPlaybackTrackC size" },
        { info.recRecordTrack, sizeof (RecRecordTrackC), "RecRecordTrackC size" },
        { info.recTake, sizeof (RecTakeC), "RecTakeC size" },
        { info.offTapWidth, offsetof (StripParams, tapWidth), "StripParams.tapWidth" },
        { info.offTapEq, offsetof (StripParams, tapEq), "StripParams.tapEq" },
        { info.tapEq, sizeof (TapEqParams), "TapEqParams size" },
        { info.analysis, sizeof (AnalysisControls), "AnalysisControls size" },
        { info.offAnalysisEq, offsetof (AnalysisControls, eq), "AnalysisControls.eq" },
        { info.offAnalysisFilterTap, offsetof (AnalysisControls, filterTap), "AnalysisControls.filterTap" },
        { info.talk, sizeof (TalkParams), "TalkParams size" },
        { info.talkPath, sizeof (TalkPathParams), "TalkPathParams size" },
        { info.monitorSource, sizeof (MonitorSourceParams), "MonitorSourceParams size" },
        { info.monitorPath, sizeof (MonitorPathParams), "MonitorPathParams size" },
        { info.source, sizeof (SourceParams), "SourceParams size" },
        { info.offControlsSoloMode, offsetof (EngineControls, soloMode), "EngineControls.soloMode" },
        { info.offControlsTalk, offsetof (EngineControls, talk), "EngineControls.talk" },
        { info.offControlsSpeakers, offsetof (EngineControls, speakers), "EngineControls.speakers" },
        { info.offControlsPhones, offsetof (EngineControls, phones), "EngineControls.phones" },
        { info.offTalkPaths, offsetof (TalkParams, paths), "TalkParams.paths" },
        { info.offMonitorPathPeak, offsetof (MonitorPathParams, peak), "MonitorPathParams.peak" },
        { info.offSourcePeak, offsetof (SourceParams, peak), "SourceParams.peak" },
        { info.monitorDsp, sizeof (MonitorDspParams), "MonitorDspParams size" },
        { info.monitorControls, sizeof (MonitorControls), "MonitorControls size" },
        { info.offControlsMonitor, offsetof (EngineControls, monitor), "EngineControls.monitor" },
        { info.offMonitorDspEq, offsetof (MonitorDspParams, eq), "MonitorDspParams.eq" },
        { info.offMonitorDspPan, offsetof (MonitorDspParams, pan), "MonitorDspParams.pan" },
        { info.offMonitorControlsSourceSolo, offsetof (MonitorControls, sourceSolo), "MonitorControls.sourceSolo" },
        { info.offSourceMuteGroups, offsetof (SourceParams, muteGroups), "SourceParams.muteGroups" },
    };

    if (info.abiVersion != kAbiVersion)
        return "ABI version";
    for (const auto& c : checks)
        if (c.rust != (int32_t) c.cpp)
            return c.what;
    return nullptr;
}

} // namespace ld
