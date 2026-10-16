#pragma once

#include "livedaw_engine.h"

#include <juce_core/juce_core.h>

namespace livedaw
{

/** Converts (and frees) a string returned by the Rust engine. Null gives an empty string. */
inline juce::String takeString (ld::LdString* s)
{
    if (s == nullptr)
        return {};

    auto result = juce::String::fromUTF8 (ld::ld_string_data (s), static_cast<int> (ld::ld_string_len (s)));
    ld::ld_string_free (s);
    return result;
}

/** UTF-8 pointer + length of a juce::String, for passing to the Rust engine. */
struct Utf8
{
    explicit Utf8 (const juce::String& s) : data (s.toRawUTF8()), size (s.getNumBytesAsUTF8()) {}

    const char* data;
    size_t size;
};

/** Appends a juce::String to a Rust sink (inside plugin bridge callbacks). */
inline void appendTo (ld::LdSink* sink, const juce::String& s)
{
    const Utf8 u (s);
    ld::ld_sink_append (sink, u.data, u.size);
}

} // namespace livedaw
