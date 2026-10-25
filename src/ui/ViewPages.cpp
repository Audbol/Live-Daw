#include "ui/ViewPages.h"
#include "dsp/DspUtils.h"
#include "engine/Models.h"
#include "ui/HomeIcons.h"
#include "ui/HomeWidgets.h"
#include "ui/PaintHash.h"
#include "ui/RoutingGroups.h"

#include <array>

namespace livedaw::ui
{

namespace
{
    constexpr Screen viewScreens[] = { Screen::Overview, Screen::Faders, Screen::Sends, Screen::Config, Screen::MuteGroups };

    const juce::Colour blockFill = Colours::screenTile, blockOutline = Colours::screenLine, chipOff = Colours::chipOff, track = Colours::screenDark,
                       eqCurve = Colours::amber,
        panGreen = Colours::width, sendRed = Colours::tapRed;

    // Measured off the console's screens (firmware 3.1): unlit text, the dim
    // print of empty cells and dark labels, the mute groups' red.
    const juce::Colour keyText = Colours::caption, cellText { 0xff585858 }, darkLabel { 0xff0c0c0c }, groupRed { 0xfff05050 },
                       engagedColumn { 0xff883838 }, slotPlate { 0xff204870 };

    /** `v` px of the WING's 1280 x 800 screen in px of this screen. */
    int px (float v) { return juce::roundToInt (wingPx (v)); }

    /** A rectangle given in WING px from the page's top left (the page starts
        under the status bar, at the console's y = 80). */
    juce::Rectangle<int> wingRect (float x, float y, float w, float h)
    {
        return { px (x), px (y), px (x + w) - px (x), px (y + h) - px (y) };
    }

    /** The console's fonts by the height of their capitals in WING px. */
    juce::Font capFont (float capWingPx, WingWeight weight = WingWeight::Medium)
    {
        return wingFontPx (juce::jmax (5.0f, wingPx (capWingPx)), weight);
    }

    /** capFont, no taller than `maxHeightPx` (JUCE font height) so it fits small tiles. */
    juce::Font capFontFit (float capWingPx, float maxHeightPx, WingWeight weight = WingWeight::Medium)
    {
        auto f = capFont (capWingPx, weight);
        return f.getHeight() > maxHeightPx ? f.withHeight (juce::jmax (6.0f, maxHeightPx)) : f;
    }

    /** A WING key: a 2 px black frame round a #303030 face (or `fill`), its
        text centred. */
    void wingKey (juce::Graphics& g, juce::Rectangle<int> r, const juce::String& text, juce::Colour fill = Colours::screenTile,
                  juce::Colour ink = keyText, float cap = 14.0f)
    {
        auto f = r.toFloat();
        g.setColour (juce::Colours::black);
        g.fillRoundedRectangle (f, wingPx (3.0f));
        g.setColour (fill);
        g.fillRoundedRectangle (f.reduced (wingPx (2.0f)), wingPx (2.0f));
        g.setColour (ink);
        g.setFont (capFont (cap));
        g.drawFittedText (text, r.reduced (px (4.0f), 0), juce::Justification::centred, 1, 0.7f);
    }

    /** A screen font on the console's type scale (TypeScale), no taller than
        `maxHeightPx` so it fits small tiles. */
    juce::Font viewFont (float capFraction, float maxHeightPx, WingWeight weight = WingWeight::SemiBold)
    {
        auto f = wingFont (capFraction, weight);
        return f.getHeight() > maxHeightPx ? f.withHeight (juce::jmax (6.0f, maxHeightPx)) : f;
    }

    Icon viewIcon (Screen s)
    {
        switch (s)
        {
            case Screen::Overview:   return Icon::Overview;
            case Screen::Faders:     return Icon::Faders;
            case Screen::Sends:      return Icon::Sends;
            case Screen::Config:     return Icon::LayersGear;
            case Screen::MuteGroups: return Icon::MuteGroup;
            case Screen::Home:
            case Screen::Effects:
            case Screen::Meters:
            case Screen::Routing:
            case Screen::Setup:
            case Screen::Library:
            case Screen::Utility:
            case Screen::Recorder:
            case Screen::UsbRecorder:
                break;
        }
        return Icon::Overview;
    }

    /** The console's meter scale: CLIP, -6, -12, -24, -36, -48, -60 evenly spaced. */
    float meterPos (float db)
    {
        static constexpr float marks[] = { -60.0f, -48.0f, -36.0f, -24.0f, -12.0f, -6.0f, 0.0f };
        if (db <= marks[0])
            return 0.0f;
        for (size_t i = 1; i < std::size (marks); ++i)
            if (db <= marks[i])
                return ((float) (i - 1) + (db - marks[i - 1]) / (marks[i] - marks[i - 1])) / 6.0f;
        return 1.0f;
    }

    /** What a fader of a layer is: a strip or a DCA, with its name, colour and
        label; a send fader (WING 3.0.5: a layer's send, or Sends on Fader) also
        has the send's level and ON switch. */
    struct Target
    {
        Strip* strip = nullptr;
        int dca = -1;
        std::atomic<float>* sendLevel = nullptr;
        std::atomic<bool>* sendOn = nullptr;
        juce::String sendTag; // "> BUS 3", "< CH 5"

        bool isValid() const noexcept { return strip != nullptr || dca >= 0; }
        bool isSend() const noexcept { return sendLevel != nullptr; }
    };

    /** The send from `from` to `to` (an input or bus to a bus, main or
        matrix), created when missing; false when it can't exist. */
    bool sendBetween (Strip& from, Strip& to, Target& t)
    {
        if (&from == &to)
            return false;
        switch (to.getKind())
        {
            case StripKind::Bus:
                if (from.isInput() || (from.isBus() && from.getId() >= kBusToBusTargets && to.getId() < kBusToBusTargets))
                {
                    auto& send = from.sends[(size_t) to.getId()];
                    t.sendLevel = &send.levelDb;
                    t.sendOn = &send.enabled;
                    return true;
                }
                return false;
            case StripKind::Main:
                if (! from.isInput() && ! from.isBus())
                    return false;
                t.sendLevel = &from.params.mains[juce::jlimit (0, kNumMains - 1, to.getId())].levelDb;
                t.sendOn = &from.params.mains[juce::jlimit (0, kNumMains - 1, to.getId())].on;
                return true;
            case StripKind::Matrix:
                if (from.getKind() == StripKind::Matrix)
                    return false;
                t.sendLevel = &from.params.matrix[juce::jlimit (0, kNumMatrices - 1, to.getId())].levelDb;
                t.sendOn = &from.params.matrix[juce::jlimit (0, kNumMatrices - 1, to.getId())].on;
                return true;
            case StripKind::Channel:
            case StripKind::Aux:
                break;
        }
        return false;
    }

    juce::String sendTagOf (const Strip& s)
    {
        return juce::String (label (s.getKind())) + " " + juce::String (s.getNumber());
    }

    Target resolve (UiContext& ctx, const state::FaderRef& f)
    {
        if (f.isEmpty())
            return {}; // a blank slot
        if (f.dca)
        {
            Target t;
            t.dca = juce::jlimit (0, kNumDcas - 1, f.dcaIndex);
            return t;
        }
        if (f.fxParam || f.midiCc)
            return {}; // FX parameters and MIDI CCs have no strip view here
        Target t;
        t.strip = state::findStrip (ctx.engine, f.strip); // a send: its source
        if (f.send && t.strip != nullptr)
            if (auto* to = state::findStrip (ctx.engine, f.to); to != nullptr && sendBetween (*t.strip, *to, t))
                t.sendTag = "> " + sendTagOf (*to);
        return t;
    }

    /** Sends on Fader on the FADERS screen, as on the surface: with a bus,
        main or matrix selected (or the fixed SOF strip) each source's fader
        sets its send to it; with a source selected each destination's fader
        sets the source's send to it. */
    Target withSendsOnFader (UiContext& ctx, Target t)
    {
        if (! ctx.sendsOnFader || t.strip == nullptr || t.isSend())
            return t;
        auto* sel = ctx.sofDestination.has_value() ? state::findStrip (ctx.engine, *ctx.sofDestination)
                                                   : (ctx.getSelectedStrip ? ctx.getSelectedStrip() : nullptr);
        if (sel == nullptr || sel == t.strip)
            return t;
        if (sendBetween (*t.strip, *sel, t))
            t.sendTag = "> " + sendTagOf (*sel);
        else if (sendBetween (*sel, *t.strip, t))
            t.sendTag = "< " + sel->getLabel().removeCharacters (" ");
        return t;
    }

    juce::Colour colourOf (UiContext& ctx, const Target& t)
    {
        return t.strip != nullptr ? stripDisplayColour (*t.strip) : wingColour (ctx.engine.getDca (t.dca).colour.load());
    }

    /** The name shown in the name bar (empty for default names, as on the console). */
    juce::String barName (UiContext& ctx, const Target& t)
    {
        if (t.strip != nullptr)
            return stripName (*t.strip) != t.strip->getLabel() ? stripName (*t.strip).toUpperCase() : juce::String();
        const auto name = ctx.engine.getDcaName (t.dca);
        return name.removeCharacters (" .").equalsIgnoreCase ("DCA" + juce::String (t.dca + 1)) ? juce::String() : name.toUpperCase();
    }

    juce::String numberOf (const Target& t)
    {
        return t.strip != nullptr ? shortStripLabel (*t.strip) : "D" + juce::String (t.dca + 1);
    }

    std::atomic<float>& faderOf (UiContext& ctx, const Target& t)
    {
        if (t.sendLevel != nullptr)
            return *t.sendLevel;
        return t.strip != nullptr ? t.strip->params.faderDb : ctx.engine.getDca (t.dca).faderDb;
    }

    std::atomic<bool>& muteOf (UiContext& ctx, const Target& t)
    {
        return t.strip != nullptr ? t.strip->params.mute : ctx.engine.getDca (t.dca).mute;
    }

    void paintNameBar (juce::Graphics& g, juce::Rectangle<float> r, juce::Colour colour, const juce::String& name)
    {
        g.setColour (colour);
        g.fillRect (r);
        g.setColour (juce::Colours::black);
        g.setFont (capFontFit (13.0f, r.getHeight() * 0.86f, WingWeight::SemiBold));
        g.drawFittedText (name, r.toNearestInt().reduced (2, 0), juce::Justification::centred, 1, 0.6f);
    }

    void paintNumberTile (juce::Graphics& g, juce::Rectangle<float> r, const Target& t)
    {
        g.setColour (blockFill);
        g.fillRect (r);
        auto inner = r.reduced (wingPx (6.0f), 2.0f);
        g.setColour (Dark::textDim);
        g.setFont (capFontFit (20.0f, inner.getHeight() * 0.9f, WingWeight::SemiBold));
        const auto number = numberOf (t);
        g.drawFittedText (number, inner.toNearestInt(), juce::Justification::centredLeft, 1, 0.7f);
        if (t.strip != nullptr && inner.getWidth() > inner.getHeight() * 2.2f)
            drawStripIcon (g, *t.strip, inner.removeFromRight (inner.getHeight()).reduced (1.0f), Dark::textDim);
    }

    void block (juce::Graphics& g, juce::Rectangle<float> r)
    {
        g.setColour (blockFill);
        g.fillRect (r);
        g.setColour (blockOutline);
        g.drawRect (r, 1.0f);
    }

    void miniChip (juce::Graphics& g, juce::Rectangle<float> r, const juce::String& text, bool on, juce::Colour onColour)
    {
        g.setColour (on ? onColour : chipOff);
        g.fillRect (r);
        g.setColour (on ? juce::Colours::black : Dark::textDim);
        g.setFont (viewFont (TypeScale::caption, juce::jmin (r.getHeight() * 0.8f, r.getWidth() * 0.36f)));
        g.drawFittedText (text, r.toNearestInt(), juce::Justification::centred, 1, 0.6f);
    }

    void verticalMeter (juce::Graphics& g, juce::Rectangle<float> r, float level, juce::Colour c, juce::Colour off = Colours::screenDark)
    {
        const int segments = juce::jlimit (4, screenCap (20), (int) (r.getHeight() / 3.0f));
        const float h = r.getHeight() / (float) segments;
        for (int i = 0; i < segments; ++i)
        {
            const bool lit = level > ((float) i + 0.5f) / (float) segments;
            g.setColour (lit ? c : off);
            g.fillRect (r.getX(), r.getBottom() - (float) (i + 1) * h + 0.5f, r.getWidth(), juce::jmax (1.0f, h - 1.0f));
        }
    }

    void levelBar (juce::Graphics& g, juce::Rectangle<float> r, float db, bool muted)
    {
        g.setColour (muted ? Dark::muted : track);
        g.fillRect (r);
        if (! muted)
        {
            g.setColour (Dark::faderFill);
            g.fillRect (r.withWidth (r.getWidth() * fader::dbToPos (db)));
        }
    }

    /** The pan display: a green arc centred on the pan position, as wide as the stereo width. */
    void panArc (juce::Graphics& g, juce::Rectangle<float> r, float pan, float width)
    {
        const float s = juce::jmin (r.getWidth(), r.getHeight());
        auto k = r.withSizeKeepingCentre (s, s).reduced (s * 0.08f);
        const float rad = k.getWidth() * 0.4f, thick = juce::jmax (2.0f, k.getWidth() * 0.18f);
        const float limit = juce::MathConstants<float>::pi * 0.75f;
        const float centre = juce::jlimit (-1.0f, 1.0f, pan / 100.0f) * limit;
        const float half = juce::jlimit (0.12f, limit, std::abs (width) / 100.0f * juce::MathConstants<float>::halfPi);
        juce::Path bg, arc;
        bg.addCentredArc (k.getCentreX(), k.getCentreY(), rad, rad, 0.0f, -limit, limit, true);
        arc.addCentredArc (k.getCentreX(), k.getCentreY(), rad, rad, 0.0f, juce::jmax (-limit, centre - half), juce::jmin (limit, centre + half), true);
        g.setColour (juce::Colours::black);
        g.strokePath (bg, juce::PathStrokeType (thick));
        g.setColour (panGreen);
        g.strokePath (arc, juce::PathStrokeType (thick));
        g.setColour (Dark::text);
        const auto c = k.getCentre();
        g.drawLine (c.x, c.y, c.x + std::sin (centre) * rad, c.y - std::cos (centre) * rad, 1.5f);
    }

    /** Mini send levels: one cell per destination, filled to the level when the send is on. */
    /** The OVERVIEW sends block [fig. 54]: a 2 x 2 grid of grey quadrants,
        the matrix sends top right and the bus sends 1-8 / 9-16 below, each
        send a thin bar filled with its level in its mode's colour (TAP red,
        POST green, GROUP blue) and a blue marker left of the first send. */
    void sendQuadrants (juce::Graphics& g, juce::Rectangle<float> r, UiContext& ctx, Strip& s)
    {
        const float gap = 2.0f;
        const float qw = (r.getWidth() - gap) / 2.0f, qh = (r.getHeight() - gap) / 2.0f;
        auto quad = [&] (int col, int row) { return juce::Rectangle<float> (r.getX() + (float) col * (qw + gap), r.getY() + (float) row * (qh + gap), qw, qh); };
        auto bars = [&g] (juce::Rectangle<float> q, int n, const std::function<std::pair<float, juce::Colour> (int)>& level)
        {
            g.setColour (Colours::screenLine);
            g.fillRect (q);
            auto inner = q.reduced (2.0f, 1.0f);
            const float w = inner.getWidth() / 8.0f;
            for (int i = 0; i < n && i < 8; ++i)
            {
                const auto [l, c] = level (i);
                if (l <= 0.0f)
                    continue;
                auto bar = juce::Rectangle<float> (inner.getX() + (float) i * w, inner.getY(), juce::jmax (1.0f, w - 1.0f), inner.getHeight());
                g.setColour (c);
                g.fillRect (bar.withTop (bar.getBottom() - bar.getHeight() * l));
            }
        };
        auto colourOf = [] (int mode) { return mode == 0 ? Colours::tapRed : (mode == 1 ? Colours::green : Colours::valueBlue); };
        g.setColour (Colours::screenWell);
        g.fillRect (quad (0, 0));
        if (s.getKind() != StripKind::Matrix)
            bars (quad (1, 0), kNumMatrices, [&s, &colourOf] (int i)
            {
                const auto& m = s.params.matrix[(size_t) i];
                return std::make_pair (m.on.load() ? fader::dbToPos (m.levelDb.load()) : 0.0f, colourOf (m.pre.load() ? 0 : 1));
            });
        else
        {
            g.setColour (Colours::screenWell);
            g.fillRect (quad (1, 0));
        }
        const bool busSends = s.getKind() != StripKind::Main && s.getKind() != StripKind::Matrix;
        const int n = busSends ? juce::jmin (16, (int) ctx.engine.getBuses().size()) : 0;
        for (int half = 0; half < 2; ++half)
        {
            auto q = quad (half, 1);
            if (! busSends)
            {
                g.setColour (Colours::screenWell);
                g.fillRect (q);
                continue;
            }
            bars (q, juce::jmax (0, n - half * 8), [&s, half, &colourOf] (int i)
            {
                auto* send = s.sends.find (half * 8 + i);
                if (send == nullptr || ! send->enabled.load() || send->mode.load() == 2)
                    return std::make_pair (0.0f, Colours::green);
                return std::make_pair (fader::dbToPos (send->levelDb.load()), colourOf (send->mode.load()));
            });
        }
        if (busSends)
        {
            auto q = quad (0, 1);
            g.setColour (Colours::valueBlue);
            g.fillRect (juce::Rectangle<float> (2.0f, juce::jmin (5.0f, q.getHeight() * 0.4f)).withPosition (q.getX() + 1.0f, q.getY() + 1.0f));
        }
    }

    /** A screen fader [WING]: a black slot, the fill under the cap, the value
        in a white box at the cap with blue marks out to the well's edges. */
    void paintFader (juce::Graphics& g, juce::Rectangle<float> r, float db, juce::Colour fill, bool active)
    {
        const float boxH = juce::jmin (wingPx (25.0f), r.getHeight() * 0.1f);
        auto lane = r.withTrimmedTop (wingPx (12.0f)).withTrimmedBottom (wingPx (14.0f));
        const float slotW = juce::jmax (4.0f, juce::jmin (wingPx (12.0f), r.getWidth() * 0.2f));
        auto slot = juce::Rectangle<float> (r.getCentreX() - slotW / 2.0f, r.getY() + wingPx (10.0f), slotW, lane.getBottom() - r.getY() - wingPx (10.0f));
        g.setColour (juce::Colours::black);
        g.fillRoundedRectangle (slot, slotW * 0.3f);
        const float pos = fader::dbToPos (db);
        const float y = lane.getBottom() - lane.getHeight() * pos;
        if (active && pos > 0.0f)
        {
            g.setColour (fill);
            g.fillRect (slot.withTop (y));
        }
        auto box = juce::Rectangle<float> (juce::jmin (r.getWidth() - wingPx (10.0f), wingPx (60.0f)), boxH).withCentre ({ r.getCentreX(), y });
        g.setColour (fill);
        const float mark = juce::jmin (wingPx (5.0f), boxH * 0.3f);
        g.fillRect (juce::Rectangle<float> (r.getX(), y - mark / 2.0f, box.getX() - r.getX(), mark));
        g.fillRect (juce::Rectangle<float> (box.getRight(), y - mark / 2.0f, r.getRight() - box.getRight(), mark));
        g.setColour (active ? Colours::text : Colours::textDim);
        g.fillRoundedRectangle (box, wingPx (3.0f));
        g.setColour (juce::Colours::black);
        g.setFont (capFontFit (12.0f, boxH * 0.8f));
        // Narrow strips (12 strips in a small window) leave out the unit
        // rather than cutting the value.
        auto text = home::wingDb (db) + " dB";
        if (juce::GlyphArrangement::getStringWidth (g.getCurrentFont(), text) * 0.85f > box.getWidth() - 2.0f)
            text = home::wingDb (db);
        g.drawFittedText (text, box.toNearestInt(), juce::Justification::centred, 1, 0.6f);
    }

    /** Vertical fader drag with the TOUCH FADER RES of SETUP > GENERAL: NORM,
        FINE (a quarter of the speed), or AUTO (FINE once the pointer leaves the strip sideways). */
    float dragScale (UiContext& ctx, juce::Rectangle<int> column, juce::Point<int> pos)
    {
        switch (prefs::faderResolution (ctx))
        {
            case 0: return 1.0f;
            case 1: return 0.25f;
            default: return pos.x < column.getX() || pos.x > column.getRight() ? 0.25f : 1.0f;
        }
    }

    /** CLOSE and its ticked box at the right of `r` [WING title rows]. */
    void drawCloseKey (juce::Graphics& g, juce::Rectangle<int> r)
    {
        auto close = r.withTrimmedRight (px (16.0f));
        auto box = close.removeFromRight (px (32.0f)).withSizeKeepingCentre (px (32.0f), px (32.0f));
        g.setColour (Colours::caption);
        g.drawRect (box, juce::jmax (1, px (2.0f)));
        juce::Path tick;
        const auto b = box.toFloat();
        tick.startNewSubPath (b.getX() + b.getWidth() * 0.22f, b.getY() + b.getHeight() * 0.5f);
        tick.lineTo (b.getX() + b.getWidth() * 0.42f, b.getY() + b.getHeight() * 0.72f);
        tick.lineTo (b.getRight() - b.getWidth() * 0.05f, b.getY() + b.getHeight() * 0.08f);
        g.strokePath (tick, juce::PathStrokeType (wingPx (2.0f)));
        close.removeFromRight (px (12.0f));
        g.setFont (capFont (14.0f));
        g.drawText ("CLOSE", close, juce::Justification::centredRight, false);
    }

    /** The SOLO BUS keys' lit olive [WING]. */
    const juce::Colour soloBusOlive { 0xff809800 };
} // namespace

/** A DCA's panel [WING, firmware 3.1], over the page: LIGHT (the scribble
    strip light) and the 18 colours in a row, the chosen one framed gold; the
    icon gallery (category keys, a grid of five columns); SOLO BUS (PHONES /
    SPEAKERS keys: where the members' solo goes in SOLO MODE LIVE), the title
    and CLOSE; the name field and the keyboard, whose CLOSE closes the panel
    too. Every change applies at once. A touch beside the panel closes it. */
class DcaPanel final : public juce::Component, private juce::Timer
{
public:
    explicit DcaPanel (UiContext& c) : ctx (c)
    {
        keyboard.setFieldVisible (false);
        keyboard.setMaxLength (16);
        keyboard.setCapsLock (true);
        keyboard.setActionText ("CLOSE");
        keyboard.onChange = [this] (const juce::String& t) { setName (t); };
        keyboard.onAction = [this] (const juce::String&) { close(); };
        keyboard.onCancel = [this] { close(); };
        addAndMakeVisible (keyboard);
    }

    void show (int index)
    {
        dca = juce::jlimit (0, kNumDcas - 1, index);
        category = home::wingIconCategory (ctx.engine.getDca (dca).icon.load());
        // The default name ("DCA.1") reads as an empty field, as on the console.
        keyboard.setText (isDefaultName() ? juce::String() : ctx.engine.getDcaName (dca));
        setVisible (true);
        toFront (false);
        startTimerHz (3);
        juce::MessageManager::callAsync ([safe = juce::Component::SafePointer<juce::Component> (&keyboard)]
        {
            if (safe != nullptr && safe->isShowing())
                safe->grabKeyboardFocus();
        });
        repaint();
    }

    void resized() override
    {
        // WING px from the page's top (the console's y = 80).
        lightRow = wingRect (10.0f, 122.0f, 1256.0f, 70.0f);
        lightKey = wingRect (18.0f, 129.0f, 56.0f, 56.0f);
        for (int i = 0; i < 18; ++i)
            swatches[i] = wingRect (200.0f + 58.8f * (float) i, 129.0f, 55.0f, 55.0f);
        iconPanel = wingRect (10.0f, 196.0f, 500.0f, 442.0f);
        for (int i = 0; i < 7; ++i)
            categoryKeys[i] = wingRect (18.0f, 205.0f + 61.0f * (float) i, 56.0f, 56.0f);
        for (int i = 0; i < 25; ++i)
            cells[i] = wingRect (78.0f + 85.8f * (float) (i % 5), 204.0f + 86.0f * (float) (i / 5), 82.0f, 82.0f);
        titleRow = wingRect (514.0f, 196.0f, 752.0f, 64.0f);
        phonesKey = wingRect (583.0f, 201.0f, 54.0f, 54.0f);
        speakerKey = wingRect (639.0f, 201.0f, 54.0f, 54.0f);
        closeKey = wingRect (1120.0f, 196.0f, 146.0f, 64.0f);
        fieldPanel = wingRect (514.0f, 264.0f, 752.0f, 100.0f);
        field = wingRect (690.0f, 278.0f, 400.0f, 72.0f);
        clearKey = field.withLeft (field.getRight() - field.getHeight());
        keyboardPanel = wingRect (514.0f, 368.0f, 752.0f, 328.0f);
        keyboard.setBounds (keyboardPanel.reduced (px (8.0f)));
    }

    void paint (juce::Graphics& g) override
    {
        // The page dimmed round the panel.
        juce::RectangleList<int> panel;
        for (auto r : { lightRow, iconPanel, titleRow, fieldPanel, keyboardPanel })
            panel.add (r);
        g.saveState();
        g.excludeClipRegion (lightRow);
        g.excludeClipRegion (iconPanel);
        g.excludeClipRegion (titleRow.getUnion (keyboardPanel));
        g.setColour (Colours::scrim);
        g.fillAll();
        g.restoreState();
        g.setColour (juce::Colours::black);
        g.fillRect (titleRow.getUnion (keyboardPanel));
        g.setColour (Colours::screenPanel);
        for (auto r : panel)
            g.fillRect (r);

        auto& d = ctx.engine.getDca (dca);

        // LIGHT: the sun on a key lit gold while the light is on, then the colours.
        const bool lit = d.led.load();
        g.setColour (lit ? Colours::gold : Colours::screenTile);
        g.fillRect (lightKey);
        g.setColour (Colours::screenLine);
        g.drawRect (lightKey, 1);
        drawIcon (g, Icon::Sun, lightKey.toFloat().reduced (wingPx (12.0f)), lit ? juce::Colours::black : Colours::caption);
        g.setColour (Colours::caption);
        g.setFont (capFont (14.0f));
        g.drawText ("LIGHT", wingRect (88.0f, 129.0f, 100.0f, 56.0f), juce::Justification::centredLeft, false);
        g.setColour (juce::Colours::black);
        g.fillRect (wingRect (196.0f, 125.0f, 1066.0f, 63.0f));
        for (int i = 0; i < 18; ++i)
        {
            g.setColour (wingColour (swatchColour (i)));
            g.fillRect (swatches[i]);
            if (d.colour.load() == swatchColour (i))
            {
                g.setColour (Colours::gold);
                g.drawRect (swatches[i].expanded (px (2.0f)), juce::jmax (2, px (2.0f)));
            }
        }

        // The icon gallery: the category keys (the shown one light grey), the
        // icons on #303030 cells, the chosen one framed gold.
        const auto& cats = home::wingIconCategories();
        for (int i = 0; i < 7 && i < (int) cats.size(); ++i)
        {
            const bool on = i == category;
            g.setColour (on ? Colours::caption : Colours::screenTile);
            g.fillRect (categoryKeys[i]);
            home::drawIconCategory (g, i, categoryKeys[i].toFloat().reduced (wingPx (9.0f)), on ? Colours::screenPanel : Colours::caption);
        }
        g.setColour (juce::Colours::black);
        g.fillRect (wingRect (76.0f, 202.0f, 428.0f, 432.0f));
        const auto& icons = cats[(size_t) juce::jlimit (0, (int) cats.size() - 1, category)].icons;
        for (int i = 0; i < 25; ++i)
        {
            g.setColour (Colours::screenPanel);
            g.fillRect (cells[i]);
            if (i >= (int) icons.size())
                continue;
            const int icon = icons[(size_t) i].number;
            if (icon != 0)
                drawWingIcon (g, icon, cells[i].toFloat().reduced (wingPx (14.0f)), Colours::caption);
            if (icon == d.icon.load())
            {
                g.setColour (Colours::gold);
                g.drawRect (cells[i], juce::jmax (2, px (2.0f)));
            }
        }

        // SOLO BUS: PHONES (A) and SPEAKERS (B) lit olive; the title; CLOSE.
        g.setColour (Colours::caption);
        g.setFont (capFont (14.0f));
        g.drawFittedText ("SOLO\nBUS", wingRect (522.0f, 200.0f, 56.0f, 56.0f), juce::Justification::centred, 2, 0.8f);
        const int mon = juce::jlimit (0, 2, (int) d.mon.load());
        for (int k = 0; k < 2; ++k)
        {
            const auto key = k == 0 ? phonesKey : speakerKey;
            const bool on = k == 0 ? mon != 1 : mon != 0;
            g.setColour (juce::Colours::black);
            g.fillRect (key);
            g.setColour (on ? soloBusOlive : Colours::screenTile);
            g.fillRect (key.reduced (px (2.0f)));
            drawIcon (g, k == 0 ? Icon::Headphones : Icon::Speaker, key.toFloat().reduced (wingPx (10.0f)), on ? juce::Colours::black : Colours::caption);
        }
        g.setColour (Colours::text);
        g.setFont (capFont (18.0f, WingWeight::SemiBold));
        g.drawText ("DCA " + juce::String (dca + 1), titleRow.withTrimmedLeft (px (200.0f)).withTrimmedRight (px (200.0f)), juce::Justification::centred, false);
        drawCloseKey (g, closeKey);

        // The name: a black field with a gold caret and the circled clear key.
        g.setColour (juce::Colours::black);
        g.fillRect (field);
        g.setColour (Colours::screenLine);
        g.drawRect (field, juce::jmax (1, px (1.5f)));
        auto text = field.toFloat().withTrimmedRight ((float) clearKey.getWidth()).withTrimmedLeft (wingPx (22.0f));
        const auto name = keyboard.getText();
        const auto font = capFont (20.0f);
        g.setFont (font);
        g.setColour (Colours::textDim);
        g.drawText (name, text.toNearestInt(), juce::Justification::centredLeft, true);
        if (caretOn)
        {
            const float x = text.getX() + juce::GlyphArrangement::getStringWidth (font, name.substring (0, keyboard.getCaretPosition())) + wingPx (2.0f);
            g.setColour (Colours::gold);
            g.fillRect (x, text.getY() + text.getHeight() * 0.25f, juce::jmax (2.0f, wingPx (2.0f)), text.getHeight() * 0.5f);
        }
        drawIcon (g, Icon::ClearCircle, clearKey.toFloat().reduced (wingPx (18.0f)), Colours::caption);
    }

    void mouseUp (const juce::MouseEvent& e) override
    {
        const auto pos = e.getPosition();
        auto& d = ctx.engine.getDca (dca);
        if (lightKey.contains (pos))
            d.led = ! d.led.load();
        else if (closeKey.contains (pos))
            close();
        else if (clearKey.contains (pos))
        {
            keyboard.setText ({});
            setName ({});
        }
        else if (phonesKey.contains (pos) || speakerKey.contains (pos))
        {
            // A key toggles its path; one of the two stays on (A, B or A+B).
            const int mon = juce::jlimit (0, 2, (int) d.mon.load());
            bool a = mon != 1, b = mon != 0;
            (phonesKey.contains (pos) ? a : b) ^= true;
            if (a || b)
                d.mon = a && b ? 2 : a ? 0 : 1;
        }
        else if (! (lightRow.contains (pos) || iconPanel.contains (pos) || titleRow.getUnion (keyboardPanel).contains (pos)))
        {
            close();
            return;
        }
        for (int i = 0; i < 18; ++i)
            if (swatches[i].contains (pos))
                d.colour = swatchColour (i);
        for (int i = 0; i < 7; ++i)
            if (categoryKeys[i].contains (pos))
                category = i;
        const auto& icons = home::wingIconCategories()[(size_t) category].icons;
        for (int i = 0; i < 25 && i < (int) icons.size(); ++i)
            if (cells[i].contains (pos))
                d.icon = icons[(size_t) i].number;
        repaint();
    }

    std::function<void()> onClose;

private:
    /** The colour of swatch `i` of the row: ICON/COLOR's grid (three columns
        of six) read column by column, as the console lines them up. */
    static int swatchColour (int i) { return wingColourAtGrid (i % 6 * 3 + i / 6); }

    bool isDefaultName() const
    {
        const auto name = ctx.engine.getDcaName (dca);
        return name.isEmpty() || name.removeCharacters (" .").equalsIgnoreCase ("DCA" + juce::String (dca + 1));
    }

    void setName (const juce::String& name)
    {
        ctx.engine.setDcaName (dca, name.trim());
        repaint (field);
    }

    void close()
    {
        stopTimer();
        setVisible (false);
        if (onClose)
            onClose();
    }

    void timerCallback() override
    {
        caretOn = ! caretOn;
        repaint (field);
    }

    UiContext& ctx;
    int dca = 0, category = 0;
    bool caretOn = true;
    popups::WingKeyboard keyboard;
    juce::Rectangle<int> lightRow, lightKey, swatches[18], iconPanel, categoryKeys[7], cells[25], titleRow, phonesKey, speakerKey, closeKey, fieldPanel,
        field, clearKey, keyboardPanel;
};

void ViewPage::showDcaPanel (int dca)
{
    if (dcaPanel == nullptr)
    {
        dcaPanel = std::make_unique<DcaPanel> (ctx);
        dcaPanel->onClose = [this] { notifyStatusKeysChanged(); };
        addChildComponent (*dcaPanel);
    }
    dcaPanel->setBounds (getLocalBounds());
    dcaPanel->show (dca);
}

const char* viewScreenName (Screen s)
{
    switch (s)
    {
        case Screen::Overview:   return "OVERVIEW";
        case Screen::Faders:     return "FADERS";
        case Screen::Sends:      return "SENDS";
        case Screen::Config:     return "CONFIG";
        case Screen::MuteGroups: return "MUTEGRP";
        case Screen::Home:
        case Screen::Effects:
        case Screen::Meters:
        case Screen::Routing:
        case Screen::Setup:
        case Screen::Library:
        case Screen::Utility:
        case Screen::Recorder:
        case Screen::UsbRecorder:
            break;
    }
    return "";
}

//==============================================================================
namespace
{
    /** A layer key's caption in the VIEW screens' column (and their title):
        one word, as the console prints it ("MAIN", "AUX", "BUSES", "USER1"). */
    juce::String columnLabel (const Layer& l)
    {
        static const std::pair<const char*, const char*> keys[] = { { "dca-1", "DCA" }, { "out-1", "MAIN" },    { "aux", "AUX" },
                                                                    { "bus-1", "BUSES" }, { "user-1", "USER1" }, { "user-2", "USER2" } };
        if (! l.user)
            for (auto [id, label] : keys)
                if (l.id == id)
                    return label;
        return (l.user ? l.name.toUpperCase() : LayerSet::keyLabel (l)).replaceCharacter ('\n', ' ');
    }

    // The column's rows (WING px from the page's top): keys 48 tall every 52,
    // 5 more between groups of bank keys; LEFT / CENTER / RIGHT at the bottom.
    constexpr float kColumnKeyH = 48.0f, kColumnStep = 52.0f, kColumnGroupGap = 5.0f, kColumnTop = 4.0f, kSectionTop = 492.0f,
                    kColumnPanelH = 648.0f, kPagerTop = 656.0f, kPagerH = 56.0f;
} // namespace

/** The layer column of the VIEW screens (firmware 3.1): on a #202020 panel
    the section's layer keys (and any further user layers) on the right with
    LEFT / CENTER / RIGHT at the bottom; the bank keys on the left (1-8 ...
    DCA 9-16; not on CONFIG); the page arrows on a panel under them. */
class ViewPage::LayerColumn final : public juce::Component
{
public:
    explicit LayerColumn (ViewPage& p) : page (p) {}

    void rebuild()
    {
        keys.clear();
        bankKeys.clear();
        const bool withBanks = page.hasBankKeys();
        const float colW = withBanks ? 192.0f : 96.0f;
        // WING px inside the column (its left edge at x = 0).
        auto at = [] (float x, float y, float w, float h) { return wingRect (x, y, w, h); };
        const float rightX = withBanks ? 100.0f : 4.0f;
        prevKey = at (0.0f, kPagerTop, colW / 2.0f, kPagerH);
        nextKey = at (colW / 2.0f, kPagerTop, colW / 2.0f, kPagerH);

        auto& layers = page.ctx.show.layers;
        std::vector<Layer> right = layers.getDefaultLayers (page.ctx.engine, page.section());
        const auto& user = layers.getUserLayers();
        for (size_t i = 2; i < user.size(); ++i)
            right.push_back (user[i]); // user layers without a USER key
        for (int i = 0; i < 3; ++i)
            sectionKeys[i] = at (rightX, kSectionTop + kColumnStep * (float) i, 88.0f, kColumnKeyH);
        float y = kColumnTop;
        for (auto& l : right)
        {
            if (y + kColumnKeyH > kSectionTop - 4.0f)
                break;
            keys.push_back ({ at (rightX, y, 88.0f, kColumnKeyH), l.id, columnLabel (l) });
            y += kColumnStep;
        }

        if (withBanks)
        {
            y = kColumnTop;
            const auto all = page.banks();
            for (size_t i = 0; i < all.size(); ++i)
            {
                if (i > 0 && all[i].group != all[i - 1].group)
                    y += kColumnGroupGap;
                if (y + kColumnKeyH > kColumnPanelH)
                    break;
                bankKeys.push_back ({ at (4.0f, y, 88.0f, kColumnKeyH), juce::String ((int) i), all[i].key });
                y += kColumnStep;
            }
        }
        shownLayer = page.currentLayerId();
        repaint();
    }

    /** The layer shown changed elsewhere (the section's layer keys). */
    void followLayer()
    {
        if (page.currentLayerId() != shownLayer)
            rebuild();
    }

    void resized() override { rebuild(); }

    void paint (juce::Graphics& g) override
    {
        const float colW = page.hasBankKeys() ? 192.0f : 96.0f;
        g.setColour (Colours::screenPanel);
        g.fillRect (wingRect (0.0f, 0.0f, colW, kColumnPanelH));
        g.fillRect (wingRect (0.0f, kPagerTop, colW, kPagerH));
        const auto current = page.currentLayerId();
        const int shown = page.shownBank();
        for (auto& k : keys)
        {
            const bool on = shown < 0 && k.id == current;
            wingKey (g, k.area, k.name, on ? Dark::layer : Colours::screenTile, on ? Dark::choiceInk : keyText);
        }
        for (auto& k : bankKeys)
        {
            const bool on = k.id.getIntValue() == shown;
            wingKey (g, k.area, k.name, on ? Dark::layer : Colours::screenTile, on ? Dark::choiceInk : keyText);
        }
        // The section's key olive, darker while a bank is shown instead of its layer [WING].
        const char* names[] = { "LEFT", "CENTER", "RIGHT" };
        const auto sectionFill = shown >= 0 ? Dark::section.withMultipliedBrightness (0.55f) : Dark::section;
        for (int i = 0; i < 3; ++i)
        {
            const bool on = (int) page.section() == i;
            wingKey (g, sectionKeys[i], names[i], on ? sectionFill : Colours::screenTile, on ? juce::Colours::black : keyText);
        }
        const bool paged = page.lastPage() > 0;
        for (auto [r, icon, enabled] : { std::tuple { prevKey, Icon::ChevronLeft, paged && page.page > 0 },
                                         std::tuple { nextKey, Icon::ChevronRight, paged && page.page < page.lastPage() } })
        {
            const float s = wingPx (36.0f);
            drawIcon (g, icon, r.toFloat().withSizeKeepingCentre (s, s), enabled ? Dark::text : Colours::screenLine);
        }
    }

    void mouseUp (const juce::MouseEvent& e) override
    {
        const auto pos = e.getPosition();
        for (auto& k : keys)
            if (k.area.contains (pos))
            {
                page.selectLayer (k.id);
                return;
            }
        for (auto& k : bankKeys)
            if (k.area.contains (pos))
            {
                page.selectBank (k.id.getIntValue());
                return;
            }
        for (int i = 0; i < 3; ++i)
            if (sectionKeys[i].contains (pos))
            {
                page.selectSection (static_cast<LayerSet::Section> (i));
                return;
            }
        if (prevKey.contains (pos))
            page.stepPage (-1);
        else if (nextKey.contains (pos))
            page.stepPage (1);
    }

private:
    struct Key
    {
        juce::Rectangle<int> area;
        juce::String id, name;
    };

    ViewPage& page;
    std::vector<Key> keys, bankKeys;
    juce::Rectangle<int> sectionKeys[3], prevKey, nextKey;
    juce::String shownLayer;
};

//==============================================================================
ViewPage::ViewPage (UiContext& c, Screen s) : ctx (c), screen (s)
{
    column = std::make_unique<LayerColumn> (*this);
    addChildComponent (*column);
    anchor.setInterceptsMouseClicks (false, false);
    addAndMakeVisible (anchor);
    startTimerHz (15);
}

ViewPage::~ViewPage() = default;

juce::Component& ViewPage::anchorAt (juce::Rectangle<int> area)
{
    anchor.setBounds (area);
    return anchor;
}

std::vector<StatusKey> ViewPage::getStatusKeys()
{
    // The VIEW screens switch between each other from the status bar.
    std::vector<StatusKey> keys;
    for (auto s : viewScreens)
        keys.push_back ({ viewScreenName (s), viewIcon (s), s == screen, [this, s] { if (ctx.showScreen) ctx.showScreen (s); } });
    return keys;
}

StatusTitle ViewPage::getStatusTitle()
{
    // A dark tile with the section letter, the layers icon and the layer [I10].
    StatusTitle t;
    t.kind = StatusTitle::Kind::Layer;
    const char letters[] = { 'L', 'C', 'R' };
    t.prefix = juce::String::charToString ((juce::juce_wchar) letters[juce::jlimit (0, 2, (int) section())]);
    const auto all = banks();
    t.text = juce::isPositiveAndBelow (shownBank(), (int) all.size()) ? all[(size_t) shownBank()].title : columnLabel (currentLayer());
    t.icon = Icon::Layers;
    return t;
}

void ViewPage::refresh()
{
    page = juce::jlimit (0, lastPage(), page);
    resized();
    repaint();
    notifyStatusKeysChanged();
}

void ViewPage::timerCallback()
{
    if (! isShowing())
        return;
    if (hasLayerColumn())
        column->followLayer();
    // The title tile follows the layer (it may change from the surface).
    if (const auto title = getStatusTitle(); title.prefix + title.text != shownTitle)
    {
        shownTitle = title.prefix + title.text;
        notifyStatusKeysChanged();
    }
    // Under a DCA's panel the page stays as it was (the panel repaints what it changes).
    if (dcaPanel != nullptr && dcaPanel->isVisible())
        return;
    // Only when something it shows changed: a full redraw of the page fifteen
    // times a second costs far more than walking its paint.
    if (const auto hash = paintHash (*this); hash != shownHash)
    {
        shownHash = hash;
        repaint();
    }
}

juce::String ViewPage::currentLayerId() const
{
    const auto s = section();
    if (prefs::screenFollowsLayer (ctx) || browsed[s].isEmpty())
        return ctx.show.layers.getSelectedLayerId (s);
    return browsed[s];
}

Layer ViewPage::currentLayer() const
{
    return ctx.show.layers.findLayer (ctx.engine, section(), currentLayerId());
}

std::vector<ViewPage::Bank> ViewPage::banks() const
{
    // Each kind in banks of eight ("1-8" ... "33-40", "AUX", "BUS 1-8", ...),
    // the mains and matrices one group, then the DCAs.
    std::vector<Bank> out;
    auto addStrips = [this, &out] (StripKind kind, const juce::String& prefix, const juce::String& titlePrefix, int group)
    {
        auto& strips = ctx.engine.getStrips (kind);
        const int n = (int) strips.size();
        for (int first = 0; first < n; first += 8)
        {
            Bank b;
            const auto range = juce::String (first + 1) + "-" + juce::String (juce::jmin (n, first + 8));
            b.key = n <= 8 && prefix.isNotEmpty() ? prefix.trim() : prefix + range;
            b.title = n <= 8 ? titlePrefix.trim() : titlePrefix + range;
            b.group = group;
            for (int i = first; i < juce::jmin (n, first + 8); ++i)
                b.faders.push_back (state::FaderRef::of (state::StripRef::of (*strips[(size_t) i])));
            out.push_back (std::move (b));
        }
    };
    addStrips (StripKind::Channel, {}, "CH ", 0);
    addStrips (StripKind::Aux, "AUX ", "AUX ", 1);
    addStrips (StripKind::Bus, "BUS ", "BUS ", 2);
    addStrips (StripKind::Main, "MAIN ", "MAIN ", 3);
    addStrips (StripKind::Matrix, "MTX ", "MATRIX ", 3);
    for (int first = 0; first < kNumDcas; first += 8)
    {
        Bank b;
        b.key = b.title = "DCA " + juce::String (first + 1) + "-" + juce::String (juce::jmin (kNumDcas, first + 8));
        b.group = 4;
        for (int i = first; i < juce::jmin (kNumDcas, first + 8); ++i)
            b.faders.push_back (state::FaderRef::ofDca (i));
        out.push_back (std::move (b));
    }
    return out;
}

int ViewPage::pageSize() const
{
    return juce::jmax (1, ctx.show.layers.getEffectiveWidth (section()));
}

int ViewPage::numPages() const
{
    if (shownBank() >= 0)
        return 1;
    const int n = (int) currentLayer().faders.size();
    return juce::jmax (1, (n + pageSize() - 1) / pageSize());
}

std::vector<state::FaderRef> ViewPage::pageFaders() const
{
    if (const auto all = banks(); juce::isPositiveAndBelow (shownBank(), (int) all.size()))
        return all[(size_t) shownBank()].faders;
    const auto layer = currentLayer();
    const int size = pageSize(), first = juce::jlimit (0, lastPage(), page) * size;
    std::vector<state::FaderRef> out;
    for (int i = first; i < juce::jmin ((int) layer.faders.size(), first + size); ++i)
        out.push_back (layer.faders[(size_t) i]);
    return out;
}

std::vector<juce::Rectangle<int>> ViewPage::stripColumns (juce::Rectangle<int> area, int count) const
{
    // [WING] 96 px strips 106 apart, the last ending 21 px left of the layer
    // column (x = 1059); wider sections (12 strips) shrink them to fit.
    count = juce::jmax (1, count);
    const float right = hasLayerColumn() ? 1059.0f : 1272.0f;
    const float pitch = juce::jmin (106.0f, (right - 9.0f + 10.0f) / (float) count), w = pitch - 10.0f;
    std::vector<juce::Rectangle<int>> out;
    for (int i = 0; i < count; ++i)
    {
        const float x = right - (float) (count - i) * pitch + 10.0f;
        out.push_back (juce::Rectangle<int> (px (x), area.getY(), px (x + w) - px (x), area.getHeight()));
    }
    return out;
}

void ViewPage::selectLayer (const juce::String& id)
{
    const auto s = section();
    browsed[s] = id;
    bank[s] = -1;
    if (prefs::screenFollowsLayer (ctx) && id != ctx.show.layers.getSelectedLayerId (s))
    {
        ctx.show.layers.setSelectedLayerId (s, id);
        notifyLayersChangedAsync (ctx);
    }
    page = 0;
    layerChanged();
    column->rebuild();
    repaint();
}

void ViewPage::selectSection (LayerSet::Section s)
{
    ctx.viewSection = s;
    page = 0;
    resized();
    layerChanged();
    repaint();
}

void ViewPage::selectBank (int index)
{
    bank[section()] = index;
    page = 0;
    layerChanged();
    column->rebuild();
    notifyStatusKeysChanged();
    repaint();
}

void ViewPage::stepPage (int delta)
{
    if (const int shown = shownBank(); shown >= 0)
    {
        // A bank: the arrows step through the banks.
        selectBank (juce::jlimit (0, (int) banks().size() - 1, shown + delta));
        return;
    }
    page = juce::jlimit (0, lastPage(), page + delta);
    layerChanged();
    column->rebuild();
    repaint();
}

void ViewPage::paint (juce::Graphics& g)
{
    g.fillAll (juce::Colours::black);
    paintContent (g);
}

void ViewPage::resized()
{
    auto b = getLocalBounds();
    column->setVisible (hasLayerColumn());
    if (hasLayerColumn())
    {
        // [WING] x = 1080 (CONFIG: 1176) to 1272, to the page arrows' panel's bottom.
        const float x = hasBankKeys() ? 1080.0f : 1176.0f;
        column->setBounds (wingRect (x, 0.0f, 1272.0f - x, kPagerTop + kPagerH));
        b.setRight (px (x) - px (8.0f));
        column->rebuild();
    }
    content = b;
    layoutContent();
}

//==============================================================================
namespace
{
    /** A strip a cell or tile of a member grid stands for. */
    struct MemberCell
    {
        juce::Rectangle<int> area;
        Strip* strip = nullptr;
    };

    /** The kinds a DCA or mute group takes and how many of each the WING shows. */
    constexpr std::pair<StripKind, int> memberKinds[] = { { StripKind::Channel, 40 }, { StripKind::Aux, 8 }, { StripKind::Bus, 16 },
                                                          { StripKind::Main, 4 },     { StripKind::Matrix, 8 } };

    const char* memberCaption (StripKind k)
    {
        switch (k)
        {
            case StripKind::Channel: return "CHANNEL";
            case StripKind::Aux:     return "AUX";
            case StripKind::Bus:     return "BUS";
            case StripKind::Main:    return "MAIN";
            case StripKind::Matrix:  return "MATRIX";
        }
        return "";
    }

    /** The OVERVIEW column of a DCA or mute group [WING]: on its panel
        (`body`, WING y = 152 to 752) a caption over each kind and its members
        as 20 px cells, four to a row (a member's cell in the member's own
        colour), then the monitor paths' two cells. Returns the strips' cells. */
    std::vector<MemberCell> paintMemberColumn (juce::Graphics& g, UiContext& ctx, juce::Rectangle<int> body, juce::Colour panel,
                                               const std::function<bool (const Strip&)>& isMember, const juce::String& phonesText,
                                               const juce::String& speakersText)
    {
        std::vector<MemberCell> cells;
        g.setColour (panel);
        g.fillRect (body);
        const int x0 = body.getX(), y0 = body.getY();
        auto at = [x0, y0] (float x, float y, float w, float h)
        { return juce::Rectangle<int> (x0 + px (x), y0 + px (y), px (x + w) - px (x), px (y + h) - px (y)); };
        auto cell = [&g] (juce::Rectangle<int> r, const juce::String& text, bool on, juce::Colour onColour)
        {
            g.setColour (on ? onColour : Colours::screenDark);
            g.fillRoundedRectangle (r.toFloat(), wingPx (2.0f));
            g.setColour (on ? juce::Colours::black : cellText);
            g.drawFittedText (text, r, juce::Justification::centred, 1, 0.6f);
        };
        float y = 0.0f;
        for (auto [kind, count] : memberKinds)
        {
            g.setColour (keyText);
            g.setFont (capFont (10.0f));
            g.drawText (memberCaption (kind), at (0.0f, y, 96.0f, 22.0f), juce::Justification::centred);
            y += 22.0f;
            auto& strips = ctx.engine.getStrips (kind);
            const int n = juce::jmin (count, (int) strips.size());
            for (int i = 0; i < n; ++i)
            {
                auto& s = *strips[(size_t) i];
                const auto r = at (5.0f + 22.0f * (float) (i % 4), y + 22.0f * (float) (i / 4), 20.0f, 20.0f);
                cell (r, juce::String (i + 1), isMember (s), stripDisplayColour (s));
                cells.push_back ({ r, &s });
            }
            y += 22.0f * (float) ((count + 3) / 4);
        }
        // The monitor paths (a DCA or mute group can't take them in Sidecar).
        y += 4.0f;
        cell (at (5.0f, y, 42.0f, 20.0f), phonesText, false, {});
        cell (at (49.0f, y, 42.0f, 20.0f), speakersText, false, {});
        return cells;
    }

    /** A strip's tile on the assign screens and CONFIG [WING]: a 22 px bar in
        its colour (its name, black), the #303030 body with the number (an
        input's in a grey badge) and its icon; framed gold when chosen. */
    void paintStripTile (juce::Graphics& g, juce::Rectangle<int> area, const Strip& s, bool framed)
    {
        auto r = area.toFloat();
        const float corner = wingPx (3.0f);
        auto bar = r.removeFromTop (r.getHeight() * 22.0f / 64.0f);
        g.setColour (stripDisplayColour (s));
        g.fillRoundedRectangle (bar.withBottom (bar.getBottom() + corner), corner);
        g.setColour (Colours::screenTile);
        g.fillRect (r);
        if (stripName (s) != s.getLabel())
        {
            g.setColour (juce::Colours::black);
            g.setFont (fontFittingWidth (capFont (12.0f, WingWeight::SemiBold), stripName (s).toUpperCase(), bar.getWidth() - wingPx (6.0f)));
            g.drawFittedText (stripName (s).toUpperCase(), bar.toNearestInt().reduced (px (3.0f), 0), juce::Justification::centred, 1, 0.6f);
        }
        const auto label = shortStripLabel (s);
        const auto font = capFont (juce::jmin (20.0f, r.getHeight() / wingPx (1.0f) * 0.48f), WingWeight::SemiBold);
        auto inner = r.reduced (wingPx (6.0f), r.getHeight() * 0.17f);
        if (s.isInput())
        {
            const float w = juce::GlyphArrangement::getStringWidth (font, label) + wingPx (6.0f);
            auto badge = inner.removeFromLeft (w);
            g.setColour (Colours::textDim);
            g.fillRoundedRectangle (badge, wingPx (2.0f));
            g.setColour (juce::Colours::black);
            g.setFont (font);
            g.drawText (label, badge, juce::Justification::centred, false);
            const float icon = juce::jmin (inner.getHeight(), wingPx (26.0f));
            if (inner.getWidth() > icon * 1.5f)
                drawStripIcon (g, s, inner.removeFromRight (icon).withSizeKeepingCentre (icon, icon), Colours::textDim);
        }
        else
        {
            g.setColour (Colours::textDim);
            g.setFont (font);
            g.drawText (label, r.reduced (wingPx (3.0f), 0.0f), juce::Justification::centredLeft, false);
        }
        if (framed)
        {
            g.setColour (Colours::gold);
            g.drawRoundedRectangle (area.toFloat().expanded (wingPx (2.0f)), corner, wingPx (2.0f));
        }
    }
} // namespace

/** Assigning a DCA's or a mute group's members [WING]: the eight groups as
    tabs on top (the one edited bright; a tab picks its group), the channels,
    aux and buses as tiles in the frame, the mains and matrices on the right
    under CLOSE and UNASSIGN. A tile toggles its strip, members are framed
    gold. A mute group has a SOURCE tab too: the sources it mutes (ROUTING >
    SOURCES > TAGS), one source group at a time as ROUTING's tiles (eight to a
    row), the source groups as tiles of four on the right (the one shown framed
    gold). */
class MemberAssignView final : public juce::Component
{
public:
    enum class Kind
    {
        Dca,
        MuteGroup
    };

    MemberAssignView (UiContext& c, Kind k) : ctx (c), kind (k) {}

    std::function<void()> onClose;

    void show (int g)
    {
        group = juce::jlimit (0, groupCount() - 1, g);
        sources = false;
        setVisible (true);
        toFront (false);
        resized();
        repaint();
    }

    int getGroup() const noexcept { return group; }

    void resized() override
    {
        tiles.clear();
        sourceTiles.clear();
        const float tabX = kind == Kind::Dca ? 221.0f : 9.0f;
        for (int i = 0; i < 8; ++i)
            tabs[i] = wingRect (tabX + 106.0f * (float) i, 0.0f, 96.0f, 64.0f);
        frame = wingRect (24.0f, 72.0f, 838.0f, 630.0f);
        groupTiles.clear();
        if (sources)
        {
            // The group's sources in rows of eight (LOCAL IN: 24, more for a
            // bigger interface), the groups in rows of four on the right.
            const int n = juce::jmin (64, routing::tileCount (sourceGroup, ctx.engine.getHardwareInputNames().size()));
            for (int i = 0; i < n; ++i)
                sourceTiles.push_back ({ wingRect (83.0f + 88.0f * (float) (i % 8), 79.0f + 76.0f * (float) (i / 8), 80.0f, 68.0f), i });
            frame = wingRect (76.0f, 72.0f, 710.0f, 76.0f * (float) ((n + 7) / 8) + 6.0f);
            const auto menu = routing::sourceMenu (false);
            for (size_t r = 0; r < menu.rows.size(); ++r)
                for (size_t c = 0; c < menu.rows[r].size(); ++c)
                    if (menu.rows[r][c] >= 0)
                        groupTiles.push_back ({ wingRect (c < 3 ? 887.0f + 91.0f * (float) c : 1165.0f, 243.0f + 91.0f * (float) r + (r >= 3 ? 5.0f : 0.0f), 86.0f, 86.0f),
                                                menu.rows[r][c] });
        }
        else
        {
            auto add = [this] (StripKind k, int count, float y)
            {
                auto& strips = ctx.engine.getStrips (k);
                for (int i = 0; i < juce::jmin (count, (int) strips.size()); ++i)
                    tiles.push_back ({ wingRect (31.0f + 104.0f * (float) (i % 8), y + 72.0f * (float) (i / 8), 96.0f, 64.0f), strips[(size_t) i].get() });
            };
            add (StripKind::Channel, 40, 79.0f);
            add (StripKind::Aux, 8, 463.0f);
            add (StripKind::Bus, 16, 559.0f);
            auto& mains = ctx.engine.getStrips (StripKind::Main);
            for (int i = 0; i < juce::jmin (4, (int) mains.size()); ++i)
                tiles.push_back ({ wingRect (891.0f, 247.0f + 72.0f * (float) i, 96.0f, 64.0f), mains[(size_t) i].get() });
            auto& matrices = ctx.engine.getStrips (StripKind::Matrix);
            for (int i = 0; i < juce::jmin (8, (int) matrices.size()); ++i)
                tiles.push_back ({ wingRect (i < 4 ? 1049.0f : 1153.0f, 247.0f + 72.0f * (float) (i % 4), 96.0f, 64.0f), matrices[(size_t) i].get() });
        }
        channelTab = wingRect (884.0f, 72.0f, 117.0f, 55.0f);
        sourceTab = wingRect (1004.0f, 72.0f, 117.0f, 55.0f);
        closeKey = kind == Kind::Dca ? wingRect (884.0f, 72.0f, 372.0f, 64.0f) : wingRect (1124.0f, 72.0f, 132.0f, 55.0f);
        unassignKey = wingRect (977.0f, 160.0f, 186.0f, 56.0f);
    }

    void paint (juce::Graphics& g) override
    {
        g.fillAll (juce::Colours::black);
        paintTabs (g);

        // The frame round the tiles, the panels on the right (SOURCE: the
        // source groups' panel reaches down to PHONES LEVEL).
        g.setColour (Colours::screenPanel);
        g.drawRect (frame, px (4.0f));
        if (sources)
            g.fillRect (wingRect (884.0f, 240.0f, 372.0f, 372.0f));
        else
            g.drawRect (wingRect (884.0f, 240.0f, 372.0f, 294.0f), px (4.0f));
        g.drawRect (wingRect (884.0f, 620.0f, 372.0f, 60.0f), px (4.0f));
        g.fillRect (wingRect (884.0f, 72.0f, 372.0f, 64.0f));
        g.fillRect (wingRect (884.0f, 144.0f, 372.0f, 88.0f));
        if (kind == Kind::MuteGroup)
        {
            // CHANNEL | SOURCE: the one shown with a gold line and gold print.
            for (auto [r, text, on] : { std::tuple { channelTab, "CHANNEL", ! sources }, std::tuple { sourceTab, "SOURCE", sources } })
            {
                g.setColour (on ? Colours::screenTile : Colours::screenPanel);
                g.fillRect (r);
                if (on)
                {
                    g.setColour (Colours::gold);
                    g.fillRect (r.withHeight (px (4.0f)));
                }
                g.setColour (on ? Colours::gold : Colours::caption);
                g.setFont (capFont (on ? 17.0f : 14.0f));
                g.drawFittedText (text, r.reduced (px (4.0f), 0), juce::Justification::centred, 1, 0.6f);
            }
            g.setColour (juce::Colours::black);
            g.fillRect (wingRect (1001.0f, 72.0f, 3.0f, 55.0f));
            g.fillRect (wingRect (1121.0f, 72.0f, 3.0f, 55.0f));
            g.fillRect (wingRect (884.0f, 127.0f, 372.0f, 3.0f));
        }
        drawCloseKey (g, closeKey);

        // UNASSIGN: an outlined key with a crossed ring.
        g.setColour (Colours::screenTile);
        g.fillRect (unassignKey);
        g.setColour (Colours::caption);
        g.drawRect (unassignKey, 1);
        auto u = unassignKey.toFloat().reduced (wingPx (16.0f), 0.0f);
        const float ring = wingPx (32.0f);
        drawIcon (g, Icon::ClearCircle, u.removeFromLeft (ring).withSizeKeepingCentre (ring, ring), Colours::caption);
        g.setFont (capFont (14.0f));
        g.drawText ("UNASSIGN", u, juce::Justification::centred, false);

        // PHONES LEVEL / SPK LEVEL: the console's DCAs and mute groups can
        // take the monitor levels; Sidecar's can't, so they are dim.
        for (int i = 0; i < 2; ++i)
        {
            auto t = wingRect (891.0f + 74.0f * (float) i, 627.0f, 66.0f, 46.0f).toFloat();
            auto top = t.removeFromTop (t.getHeight() / 2.0f);
            g.setColour (Dark::olive.withMultipliedBrightness (0.45f));
            g.fillRect (top);
            g.setColour (Colours::screenPanel);
            g.fillRect (t);
            g.setFont (capFont (11.0f, WingWeight::SemiBold));
            g.setColour (juce::Colours::black);
            g.drawText (i == 0 ? "PHONES" : "SPK", top, juce::Justification::centred, false);
            g.setColour (Colours::screenLine);
            g.drawText ("LEVEL", t, juce::Justification::centred, false);
        }

        for (auto& t : tiles)
            paintStripTile (g, t.area, *t.strip, isMember (*t.strip));
        // The meters stay down: ROUTING is the source peaks' only reader.
        for (auto& [area, index] : sourceTiles)
            routing::paintSourceTile (g, area, ctx.engine, sourceGroup, index, 0.0f, isMember (index));
        for (auto& [area, id] : groupTiles)
            routing::paintGroupTile (g, area, (routing::Group) id, id == (int) sourceGroup);
    }

    void mouseUp (const juce::MouseEvent& e) override
    {
        const auto pos = e.getPosition();
        for (int i = 0; i < 8; ++i)
            if (tabs[i].contains (pos))
            {
                if (const int g = firstTab() + i; g < groupCount())
                    group = g;
                repaint();
                return;
            }
        if (closeKey.contains (pos))
        {
            setVisible (false);
            if (onClose)
                onClose();
            return;
        }
        if (unassignKey.contains (pos))
        {
            unassignAll();
            return;
        }
        if (kind == Kind::MuteGroup && (channelTab.contains (pos) || sourceTab.contains (pos)))
        {
            sources = sourceTab.contains (pos);
            resized();
            repaint();
            return;
        }
        for (auto& [area, id] : groupTiles)
            if (area.contains (pos))
            {
                sourceGroup = (routing::Group) id;
                resized();
                repaint();
                return;
            }
        for (auto& t : tiles)
            if (t.area.contains (pos))
            {
                toggle (*t.strip);
                repaint();
                return;
            }
        for (auto& [area, index] : sourceTiles)
            if (area.contains (pos))
            {
                if (auto* src = sourceOf (index))
                    src->muteGroups = src->muteGroups.load() ^ bit();
                repaint();
                return;
            }
    }

private:
    int groupCount() const noexcept { return kind == Kind::Dca ? kNumDcas : kNumMuteGroups; }
    int firstTab() const noexcept { return kind == Kind::Dca ? group / 8 * 8 : 0; }
    uint32_t bit() const noexcept { return 1u << (uint32_t) group; }

    /** The source of tile `index` of the group shown (nullptr: n/a). */
    ld::SourceParams* sourceOf (int index) const
    {
        const int port = routing::sourceAt (sourceGroup, index);
        return port >= 0 ? ctx.engine.getSource (MixerEngine::sourceIndex (port)) : nullptr;
    }

    bool isMember (int sourceTile) const
    {
        const auto* src = sourceOf (sourceTile);
        return src != nullptr && (src->muteGroups.load() & bit()) != 0;
    }

    bool isMember (const Strip& s) const
    {
        return ((kind == Kind::Dca ? s.params.dcaMask.load() : s.params.muteGroupMask.load()) & bit()) != 0;
    }

    void toggle (Strip& s)
    {
        auto& mask = kind == Kind::Dca ? s.params.dcaMask : s.params.muteGroupMask;
        mask = mask.load() ^ bit();
    }

    void unassignAll()
    {
        for (int k = 0; k < kNumStripKinds; ++k)
            for (auto& s : ctx.engine.getStrips (static_cast<StripKind> (k)))
            {
                auto& mask = kind == Kind::Dca ? s->params.dcaMask : s->params.muteGroupMask;
                mask = mask.load() & ~bit();
            }
        // A mute group's sources too (ROUTING > SOURCES > TAGS).
        if (kind == Kind::MuteGroup)
            for (int i = 0; i < ctx.engine.getNumSources(); ++i)
                if (auto* src = ctx.engine.getSource (i))
                    src->muteGroups = src->muteGroups.load() & ~bit();
        repaint();
    }

    void paintTabs (juce::Graphics& g)
    {
        for (int i = 0; i < 8; ++i)
        {
            const int index = firstTab() + i;
            if (index >= groupCount())
                continue;
            const bool on = index == group;
            auto r = tabs[i];
            const auto colour = kind == Kind::Dca ? wingColour (ctx.engine.getDca (index).colour.load()) : groupRed;
            const auto bar = r.removeFromTop (px (22.0f));
            g.setColour (on ? colour : colour.withMultipliedBrightness (0.5f));
            g.fillRect (bar);
            g.setColour (on ? Colours::screenTile : juce::Colour (0xff181818));
            g.fillRect (r);
            const auto name = kind == Kind::Dca ? ctx.engine.getDcaName (index) : ctx.engine.getMuteGroupName (index);
            const bool defaultName = kind == Kind::Dca && name.removeCharacters (" .").equalsIgnoreCase ("DCA" + juce::String (index + 1));
            if (! defaultName)
            {
                g.setColour (on ? juce::Colours::black : juce::Colours::black.withAlpha (0.6f));
                g.setFont (capFont (13.0f, WingWeight::SemiBold));
                g.drawFittedText (name.toUpperCase(), bar, juce::Justification::centred, 1, 0.6f);
            }
            g.setColour (on ? Colours::textDim : Colours::screenTile);
            g.setFont (capFont (20.0f, WingWeight::SemiBold));
            if (kind == Kind::Dca)
                g.drawText ("D" + juce::String (index + 1), r.withTrimmedLeft (px (5.0f)), juce::Justification::centredLeft, false);
            else
                g.drawText (juce::String (index + 1), r, juce::Justification::centred, false);
        }
    }

    UiContext& ctx;
    const Kind kind;
    int group = 0;
    bool sources = false;
    routing::Group sourceGroup = routing::Group::LocalIn;
    juce::Rectangle<int> tabs[8], frame, channelTab, sourceTab, closeKey, unassignKey;
    std::vector<MemberCell> tiles;
    std::vector<std::pair<juce::Rectangle<int>, int>> sourceTiles, groupTiles;
};

//==============================================================================
namespace
{
    /** The rows of an OVERVIEW strip, top to bottom: five processing blocks
        with the level bar among them where the fader sits. */
    struct OverviewRows
    {
        juce::Rectangle<float> name, number, input, blocks[5], bar, pan, sends;
    };

    /** The processing blocks of a strip in its WING order (HomeTab::Home = an
        empty position), and how many come before the fader. */
    struct Blocks
    {
        std::array<HomeTab, 5> tabs { HomeTab::Home, HomeTab::Home, HomeTab::Home, HomeTab::Home, HomeTab::Home };
        int beforeFader = 4;
    };

    Blocks blocksOf (const Strip& s)
    {
        Blocks b;
        if (s.isOutput())
        {
            // Buses, mains, matrices: DYN, INS 1, fader, EQ, INS 2.
            b.tabs = { HomeTab::Home, HomeTab::Dynamics, HomeTab::PreInsert, HomeTab::Eq, HomeTab::PostInsert };
            b.beforeFader = 3;
            return b;
        }
        if (! s.hasSelectableTap())
        {
            // Aux: DYN, EQ, INS 1 before the fader; no INS 2.
            b.tabs = { HomeTab::Home, HomeTab::Dynamics, HomeTab::Eq, HomeTab::PreInsert, HomeTab::Home };
            return b;
        }
        // Input channels: G, E, D, I in their process order, then INS 2.
        const auto order = models::list (models::ProcessOrders)[juce::jlimit (0, 23, (int) s.params.processOrder.load())];
        size_t i = 0;
        for (auto letter : order)
        {
            if (i >= 4)
                break;
            if (letter == 'G')
                b.tabs[i++] = HomeTab::Gate;
            else if (letter == 'E')
                b.tabs[i++] = HomeTab::Eq;
            else if (letter == 'D')
                b.tabs[i++] = HomeTab::Dynamics;
            else if (letter == 'I')
                b.tabs[i++] = HomeTab::PreInsert;
        }
        b.tabs[4] = HomeTab::PostInsert;
        return b;
    }

    OverviewRows overviewRows (juce::Rectangle<int> column, int beforeFader)
    {
        OverviewRows r;
        auto c = column.toFloat();
        r.name = c.removeFromTop (wingPx (22.0f));
        r.number = c.removeFromTop (wingPx (42.0f));
        c.removeFromTop (wingPx (8.0f));
        const float h = column.getHeight();
        const float barH = juce::jmax (5.0f, h * 0.028f);
        const float gap = 3.0f;
        const float blockH = (c.getHeight() - barH - gap * 9.0f) / 8.0f;
        r.input = c.removeFromTop (blockH);
        for (int i = 0; i < 5; ++i)
        {
            if (i == beforeFader)
            {
                c.removeFromTop (gap);
                r.bar = c.removeFromTop (barH);
            }
            c.removeFromTop (gap);
            r.blocks[i] = c.removeFromTop (blockH);
        }
        if (beforeFader >= 5)
        {
            c.removeFromTop (gap);
            r.bar = c.removeFromTop (barH);
        }
        c.removeFromTop (gap);
        r.pan = c.removeFromTop (blockH);
        c.removeFromTop (gap);
        r.sends = c.removeFromTop (blockH);
        return r;
    }

    void paintInsertBlock (juce::Graphics& g, juce::Rectangle<float> r, UiContext& ctx, Strip& s, bool post)
    {
        block (g, r);
        auto& ins = post ? s.params.postInsert : s.params.preInsert;
        const int slot = ins.fx.load();
        const int mode = post ? ins.mode.load() : 0;
        const bool on = ins.on.load() && (slot >= 0 || mode > 0);
        auto inner = r.reduced (3.0f);
        const juce::String text = mode > 0 ? juce::String ("AUTO") : slot >= 0 ? "FX " + juce::String (slot + 1) : juce::String ("INS");
        miniChip (g, inner.removeFromTop (juce::jmin (inner.getHeight() * 0.5f, screenCap (16.0f))).withWidth (inner.getWidth() * 0.62f), text, on, Dark::on);
        juce::String device;
        auto& fx = ctx.engine.getFx();
        if (mode > 0)
            device = mode == 1 ? "X" : "Y";
        else if (slot >= 0)
        {
            if (auto* plugin = fx.getPlugin (slot))
                device = plugin->getName().toUpperCase();
            else if (fx.getModel (slot) > ld::kFxExt)
                device = models::model (models::Fx, fx.getModel (slot)).name.toUpperCase();
        }
        g.setColour (Dark::text);
        g.setFont (viewFont (TypeScale::caption, inner.getHeight() * 0.7f));
        g.drawFittedText (device, inner.toNearestInt(), juce::Justification::bottomRight, 1, 0.6f);
    }

    void paintDynBlock (juce::Graphics& g, juce::Rectangle<float> r, Strip& s, bool gate)
    {
        block (g, r);
        auto& slot = gate ? s.params.gate : s.params.dynamics;
        auto inner = r.reduced (3.0f);
        const float gr = (gate ? s.meters.gateReductionDb.load() : s.meters.dynReductionDb.load()) / 24.0f;
        verticalMeter (g, inner.removeFromRight (juce::jmax (3.0f, inner.getWidth() * 0.1f)), juce::jlimit (0.0f, 1.0f, gr), Colours::meterYellow);
        inner.removeFromRight (2.0f);
        auto top = inner.removeFromTop (juce::jmin (inner.getHeight() * 0.5f, screenCap (16.0f)));
        if (gate)
        {
            // Green while the gate is open.
            auto led = top.removeFromRight (top.getHeight()).reduced (top.getHeight() * 0.2f);
            g.setColour (slot.on.load() && ! s.meters.gateActive.load() ? Colours::meterGreen : Colours::screenDark);
            g.fillRect (led);
            top.removeFromRight (2.0f);
        }
        miniChip (g, top.withWidth (juce::jmin (top.getWidth(), inner.getWidth() * 0.75f)), models::model (models::Dynamics, slot.model.load()).token,
                  slot.on.load(), Dark::on);
    }

    void paintEqBlock (juce::Graphics& g, juce::Rectangle<float> r, Strip& s)
    {
        g.setColour (juce::Colours::black);
        g.fillRect (r);
        g.setColour (blockOutline);
        g.drawRect (r, 1.0f);
        std::array<double, 24> freqs {}, db {};
        for (size_t i = 0; i < freqs.size(); ++i)
            freqs[i] = 20.0 * std::pow (1000.0, (double) i / 23.0);
        ld::ld_strip_response (s.getHandle(), 1, freqs.data(), db.data(), (int32_t) freqs.size(), 48000.0);
        juce::Path curve;
        auto inner = r.reduced (2.0f);
        for (size_t i = 0; i < db.size(); ++i)
        {
            const float x = inner.getX() + inner.getWidth() * (float) i / (float) (db.size() - 1);
            const float y = inner.getCentreY() - juce::jlimit (-1.0f, 1.0f, (float) db[i] / 15.0f) * inner.getHeight() * 0.45f;
            if (i == 0)
                curve.startNewSubPath (x, y);
            else
                curve.lineTo (x, y);
        }
        g.setColour (s.params.eq.on.load() ? eqCurve : Dark::textDim.withAlpha (0.5f));
        g.strokePath (curve, juce::PathStrokeType (1.5f));
    }

    void paintInputBlock (juce::Graphics& g, juce::Rectangle<float> r, UiContext& ctx, Strip& s)
    {
        block (g, r);
        auto inner = r.reduced (3.0f);
        const float level = meterPos (dsp::gainToDb (ctx.meters.get (&s, MeterPoint::Trim, 0)));
        verticalMeter (g, inner.removeFromRight (juce::jmax (3.0f, inner.getWidth() * 0.1f)), level, Colours::meterGreen);
        inner.removeFromRight (2.0f);
        auto& p = s.params;
        const bool ch = s.getKind() == StripKind::Channel;
        const std::pair<const char*, bool> chips[] = { { "INV", p.input.invert.load() }, { "LC", ch && p.filter.lowCut.load() },
                                                       { "HC", ch && p.filter.highCut.load() }, { "FLT", ch && p.filter.tool.load() },
                                                       { "DLY", p.input.delayOn.load() }, { p.stereo.load() || s.isOutput() ? "ST" : "M", false } };
        const float w = inner.getWidth() / 3.0f, h = juce::jmin (inner.getHeight() / 2.0f, screenCap (16.0f));
        for (int i = 0; i < 6; ++i)
            miniChip (g, juce::Rectangle<float> (inner.getX() + w * (float) (i % 3), inner.getCentreY() - h + h * (float) (i / 3), w, h).reduced (1.0f),
                      chips[i].first, chips[i].second, Dark::choice);
    }

    void paintPanBlock (juce::Graphics& g, juce::Rectangle<float> r, UiContext& ctx, Strip& s)
    {
        block (g, r);
        auto inner = r.reduced (3.0f);
        auto meter = inner.removeFromRight (juce::jmax (6.0f, inner.getWidth() * 0.16f));
        for (int ch = 0; ch < 2; ++ch)
            verticalMeter (g, meter.withWidth (meter.getWidth() / 2.0f - 1.0f).translated ((float) ch * meter.getWidth() / 2.0f, 0.0f),
                           meterPos (dsp::gainToDb (ctx.meters.get (&s, prefs::meterPoint (ctx, s.getKind()), ch))), Colours::meterGreen);
        inner.removeFromRight (2.0f);
        if (s.getKind() != StripKind::Main && s.getKind() != StripKind::Matrix)
        {
            auto mains = inner.removeFromRight (juce::jmax (5.0f, inner.getWidth() * 0.14f));
            const float h = mains.getHeight() / 4.0f;
            for (int i = 0; i < kNumMains; ++i)
            {
                g.setColour (s.params.mains[i].on.load() ? Dark::faderFill : Colours::screenDark);
                g.fillRect (mains.withHeight (h).translated (0.0f, h * (float) i).reduced (0.5f));
            }
            inner.removeFromRight (2.0f);
        }
        panArc (g, inner, s.params.pan.load(), s.params.width.load());
    }
} // namespace

OverviewViewPage::OverviewViewPage (UiContext& c) : ViewPage (c, Screen::Overview)
{
    assign = std::make_unique<MemberAssignView> (ctx, MemberAssignView::Kind::Dca);
    assign->onClose = [this] { notifyStatusKeysChanged(); };
    addChildComponent (*assign);
}

OverviewViewPage::~OverviewViewPage() = default;

void OverviewViewPage::resized()
{
    ViewPage::resized();
    assign->setBounds (getLocalBounds());
}

bool OverviewViewPage::titleClicked()
{
    // Assigning a DCA: its title tile opens the DCA's panel [WING].
    if (! assign->isVisible())
        return false;
    showDcaPanel (assign->getGroup());
    return true;
}

StatusTitle OverviewViewPage::getStatusTitle()
{
    // Assigning a DCA: the title tile in its colour with "D1" (or its name).
    if (! assign->isVisible())
        return ViewPage::getStatusTitle();
    const int dca = assign->getGroup();
    StatusTitle t;
    t.kind = StatusTitle::Kind::Text;
    t.fill = wingColour (ctx.engine.getDca (dca).colour.load());
    t.ink = juce::Colours::black;
    const auto name = ctx.engine.getDcaName (dca);
    const bool defaultName = name.isEmpty() || name.removeCharacters (" .").equalsIgnoreCase ("DCA" + juce::String (dca + 1));
    t.text = defaultName ? "D" + juce::String (dca + 1) : name.toUpperCase();
    return t;
}

namespace
{
    /** Where the VIEW pages' strips end: the WING's y = 752. */
    juce::Rectangle<int> stripArea (juce::Rectangle<int> content)
    {
        return content.withHeight (px (672.0f));
    }
} // namespace

void OverviewViewPage::paintContent (juce::Graphics& g)
{
    const auto faders = pageFaders();
    const auto cols = stripColumns (stripArea (content), juce::jmax ((int) faders.size(), pageSize()));
    auto* selected = ctx.getSelectedStrip ? ctx.getSelectedStrip() : nullptr;
    for (size_t i = 0; i < faders.size() && i < cols.size(); ++i)
    {
        const auto t = resolve (ctx, faders[i]);
        if (! t.isValid())
            continue;
        const auto blocks = t.strip != nullptr ? blocksOf (*t.strip) : Blocks {};
        const auto rows = overviewRows (cols[i], blocks.beforeFader);
        paintNameBar (g, rows.name, colourOf (ctx, t), barName (ctx, t));
        paintNumberTile (g, rows.number, t);
        if (t.strip == nullptr)
        {
            // [WING] a DCA: its members, "DCA" at the bottom.
            const uint32_t bit = 1u << (uint32_t) t.dca;
            const auto body = cols[i].withTop (cols[i].getY() + px (72.0f));
            paintMemberColumn (g, ctx, body, Colours::screenPanel, [bit] (const Strip& s) { return (s.params.dcaMask.load() & bit) != 0; },
                               "PH", "SPK");
            // Yellow while the DCA is soloed [WING].
            g.setColour (ctx.engine.getDca (t.dca).solo.load() ? Colours::solo : darkLabel);
            g.setFont (capFont (20.0f, WingWeight::SemiBold));
            g.drawText ("DCA", body.withTop (body.getY() + px (561.0f)).withHeight (px (32.0f)), juce::Justification::centred, false);
            continue;
        }
        levelBar (g, rows.bar, faderOf (ctx, t).load(), muteOf (ctx, t).load());

        auto& s = *t.strip;
        paintInputBlock (g, rows.input, ctx, s);
        for (size_t k = 0; k < blocks.tabs.size(); ++k)
        {
            switch (blocks.tabs[k])
            {
                case HomeTab::Gate:       paintDynBlock (g, rows.blocks[k], s, true); break;
                case HomeTab::Dynamics:   paintDynBlock (g, rows.blocks[k], s, false); break;
                case HomeTab::Eq:         paintEqBlock (g, rows.blocks[k], s); break;
                case HomeTab::PreInsert:  paintInsertBlock (g, rows.blocks[k], ctx, s, false); break;
                case HomeTab::PostInsert: paintInsertBlock (g, rows.blocks[k], ctx, s, true); break;
                case HomeTab::Home:
                case HomeTab::Icon:
                case HomeTab::Tags:
                case HomeTab::Input:
                case HomeTab::Filter:
                case HomeTab::Main:
                case HomeTab::Sends:
                case HomeTab::Name:
                    break;
            }
        }
        paintPanBlock (g, rows.pan, ctx, s);
        block (g, rows.sends);
        sendQuadrants (g, rows.sends.reduced (3.0f), ctx, s);
        if (&s == selected)
        {
            g.setColour (Dark::on);
            g.fillRect (cols[i].toFloat().withTop ((float) cols[i].getBottom() - 3.0f));
        }
    }
}

std::vector<StatusKey> OverviewViewPage::getStatusKeys()
{
    // Only while the page shows channels: a DCA or bus layer has the five VIEW keys.
    auto keys = ViewPage::getStatusKeys();
    const auto faders = pageFaders();
    if (std::none_of (faders.begin(), faders.end(), [] (const state::FaderRef& f) { return (f.isStrip() || f.send) && isInputKind (f.strip.kind); }))
        return keys;
    const bool exclusive = ctx.settings.getBoolValue (uiprefs::exclusiveSolo, false);
    keys.push_back ({ "EXCL SOLO", std::nullopt, exclusive, [this, exclusive]
                      {
                          ctx.settings.setValue (uiprefs::exclusiveSolo, ! exclusive);
                          notifyStatusKeysChanged();
                      } });
    return keys;
}

void OverviewViewPage::mouseUp (const juce::MouseEvent& e)
{
    const auto faders = pageFaders();
    const auto cols = stripColumns (stripArea (content), juce::jmax ((int) faders.size(), pageSize()));
    const auto pos = e.position;
    for (size_t i = 0; i < faders.size() && i < cols.size(); ++i)
    {
        if (! cols[i].toFloat().contains (pos))
            continue;
        const auto t = resolve (ctx, faders[i]);
        const auto blocks = t.strip != nullptr ? blocksOf (*t.strip) : Blocks {};
        const auto rows = overviewRows (cols[i], blocks.beforeFader);
        if (t.dca >= 0)
        {
            // A DCA's column opens its assign screen.
            assign->show (t.dca);
            notifyStatusKeysChanged();
            return;
        }
        if (t.strip == nullptr)
            return;
        auto tab = HomeTab::Home;
        if (rows.input.contains (pos))
            tab = HomeTab::Input;
        for (size_t k = 0; k < blocks.tabs.size(); ++k)
            if (rows.blocks[k].contains (pos))
                tab = blocks.tabs[k];
        if (rows.pan.contains (pos))
            tab = HomeTab::Main;
        else if (rows.sends.contains (pos))
            tab = HomeTab::Sends;
        if (ctx.selectStrip)
            ctx.selectStrip (t.strip);
        if (ctx.showHomeTab)
            ctx.showHomeTab (tab);
        return;
    }
}

//==============================================================================
namespace
{
    struct FaderRows
    {
        juce::Rectangle<float> name, number, pan, gr, panel, mute, fader, meter, assign, label;
    };

    FaderRows faderRows (juce::Rectangle<int> column)
    {
        // WING px down the column (its top at the page's top, y = 80).
        const auto c = column.toFloat();
        auto at = [&c] (float x, float y, float w, float h) { return juce::Rectangle<float> (c.getX() + wingPx (x), c.getY() + wingPx (y), wingPx (w), wingPx (h)); };
        FaderRows r;
        r.name = at (0.0f, 0.0f, 96.0f, 22.0f);
        r.number = at (0.0f, 22.0f, 96.0f, 42.0f);
        r.pan = at (0.0f, 72.0f, 96.0f, 52.0f);
        r.gr = at (0.0f, 128.0f, 96.0f, 18.0f);
        r.assign = at (4.0f, 150.0f, 88.0f, 10.0f);
        r.panel = at (0.0f, 164.0f, 96.0f, 508.0f);
        r.mute = at (4.0f, 168.0f, 88.0f, 50.0f);
        r.fader = at (4.0f, 224.0f, 70.0f, 400.0f);
        r.meter = at (77.0f, 227.0f, 15.0f, 394.0f);
        r.label = at (0.0f, 632.0f, 96.0f, 32.0f);
        return r;
    }

    /** MUTE [WING]: a 3 px black frame round the key, lit red while muted. */
    void paintMuteKey (juce::Graphics& g, juce::Rectangle<float> r, bool muted)
    {
        g.setColour (juce::Colours::black);
        g.fillRect (r);
        auto k = r.reduced (wingPx (3.0f));
        g.setColour (muted ? Dark::muted : Colours::screenTile);
        g.fillRect (k);
        g.setColour (muted ? juce::Colours::black : Dark::textDim);
        g.setFont (capFontFit (14.0f, k.getHeight() * 0.55f));
        g.drawFittedText ("MUTE", k.toNearestInt(), juce::Justification::centred, 1, 0.6f);
    }

    /** The fader's well: a 2 px black frame round #101010. */
    void paintFaderWell (juce::Graphics& g, juce::Rectangle<float> r)
    {
        g.setColour (juce::Colours::black);
        g.fillRect (r);
        g.setColour (Colours::screenDark);
        g.fillRect (r.reduced (wingPx (2.0f)));
    }

    /** DCA 1-16, MAIN 1-4 and MUTE GROUP 1-8 membership as rows of tiny cells. */
    void paintAssignments (juce::Graphics& g, juce::Rectangle<float> r, Strip& s)
    {
        const uint32_t dca = s.params.dcaMask.load(), mute = s.params.muteGroupMask.load();
        const float rowH = r.getHeight() / 3.0f;
        auto row = [&g, &r, rowH] (int index, int count, auto isOn, juce::Colour c)
        {
            const float w = r.getWidth() / 16.0f;
            for (int i = 0; i < count; ++i)
            {
                g.setColour (isOn (i) ? c : Colours::screenPanel);
                g.fillRect (juce::Rectangle<float> (r.getX() + w * (float) i, r.getY() + rowH * (float) index, w, rowH).reduced (0.5f));
            }
        };
        row (0, kNumDcas, [dca] (int i) { return (dca >> i) & 1u; }, Colours::cyan);
        row (1, kNumMains, [&s] (int i) { return s.params.mains[i].on.load(); }, Dark::faderFill);
        row (2, kNumMuteGroups, [mute] (int i) { return (mute >> i) & 1u; }, Dark::muteGroup);
    }

    juce::String bottomLabel (const Target& t)
    {
        return t.strip != nullptr ? t.strip->getLabel() : "DCA " + juce::String (t.dca + 1);
    }
} // namespace

void FadersViewPage::paintContent (juce::Graphics& g)
{
    const auto faders = pageFaders();
    const auto cols = stripColumns (stripArea (content), juce::jmax ((int) faders.size(), pageSize()));
    auto* selected = ctx.getSelectedStrip ? ctx.getSelectedStrip() : nullptr;
    for (size_t i = 0; i < faders.size() && i < cols.size(); ++i)
    {
        const auto t = withSendsOnFader (ctx, resolve (ctx, faders[i]));
        if (! t.isValid())
            continue;
        const auto rows = faderRows (cols[i]);
        g.setColour (Colours::screenPanel);
        g.fillRect (rows.panel);
        paintNameBar (g, rows.name, colourOf (ctx, t), barName (ctx, t));
        paintNumberTile (g, rows.number, t);
        // A send fader's MUTE switches the send (lit while it's off), as on the surface.
        paintMuteKey (g, rows.mute, t.isSend() ? ! t.sendOn->load() : muteOf (ctx, t).load());
        paintFaderWell (g, rows.fader);

        auto faderArea = rows.fader.reduced (wingPx (2.0f));
        if (t.strip == nullptr)
        {
            // A DCA has no meter: the console draws only its top segments.
            for (int ch = 0; ch < 2; ++ch)
                verticalMeter (g, rows.meter.withHeight (wingPx (86.0f)).withWidth (rows.meter.getWidth() / 2.0f - 1.0f).translated ((float) ch * rows.meter.getWidth() / 2.0f, 0.0f),
                               0.0f, Colours::meterGreen, juce::Colours::black);
        }
        else
        {
            auto& s = *t.strip;
            block (g, rows.pan);
            auto pan = rows.pan.reduced (3.0f);
            panArc (g, pan.removeFromLeft (juce::jmin (pan.getHeight(), pan.getWidth() * 0.5f)), s.params.pan.load(), s.params.width.load());
            sendQuadrants (g, pan.reduced (2.0f, 1.0f), ctx, s);
            if (s.getKind() == StripKind::Channel)
            {
                // GATE and COMP gain reduction: the gate's square lights green while it's open.
                auto gr = rows.gr;
                for (int k = 0; k < 2; ++k)
                {
                    auto bar = gr.removeFromTop (gr.getHeight() / (k == 0 ? 2.0f : 1.0f)).reduced (0.0f, 1.0f);
                    auto& slot = k == 0 ? s.params.gate : s.params.dynamics;
                    const bool isGate = models::model (models::Dynamics, slot.model.load()).token == "GATE";
                    auto square = bar.removeFromLeft (bar.getHeight());
                    if (k == 0 && isGate)
                    {
                        g.setColour (slot.on.load() && ! s.meters.gateActive.load() ? Colours::meterGreen : Colours::screenPanel);
                        g.fillRect (square.reduced (1.0f));
                    }
                    bar.removeFromLeft (2.0f);
                    g.setColour (track);
                    g.fillRect (bar);
                    const float reduction = (k == 0 ? s.meters.gateReductionDb.load() : s.meters.dynReductionDb.load()) / 24.0f;
                    g.setColour (k == 0 && ! isGate ? Colours::meterYellow : Colours::meterGreen);
                    g.fillRect (bar.withWidth (bar.getWidth() * juce::jlimit (0.0f, 1.0f, reduction)));
                }
            }
            // [fig. 56] the level meter right of the fader: a column of
            // segments (unlit black) along the whole fader.
            auto meter = rows.meter;
            const int numCh = juce::jlimit (1, 2, s.getNumChannels());
            for (int ch = 0; ch < numCh; ++ch)
                verticalMeter (g, meter.withWidth (meter.getWidth() / (float) numCh - (numCh > 1 ? 1.0f : 0.0f)).translated ((float) ch * meter.getWidth() / (float) numCh, 0.0f),
                               meterPos (dsp::gainToDb (ctx.meters.get (&s, prefs::meterPoint (ctx, s.getKind()), ch))), Colours::meterGreen, juce::Colours::black);
            paintAssignments (g, rows.assign, s);
        }
        // Send faders are drawn in the send colour, with the destination under them.
        paintFader (g, faderArea, faderOf (ctx, t).load(), t.isSend() ? sendRed : Dark::faderFill, ! t.isSend() || t.sendOn->load());
        // A DCA's label is yellow while the DCA is soloed [WING].
        const bool dcaSolo = t.strip == nullptr && ! t.isSend() && ctx.engine.getDca (t.dca).solo.load();
        g.setColour (t.isSend() ? sendRed.brighter (0.4f) : dcaSolo ? Colours::solo : darkLabel);
        g.setFont (capFontFit (20.0f, rows.label.getHeight() * 0.9f, WingWeight::SemiBold));
        g.drawFittedText (t.isSend() ? t.sendTag : bottomLabel (t), rows.label.toNearestInt(), juce::Justification::centred, 1, 0.6f);
        if (t.strip != nullptr && t.strip == selected)
        {
            g.setColour (Dark::on);
            g.fillRect (cols[i].toFloat().withTop ((float) cols[i].getBottom() - 3.0f));
        }
    }
}

std::atomic<float>* FadersViewPage::faderAt (juce::Point<int> pos, juce::Rectangle<int>* columnOut) const
{
    const auto faders = pageFaders();
    const auto cols = stripColumns (stripArea (content), juce::jmax ((int) faders.size(), pageSize()));
    for (size_t i = 0; i < faders.size() && i < cols.size(); ++i)
        if (cols[i].contains (pos))
        {
            const auto t = withSendsOnFader (ctx, resolve (ctx, faders[i]));
            if (! t.isValid() || ! faderRows (cols[i]).fader.contains (pos.toFloat()))
                return nullptr;
            if (columnOut != nullptr)
                *columnOut = cols[i];
            return &faderOf (ctx, t);
        }
    return nullptr;
}

void FadersViewPage::mouseDown (const juce::MouseEvent& e)
{
    dragging = faderAt (e.getPosition(), &dragColumn);
    if (dragging != nullptr)
    {
        dragPos = fader::dbToPos (dragging->load());
        lastY = e.getPosition().y;
        // TOUCH FADER SELECT: the knob right of the display fine-tunes the touched fader.
        if (ctx.setFineTarget && ctx.settings.getBoolValue (uiprefs::touchFaderSelect, false))
            ctx.setFineTarget (bindFader ("LEVEL", *dragging), this);
    }
}

void FadersViewPage::mouseDrag (const juce::MouseEvent& e)
{
    if (dragging == nullptr)
        return;
    const float h = (float) juce::jmax (40, dragColumn.getHeight() / 2);
    dragPos = juce::jlimit (0.0f, 1.0f, dragPos + (float) (lastY - e.getPosition().y) / h * dragScale (ctx, dragColumn, e.getPosition()));
    lastY = e.getPosition().y;
    dragging->store (fader::posToDb (dragPos));
    repaint();
}

void FadersViewPage::mouseUp (const juce::MouseEvent& e)
{
    const bool dragged = dragging != nullptr && e.mouseWasDraggedSinceMouseDown();
    dragging = nullptr;
    if (dragged)
        return;
    const auto faders = pageFaders();
    const auto cols = stripColumns (stripArea (content), juce::jmax ((int) faders.size(), pageSize()));
    const auto pos = e.position;
    for (size_t i = 0; i < faders.size() && i < cols.size(); ++i)
    {
        if (! cols[i].toFloat().contains (pos))
            continue;
        const auto t = withSendsOnFader (ctx, resolve (ctx, faders[i]));
        if (! t.isValid())
            return; // a blank slot (or an FX parameter / MIDI CC): nothing to press
        const auto rows = faderRows (cols[i]);
        if (rows.mute.contains (pos))
        {
            auto& m = t.isSend() ? *t.sendOn : muteOf (ctx, t);
            m = ! m.load();
        }
        else if (rows.name.contains (pos) || rows.number.contains (pos))
        {
            if (t.dca >= 0)
                showDcaPanel (t.dca);
            else if (t.strip != nullptr)
            {
                if (ctx.selectStrip)
                    ctx.selectStrip (t.strip);
                if (ctx.showHomeTab)
                    ctx.showHomeTab (HomeTab::Home);
            }
        }
        else if (t.strip != nullptr && rows.pan.contains (pos))
        {
            if (ctx.selectStrip)
                ctx.selectStrip (t.strip);
            if (ctx.showHomeTab)
                ctx.showHomeTab (HomeTab::Main);
        }
        else if (t.strip != nullptr && ctx.selectStrip)
            ctx.selectStrip (t.strip);
        repaint();
        return;
    }
}

void FadersViewPage::mouseDoubleClick (const juce::MouseEvent& e)
{
    if (auto* p = faderAt (e.getPosition()))
        p->store (0.0f);
}

void FadersViewPage::mouseWheelMove (const juce::MouseEvent& e, const juce::MouseWheelDetails& w)
{
    if (auto* p = faderAt (e.getPosition()))
        p->store (fader::posToDb (juce::jlimit (0.0f, 1.0f, fader::dbToPos (p->load()) + (w.deltaY > 0.0f ? 0.01f : -0.01f))));
}

//==============================================================================
std::vector<SendsViewPage::Destination> SendsViewPage::destinations() const
{
    // BUS 1-8, BUS 9-16 (and more banks of 8 for extra buses), MAIN, MATRIX.
    std::vector<Destination> d;
    const int buses = (int) ctx.engine.getBuses().size();
    const int busGroups = (buses + 7) / 8;
    if (group < busGroups)
    {
        for (int i = group * 8; i < juce::jmin (buses, group * 8 + 8); ++i)
            d.push_back ({ StripKind::Bus, i });
    }
    else if (group == busGroups)
        for (int i = 0; i < kNumMains; ++i)
            d.push_back ({ StripKind::Main, i });
    else
        for (int i = 0; i < kNumMatrices; ++i)
            d.push_back ({ StripKind::Matrix, i });
    return d;
}

SendsViewPage::Destination SendsViewPage::destination() const
{
    const auto d = destinations();
    return d.empty() ? Destination {} : d[(size_t) juce::jlimit (0, (int) d.size() - 1, chosen)];
}

Strip* SendsViewPage::destinationStrip() const
{
    const auto d = destination();
    auto& list = ctx.engine.getStrips (d.kind);
    return juce::isPositiveAndBelow (d.index, (int) list.size()) ? list[(size_t) d.index].get() : nullptr;
}

bool SendsViewPage::canSend (const Strip& s) const
{
    auto* dest = destinationStrip();
    if (dest == nullptr)
        return false;
    switch (dest->getKind())
    {
        // Inputs feed every bus, buses 9 and up feed buses 1-8.
        case StripKind::Bus:    return s.isInput() || (s.isBus() && s.getId() >= kBusToBusTargets && dest->getId() < kBusToBusTargets);
        case StripKind::Main:   return s.isInput() || s.isBus();
        case StripKind::Matrix: return s.getKind() != StripKind::Matrix;
        case StripKind::Channel:
        case StripKind::Aux:
            break;
    }
    return false;
}

std::atomic<float>* SendsViewPage::sendLevel (Strip& s, bool create) const
{
    if (! canSend (s))
        return nullptr;
    auto* dest = destinationStrip();
    const auto d = destination();
    if (d.kind == StripKind::Bus)
    {
        auto* send = create ? &s.sends[(size_t) dest->getId()] : s.sends.find (dest->getId());
        return send != nullptr ? &send->levelDb : nullptr;
    }
    return d.kind == StripKind::Main ? &s.params.mains[d.index].levelDb : &s.params.matrix[d.index].levelDb;
}

std::atomic<bool>* SendsViewPage::sendOn (Strip& s, bool create) const
{
    if (! canSend (s))
        return nullptr;
    auto* dest = destinationStrip();
    const auto d = destination();
    if (d.kind == StripKind::Bus)
    {
        auto* send = create ? &s.sends[(size_t) dest->getId()] : s.sends.find (dest->getId());
        return send != nullptr ? &send->enabled : nullptr;
    }
    return d.kind == StripKind::Main ? &s.params.mains[d.index].on : &s.params.matrix[d.index].on;
}

void SendsViewPage::layoutContent()
{
    // [WING] the destination keys on a panel from x = 221, the destination
    // tiles on one from x = 587 (WING y = 80 to 164); the strips under them.
    strips = stripArea (content).withTrimmedTop (px (92.0f));
    groupKeys.clear();
    destTiles.clear();
    const int buses = (int) ctx.engine.getBuses().size();
    const int groups = juce::jmin (6, (buses + 7) / 8 + 2);
    const float kw = juce::jmin (87.0f, 348.0f / (float) juce::jmax (4, groups));
    for (int i = 0; i < groups; ++i)
        groupKeys.push_back (wingRect (230.0f + kw * (float) i, 5.0f, kw - 4.0f, 70.0f));
    for (int i = 0; i < 8; ++i)
        destTiles.push_back (wingRect (591.0f + 58.0f * (float) i, 0.0f, 58.0f, 80.0f));
}

void SendsViewPage::paintContent (juce::Graphics& g)
{
    // The panels, the destination keys (BUS 1-8, BUS 9-16, MAIN, MATRIX).
    g.setColour (Colours::screenPanel);
    g.fillRect (wingRect (221.0f, 0.0f, 362.0f, 84.0f));
    g.fillRect (wingRect (587.0f, 0.0f, 472.0f, 84.0f));
    const int buses = (int) ctx.engine.getBuses().size();
    const int busGroups = (buses + 7) / 8;
    for (size_t i = 0; i < groupKeys.size(); ++i)
    {
        const int gi = (int) i;
        const juce::String name = gi < busGroups ? "BUS " + juce::String (gi * 8 + 1) + "-" + juce::String (juce::jmin (buses, gi * 8 + 8))
                                                 : gi == busGroups ? juce::String ("MAIN") : juce::String ("MATRIX");
        const bool on = gi == group;
        wingKey (g, groupKeys[i], name, on ? Dark::layer : Colours::screenTile, on ? Dark::choiceInk : keyText, 13.0f);
    }

    // Destination tiles: the label on a bar in the destination's colour, its
    // icon and name below; the chosen one framed gold.
    const auto dests = destinations();
    for (size_t i = 0; i < destTiles.size(); ++i)
    {
        if (i >= dests.size())
            continue;
        auto& list = ctx.engine.getStrips (dests[i].kind);
        if (! juce::isPositiveAndBelow (dests[i].index, (int) list.size()))
            continue;
        auto& s = *list[(size_t) dests[i].index];
        auto r = destTiles[i].toFloat().reduced (wingPx (2.0f));
        auto header = r.removeFromTop (wingPx (16.0f));
        g.setColour (stripDisplayColour (s));
        g.fillRect (header);
        g.setColour (juce::Colours::black);
        g.setFont (capFontFit (10.0f, header.getHeight(), WingWeight::SemiBold));
        g.drawFittedText (shortStripLabel (s), header.toNearestInt(), juce::Justification::centred, 1);
        g.setColour (blockFill);
        g.fillRect (r);
        auto name = r.removeFromBottom (wingPx (16.0f));
        g.setColour (juce::Colours::black);
        g.fillRect (name.withHeight (wingPx (1.0f)).translated (0.0f, -wingPx (1.0f)));
        drawWingIcon (g, s.getIcon(), r.reduced (r.getHeight() * 0.15f), Dark::textDim);
        if (stripName (s) != s.getLabel())
        {
            g.setColour (Dark::text);
            g.setFont (capFontFit (9.0f, name.getHeight() * 0.9f));
            g.drawFittedText (stripName (s).toUpperCase(), name.toNearestInt(), juce::Justification::centred, 1, 0.6f);
        }
        if ((int) i == chosen)
        {
            g.setColour (Dark::selectedOutline);
            g.drawRect (destTiles[i].toFloat(), wingPx (2.0f));
        }
    }

    // The layer's strips as send faders (a DCA has none: nothing shows, as on the console).
    const auto faders = pageFaders();
    const auto cols = stripColumns (strips, juce::jmax ((int) faders.size(), pageSize()));
    for (size_t i = 0; i < faders.size() && i < cols.size(); ++i)
    {
        const auto t = resolve (ctx, faders[i]);
        if (t.strip == nullptr)
            continue;
        auto c = cols[i].withTrimmedBottom (6).toFloat();
        const float h = c.getHeight();
        auto name = c.removeFromTop (h * 0.05f);
        c.removeFromTop (2.0f);
        auto number = c.removeFromTop (h * 0.075f);
        c.removeFromTop (4.0f);
        auto onKey = c.removeFromTop (h * 0.075f);
        c.removeFromTop (4.0f);
        auto label = c.removeFromBottom (h * 0.08f);
        paintNameBar (g, name, colourOf (ctx, t), barName (ctx, t));
        paintNumberTile (g, number, t);
        auto* level = t.strip != nullptr ? sendLevel (*t.strip, false) : nullptr;
        auto* on = t.strip != nullptr ? sendOn (*t.strip, false) : nullptr;
        const bool sends = t.strip != nullptr && canSend (*t.strip);
        const bool isOn = on != nullptr && on->load();
        miniChip (g, onKey.reduced (onKey.getWidth() * 0.12f, 1.0f), "ON", isOn, Dark::on);
        g.setColour (Colours::screenDark);
        g.fillRect (c);
        auto meter = c.removeFromRight (juce::jmax (5.0f, c.getWidth() * 0.12f));
        if (t.strip != nullptr)
            for (int ch = 0; ch < 2; ++ch)
                verticalMeter (g, meter.withWidth (meter.getWidth() / 2.0f - 0.5f).translated ((float) ch * meter.getWidth() / 2.0f, 0.0f),
                               meterPos (dsp::gainToDb (ctx.meters.get (t.strip, prefs::meterPoint (ctx, t.strip->getKind()), ch))), Colours::meterGreen, Colours::screenTile);
        if (sends)
            paintFader (g, c.reduced (2.0f, 2.0f), level != nullptr ? level->load() : -100.0f, sendRed, isOn);
        g.setColour (Dark::textDim);
        g.setFont (viewFont (TypeScale::value, label.getHeight() * 0.7f));
        g.drawFittedText (sends ? "SEND" : "-", label.toNearestInt(), juce::Justification::centred, 1);
    }
}

std::atomic<float>* SendsViewPage::faderAt (juce::Point<int> pos, juce::Rectangle<int>* columnOut) const
{
    const auto faders = pageFaders();
    const auto cols = stripColumns (strips, juce::jmax ((int) faders.size(), pageSize()));
    for (size_t i = 0; i < faders.size() && i < cols.size(); ++i)
        if (cols[i].contains (pos))
        {
            const auto t = resolve (ctx, faders[i]);
            if (t.strip == nullptr)
                return nullptr;
            const auto c = cols[i].withTrimmedBottom (6);
            const int h = c.getHeight();
            if (pos.y < c.getY() + h * 25 / 100 || pos.y > c.getBottom() - h * 8 / 100)
                return nullptr;
            if (columnOut != nullptr)
                *columnOut = cols[i];
            return sendLevel (*t.strip, true);
        }
    return nullptr;
}

void SendsViewPage::mouseDown (const juce::MouseEvent& e)
{
    dragging = faderAt (e.getPosition(), &dragColumn);
    if (dragging != nullptr)
    {
        dragPos = fader::dbToPos (dragging->load());
        lastY = e.getPosition().y;
    }
}

void SendsViewPage::mouseDrag (const juce::MouseEvent& e)
{
    if (dragging == nullptr)
        return;
    const float h = (float) juce::jmax (40, dragColumn.getHeight() / 2);
    dragPos = juce::jlimit (0.0f, 1.0f, dragPos + (float) (lastY - e.getPosition().y) / h * dragScale (ctx, dragColumn, e.getPosition()));
    lastY = e.getPosition().y;
    dragging->store (fader::posToDb (dragPos));
    repaint();
}

void SendsViewPage::mouseUp (const juce::MouseEvent& e)
{
    const bool dragged = dragging != nullptr && e.mouseWasDraggedSinceMouseDown();
    dragging = nullptr;
    if (dragged)
        return;
    const auto pos = e.getPosition();
    for (size_t i = 0; i < groupKeys.size(); ++i)
        if (groupKeys[i].contains (pos))
        {
            group = (int) i;
            chosen = 0;
            repaint();
            return;
        }
    for (size_t i = 0; i < destTiles.size(); ++i)
        if (destTiles[i].contains (pos) && i < destinations().size())
        {
            chosen = (int) i;
            repaint();
            return;
        }
    const auto faders = pageFaders();
    const auto cols = stripColumns (strips, juce::jmax ((int) faders.size(), pageSize()));
    for (size_t i = 0; i < faders.size() && i < cols.size(); ++i)
    {
        if (! cols[i].contains (pos))
            continue;
        const auto t = resolve (ctx, faders[i]);
        if (t.strip == nullptr)
            return;
        const auto c = cols[i].withTrimmedBottom (6);
        const int h = c.getHeight();
        const int onTop = c.getY() + juce::roundToInt ((float) h * 0.125f) + 6;
        if (pos.y >= onTop && pos.y < onTop + juce::roundToInt ((float) h * 0.075f))
        {
            if (auto* on = sendOn (*t.strip, true))
                *on = ! on->load();
        }
        else if (pos.y < onTop && ctx.selectStrip)
            ctx.selectStrip (t.strip);
        repaint();
        return;
    }
}

void SendsViewPage::mouseDoubleClick (const juce::MouseEvent& e)
{
    if (auto* p = faderAt (e.getPosition()))
        p->store (0.0f);
}

//==============================================================================
//==============================================================================
namespace
{
    /** A new user layer's name: "USER n" after its place in the list (so the
        layer on the USER 2 key reads USER 2), or the next n not taken yet. */
    juce::String freeUserLayerName (LayerSet& layers)
    {
        const auto& user = layers.getUserLayers();
        for (int n = (int) user.size() + 1;; ++n)
        {
            const auto name = "USER " + juce::String (n);
            if (std::none_of (user.begin(), user.end(), [&] (const Layer& l) { return l.name.equalsIgnoreCase (name); }))
                return name;
        }
    }

    int userLayerIndex (LayerSet& layers, const juce::String& id)
    {
        const auto& user = layers.getUserLayers();
        for (int i = 0; i < (int) user.size(); ++i)
            if (user[(size_t) i].id == id)
                return i;
        return -1;
    }

    /** USER 1 / USER 2's layer as it starts out: empty, named after its key. */
    bool isBlankUserKeyLayer (LayerSet& layers, const juce::String& id)
    {
        const int i = userLayerIndex (layers, id);
        if (i < 0 || i >= 2)
            return false;
        const auto& l = layers.getUserLayers()[(size_t) i];
        return l.faders.empty() && l.name.equalsIgnoreCase ("USER " + juce::String (i + 1));
    }

    /** Deletes a user layer. The layers behind USER 1 / USER 2 keep their
        keys: deleting USER 1's layer leaves it blank rather than moving USER
        2's layer onto it. Blank key layers left at the end go too. */
    void deleteUserLayer (LayerSet& layers, const juce::String& id)
    {
        const int index = userLayerIndex (layers, id);
        if (index < 0)
            return;
        const int count = (int) layers.getUserLayers().size();
        layers.removeUserLayer (id);
        if (index < 2 && index < count - 1)
        {
            const auto blank = layers.addUserLayer ("USER " + juce::String (index + 1));
            for (int i = (int) layers.getUserLayers().size() - 1; i > index; --i)
                layers.moveUserLayer (blank, -1);
        }
        while (! layers.getUserLayers().empty() && isBlankUserKeyLayer (layers, layers.getUserLayers().back().id))
            layers.removeUserLayer (layers.getUserLayers().back().id);
    }
} // namespace

ConfigViewPage::ConfigViewPage (UiContext& c) : ViewPage (c, Screen::Config)
{
    // SENDS (the input channels' sends to a bus, main or matrix), BUS -> BUS
    // (the buses' sends to one) and FX (a slot's parameters) as faders; each
    // field picks its destination or slot and shows those items. MIDI CC: a
    // MIDI channel's CCs (32 at a time) as faders sending out of EXTERNAL MIDI
    // CONTROL (the WING's layer node type MIDI).
    for (int i = 0; i < 4; ++i)
    {
        static const char* captions[] = { "SENDS", "BUS -> BUS", "FX", "MIDI CC" };
        auto* f = fields.add (new DropdownField (captions[i], [this, i] { return fieldText (i); }, [this, i] (DropdownField& field) { chooseField (i, field); }));
        addAndMakeVisible (f);
    }
    for (auto* k : { &clearKey, &renameKey, &newKey, &deleteKey })
    {
        styleIconKey (*k, k != &deleteKey);
        addAndMakeVisible (k);
    }
    newKey.setTooltip ("A new user layer, copied from the layer shown");
    deleteKey.setTooltip ("Delete the user layer, or give an edited layer key its own faders back");
    clearKey.onClick = [this]
    {
        const auto layer = currentLayer();
        popups::confirm (*this, "CLEAR", "Remove every fader from \"" + layer.name + "\"?", "CLEAR", [this, layer] { setLayerFaders (layer, {}); }, true);
    };
    renameKey.onClick = [this]
    {
        const auto layer = currentLayer();
        const auto id = userLayerId (layer, true);
        if (id.isEmpty())
            return;
        popups::TextOptions options;
        options.icon = Icon::Layers;
        juce::Component::SafePointer<ConfigViewPage> safe (this);
        popups::editText (*this, "LAYER NAME", layer.name, [safe, id] (const juce::String& text)
        {
            if (safe == nullptr || text.trim().isEmpty())
                return;
            safe->ctx.show.layers.renameUserLayer (id, text.trim());
            notifyLayersChangedAsync (safe->ctx);
            safe->resized();
            safe->notifyStatusKeysChanged();
        }, options);
    };
    newKey.onClick = [this]
    {
        // A new name ("USER 3"), so it isn't mistaken for the layer it copies.
        const auto layer = currentLayer();
        const auto id = ctx.show.layers.addUserLayer (freeUserLayerName (ctx.show.layers), layer.faders);
        notifyLayersChangedAsync (ctx);
        selectLayer (id);
        resized();
    };
    deleteKey.onClick = [this]
    {
        const auto layer = currentLayer();
        if (const auto id = userLayerId (layer, false); id.isNotEmpty() && ! isBlankUserKeyLayer (ctx.show.layers, id))
        {
            popups::confirm (*this, "DELETE LAYER", "Delete the user layer \"" + layer.name + "\"?", "DELETE", [this, id, viaKey = ! layer.user]
            {
                deleteUserLayer (ctx.show.layers, id);
                // USER 1 / USER 2 stay selected (now blank); another layer
                // goes back to the section's first key.
                const auto defaults = ctx.show.layers.getDefaultLayers (ctx.engine, section());
                if (! viaKey && ! defaults.empty())
                    selectLayer (defaults.front().id);
                notifyLayersChangedAsync (ctx);
                resized();
            }, true);
        }
        else if (ctx.show.layers.hasDefaultOverride (section(), layer.id))
        {
            popups::confirm (*this, "RESTORE LAYER", "Give \"" + layer.name + "\" its own faders back?", "RESTORE", [this, id = layer.id]
            {
                ctx.show.layers.clearDefaultOverride (section(), id);
                notifyLayersChangedAsync (ctx);
                resized();
                repaint();
            }, true);
        }
    };
}

void ConfigViewPage::layerChanged()
{
    updateKeys();
}

juce::String ConfigViewPage::userLayerId (const Layer& layer, bool create)
{
    if (layer.user)
        return layer.id;
    if (layer.id != "user-1" && layer.id != "user-2")
        return {};
    // USER 1 / USER 2 show the first two user layers: make them on first use.
    auto& layers = ctx.show.layers;
    const size_t index = layer.id == "user-1" ? 0 : 1;
    while (create && layers.getUserLayers().size() <= index)
        layers.addUserLayer ("USER " + juce::String ((int) layers.getUserLayers().size() + 1));
    const auto& user = layers.getUserLayers();
    return index < user.size() ? user[index].id : juce::String();
}

void ConfigViewPage::setLayerFaders (const Layer& layer, const std::vector<state::FaderRef>& faders)
{
    auto& layers = ctx.show.layers;
    if (const auto id = userLayerId (layer, true); id.isNotEmpty())
    {
        // A user layer: its slots as given, blanks included.
        layers.setUserLayerFaders (id, faders);
    }
    else
    {
        // A default key keeps an edited copy in the show.
        layers.setDefaultOverride (section(), layer.id, faders);
    }
    notifyLayersChangedAsync (ctx);
    resized(); // the layer column and the page map show the new pages
    repaint();
}

void ConfigViewPage::updateKeys()
{
    const auto layer = currentLayer();
    enableKey (clearKey, ! layer.faders.empty());
    enableKey (renameKey, layer.user || layer.id == "user-1" || layer.id == "user-2");
    const auto userId = userLayerId (layer, false);
    enableKey (deleteKey, (userId.isNotEmpty() && ! isBlankUserKeyLayer (ctx.show.layers, userId))
                              || ctx.show.layers.hasDefaultOverride (section(), layer.id));
    deleteKey.setButtonText (! layer.user && ctx.show.layers.hasDefaultOverride (section(), layer.id) ? "RESTORE" : "DELETE");
}

std::vector<state::StripRef> ConfigViewPage::destinations (bool busToBus) const
{
    // Inputs send to every bus, main and matrix; buses 9+ to buses 1-8, every bus to the mains and matrices.
    std::vector<state::StripRef> out;
    for (auto& b : ctx.engine.getStrips (StripKind::Bus))
        if (! busToBus || b->getId() < kBusToBusTargets)
            out.push_back (state::StripRef::of (*b));
    for (auto k : { StripKind::Main, StripKind::Matrix })
        for (auto& s : ctx.engine.getStrips (k))
            out.push_back (state::StripRef::of (*s));
    return out;
}

juce::String ConfigViewPage::fieldText (int field) const
{
    if (field == 2)
        return juce::String (fxSlot + 1);
    if (field == 3)
        return midiBankName (midiBank);
    const auto dests = destinations (field == 1);
    const int i = field == 0 ? sendDest : busDest;
    if (! juce::isPositiveAndBelow (i, (int) dests.size()))
        return "-";
    auto* s = state::findStrip (ctx.engine, dests[(size_t) i]);
    return s != nullptr ? s->getLabel().toUpperCase() : juce::String ("-");
}

void ConfigViewPage::chooseField (int field, DropdownField& fieldKey)
{
    juce::StringArray items;
    int selected = 0;
    if (field == 2)
    {
        auto& fx = ctx.engine.getFx();
        for (int i = 0; i < kNumFx; ++i)
            items.add ("FX " + juce::String (i + 1) + "  " + (fx.getModel (i) > ld::kFxExt ? models::model (models::Fx, fx.getModel (i)).name.toUpperCase() : juce::String ("-")));
        selected = fxSlot;
    }
    else if (field == 3)
    {
        for (int i = 0; i < kNumMidiBanks; ++i)
            items.add (midiBankName (i));
        selected = midiBank;
    }
    else
    {
        for (auto& r : destinations (field == 1))
            if (auto* s = state::findStrip (ctx.engine, r))
                items.add (describeStrip (*s).toUpperCase());
        selected = field == 0 ? sendDest : busDest;
    }
    popups::ListOptions options;
    options.columns = field == 2 ? 2 : 4;
    options.caption = field == 0 ? "SENDS TO" : field == 1 ? "BUS -> BUS TO" : field == 2 ? "FX PARAMETERS OF" : "MIDI CC FADERS";
    juce::Component::SafePointer<ConfigViewPage> safe (this);
    popups::chooseFrom (fieldKey, items, selected, [safe, field] (int i)
    {
        if (safe == nullptr)
            return;
        if (field == 0)
            safe->sendDest = i;
        else if (field == 1)
            safe->busDest = i;
        else if (field == 2)
            safe->fxSlot = i;
        else
            safe->midiBank = i;
        safe->category = 3 + field;
        safe->selectedItems.clear();
        safe->layoutContent();
        safe->repaint();
    }, options);
}

std::vector<state::FaderRef> ConfigViewPage::sourceItems() const
{
    std::vector<state::FaderRef> items;
    auto add = [this, &items] (StripKind k)
    {
        for (auto& s : ctx.engine.getStrips (k))
            items.push_back (state::FaderRef::of (state::StripRef::of (*s)));
    };
    if (category == 3 || category == 4)
    {
        const auto dests = destinations (category == 4);
        const int i = category == 3 ? sendDest : busDest;
        if (! juce::isPositiveAndBelow (i, (int) dests.size()))
            return items;
        const auto to = dests[(size_t) i];
        if (category == 3)
        {
            for (auto k : { StripKind::Channel, StripKind::Aux })
                for (auto& s : ctx.engine.getStrips (k))
                    items.push_back (state::FaderRef::ofSend (state::StripRef::of (*s), to));
        }
        else
            for (auto& b : ctx.engine.getStrips (StripKind::Bus))
                if (! (to.kind == StripKind::Bus && (b->getId() < kBusToBusTargets || b->getId() == to.id)))
                    items.push_back (state::FaderRef::ofSend (state::StripRef::of (*b), to));
        return items;
    }
    if (category == 6)
    {
        const int channel = midiBank / kMidiBanksPerChannel, first = midiBank % kMidiBanksPerChannel * kMidiCcsPerBank;
        for (int i = 0; i < kMidiCcsPerBank; ++i)
            items.push_back (state::FaderRef::ofMidiCc (channel, first + i));
        return items;
    }
    if (category == 5)
    {
        auto& fx = ctx.engine.getFx();
        if (const int model = fx.getModel (fxSlot); model > ld::kFxExt)
        {
            const auto& m = models::model (models::Fx, model);
            for (size_t i = 0; i < m.params.size(); ++i)
                if (m.params[i].isContinuous())
                    items.push_back (state::FaderRef::ofFxParam (fxSlot, (int) i));
        }
        return items;
    }
    if (category == 0)
    {
        add (StripKind::Channel);
        add (StripKind::Aux);
    }
    else if (category == 1)
    {
        add (StripKind::Bus);
        add (StripKind::Main);
        add (StripKind::Matrix);
    }
    else
        for (int i = 0; i < kNumDcas; ++i)
            items.push_back (state::FaderRef::ofDca (i));
    return items;
}

void ConfigViewPage::layoutContent()
{
    // [WING] the top panel (x = 8 to 1168): CHANNELS, BUS/MAIN, DCA, the four
    // fields, CLEAR and RENAME; the items as tiles 106 apart centred under it
    // (a new row for each kind); the page's slots at the bottom.
    categoryKeys.clear();
    for (int i = 0; i < 3; ++i)
        categoryKeys.push_back (wingRect (16.0f + 124.0f * (float) i, 8.0f, 112.0f, 64.0f));
    for (int i = 0; i < fields.size(); ++i)
        fields[i]->setBounds (wingRect (388.0f + 152.0f * (float) i, 8.0f, 140.0f, 64.0f));
    clearKey.setBounds (wingRect (1018.0f, 8.0f, 64.0f, 64.0f));
    renameKey.setBounds (wingRect (1094.0f, 8.0f, 68.0f, 64.0f));

    // The page's slots, four to a blue plate, centred under the grid.
    slotAreas.clear();
    plates.clear();
    const int n = pageSize();
    const int numPlates = (n + 3) / 4;
    const float centre = 588.0f, platesW = 376.0f * (float) numPlates - 4.0f;
    const float x0 = juce::jmax (8.0f, centre - platesW / 2.0f);
    for (int p = 0; p < numPlates; ++p)
        plates.push_back (wingRect (x0 + 376.0f * (float) p, 620.0f, 372.0f, 72.0f));
    for (int i = 0; i < n; ++i)
        slotAreas.push_back (wingRect (x0 + 376.0f * (float) (i / 4) + 4.0f + 92.0f * (float) (i % 4), 624.0f, 88.0f, 64.0f));
    slotsArea = wingRect (x0, 620.0f, platesW, 92.0f);
    hintArea = slotsArea;
    // A user layer's own keys (not on the console) left of the slots, or
    // above each other left of the items when the slots take the width.
    if (x0 >= 212.0f)
    {
        newKey.setBounds (wingRect (16.0f, 624.0f, 88.0f, 64.0f));
        deleteKey.setBounds (wingRect (112.0f, 624.0f, 92.0f, 64.0f));
    }
    else
    {
        newKey.setBounds (wingRect (16.0f, 468.0f, 140.0f, 64.0f));
        deleteKey.setBounds (wingRect (16.0f, 540.0f, 140.0f, 64.0f));
    }

    // The items: a kind (channels, aux, buses, mains, matrices, DCAs) starts
    // a new row, 24 px further down; rows close up when they don't fit.
    sourceTiles.clear();
    const auto items = sourceItems();
    auto kindOf = [] (const state::FaderRef& f) { return f.dca ? -1 : (f.send ? 100 : (int) f.strip.kind); };
    struct Place
    {
        int row, col, gaps;
    };
    std::vector<Place> places;
    int rows = 0, gaps = 0, col = 8;
    for (size_t i = 0; i < items.size(); ++i)
    {
        const bool newKind = i > 0 && ! items[i].fxParam && ! items[i].midiCc && kindOf (items[i]) != kindOf (items[i - 1]);
        if (col >= 8 || newKind)
        {
            gaps += newKind ? 1 : 0;
            ++rows;
            col = 0;
        }
        places.push_back ({ rows - 1, col++, gaps });
    }
    const float top = 126.0f, bottom = 612.0f;
    const float pitch = juce::jmin (72.0f, (bottom - top - 24.0f * (float) gaps) / (float) juce::jmax (1, rows));
    const float tileH = juce::jmin (64.0f, pitch - 4.0f);
    for (auto& p : places)
        sourceTiles.push_back (wingRect (169.0f + 106.0f * (float) p.col, top + pitch * (float) p.row + 24.0f * (float) p.gaps, 96.0f, tileH));
    sourceArea = wingRect (169.0f, top, 838.0f, bottom - top);
    updateKeys();
}

void ConfigViewPage::paintContent (juce::Graphics& g)
{
    // The top panel and the category keys (the chosen one light grey).
    g.setColour (Colours::screenPanel);
    g.fillRect (wingRect (8.0f, 0.0f, 1160.0f, 80.0f));
    const char* names[] = { "CHANNELS", "BUS/MAIN", "DCA" };
    for (int i = 0; i < 3; ++i)
    {
        const bool on = i == category;
        wingKey (g, categoryKeys[(size_t) i], names[i], on ? Colours::caption : Colours::screenTile, on ? juce::Colours::black : Dark::text, 14.0f);
    }

    // The active SENDS / BUS -> BUS / FX field is framed.
    if (category >= 3 && category - 3 < fields.size())
    {
        g.setColour (Colours::yellow);
        g.drawRect (fields[category - 3]->getBounds().expanded (2), 2);
    }

    auto tile = [this, &g] (juce::Rectangle<float> r, const state::FaderRef& f, bool highlighted)
    {
        if (f.midiCc)
        {
            // A MIDI CC: its MIDI channel on a bar, the CC number large.
            auto bar = r.removeFromTop (r.getHeight() * 0.36f);
            paintNameBar (g, bar, Colours::midiCc, "MIDI CH " + juce::String (f.midiChannel + 1));
            g.setColour (blockFill);
            g.fillRect (r);
            g.setColour (Dark::text);
            g.setFont (viewFont (TypeScale::value * 1.2f, r.getHeight() * 0.85f));
            g.drawFittedText ("CC " + juce::String (f.cc), r.reduced (3.0f, 1.0f).toNearestInt(), juce::Justification::centredLeft, 1, 0.7f);
            if (highlighted)
            {
                g.setColour (Colours::yellow);
                g.drawRect (bar.getUnion (r).expanded (1.0f), 2.0f);
            }
            return;
        }
        if (f.fxParam)
        {
            // An FX parameter: the slot on an FX-coloured bar, the parameter's name.
            const int model = ctx.engine.getFx().getModel (f.fxSlot);
            const auto& m = models::model (models::Fx, juce::jmax (0, model));
            const auto label = juce::isPositiveAndBelow (f.param, (int) m.params.size()) && model > ld::kFxExt ? m.params[(size_t) f.param].label : juce::String ("?");
            auto bar = r.removeFromTop (r.getHeight() * 0.36f);
            paintNameBar (g, bar, FileColours::fx, "FX " + juce::String (f.fxSlot + 1));
            g.setColour (blockFill);
            g.fillRect (r);
            g.setColour (Dark::text);
            g.setFont (viewFont (TypeScale::body, r.getHeight() * 0.6f));
            g.drawFittedText (label.toUpperCase(), r.reduced (3.0f, 1.0f).toNearestInt(), juce::Justification::centredLeft, 1, 0.6f);
            if (highlighted)
            {
                g.setColour (Colours::yellow);
                g.drawRect (bar.getUnion (r).expanded (1.0f), 2.0f);
            }
            return;
        }
        const auto t = resolve (ctx, f);
        if (! t.isValid())
            return;
        if (t.strip != nullptr && ! f.send)
        {
            paintStripTile (g, r.toNearestInt(), *t.strip, highlighted);
            return;
        }
        if (t.dca >= 0)
        {
            // [WING] a DCA: its colour bar (its name), "D1" plain.
            auto bar = r.removeFromTop (r.getHeight() * 22.0f / 64.0f);
            paintNameBar (g, bar, colourOf (ctx, t), barName (ctx, t));
            g.setColour (blockFill);
            g.fillRect (r);
            g.setColour (Dark::textDim);
            g.setFont (capFontFit (20.0f, r.getHeight() * 0.7f, WingWeight::SemiBold));
            g.drawText (numberOf (t), r.reduced (wingPx (5.0f), 0.0f), juce::Justification::centredLeft, false);
            if (highlighted)
            {
                g.setColour (Colours::gold);
                g.drawRect (bar.getUnion (r).expanded (wingPx (2.0f)), wingPx (2.0f));
            }
            return;
        }
        auto bar = r.removeFromTop (r.getHeight() * 0.36f);
        paintNameBar (g, bar, colourOf (ctx, t), barName (ctx, t));
        g.setColour (blockFill);
        g.fillRect (r);
        auto inner = r.reduced (3.0f, 1.0f);
        if (f.send)
        {
            // A send level: the source with an "S > B4" mark (fig. 59).
            auto* to = state::findStrip (ctx.engine, f.to);
            const auto mark = "S>" + (to != nullptr ? shortStripLabel (*to) : juce::String ("?"));
            g.setColour (Colours::solo);
            g.setFont (viewFont (TypeScale::caption, inner.getHeight() * 0.6f));
            g.drawFittedText (mark, inner.removeFromRight (inner.getWidth() * 0.5f).toNearestInt(), juce::Justification::centredRight, 1, 0.6f);
        }
        g.setColour (Dark::text);
        g.setFont (viewFont (TypeScale::value * 1.2f, inner.getHeight() * 0.85f));
        g.drawFittedText (numberOf (t), inner.toNearestInt(), juce::Justification::centredLeft, 1, 0.7f);
        if (! f.send && t.strip != nullptr && inner.getWidth() > inner.getHeight() * 2.5f)
            drawStripIcon (g, *t.strip, inner.removeFromRight (inner.getHeight()), Dark::textDim);
        if (highlighted)
        {
            g.setColour (Colours::yellow);
            g.drawRect (bar.getUnion (r).expanded (1.0f), 2.0f);
        }
    };

    // The source grid.
    const auto items = sourceItems();
    for (size_t i = 0; i < items.size() && i < sourceTiles.size(); ++i)
    {
        const bool sel = std::find (selectedItems.begin(), selectedItems.end(), items[i]) != selectedItems.end();
        tile (sourceTiles[i].toFloat(), items[i], sel);
    }

    // The layer's slots, in groups of four framed in blue; a blank slot (or
    // one past the end) shows its number large and dim over EMPTY.
    const auto layer = currentLayer();
    const auto faders = pageFaders();
    // [WING] each group of four sits on a #204870 plate, the slots' numbers
    // on a bar of the same blue under it.
    for (auto& p : plates)
    {
        g.setColour (slotPlate);
        g.fillRect (p);
        g.fillRect (p.withY (p.getBottom() + px (4.0f)).withHeight (px (16.0f)));
    }
    for (size_t i = 0; i < slotAreas.size(); ++i)
    {
        auto r = slotAreas[i].toFloat();
        g.setColour (Dark::well);
        g.fillRect (r);
        const int number = page * pageSize() + (int) i + 1;
        if (i < faders.size() && ! faders[i].isEmpty() && ! (dragActive && dragSlot == (int) i))
            tile (r, faders[i], false);
        else
        {
            auto text = r.reduced (2.0f);
            g.setColour (Dark::textDim.withAlpha (0.55f));
            g.setFont (viewFont (TypeScale::value * 1.6f, text.getHeight() * 0.5f));
            g.drawFittedText (juce::String (number), text.removeFromTop (text.getHeight() * 0.62f).toNearestInt(), juce::Justification::centredBottom, 1);
            g.setFont (viewFont (TypeScale::caption, text.getHeight() * 0.8f));
            g.drawFittedText ("EMPTY", text.toNearestInt(), juce::Justification::centredTop, 1);
        }
        g.setColour (juce::Colours::black);
        g.setFont (capFont (9.0f, WingWeight::SemiBold));
        g.drawText (juce::String (number), juce::Rectangle<int> (slotAreas[i].getX(), slotAreas[i].getBottom() + px (8.0f), slotAreas[i].getWidth(), px (16.0f)),
                    juce::Justification::centred);
    }

    // While dragging: a frame on the slot the items would land in.
    if (dragActive)
        if (const int local = slotAt (dragPoint) - page * pageSize(); juce::isPositiveAndBelow (local, (int) slotAreas.size()))
        {
            g.setColour (Dark::layer.brighter (0.3f));
            g.drawRect (slotAreas[(size_t) local].expanded (2).toFloat(), 3.0f);
        }

    if (! layer.user && ctx.show.layers.hasDefaultOverride (section(), layer.id) && faders.empty() && page == 0)
    {
        g.setColour (Dark::text);
        g.setFont (wingFont (TypeScale::body));
        g.drawFittedText ("EDITED LAYER KEY: RESTORE GIVES IT ITS OWN FADERS BACK", hintArea.withTrimmedBottom (px (20.0f)).reduced (8, 0),
                          juce::Justification::centred, 1);
    }

    // The item being dragged.
    if (dragActive)
    {
        const int count = dragSlot >= 0 ? 1 : juce::jmax (1, (int) selectedItems.size());
        auto ghost = juce::Rectangle<float> (70.0f, 34.0f).withCentre (dragPoint.toFloat());
        g.setColour (Dark::layer.withAlpha (0.8f));
        g.fillRect (ghost);
        g.setColour (juce::Colours::white);
        g.setFont (wingFont (TypeScale::body, WingWeight::SemiBold));
        g.drawText (count > 1 ? juce::String (count) + " ITEMS" : juce::String ("1 ITEM"), ghost, juce::Justification::centred);
    }
}

int ConfigViewPage::slotAt (juce::Point<int> pos) const
{
    // The whole slot (and the gap around it) is its target: slots never shift.
    for (size_t i = 0; i < slotAreas.size(); ++i)
        if (slotAreas[i].expanded (3, 6).contains (pos))
            return page * pageSize() + (int) i;
    return -1;
}

void ConfigViewPage::editSlots (const std::function<void (std::vector<state::FaderRef>&)>& edit)
{
    const auto layer = currentLayer();
    auto faders = layer.faders;
    edit (faders);
    layerslots::trim (faders);
    if (faders != layer.faders)
        setLayerFaders (layer, faders);
}

void ConfigViewPage::placeAt (const std::vector<state::FaderRef>& items, int index)
{
    if (! items.empty() && index >= 0)
        editSlots ([&] (auto& faders) { layerslots::place (faders, items, index); });
}

void ConfigViewPage::clearSlot (int index)
{
    editSlots ([index] (auto& faders) { layerslots::clear (faders, index); });
}

void ConfigViewPage::showSlotMenu (int local)
{
    if (! juce::isPositiveAndBelow (local, (int) slotAreas.size()))
        return;
    const int index = page * pageSize() + local;
    juce::Component::SafePointer<ConfigViewPage> safe (this);
    popups::chooseFrom (anchorAt (slotAreas[(size_t) local]), { "CLEAR SLOT" }, -1, [safe, index] (int)
    {
        if (safe != nullptr)
            safe->clearSlot (index);
    });
}

void ConfigViewPage::mouseDown (const juce::MouseEvent& e)
{
    dragActive = false;
    dragSlot = -1;
    dragPoint = e.getPosition();
    // A filled slot can be dragged (a blank one has nothing to move).
    const auto faders = pageFaders();
    for (size_t i = 0; i < slotAreas.size() && i < faders.size(); ++i)
        if (slotAreas[i].contains (e.getPosition()) && ! faders[i].isEmpty())
            dragSlot = (int) i;
}

void ConfigViewPage::mouseDrag (const juce::MouseEvent& e)
{
    if (e.getDistanceFromDragStart() < 6)
        return;
    if (! dragActive)
    {
        // Start dragging the tile under the pointer (with the other selected tiles).
        if (dragSlot < 0)
        {
            const auto items = sourceItems();
            bool onTile = false;
            for (size_t i = 0; i < items.size() && i < sourceTiles.size(); ++i)
                if (sourceTiles[i].contains (e.getMouseDownPosition()))
                {
                    onTile = true;
                    if (std::find (selectedItems.begin(), selectedItems.end(), items[i]) == selectedItems.end())
                        selectedItems.push_back (items[i]);
                }
            if (! onTile)
                return;
        }
        dragActive = true;
    }
    dragPoint = e.getPosition();
    repaint();
}

void ConfigViewPage::mouseUp (const juce::MouseEvent& e)
{
    const auto pos = e.getPosition();
    if (dragActive)
    {
        dragActive = false;
        const int target = slotAt (pos);
        if (dragSlot >= 0)
        {
            // A slot dragged onto another swaps with it (moves into a blank
            // one); dragged off the slots, it turns blank.
            const int from = page * pageSize() + dragSlot;
            if (target >= 0)
                editSlots ([from, target] (auto& faders) { layerslots::swap (faders, from, target); });
            else if (! slotsArea.contains (pos))
                clearSlot (from);
        }
        else if (target >= 0)
        {
            placeAt (selectedItems, target);
            selectedItems.clear();
        }
        dragSlot = -1;
        repaint();
        return;
    }

    for (size_t i = 0; i < categoryKeys.size(); ++i)
        if (categoryKeys[i].contains (pos))
        {
            category = (int) i;
            selectedItems.clear();
            layoutContent();
            repaint();
            return;
        }

    // A right-click or a long press on a filled slot: CLEAR SLOT.
    if (dragSlot >= 0 && (e.mods.isPopupMenu() || e.getLengthOfMousePress() >= kLongPressMs))
    {
        showSlotMenu (dragSlot);
        dragSlot = -1;
        return;
    }

    // A click on a tile toggles it; a click on a slot puts the selected tiles there.
    const auto items = sourceItems();
    for (size_t i = 0; i < items.size() && i < sourceTiles.size(); ++i)
        if (sourceTiles[i].contains (pos))
        {
            auto it = std::find (selectedItems.begin(), selectedItems.end(), items[i]);
            if (it != selectedItems.end())
                selectedItems.erase (it);
            else
                selectedItems.push_back (items[i]);
            repaint();
            return;
        }
    if (const int target = slotAt (pos); target >= 0 && ! selectedItems.empty())
    {
        placeAt (selectedItems, target);
        selectedItems.clear();
        repaint();
        return;
    }
    dragSlot = -1;
}

//==============================================================================
MuteGroupsViewPage::MuteGroupsViewPage (UiContext& c) : ViewPage (c, Screen::MuteGroups)
{
    assign = std::make_unique<MemberAssignView> (ctx, MemberAssignView::Kind::MuteGroup);
    addChildComponent (*assign);
}

void MuteGroupsViewPage::renameGroup (int gi)
{
    // [WING] MUTE GROUP n: the name field over the keyboard; the name changes
    // as it's typed, both CLOSE keys keep it.
    popups::TextOptions options;
    options.actionText = "CLOSE";
    options.capsLock = true;
    options.centreX = 428.0f / 1280.0f; // over the columns, as on the console
    juce::Component::SafePointer<MuteGroupsViewPage> safe (this);
    options.onChange = [safe, gi] (const juce::String& text)
    {
        if (safe == nullptr)
            return;
        safe->ctx.engine.setMuteGroupName (gi, text.trim());
        safe->repaint();
    };
    popups::editText (*this, "MUTE GROUP " + juce::String (gi + 1), ctx.engine.getMuteGroupName (gi), options.onChange, options);
}

MuteGroupsViewPage::~MuteGroupsViewPage() = default;

void MuteGroupsViewPage::layoutContent()
{
    // [WING] eight columns 106 apart from x = 9, the group keys in two
    // columns of four on the right.
    for (int gi = 0; gi < kNumMuteGroups; ++gi)
    {
        columns[gi] = wingRect (9.0f + 106.0f * (float) gi, 0.0f, 96.0f, 672.0f);
        headers[gi] = columns[gi].withHeight (px (64.0f));
        groupKeys[gi] = wingRect (856.0f + 212.0f * (float) (gi / 4), 72.0f + 152.0f * (float) (gi % 4), 204.0f, 144.0f);
    }
    assign->setBounds (getLocalBounds());
}

void MuteGroupsViewPage::paintContent (juce::Graphics& g)
{
    const uint32_t engaged = ctx.engine.muteGroups.load();
    for (int gi = 0; gi < kNumMuteGroups; ++gi)
    {
        const uint32_t bit = 1u << (uint32_t) gi;
        const bool on = (engaged & bit) != 0;
        auto col = columns[gi];
        // The name on the red bar, the number, then the members (the column
        // red while the group is engaged).
        const auto bar = col.removeFromTop (px (22.0f));
        g.setColour (groupRed);
        g.fillRect (bar);
        g.setColour (juce::Colours::black);
        g.setFont (capFont (13.0f, WingWeight::SemiBold));
        g.drawFittedText (ctx.engine.getMuteGroupName (gi).toUpperCase(), bar.reduced (px (2.0f), 0), juce::Justification::centred, 1, 0.6f);
        const auto number = col.removeFromTop (px (42.0f));
        g.setColour (Colours::screenTile);
        g.fillRect (number);
        g.setColour (Dark::textDim);
        g.setFont (capFont (20.0f, WingWeight::SemiBold));
        g.drawText (juce::String (gi + 1), number, juce::Justification::centred);
        col.removeFromTop (px (8.0f));
        paintMemberColumn (g, ctx, col, on ? engagedColumn : Colours::screenPanel,
                           [bit] (const Strip& s) { return (s.params.muteGroupMask.load() & bit) != 0; }, "M PH", "M SPK");

        // SOURCES: how many sources the group mutes (ROUTING > SOURCES > TAGS).
        int sources = 0;
        for (int i = 0; i < ctx.engine.getNumSources(); ++i)
            if (const auto* src = ctx.engine.getSource (i); src != nullptr && (src->muteGroups.load() & bit) != 0)
                ++sources;
        auto at = [&col] (float y, float x, float w, float h) { return juce::Rectangle<int> (col.getX() + px (x), col.getY() + px (y), px (x + w) - px (x), px (y + h) - px (y)); };
        g.setColour (keyText);
        g.setFont (capFont (10.0f));
        g.drawText ("SOURCES", at (555.0f, 0.0f, 96.0f, 22.0f), juce::Justification::centred);
        const auto box = at (576.0f, 5.0f, 86.0f, 20.0f);
        g.setColour (Colours::screenDark);
        g.fillRoundedRectangle (box.toFloat(), wingPx (2.0f));
        g.setColour (sources > 0 ? Dark::text : cellText);
        g.drawText (juce::String (sources), box, juce::Justification::centred);
    }

    // The group keys: the number large and dim, the key lit red while the group is engaged.
    for (int gi = 0; gi < kNumMuteGroups; ++gi)
    {
        auto r = groupKeys[gi];
        g.setColour (Colours::screenPanel);
        g.fillRect (r);
        g.setColour (Colours::screenLine);
        g.setFont (capFont (28.0f, WingWeight::SemiBold));
        g.drawText (juce::String (gi + 1), r.withHeight (px (60.0f)), juce::Justification::centred);
        const bool on = (engaged >> (uint32_t) gi) & 1u;
        auto key = juce::Rectangle<int> (r.getX() + px (42.0f), r.getY() + px (60.0f), px (120.0f), px (64.0f));
        g.setColour (juce::Colours::black);
        g.fillRect (key);
        key = key.reduced (px (3.0f));
        g.setColour (on ? groupRed : Colours::screenTile);
        g.fillRect (key);
        g.setColour (on ? juce::Colours::black : Dark::textDim);
        g.setFont (capFont (14.0f));
        g.drawFittedText (ctx.engine.getMuteGroupName (gi).toUpperCase(), key.reduced (px (4.0f), 0), juce::Justification::centred, 1, 0.6f);
    }
}

void MuteGroupsViewPage::mouseUp (const juce::MouseEvent& e)
{
    const auto pos = e.getPosition();
    for (int gi = 0; gi < kNumMuteGroups; ++gi)
    {
        if (groupKeys[gi].contains (pos))
        {
            ctx.engine.muteGroups = ctx.engine.muteGroups.load() ^ (1u << (uint32_t) gi);
            repaint();
            return;
        }
        // The header (MGRP.n over the number) renames the group, the rest of
        // the column opens the group's assign screen.
        if (headers[gi].contains (pos))
        {
            renameGroup (gi);
            return;
        }
        if (columns[gi].contains (pos))
        {
            assign->show (gi);
            return;
        }
    }
}

} // namespace livedaw::ui
