#include "engine/Strip.h"
#include "dsp/DspUtils.h"

namespace livedaw
{

const char* toString (StripKind kind) noexcept
{
    switch (kind)
    {
        case StripKind::Input:  return "input";
        case StripKind::Aux:    return "aux";
        case StripKind::Group:  return "group";
        case StripKind::Master: return "master";
    }
    return "input";
}

StripKind stripKindFromString (const juce::String& s) noexcept
{
    if (s == "aux")    return StripKind::Aux;
    if (s == "group")  return StripKind::Group;
    if (s == "master") return StripKind::Master;
    return StripKind::Input;
}

Strip::Strip (StripKind k, int stripId, RtGarbage& garbage)
    : inserts (garbage), kind (k), id (stripId)
{
    for (auto& p : inputPeak)
        p.store (0.0f);
    for (auto& p : outputPeak)
        p.store (0.0f);

    // Sensible defaults per strip type.
    switch (kind)
    {
        case StripKind::Input:
            params.mainAssign = true;
            params.stereo = false;
            colour = juce::Colour (0xff3d7ab8);
            break;
        case StripKind::Aux:
            params.mainAssign = false;
            params.stereo = false;
            colour = juce::Colour (0xff3daa6e);
            break;
        case StripKind::Group:
            params.mainAssign = true;
            params.stereo = true;
            colour = juce::Colour (0xffb8863d);
            break;
        case StripKind::Master:
            params.mainAssign = false;
            params.stereo = true;
            params.outputLeft = 0;
            params.outputRight = 1;
            colour = juce::Colour (0xffb83d4f);
            break;
    }

    prepare (sampleRate, maxBlock);
}

void Strip::prepare (double newSampleRate, int maxBlockSize)
{
    sampleRate = newSampleRate;
    maxBlock = juce::jmax (1, maxBlockSize);

    for (auto* b : { &work, &pre, &post, &main })
        b->setSize (2, maxBlock, false, true, false);

    hpf.reset();
    lastHpfFreq = -1.0f;
    gate.prepare (sampleRate);
    eq.prepare (sampleRate);
    comp.prepare (sampleRate);
    inserts.prepare (sampleRate, maxBlock);
}

void Strip::readHardwareInputs (const float* const* hwInputs, int numHwInputs, int numSamples) noexcept
{
    const int left = params.inputLeft.load (std::memory_order_relaxed);
    int right = params.inputRight.load (std::memory_order_relaxed);
    numChannels = params.stereo.load (std::memory_order_relaxed) ? 2 : 1;

    // A stereo channel without an explicit right input uses the next hardware input.
    if (numChannels == 2 && right < 0 && left >= 0)
        right = left + 1;

    currentWidth.store (numChannels, std::memory_order_relaxed);

    const int sources[2] = { left, right };

    for (int ch = 0; ch < numChannels; ++ch)
    {
        const int hw = sources[ch];

        if (hw >= 0 && hw < numHwInputs && hwInputs[hw] != nullptr)
            work.copyFrom (ch, 0, hwInputs[hw], numSamples);
        else
            work.clear (ch, 0, numSamples);

        dsp::atomicMax (inputPeak[(size_t) ch], dsp::peakLevel (work.getReadPointer (ch), numSamples));
    }
}

void Strip::beginSummedBlock (int numSamples) noexcept
{
    numChannels = (kind == StripKind::Master || params.stereo.load (std::memory_order_relaxed)) ? 2 : 1;
    currentWidth.store (numChannels, std::memory_order_relaxed);

    for (int ch = 0; ch < numChannels; ++ch)
        work.clear (ch, 0, numSamples);
}

void Strip::process (int numSamples) noexcept
{
    jassert (numSamples <= maxBlock);

    if (kind != StripKind::Input)
        for (int ch = 0; ch < numChannels; ++ch)
            dsp::atomicMax (inputPeak[(size_t) ch], dsp::peakLevel (work.getReadPointer (ch), numSamples));

    runChain (numSamples);

    // Never let NaN/inf escape a strip: it would poison every bus it feeds and the
    // filter states downstream. Silence this block, reset the DSP and report it.
    bool finite = true;
    for (int c = 0; c < numChannels; ++c)
        finite = finite && dsp::allFinite (work.getReadPointer (c), numSamples);

    if (! finite)
    {
        for (int c = 0; c < 2; ++c)
            work.clear (c, 0, numSamples);

        resetDsp();
        dspFaults.fetch_add (1, std::memory_order_relaxed);
    }

    runOutputStage (numSamples);
}

void Strip::resetDsp() noexcept
{
    hpf.reset();
    gate.reset();
    eq.reset();
    comp.reset();
}

void Strip::runChain (int numSamples) noexcept
{
    auto* const* ch = work.getArrayOfWritePointers();

    // Trim + polarity
    const float polarity = params.polarityInvert.load (std::memory_order_relaxed) ? -1.0f : 1.0f;
    const float trim = dsp::dbToGain (params.trimDb.load (std::memory_order_relaxed)) * polarity;
    for (int c = 0; c < numChannels; ++c)
        dsp::applyGainRamp (ch[c], numSamples, lastTrim, trim);
    lastTrim = trim;

    // High-pass filter (12 dB/oct)
    if (params.hpfEnabled.load (std::memory_order_relaxed))
    {
        const float f = params.hpfFrequency.load (std::memory_order_relaxed);

        if (! hpfWasOn)
            hpf.reset();

        if (f != lastHpfFreq)
        {
            hpf.setCoefficients (dsp::BiquadCoeffs::highPass (sampleRate, f, 0.7071));
            lastHpfFreq = f;
        }

        for (int c = 0; c < numChannels; ++c)
            hpf.process (ch[c], c, numSamples);

        hpfWasOn = true;
    }
    else
    {
        hpfWasOn = false;
    }

    // Dynamics + EQ
    gate.setSettings (params.gate.snapshot());
    gate.process (ch, numChannels, numSamples);
    gateGain.store (gate.getLastBlockMinGain(), std::memory_order_relaxed);

    eq.setSettings (params.eq.snapshot());
    eq.process (ch, numChannels, numSamples);

    comp.setSettings (params.comp.snapshot());
    comp.process (ch, numChannels, numSamples);
    compReductionDb.store (comp.getLastBlockGainReductionDb(), std::memory_order_relaxed);

    // Plugin inserts
    inserts.process (ch, numChannels, numSamples);
}

void Strip::runOutputStage (int numSamples) noexcept
{
    const float muteGain = params.mute.load (std::memory_order_relaxed) ? 0.0f : 1.0f;
    const float faderGain = dsp::dbToGain (params.faderDb.load (std::memory_order_relaxed));

    for (int c = 0; c < numChannels; ++c)
    {
        pre.copyFrom (c, 0, work, c, 0, numSamples);
        dsp::applyGainRamp (pre.getWritePointer (c), numSamples, lastMute, muteGain);

        post.copyFrom (c, 0, pre, c, 0, numSamples);
        dsp::applyGainRamp (post.getWritePointer (c), numSamples, lastFader, faderGain);
    }

    lastMute = muteGain;
    lastFader = faderGain;

    const float pan = params.pan.load (std::memory_order_relaxed);
    if (numChannels == 1)
        dsp::constantPowerPan (pan, panLeft, panRight);
    else
        dsp::stereoBalance (pan, panLeft, panRight);

    main.copyFrom (0, 0, post, 0, 0, numSamples);
    main.copyFrom (1, 0, post, numChannels > 1 ? 1 : 0, 0, numSamples);
    dsp::applyGainRamp (main.getWritePointer (0), numSamples, lastPanLeft, panLeft);
    dsp::applyGainRamp (main.getWritePointer (1), numSamples, lastPanRight, panRight);
    lastPanLeft = panLeft;
    lastPanRight = panRight;

    for (int c = 0; c < 2; ++c)
        dsp::atomicMax (outputPeak[(size_t) c], dsp::peakLevel (main.getReadPointer (c), numSamples));
}

} // namespace livedaw
