#include "ui/Faceplate.h"
#include "ui/ModelParams.h"

#include <cmath>
#include <map>

namespace livedaw::ui
{

namespace
{
    using Knob = FaceplateStyle::Knob;
    using Meter = FaceplateStyle::Meter;
    using Layout = FaceplateStyle::Layout;

    constexpr float kStartAngle = -2.35619449f; // -135 degrees
    constexpr float kSweep = 4.71238898f;       // 270 degrees

    /** A style in one line: panel, text, knob, cap/accent colour, title. */
    FaceplateStyle make (juce::uint32 panel, juce::uint32 text, Knob knob, juce::uint32 cap, const char* title)
    {
        FaceplateStyle s;
        s.panel = juce::Colour (panel);
        s.text = juce::Colour (text);
        s.knob = knob;
        s.cap = juce::Colour (cap);
        s.accent = juce::Colour (cap);
        s.title = title;
        return s;
    }

    FaceplateStyle with (FaceplateStyle s, std::function<void (FaceplateStyle&)> f)
    {
        f (s);
        return s;
    }

    constexpr juce::uint32 white = 0xffeeeeee, ink = 0xff222222;

    const std::map<juce::String, FaceplateStyle>& fxStyles()
    {
        static const std::map<juce::String, FaceplateStyle> styles = []
        {
            std::map<juce::String, FaceplateStyle> m;
            // The 480L family: cream remote with a display and faders.
            auto larc = [] (const char* title)
            {
                return with (make (0xffe9e2cf, ink, Knob::Black, 0xff8a1a1a, title), [] (FaceplateStyle& s)
                {
                    s.layout = Layout::Faders;
                    s.display = true;
                    s.lcd = juce::Colour (0xff1a1f1c);
                    s.lcdText = juce::Colour (0xffd9e6cf);
                });
            };
            for (auto [t, n] : { std::pair { "HALL", "HALL" }, { "ROOM", "ROOM" }, { "CHAMBER", "CHAMBER" }, { "PLATE", "PLATE" },
                                 { "CONCERT", "CONCERT" }, { "AMBI", "AMBIENCE" } })
                m[t] = larc (n);
            m["VSS3"] = with (make (0xffc9ccd0, ink, Knob::Chrome, 0xff2a6ad0, "VSS3 REVERB"), [] (FaceplateStyle& s)
            {
                s.layout = Layout::Faders;
                s.brushed = 0.6f;
                s.display = true;
                s.lcd = juce::Colour (0xff10151c);
                s.lcdText = juce::Colour (0xff8ad0ff);
            });
            m["V-ROOM"] = with (make (0xff1b1d22, 0xffd0d0d0, Knob::Black, 0xffff3b30, "VINTAGE ROOM"), [] (FaceplateStyle& s)
            {
                s.display = true;
                s.lcd = juce::Colour (0xff0a0a0a);
                s.lcdText = juce::Colour (0xffff3b30);
            });
            m["V-REV"] = with (make (0xff161616, 0xffdddddd, Knob::Black, 0xffd02020, "VINTAGE REVERB"), [] (FaceplateStyle& s)
            {
                s.layout = Layout::Faders;
                s.meter = Meter::Led;
            });
            m["V-PLATE"] = with (make (0xffd8d8d4, ink, Knob::Chrome, 0xffc03020, "VINTAGE PLATE"), [] (FaceplateStyle& s) { s.meter = Meter::Vu; });
            auto native = [] (const char* title)
            {
                return with (make (0xff222222, 0xffd8d8d8, Knob::Black, 0xff7cff9a, title), [] (FaceplateStyle& s)
                {
                    s.display = true;
                    s.lcd = juce::Colour (0xff0c120d);
                    s.lcdText = juce::Colour (0xff7cff9a);
                });
            };
            m["GATED"] = native ("GATED REVERB");
            m["REVERSE"] = native ("REVERSE REVERB");
            auto pcm = [] (juce::uint32 panel, const char* title)
            {
                return with (make (panel, white, Knob::Black, 0xff3fc8ff, title), [] (FaceplateStyle& s)
                {
                    s.display = true;
                    s.lcd = juce::Colour (0xff081018);
                    s.lcdText = juce::Colour (0xff6fd8ff);
                });
            };
            m["DEL/REV"] = pcm (0xff1f5fbf, "DUAL FX");
            m["BPLATE"] = pcm (0xff1d4fa8, "BLUE PLATE");
            m["PITCH"] = pcm (0xff1b1f2a, "STEREO PITCH");
            m["D-PITCH"] = with (make (0xffcfd2d6, ink, Knob::Chrome, 0xffd03020, "DUAL PITCH"), [] (FaceplateStyle& s)
            {
                s.brushed = 0.5f;
                s.display = true;
                s.lcd = juce::Colour (0xff140808);
                s.lcdText = juce::Colour (0xffff5040);
            });
            m["SHIMMER"] = with (make (0xff3b2a7a, white, Knob::Chrome, 0xffb070ff, "SHIMMER"), [] (FaceplateStyle& s) { s.rackEars = false; });
            m["SPRING"] = with (make (0xffeeeeea, ink, Knob::White, 0xff2a8a3a, "SPRING REVERB"), [] (FaceplateStyle& s)
            {
                s.wood = true;
                s.display = true;
                s.lcd = juce::Colour (0xff1c5a28);
                s.lcdText = juce::Colour (0xffb8ffc0);
            });
            m["DIMCRS"] = with (make (0xff1a1a1a, white, Knob::Black, 0xffff4030, "DIMENSION"), [] (FaceplateStyle& s) { s.meter = Meter::Led; });
            m["CHORUS"] = with (make (0xff22b8c8, ink, Knob::Pedal, 0xff1a1a1a, "CHORUS"), [] (FaceplateStyle& s) { s.rackEars = false; });
            m["FLANGER"] = with (make (0xffe8b820, ink, Knob::White, 0xff1a1a1a, "FLANGER"), [] (FaceplateStyle& s) { s.rackEars = false; });
            m["ST-DL"] = make (0xff2e9e3e, ink, Knob::Black, 0xff111111, "WING DELAY");
            m["TAP-DL"] = make (0xff2a2c30, white, Knob::Cap, 0xffff8a1a, "ULTRATAP");
            m["TAPE-DL"] = with (make (0xff3d4a3e, white, Knob::Chrome, 0xffd0b060, "TAPE-ECHO"), [] (FaceplateStyle& s) { s.meter = Meter::Vu; });
            m["OILCAN"] = with (make (0xffd5d5d5, ink, Knob::Chrome, 0xff6b3a1a, "OIL CAN"), [] (FaceplateStyle& s) { s.wood = true; });
            m["BBD-DL"] = with (make (0xffc23bd8, ink, Knob::Pedal, 0xff1a1a1a, "BBD DELAY"), [] (FaceplateStyle& s) { s.rackEars = false; });
            // Standard devices.
            m["GEQ"] = with (make (0xff3e4f7a, white, Knob::Black, 0xffe8c040, "GRAPHIC EQ"), [] (FaceplateStyle& s)
            {
                s.layout = Layout::Faders;
                s.brushed = 0.4f;
            });
            m["PIA"] = with (make (0xff151515, white, Knob::Black, 0xffd8c040, "PIA 560"), [] (FaceplateStyle& s) { s.layout = Layout::Faders; });
            m["DOUBLE"] = make (0xff2a2d33, white, Knob::Cap, 0xffff8a00, "DOUBLE VOCAL");
            m["PCORR"] = make (0xff1e2f33, white, Knob::Cap, 0xff2bb3c0, "PITCH FIX");
            m["LIMITER"] = with (make (0xffe6e6e6, ink, Knob::Chrome, 0xff1a1a1a, "PRECISION LIMITER"), [] (FaceplateStyle& s)
            {
                s.meter = Meter::Led;
                s.brushed = 0.3f;
            });
            m["DE-S2"] = with (make (0xffcfcfcf, ink, Knob::Black, 0xff30a0ff, "DUAL DE-ESSER"), [] (FaceplateStyle& s)
            {
                s.meter = Meter::Led;
                s.brushed = 0.5f;
            });
            m["ENHANCE"] = with (make (0xffd9a35a, ink, Knob::White, 0xff5a3010, "ULTRA ENHANCER"), [] (FaceplateStyle& s)
            {
                s.meter = Meter::Vu;
                s.wood = true;
            });
            m["EXCITER"] = make (0xfff0f0f0, ink, Knob::Cap, 0xff2fae4a, "EXCITER");
            m["P-BASS"] = with (make (0xff1b2340, white, Knob::Black, 0xff4a7ad0, "PSYCHO BASS"), [] (FaceplateStyle& s) { s.meter = Meter::Vu; });
            m["ROTARY"] = with (make (0xff6b3d22, white, Knob::Chrome, 0xffe0c080, "ROTARY"), [] (FaceplateStyle& s) { s.wood = true; });
            m["PHASER"] = make (0xff5b7aa0, white, Knob::White, 0xffe85a30, "MULTI PHASER");
            m["PANNER"] = make (0xff7a7a7a, white, Knob::Chrome, 0xffc06a30, "TREMOLO");
            m["TAPE"] = with (make (0xffe8e8e8, ink, Knob::Chrome, 0xff303030, "TAPE MACHINE"), [] (FaceplateStyle& s) { s.meter = Meter::Vu; });
            m["MOOD"] = make (0xff1c2b4a, white, Knob::Cap, 0xff6aa8ff, "MOOD FILTER");
            m["SUB"] = with (make (0xff7a2a24, white, Knob::Pedal, 0xff1a1a1a, "SUB OCTAVER"), [] (FaceplateStyle& s) { s.rackEars = false; });
            m["SUB-M"] = with (make (0xff6b7d96, white, Knob::White, 0xff1a2a40, "SUB MONSTER"), [] (FaceplateStyle& s) { s.layout = Layout::Faders; });
            m["V-IMG"] = with (make (0xff6a1f2a, white, Knob::Black, 0xffff4060, "VELVET IMAGER"), [] (FaceplateStyle& s) { s.meter = Meter::Led; });
            m["SPKMAN"] = make (0xff1d2b4a, white, Knob::Black, 0xff40a0ff, "SPEAKER MANAGER");
            m["DEQ3"] = make (0xff5a5a3a, white, Knob::Chrome, 0xffe8c040, "TRIPLE DEQ");
            m["C5-CMB"] = with (make (0xff9a9a9a, ink, Knob::Cap, 0xff7a4ad0, "COMBINATOR"), [] (FaceplateStyle& s)
            {
                s.meter = Meter::Led;
                s.brushed = 0.4f;
            });
            m["BODY"] = with (make (0xff3a2a20, white, Knob::Black, 0xffd0a060, "BODYREZ"), [] (FaceplateStyle& s) { s.wood = true; });
            m["RACKAMP"] = with (make (0xff2b2722, white, Knob::Cap, 0xffe8c020, "RACK AMP"), [] (FaceplateStyle& s) { s.wood = true; });
            m["UKROCK"] = make (0xff141414, 0xffd8b860, Knob::Cap, 0xffc8a24a, "UK ROCK");
            m["ANGEL"] = make (0xff1a1a1a, white, Knob::Black, 0xffe02020, "ANGEL");
            m["JAZZC"] = make (0xff121212, white, Knob::Chrome, 0xffc0c0c0, "JAZZ CLEAN");
            m["DELUXE"] = make (0xff161616, white, Knob::Skirt, 0xffd8d8d8, "DELUXE");
            // EQ models and channel strips.
            m["SOUL"] = with (make (0xff8a8d90, ink, Knob::Skirt, 0xff3a6ad8, "SOUL ANALOGUE"), [] (FaceplateStyle& s) { s.brushed = 0.4f; });
            m["E88"] = make (0xff2c3b33, white, Knob::Cap, 0xff3fb8e0, "EVEN 88 FORMANT");
            m["E84"] = make (0xff2a3448, white, Knob::Cap, 0xff3fb8e0, "EVEN 84");
            m["F110"] = make (0xff1f3fa0, white, Knob::Cap, 0xffe8d040, "FORTISSIMO 110");
            m["PULSAR"] = make (0xff4d5f70, white, Knob::Black, 0xffe86a30, "PULSAR EQ");
            m["MACH4"] = make (0xff2f9ae0, white, Knob::Cap, 0xffff7a2a, "EQ4");
            m["*EVEN*"] = with (make (0xff2a3a32, white, Knob::Cap, 0xff3fb8e0, "EVEN CHANNEL"), [] (FaceplateStyle& s) { s.meter = Meter::Led; });
            m["*SOUL*"] = with (make (0xff7e8186, ink, Knob::Skirt, 0xff3a6ad8, "SOUL CHANNEL"), [] (FaceplateStyle& s)
            {
                s.meter = Meter::Led;
                s.brushed = 0.4f;
            });
            m["*VINTAGE*"] = with (make (0xff1a1a1a, white, Knob::Chrome, 0xffd0b060, "VINTAGE CHANNEL"), [] (FaceplateStyle& s) { s.meter = Meter::Vu; });
            m["*BUS*"] = with (make (0xff202226, white, Knob::Cap, 0xff3fa0ff, "BUS CHANNEL"), [] (FaceplateStyle& s) { s.meter = Meter::Vu; });
            m["*MASTER*"] = with (make (0xffd0d2d6, ink, Knob::Chrome, 0xff2a2a2a, "MASTERING"), [] (FaceplateStyle& s)
            {
                s.meter = Meter::Led;
                s.brushed = 0.6f;
            });
            return m;
        }();
        return styles;
    }

    const std::map<juce::String, FaceplateStyle>& dynStyles()
    {
        static const std::map<juce::String, FaceplateStyle> styles = []
        {
            std::map<juce::String, FaceplateStyle> m;
            auto native = [] (const char* title) { return make (0xff262626, 0xffd8d8d8, Knob::Black, 0xfff5e14a, title); };
            m["GATE"] = native ("GATE / EXPANDER");
            m["DUCK"] = native ("DUCKER");
            m["COMP"] = native ("WING COMPRESSOR");
            m["EXP"] = native ("WING EXPANDER");
            m["LMT"] = native ("TRANSIENT COMP");
            m["ONEC"] = native ("ONE-KNOB COMP");
            auto soul = [] (const char* title)
            {
                return with (make (0xff6e7074, white, Knob::Skirt, 0xff2a2a2a, title), [] (FaceplateStyle& s)
                {
                    s.meter = Meter::Led;
                    s.brushed = 0.5f;
                });
            };
            m["9000G"] = soul ("SOUL 9000 GATE");
            m["9000C"] = soul ("SOUL 9000");
            auto even = [] (const char* title)
            {
                return with (make (0xff2a3a32, white, Knob::Cap, 0xff2f9e4a, title), [] (FaceplateStyle& s) { s.meter = Meter::Led; });
            };
            m["E88"] = even ("EVEN 88 GATE");
            m["E88C"] = even ("EVEN 88 COMP");
            m["D241G"] = make (0xff3a3a3a, white, Knob::Cap, 0xffe8d43a, "DRAW MORE 241");
            m["D241C"] = with (make (0xff3a3a3a, white, Knob::Black, 0xffe8d43a, "DRAW MORE COMP"), [] (FaceplateStyle& s) { s.meter = Meter::Led; });
            m["DS902"] = with (make (0xff141414, white, Knob::Cap, 0xffe8d43a, "BDX 902 DE-ESSER"), [] (FaceplateStyle& s) { s.meter = Meter::Led; });
            m["WAVE"] = with (make (0xffc0c0c0, ink, Knob::Chrome, 0xff1a1a1a, "WAVE DESIGNER"), [] (FaceplateStyle& s) { s.brushed = 0.7f; });
            m["PSE"] = make (0xffaecbe0, ink, Knob::Black, 0xff1a3a60, "SOURCE EXTRACTOR");
            m["CMB"] = with (make (0xffaecbe0, ink, Knob::Black, 0xff1a3a60, "EXTRACTOR + LEVELER"), [] (FaceplateStyle& s) { s.meter = Meter::Vu; });
            m["RIDE"] = with (make (0xffd0d0d0, ink, Knob::Black, 0xff1a1a1a, "AUTO RIDER"), [] (FaceplateStyle& s) { s.meter = Meter::Led; });
            m["WARM"] = make (0xff26282c, white, Knob::Chrome, 0xffe02020, "SOUL WARMTH");
            m["DEQ"] = make (0xff5a5640, white, Knob::Chrome, 0xffe8c040, "DYNAMIC EQ");
            m["DEQ2"] = make (0xff5a5640, white, Knob::Chrome, 0xffe8c040, "DUAL DYNAMIC EQ");
            m["B160"] = with (make (0xff1c1c1c, white, Knob::Chrome, 0xffff8a20, "BDX 160"), [] (FaceplateStyle& s)
            {
                s.meter = Meter::Vu;
                s.wood = true;
            });
            m["B560"] = with (make (0xff161616, white, Knob::Cap, 0xffe03030, "BDX 560 EASY"), [] (FaceplateStyle& s) { s.meter = Meter::Led; });
            m["ECL33"] = with (make (0xff28344a, white, Knob::Cap, 0xff3a8ae0, "EVEN COMP/LIM"), [] (FaceplateStyle& s) { s.meter = Meter::Vu; });
            m["SBUS"] = with (make (0xff202226, white, Knob::Cap, 0xff3fa0ff, "SOUL BUS COMP"), [] (FaceplateStyle& s) { s.meter = Meter::Vu; });
            m["RED3"] = with (make (0xffa0201c, white, Knob::Chrome, 0xffffffff, "RED3 COMPRESSOR"), [] (FaceplateStyle& s) { s.meter = Meter::Vu; });
            m["76LA"] = with (make (0xff1a1a1a, white, Knob::Chrome, 0xffd0b060, "76 LIMITING AMPLIFIER"), [] (FaceplateStyle& s)
            {
                s.meter = Meter::Vu;
                s.brushed = 0.3f;
            });
            m["LA"] = with (make (0xffd6d6d2, ink, Knob::Black, 0xffc03020, "LEVELING AMPLIFIER"), [] (FaceplateStyle& s) { s.meter = Meter::Vu; });
            m["F670"] = with (make (0xff1c1c1c, white, Knob::Black, 0xffe04020, "FAIR KID"), [] (FaceplateStyle& s) { s.meter = Meter::Vu; });
            m["BLISS"] = make (0xff1d2f5a, white, Knob::Chrome, 0xffd0d0d0, "ETERNAL BLISS");
            m["NSTR"] = with (make (0xff161616, white, Knob::Chrome, 0xffe02020, "NO-STRESSOR"), [] (FaceplateStyle& s) { s.meter = Meter::Led; });
            m["2250"] = with (make (0xff1e3a6a, white, Knob::Cap, 0xff2bb3c0, "PIA 2250"), [] (FaceplateStyle& s) { s.meter = Meter::Led; });
            m["L100"] = with (make (0xffaab8c8, ink, Knob::Cap, 0xff2a5aa0, "LTA100 LEVELER"), [] (FaceplateStyle& s) { s.meter = Meter::Vu; });
            return m;
        }();
        return styles;
    }

    juce::String fit (juce::String s)
    {
        return s.toUpperCase();
    }
} // namespace

FaceplateStyle faceplateStyle (models::Table table, const juce::String& token)
{
    const auto& styles = table == models::Fx ? fxStyles() : (table == models::Dynamics ? dynStyles() : fxStyles());
    if (auto it = styles.find (token); it != styles.end())
        return it->second;
    FaceplateStyle s;
    s.title = token;
    return s;
}

//==============================================================================
Faceplate::Faceplate()
{
    startTimerHz (15);
}

void Faceplate::bind (models::Table t, int model, std::atomic<float>* v, std::function<float()> meter)
{
    table = t;
    modelIndex = model;
    values = v;
    meterFn = std::move (meter);
    spec = &models::model (t, model);
    style = faceplateStyle (t, spec->token);
    if (style.title.isEmpty())
        style.title = fit (spec->name);
    shown.assign (spec->params.size(), std::numeric_limits<float>::quiet_NaN());
    lastTouched = -1;
    layoutControls();
    repaint();
}

void Faceplate::unbind()
{
    spec = nullptr;
    values = nullptr;
    meterFn = {};
    controls.clear();
    repaint();
}

float Faceplate::value (int param) const
{
    return values != nullptr ? values[param].load() : 0.0f;
}

void Faceplate::setValue (int param, float v)
{
    if (values == nullptr || spec == nullptr)
        return;
    const auto& p = spec->params[(size_t) param];
    values[param].store (p.fromNormalised (p.toNormalised (v)));
}

void Faceplate::touch (int param)
{
    lastTouched = param;
    if (onTouched)
        onTouched (param);
}

void Faceplate::timerCallback()
{
    if (spec == nullptr || values == nullptr)
        return;
    bool changed = false;
    for (size_t i = 0; i < shown.size(); ++i)
    {
        const float v = values[i].load();
        if (! (v == shown[i]))
        {
            shown[i] = v;
            changed = true;
        }
    }
    const float m = meterFn ? meterFn() : 0.0f;
    if (std::abs (m - shownMeter) > 0.1f)
    {
        shownMeter = m;
        changed = true;
    }
    if (changed)
        repaint();
}

void Faceplate::resized()
{
    layoutControls();
}

void Faceplate::layoutControls()
{
    controls.clear();
    auto b = getLocalBounds().toFloat().reduced (2.0f);
    panelArea = b;
    if (spec == nullptr)
        return;

    auto r = b;
    if (style.wood)
        r = r.reduced (r.getWidth() * 0.035f, 0.0f);
    if (style.rackEars)
        r = r.reduced (juce::jmin (22.0f, r.getWidth() * 0.04f), 0.0f);
    r = r.reduced (8.0f, 6.0f);

    titleArea = r.removeFromTop (juce::jlimit (16.0f, 30.0f, r.getHeight() * 0.11f));
    r.removeFromTop (4.0f);
    displayArea = {};
    if (style.display)
    {
        displayArea = r.removeFromTop (juce::jlimit (26.0f, 54.0f, r.getHeight() * 0.18f));
        r.removeFromTop (6.0f);
    }
    meterArea = {};
    if (style.meter == FaceplateStyle::Meter::Vu)
    {
        meterArea = r.removeFromRight (juce::jmin (r.getWidth() * 0.26f, r.getHeight() * 1.2f)).withSizeKeepingCentre (
            juce::jmin (r.getWidth() * 0.26f, r.getHeight() * 1.2f), juce::jmin (r.getHeight() * 0.8f, 140.0f));
        r.removeFromRight (8.0f);
    }
    else if (style.meter == FaceplateStyle::Meter::Led)
    {
        meterArea = r.removeFromRight (juce::jlimit (10.0f, 18.0f, r.getWidth() * 0.025f));
        r.removeFromRight (8.0f);
    }

    const auto& params = spec->params;
    const int n = (int) params.size();
    if (n == 0)
        return;

    auto kindOf = [] (const models::Param& p)
    {
        if (p.kind == models::ParamKind::Toggle)
            return Kind::Switch;
        if (p.kind == models::ParamKind::Choice && p.choices.size() <= 4)
            return Kind::Selector;
        return Kind::Knob;
    };

    if (style.layout == FaceplateStyle::Layout::Faders)
    {
        // Continuous controls as faders; switches and selectors in a row below.
        std::vector<int> faders, others;
        for (int i = 0; i < n; ++i)
        {
            const auto& p = params[(size_t) i];
            (p.isContinuous() || p.kind == models::ParamKind::Integer ? faders : others).push_back (i);
        }
        auto row = others.empty() ? juce::Rectangle<float>() : r.removeFromBottom (juce::jlimit (24.0f, 44.0f, r.getHeight() * 0.2f));
        const float fw = r.getWidth() / (float) juce::jmax (1, (int) faders.size());
        for (size_t k = 0; k < faders.size(); ++k)
            controls.push_back ({ faders[k], Kind::Fader, juce::Rectangle<float> (r.getX() + fw * (float) k, r.getY(), fw, r.getHeight()) });
        const float ow = row.getWidth() / (float) juce::jmax (1, (int) others.size());
        for (size_t k = 0; k < others.size(); ++k)
        {
            const auto& p = params[(size_t) others[k]];
            controls.push_back ({ others[k], kindOf (p), juce::Rectangle<float> (row.getX() + ow * (float) k, row.getY(), ow, row.getHeight()) });
        }
        return;
    }

    // Knobs: a grid whose cells are about as wide as they are tall.
    const float aspect = r.getWidth() / juce::jmax (1.0f, r.getHeight());
    int cols = juce::jlimit (1, n, (int) std::ceil (std::sqrt ((float) n * aspect * 0.9f)));
    int rows = (n + cols - 1) / cols;
    while (rows > 1 && (rows - 1) * cols >= n)
        --rows;
    const float cw = r.getWidth() / (float) cols, ch = r.getHeight() / (float) rows;
    for (int i = 0; i < n; ++i)
    {
        const int row = i / cols, col = i % cols;
        // Centre a short last row.
        const int inRow = row == rows - 1 ? n - row * cols : cols;
        const float offset = (float) (cols - inRow) * cw * 0.5f;
        controls.push_back ({ i, kindOf (params[(size_t) i]),
                              juce::Rectangle<float> (r.getX() + offset + cw * (float) col, r.getY() + ch * (float) row, cw, ch) });
    }
}

int Faceplate::controlAt (juce::Point<float> p) const
{
    for (size_t i = 0; i < controls.size(); ++i)
        if (controls[i].area.contains (p))
            return (int) i;
    return -1;
}

//==============================================================================
static void paintPanel (juce::Graphics& g, juce::Rectangle<float> b, const FaceplateStyle& style, float titleH, const juce::String& title)
{
    // Rack body with ears, or a pedal / desktop unit.
    if (style.rackEars)
    {
        g.setColour (juce::Colour (0xff3a3c40));
        g.fillRoundedRectangle (b, 3.0f);
        const float ear = juce::jmin (22.0f, b.getWidth() * 0.04f);
        for (auto e : { b.withWidth (ear), b.withTrimmedLeft (b.getWidth() - ear) })
        {
            juce::ColourGradient eg (juce::Colour (0xff6a6c70), e.getX(), e.getY(), juce::Colour (0xff2c2d30), e.getRight(), e.getY(), false);
            g.setGradientFill (eg);
            g.fillRect (e);
            for (float y : { e.getY() + e.getHeight() * 0.18f, e.getBottom() - e.getHeight() * 0.18f })
            {
                const auto c = juce::Point<float> (e.getCentreX(), y);
                g.setColour (juce::Colour (0xff1a1a1a));
                g.fillEllipse (juce::Rectangle<float> (ear * 0.45f, ear * 0.45f).withCentre (c));
                g.setColour (juce::Colour (0xff8a8a8a));
                g.drawEllipse (juce::Rectangle<float> (ear * 0.45f, ear * 0.45f).withCentre (c), 1.0f);
            }
        }
        b = b.reduced (ear, 0.0f);
    }
    if (style.wood)
    {
        const float w = b.getWidth() * 0.035f;
        for (auto e : { b.withWidth (w), b.withTrimmedLeft (b.getWidth() - w) })
        {
            juce::ColourGradient wg (juce::Colour (0xff8a5a32), e.getX(), e.getY(), juce::Colour (0xff4a2a14), e.getRight(), e.getBottom(), false);
            g.setGradientFill (wg);
            g.fillRect (e);
            g.setColour (juce::Colour (0x30000000));
            for (int i = 1; i < 6; ++i)
                g.drawVerticalLine (juce::roundToInt (e.getX() + e.getWidth() * (float) i / 6.0f), e.getY(), e.getBottom());
        }
        b = b.reduced (w, 0.0f);
    }

    juce::ColourGradient grad (style.panel.brighter (0.12f), b.getX(), b.getY(), style.panel.darker (0.25f), b.getX(), b.getBottom(), false);
    g.setGradientFill (grad);
    g.fillRoundedRectangle (b, style.rackEars ? 1.0f : 6.0f);
    if (style.brushed > 0.0f)
    {
        juce::Random rnd (1234);
        for (float y = b.getY() + 1.0f; y < b.getBottom(); y += 1.5f)
        {
            g.setColour ((rnd.nextBool() ? juce::Colours::white : juce::Colours::black).withAlpha (0.035f * style.brushed + rnd.nextFloat() * 0.03f * style.brushed));
            g.drawHorizontalLine (juce::roundToInt (y), b.getX(), b.getRight());
        }
    }
    g.setColour (juce::Colours::black.withAlpha (0.5f));
    g.drawRoundedRectangle (b, style.rackEars ? 1.0f : 6.0f, 1.2f);

    if (title.isNotEmpty() && titleH > 0.0f)
    {
        auto t = b.reduced (12.0f, 6.0f).withHeight (titleH);
        g.setColour (style.text.withAlpha (0.9f));
        g.setFont (juce::FontOptions (juce::jlimit (6.0f, 22.0f, titleH * 0.8f), juce::Font::bold | juce::Font::italic)); // fits the title band on thumbnails
        g.drawFittedText (title, t.toNearestInt(), juce::Justification::centredLeft, 1);
        g.setColour (style.accent.withAlpha (0.8f));
        g.fillRect (t.withTop (t.getBottom() - 1.5f).withWidth (juce::jmin (t.getWidth(), 60.0f)));
    }
}

void Faceplate::paintThumbnail (juce::Graphics& g, juce::Rectangle<float> r, models::Table t, int model, const juce::String& caption)
{
    const auto& m = models::model (t, model);
    auto style = faceplateStyle (t, m.token);
    paintPanel (g, r, style, juce::jlimit (8.0f, 16.0f, r.getHeight() * 0.22f), caption.isNotEmpty() ? caption : style.title);

    // A few dummy controls so the unit reads as itself at a glance.
    auto inner = r.reduced (r.getWidth() * (style.rackEars ? 0.1f : 0.06f), r.getHeight() * 0.12f).withTrimmedTop (r.getHeight() * 0.25f);
    const int n = juce::jlimit (2, 6, (int) m.params.size());
    if (style.display)
    {
        auto d = inner.removeFromTop (inner.getHeight() * 0.35f).reduced (0.0f, 1.0f);
        g.setColour (style.lcd);
        g.fillRoundedRectangle (d, 2.0f);
        g.setColour (style.lcdText.withAlpha (0.8f));
        for (int i = 0; i < n; ++i)
            g.fillRect (juce::Rectangle<float> (d.getX() + d.getWidth() * ((float) i + 0.2f) / (float) n, d.getCentreY() - 1.5f, d.getWidth() * 0.5f / (float) n, 3.0f));
    }
    const float w = inner.getWidth() / (float) n;
    for (int i = 0; i < n; ++i)
    {
        auto cell = juce::Rectangle<float> (inner.getX() + w * (float) i, inner.getY(), w, inner.getHeight());
        if (style.layout == FaceplateStyle::Layout::Faders)
        {
            g.setColour (juce::Colours::black.withAlpha (0.7f));
            g.fillRect (cell.withSizeKeepingCentre (2.0f, cell.getHeight() * 0.85f));
            g.setColour (style.cap.withAlpha (0.9f));
            g.fillRect (cell.withSizeKeepingCentre (w * 0.5f, 4.0f).translated (0.0f, (i % 3 - 1) * cell.getHeight() * 0.2f));
        }
        else
        {
            const float d = juce::jmin (w * 0.7f, cell.getHeight() * 0.8f);
            auto k = cell.withSizeKeepingCentre (d, d);
            const auto base = style.knob == FaceplateStyle::Knob::Chrome || style.knob == FaceplateStyle::Knob::White
                                  ? juce::Colour (0xffd8d8d8)
                                  : (style.knob == FaceplateStyle::Knob::Cap ? style.cap : juce::Colour (0xff151515));
            g.setColour (base);
            g.fillEllipse (k);
            g.setColour (base.getPerceivedBrightness() > 0.5f ? juce::Colours::black : juce::Colours::white);
            const float a = kStartAngle + kSweep * (0.3f + 0.1f * (float) (i % 4));
            g.drawLine (k.getCentreX(), k.getCentreY(), k.getCentreX() + std::sin (a) * d * 0.4f, k.getCentreY() - std::cos (a) * d * 0.4f, 1.2f);
        }
    }
}

void Faceplate::paintKnob (juce::Graphics& g, juce::Rectangle<float> k, float pos, bool bipolar) const
{
    const auto c = k.getCentre();
    const float rad = k.getWidth() * 0.5f;
    const float angle = kStartAngle + kSweep * juce::jlimit (0.0f, 1.0f, pos);

    // Scale ticks.
    g.setColour (style.text.withAlpha (0.55f));
    for (int i = 0; i <= 10; ++i)
    {
        const float a = kStartAngle + kSweep * (float) i / 10.0f;
        const float r0 = rad * 1.12f, r1 = rad * (i == 0 || i == 10 || (bipolar && i == 5) ? 1.32f : 1.22f);
        g.drawLine (c.x + std::sin (a) * r0, c.y - std::cos (a) * r0, c.x + std::sin (a) * r1, c.y - std::cos (a) * r1, 1.0f);
    }

    juce::Colour body, pointer;
    switch (style.knob)
    {
        case Knob::Chrome:
        case Knob::White:
        {
            const bool chrome = style.knob == Knob::Chrome;
            juce::ColourGradient gr (chrome ? juce::Colour (0xfff4f4f4) : juce::Colour (0xfff6f2e8), c.x - rad * 0.4f, c.y - rad * 0.5f,
                                     chrome ? juce::Colour (0xff7a7c80) : juce::Colour (0xffbdb8aa), c.x + rad * 0.5f, c.y + rad * 0.7f, true);
            g.setGradientFill (gr);
            g.fillEllipse (k);
            if (chrome)
            {
                g.setColour (juce::Colour (0x50000000));
                for (int i = 0; i < 28; ++i)
                {
                    const float a = juce::MathConstants<float>::twoPi * (float) i / 28.0f;
                    g.drawLine (c.x + std::sin (a) * rad * 0.86f, c.y - std::cos (a) * rad * 0.86f, c.x + std::sin (a) * rad, c.y - std::cos (a) * rad, 1.0f);
                }
                juce::ColourGradient cap (juce::Colour (0xffe8e8e8), c.x, c.y - rad * 0.6f, juce::Colour (0xff9a9ca0), c.x, c.y + rad * 0.6f, false);
                g.setGradientFill (cap);
                g.fillEllipse (k.reduced (rad * 0.18f));
            }
            pointer = juce::Colour (0xff141414);
            break;
        }
        case Knob::Skirt:
        {
            juce::ColourGradient skirt (juce::Colour (0xff3a3a3a), c.x, c.y - rad, juce::Colour (0xff0a0a0a), c.x, c.y + rad, false);
            g.setGradientFill (skirt);
            g.fillEllipse (k);
            auto capArea = k.reduced (rad * 0.36f);
            juce::ColourGradient cap (style.cap.brighter (0.4f), capArea.getX(), capArea.getY(), style.cap.darker (0.5f), capArea.getRight(),
                                      capArea.getBottom(), false);
            g.setGradientFill (cap);
            g.fillEllipse (capArea);
            pointer = juce::Colours::white;
            break;
        }
        case Knob::Cap:
        {
            juce::ColourGradient cap (style.cap.brighter (0.35f), c.x - rad * 0.5f, c.y - rad * 0.6f, style.cap.darker (0.55f), c.x + rad * 0.5f,
                                      c.y + rad * 0.7f, true);
            g.setGradientFill (cap);
            g.fillEllipse (k);
            g.setColour (juce::Colours::black.withAlpha (0.35f));
            g.drawEllipse (k, 1.0f);
            pointer = style.cap.getPerceivedBrightness() > 0.6f ? juce::Colour (0xff141414) : juce::Colours::white;
            break;
        }
        case Knob::Black:
        case Knob::Pedal:
        {
            juce::ColourGradient gr (juce::Colour (0xff4a4a4a), c.x, c.y - rad, juce::Colour (0xff070707), c.x, c.y + rad, false);
            g.setGradientFill (gr);
            g.fillEllipse (k);
            if (style.knob == Knob::Pedal)
            {
                g.setColour (juce::Colour (0xff2a2a2a));
                g.fillEllipse (k.reduced (rad * 0.22f));
            }
            g.setColour (juce::Colours::white.withAlpha (0.12f));
            g.drawEllipse (k.reduced (1.0f), 1.0f);
            pointer = juce::Colours::white;
            break;
        }
    }
    (void) body;
    g.setColour (pointer);
    g.drawLine (c.x + std::sin (angle) * rad * 0.2f, c.y - std::cos (angle) * rad * 0.2f, c.x + std::sin (angle) * rad * 0.88f,
                c.y - std::cos (angle) * rad * 0.88f, juce::jmax (1.5f, rad * 0.11f));
}

void Faceplate::paintControl (juce::Graphics& g, const Control& ctl) const
{
    const auto& p = spec->params[(size_t) ctl.param];
    const float v = value (ctl.param);
    const float pos = p.toNormalised (v);
    auto a = ctl.area.reduced (3.0f, 2.0f);
    const bool touched = ctl.param == lastTouched;
    const float labelH = juce::jlimit (10.0f, 15.0f, a.getHeight() * 0.16f);
    const auto label = fit (p.label.isNotEmpty() ? consoleLabel (p) : p.id);

    switch (ctl.kind)
    {
        case Kind::Knob:
        {
            auto lab = a.removeFromTop (labelH);
            auto val = a.removeFromBottom (labelH);
            g.setColour (style.text);
            g.setFont (juce::FontOptions (labelH * 0.92f, juce::Font::bold));
            g.drawFittedText (label, lab.toNearestInt(), juce::Justification::centred, 1, 0.7f);
            const float d = juce::jmin (a.getWidth() * 0.62f, a.getHeight() * 0.78f);
            auto k = a.withSizeKeepingCentre (d, d);
            paintKnob (g, k, pos, p.min < 0.0f && p.max > 0.0f && p.isContinuous());
            g.setColour (touched ? style.accent : style.text.withAlpha (0.85f));
            g.setFont (juce::FontOptions (labelH * 0.92f, juce::Font::bold));
            g.drawFittedText (consoleText (p, v), val.toNearestInt(), juce::Justification::centred, 1, 0.7f);
            break;
        }
        case Kind::Fader:
        {
            auto val = a.removeFromTop (labelH);
            auto lab = a.removeFromBottom (labelH);
            g.setColour (touched ? style.accent : style.text.withAlpha (0.85f));
            g.setFont (juce::FontOptions (labelH * 0.85f, juce::Font::bold));
            g.drawFittedText (consoleText (p, v), val.toNearestInt(), juce::Justification::centred, 1, 0.6f);
            g.setColour (style.text);
            g.drawFittedText (label, lab.toNearestInt(), juce::Justification::centred, 1, 0.6f);
            auto slot = a.reduced (0.0f, 6.0f);
            g.setColour (juce::Colours::black.withAlpha (0.75f));
            g.fillRoundedRectangle (slot.withSizeKeepingCentre (juce::jmax (3.0f, slot.getWidth() * 0.08f), slot.getHeight()), 1.5f);
            if (p.min < 0.0f && p.max > 0.0f)
            {
                g.setColour (style.text.withAlpha (0.4f));
                g.drawHorizontalLine (juce::roundToInt (slot.getCentreY()), slot.getCentreX() - slot.getWidth() * 0.3f, slot.getCentreX() + slot.getWidth() * 0.3f);
            }
            const float capH = juce::jlimit (8.0f, 22.0f, slot.getHeight() * 0.09f);
            const float y = slot.getBottom() - capH * 0.5f - pos * (slot.getHeight() - capH);
            auto cap = juce::Rectangle<float> (juce::jmin (slot.getWidth() * 0.7f, 30.0f), capH).withCentre ({ slot.getCentreX(), y });
            juce::ColourGradient cg (style.cap.brighter (0.4f), cap.getX(), cap.getY(), style.cap.darker (0.4f), cap.getX(), cap.getBottom(), false);
            g.setGradientFill (cg);
            g.fillRoundedRectangle (cap, 2.0f);
            g.setColour (juce::Colours::black.withAlpha (0.6f));
            g.drawHorizontalLine (juce::roundToInt (cap.getCentreY()), cap.getX() + 2.0f, cap.getRight() - 2.0f);
            break;
        }
        case Kind::Switch:
        {
            auto lab = a.removeFromBottom (labelH);
            const float s = juce::jmin (a.getWidth() * 0.5f, a.getHeight() * 0.6f, 34.0f);
            auto key = a.withSizeKeepingCentre (s, s);
            const bool on = v >= 0.5f;
            juce::ColourGradient kg (juce::Colour (0xff5a5a5a), key.getX(), key.getY(), juce::Colour (0xff1a1a1a), key.getX(), key.getBottom(), false);
            g.setGradientFill (kg);
            g.fillRoundedRectangle (key, 3.0f);
            auto led = key.reduced (s * 0.32f);
            g.setColour (on ? style.accent : juce::Colour (0xff2a2a2a));
            g.fillEllipse (led);
            if (on)
            {
                g.setColour (style.accent.withAlpha (0.3f));
                g.fillEllipse (led.expanded (s * 0.12f));
            }
            g.setColour (style.text);
            g.setFont (juce::FontOptions (labelH * 0.9f, juce::Font::bold));
            g.drawFittedText (label, lab.toNearestInt(), juce::Justification::centred, 1, 0.6f);
            break;
        }
        case Kind::Selector:
        {
            auto lab = a.removeFromTop (labelH);
            g.setColour (style.text);
            g.setFont (juce::FontOptions (labelH * 0.9f, juce::Font::bold));
            g.drawFittedText (label, lab.toNearestInt(), juce::Justification::centred, 1, 0.6f);
            const int n = juce::jmax (1, p.choices.size());
            const int current = juce::jlimit (0, n - 1, juce::roundToInt (v - p.min));
            auto keys = a.withSizeKeepingCentre (a.getWidth(), juce::jmin (a.getHeight(), labelH * 2.2f));
            const float w = keys.getWidth() / (float) n;
            for (int i = 0; i < n; ++i)
            {
                auto k = juce::Rectangle<float> (keys.getX() + w * (float) i, keys.getY(), w, keys.getHeight()).reduced (1.0f);
                const bool on = i == current;
                g.setColour (on ? style.accent : juce::Colour (0xff1e1e1e).withAlpha (0.85f));
                g.fillRoundedRectangle (k, 2.0f);
                g.setColour (on ? (style.accent.getPerceivedBrightness() > 0.55f ? juce::Colours::black : juce::Colours::white) : juce::Colour (0xffd0d0d0));
                g.setFont (juce::FontOptions (juce::jmin (labelH, k.getHeight() * 0.6f), juce::Font::bold));
                g.drawFittedText (p.choices[i].toUpperCase(), k.toNearestInt(), juce::Justification::centred, 1, 0.6f);
            }
            break;
        }
    }
}

void Faceplate::paint (juce::Graphics& g)
{
    if (spec == nullptr)
        return;
    paintPanel (g, panelArea, style, titleArea.getHeight(), style.title);

    // Display strip: the first controls' values, as the unit's own display shows them.
    if (! displayArea.isEmpty())
    {
        g.setColour (style.lcd);
        g.fillRoundedRectangle (displayArea, 3.0f);
        g.setColour (juce::Colours::black.withAlpha (0.6f));
        g.drawRoundedRectangle (displayArea, 3.0f, 1.0f);
        const int n = juce::jmin (6, (int) spec->params.size());
        const float w = displayArea.getWidth() / (float) juce::jmax (1, n);
        for (int i = 0; i < n; ++i)
        {
            auto cell = juce::Rectangle<float> (displayArea.getX() + w * (float) i, displayArea.getY(), w, displayArea.getHeight()).reduced (4.0f, 3.0f);
            const auto& p = spec->params[(size_t) i];
            g.setColour (style.lcdText.withAlpha (0.65f));
            g.setFont (juce::FontOptions (juce::jmin (12.0f, cell.getHeight() * 0.36f), juce::Font::bold));
            g.drawFittedText (fit (consoleLabel (p)), cell.removeFromTop (cell.getHeight() * 0.42f).toNearestInt(), juce::Justification::centredLeft, 1, 0.6f);
            g.setColour (style.lcdText);
            g.setFont (juce::FontOptions (juce::jmin (20.0f, cell.getHeight() * 0.9f), juce::Font::bold));
            g.drawFittedText (consoleText (p, value (i)), cell.toNearestInt(), juce::Justification::centredLeft, 1, 0.6f);
        }
    }

    // Meters show gain reduction.
    const float gr = juce::jlimit (0.0f, 24.0f, -shownMeter);
    if (style.meter == FaceplateStyle::Meter::Vu && ! meterArea.isEmpty())
    {
        auto m = meterArea;
        g.setColour (juce::Colour (0xff141414));
        g.fillRoundedRectangle (m, 4.0f);
        // A landscape meter face, backlit amber as on the units' panels.
        auto face = m.reduced (5.0f);
        face = face.withSizeKeepingCentre (face.getWidth(), juce::jmin (face.getHeight(), face.getWidth() * 0.7f));
        juce::ColourGradient fg (juce::Colour (0xfff8dc8c), face.getCentreX(), face.getY(), juce::Colour (0xffd9a144), face.getCentreX(), face.getBottom(), false);
        g.setGradientFill (fg);
        g.fillRoundedRectangle (face, 3.0f);
        const auto pivot = juce::Point<float> (face.getCentreX(), face.getBottom() + face.getHeight() * 0.18f);
        const float len = juce::jmin (face.getHeight() * 1.05f, face.getWidth() * 0.62f);
        g.saveState();
        g.reduceClipRegion (face.toNearestInt());
        g.setColour (juce::Colour (0xff2a2a2a));
        for (int i = 0; i <= 10; ++i)
        {
            const float a = -0.75f + 1.5f * (float) i / 10.0f;
            const float r0 = len * 0.82f, r1 = len * (i % 5 == 0 ? 0.95f : 0.9f);
            g.setColour (i >= 8 ? juce::Colour (0xffc02020) : juce::Colour (0xff2a2a2a));
            g.drawLine (pivot.x + std::sin (a) * r0, pivot.y - std::cos (a) * r0, pivot.x + std::sin (a) * r1, pivot.y - std::cos (a) * r1, 1.2f);
        }
        g.setColour (juce::Colour (0xff3a3a3a));
        g.setFont (juce::FontOptions (juce::jmin (12.0f, face.getHeight() * 0.16f), juce::Font::bold));
        g.drawText ("GAIN REDUCTION", face.withTrimmedTop (face.getHeight() * 0.62f).toNearestInt(), juce::Justification::centred);
        // Rest at 0 (right of centre), falling left with reduction.
        const float a = 0.45f - 1.2f * (gr / 20.0f);
        g.setColour (juce::Colour (0xff111111));
        g.drawLine (pivot.x, pivot.y, pivot.x + std::sin (a) * len * 0.93f, pivot.y - std::cos (a) * len * 0.93f, 1.6f);
        g.restoreState();
    }
    else if (style.meter == FaceplateStyle::Meter::Led && ! meterArea.isEmpty())
    {
        auto m = meterArea;
        g.setColour (juce::Colour (0xff0e0e0e));
        g.fillRoundedRectangle (m, 2.0f);
        const int segs = 12;
        const float h = (m.getHeight() - 4.0f) / (float) segs;
        for (int i = 0; i < segs; ++i)
        {
            const bool lit = gr > (float) i * 2.0f;
            g.setColour (lit ? (i < 4 ? juce::Colour (0xfff5c030) : juce::Colour (0xffff5030)) : juce::Colour (0xff2a2a2a));
            g.fillRect (juce::Rectangle<float> (m.getX() + 2.0f, m.getY() + 2.0f + h * (float) i, m.getWidth() - 4.0f, h - 1.5f));
        }
    }

    for (auto& c : controls)
        paintControl (g, c);
}

//==============================================================================
void Faceplate::mouseDown (const juce::MouseEvent& e)
{
    dragging = -1;
    const int i = controlAt (e.position);
    if (i < 0 || spec == nullptr)
        return;
    const auto& ctl = controls[(size_t) i];
    const auto& p = spec->params[(size_t) ctl.param];
    touch (ctl.param);

    if (ctl.kind == Kind::Switch)
    {
        setValue (ctl.param, value (ctl.param) >= 0.5f ? 0.0f : 1.0f);
    }
    else if (ctl.kind == Kind::Selector)
    {
        const int n = juce::jmax (1, p.choices.size());
        auto keys = ctl.area.reduced (3.0f, 2.0f);
        const int k = juce::jlimit (0, n - 1, (int) ((e.position.x - keys.getX()) / keys.getWidth() * (float) n));
        setValue (ctl.param, p.min + (float) k);
    }
    else
    {
        dragging = i;
        dragStartPos = p.toNormalised (value (ctl.param));
    }
    repaint();
}

void Faceplate::mouseDrag (const juce::MouseEvent& e)
{
    if (dragging < 0 || spec == nullptr)
        return;
    const auto& ctl = controls[(size_t) dragging];
    const auto& p = spec->params[(size_t) ctl.param];
    const float range = ctl.kind == Kind::Fader ? juce::jmax (40.0f, ctl.area.getHeight() * 0.8f) : 180.0f;
    const float fine = e.mods.isShiftDown() ? 0.2f : 1.0f;
    const float pos = juce::jlimit (0.0f, 1.0f, dragStartPos - (float) e.getDistanceFromDragStartY() / range * fine);
    values[ctl.param].store (p.fromNormalised (pos));
    repaint();
}

void Faceplate::mouseUp (const juce::MouseEvent&)
{
    dragging = -1;
}

void Faceplate::mouseDoubleClick (const juce::MouseEvent& e)
{
    const int i = controlAt (e.position);
    if (i < 0 || spec == nullptr)
        return;
    const auto& ctl = controls[(size_t) i];
    if (ctl.kind == Kind::Knob || ctl.kind == Kind::Fader)
        setValue (ctl.param, spec->params[(size_t) ctl.param].defaultValue);
    repaint();
}

void Faceplate::mouseWheelMove (const juce::MouseEvent& e, const juce::MouseWheelDetails& w)
{
    const int i = controlAt (e.position);
    if (i < 0 || spec == nullptr)
        return;
    const auto& ctl = controls[(size_t) i];
    const auto& p = spec->params[(size_t) ctl.param];
    touch (ctl.param);
    const float dir = (w.deltaY != 0.0f ? w.deltaY : -w.deltaX) > 0.0f ? 1.0f : -1.0f;
    if (p.isContinuous())
        values[ctl.param].store (p.fromNormalised (p.toNormalised (value (ctl.param)) + dir * (e.mods.isShiftDown() ? 0.005f : 0.025f)));
    else
        setValue (ctl.param, juce::jlimit (juce::jmin (p.min, p.max), juce::jmax (p.min, p.max), value (ctl.param) + dir));
    repaint();
}

} // namespace livedaw::ui
