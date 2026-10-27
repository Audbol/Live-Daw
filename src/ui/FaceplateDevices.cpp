#include "ui/FaceplateDesign.h"
#include "ui/ModelParams.h"

#include <cstring>
#include <functional>
#include <map>
#include <set>
#include <string>
#include <utility>

/* The front panels of the FX devices, the PSE/LA combo and the channel
   strips. Most are measured from the manual's figure of the console's page
   (chapter 11: a figure is the panel area of the faceplate frame); the
   channel strips, VSS3 and SPEAKER MANAGER's other pages, which no figure
   shows, are laid out from their parameter lists (captions and value text as
   the console prints them). Same frame as FaceplateDesigns.cpp: 1156 x 479 px. */

namespace livedaw::ui::fp
{

namespace
{
    using J = juce::Justification;
    using R = juce::Rectangle<float>;

    /** A unit's look: panel, print, knobs, keys and value readouts. */
    struct Kit
    {
        juce::uint32 panel = 0xff1e1f22;
        Texture texture = Texture::Grunge;
        float wear = 0.6f;
        juce::uint32 ink = 0xffe6e6e6;    // panel print
        juce::uint32 accent = 0xfff0a030; // logo print, lit selector keys
        Look knob = Look::BlackSkirt;
        juce::uint32 cap = 0xff2a6ad8;    // knob colour (coloured knob looks)
        float maxKnob = 84.0f;
        Look key = Look::LedKey;          // switches
        juce::uint32 lit = 0xffff3020;
        Look valueFace = Look::Sans;      // value readouts
        juce::uint32 valueInk = 0;        // 0: the print colour
        juce::uint32 valueBg = 0;         // a window behind each readout (0: none)
        juce::uint32 line = 0x66ffffff;   // section outlines
        bool screws = true;
    };

    /** Builds one unit's panel from a model's parameters (by id). */
    class Unit
    {
    public:
        Unit (models::Table t, const char* token, const Kit& k) : kit (k), spec (&models::model (t, models::findModel (t, token)))
        {
            jassert (spec->token == token);
            d.panel = juce::Colour (k.panel);
            d.texture = k.texture;
            d.wear = k.wear;
            d.screws = k.screws;
            d.plate = { 30.0f, 56.0f, 1096.0f, 368.0f };
        }

        /** A parameter's index (-1 for nullptr). */
        int p (const char* id) const
        {
            if (id == nullptr)
                return -1;
            const int i = spec->find (id);
            jassert (i >= 0);
            return i;
        }

        std::vector<int> ids (std::initializer_list<const char*> list) const
        {
            std::vector<int> v;
            for (auto* id : list)
                v.push_back (p (id));
            return v;
        }

        /** The parameters whose ids start with `prefix` (or, with an empty prefix, those
            matching none of `others`), in table order. */
        std::vector<int> withPrefix (const juce::String& prefix, const juce::StringArray& others = {}) const
        {
            std::vector<int> v;
            for (size_t i = 0; i < spec->params.size(); ++i)
            {
                const auto& id = spec->params[i].id;
                bool other = false;
                for (auto& o : others)
                    other = other || id.startsWith (o);
                if (prefix.isNotEmpty() ? id.startsWith (prefix) : ! other)
                    v.push_back ((int) i);
            }
            return v;
        }

        /** The logo or model print. */
        Element& print (const juce::String& s, R r, float size, Look face = Look::SansLight, juce::uint32 colour = 0, J j = J::centredLeft)
        {
            return d.text (s, r.getX(), r.getY(), r.getWidth(), r.getHeight(), size, colour != 0 ? colour : kit.ink, face, j);
        }

        /** An outlined section with its title. */
        void section (R r, const juce::String& title, int page = -1)
        {
            const auto mark = d.elements.size();
            d.box (r.getX(), r.getY(), r.getWidth(), r.getHeight(), Look::Line, kit.line, 0, 1.5f);
            if (title.isNotEmpty())
            {
                const float w = juce::jmin (r.getWidth() - 20.0f, 14.0f + 8.5f * (float) title.length());
                d.box (r.getX() + 12.0f, r.getY() - 9.0f, w, 18.0f, Look::Fill, kit.panel);
                d.text (title, r.getX() + 12.0f, r.getY() - 9.0f, w, 18.0f, 13.0f, kit.ink);
            }
            onPage (mark, page);
        }

        /** Controls for `params` in a grid filling `r` (`cols` 0: one row). */
        void controls (const std::vector<int>& params, R r, int cols = 0, int page = -1, float knobScale = 1.0f)
        {
            const int n = (int) params.size();
            if (n == 0)
                return;
            cols = cols > 0 ? cols : n;
            const int rows = (n + cols - 1) / cols;
            const float w = r.getWidth() / (float) cols, h = r.getHeight() / (float) rows;
            for (int i = 0; i < n; ++i)
            {
                const int row = i / cols, col = i % cols;
                const int inRow = row == rows - 1 ? n - row * cols : cols;
                const float offset = (float) (cols - inRow) * w * 0.5f;
                control (params[(size_t) i], { r.getX() + offset + w * (float) col, r.getY() + h * (float) row, w, h }, page, knobScale);
            }
        }

        void controls (std::initializer_list<const char*> list, R r, int cols = 0, int page = -1, float knobScale = 1.0f)
        {
            controls (ids (list), r, cols, page, knobScale);
        }

        /** Vertical faders with their captions over and readouts under (graphic EQs, levers). */
        void faders (const std::vector<int>& params, R r, juce::uint32 cap, bool readouts = true, int page = -1)
        {
            const auto mark = d.elements.size();
            const float w = r.getWidth() / (float) juce::jmax (1, (int) params.size());
            const float captionSize = juce::jlimit (9.0f, 13.0f, w * 0.3f);
            for (size_t i = 0; i < params.size(); ++i)
            {
                const auto& prm = spec->params[(size_t) params[i]];
                const float cx = r.getX() + w * ((float) i + 0.5f);
                d.text (consoleLabel (prm).toUpperCase(), cx - w * 0.5f - 6.0f, r.getY(), w + 12.0f, captionSize * 1.3f, captionSize, kit.ink);
                auto slot = R (cx - juce::jmin (12.0f, w * 0.3f), r.getY() + captionSize * 1.4f + 4.0f, juce::jmin (24.0f, w * 0.6f),
                               r.getHeight() - captionSize * 1.4f - (readouts ? 30.0f : 8.0f));
                for (int k = 0; k <= 10; ++k)
                {
                    const float y = slot.getY() + 8.0f + (slot.getHeight() - 16.0f) * (float) k / 10.0f;
                    const float tick = k == 0 || k == 5 || k == 10 ? 7.0f : 4.0f;
                    d.box (cx - slot.getWidth() * 0.5f - tick - 2.0f, y, tick, 0, Look::Line, kit.ink, 0, 1.0f);
                }
                d.fader (params[i], slot.getX(), slot.getY(), slot.getWidth(), slot.getHeight(), cap);
                if (readouts)
                    value (params[i], R (cx - w * 0.5f, slot.getBottom() + 4.0f, w, 20.0f), juce::jlimit (10.0f, 13.0f, w * 0.26f));
            }
            onPage (mark, page);
        }

        void faders (std::initializer_list<const char*> list, R r, juce::uint32 cap, bool readouts = true, int page = -1)
        {
            faders (ids (list), r, cap, readouts, page);
        }

        Element& graph (GraphKind kind, R r, std::vector<int> params = {}, int variant = -1, int page = -1)
        {
            auto& e = d.add (Type::Graph, r.getX(), r.getY(), r.getWidth(), r.getHeight());
            e.shows (kind, std::move (params), variant);
            e.page = page;
            return e;
        }

        Element& graph (GraphKind kind, R r, std::initializer_list<const char*> list, int variant = -1, int page = -1)
        {
            return graph (kind, r, ids (list), variant, page);
        }

        /** Keys that show the panel's pages. */
        void tabs (const juce::StringArray& names, float x, float y, float w = 104.0f, float h = 28.0f, juce::uint32 colour = 0xffcfcfcf)
        {
            for (int i = 0; i < names.size(); ++i)
            {
                auto& k = d.key (-1, x + (w + 8.0f) * (float) i, y, w, h, Look::TabKey, names[i]);
                k.choice = i;
                k.colour2 = juce::Colour (colour);
                k.size = 14.0f;
            }
        }

        /** A gain-reduction meter. */
        void meter (R r, Look face = Look::VuAmberDark)
        {
            d.vu (r.getX(), r.getY(), r.getWidth(), r.getHeight(), face, "20|10|7|5|3|1|0", { 20, 10, 7, 5, 3, 1, 0 }, "GAIN REDUCTION").reverse = true;
        }

        Element& value (int param, R r, float size)
        {
            auto& v = d.add (Type::Value, r.getX(), r.getY(), r.getWidth(), r.getHeight());
            v.param = param;
            v.look = kit.valueFace;
            v.size = size;
            v.colour = juce::Colour (kit.valueInk != 0 ? kit.valueInk : kit.ink);
            v.colour2 = juce::Colour (kit.valueBg);
            v.just = J::centred;
            return v;
        }

        Design take() { return std::move (d); }

        Kit kit;
        const models::Model* spec;
        Design d;

    private:
        void onPage (size_t mark, int page)
        {
            for (size_t i = mark; i < d.elements.size(); ++i)
                d.elements[i].page = page;
        }

        void control (int param, R cell, int page, float knobScale)
        {
            if (param < 0)
                return;
            const auto mark = d.elements.size();
            const auto& prm = spec->params[(size_t) param];
            const float capSize = juce::jlimit (11.0f, 15.0f, cell.getWidth() * 0.13f);
            const float capH = capSize * 1.35f;
            const float cx = cell.getCentreX();
            // A short choice list is a column of keys when it fits; longer ones turn a knob.
            const float keysH = (cell.getHeight() - capH - 8.0f) / (float) juce::jmax (1, prm.choices.size()) - 4.0f;
            const bool fitsColumn = prm.kind == models::ParamKind::Choice && prm.choices.size() <= 6 && keysH >= 16.0f;
            const bool isKnob = prm.kind != models::ParamKind::Toggle && ! fitsColumn;
            const float readout = 20.0f;
            const float dia = juce::jmin (cell.getWidth() * 0.56f, (cell.getHeight() - capH - readout - 12.0f) * 0.8f, kit.maxKnob) * knobScale;
            if (isKnob)
            {
                // Caption, knob and readout as a group, centred in a tall cell.
                const float group = capH + 4.0f + dia * 1.26f + readout;
                cell = cell.withTrimmedTop (juce::jmax (0.0f, (cell.getHeight() - group) * 0.5f));
            }
            d.text (consoleLabel (prm).toUpperCase(), cell.getX() - 4.0f, cell.getY(), cell.getWidth() + 8.0f, capH, capSize, kit.ink);
            auto body = cell.withTrimmedTop (capH + 2.0f).reduced (0.0f, 2.0f);
            if (prm.kind == models::ParamKind::Toggle)
            {
                const float s = juce::jmin (cell.getWidth() * 0.34f, body.getHeight() * 0.5f, 40.0f);
                d.key (param, cx - s * 0.5f, body.getCentreY() - s * 0.5f - 6.0f, s, s, kit.key, "", kit.lit);
            }
            else if (fitsColumn)
            {
                const int n = prm.choices.size();
                const float h = juce::jmin (30.0f, (body.getHeight() - 4.0f) / (float) n - 4.0f);
                const float w = juce::jmin (cell.getWidth() * 0.78f, 110.0f);
                const float top = body.getCentreY() - ((h + 4.0f) * (float) n - 4.0f) * 0.5f;
                for (int i = 0; i < n; ++i)
                    d.key (param, cx - w * 0.5f, top + (h + 4.0f) * (float) i, w, h, Look::FlatKey, prm.choices[i].toUpperCase(), kit.accent)
                        .sel (i)
                        .size = juce::jmin (13.0f, h * 0.6f);
            }
            else
            {
                const float cy = body.getY() + dia * 0.63f;
                auto& k = d.knob (param, cx, cy, dia, kit.knob, kit.cap).scaleOf (11, "", 280).radii (1.16f, 1.5f).scol (kit.ink);
                k.dots = true;
                value (param, R (cell.getX() + 2.0f, cy + dia * 0.63f + 2.0f, cell.getWidth() - 4.0f, readout),
                       juce::jlimit (11.0f, 14.0f, cell.getWidth() * 0.12f));
            }
            onPage (mark, page);
        }
    };

    //==========================================================================
    // Looks

    Kit lightRack (juce::uint32 panel)
    {
        Kit k;
        k.panel = panel;
        k.texture = Texture::Brushed;
        k.ink = 0xff1a1a1a;
        k.line = 0x80303030;
        k.knob = Look::BlackSkirt;
        return k;
    }

    //==========================================================================
    // Units measured from the manual's figures (chapter 11): a figure is the
    // panel area of the console's faceplate frame, so frame = figure + (30, 70).

    constexpr juce::uint32 kPaper = 0xffeeeeee;

    Design figurePanel (juce::uint32 panel, Texture texture, float wear, bool screws = false)
    {
        Design d;
        d.panel = juce::Colour (panel);
        d.texture = texture;
        d.wear = wear;
        d.screws = screws;
        d.plate = { 30.0f, 70.0f, 1096.0f, 336.0f };
        return d;
    }

    /** Print centred on (cx, cy). */
    Element& label (Design& d, const juce::String& s, float cx, float cy, float size, juce::uint32 colour, Look face = Look::Sans)
    {
        return d.text (s, cx - 160.0f, cy - size * 0.7f, 320.0f, size * 1.4f, size, colour, face);
    }

    /** The ends of a knob's scale printed under it (and a unit between them). */
    void ends (Design& d, float cx, float y, float dx, const juce::String& lo, const juce::String& hi, float size, juce::uint32 colour, const char* unit = nullptr)
    {
        label (d, lo, cx - dx, y, size, colour);
        label (d, hi, cx + dx, y, size, colour);
        if (unit != nullptr)
            label (d, unit, cx, y + size * 0.3f, size * 0.85f, colour);
    }

    void screwsAt (Design& d, std::initializer_list<std::pair<float, float>> at, float dia = 14.0f)
    {
        for (auto [x, y] : at)
            d.add (Type::Screw, x, y, dia, dia);
    }

    /** A tab key of a paged panel. */
    void pageTab (Design& d, int page, float x, float y, float w, float h, const char* name, juce::uint32 colour = 0xffb8c4d8)
    {
        auto& k = d.key (-1, x, y, w, h, Look::TabKey, name);
        k.choice = page;
        k.colour2 = juce::Colour (colour);
        k.size = 13;
    }

    /** A parameter's value on a 7-segment display (digits right-aligned). */
    Element& segment (Design& d, int param, R r, float size, juce::uint32 colour, juce::uint32 bg = 0x00000000)
    {
        auto& v = d.add (Type::Value, r.getX(), r.getY(), r.getWidth(), r.getHeight());
        v.param = param;
        v.look = Look::Segment;
        v.size = size;
        v.colour = juce::Colour (colour);
        v.colour2 = juce::Colour (bg);
        v.just = J::centredRight;
        return v;
    }

    /** A 7-segment display showing the control last touched (`first` before any is). */
    Element& readout (Design& d, int first, R r, float size, juce::uint32 colour, juce::uint32 bg = 0x00000000)
    {
        auto& v = segment (d, -1, r, size, colour, bg);
        v.params = { first };
        return v;
    }

    /** A ladder of LEDs: one per value (top first), lit for the band around it;
        clicking one sets its value. `labels` print beside it (left or right). */
    void ladder (Design& d, int param, float x, float y0, float step, std::vector<float> values, const juce::StringArray& labels, bool labelsLeft,
                 juce::uint32 lit, juce::uint32 ink, float size = 12.0f, float labelW = 46.0f)
    {
        for (size_t i = 0; i < values.size(); ++i)
        {
            const float y = y0 + step * (float) i;
            // The band: halfway to the neighbours; the ends reach past the range.
            const bool falling = values.size() > 1 && values.front() > values.back();
            const size_t n = values.size();
            const bool hasLower = falling ? i + 1 < n : i > 0, hasUpper = falling ? i > 0 : i + 1 < n;
            const float lowerN = hasLower ? values[falling ? i + 1 : i - 1] : 0.0f, upperN = hasUpper ? values[falling ? i - 1 : i + 1] : 0.0f;
            const float lo = hasLower ? (lowerN + values[i]) * 0.5f : -1.0e9f, hi = hasUpper ? (upperN + values[i]) * 0.5f : 1.0e9f;
            d.lamp (x, y, 9.0f, lit, param).lights (lo, hi, values[i]);
            if (i < (size_t) labels.size())
                d.text (labels[(int) i], labelsLeft ? x - 10.0f - labelW : x + 10.0f, y - size * 0.7f, labelW, size * 1.4f, size, ink, Look::Sans,
                        labelsLeft ? J::centredRight : J::centredLeft);
        }
    }

    //--------------------------------------------------------------------------
    // Premium reverbs

    /** GATED and REVERSE REVERB (Fig. 130, 131): the WING digital reverb, a black
        unit with an LCD, the value display and bar knobs. */
    Design wingDigitalReverb (const char* token, const char* lcd, std::vector<std::tuple<const char*, float, const char*, const char*, const char*>> row)
    {
        const auto& spec = models::model (models::Fx, models::findModel (models::Fx, token));
        const auto p = [&spec] (const char* id) { return spec.find (id); };
        auto d = figurePanel (0xff1b1b1c, Texture::Grunge, 0.9f);
        screwsAt (d, { { 46, 85 }, { 1108, 85 }, { 46, 390 }, { 1108, 390 } });
        const juce::uint32 cream = 0xffe4dcc0, scale = 0xffd8cca0;
        // The display window.
        d.box (85, 95, 585, 145, Look::Fill, 0xff070707, 0xff2c2c2c, 2.0f);
        d.text ("WING", 96, 148, 100, 30, 30, 0xffa89c82, Look::Sans, J::centredLeft);
        d.text ("DIGITAL REVERB", 97, 178, 110, 14, 11, 0xff9a9a9a, Look::Sans, J::centredLeft);
        const char* meter[] = { "CLIP", "0", "-3", "-6", "-10", "-15", "-20", "-30" };
        for (int i = 0; i < 8; ++i)
        {
            const float y = 135.0f + 10.0f * (float) i;
            d.box (186, y - 2.5f, 14, 5, Look::Fill, i < 3 ? 0xff555555 : (i == 3 ? 0xff8a1a14 : 0xff28c03c));
            d.text (meter[i], 204, y - 5, 30, 10, 9, 0xffb0b0b0, Look::Sans, J::centredLeft);
        }
        d.box (245, 152, 227, 31, Look::Fill, 0xffa2c83c, 0xff5a7020, 1.5f);
        d.text (lcd, 245, 152, 227, 31, 21, 0xff1c2a08, Look::Dot).kerning (0.08f);
        d.box (476, 128, 194, 76, Look::Fill, 0xff140606);
        readout (d, p ("dcy"), { 478, 130, 190, 72 }, 64, 0xffff4848).unitSide (2, 17); // the knob selected: DECAY at first
        auto knob = [&] (const char* id, float cx, float cy, float dia, const char* caption, float capY, const char* lo, const char* hi, float endsY)
        {
            d.knob (p (id), cx, cy, dia, Look::BarKnob).scaleOf (11, "", 300, false).radii (1.12f, 1.5f).scol (scale);
            label (d, caption, cx, capY, 15, cream);
            ends (d, cx, endsY, dia * 0.38f, lo, hi, 12, 0xffeeeeee);
        };
        knob ("lc", 735, 180, 70, "LO CUT", 112, "20", "400", 228);
        d.box (812, 85, 0, 145, Look::Line, 0xffb0a070, 0, 1.5f);
        d.path ("M 893 120 L 893 112 L 1053 112 L 1053 120", 0xffb0a070, 1.5f);
        d.box (920, 104, 104, 16, Look::Fill, 0xff1b1b1c);
        label (d, "HIGH SHELV", 972, 112, 14, cream);
        knob ("hsf", 888, 188, 60, "FREQ", 130, "200", "20k", 228);
        knob ("hsg", 1050, 188, 60, "GAIN", 130, "-30", "0", 228);
        d.box (698, 248, 402, 0, Look::Line, 0xffb0a070, 0, 1.5f);
        for (auto [id, cx, caption, lo, hi] : row)
            knob (id, cx, 335, 76, caption, 272, lo, hi, 390);
        return d;
    }

    Design gatedReverb()
    {
        return wingDigitalReverb ("GATED", "GATED REVERB",
                                  { { "pdel", 170.0f, "PRE DELAY", "0", "200" },
                                    { "dcy", 333.0f, "DECAY", "0.14", "1" },
                                    { "att", 490.0f, "ATTACK", "0", "30" },
                                    { "dens", 652.0f, "DENSITY", "1", "50" },
                                    { "sprd", 815.0f, "SPREAD", "0", "100" },
                                    { "diff", 970.0f, "DIFFUSE", "1", "30" } });
    }

    Design reverseReverb()
    {
        return wingDigitalReverb ("REVERSE", "REVERSE REVERB",
                                  { { "pdel", 170.0f, "PRE DELAY", "0", "200" },
                                    { "dcy", 333.0f, "DECAY", "0.14", "1" },
                                    { "rise", 490.0f, "RISE", "0", "50" },
                                    { "sprd", 815.0f, "SPREAD", "0", "100" },
                                    { "diff", 970.0f, "DIFFUSE", "1", "30" } });
    }

    /** DELAY/REVERB (Fig. 132): "Dual FX", two displays over blue knob strips.
        The keys turn FEEDBACK (HiCut) and the last knob (Size, PreDel, Damp,
        LoCut) to another parameter, as on the hardware. */
    Design delayReverb()
    {
        const auto& spec = models::model (models::Fx, models::findModel (models::Fx, "DEL/REV"));
        const auto p = [&spec] (const char* id) { return spec.find (id); };
        auto d = figurePanel (0xff19191b, Texture::Brushed, 0.7f);
        screwsAt (d, { { 40, 82 }, { 1110, 82 }, { 40, 392 }, { 1110, 392 } });
        const juce::uint32 blue = 0xff1f50e8, cyan = 0xff40e0f0;
        d.path ("M 30 315 L 500 315 L 500 406 L 30 406 Z", blue, 0.0f);
        d.path ("M 580 315 L 905 315 L 905 70 L 1065 70 L 1065 406 L 580 406 Z", blue, 0.0f);
        d.text ("Dual", 62, 112, 120, 44, 36, kPaper, Look::SansLight, J::centredLeft);
        d.box (95, 154, 82, 30, Look::Fill, blue);
        d.text ("FX", 95, 154, 82, 30, 26, kPaper, Look::SansLight);
        for (auto [x, param, name] : { std::tuple { 190.0f, p ("time"), "TIME" }, { 572.0f, p ("pdel"), "PRE DLY" } })
        {
            d.box (x, 105, 316, 100, Look::Fill, 0xff060808, 0xff2a2a2a, 2.0f);
            segment (d, param, { x + 10, 116, 168, 80 }, 74, cyan, 0xff060808);
            d.text (name, x + 182, 128, 130, 22, 19, cyan, Look::Dot, J::centredLeft);
            d.text ("MS", x + 182, 158, 130, 22, 19, cyan, Look::Dot, J::centredLeft);
        }
        label (d, "DELAY", 432, 235, 20, kPaper).kerning (0.25f);
        label (d, "REVERB", 650, 235, 20, kPaper).kerning (0.25f);
        label (d, "Input", 733, 330, 13, kPaper, Look::SerifItalic);
        auto tab = [&] (int page, float x, float y, const char* name)
        {
            auto& k = d.key (-1, x, y, 54, 28, Look::TabKey, name);
            k.choice = page;
            k.colour2 = juce::Colour (0xff3a3a3c);
            k.size = 14;
        };
        tab (1, 918, 102, "Size");
        tab (2, 995, 102, "PreDel");
        tab (3, 918, 182, "Damp");
        tab (4, 995, 182, "LoCut");
        tab (5, 278, 220, "HiCut");
        auto knob = [&] (int param, float cx, const char* caption, int page)
        {
            d.knob (param, cx, 315, 74, Look::BlackSkirt).page = page;
            d.text (caption, cx - 70, 355, 140, 22, 16, 0xffe8eefc, Look::SerifItalic).page = page;
        };
        knob (p ("time"), 175, "Time", -1);
        knob (p ("dly"), 428, "Level", -1);
        knob (p ("d2r"), 645, "Delay", -1);
        knob (p ("i2r"), 810, "Direct", -1);
        const std::pair<const char*, const char*> last[] = { { "dcy", "Decay" }, { "size", "Size" }, { "pdel", "PreDel" },
                                                             { "damp", "Damp" }, { "rlc", "LoCut" }, { "dcy", "Decay" } };
        for (int page = 0; page < 6; ++page)
        {
            knob (page == 5 ? p ("fhc") : p ("feed"), 300, page == 5 ? "HiCut" : "Feedback", page);
            knob (p (last[page].first), 980, last[page].second, page);
        }
        return d;
    }

    /** SHIMMER REVERB (Fig. 133): three pedals - the shimmer pedal, the blue
        reverb with its display, and the twin filter. */
    Design shimmerReverb()
    {
        const auto& spec = models::model (models::Fx, models::findModel (models::Fx, "SHIMMER"));
        const auto p = [&spec] (const char* id) { return spec.find (id); };
        auto d = figurePanel (0xff5a321c, Texture::Smooth, 0.0f);
        d.box (30, 70, 1096, 336, Look::Wood, 0);
        const juce::uint32 ink = 0xff141414;
        // Cables between the pedals.
        d.path ("M 275 110 C 300 110 300 150 350 150", 0xff1a1a1a, 6.0f);
        d.path ("M 802 200 C 850 200 840 280 922 280", 0xff1a1a1a, 5.0f);
        // Shimmer pedal.
        d.box (30, 70, 245, 336, Look::Fill, 0xff4a1248, 0xff2a0a28, 3.0f);
        d.box (30, 220, 245, 186, Look::Fill, 0x40ff3090);
        for (auto [id, cx, caption] : { std::tuple { "shim", 102.0f, "SHIMMER" }, { "shine", 213.0f, "SHINE" } })
        {
            d.knob (p (id), cx, 142, 62, Look::BlackSkirt);
            ends (d, cx, 188, 20, "min", "max", 11, kPaper);
            label (d, caption, cx, 210, 12, kPaper);
        }
        d.lamp (160, 238, 14, 0xfff0e030).at (1.0f);
        label (d, "Shimmer", 155, 272, 36, kPaper, Look::SerifItalic);
        d.knob (-1, 158, 312, 44, Look::Chrome).at (0.5f).sweep = 0.0f;
        label (d, "BYPASS", 158, 346, 10, kPaper).kerning (0.2f);
        label (d, "TRANSFORM OUT", 268, 330, 10, kPaper).centre = -90.0f;
        // Reverb pedal.
        d.box (350, 70, 452, 336, Look::Fill, 0xff1c2868, 0xff4a5cb0, 3.0f);
        d.box (400, 118, 120, 46, Look::Fill, 0xff0a0606, 0xff2a2a2a, 1.5f);
        readout (d, p ("pdel"), { 402, 120, 116, 42 }, 38, 0xffff4040);
        d.knob (p ("pdel"), 648, 142, 54, Look::BlackSkirt);
        d.text ("PRE DELAY", 684, 134, 120, 16, 12, kPaper, Look::Sans, J::centredLeft);
        for (auto [id, cx, caption] : { std::tuple { "dcy", 432.0f, "DECAY" }, { "size", 575.0f, "SIZE" }, { "damp", 720.0f, "DAMPING" } })
        {
            d.knob (p (id), cx, 282, 66, Look::BlackSkirt);
            label (d, caption, cx, 334, 13, kPaper);
        }
        label (d, "WING  REVERB", 680, 372, 13, 0xffe0d040).kerning (0.45f);
        // Twin filter.
        d.box (922, 70, 204, 336, Look::Fill, 0xffa890e8, 0xff6a50a8, 3.0f);
        d.box (930, 78, 188, 294, Look::Line, ink, 0, 1.5f);
        d.knob (p ("lc"), 978, 140, 60, Look::BlackSkirt);
        d.knob (p ("hc"), 1060, 140, 60, Look::BlackSkirt);
        label (d, "LO CUT", 978, 104, 14, ink);
        label (d, "HI CUT", 1060, 104, 14, ink);
        ends (d, 978, 182, 22, "25", "250", 11, ink, "Hz");
        ends (d, 1060, 182, 22, "500", "7k", 11, ink, "Hz");
        label (d, "filter", 1030, 250, 44, ink, Look::Script);
        d.knob (-1, 1025, 315, 42, Look::Chrome).at (0.5f).sweep = 0.0f;
        label (d, "RESET", 1025, 348, 10, ink);
        d.box (930, 375, 188, 27, Look::Line, ink, 0, 2.0f);
        label (d, "TWIN FILTER", 1024, 389, 17, ink).kerning (0.1f);
        return d;
    }

    /** SPRING REVERB (Fig. 134): a white panel with the tank's window, wooden cheeks. */
    Design springReverb()
    {
        auto d = figurePanel (0xffe6e8e8, Texture::Brushed, 0.5f);
        d.box (30, 70, 56, 336, Look::Wood, 0);
        d.box (1070, 70, 56, 336, Look::Wood, 0);
        d.box (770, 102, 202, 24, Look::Line, 0xff9a9a9a, 0, 1.0f);
        d.box (770, 102, 16, 24, Look::Fill, 0xff8a8a8a);
        d.text ("SPRING REVERB", 792, 102, 178, 24, 15, 0xff8a8a8a, Look::Serif).kerning (0.2f);
        // The tank's window: springs glowing green.
        d.box (185, 140, 789, 114, Look::Fill, 0xff2a1a3a);
        d.box (189, 144, 781, 106, Look::Fill, 0xff04201a);
        d.box (420, 160, 300, 70, Look::Fill, 0x3020e060);
        d.box (520, 170, 140, 50, Look::Fill, 0x4020f070);
        for (int row = 0; row < 2; ++row)
            for (float x = 205.0f; x < 960.0f; x += 19.0f)
            {
                const float y = 152.0f + 48.0f * (float) row;
                d.path ("M " + juce::String (x) + " " + juce::String (y) + " C " + juce::String (x - 12) + " " + juce::String (y + 12) + " "
                            + juce::String (x - 12) + " " + juce::String (y + 30) + " " + juce::String (x) + " " + juce::String (y + 42),
                        0x9030c050, 1.2f);
            }
        screwsAt (d, { { 163, 198 }, { 993, 198 } }, 13);
        const juce::uint32 ink = 0xff262626;
        const auto& spec = models::model (models::Fx, models::findModel (models::Fx, "SPRING"));
        auto knob = [&] (const char* id, float cx, const char* caption, float captionRight, const char* lo, const char* unit, const char* hi)
        {
            d.knob (-1, cx, 332, 70, Look::Chrome).at (0.5f).sweep = 0.0f;
            d.knob (spec.find (id), cx, 332, 58, Look::WhiteDome).scaleOf (3, "", 280).radii (1.18f, 1.4f).scol (ink);
            d.text (caption, captionRight - 120, 322, 120, 22, 16, ink, Look::Sans, J::centredRight);
            ends (d, cx, 378, 22, lo, hi, 12, ink, unit);
        };
        knob ("dcy", 260, "DECAY", 198, "1.5", "s", "6");
        knob ("dens", 495, "DENSITY", 438, "min", nullptr, "max");
        knob ("low", 732, "BASS", 665, "min", nullptr, "max");
        knob ("high", 967, "TREBLE", 917, "min", nullptr, "max");
        return d;
    }

    /** VINTAGE ROOM (Fig. 126): meters, value tiles round the data wheel. The
        tiles are the controls (drag one); ADD and FREEZE keys. */
    Design vintageRoom()
    {
        const auto& spec = models::model (models::Fx, models::findModel (models::Fx, "V-ROOM"));
        const auto p = [&spec] (const char* id) { return spec.find (id); };
        auto d = figurePanel (0xff1d1f23, Texture::Brushed, 0.6f);
        screwsAt (d, { { 55, 92 }, { 1098, 92 }, { 55, 372 }, { 1098, 372 } });
        const juce::uint32 red = 0xffff3428, grey = 0xffd8d8d8;
        // Meters.
        d.box (78, 100, 192, 275, Look::Fill, 0xff16181c, 0xffa8a8a8, 2.0f);
        d.text ("INPUTS", 92, 112, 70, 16, 12, kPaper);
        d.text ("dB", 164, 113, 22, 14, 9, kPaper);
        d.text ("OUTPUTS", 186, 112, 76, 16, 12, kPaper);
        d.box (95, 132, 160, 210, Look::Fill, 0xff0a0a0a);
        const char* scale[] = { "+12", "+6", "0", "-12", "-24", "-36", "-48", "-60", "-72" };
        for (int i = 0; i < 9; ++i)
            d.text (scale[i], 160, 139.0f + 23.4f * (float) i, 30, 12, 9, kPaper);
        for (float x : { 110.0f, 138.0f, 207.0f, 235.0f })
            for (int k = 0; k < 21; ++k)
            {
                const float y = 145.0f + 9.5f * (float) k;
                const bool on = k >= (x < 180.0f ? 5 : 6);
                d.box (x - 10, y - 3, 20, 6, Look::Fill, on ? 0xff50e040 : 0xff2a2c2a);
            }
        for (float x : { 110.0f, 138.0f, 207.0f, 235.0f })
            d.text (x < 125.0f || (x > 200.0f && x < 220.0f) ? "L" : "R", x - 10, 345, 20, 14, 11, kPaper);
        // Tiles: value window, unit, caption bar.
        auto tile = [&] (const char* id, float x, float y, const char* caption, bool unitRight)
        {
            d.box (x, y, 182, 64, Look::Fill, 0xff2a2c31, 0xff4a4d52, 1.5f);
            d.box (x + 40, y + 7, 134, 34, Look::Fill, 0xff180808);
            segment (d, p (id), unitRight ? R (x + 40, y + 7, 140, 34) : R (x + 6, y + 7, 168, 34), 32, red).unitSide (unitRight ? 2 : 1, 12);
            d.box (x + 6, y + 44, 170, 16, Look::Fill, 0xff3a3d42);
            d.text (caption, x + 6, y + 44, 170, 16, 13, grey);
        };
        for (auto [y0, y1] : { std::pair { 98.0f, 238.0f }, { 240.0f, 380.0f } })
            for (float x : { 282.0f, 692.0f, 886.0f })
                d.box (x, y0, 194, y1 - y0, Look::Line, 0xff3a3d42, 0, 1.5f);
        tile ("pdel", 288, 102, "REV DELAY", false);
        tile ("dcy", 288, 170, "DECAY", false);
        tile ("size", 288, 242, "ROOM SIZE", false);
        tile ("dens", 288, 310, "DENSITY", false);
        tile ("lmult", 698, 102, "LOW MULTIPLY", true);
        tile ("hmult", 698, 170, "HIGH MULTIPLY", true);
        tile ("erl", 698, 242, "ER DELAY L", true);
        tile ("err", 698, 310, "ER DELAY R", true);
        tile ("lc", 892, 102, "LOW CUT", true);
        tile ("hc", 892, 170, "HIGH CUT", true);
        tile ("erlvl", 892, 242, "ER LEVEL", true);
        tile ("lvl", 892, 310, "LEVEL", true);
        // Centre: name, data wheel, keys.
        d.box (480, 98, 208, 282, Look::Fill, 0xff26282c);
        d.text ("VINTAGE", 480, 96, 208, 26, 24, 0xffd0d0d0, Look::SansLight).kerning (0.12f);
        d.text ("ROOM", 480, 118, 208, 26, 24, 0xffd0d0d0, Look::SansLight).kerning (0.12f);
        label (d, "-", 534, 168, 20, kPaper);
        label (d, "+", 634, 168, 20, kPaper);
        d.knob (-1, 585, 235, 110, Look::ChromeSmall).at (0.5f);
        for (auto [id, x, name] : { std::tuple { "add", 502.0f, "ADD" }, { "frz", 600.0f, "FREEZE" } })
        {
            d.key (p (id), x, 303, 76, 40, Look::StackKey, name).colour2 = juce::Colour (grey);
            d.lamp (x + 6, 306, 5, red, p (id));
        }
        return d;
    }

    /** VINTAGE REVERB (Fig. 127): the levers and LED scales of the original.
        Levers: PREDELAY, LO CUT, HI CUT, MODULATE; the red LEDs pick DECAY,
        LO MULTI and HI MULTI; the green ones show the lever's value. */
    Design vintageReverb()
    {
        const auto& spec = models::model (models::Fx, models::findModel (models::Fx, "V-REV"));
        const auto p = [&spec] (const char* id) { return spec.find (id); };
        auto d = figurePanel (0xff1c1c1e, Texture::Brushed, 0.6f);
        const juce::uint32 red = 0xffff2a20, green = 0xff30e040, ink = 0xffeeeeee;
        // The red side cheek with its grip.
        d.box (30, 70, 124, 336, Look::Fill, 0xffb4161a);
        d.box (58, 70, 70, 336, Look::Fill, 0xff2c2c2e, 0xff111111, 2.0f);
        for (int r = 0; r < 26; ++r)
            for (int c = 0; c < 4; ++c)
                d.box (66.0f + 15.0f * (float) c + (r % 2) * 7.0f, 84.0f + 12.4f * (float) r, 4, 4, Look::Fill, 0xff151515);
        label (d, "VINTAGE REVERB", 600, 103, 24, 0xffc8c8c8).kerning (0.12f);
        d.lamp (1048, 110, 12, red).at (1.0f);
        // Decay LEDs (pick), the predelay lever and its LEDs.
        ladder (d, p ("dcy"), 270, 110, 16, { 4.5f, 4.0f, 3.6f, 3.3f, 3.0f, 2.8f, 2.5f, 2.2f, 2.0f, 1.8f, 1.5f, 1.2f, 1.0f, 0.8f, 0.6f, 0.4f },
                { "4.5", "4.0", "3.6", "3.3", "3.0", "2.8", "2.5", "2.2", "2.0", "1.8", "1.5", "1.2", "1.0", "0.8", "0.6", "0.4" }, true, red, ink, 11, 34);
        d.box (300, 100, 30, 256, Look::Fill, 0xff0e0e0e);
        d.fader (p ("pdel"), 300, 100, 30, 256, green);
        ladder (d, p ("pdel"), 350, 132, 16.2f, { 120, 110, 100, 90, 80, 70, 60, 50, 40, 30, 20, 10, 0 },
                { "120", "110", "100", "90", "80", "70", "60", "50", "40", "30", "20", "10", "0" }, false, green, ink, 11, 34);
        // Lo / hi multiplier and cut: LEDs either side of a lever.
        auto pair = [&] (float x, int multi, std::vector<float> mv, juce::StringArray ml, int cut, std::vector<float> cv, juce::StringArray cl)
        {
            ladder (d, multi, x - 40, 205, 32.7f, mv, ml, true, red, ink, 12, 46);
            d.box (x - 15, 145, 30, 212, Look::Fill, 0xff0e0e0e);
            d.fader (cut, x - 15, 145, 30, 212, 0xffd82020);
            ladder (d, cut, x + 40, 205, 32.7f, cv, cl, false, green, ink, 12, 52);
            for (float lx : { x - 40, x + 40 })
                d.box (lx, 314, 0, 46, Look::Line, 0xffd0d0d0, 0, 1.2f);
        };
        pair (502, p ("lmult"), { 2.0f, 1.5f, 1.0f, 0.5f }, { "X2.0", "X1.5", "X1.0", "X0.5" }, p ("lc"), { 200, 100, 50, 20 },
              { "200 Hz", "100 Hz", "50 Hz", "20 kHz" });
        pair (692, p ("hmult"), { 0.67f, 0.5f, 0.33f, 0.25f }, { "max", "X0.5", "X0.33", "X0.25" }, p ("hc"), { 20000, 15000, 10000, 5000 },
              { "20 kHz", "15 kHz", "10 kHz", "5 kHz" });
        ladder (d, p ("mod"), 845, 205, 32.7f, { 100, 70, 50, 25 }, { "100%", "70%", "50%", "25%" }, true, red, ink, 12, 46);
        d.box (872, 145, 30, 212, Look::Fill, 0xff0e0e0e);
        d.fader (p ("mod"), 872, 145, 30, 212, 0xffd8d8d8);
        // Output and transformer keys.
        d.key (p ("out"), 952, 158, 62, 62, Look::LitSquare, "FRONT", green).sel (0).size = 14;
        d.key (p ("out"), 1018, 158, 62, 62, Look::LitSquare, "BACK", green).sel (1).size = 14;
        label (d, "Output", 1050, 246, 13, ink);
        d.key (p ("trans"), 952, 288, 62, 62, Look::LitSquare, "OFF", green).sel (0).size = 14;
        d.key (p ("trans"), 1018, 288, 62, 62, Look::LitSquare, "ON", green).sel (1).size = 14;
        const std::pair<const char*, float> captions[] = { { "Decay", 255 },  { "Predelay", 350 }, { "Lo Multi", 462 }, { "Lo Cut", 542 },
                                                           { "Hi Multi", 652 }, { "Hi Cut", 732 },  { "Modulate", 845 }, { "Transformer", 1050 } };
        for (auto [s, x] : captions)
            label (d, s, x, 374, 13, ink);
        return d;
    }

    //--------------------------------------------------------------------------
    // Premium modulation, delays and pitch

    /** The index of a parameter of an FX model. */
    std::function<int (const char*)> fxParams (const char* token)
    {
        const auto* spec = &models::model (models::Fx, models::findModel (models::Fx, token));
        return [spec] (const char* id)
        {
            const int i = spec->find (id);
            jassert (i >= 0);
            return i;
        };
    }

    /** DIMENSION CRS (Fig. 140): the rack unit with its four push buttons. */
    Design dimension()
    {
        const auto p = fxParams ("DIMCRS");
        auto d = figurePanel (0xff1e1f23, Texture::Rough, 0.7f);
        screwsAt (d, { { 45, 115 }, { 45, 340 }, { 1112, 115 }, { 1112, 340 } }, 12);
        const juce::uint32 grey = 0xffd0d0d0;
        auto key = [&] (int param, float x, const char* led, juce::uint32 ledColour, const char* caption)
        {
            d.lamp (x + 19, 146, 10, ledColour, param).at (1.0f);
            d.text (led, x + 31, 138, 80, 16, 13, grey, Look::Sans, J::centredLeft);
            d.key (param, x, 167, 85, 38, Look::StackKey);
            label (d, caption, x + 42, 232, 15, grey);
        };
        key (p ("in"), 108, "STEREO", 0xfff0e030, "MODE");
        key (p ("drysw"), 412, "DRY", 0xffff3020, "MIX");
        key (-1, 963, "ON", 0xffff3020, "POWER");
        for (int i = 0; i < 9; ++i)
            d.box (108, 254.0f + 11.5f * (float) i, 85, 7, Look::Fill, 0xff3a3b3e, 0xff111111, 1.0f);
        label (d, "OUTPUT", 285, 108, 14, grey);
        label (d, "dB", 285, 128, 12, grey);
        d.box (275, 152, 20, 210, Look::Fill, 0xff0c0c0c);
        for (int k = 0; k < 20; ++k)
            d.box (279, 156.0f + 10.2f * (float) k, 12, 6, Look::Fill, k >= 5 ? 0xffe02020 : 0xff3a1414);
        const char* scale[] = { "+12", "+6", "0", "-12", "-24", "-36", "-48", "-60", "-72" };
        for (int i = 0; i < 9; ++i)
            d.text (scale[i], 304, 138.0f + 26.5f * (float) i, 34, 14, 11, grey);
        // The push buttons: OFF (lit while none is in) and 1 to 4.
        label (d, "OFF", 620, 141, 14, grey);
        d.key (-1, 590, 170, 60, 38, Look::LitSquare, "", 0xffd82020).at (1.0f);
        for (int i = 0; i < 4; ++i)
        {
            const float x = 652.0f + 61.0f * (float) i;
            label (d, juce::String (i + 1), x + 30, 141, 14, grey);
            d.key (p (("sw" + std::to_string (i + 1)).c_str()), x, 170, 58, 38, Look::LitSquare, "", 0xfff4f2e2);
        }
        label (d, "DIMENSION", 918, 322, 20, 0xffb4b4b4).kerning (0.45f);
        d.box (780, 336, 275, 0, Look::Line, 0xff8a8a8a, 0, 1.2f);
        label (d, "CHORUS", 918, 346, 13, 0xffb4b4b4).kerning (0.6f);
        return d;
    }

    /** The twin filter pedal of the premium pedalboards (FLANGER, SHIMMER, CHORUS). */
    void twinFilter (Design& d, float x, int lo, int hi, const char* loHi, const char* hiLo, const char* hiHi)
    {
        const juce::uint32 ink = 0xff141414;
        d.box (x, 70, 204, 336, Look::Fill, 0xffa890e8, 0xff6a50a8, 3.0f);
        d.box (x + 8, 78, 188, 294, Look::Line, ink, 0, 1.5f);
        d.knob (lo, x + 56, 145, 60, Look::BlackSkirt);
        d.knob (hi, x + 146, 145, 60, Look::BlackSkirt);
        label (d, "LO CUT", x + 56, 110, 14, ink);
        label (d, "HI CUT", x + 146, 110, 14, ink);
        ends (d, x + 56, 188, 22, "20", loHi, 11, ink, "Hz");
        ends (d, x + 146, 188, 22, hiLo, hiHi, 11, ink, "Hz");
        label (d, "filter", x + 104, 255, 44, ink, Look::Script);
        d.knob (-1, x + 103, 320, 42, Look::Chrome).at (0.5f).sweep = 0.0f;
        label (d, "RESET", x + 103, 352, 10, ink);
        d.box (x + 8, 375, 188, 27, Look::Line, ink, 0, 2.0f);
        label (d, "TWIN FILTER", x + 102, 389, 17, ink).kerning (0.1f);
    }

    /** STEREO FLANGER (Fig. 142): feedback pedal, the flanger and the twin filter. */
    Design stereoFlanger()
    {
        const auto p = fxParams ("FLANGER");
        auto d = figurePanel (0xff5a321c, Texture::Smooth, 0.0f);
        d.box (30, 70, 1096, 336, Look::Wood, 0);
        const juce::uint32 ink = 0xff141414;
        // Feedback pedal.
        d.box (60, 70, 232, 336, Look::Fill, 0xff2e9a6a, 0xff1a6a48, 3.0f);
        for (int i = 0; i < 4; ++i)
            d.box (66, 344.0f + 6.0f * (float) i, 220, 0, Look::Line, 0x80103020, 0, 1.0f);
        d.knob (p ("flc"), 135, 160, 66, Look::BlackSkirt);
        d.knob (p ("fhc"), 235, 160, 66, Look::BlackSkirt);
        label (d, "FEED LC", 135, 112, 15, ink);
        label (d, "FEED HC", 235, 112, 15, ink);
        ends (d, 135, 208, 26, "20", "400", 12, ink, "Hz");
        ends (d, 235, 208, 26, "200", "20k", 12, ink, "Hz");
        label (d, "FEEDBACK", 190, 233, 15, ink);
        d.knob (p ("feed"), 190, 282, 66, Look::BlackSkirt);
        ends (d, 190, 324, 30, "-90", "90", 12, ink, "%");
        d.knob (-1, 190, 365, 34, Look::Chrome).at (0.5f).sweep = 0.0f;
        label (d, "RESET", 240, 368, 9, 0xff1a4a30).kerning (0.3f);
        for (float y : { 172.0f, 287.0f })
            d.box (296, y - 12, 54, 24, Look::Fill, 0xffc8a040, 0xff7a6020, 1.5f);
        // Flanger pedal.
        d.box (362, 70, 458, 336, Look::Fill, 0xffe8b020, 0xffa07810, 3.0f);
        d.box (408, 90, 392, 120, Look::Line, ink, 0, 1.5f);
        d.box (408, 265, 392, 130, Look::Line, ink, 0, 1.5f);
        auto white = [&] (const char* id, float cx, float cy, const char* lo, const char* unit, const char* hi, const char* caption)
        {
            d.knob (p (id), cx, cy, 64, Look::WhiteDome);
            ends (d, cx, cy + 40, 24, lo, hi, 11, ink, unit);
            label (d, caption, cx, cy + 56, 13, ink);
        };
        white ("dlyl", 460, 142, "0.5", "ms", "20", "DELAY L");
        white ("depl", 581, 142, "0", "%", "100", "DEPTH L");
        white ("spd", 702, 142, "0.05", "Hz", "5", "SPEED");
        white ("dlyr", 460, 315, "0.5", "ms", "20", "DELAY R");
        white ("depr", 581, 315, "0", "%", "100", "DEPTH R");
        white ("phase", 702, 315, "0", "", "180", "PHASE");
        d.knob (p ("mix"), 517, 240, 60, Look::BlackSkirt);
        label (d, "DRY", 471, 254, 12, ink);
        label (d, "WET", 563, 254, 12, ink);
        label (d, "STEREO", 722, 225, 10, ink).kerning (0.35f);
        label (d, "Flanger", 680, 244, 40, ink, Look::Script);
        label (d, "FEED IN", 380, 170, 9, ink).centre = -90.0f;
        label (d, "OUT", 380, 370, 9, ink).centre = -90.0f;
        twinFilter (d, 855, p ("lc"), p ("hc"), "400", "200", "20k");
        return d;
    }

    /** ULTRATAP DELAY (Fig. 136): stereo, filter and delay sections, the taps display. */
    Design ultratap()
    {
        const auto p = fxParams ("TAP-DL");
        auto d = figurePanel (0xff26272a, Texture::Rough, 0.8f);
        const juce::uint32 ink = 0xffeeeeee, line = 0xff5a5c60, title = 0xffb8b8b8;
        auto section = [&] (float x, float y, float w, float h, const char* name)
        {
            d.box (x, y, w, h, Look::Line, line, 0, 2.0f);
            const float tw = 14.0f + 9.0f * (float) std::strlen (name);
            d.box (x + w * 0.5f - tw * 0.5f, y - 9, tw, 18, Look::Fill, 0xff26272a);
            label (d, name, x + w * 0.5f, y, 14, title);
        };
        section (168, 88, 210, 130, "STEREO");
        section (385, 88, 215, 130, "FILTER");
        section (168, 232, 432, 163, "DELAY");
        section (615, 88, 475, 307, "TRIM / BALANCE");
        // Logo.
        d.box (82, 155, 26, 66, Look::Fill, 0xffe8e020);
        label (d, "TAP", 95, 188, 16, 0xff1a1a1a).centre = -90.0f;
        const char* ultra = "ULTRA";
        for (int i = 0; i < 5; ++i)
            label (d, juce::String::charToString ((juce::juce_wchar) ultra[i]), 95, 340.0f - 22.0f * (float) i, 16, 0xffe8e020).centre = -90.0f;
        d.box (45, 220, 100, 23, Look::Line, ink, 0, 1.5f);
        label (d, "DELAY", 95, 231, 16, ink).kerning (0.35f);
        auto knob = [&] (const char* id, float cx, float cy, float dia, Look look, juce::uint32 colour, const char* caption, float capY)
        {
            auto& k = d.knob (p (id), cx, cy, dia, look, colour);
            label (d, caption, cx, capY, 13, ink);
            return std::ref (k);
        };
        knob ("wid", 220, 160, 64, Look::ColourDome, 0xffff5022, "WIDTH", 108);
        ends (d, 220, 196, 26, "-100", "100", 11, ink);
        knob ("diff", 322, 160, 64, Look::ColourDome, 0xffff5022, "DIFFUSION", 108);
        ends (d, 322, 196, 26, "0", "100", 11, ink);
        knob ("lc", 442, 160, 64, Look::ColourDome, 0xfff4f060, "LO CUT", 108);
        ends (d, 442, 196, 26, "20", "400", 11, ink, "Hz");
        knob ("hc", 542, 160, 64, Look::ColourDome, 0xfff4f060, "HI CUT", 108);
        ends (d, 542, 196, 26, "200", "20k", 11, ink, "Hz");
        // Delay.
        d.text ("TIME", 180, 248, 40, 20, 13, ink);
        d.box (218, 245, 52, 21, Look::Fill, 0xff100404);
        segment (d, p ("time"), { 218, 245, 52, 21 }, 19, 0xffff3020);
        d.text ("ms", 274, 248, 24, 18, 11, ink, Look::Sans, J::centredLeft);
        knob ("time", 240, 320, 84, Look::BarKnob, 0, "", 0);
        ends (d, 240, 368, 26, "1", "2000", 11, ink);
        knob ("fact", 388, 320, 66, Look::BarKnob, 0, "FACTOR", 258).get().scaleOf (9, "1/3|1/2|2/3|3/4|1|5/4|4/3|3/2|2", 300).radii (1.12f, 1.62f).lsize (11).scol (ink);
        knob ("pdel", 525, 320, 66, Look::BarKnob, 0, "PRE DELAY", 258);
        ends (d, 525, 360, 26, "0", "500", 11, ink, "ms");
        knob ("rep", 706, 320, 78, Look::BarKnob, 0, "REPEATS", 258).get().scaleOf (16, "1|2|3|4|5|6|7|8|9|10|11|12|13|14|15|16", 315).radii (1.1f, 1.5f).lsize (10).scol (ink);
        knob ("mode", 866, 320, 64, Look::BarKnob, 0, "", 0).get().scaleOf (4, "MOVE|JUMP|FOCUS|SPREAD", 270).radii (1.1f, 1.75f).lsize (12).scol (ink);
        knob ("slp", 1015, 320, 64, Look::BarKnob, 0, "SLOPE", 258);
        ends (d, 1015, 360, 26, "-24", "24", 11, ink, "dB");
        auto& g = d.add (Type::Graph, 635, 110, 440, 120);
        g.shows (GraphKind::Taps, { p ("time"), p ("rep"), p ("slp"), p ("pdel") }, 1);
        return d;
    }

    /** TAPE DELAY (Fig. 137): the tape echo, chrome plate on a green panel, VU meters. */
    Design tapeEcho()
    {
        const auto p = fxParams ("TAPE-DL");
        auto d = figurePanel (0xff2a2826, Texture::Rough, 0.9f);
        const juce::uint32 ink = 0xffeeeeee;
        d.box (52, 92, 1053, 303, Look::Bevel, 0xffc6c8ca);
        d.text ("WING TAPE-ECHO", 116, 102, 300, 26, 21, 0xff2a2a2a, Look::Sans, J::centredLeft).kerning (0.18f);
        d.box (88, 143, 989, 234, Look::Fill, 0xff9a9c9e);
        d.box (92, 147, 981, 226, Look::Fill, 0xff1e2a1e);
        auto knob = [&] (const char* id, float cx, const char* caption, const char* lo, const char* unit, const char* hi)
        {
            d.knob (p (id), cx, 270, 92, Look::SilverDial);
            label (d, caption, cx, 200, 14, ink);
            d.lamp (cx - 28, 330, 4, ink).at (1.0f);
            d.lamp (cx + 34, 330, 4, 0xff8a8a8a).at (1.0f);
            ends (d, cx, 342, 32, lo, hi, 12, ink, unit);
        };
        knob ("time", 168, "TIME", "60", "MS", "650");
        knob ("sust", 328, "SUSTAIN", "MIN", "%", "MAX");
        knob ("drv", 818, "DRIVE", "MIN", "%", "MAX");
        knob ("wf", 978, "FLUTTER", "MIN", "%", "MAX");
        d.box (428, 165, 290, 195, Look::Fill, 0xff111213, 0xff2a2a2a, 2.0f);
        for (float x : { 432.0f, 578.0f })
            d.vu (x, 168, 136, 72, Look::VuWhite, "20|10|7|5|3|0|3", { 20, 10, 7, 5, 3, 0, -3 }, "VU");
        d.lamp (462, 302, 12, 0xffff2020).at (1.0f);
        for (float x : { 535.0f, 615.0f })
            d.knob (-1, x, 302, 62, Look::ChromeSmall).at (0.3f);
        d.toggle (-1, 685, 306, 22).at (1.0f);
        label (d, "POWER", 685, 275, 12, ink);
        return d;
    }

    /** OILCAN DELAY (Fig. 138): four chrome knobs on a grey panel in a leather frame. */
    Design oilcanDelay()
    {
        const auto p = fxParams ("OILCAN");
        auto d = figurePanel (0xff4a2c1e, Texture::Rough, 1.0f);
        d.box (80, 85, 998, 307, Look::Bevel, 0xffdadcdc);
        d.box (80, 155, 998, 145, Look::Fill, 0x22000000);
        d.box (448, 95, 292, 55, Look::Fill, 0xff7a7c7e);
        d.box (452, 99, 284, 47, Look::Fill, 0xff0c0c0c);
        label (d, "OIL", 520, 122, 40, 0xffd8d8d8, Look::SerifItalic).kerning (0.15f);
        label (d, "CAN", 664, 122, 40, 0xffd8d8d8, Look::SerifItalic).kerning (0.15f);
        d.lamp (578, 125, 12, 0xffe01818).at (1.0f);
        const juce::uint32 ink = 0xff1e1e1e;
        for (auto [id, cx, caption] : { std::tuple { "time", 230.0f, "TIME" }, { "sust", 480.0f, "SUSTAIN" }, { "wb", 685.0f, "WOBBLE" },
                                        { "tone", 935.0f, "TONE" } })
        {
            d.knob (p (id), cx, 247, 130, Look::ChromeCollar);
            label (d, caption, cx, 172, 16, ink);
            d.lamp (cx - 40, 320, 3.5f, ink).at (1.0f);
            d.lamp (cx + 40, 320, 3.5f, ink).at (1.0f);
            ends (d, cx, 336, 40, "MIN.", "MAX.", 13, ink);
        }
        d.box (905, 348, 110, 27, Look::Fill, 0xff0e0e0e, 0xff6a6a6a, 1.5f);
        label (d, "Wing - echo", 960, 361, 15, 0xffe8e8e8, Look::Script);
        return d;
    }

    /** BBD DELAY (Fig. 139): a purple pedal with its cables. */
    Design bbdDelay()
    {
        const auto p = fxParams ("BBD-DL");
        auto d = figurePanel (0xff5a321c, Texture::Smooth, 0.0f);
        d.box (30, 70, 1096, 336, Look::Wood, 0);
        for (float y : { 140.0f, 340.0f })
        {
            d.box (30, y - 11, 192, 22, Look::Fill, 0xffe8a028, 0xff8a5810, 1.5f);
            for (float x = 40.0f; x < 220.0f; x += 16.0f)
                d.box (x, y - 7, 7, 4, Look::Fill, 0xff2a1a08);
            d.box (220, y - 20, 240, 40, Look::Fill, 0xff17241c, 0xff0a120c, 2.0f);
            d.box (460, y - 12, 40, 24, Look::Bevel, 0xffb8b8b8);
        }
        d.box (500, 70, 626, 336, Look::Fill, 0xffb020e0, 0xff8010b0, 2.0f);
        d.path ("M 556 82 L 990 82 Q 1000 82 1006 92 L 1012 104 Q 1018 116 1018 128 L 1018 340 Q 1018 352 1012 364 L 1006 376 Q 1000 386 990 386 "
                "L 556 386 Q 545 386 545 375 L 545 93 Q 545 82 556 82 Z",
                0xfff0e0f8, 2.5f);
        label (d, "IN", 523, 155, 10, 0xfff0e0f8);
        label (d, "OUT", 523, 342, 10, 0xfff0e0f8);
        for (auto [id, cx, caption] : { std::tuple { "fb", 638.0f, "FEEDBACK" }, { "dly", 798.0f, "DELAY" } })
        {
            d.knob (p (id), cx, 242, 96, Look::BlackDot);
            label (d, caption, cx, 180, 16, kPaper);
            d.lamp (cx - 27, 290, 3, kPaper).at (1.0f);
            d.lamp (cx + 27, 290, 3, kPaper).at (1.0f);
            ends (d, cx, 300, 27, "MIN", "MAX", 12, kPaper);
        }
        label (d, "BBD", 896, 240, 32, 0xfff0e0f8, Look::SansLight).centre = -90.0f;
        label (d, "DELAY", 926, 240, 32, 0xfff0e0f8).centre = -90.0f;
        d.lamp (967, 240, 10, 0xffff2020).at (1.0f);
        d.knob (-1, 1020, 240, 62, Look::Chrome).at (0.5f).sweep = 0.0f;
        screwsAt (d, { { 1085, 142 }, { 1085, 345 } }, 22);
        return d;
    }

    /** STEREO PITCH (Fig. 148): the display over the knobs, HiCut / LoCut on the blue strip. */
    Design stereoPitch()
    {
        const auto p = fxParams ("PITCH");
        auto d = figurePanel (0xff1a1a1c, Texture::Brushed, 0.7f);
        screwsAt (d, { { 40, 85 }, { 1110, 85 }, { 40, 380 }, { 1110, 380 } });
        const juce::uint32 blue = 0xff1f50e8, cyan = 0xff40e0f0;
        d.box (368, 208, 215, 198, Look::Fill, blue);
        d.box (925, 70, 120, 336, Look::Fill, blue);
        d.text ("Stereo", 56, 108, 140, 40, 34, kPaper, Look::SansLight, J::centredLeft);
        d.box (80, 158, 100, 32, Look::Fill, blue);
        d.text ("Pitch", 80, 158, 100, 32, 28, kPaper, Look::SansLight);
        d.box (195, 110, 687, 95, Look::Fill, 0xff090b0b, 0xff2a2a2a, 2.0f);
        for (auto [id, x, unit] : { std::tuple { "semi", 225.0f, "SEMI" }, { "cent", 440.0f, "CENTS" }, { "dly", 655.0f, "MS" } })
        {
            segment (d, p (id), { x, 118, 138, 80 }, 74, cyan);
            d.text (unit, x + 142, 160, 80, 22, 18, cyan, Look::Dot, J::centredLeft);
        }
        auto italic = [&] (const char* s, float cx, float cy, float size) { label (d, s, cx, cy, size, kPaper, Look::SerifItalic); };
        d.knob (p ("mix"), 177, 320, 60, Look::BlackSkirt).scaleOf (9, "", 280).radii (1.3f, 1.5f).scol (kPaper);
        italic ("Mix", 175, 255, 15);
        italic ("0", 152, 362, 12);
        italic ("100", 200, 362, 12);
        d.knob (p ("semi"), 365, 318, 78, Look::BlackSkirt);
        d.knob (p ("cent"), 583, 318, 78, Look::BlackSkirt);
        d.knob (p ("dly"), 798, 318, 78, Look::BlackSkirt);
        italic ("Pitch", 476, 318, 22);
        italic ("Delay", 690, 318, 22);
        d.knob (p ("hc"), 985, 160, 58, Look::BlackSkirt).scaleOf (9, "", 280).radii (1.3f, 1.5f).scol (kPaper);
        italic ("HiCut", 985, 95, 15);
        italic ("200", 963, 200, 12);
        italic ("20k", 1007, 200, 12);
        d.knob (p ("lc"), 985, 320, 58, Look::BlackSkirt).scaleOf (9, "", 280).radii (1.3f, 1.5f).scol (kPaper);
        italic ("LoCut", 985, 255, 15);
        italic ("20", 963, 360, 12);
        italic ("400", 1007, 360, 12);
        return d;
    }

    /** DUAL PITCH (Fig. 149): sections A and B with dot-matrix captions, the
        filters and a meter between them. */
    Design dualPitch()
    {
        const auto p = fxParams ("D-PITCH");
        auto d = figurePanel (0xffc6c8ca, Texture::Brushed, 0.8f);
        screwsAt (d, { { 50, 90 }, { 1102, 90 }, { 50, 380 }, { 1102, 380 } });
        const juce::uint32 red = 0xffff3020, blue = 0xff3448d0, ink = 0xff2a2a2a;
        label (d, "DUAL", 96, 200, 36, blue, Look::SerifItalic).centre = -90.0f;
        label (d, "P I T C H", 122, 160, 13, blue).centre = -90.0f;
        d.lamp (105, 294, 9, 0xfff0e030).at (1.0f);
        d.box (70, 315, 72, 18, Look::Fill, 0xff1a1a1a, 0xff050505, 1.5f);
        label (d, "POWER", 105, 345, 11, 0xff5a5a5a);
        auto section = [&] (float dx, const char* name, const char* sfx)
        {
            d.box (178 + dx, 82, 380, 310, Look::Bevel, 0xffd4d6d8);
            d.box (178 + dx, 82, 380, 310, Look::Line, 0xff8a8a8a, 0, 2.0f);
            d.box (197 + dx, 108, 340, 32, Look::Fill, 0xff0a0606);
            d.text ("DELAY", 207 + dx, 108, 120, 32, 20, red, Look::Dot, J::centredLeft);
            d.text ("PAN", 407 + dx, 108, 120, 32, 20, red, Look::Dot, J::centredRight);
            d.box (197 + dx, 250, 340, 33, Look::Fill, 0xff0a0606);
            d.text ("SEMITONES  CENTS", 203 + dx, 250, 230, 33, 20, red, Look::Dot, J::centredLeft);
            d.text ("LEVEL", 407 + dx, 250, 120, 33, 20, red, Look::Dot, J::centredRight);
            label (d, name, 365 + dx, 190, 22, blue);
            const auto id = [sfx] (const char* base) { return std::string (base) + sfx; };
            d.knob (p (id ("dly").c_str()), 248 + dx, 180, 64, Look::ChromeSmall);
            d.knob (p (id ("pan").c_str()), 488 + dx, 180, 64, Look::ChromeSmall);
            d.knob (p (id ("semi").c_str()), 248 + dx, 328, 64, Look::ChromeSmall);
            d.knob (p (id ("cent").c_str()), 368 + dx, 328, 64, Look::ChromeSmall);
            d.knob (p (id ("lvl").c_str()), 488 + dx, 328, 64, Look::ChromeSmall);
        };
        section (0, "A", "1");
        section (528, "B", "2");
        d.knob (p ("hc"), 633, 125, 52, Look::ChromeSmall);
        label (d, "HI CUT", 633, 92, 13, ink).kerning (0.1f);
        ends (d, 633, 164, 26, "200", "20K", 11, ink, "Hz");
        d.knob (p ("lc"), 633, 215, 52, Look::ChromeSmall);
        label (d, "LO CUT", 633, 182, 13, ink).kerning (0.1f);
        ends (d, 633, 254, 26, "20", "400", 11, ink, "Hz");
        d.box (602, 268, 63, 124, Look::Fill, 0xff9a9c9e, 0xff6a6a6a, 1.5f);
        for (int r = 0; r < 6; ++r)
            for (float x : { 618.0f, 645.0f })
                d.lamp (x, 282.0f + 20.0f * (float) r, 8, r < 2 ? 0xffd02020 : (r == 2 ? 0xfff0e030 : 0xff40d040)).at (r == 2 && x > 640.0f ? 1.0f : (r > 2 ? 1.0f : 0.0f));
        return d;
    }

    /** VSS3 REVERB (Fig. 125): the TC ICON remote. The home page has six
        faders (DECAY, LO DAMP, HI DCY, HI CUT, REV LVL, ER LVL) and the preset
        wheel; the arrow opens the layers MAIN, ER, TAIL and MOD. */
    Design vss3()
    {
        const auto p = fxParams ("VSS3");
        auto d = figurePanel (0xffc4c6c8, Texture::Brushed, 1.0f);
        const juce::uint32 ink = 0xff111111;
        d.text ("tc electronic", 85, 76, 170, 30, 25, ink, Look::Sans, J::centredLeft);
        d.box (244, 80, 0, 26, Look::Line, ink, 0, 1.5f);
        d.text ("TC ICON", 254, 78, 100, 14, 12, ink, Look::Sans, J::centredLeft);
        d.text ("REMOTE SYSTEM", 254, 92, 120, 14, 12, ink, Look::Sans, J::centredLeft);
        const auto faders = [&] (std::vector<int> params, float x0, float step, int page)
        {
            const auto mark = d.elements.size();
            for (size_t i = 0; i < params.size(); ++i)
            {
                const float cx = x0 + step * (float) i;
                for (int k = 0; k <= 20; ++k)
                {
                    const float y = 152.0f + 10.0f * (float) k;
                    d.box (cx - 24, y, 48, 0, Look::Line, ink, 0, k % 2 == 0 ? 1.4f : 0.8f);
                }
                d.box (cx - 4, 136, 8, 232, Look::Fill, 0xff0a0a0a);
                d.fader (params[i], cx - 19, 136, 38, 232, 0xffe8e8e8);
                const auto& spec = models::model (models::Fx, models::findModel (models::Fx, "VSS3"));
                label (d, spec.params[(size_t) params[i]].label, cx, 386, step < 80.0f ? 10.0f : 12.0f, ink);
            }
            for (auto [x, j] : { std::pair { x0 - 34, J::centredRight }, { x0 + step * (float) (params.size() - 1) + 34, J::centredLeft } })
                for (int k = 0; k <= 10; ++k)
                    d.text (juce::String (100 - 10 * k), j == J::centredRight ? x - 30 : x, 145.0f + 20.0f * (float) k, 30, 14, 13, ink, Look::Sans, j);
            for (size_t i = mark; i < d.elements.size(); ++i)
                d.elements[i].page = page;
        };
        // Home: six faders, the preset wheel, Load.
        faders ({ p ("dcy"), p ("lsdmp"), p ("hdcy"), p ("hcut"), p ("rvlvl"), p ("erlvl") }, 140, 107, 0);
        const auto mark = d.elements.size();
        d.knob (-1, 915, 258, 280, Look::SilverDial).at (0.5f).sweep = 0.0f;
        d.box (790, 133, 250, 250, Look::Fill, 0x00000000);
        auto& wheel = d.add (Type::Value, 800, 150, 230, 216);
        wheel.param = p ("preset");
        wheel.look = Look::Wheel;
        wheel.size = 17;
        wheel.colour = juce::Colour (0xffe8e8e8);
        wheel.colour2 = juce::Colour (0xff1e1e20);
        wheel.band = juce::Colour (0xfff0a020);
        d.path ("M 770 248 L 792 260 L 770 272 Z", 0xfff0a020, 0.0f);
        d.key (p ("load"), 1048, 356, 62, 34, Look::LitSquare, "Load", 0xffe8c060).size = 16;
        pageTab (d, 1, 1040, 80, 76, 30, "LAYERS", 0xffd8d8d8);
        auto& view = d.key (p ("view"), 1040, 116, 76, 22, Look::StackKey, "VIEW");
        view.colour2 = juce::Colour (0xfff0f0f0);
        view.size = 11;
        for (size_t i = mark; i < d.elements.size(); ++i)
            d.elements[i].page = 0;
        // Layers.
        const std::pair<const char*, std::vector<int>> layers[] = {
            { "MAIN", { p ("ldcy"), p ("lmdcy"), p ("hmdcy"), p ("hdcy"), p ("lxo"), p ("mxo"), p ("hxo"), p ("dcy"), p ("diff"), p ("lshv"), p ("lsdmp"),
                        p ("hsoft"), p ("hcut") } },
            { "ER", { p ("ertype"), p ("ersize"), p ("erpos"), p ("erpdly"), p ("erlc"), p ("ercol"), p ("erbal"), p ("erlvl") } },
            { "TAIL", { p ("rvtype"), p ("rvwid"), p ("rvpdly"), p ("rvbal"), p ("rvlvl") } },
            { "MOD", { p ("mtype"), p ("mrate"), p ("mwid") } }
        };
        for (int l = 0; l < 4; ++l)
        {
            const auto& ps = layers[l].second;
            const float step = juce::jmin (107.0f, 900.0f / (float) ps.size());
            faders (ps, 140, step, l + 1);
            for (int t = 0; t < 4; ++t)
            {
                pageTab (d, t + 1, 420.0f + 84.0f * (float) t, 80, 78, 26, layers[t].first, 0xffd8d8d8);
                d.elements.back().page = l + 1;
            }
            pageTab (d, 0, 1040, 80, 76, 30, "HOME", 0xffd8d8d8);
            d.elements.back().page = l + 1;
        }
        return d;
    }

    /** SPEAKER MANAGER (Fig. 168): FILTER, DYN EQ and LIMITER pages beside the response. */
    Design speakerManager()
    {
        const auto p = fxParams ("SPKMAN");
        auto d = figurePanel (0xff22344e, Texture::Grunge, 0.9f);
        screwsAt (d, { { 45, 85 }, { 1108, 85 }, { 45, 385 }, { 1108, 385 } }, 14);
        const juce::uint32 ink = 0xffe8ecf2, dim = 0xffb8c4d4;
        d.box (30, 97, 155, 0, Look::Line, 0xffd8dce4, 0, 1.0f);
        d.box (450, 97, 420, 0, Look::Line, 0xffd8dce4, 0, 1.0f);
        d.text ("SPEAKER MANAGER", 880, 84, 236, 26, 18, 0xffd8dce4, Look::SansLight, J::centredRight).kerning (0.08f);
        pageTab (d, 0, 220, 85, 92, 26, "FILTER", 0xffd0d4da);
        pageTab (d, 1, 308, 85, 92, 26, "DYN EQ", 0xffd0d4da);
        pageTab (d, 2, 404, 85, 74, 26, "LIMITER", 0xffd0d4da);
        d.box (70, 120, 556, 270, Look::Line, 0xff3a4a62, 0, 1.5f);
        auto& g = d.add (Type::Graph, 675, 125, 410, 225);
        g.shows (GraphKind::Response, {});
        int page = 0;
        const auto knob = [&] (int param, float cx, float cy, float dia, const char* caption, bool value = true)
        {
            d.knob (param, cx, cy, dia, Look::Chrome).scaleOf (21, "", 280, false).radii (1.08f, 1.3f).scol (0xffd8dce4).page = page;
            label (d, caption, cx, cy - dia * 0.5f - 22, 14, ink).page = page;
            if (value)
            {
                auto& v = d.add (Type::Value, cx - 50, cy + dia * 0.5f + 8, 100, 20);
                v.param = param;
                v.look = Look::Sans;
                v.size = 14;
                v.colour = juce::Colour (dim);
                v.colour2 = juce::Colour (0x00000000);
                v.page = page;
            }
        };
        const auto key = [&] (int param, float x, float y, float w, const char* text, int choice = -1)
        {
            auto& k = d.key (param, x, y, w, 34, Look::LitSquare, text, 0xff40d0a0);
            k.size = 12;
            k.choice = choice;
            k.page = page;
            return std::ref (k);
        };
        const auto lever = [&] (int param, float cx, float cy, const char* up, const char* down)
        {
            d.toggle (param, cx, cy, 24).page = page;
            label (d, up, cx, cy - 38, 14, ink).page = page;
            label (d, down, cx, cy + 40, 14, ink).page = page;
        };
        // FILTER: high pass, low pass, tilt; delay and phase.
        knob (p ("hpf"), 128, 175, 44, "HP Freq");
        knob (p ("hptype"), 222, 175, 44, "HP Type");
        knob (p ("lpf"), 316, 175, 44, "LP Freq");
        knob (p ("lptype"), 410, 175, 44, "LP Type");
        knob (p ("tiltf"), 504, 175, 44, "Tilt Freq");
        knob (p ("tiltg"), 584, 175, 44, "Tilt");
        knob (p ("phase"), 128, 315, 44, "Phase");
        lever (p ("invert"), 222, 315, "Invert", "Normal");
        knob (p ("dist"), 316, 315, 44, "Distance");
        knob (p ("pos"), 410, 315, 44, "Position");
        // DYN EQ (Fig. 168).
        page = 1;
        knob (p ("deqatt"), 128, 170, 44, "Attack");
        knob (p ("deqrel"), 215, 170, 44, "Release");
        lever (p ("deqmode"), 300, 172, "Above", "Below");
        knob (p ("deqg"), 390, 170, 44, "Gain");
        knob (p ("deqf"), 473, 170, 44, "Freq");
        knob (p ("deqq"), 565, 170, 44, "Q");
        label (d, "Dyn EQ", 125, 270, 14, ink).page = page;
        key (p ("dyneq"), 102, 287, 50, "On");
        knob (p ("deqthr"), 235, 302, 66, "Threshold");
        label (d, "Filter Types", 388, 247, 14, ink).page = page;
        key (p ("deqfilt"), 300, 266, 50, "LP 6dB", 2);
        key (p ("deqfilt"), 300, 308, 50, "LP 12dB", 3);
        key (p ("deqfilt"), 365, 266, 50, "Flat", 0);
        key (p ("deqfilt"), 365, 308, 50, "BP", 1);
        key (p ("deqfilt"), 430, 266, 50, "HP 6dB", 4);
        key (p ("deqfilt"), 430, 308, 50, "HP 12dB", 5);
        d.knob (p ("deqratio"), 543, 300, 56, Look::WhiteDome).scaleOf (7, "1.2|1.3|1.5|2.0|3.0|5.0|10", 300).radii (1.2f, 1.62f).lsize (11).scol (ink).page = page;
        label (d, "Ratio", 543, 247, 14, ink).page = page;
        // LIMITER.
        page = 2;
        label (d, "Limiter", 140, 172, 14, ink).page = page;
        key (p ("lim"), 115, 190, 50, "On");
        knob (p ("limthr"), 270, 210, 66, "Threshold");
        label (d, "RMS", 400, 172, 14, ink).page = page;
        key (p ("limrms"), 375, 190, 50, "On");
        knob (p ("limrel"), 520, 210, 66, "Release");
        return d;
    }

    //--------------------------------------------------------------------------
    // Standard devices

    /** GRAPHIC EQ (Fig. 81): 31 faders on a brushed blue panel, TRUE CURVE
        top right. As on the console, the faders (and TRIM) and the curve are
        on pages of their own. */
    Design graphicEq()
    {
        const auto p = fxParams ("GEQ");
        auto d = figurePanel (0xff56647e, Texture::Brushed, 0.8f);
        d.path ("M 440 70 C 482 70 472 108 522 108 L 608 108 C 658 108 648 70 690 70 Z", 0xff8c98ac, 0.0f);
        label (d, "GEQ", 565, 90, 26, 0xfff0f2f6).kerning (0.12f);
        screwsAt (d, { { 60, 112 }, { 1098, 112 }, { 60, 358 }, { 1098, 358 } });
        const juce::uint32 ink = 0xffdfe4ec;
        label (d, "TRUE CURVE", 1030, 92, 10, ink);
        label (d, "OFF", 992, 118, 10, ink);
        label (d, "ON", 1072, 118, 10, ink);
        d.toggle (p ("type"), 1032, 114, 20).sideways();
        pageTab (d, 0, 76, 78, 82, 22, "FADERS");
        pageTab (d, 1, 164, 78, 82, 22, "CURVE");
        // FADERS
        auto onFaders = d.elements.size();
        d.box (83, 127, 193, 270, Look::Fill, 0x30e8c040, 0xffe8c040, 2.0f);
        for (auto [s, y] : { std::pair { "15", 145.0f }, { "0", 238.0f }, { "-15", 330.0f } })
            label (d, s, 66, y, 11, ink);
        d.box (84, 238, 1040, 0, Look::Line, 0x40ffffff, 0, 1.0f);
        for (int i = 0; i < 32; ++i)
        {
            const float x = i < 31 ? 98.0f + 32.07f * (float) i : 1102.0f;
            const int param = i < 31 ? i + 1 : p ("TRIM");
            d.box (x - 3, 138, 6, 200, Look::Fill, 0xff0a0c10);
            d.fader (param, x - 12, 131, 24, 214, i < 31 ? 0xffc4c8cc : 0xffe8c040);
            const auto name = i < 31 ? models::model (models::Fx, models::findModel (models::Fx, "GEQ")).params[(size_t) param].label : juce::String ("TRIM");
            label (d, name == "31" ? juce::String ("31.5") : name.replace ("K", "k"), x, 370, 13, ink).centre = -90.0f;
        }
        for (size_t i = onFaders; i < d.elements.size(); ++i)
            d.elements[i].page = 0;
        // CURVE
        auto& g = d.add (Type::Graph, 70, 126, 1000, 258);
        g.shows (GraphKind::Response, {});
        g.page = 1;
        return d;
    }

    /** PIA 560 GEQ (Fig. 83): ten slider bands, GAIN and MIX; the bus EQ model
        has no MIX. */
    Design piaEq (models::Table table)
    {
        const auto& spec = models::model (table, models::findModel (table, "PIA"));
        const auto p = [&spec] (const char* id) { return spec.find (id); };
        auto d = figurePanel (0xff23272b, Texture::Brushed, 0.8f);
        screwsAt (d, { { 50, 152 }, { 1105, 152 }, { 50, 342 }, { 1105, 342 } }, 18);
        const juce::uint32 ink = 0xffe8ecf0, blue = 0xff2a9cf0, line = 0xff2a8ac8;
        d.path ("M 90 82 L 152 120 L 90 158 Z", blue, 0.0f);
        d.text ("pia", 92, 100, 100, 44, 36, ink, Look::Serif, J::centredLeft);
        label (d, "560", 1060, 94, 28, blue, Look::SansLight);
        label (d, "GRAPHIC EQ", 1060, 117, 13, blue);
        d.box (182, 78, 401, 320, Look::Fill, 0x30e8c040, 0xffe8c040, 2.0f);
        for (auto [s, y] : { std::pair { "+12", 158.0f }, { "0", 238.0f }, { "-12", 318.0f } })
            label (d, s, 196, y, 12, ink);
        const char* bands[] = { "31", "63", "125", "250", "500", "1k", "2k", "4k", "8k", "16k" };
        for (int i = 0; i < 10; ++i)
        {
            const float x = 222.0f + 80.0f * (float) i;
            for (int k = 0; k < 5; ++k)
                if (i < 9)
                    d.box (x + 18, 158.0f + 40.0f * (float) k, 44, 0, Look::Line, line, 0, 2.0f);
            d.box (x - 11, 138, 22, 202, Look::Fill, 0xff050505);
            d.fader (p (bands[i]), x - 13, 128, 26, 222, 0xffe8e8e8);
            label (d, juce::String (bands[i]).toUpperCase(), x, 357, 13, ink);
        }
        d.knob (p ("g"), 1047, 162, 40, Look::BlackDome).scaleOf (9, "", 280).radii (1.4f, 1.6f).scol (ink);
        label (d, "GAIN", 1047, 196, 12, ink);
        if (table == models::Fx)
        {
            d.knob (p ("mix"), 1047, 240, 40, Look::ColourCap, 0xff20a0ff).scaleOf (9, "", 280).radii (1.4f, 1.6f).scol (ink);
            label (d, "MIX", 1047, 276, 12, ink);
        }
        d.key (-1, 1001, 299, 38, 38, Look::RoundKey);
        d.lamp (1073, 320, 9, 0xffff2020).at (1.0f);
        label (d, "IN", 1020, 357, 12, ink);
        return d;
    }

    /** DOUBLE VOCAL (Fig. 151): the console's flat design. */
    Design doubleVocal()
    {
        const auto p = fxParams ("DOUBLE");
        auto d = figurePanel (0xff121212, Texture::Smooth, 0.0f);
        const juce::uint32 orange = 0xffe8901e, white = 0xfff4f4f4;
        d.box (30, 70, 1096, 75, Look::Fill, 0xff2f363e);
        d.box (30, 70, 75, 75, Look::Fill, 0xfff07040);
        for (float r : { 30.0f, 24.0f, 18.0f })
            d.path ("M " + juce::String (67.5f - r) + " 107 a " + juce::String (r) + " " + juce::String (r) + " 0 1 0 " + juce::String (2 * r)
                        + " 0 a " + juce::String (r) + " " + juce::String (r) + " 0 1 0 " + juce::String (-2 * r) + " 0",
                    0x90ffe0d0, 1.5f);
        d.path ("M 53 107 a 14.5 14.5 0 1 0 29 0 a 14.5 14.5 0 1 0 -29 0 Z", white, 0.0f);
        d.text ("DOUBLE VOCAL", 130, 78, 600, 60, 46, 0xffb8c4d0, Look::SansLight, J::centredLeft);
        // MODE: an orange knob in rings.
        for (float r : { 70.0f, 62.0f, 54.0f, 47.0f })
            d.path ("M " + juce::String (225 - r) + " 275 a " + juce::String (r) + " " + juce::String (r) + " 0 1 0 " + juce::String (2 * r)
                        + " 0 a " + juce::String (r) + " " + juce::String (r) + " 0 1 0 " + juce::String (-2 * r) + " 0",
                    r > 66.0f ? 0xff2a1a0a : 0xffd8701a, r > 66.0f ? 8.0f : 1.6f);
        d.knob (p ("mode"), 225, 275, 78, Look::ColourDome, 0xfff8a010).scaleOf (0, "", 240).sweep = 240.0f;
        for (auto [s, x, y] : { std::tuple { "Tight", 118.0f, 315.0f }, { "Loose", 117.0f, 238.0f }, { "Group", 225.0f, 187.0f },
                                { "Detune", 340.0f, 238.0f }, { "Thick", 333.0f, 315.0f } })
            label (d, s, x, y, 22, white, Look::SansLight);
        label (d, "MODE", 225, 378, 28, orange, Look::SansLight);
        for (auto [id, cx, caption] : { std::tuple { "mix", 580.0f, "MIX" }, { "sprd", 898.0f, "SPREAD" } })
        {
            d.knob (p (id), cx, 285, 132, Look::ArcRing, 0xfff0c040);
            label (d, "50", cx, 197, 22, white, Look::SansLight);
            label (d, "0", cx - 78, 335, 22, white, Look::SansLight);
            label (d, "100", cx + 77, 335, 22, white, Look::SansLight);
            label (d, caption, cx, 373, 28, orange, Look::SansLight);
        }
        return d;
    }

    /** PITCH FIX (Fig. 150): three ring knobs with their values, the note keys
        (a key left lit is corrected to; dimmed, the note is left out). */
    Design pitchFix()
    {
        const auto p = fxParams ("PCORR");
        auto d = figurePanel (0xff1f2226, Texture::Rough, 0.6f);
        d.path ("M 30 120 Q 578 60 1126 120 L 1126 70 L 30 70 Z", 0xff121416, 0.0f);
        d.path ("M 300 110 L 380 330 L 30 330 L 30 120 Z", 0x16ffffff, 0.0f);
        d.path ("M 860 110 L 780 330 L 1126 330 L 1126 120 Z", 0x16ffffff, 0.0f);
        label (d, "PITCH FIX", 390, 108, 20, 0xffe8e8e8, Look::SerifBold).kerning (0.05f);
        const juce::uint32 cyan = 0xff8ad8f0;
        for (auto [id, cx, caption] : { std::tuple { "spd", 215.0f, "SPEED" }, { "amnt", 578.0f, "AMOUNT" }, { "a4", 938.0f, "A4 PITCH" } })
        {
            d.knob (p (id), cx, 200, 150, Look::ArcRing, cyan).scaleOf (7, "", 270, false).radii (1.05f, 1.3f).scol (0xff8a8a8a);
            d.knob (-1, cx, 200, 92, Look::BlackDome).at (0.5f).sweep = 0.0f;
            d.box (cx - 85, 285, 165, 30, Look::Line, 0xff5a5e64, 0, 1.5f);
            d.text (caption, cx - 75, 285, 100, 30, 17, 0xfff0f0f0, Look::Sans, J::centredLeft);
            auto& v = d.add (Type::Value, cx + 15, 285, 62, 30);
            v.param = p (id);
            v.look = Look::Sans;
            v.size = 16;
            v.colour = juce::Colour (cyan);
            v.colour2 = juce::Colour (0x00000000);
            v.just = J::centredLeft;
        }
        struct NoteKey
        {
            const char* id;
            const char* name;
            float x;
            bool black;
        };
        const NoteKey keys[] = { { "sw_c", "C", 30, false },       { "sw_db", "C# Db", 122, true }, { "sw_d", "D", 215, false },
                                 { "sw_eb", "D# Eb", 307, true },  { "sw_e", "E", 398, false },     { "sw_f", "F", 490, false },
                                 { "sw_gb", "F# Gb", 582, true },  { "sw_g", "G", 673, false },     { "sw_ab", "G# Ab", 767, true },
                                 { "sw_a", "A", 858, false },      { "sw_bb", "A# Bb", 950, true }, { "sw_b", "B", 1042, false } };
        for (const auto& k : keys)
        {
            auto& key = d.key (p (k.id), k.x, k.black ? 336.0f : 340.0f, 84, 60, Look::PianoKey, k.name, k.black ? 0xff24272b : 0xffc6c8ca);
            key.colour2 = juce::Colour (k.black ? 0xffd8d8d8 : 0xff141414);
            key.size = 16;
            key.inv(); // lit while the note is corrected to (switch off)
        }
        return d;
    }

    /** PRECISION LIMITER (Fig. 114): a brushed silver unit, the reduction meter in the middle. */
    Design precisionLimiter()
    {
        const auto p = fxParams ("LIMITER");
        auto d = figurePanel (0xffd6d8da, Texture::Brushed, 0.9f);
        screwsAt (d, { { 115, 92 }, { 1035, 92 }, { 115, 375 }, { 1035, 375 } });
        const juce::uint32 ink = 0xff3a4048;
        label (d, "P R E C I S I O N", 262, 145, 12, 0xff6a6e74).kerning (0.3f);
        d.box (330, 146, 75, 0, Look::Line, 0xff9a9ea4, 0, 1.0f);
        d.box (155, 170, 70, 0, Look::Line, 0xff9a9ea4, 0, 1.0f);
        label (d, "L I M I T E R", 282, 170, 12, 0xff6a6e74).kerning (0.3f);
        auto knob = [&] (const char* id, float cx, float cy, const char* caption, const char* lo, const char* unit, const char* hi)
        {
            d.knob (p (id), cx, cy, 80, Look::Chrome).scaleOf (21, "", 280, false).radii (1.04f, 1.3f).scol (0xff5a5e64);
            label (d, caption, cx, cy - 62, 14, ink);
            ends (d, cx, cy + 50, 28, lo, hi, 12, ink, unit);
        };
        knob ("gin", 202, 307, "IN GAIN", "0", "db", "18");
        knob ("gout", 412, 307, "OUT GAIN", "-18", "db", "0");
        knob ("sqz", 632, 170, "SQUEEZE", "0", nullptr, "100");
        knob ("knee", 840, 170, "KNEE", "Hard", nullptr, "Soft");
        knob ("att", 738, 307, "ATTACK", "0.05", "ms", "1");
        knob ("rel", 952, 307, "RELEASE", "20", "ms", "2000");
        label (d, "AUTO GAIN", 305, 254, 12, ink);
        d.key (p ("again"), 286, 273, 38, 38, Look::RoundKey, "", 0xffd8d8d8);
        d.lamp (307, 342, 10, 0xffff2020, p ("again"));
        // Gain reduction: a column of LEDs, 0 at the top.
        d.box (513, 126, 34, 228, Look::Fill, 0xff16181a, 0xff6a6e72, 1.5f);
        d.leds (517, 130, 26, 220, "", { 0.5f, 1, 1.5f, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12 }, true, 0xff40a8ff).dots = false;
        for (int i = 0; i <= 6; ++i)
            label (d, juce::String (i * 2), 500, 133.0f + 36.0f * (float) i, 10, ink);
        label (d, "REDUCTION", 530, 368, 11, ink);
        return d;
    }

    /** 2-BAND DEESSER (Fig. 122): a black pill on a silver frame, big white knobs. */
    Design dualDeEsser()
    {
        const auto p = fxParams ("DE-S2");
        auto d = figurePanel (0xffcfd0d0, Texture::Brushed, 0.9f);
        screwsAt (d, { { 68, 225 }, { 1098, 225 } }, 16);
        d.path ("M 225 130 L 930 130 A 130 130 0 0 1 930 390 L 225 390 A 130 130 0 0 1 225 130 Z", 0xff8a8c8e, 0.0f);
        d.path ("M 225 137 L 930 137 A 123 123 0 0 1 930 383 L 225 383 A 123 123 0 0 1 225 137 Z", 0xff121314, 0.0f);
        auto big = [&] (const char* id, float cx, float cy, float dia, const char* caption, float capY)
        {
            d.knob (-1, cx, cy, dia * 1.06f, Look::Chrome).at (0.5f).sweep = 0.0f;
            d.knob (p (id), cx, cy, dia * 0.86f, Look::WhiteDome).scaleOf (31, "", 270).radii (1.24f, 1.4f).scol (0xffeeeeee);
            label (d, caption, cx, capY, 14, 0xffeeeeee).kerning (0.15f);
            ends (d, cx, cy + dia * 0.52f, dia * 0.27f, "1", "50", 11, 0xffeeeeee);
        };
        big ("lo", 220, 235, 120, "LOW", 320);
        big ("hi", 505, 235, 120, "HIGH", 320);
        big ("los", 820, 320, 82, "LOW (SIDE)", 180);
        big ("his", 970, 320, 82, "HIGH (SIDE)", 180);
        for (auto [x, y] : { std::pair { 262.0f, 320.0f }, { 548.0f, 320.0f }, { 868.0f, 190.0f }, { 1020.0f, 190.0f } })
            d.lamp (x, y, 9, 0xff40d040).at (0.0f);
        label (d, "FEMALE", 360, 160, 13, 0xffeeeeee);
        d.key (p ("gdr"), 348, 183, 24, 24, Look::LedKey, "", 0xfff0e030).sel (0);
        d.key (p ("gdr"), 348, 250, 24, 24, Look::LedKey, "", 0xff40b0ff).sel (1);
        label (d, "MALE", 360, 300, 13, 0xffeeeeee);
        label (d, "M/S MODE", 662, 160, 13, 0xffeeeeee);
        d.key (p ("mode"), 645, 181, 34, 34, Look::LedKey, "", 0xfff0e030);
        label (d, "Dual", 712, 277, 32, 0xff2a5ad8);
        label (d, "DeEsser", 670, 304, 36, 0xff58c048);
        return d;
    }

    /** C5-COMBINATOR (Fig. 113): the band screen and the controls; the band
        row edits the band selected (touch a band on the screen). */
    Design combinator()
    {
        const auto p = fxParams ("C5-CMB");
        auto d = figurePanel (0xff707276, Texture::Brushed, 1.0f);
        screwsAt (d, { { 48, 87 }, { 1108, 87 }, { 48, 382 }, { 1108, 382 } }, 18);
        d.text ("C5-COMBINATOR", 880, 80, 236, 30, 22, 0xff161616, Look::SerifBold, J::centredRight).kerning (0.1f);
        auto& g = d.add (Type::Graph, 80, 128, 404, 222);
        g.shows (GraphKind::Bands, { p ("thr_1"), p ("thr_2"), p ("thr_3"), p ("thr_4"), p ("thr_5"), p ("bandsel") });
        // Touch a band on the screen: it is the band selected, the row below shows it.
        const float edges[] = { 0.0f, 0.19f, 0.37f, 0.57f, 0.75f, 1.0f };
        for (int b = 0; b < 5; ++b)
        {
            const float x0 = 116.0f + 360.0f * edges[b], x1 = 116.0f + 360.0f * edges[b + 1];
            auto& tab = d.key (p ("bandsel"), x0, 136, x1 - x0, 206, Look::TabKey, "");
            tab.choice = b;
        }
        d.box (528, 122, 552, 233, Look::Fill, 0xff1c1c1e, 0xff8a8a8a, 1.5f);
        const juce::uint32 ink = 0xfff0f0f0;
        auto knob = [&] (int param, float cx, float cy, float dia, const char* caption, float capY, float valueY, int page = -1)
        {
            d.knob (param, cx, cy, dia, Look::Chrome).scaleOf (25, "", 280, false).radii (1.08f, 1.3f).scol (0xffd8d8d8).page = page;
            label (d, caption, cx, capY, 13, ink).page = page;
            auto& v = d.add (Type::Value, cx - 45, valueY - 9, 90, 18);
            v.param = param;
            v.look = Look::Sans;
            v.size = 13;
            v.colour = juce::Colour (ink);
            v.colour2 = juce::Colour (0x00000000);
            v.page = page;
        };
        auto bat = [&] (int param, float cx, float cy, const char* caption, float capY, int page = -1)
        {
            d.toggle (param, cx, cy, 24).page = page;
            label (d, caption, cx, capY, 13, ink).page = page;
        };
        knob (p ("att"), 588, 180, 42, "Attack", 148, 220);
        knob (p ("rel"), 653, 180, 42, "Release", 148, 220);
        bat (p ("arel"), 710, 180, "Auto", 148);
        knob (p ("sbc"), 770, 180, 42, "SBC", 148, 220);
        bat (p ("sbcon"), 827, 180, "SBC", 148);
        knob (p ("thr"), 887, 180, 42, "Thresh", 148, 220);
        knob (p ("gain"), 955, 180, 42, "Gain", 148, 220);
        knob (p ("ratio"), 1020, 180, 42, "Ratio", 148, 220);
        d.box (552, 232, 383, 113, Look::Fill, 0x30c89030, 0xffc89030, 2.0f);
        for (int b = 0; b < 5; ++b)
        {
            const auto n = juce::String (b + 1);
            knob (p (("thr_" + n).toRawUTF8()), 600, 283, 60, "Thresh", 245, 335, b);
            knob (p (("gain_" + n).toRawUTF8()), 690, 283, 60, "Gain", 245, 335, b);
            bat (p (("byp_" + n).toRawUTF8()), 765, 290, "Bypass", 245, b);
            knob (p (("width_" + n).toRawUTF8()), 890, 283, 60, "Width", 245, 335, b);
        }
        bat (-1, 813, 290, "Solo", 245);
        bat (p ("slope"), 963, 283, "Slope", 245);
        knob (p ("mix"), 1020, 288, 42, "Mix", 245, 335);
        return d;
    }

    /** A knob with its caption over it and the scale's ends (and unit) under it. */
    void scaled (Design& d, int param, float cx, float cy, float dia, Look look, juce::uint32 colour, const char* caption, float capY, const char* lo,
                 const char* hi, const char* unit, juce::uint32 ink, float capSize = 14.0f)
    {
        d.knob (param, cx, cy, dia, look, colour);
        if (caption != nullptr)
            label (d, caption, cx, capY, capSize, ink);
        if (lo != nullptr)
            ends (d, cx, cy + dia * 0.62f, dia * 0.36f, lo, hi, 11, ink, unit);
    }

    /** ULTRA ENHANCER (Fig. 156): bands, the stereo and mono sections with VU meters. */
    Design ultraEnhancer()
    {
        const auto p = fxParams ("ENHANCE");
        auto d = figurePanel (0xffc89a62, Texture::Grunge, 0.9f);
        screwsAt (d, { { 55, 105 }, { 1098, 105 }, { 55, 372 }, { 1098, 372 } }, 22);
        const juce::uint32 ink = 0xff1e1408, line = 0xff6a4a28;
        label (d, "Enhancer", 74, 240, 40, 0xff3a2410, Look::Script).centre = -90.0f;
        for (auto [x, w, name] : { std::tuple { 128.0f, 100.0f, "BASS" }, { 233.0f, 100.0f, "MID" }, { 338.0f, 100.0f, "HIGH" },
                                   { 445.0f, 332.0f, "STEREO" }, { 785.0f, 218.0f, "MONO" } })
        {
            d.box (x, 86, w, 316, Look::Line, line, 0, 2.0f);
            label (d, name, x + w * 0.5f, 110, 13, ink);
        }
        const auto knob = [&] (const char* id, float cx, float cy, const char* caption, const char* lo, const char* unit, const char* hi)
        {
            scaled (d, p (id), cx, cy, 60, Look::WhiteDome, 0, nullptr, 0, lo, hi, unit, ink);
            label (d, caption, cx, cy + 56, 13, ink);
        };
        knob ("bassf", 180, 182, "FREQ", "min", nullptr, "max");
        knob ("midq", 284, 182, "Q", "min", nullptr, "max");
        knob ("highf", 388, 182, "FREQ", "min", nullptr, "max");
        knob ("bass", 180, 310, "GAIN", "0", "%", "100");
        knob ("mid", 284, 310, "GAIN", "0", "%", "100");
        knob ("high", 388, 310, "GAIN", "0", "%", "100");
        knob ("lmf", 500, 310, "LMF SPREAD", "-100", "%", "100");
        knob ("stlvl", 612, 310, "LEVEL", "-100", "%", "100");
        knob ("st", 723, 310, "PAN", "L", nullptr, "R");
        knob ("mlvl", 842, 310, "LEVEL", "-100", nullptr, "100");
        knob ("m", 948, 310, "PAN", "L", nullptr, "R");
        knob ("g", 1060, 310, "GAIN", "-12", "dB", "12");
        for (float cx : { 612.0f, 892.0f })
        {
            d.box (cx - 66, 122, 132, 132, Look::Fill, 0xff0e0c0a);
            d.vu (cx - 56, 132, 112, 62, Look::VuCream, "20|10|7|5|3|1|0", { 20, 10, 7, 5, 3, 1, 0 }, "VU");
        }
        d.lamp (1062, 136, 8, 0xff8a9a30).at (0.0f);
        d.key (p ("solo"), 1032, 160, 58, 58, Look::RoundKey, "", 0xffe0e0e0);
        label (d, "SOLO", 1060, 234, 13, ink);
        return d;
    }

    /** EXCITER (Fig. 152): green knobs on a light grey rack unit. */
    Design exciter()
    {
        const auto p = fxParams ("EXCITER");
        auto d = figurePanel (0xffd2d4d6, Texture::Brushed, 0.7f);
        d.box (178, 92, 650, 300, Look::Fill, 0x18000000);
        d.box (828, 92, 220, 300, Look::Fill, 0x10000000);
        for (float x = 205.0f; x < 1030.0f; x += 60.0f)
        {
            d.box (x, 94, 6, 28, Look::Fill, 0xffa8aaac);
            d.box (x, 362, 6, 28, Look::Fill, 0xffa8aaac);
        }
        d.box (75, 210, 52, 52, Look::Fill, 0xffeef0f2, 0xffb8babc, 2.0f);
        d.lamp (101, 236, 13, 0xff20a8ff).at (1.0f);
        label (d, "EXCITER", 102, 318, 22, 0xffeaeaea).kerning (0.05f);
        const juce::uint32 ink = 0xff3a3c40;
        const auto knob = [&] (const char* id, float cx, const char* caption, const char* lo, const char* unit, const char* hi)
        {
            scaled (d, p (id), cx, 245, 62, Look::ColourDome, 0xff16b040, caption, 185, lo, hi, unit, ink, 13);
        };
        knob ("tune", 245, "TUNE", "1K", "Hz", "10K");
        knob ("peak", 370, "PEAK", "MIN", "%", "MAX");
        knob ("zero", 500, "ZERO FILL", "MIN", "%", "MAX");
        knob ("tmb", 628, "TIMBRE", "LOW", nullptr, "HIGH");
        knob ("hrm", 755, "HARMONICS", "MIN", "%", "MAX");
        label (d, "DRY", 888, 185, 13, ink);
        d.lamp (888, 241, 13, 0xffa8e020, p ("solo")).invert = true; // lit while the dry signal is in
        scaled (d, p ("mix"), 970, 245, 54, Look::BlackSkirt, 0, "MIX", 185, "DRY", "WET", "%", ink, 13);
        return d;
    }

    /** PSYCHO BASS (Fig. 159): two blue VU meters, chrome knobs. */
    Design psychoBass()
    {
        const auto p = fxParams ("P-BASS");
        auto d = figurePanel (0xff1a2034, Texture::Brushed, 0.8f);
        screwsAt (d, { { 46, 85 }, { 1108, 85 }, { 46, 388 }, { 1108, 388 } }, 14);
        d.box (110, 102, 935, 155, Look::Fill, 0xff07090e);
        const juce::uint32 blue = 0xff5a6af0, ink = 0xffa8aec0;
        for (float x : { 178.0f, 708.0f })
            d.vu (x, 112, 270, 136, Look::VuWhite, "0|0.01|0.1|1|10|100|200", { 20, 14, 8, 3, 1, 0, -2 }, "dB").colour = juce::Colour (blue);
        label (d, "P S Y C H O", 578, 167, 22, 0xff7a7acc, Look::Serif).kerning (0.15f);
        label (d, "BASS", 578, 193, 17, blue);
        d.box (492, 194, 32, 5, Look::Fill, blue);
        d.box (628, 194, 32, 5, Look::Fill, 0xfff0f0ff);
        const auto knob = [&] (const char* id, float cx, const char* caption, const char* lo, const char* unit, const char* hi)
        {
            scaled (d, p (id), cx, 333, 62, Look::Chrome, 0, caption, 280, lo, hi, unit, ink, 14);
        };
        knob ("int", 168, "INTENSITY", "-24", "dB", "6");
        knob ("bass", 310, "BASS GAIN", "-60", "dB", "0");
        knob ("xf", 455, "X/O FREQUENCY", "32", "Hz", "200");
        label (d, "SOLO", 580, 280, 14, ink);
        d.key (p ("solo"), 553, 306, 54, 54, Look::RoundKey, "", 0xffe0e0e0);
        return d;
    }

    /** ROTARY SPEAKER (Fig. 143): the cabinet's control panel over its grille. */
    Design rotary()
    {
        const auto p = fxParams ("ROTARY");
        auto d = figurePanel (0xff4a2418, Texture::Grunge, 0.6f);
        d.box (30, 70, 70, 336, Look::Wood, 0);
        d.box (1056, 70, 70, 336, Look::Wood, 0);
        d.box (100, 85, 956, 158, Look::Fill, 0xff26282a, 0xff111111, 2.0f);
        d.box (100, 252, 956, 154, Look::Wood, 0);
        for (float x0 : { 200.0f, 785.0f })
            for (int i = 0; i < 7; ++i)
                d.box (x0 + 27.0f * (float) i, 275, 11, 100, Look::Fill, 0xff140a06);
        label (d, "Rotary", 628, 312, 18, 0xff6a4030, Look::SerifItalic);
        label (d, "WING", 575, 342, 46, 0xff6a4030, Look::SerifItalic);
        const juce::uint32 ink = 0xffd8d8d8, title = 0xffb8b8b8;
        label (d, "SPEED CONTROL", 260, 104, 15, title).kerning (0.08f);
        label (d, "ACCELERATION", 545, 104, 15, title).kerning (0.08f);
        d.box (418, 120, 0, 100, Look::Line, 0xff5a5a5a, 0, 1.2f);
        d.box (672, 120, 0, 100, Look::Line, 0xff5a5a5a, 0, 1.2f);
        const auto knob = [&] (const char* id, float cx, const char* caption, const char* lo, const char* unit, const char* hi)
        {
            scaled (d, p (id), cx, 172, 60, Look::SilverDial, 0, caption, 125, lo, hi, unit, ink, 13);
        };
        knob ("lo", 175, "SLOW RATE", "0.1", "Hz", "4");
        knob ("hi", 345, "FAST RATE", "4", "Hz", "10");
        knob ("dac", 488, "BASS", "MIN", nullptr, "MAX");
        knob ("hac", 600, "HORN", "MIN", nullptr, "MAX");
        knob ("bal", 742, "BALANCE", "BASS", nullptr, "HORN");
        knob ("dist", 855, "DISTANCE", "MIN", nullptr, "MAX");
        knob ("mix", 968, "MIX", "MIN", nullptr, "MAX");
        // The speed lever: STOP / SLOW / FAST.
        d.box (234, 140, 8, 76, Look::Fill, 0xff0e0e0e);
        for (int i = 0; i < 3; ++i)
        {
            const char* names[] = { "STOP", "SLOW", "FAST" };
            const float y = 203.0f - 29.0f * (float) i;
            d.key (p ("sw"), 228, y - 9, 20, 18, Look::FlatKey, "", 0xffd8b030).sel (i);
            d.text (names[i], 252, y - 8, 60, 16, 12, ink, Look::Sans, J::centredLeft);
        }
        return d;
    }

    /** The LFO and envelope sections PHASER and TREMOLO/PANNER share (Fig. 144, 145). */
    void lfoAndEnvelope (Design& d, const std::function<int (const char*)>& p, const juce::String& phaseLo, int wave)
    {
        d.box (602, 70, 140, 336, Look::Bevel, 0xffa8aaae);
        label (d, "LFO", 700, 98, 22, 0xffd8dade);
        const juce::uint32 dark = 0xff1a1a1a;
        d.knob (p ("spd"), 655, 145, 50, Look::BlackSkirt);
        label (d, "SPEED", 708, 145, 12, dark);
        ends (d, 655, 178, 20, "0.05", "5", 10, dark, "Hz");
        d.knob (p ("phase"), 712, 240, 60, Look::BlackSkirt);
        label (d, "PHASE", 650, 242, 12, dark);
        label (d, "L/R", 650, 255, 10, dark);
        ends (d, 712, 282, 26, phaseLo, juce::String::fromUTF8 ("180\xc2\xb0"), 10, dark);
        d.knob (wave, 650, 335, 52, Look::BlackSkirt);
        label (d, "SHAPE", 708, 334, 12, dark);
        d.box (802, 70, 232, 336, Look::Bevel, 0xffc08868);
        label (d, "ENVELOPE", 918, 98, 22, 0xffeec4a8).kerning (0.05f);
        auto& g = d.add (Type::Graph, 846, 140, 172, 100);
        g.shows (GraphKind::Decay, { p ("att"), p ("hld"), p ("rel") }, 3);
        for (auto [id, cx, caption, hi] : { std::tuple { "att", 840.0f, "ATTACK", "1000" }, { "hld", 932.0f, "HOLD", "2000" }, { "rel", 1020.0f, "RELEASE", "1000" } })
        {
            d.knob (p (id), cx, 330, 52, Look::BlackSkirt);
            label (d, caption, cx, 286, 13, dark);
            ends (d, cx, 362, 20, "10", hi, 10, dark, "ms");
        }
        for (float x : { 560.0f, 1072.0f })
            d.box (x, 115, 20, 230, Look::Fill, 0xff102a70);
    }

    /** PHASER (Fig. 144): the multi phaser, LFO and envelope. */
    Design phaser()
    {
        const auto p = fxParams ("PHASER");
        auto d = figurePanel (0xff17377a, Texture::Rough, 0.6f);
        d.box (98, 70, 450, 336, Look::Bevel, 0xff6a84a8);
        d.box (70, 115, 20, 230, Look::Fill, 0xff102a70);
        const juce::uint32 ink = 0xff101828;
        label (d, "MULTI", 290, 130, 34, 0xff8ab0e0);
        label (d, "PHASER", 290, 170, 34, 0xff8ab0e0);
        d.box (170, 98, 42, 14, Look::Fill, 0xffd82020);
        label (d, "MIX", 191, 105, 10, 0xffffffff);
        scaled (d, p ("mix"), 195, 148, 58, Look::Chrome, 0, nullptr, 0, "DRY", "WET", nullptr, ink);
        scaled (d, p ("reso"), 450, 132, 66, Look::Chrome, 0, "RESONANCE", 91, "MIN", "MAX", nullptr, ink, 13);
        d.box (205, 240, 224, 12, Look::Fill, 0xff0a1020);
        d.fader (p ("stg"), 202, 228, 230, 36, 0xffd8d8d8).sideways();
        label (d, "2", 216, 216, 12, ink);
        label (d, "12", 416, 216, 12, ink);
        label (d, "STAGES", 470, 246, 13, ink);
        scaled (d, p ("range"), 207, 330, 70, Look::Chrome, 0, "RANGE", 286, "MIN", "MAX", nullptr, ink, 13);
        scaled (d, p ("depth"), 328, 330, 70, Look::Chrome, 0, "DEPTH", 286, "MIN", "MAX", nullptr, ink, 13);
        scaled (d, p ("emod"), 450, 330, 70, Look::Chrome, 0, "ENV. MOD", 286, "MIN", "MAX", nullptr, ink, 13);
        lfoAndEnvelope (d, p, juce::String::fromUTF8 ("0\xc2\xb0"), p ("wave"));
        return d;
    }

    /** TREMOLO/PANNER (Fig. 145): envelope modulation, tremolo with its meter, LFO and envelope. */
    Design tremolo()
    {
        const auto p = fxParams ("PANNER");
        auto d = figurePanel (0xff17377a, Texture::Rough, 0.6f);
        d.box (98, 70, 450, 336, Look::Bevel, 0xff9ea2a8);
        d.box (70, 115, 20, 230, Look::Fill, 0xff102a70);
        const juce::uint32 ink = 0xff101418;
        d.box (132, 88, 125, 304, Look::Line, ink, 0, 1.2f);
        label (d, "ENVELOPE", 195, 101, 13, ink);
        label (d, "MODULATION", 195, 115, 13, ink);
        scaled (d, p ("edep"), 195, 192, 70, Look::Chrome, 0, "DEPTH", 140, "MIN", "MAX", nullptr, ink, 13);
        scaled (d, p ("espd"), 195, 318, 70, Look::Chrome, 0, "SPEED", 268, "MIN", "MAX", nullptr, ink, 13);
        label (d, "TREMOLO", 325, 170, 22, 0xff3a3e44);
        scaled (d, p ("depth"), 325, 258, 68, Look::Chrome, 0, "DEPTH", 210, "MIN", "MAX", nullptr, ink, 13);
        d.box (390, 112, 98, 254, Look::Fill, 0xff2a2a2a);
        d.box (395, 117, 88, 220, Look::Fill, 0xffd8c8a0);
        d.box (439, 117, 0, 220, Look::Line, 0x60000000, 0, 1.0f);
        d.text ("L  C  R", 395, 340, 88, 22, 12, 0xffeeeeee);
        lfoAndEnvelope (d, p, juce::String::fromUTF8 ("-180\xc2\xb0"), p ("wave"));
        return d;
    }

    /** TAPE MACHINE (Fig. 157): the reels over the machine's light grey front. */
    Design tapeMachine()
    {
        const auto p = fxParams ("TAPE");
        auto d = figurePanel (0xff5a5048, Texture::Grunge, 0.7f);
        // Reels.
        for (float cx : { 330.0f, 820.0f })
        {
            d.knob (-1, cx, 112, 230, Look::SilverDial).at (0.3f).sweep = 0.0f;
            d.box (cx - 70, 145, 140, 70, Look::Fill, 0xff2a2624, 0xff111111, 2.0f);
        }
        d.box (450, 145, 240, 60, Look::Bevel, 0xffa8aaac);
        d.path ("M 70 255 L 140 140 L 1020 140 L 1090 255 L 1090 406 L 70 406 Z", 0xffe2e4e6, 0.0f);
        d.box (70, 255, 1020, 151, Look::Fill, 0xffeceeee);
        const juce::uint32 ink = 0xff2a2c30;
        scaled (d, p ("out"), 177, 318, 50, Look::Chrome, 0, nullptr, 0, nullptr, nullptr, nullptr, ink);
        label (d, "0dB", 177, 274, 12, ink);
        label (d, "-12dB", 128, 338, 12, ink);
        label (d, "12dB", 222, 338, 12, ink);
        label (d, "OUT GAIN", 177, 356, 13, ink);
        for (auto [id, cx, caption] : { std::tuple { "low", 315.0f, "LOW BUMP" }, { "hi", 415.0f, "HIGH SHELV" } })
        {
            d.toggle (p (id), cx, 315, 20);
            label (d, "ON", cx - 30, 300, 12, ink);
            label (d, "OFF", cx - 32, 326, 12, ink);
            label (d, caption, cx - 10, 356, 13, ink);
        }
        d.box (468, 272, 220, 84, Look::Fill, 0xff7a7c7e);
        d.vu (472, 276, 212, 76, Look::VuAmber, "-20|-10|-7|-5|-3|-1|0|+1|+2|+3", { 20, 10, 7, 5, 3, 1, 0, -1, -2, -3 }, "VU");
        scaled (d, p ("drv"), 795, 318, 50, Look::Chrome, 0, nullptr, 0, nullptr, nullptr, nullptr, ink);
        label (d, "10dB", 795, 274, 12, ink);
        label (d, "-5dB", 748, 338, 12, ink);
        label (d, "25dB", 846, 338, 12, ink);
        label (d, "DRIVE", 795, 356, 13, ink);
        scaled (d, p ("spd"), 1005, 318, 50, Look::Chrome, 0, nullptr, 0, nullptr, nullptr, nullptr, ink);
        label (d, "7.5 ips", 948, 338, 12, ink);
        label (d, "30 ips", 1060, 338, 12, ink);
        label (d, "SPEED", 1005, 356, 13, ink);
        return d;
    }

    /** MOOD FILTER (Fig. 146): envelope, filter and LFO sections round the logo. */
    Design moodFilter()
    {
        const auto p = fxParams ("MOOD");
        auto d = figurePanel (0xff232528, Texture::Rough, 0.6f);
        d.box (30, 70, 30, 336, Look::Wood, 0);
        d.box (1096, 70, 30, 336, Look::Wood, 0);
        screwsAt (d, { { 75, 177 }, { 75, 312 }, { 1080, 177 }, { 1080, 312 } }, 14);
        const juce::uint32 sky = 0xffb8e0f8;
        const auto section = [&] (float x, float w, const char* name)
        {
            d.path ("M " + juce::String (x) + " 406 L " + juce::String (x) + " 105 Q " + juce::String (x) + " 92 " + juce::String (x + 13) + " 92 L "
                        + juce::String (x + w - 13) + " 92 Q " + juce::String (x + w) + " 92 " + juce::String (x + w) + " 105 L " + juce::String (x + w) + " 406",
                    sky, 3.0f);
            d.box (x + 6, 92, w - 12, 16, Look::Fill, sky);
            label (d, name, x + w * 0.5f, 100, 13, 0xff1a1a1a);
        };
        section (102, 233, "ENVELOPE");
        section (335, 230, "FILTER");
        section (815, 240, "LFO");
        const auto knob = [&] (int param, float cx, float cy, const char* caption, const char* lo, const char* hi)
        {
            scaled (d, param, cx, cy, 50, Look::ChromeSmall, 0, caption, cy - 52, nullptr, nullptr, nullptr, sky, 13);
            if (lo != nullptr)
                ends (d, cx, cy + 40, 32, lo, hi, 12, sky);
        };
        knob (p ("env"), 162, 188, "DEPTH", "-100 %", "100 %");
        knob (p ("att"), 270, 188, "ATTACK", "10 ms", "250 ms");
        knob (p ("hld"), 162, 330, "HOLD", "1 ms", "500 ms");
        knob (p ("rel"), 270, 330, "RELEASE", "10 ms", "500 ms");
        knob (p ("fbase"), 395, 188, "BASE FREQ", "20Hz", "15KHz");
        d.knob (p ("filt"), 505, 188, 50, Look::ChromeSmall).scaleOf (4, "LP|HP|BP|NOTCH", 210).radii (1.2f, 1.75f).lsize (12).scol (sky);
        label (d, "TYPE", 505, 132, 13, sky);
        knob (p ("reso"), 505, 330, "RESONANCE", "0.0", "10.0");
        label (d, "SLOPE", 390, 278, 13, sky);
        d.key (p ("slope"), 365, 307, 50, 22, Look::LitSquare, "", 0xffd03030).sel (1).colour = juce::Colour (0xffb02828);
        d.lamp (363, 340, 5, 0xffa0e030, p ("slope")).sel (0);
        d.lamp (415, 340, 5, 0xffa0e030, p ("slope")).sel (1);
        label (d, "12 dB", 363, 357, 12, sky);
        label (d, "24 dB", 415, 357, 12, sky);
        label (d, "Oct", 363, 372, 12, sky);
        label (d, "Oct", 415, 372, 12, sky);
        label (d, "mood", 680, 150, 56, 0xff6ac0f0, Look::SansLight);
        label (d, "MULTIMODE FILTER", 680, 182, 15, sky);
        knob (p ("drv"), 620, 285, "DRIVE", "0.0", "10.0");
        knob (p ("mix"), 750, 285, "MIX", "0 %", "100 %");
        knob (p ("lfo"), 885, 188, "DEPTH", "0 %", "100 %");
        knob (p ("spd"), 995, 188, "SPEED", "0.05 Hz", "20 Hz");
        knob (p ("phase"), 885, 330, "PHASE", "0 deg", "180 deg");
        d.knob (p ("wave"), 985, 330, 46, Look::ChromeSmall).scaleOf (7, "TRI|SIN|SAW+|SAW-|RAMP|SQUARE|RANDOM", 260).radii (1.2f, 2.0f).lsize (11).scol (sky);
        label (d, "WAVE", 985, 278, 13, sky);
        return d;
    }

    /** SUB OCTAVER (Fig. 160): the red pedal with its cables. */
    Design subOctaver()
    {
        const auto p = fxParams ("SUB");
        auto d = figurePanel (0xffd8d4cc, Texture::Grunge, 0.5f);
        for (float y : { 150.0f, 290.0f })
        {
            d.path ("M 30 " + juce::String (y + 80) + " C 120 " + juce::String (y + 40) + " 160 " + juce::String (y) + " 260 " + juce::String (y),
                    0xff151515, 12.0f);
            d.box (260, y - 14, 140, 28, Look::Bevel, 0xffb8b8b8);
        }
        d.box (400, 70, 330, 336, Look::Fill, 0xff7a2a2a, 0xff4a1616, 2.0f);
        d.box (730, 70, 320, 336, Look::Fill, 0xff6a2424, 0xff4a1616, 2.0f);
        const juce::uint32 ink = 0xfff0e8e8;
        for (auto [id, cy, caption] : { std::tuple { "oct1", 166.0f, "OCTAVE 1" }, { "oct2", 306.0f, "OCTAVE 2" } })
        {
            d.knob (p (id), 507, cy, 92, Look::BlackDot);
            label (d, "50 %", 507, cy - 62, 14, ink);
            label (d, "0 %", 470, cy + 49, 14, ink);
            label (d, "100 %", 568, cy + 49, 14, ink);
            label (d, caption, 420, cy, 13, ink).centre = -90.0f;
        }
        d.lamp (424, 242, 7, 0xffff2020).at (1.0f);
        d.box (595, 178, 130, 138, Look::Line, ink, 0, 1.5f);
        label (d, "RANGE", 657, 157, 14, ink);
        d.knob (p ("rng"), 660, 245, 54, Look::BlackDot).scaleOf (3, "LOW|MID|HIGH", 180).radii (1.0f, 1.55f).lsize (13).scol (ink).centred (0);
        label (d, "INPUT", 795, 140, 12, ink).centre = -90.0f;
        label (d, "OUTPUT", 795, 330, 12, ink).centre = -90.0f;
        label (d, "SUBOCTAVER", 860, 238, 36, ink).centre = -90.0f;
        d.box (910, 85, 125, 305, Look::Fill, 0xff3a3a3a, 0xff1a1a1a, 2.0f);
        label (d, "PRESS", 972, 238, 32, 0xff5a5a5a, Look::Serif).centre = 90.0f;
        return d;
    }

    /** SUB MONSTER (Fig. 161): five band faders with their frequencies, TUNE and MIX. */
    Design subMonster()
    {
        const auto p = fxParams ("SUB-M");
        auto d = figurePanel (0xff6e7c8e, Texture::Brushed, 0.8f);
        screwsAt (d, { { 65, 100 }, { 1100, 100 }, { 65, 370 }, { 1100, 370 } }, 12);
        const juce::uint32 ink = 0xfff2f4f8;
        label (d, "SUB Monster", 915, 138, 36, 0xffe8eaee, Look::Script);
        label (d, "MIDAS", 955, 180, 22, 0xff1a1a1a).kerning (0.15f);
        const auto readout = [&] (int param, float cx, float y)
        {
            d.box (cx - 34, y, 68, 22, Look::Fill, 0xfff0f0f0, 0xff8a8a8a, 1.0f);
            auto& v = d.add (Type::Value, cx - 34, y, 68, 22);
            v.param = param;
            v.look = Look::Sans;
            v.size = 13;
            v.colour = juce::Colour (0xff1a1a1a);
            v.colour2 = juce::Colour (0x00000000);
        };
        for (int b = 0; b < 5; ++b)
        {
            const float cx = 170.0f + 125.0f * (float) b;
            label (d, "Band " + juce::String (b + 1), cx, 112, 13, ink);
            d.box (cx - 46, 123, 92, 260, Look::Line, 0xffc8ccd2, 0, 2.0f);
            d.box (cx - 32, 135, 64, 205, Look::Fill, 0xff3a4250);
            for (int k = 0; k < 7; ++k)
                d.box (cx - 24, 157.0f + 27.0f * (float) k, 48, 0, Look::Line, 0x90ffffff, 0, 2.0f);
            d.box (cx - 3, 142, 6, 192, Look::Fill, 0xff0a0a0a);
            d.fader (p (("bd" + std::to_string (b + 1)).c_str()), cx - 46, 140, 92, 196, 0xffeceef0);
            readout (p (("bd" + std::to_string (b + 1)).c_str()), cx, 355);
        }
        d.knob (p ("freq"), 845, 293, 70, Look::WhiteDome);
        label (d, "Tune", 845, 238, 14, ink);
        readout (p ("freq"), 845, 355);
        d.knob (p ("mix"), 983, 293, 70, Look::WhiteDome);
        label (d, "Mix", 983, 238, 14, ink);
        readout (p ("mix"), 983, 355);
        return d;
    }

    /** VELVET IMAGER (Fig. 147): meters, three knobs round the scope, mode keys. */
    Design velvetImager()
    {
        const auto p = fxParams ("V-IMG");
        auto d = figurePanel (0xff8e1428, Texture::Brushed, 0.6f);
        screwsAt (d, { { 45, 90 }, { 1108, 90 }, { 45, 380 }, { 1108, 380 } }, 16);
        const juce::uint32 ink = 0xfff2e6ea, cap = 0xffd8c4ca;
        label (d, "velvet", 578, 100, 30, 0xffd0a0b0, Look::SansLight);
        label (d, "IMAGER", 580, 126, 18, cap).kerning (0.25f);
        const auto meter = [&] (float x, const char* l, const char* r)
        {
            d.box (x, 166, 54, 160, Look::Fill, 0xff1a1416, 0xff3a2a2e, 1.5f);
            for (int c = 0; c < 2; ++c)
                for (int k = 0; k < 14; ++k)
                    d.box (x + 6 + 23.0f * (float) c, 172.0f + 10.8f * (float) k, 19, 7, Look::Fill, k >= 5 ? 0xffe8a030 : 0xff3a2a20);
            d.text (l, x - 4, 330, 32, 12, 9, cap);
            d.text (r, x + 26, 330, 32, 12, 9, cap);
        };
        label (d, "INPUT", 115, 151, 12, cap);
        meter (88, "LEFT", "RIGHT");
        label (d, "OUTPUT", 1013, 151, 12, cap);
        meter (955, "LEFT", "RIGHT");
        meter (1018, "MID", "SIDE");
        const auto knob = [&] (const char* id, float cx, const char* top, const char* lo, const char* hi, const char* caption)
        {
            d.knob (p (id), cx, 228, 66, Look::BlackDot);
            label (d, top, cx, 171, 14, ink);
            ends (d, cx, 271, 22, lo, hi, 14, ink);
            label (d, caption, cx, 299, 15, cap, Look::SansLight);
        };
        knob ("wid", 270, "0", "-1", "1", "MS-WIDTH");
        knob ("st", 400, "50", "0", "100", "STEREOIZE");
        knob ("g", 750, "0", "-6", "6", "GAIN");
        d.box (505, 155, 150, 150, Look::Fill, 0xff050505, 0xff2a2a2a, 2.0f);
        for (int i = 0; i < 60; ++i)
            d.box (575.0f + 6.0f * std::sin ((float) i * 1.7f), 175.0f + 1.9f * (float) i, 2, 2, Look::Fill, 0xffe8a030);
        label (d, "LF VECTOR SCOPE", 580, 316, 11, cap);
        label (d, "MODE", 870, 151, 12, cap);
        d.key (p ("mode"), 843, 160, 54, 30, Look::LitSquare, "K-Stereo", 0xffe8b030).sel (0).size = 11;
        d.key (p ("mode"), 843, 192, 54, 30, Look::LitSquare, "Velvet", 0xffe8b030).sel (1).size = 11;
        label (d, "DEEP", 870, 263, 12, cap);
        d.key (p ("deep"), 843, 273, 54, 30, Look::LitSquare, "On", 0xffe8b030).size = 11;
        label (d, "CORRELATION", 580, 343, 11, cap);
        d.box (316, 360, 528, 24, Look::Fill, 0xff1a1012, 0xff3a2a2e, 1.5f);
        for (int k = 0; k < 40; ++k)
            d.box (322.0f + 13.0f * (float) k, 365, 7, 14, Look::Fill, k == 33 ? 0xffe8a030 : (k == 20 ? 0xff9a9a9a : 0xff3a2c30));
        label (d, "-1", 298, 372, 13, ink);
        label (d, "+1", 862, 372, 13, ink);
        return d;
    }

    /** RACK AMP (Fig. 162): the amp with its two footswitches and the Colour EQ pedal. */
    Design rackAmp()
    {
        const auto p = fxParams ("RACKAMP");
        auto d = figurePanel (0xff5a321c, Texture::Smooth, 0.0f);
        d.box (30, 70, 1096, 336, Look::Wood, 0);
        d.box (112, 70, 600, 336, Look::Bevel, 0xff464a4c);
        d.box (122, 270, 580, 136, Look::Fill, 0xff2e3032);
        const juce::uint32 ink = 0xffe8e030;
        d.box (162, 112, 120, 108, Look::Line, ink, 0, 2.5f);
        d.box (290, 112, 395, 108, Look::Line, ink, 0, 2.5f);
        label (d, "OUT", 130, 140, 11, ink).centre = -90.0f;
        const auto knob = [&] (const char* id, float cx, const char* caption, const char* lo, const char* unit, const char* hi, bool above = false)
        {
            scaled (d, p (id), cx, above ? 175.0f : 147.0f, 50, Look::BlackDome, 0, nullptr, 0, lo, hi, unit, 0xffe8e8e8);
            label (d, caption, cx, above ? 136.0f : 207.0f, 12, 0xffe8e8e8);
        };
        knob ("out", 210, "OUT GAIN", "0", "dB", "10");
        knob ("drive", 327, "DRIVE", "min", "%", "max");
        knob ("pre", 405, "PRE AMP", "0", "dB", "10");
        knob ("buzz", 488, "BUZZ", "min", "%", "max", true);
        knob ("punch", 567, "PUNCH", "min", "%", "max", true);
        knob ("crunch", 648, "CRUNCH", "min", "%", "max", true);
        d.lamp (473, 255, 6, 0xffff2020).at (0.0f);
        d.lamp (640, 255, 6, 0xffff2020, p ("cab"));
        label (d, "BYPASS", 473, 307, 12, ink);
        label (d, "CABINET SIMULATION", 635, 307, 12, ink);
        d.knob (-1, 473, 340, 42, Look::Chrome).at (0.5f).sweep = 0.0f;
        d.key (p ("cab"), 614, 318, 44, 44, Look::Hidden);
        d.knob (-1, 636, 340, 42, Look::Chrome).at (0.5f).sweep = 0.0f;
        d.box (150, 337, 180, 30, Look::Fill, 0xff0a0a0a);
        label (d, "WING AMP", 240, 352, 20, ink, Look::SerifItalic).kerning (0.1f);
        // Colour: the EQ pedal.
        d.path ("M 712 205 C 760 205 730 385 790 385 C 850 385 830 205 882 205", 0xff151515, 3.0f);
        d.box (882, 72, 132, 332, Look::Fill, 0xff38b028, 0xff207018, 3.0f);
        d.knob (p ("heq"), 948, 133, 56, Look::BlackDot);
        label (d, "HI EQ", 948, 181, 10, 0xff1a3a10);
        d.knob (p ("leq"), 948, 242, 56, Look::BlackDot);
        label (d, "LO EQ", 948, 287, 10, 0xff1a3a10);
        label (d, "Colour", 948, 341, 30, 0xff0e1e08, Look::Script);
        return d;
    }

    /** The guitar amp heads (Fig. 163-166): the logo over the control strip. */
    Design guitarAmp (const char* token)
    {
        const auto p = fxParams (token);
        const juce::String t (token);
        const bool uk = t == "UKROCK", angel = t == "ANGEL", jazz = t == "JAZZC";
        auto d = figurePanel (0xff1a1a1a, Texture::Rough, 1.0f);
        if (uk || angel)
        {
            const juce::uint32 strip = uk ? 0xffd8c898 : 0xff202122;
            d.box (68, 92, 1022, 165, Look::Fill, 0xff0c0c0c, 0xff3a3a3a, 3.0f);
            if (angel)
                for (int r = 0; r < 9; ++r)
                    for (int c = 0; c < 34; ++c)
                        d.path ("M " + juce::String (78 + 30 * c) + " " + juce::String (106 + 17 * r) + " l 10 0 m -5 -5 l 0 10", 0xffb8b8b8, 1.5f);
            label (d, uk ? "Musichall" : "ANGEL", 580, 168, uk ? 64 : 62, 0xfff0f0f0, uk ? Look::Script : Look::SansLight).kerning (angel ? 0.05f : 0.0f);
            d.box (68, 260, 1022, uk ? 120 : 132, Look::Bevel, strip);
            const juce::uint32 ink = uk ? 0xff1a1a1a : 0xffe8e8e8;
            const auto amp = [&] (const char* id, float cx, const char* caption)
            {
                auto& k = d.knob (p (id), cx, 330, uk ? 44 : 40, uk ? Look::Knurled : Look::BlackSkirt, 0xffd8b860);
                if (uk)
                    k.scaleOf (6, "0|2|4|6|8|10", 300).radii (1.3f, 1.75f).lsize (11).scol (ink);
                else
                    k.scaleOf (11, "", 300).radii (1.3f, 1.5f).scol (ink);
                label (d, caption, cx, 285, 13, ink);
            };
            const auto lever = [&] (int param, float cx, const char* caption)
            {
                d.toggle (param, cx, 336, 20);
                label (d, "ON", cx - 28, 322, 12, ink);
                label (d, "OFF", cx - 30, 349, 12, ink);
                d.lamp (cx, 296, 5, 0xffff2020, param);
                label (d, caption, cx - 6, 285, 13, ink);
            };
            if (uk)
            {
                d.key (-1, 80, 300, 40, 40, Look::LitSquare, "", 0xffd03020).at (1.0f);
                label (d, "POWER", 100, 362, 10, ink);
                lever (p ("cab"), 185, "CABINET");
                d.lamp (185, 296, 5, 0xffff2020, p ("cab"));
                amp ("out", 255, "OUTPUT GAIN");
                amp ("sag", 338, "SAG");
                label (d, "UK ROCK", 470, 312, 22, ink);
                label (d, "GUITAR AMP", 487, 340, 20, ink, Look::SansLight);
                const std::pair<const char*, const char*> row[] = { { "gain", "GAIN" },  { "bass", "BASS" },     { "mid", "MIDDLE" },
                                                                    { "treb", "TREBLE" }, { "pres", "PRESENCE" }, { "mstr", "MASTER" } };
                for (int i = 0; i < 6; ++i)
                    amp (row[i].first, 588.0f + 80.0f * (float) i, row[i].second);
            }
            else
            {
                const std::pair<const char*, const char*> row[] = { { "out", "Output Gain" }, { "sag", "SAG" },      { "gain", "Clean Gain" },
                                                                    { "bass", "Bass" },        { "mid", "Middle" },  { "treb", "Treble" },
                                                                    { "mstr", "Master" },      { "pres", "Presence" } };
                for (int i = 0; i < 8; ++i)
                    amp (row[i].first, (i < 2 ? 106.0f + 78.0f * (float) i : 270.0f + 80.0f * (float) (i - 2)), row[i].second);
                lever (p ("midb"), 760, "MID Boost");
                lever (p ("bri"), 840, "Bright");
                lever (p ("bot"), 915, "Bottom");
                lever (p ("cab"), 995, "Cabinet");
                label (d, "Power", 1048, 285, 13, ink);
                d.key (-1, 1030, 320, 38, 38, Look::LitSquare, "", 0xffd03020).at (1.0f);
            }
            return d;
        }
        // Combos: control strip on top, grille cloth below.
        const juce::uint32 ink = 0xffeeeeee;
        d.box (55, 118, 1048, 150, Look::Fill, 0xff26282a, 0xff111111, 2.0f);
        d.box (55, 268, 1048, 138, Look::Fill, jazz ? 0xff3a3a3c : 0xffd4ccbc);
        for (int i = 0; i < 26; ++i)
            d.box (60, 275.0f + 5.0f * (float) i, 1038, 0, Look::Line, jazz ? 0x50ffffff : 0x40000000, 0, 1.0f);
        const auto cab = [&] (float cx)
        {
            d.lamp (cx, 160, 5, 0xffff2020, p ("cab"));
            d.toggle (p ("cab"), cx, 194, 18);
            label (d, "ON", cx + 28, 182, 12, ink);
            label (d, "OFF", cx + 30, 207, 12, ink);
        };
        if (jazz)
        {
            d.text ("INPUT", 85, 138, 90, 14, 12, ink);
            for (float x : { 105.0f, 158.0f })
                d.knob (-1, x, 197, 26, Look::Chrome).at (0.5f).sweep = 0.0f;
            d.box (205, 130, 440, 125, Look::Fill, 0xff34363a, 0xff1a1a1a, 1.5f);
            label (d, "CABINET", 237, 141, 13, ink);
            cab (240);
            label (d, "BRI", 315, 141, 13, ink);
            d.lamp (315, 160, 5, 0xffff2020, p ("bri"));
            d.toggle (p ("bri"), 315, 194, 18);
            d.box (718, 122, 115, 140, Look::Fill, 0xff6a5a30);
            const std::pair<const char*, const char*> row[] = { { "vol", "VOLUME" }, { "treb", "TREBLE" }, { "mid", "MIDDLE" }, { "bass", "BASS" }, { "out", "OUTPUT GAIN" } };
            for (int i = 0; i < 5; ++i)
            {
                const float cx = 388.0f + 92.0f * (float) i + (i == 4 ? 30.0f : 0.0f);
                d.knob (p (row[i].first), cx, 196, 54, Look::ChromeCollar).scaleOf (11, "", 280).radii (1.18f, 1.5f).scol (ink);
                label (d, row[i].second, cx, 141, 13, ink);
                ends (d, cx, 236, 34, "0", "10", 10, ink);
            }
            label (d, "JAZZ CLEAN", 930, 141, 26, 0xffd8f0ff).kerning (0.02f);
            d.lamp (873, 200, 10, 0xffff8030).at (1.0f);
            label (d, "POWER", 948, 174, 12, ink);
            d.key (-1, 955, 190, 26, 26, Look::StackKey);
            return d;
        }
        // DELUXE.
        cab (125);
        label (d, "CABINET", 120, 251, 13, ink);
        d.box (382, 122, 0, 140, Look::Line, 0xff8a8a8a, 0, 2.0f);
        const auto dial = [&] (const char* id, float cx, const char* caption)
        {
            d.knob (p (id), cx, 188, 70, Look::NumberDial).scaleOf (10, "1|2|3|4|5|6|7|8|9|10", 300).lsize (12);
            label (d, caption, cx, 251, 13, ink);
        };
        dial ("out", 218, "OUTPUT GAIN");
        dial ("sag", 325, "SAG");
        d.knob (-1, 431, 168, 26, Look::Chrome).at (0.5f).sweep = 0.0f;
        label (d, "NORMAL", 432, 202, 12, ink);
        dial ("vol", 518, "VOLUME");
        dial ("treb", 627, "TREBLE");
        dial ("bass", 737, "BASS");
        label (d, "Deluxe", 862, 150, 34, ink, Look::Script);
        label (d, "Amplifier", 925, 188, 32, ink, Look::Script);
        label (d, "FOR MUSICAL INSTRUMENTS", 902, 251, 12, ink);
        d.lamp (1048, 138, 13, 0xffff8030).at (1.0f);
        return d;
    }

    /** BODYREZ (Fig. 167): the pedal's one knob. */
    Design bodyRez()
    {
        const auto p = fxParams ("BODY");
        auto d = figurePanel (0xff3a2a20, Texture::Smooth, 0.0f);
        d.box (30, 70, 1096, 336, Look::Wood, 0);
        d.box (205, 70, 742, 336, Look::Fill, 0xff111214, 0xff2a2a2a, 3.0f);
        for (float x : { 182.0f, 965.0f })
            d.box (x, 190, 22, 92, Look::Bevel, 0xffb8b8b8);
        for (float r : { 112.0f, 100.0f, 88.0f, 76.0f, 64.0f })
            d.path ("M " + juce::String (390 - r) + " 240 a " + juce::String (r) + " " + juce::String (r) + " 0 1 0 " + juce::String (2 * r) + " 0 a "
                        + juce::String (r) + " " + juce::String (r) + " 0 1 0 " + juce::String (-2 * r) + " 0",
                    0xffa88a60, 3.0f);
        label (d, "BODYREZ", 425, 240, 74, 0xfff0ecd8).kerning (0.02f);
        label (d, "acoustic pickup enhancer", 428, 295, 22, 0xffe8e0e8);
        d.knob (p ("body"), 712, 230, 150, Look::BarKnob);
        d.lamp (857, 142, 18, 0xffe01818).at (1.0f);
        label (d, "tc electronic", 715, 362, 20, 0xffeeeeee);
        return d;
    }

    //==========================================================================
    // Channel strips: one page (tab) per section, each the panel of the unit
    // it recreates (the FX menu's CH thumbnails are collages of exactly these
    // panels, tinted: EVEN = Even 88 gate | Even 88 EQ | comp/limiter...).

    /** One section of a strip: the unit's panel (a design already built) and
        how the strip's parameter ids name the unit's ("g_" + "thr"). */
    struct StripSection
    {
        const char* tab;
        const char* source;  // design key ("D:E88"), or nullptr: laid out from the parameters
        const char* prefix;  // the section's ids: prefix + the unit's id ("" for the EQ)
        juce::uint32 tint;   // its colour in the FX menu's collage
        std::vector<std::pair<const char*, const char*>> renames = {}; // unit id -> strip id (no prefix)
    };

    /** Copies a unit's elements onto page `page`, its parameters renamed to the strip's. */
    void addSection (Design& d, const Design& unit, const models::Model& unitSpec, const models::Model& strip, const StripSection& sec, int page)
    {
        const auto toStrip = [&] (int p) -> int
        {
            if (p < 0 || p >= (int) unitSpec.params.size())
                return -1;
            juce::String id = unitSpec.params[(size_t) p].id;
            for (auto [from, to] : sec.renames)
                if (id == from)
                    id = to;
            return strip.find (juce::String (sec.prefix) + id);
        };
        // The unit's plate: its colour, texture and screws.
        auto& plate = d.add (Type::Box, unit.plate.getX(), unit.plate.getY(), unit.plate.getWidth(), unit.plate.getHeight());
        plate.look = Look::Plate;
        plate.colour = unit.panel;
        plate.choice = (int) unit.texture;
        plate.fixed = unit.wear;
        plate.vertical = unit.screws;
        plate.page = page;
        for (const auto& src : unit.elements)
        {
            if (src.page > 0 || (src.type == Type::Key && src.look == Look::TabKey))
                continue; // the unit's other pages
            auto e = src;
            e.page = page;
            e.param = toStrip (src.param);
            for (auto& p : e.params)
                p = toStrip (p);
            d.elements.push_back (e);
        }
    }

    /** A strip from its sections. The strip's parameters no section panel has
        (the sections' ON switches, the dynamics' GAIN...) go in a row under the panel. */
    Design channelStrip (const std::map<juce::String, Design>& made, const char* token, std::vector<StripSection> sections,
                         const std::function<void (Unit&, int page)>& custom = {})
    {
        const auto& strip = models::model (models::Fx, models::findModel (models::Fx, token));
        Kit k;
        k.panel = 0xff16171a;
        k.ink = 0xffe8e8e8;
        k.knob = Look::BlackSkirt;
        k.key = Look::LedKey;
        k.lit = 0xff40e040;
        k.maxKnob = 40.0f;
        k.screws = false;
        Unit u (models::Fx, token, k);
        u.d.plate = { 30.0f, 56.0f, 1096.0f, 368.0f };
        u.d.wear = 0.0f;
        for (int i = 0; i < (int) sections.size(); ++i)
        {
            const auto& sec = sections[(size_t) i];
            auto& tab = u.d.key (-1, 40.0f + 150.0f * (float) i, 14, 140, 32, Look::TabKey, sec.tab);
            tab.choice = i;
            tab.colour2 = juce::Colour (sec.tint).withMultipliedSaturation (0.5f).brighter (0.6f);
            tab.size = 14;
            u.d.collage.push_back (juce::Colour (sec.tint));
            if (sec.source != nullptr)
            {
                const juce::String key (sec.source);
                const auto it = made.find (key);
                jassert (it != made.end());
                if (it == made.end())
                    continue;
                const auto table = key.startsWith ("D:") ? models::Dynamics : (key.startsWith ("E:") ? models::ChannelEq : models::Fx);
                const auto& unitSpec = models::model (table, models::findModel (table, key.fromFirstOccurrenceOf (":", false, false)));
                addSection (u.d, it->second, unitSpec, strip, sec, i);
            }
            else if (custom)
                custom (u, i);
        }
        // What no panel shows: the row under the plates, by section.
        std::set<int> reached;
        for (const auto& e : u.d.elements)
            if (e.param >= 0 && (e.type == Type::Knob || e.type == Type::Key || e.type == Type::Toggle || e.type == Type::Fader || e.type == Type::Lamp))
                reached.insert (e.param);
        for (int i = 0; i < (int) sections.size(); ++i)
        {
            const juce::String prefix (sections[(size_t) i].prefix);
            std::vector<int> rest;
            for (size_t p = 0; p < strip.params.size(); ++p)
            {
                const auto& id = strip.params[p].id;
                bool mine = prefix.isNotEmpty() && id.startsWith (prefix);
                if (prefix.isEmpty())
                {
                    mine = true;
                    for (const auto& other : sections)
                        mine = mine && (juce::String (other.prefix).isEmpty() || ! id.startsWith (other.prefix));
                }
                if (mine && reached.count ((int) p) == 0 && strip.params[p].min != strip.params[p].max)
                    rest.push_back ((int) p);
            }
            const float w = juce::jmin (1056.0f, 132.0f * (float) rest.size());
            u.controls (rest, { 50, 428, w, 50 }, 0, i, 0.6f);
        }
        return u.take();
    }

    Design evenChannel (const std::map<juce::String, Design>& made)
    {
        return channelStrip (made, "*EVEN*",
                             { { "GATE", "D:E88", "g_", 0xff8a2020 }, { "EQ", "F:E88", "", 0xff5a28a0 }, { "COMP / LIM", nullptr, "d_", 0xff207880 } },
                             [] (Unit& u, int page)
                             {
                                 // The Even channel's compressor / limiter: teal, a VU over the two sections.
                                 auto& plate = u.d.box (30, 56, 1096, 368, Look::Fill, 0xff1e5a60);
                                 plate.page = page;
                                 u.d.vu (440, 74, 276, 110, Look::VuWhite, "20|10|7|5|3|1|0", { 20, 10, 7, 5, 3, 1, 0 }, "GR").page = page;
                                 u.print ("COMPRESSOR / LIMITER", { 760, 380, 340, 30 }, 18, Look::SansLight, 0xffd8f0f0, J::centredRight).page = page;
                                 u.section ({ 50, 206, 500, 168 }, "LIMITER", page);
                                 u.controls ({ "d_lon", "d_lthr", "d_lrec", "d_lfast" }, { 56, 216, 488, 152 }, 0, page);
                                 u.section ({ 570, 206, 536, 168 }, "COMPRESSOR", page);
                                 u.controls ({ "d_con", "d_cthr", "d_ratio", "d_crec", "d_cfast" }, { 576, 216, 524, 152 }, 0, page);
                                 u.controls ({ "d_gain" }, { 50, 74, 160, 120 }, 0, page);
                             });
    }

    Design soulChannel (const std::map<juce::String, Design>& made)
    {
        return channelStrip (made, "*SOUL*",
                             { { "GATE", "D:9000G", "g_", 0xffa02870 }, { "EQ", "F:SOUL", "", 0xff2a7088 }, { "DYNAMICS", "D:9000C", "d_", 0xff5a8a20 } });
    }

    Design vintageChannel (const std::map<juce::String, Design>& made)
    {
        return channelStrip (made, "*VINTAGE*",
                             { { "76 LIMITER", "D:76LA", "d_", 0xff1e6a1e }, { "PULSAR EQ", "F:PULSAR", "", 0xffa02828 }, { "LEVELER", "D:LA", "l_", 0xff9090d8 } });
    }

    Design busChannel (const std::map<juce::String, Design>& made)
    {
        return channelStrip (made, "*BUS*",
                             { { "WARMTH", "D:WARM", "w_", 0xff6a5a20, { { "wmix", "mix" } } }, { "EQ", "F:E84", "", 0xff1e7a3a },
                               { "BUS COMP", "D:SBUS", "d_", 0xff8a2020 } });
    }

    Design masteringChannel (const std::map<juce::String, Design>& made)
    {
        return channelStrip (made, "*MASTER*",
                             { { "TAPE", "F:TAPE", "t_", 0xffc04040 }, { "EQ4", "F:MACH4", "", 0xff6a8a70 }, { "ENHANCER", "F:ENHANCE", "e_", 0xffc8c030 },
                               { "LIMITER", "F:LIMITER", "l_", 0xffb0c8f0 } });
    }

    //==========================================================================
    // Dynamics models

    Design pseLaCombo()
    {
        Kit k = lightRack (0xffb4ccd8);
        k.texture = Texture::Grunge;
        k.knob = Look::BlackSkirt;
        k.key = Look::RoundKey;
        k.lit = 0xff40c0ff;
        k.accent = 0xff1e2e5a;
        k.maxKnob = 92.0f;
        Unit u (models::Dynamics, "CMB", k);
        u.d.plate = { 30.0f, 70.0f, 1096.0f, 336.0f };
        u.print ("PSE / LA", { 70, 84, 300, 40 }, 34, Look::SansLight, 0xff1a1e22);
        u.print ("Source Extractor + Leveler", { 70, 124, 400, 26 }, 18, Look::SansLight, 0xff1e2e5a);
        u.section ({ 60, 166, 580, 226 }, "EXTRACTOR");
        u.controls ({ "thr", "depth", "fast", "peak" }, { 66, 176, 568, 210 });
        u.section ({ 660, 98, 446, 294 }, "LEVELER");
        u.controls ({ "ingain", "cpeak", "cmode" }, { 666, 110, 434, 276 });
        return u.take();
    }
} // namespace

int rackTileParam (const models::Model& m)
{
    // Read off a real WING's EFFECTS screen (3.1).
    static const std::pair<const char*, const char*> shown[] = { { "REVERSE", "rise" }, { "DEL/REV", "time" }, { "SPKMAN", "tiltf" },
                                                                 { "LIMITER", "sqz" },  { "ROTARY", "sw" },    { "TAPE", "drv" },
                                                                 { "PCORR", "amnt" } };
    for (auto [token, id] : shown)
        if (m.token == token)
            if (const int i = m.find (id); i >= 0)
                return i;
    static const char* const ids[] = { "dcy", "time", "dly", "spd", "fbase", "semi", "semi1", "oct1", "int", "gin", "hrm",
                                       "wid", "body", "drv", "gain", "drive", "vol", "pre", "g", "TRIM", "thr", "1-thr", "hpf", "mix" };
    for (auto* id : ids)
        if (const int i = m.find (id); i >= 0 && m.params[(size_t) i].isContinuous())
            return i;
    for (size_t i = 0; i < m.params.size(); ++i)
        if (m.params[i].isContinuous())
            return (int) i;
    return -1;
}

void addDeviceDesigns (std::map<juce::String, Design>& m)
{
    auto put = [&m] (const juce::String& key, Design d)
    {
        d.token = key.fromFirstOccurrenceOf (":", false, false);
        m[key] = std::move (d);
    };
    put ("F:V-ROOM", vintageRoom());
    put ("F:V-REV", vintageReverb());
    put ("F:SHIMMER", shimmerReverb());
    put ("F:SPRING", springReverb());
    put ("F:GATED", gatedReverb());
    put ("F:REVERSE", reverseReverb());
    put ("F:DEL/REV", delayReverb());
    put ("F:DIMCRS", dimension());
    put ("F:FLANGER", stereoFlanger());
    put ("F:TAP-DL", ultratap());
    put ("F:TAPE-DL", tapeEcho());
    put ("F:OILCAN", oilcanDelay());
    put ("F:BBD-DL", bbdDelay());
    put ("F:PITCH", stereoPitch());
    put ("F:D-PITCH", dualPitch());
    put ("F:VSS3", vss3());
    put ("F:GEQ", graphicEq());
    put ("F:PIA", piaEq (models::Fx));
    put ("E:PIA", piaEq (models::BusEq));
    put ("F:DOUBLE", doubleVocal());
    put ("F:PCORR", pitchFix());
    put ("F:LIMITER", precisionLimiter());
    put ("F:DE-S2", dualDeEsser());
    put ("F:ENHANCE", ultraEnhancer());
    put ("F:EXCITER", exciter());
    put ("F:P-BASS", psychoBass());
    put ("F:ROTARY", rotary());
    put ("F:PHASER", phaser());
    put ("F:PANNER", tremolo());
    put ("F:TAPE", tapeMachine());
    put ("F:MOOD", moodFilter());
    put ("F:SUB", subOctaver());
    put ("F:RACKAMP", rackAmp());
    put ("F:UKROCK", guitarAmp ("UKROCK"));
    put ("F:ANGEL", guitarAmp ("ANGEL"));
    put ("F:JAZZC", guitarAmp ("JAZZC"));
    put ("F:DELUXE", guitarAmp ("DELUXE"));
    put ("F:BODY", bodyRez());
    put ("F:C5-CMB", combinator());
    put ("F:SUB-M", subMonster());
    put ("F:V-IMG", velvetImager());
    put ("F:SPKMAN", speakerManager());
    put ("F:*EVEN*", evenChannel (m)); // after the units they are made of
    put ("F:*SOUL*", soulChannel (m)); // after the units they are made of
    put ("F:*VINTAGE*", vintageChannel (m)); // after the units they are made of
    put ("F:*BUS*", busChannel (m)); // after the units they are made of
    put ("F:*MASTER*", masteringChannel (m)); // after the units they are made of
    put ("D:CMB", pseLaCombo());
}

} // namespace livedaw::ui::fp
