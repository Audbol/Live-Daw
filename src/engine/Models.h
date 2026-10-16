#pragma once

#include "engine/RustStrings.h"

#include <juce_core/juce_core.h>

#include <vector>

namespace livedaw::models
{

/** Model tables (the same ones the Rust DSP uses), for building the UI. */
enum Table : int
{
    Dynamics = 0,    // the 32 gate/dynamics models (both slots)
    ChannelEq = 1,   // channel and aux EQ models
    BusEq = 2,       // bus, main and matrix EQ models
    FilterTools = 3, // the FILTER block's tool filters
    Fx = 4           // the FX rack's devices (NONE, EXT = plugin, premium, standard, channel strips)
};

/** Choice lists. */
enum List : int
{
    ProcessOrders = 0,
    TapPoints = 1,
    SidechainFilters = 2,
    XoverTypes = 3,
    SendModes = 4,
    DelayUnits = 5,
    PostInsertModes = 6,
    MonitorBuses = 7,
    LowCutSlopes = 8,
    HighCutSlopes = 9,
    AuxDynModels = 10,
    Colours = 11
};

enum class ParamKind
{
    Linear,
    Log,
    Choice,
    Values,
    Toggle,
    Integer
};

enum class Category
{
    Gate,
    Compressor,
    Special,
    Eq,
    Filter,
    FxNone,
    FxExternal,
    FxPremium,  // FX slots 1-8 only
    FxStandard,
    FxChannel
};

struct Param
{
    int table = 0, model = 0, index = 0;
    juce::String id, label, unit;
    ParamKind kind = ParamKind::Linear;
    float min = 0, max = 1, defaultValue = 0;
    int steps = 0;
    juce::StringArray choices;

    bool isContinuous() const noexcept { return kind == ParamKind::Linear || kind == ParamKind::Log; }
    bool isStepped() const noexcept { return ! isContinuous(); }
    /** Console-style value text ("-20.0 dB", "1:4", "8k"). */
    juce::String display (float value) const;
    /** Knob position 0..1 (log taper for log parameters) and back. */
    float toNormalised (float value) const;
    float fromNormalised (float position) const;
};

struct Model
{
    int table = 0, index = 0;
    juce::String token, name;
    Category category = Category::Gate;
    std::vector<Param> params;

    /** Index of the parameter `id` ("thr"), or -1. */
    int find (const juce::String& id) const noexcept;
};

/** All models of a table (built once, message thread). */
const std::vector<Model>& table (Table);

/** Model `index` of a table (clamped to the table). */
const Model& model (Table, int index);

/** Index of the model with this token ("COMP"), or -1. */
int findModel (Table, const juce::String& token);

juce::StringArray list (List);

} // namespace livedaw::models
