#include "ui/FaceplateDesign.h"
#include "ui/ModelParams.h"

/* The front panels of the units the console's screen pictures haven't been
   measured for yet: each device's controls laid out from its parameter list
   (captions and value text as the console prints them) on a panel in the
   colours, knobs and print of the hardware it recreates, with the graph its
   page shows (delay taps, reverb decay, LFO, EQ response...). Same frame as
   FaceplateDesigns.cpp: 1156 x 479 px. */

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

    /** A pedalboard with one or more pedals: wooden floor. */
    void pedalboard (Design& d)
    {
        d.panel = juce::Colour (0xff5a321c);
        d.texture = Texture::Smooth;
        d.screws = false;
        d.plate = { 30.0f, 70.0f, 1096.0f, 336.0f };
        d.box (30, 70, 1096, 336, Look::Wood, 0);
    }

    /** A pedal's body with its footswitch and name. */
    void pedal (Design& d, R r, juce::uint32 colour, const juce::String& name, juce::uint32 ink)
    {
        d.box (r.getX(), r.getY(), r.getWidth(), r.getHeight(), Look::Fill, colour, juce::Colour (colour).darker (0.4f).getARGB(), 3.0f);
        d.text (name, r.getX(), r.getBottom() - 104.0f, r.getWidth(), 30.0f, 24.0f, ink).kerning (0.15f);
        d.knob (-1, r.getCentreX(), r.getBottom() - 46.0f, 42.0f, Look::Chrome).at (0.5f).sweep = 0.0f;
    }

    //==========================================================================
    // Premium devices

    Design vintageRoom()
    {
        Kit k;
        k.panel = 0xff2c2e33;
        k.texture = Texture::Brushed;
        k.wear = 0.5f;
        k.ink = 0xffd2d2d2;
        k.knob = Look::BlackDot;
        k.valueFace = Look::Dot;
        k.valueInk = 0xffff3a28;
        k.valueBg = 0xff0c0606;
        k.lit = 0xffff3a28;
        Unit u (models::Fx, "V-ROOM", k);
        u.print ("VINTAGE ROOM", { 76, 62, 400, 34 }, 26);
        u.print ("ROOM SIMULATOR", { 330, 66, 300, 28 }, 15, Look::Sans, 0xff9a9a9a);
        u.section ({ 50, 112, 540, 146 }, "ROOM");
        u.controls ({ "pdel", "size", "dcy", "dens" }, { 56, 120, 528, 134 });
        u.graph (GraphKind::Decay, { 610, 104, 496, 154 }, { "pdel", "dcy" });
        u.section ({ 50, 276, 336, 140 }, "REFLECTIONS");
        u.controls ({ "erlvl", "erl", "err" }, { 56, 284, 324, 128 });
        u.section ({ 396, 276, 420, 140 }, "COLOUR");
        u.controls ({ "lmult", "hmult", "lc", "hc" }, { 402, 284, 408, 128 });
        u.section ({ 826, 276, 280, 140 }, "OUTPUT");
        u.controls ({ "frz", "add", "lvl" }, { 832, 284, 268, 128 });
        return u.take();
    }

    Design vintageReverb()
    {
        Kit k;
        k.panel = 0xff3b3f46;
        k.texture = Texture::Rough;
        k.wear = 0.5f;
        k.ink = 0xffe0e0e0;
        k.valueFace = Look::Dot;
        k.valueInk = 0xffffb040;
        k.valueBg = 0xff100a04;
        k.accent = 0xffffb040;
        Unit u (models::Fx, "V-REV", k);
        u.print ("VINTAGE REVERB", { 76, 62, 400, 34 }, 26);
        u.print ("DIGITAL REVERBERATOR", { 360, 66, 300, 28 }, 15, Look::Sans, 0xff9a9a9a);
        u.d.box (50, 106, 700, 310, Look::Inset, 0xff2a2d33);
        u.faders ({ "pdel", "dcy", "lmult", "hmult", "mod", "lc", "hc" }, { 60, 114, 680, 296 }, 0xffd83020);
        u.graph (GraphKind::Decay, { 772, 104, 334, 172 }, { "pdel", "dcy" });
        u.controls ({ "out", "trans" }, { 772, 290, 334, 126 });
        return u.take();
    }

    Design shimmerReverb()
    {
        Kit k;
        k.panel = 0xff2a2150;
        k.texture = Texture::Rough;
        k.wear = 0.4f;
        k.ink = 0xffeae6ff;
        k.knob = Look::ColourDome;
        k.cap = 0xffb07aff;
        k.accent = 0xffc8a0ff;
        Unit u (models::Fx, "SHIMMER", k);
        u.print ("Shimmer", { 76, 58, 300, 44 }, 34, Look::Script, 0xffd8c8ff);
        u.section ({ 50, 112, 540, 146 }, "REVERB");
        u.controls ({ "pdel", "size", "dcy", "damp" }, { 56, 120, 528, 134 });
        u.graph (GraphKind::Decay, { 610, 104, 496, 154 }, { "pdel", "dcy" });
        u.section ({ 50, 276, 336, 140 }, "TONE");
        u.controls ({ "lc", "hc" }, { 56, 284, 324, 128 });
        u.section ({ 396, 276, 710, 140 }, "SHIMMER");
        u.controls ({ "shim", "shine" }, { 402, 284, 698, 128 }, 0, -1, 1.15f);
        return u.take();
    }

    Design springReverb()
    {
        Kit k;
        k.panel = 0xff201c1a;
        k.texture = Texture::Rough;
        k.wear = 0.8f;
        k.ink = 0xff1a1a1a;
        k.knob = Look::Chicken;
        k.maxKnob = 96.0f;
        k.screws = false;
        Unit u (models::Fx, "SPRING", k);
        // A chrome control plate in a tolex cabinet, the tank's springs behind a grille.
        u.d.box (50, 92, 640, 316, Look::Bevel, 0xffd4d4d0);
        u.print ("SPRING REVERB", { 72, 104, 400, 34 }, 24, Look::SerifItalic);
        u.controls ({ "dcy", "dens", "low", "high" }, { 64, 150, 612, 240 });
        u.graph (GraphKind::Decay, { 712, 104, 394, 196 }, { nullptr, "dcy" });
        u.d.box (712, 318, 394, 90, Look::Fill, 0xff8a7a5a, 0xff3a3020, 2.0f);
        for (int i = 0; i < 3; ++i)
            u.d.path ("M 730 " + juce::String (336 + 22 * i) + " C 780 " + juce::String (326 + 22 * i) + " 820 " + juce::String (346 + 22 * i)
                          + " 870 " + juce::String (336 + 22 * i) + " S 960 " + juce::String (326 + 22 * i) + " 1090 " + juce::String (336 + 22 * i),
                      0xff2a2418, 3.0f);
        return u.take();
    }

    Design dimension()
    {
        Kit k;
        k.panel = 0xff22242a;
        k.texture = Texture::Brushed;
        k.wear = 0.4f;
        k.ink = 0xffd8d8d8;
        k.key = Look::LedKey;
        k.lit = 0xffff4030;
        k.accent = 0xffff6040;
        Unit u (models::Fx, "DIMCRS", k);
        u.print ("DIMENSION", { 76, 62, 400, 40 }, 30);
        u.print ("STEREO CHORUS ENSEMBLE", { 60, 100, 400, 22 }, 14, Look::Sans, 0xff9a9a9a);
        u.section ({ 50, 150, 640, 250 }, "MODE");
        u.controls ({ "sw1", "sw2", "sw3", "sw4" }, { 60, 170, 620, 220 });
        u.section ({ 710, 150, 396, 250 }, "INPUT");
        u.controls ({ "in", "drysw" }, { 720, 170, 376, 220 });
        return u.take();
    }

    Design stereoFlanger()
    {
        Design d;
        pedalboard (d);
        const juce::uint32 ink = 0xff141414;
        Kit k;
        k.ink = ink;
        k.knob = Look::BlackSkirt;
        k.maxKnob = 52.0f;
        Unit u (models::Fx, "FLANGER", k);
        u.d = d;
        // Modulator, flanger and filter pedals.
        u.d.box (40, 78, 330, 320, Look::Fill, 0xff2a5ad0, 0xff1a3a90, 3.0f);
        u.print ("MODULATOR", { 40, 360, 330, 30 }, 22, Look::Sans, 0xffdfe6ff, J::centred);
        u.graph (GraphKind::Lfo, { 56, 92, 298, 120 }, { "spd", "depl", "depr", "phase" });
        u.kit.ink = 0xffeef2ff;
        u.controls ({ "spd", "phase" }, { 56, 222, 298, 130 });
        u.kit.ink = ink;
        pedal (u.d, { 386, 78, 420, 320 }, 0xffe8c020, "FLANGER", ink);
        u.controls ({ "dlyl", "dlyr", "depl", "depr" }, { 396, 86, 400, 104 });
        u.controls ({ "feed", "mix" }, { 446, 190, 300, 100 });
        pedal (u.d, { 822, 78, 294, 320 }, 0xffa070e0, "FILTER", ink);
        u.controls ({ "lc", "hc", "flc", "fhc" }, { 832, 86, 274, 200 }, 2);
        return u.take();
    }

    Design ultratap()
    {
        Kit k;
        k.panel = 0xff3a3d44;
        k.texture = Texture::Brushed;
        k.wear = 0.5f;
        k.ink = 0xffe0e2e6;
        k.knob = Look::BlackDome;
        k.valueFace = Look::Dot;
        k.valueInk = 0xff7cff9a;
        k.valueBg = 0xff07100a;
        k.accent = 0xff7cff9a;
        Unit u (models::Fx, "TAP-DL", k);
        u.print ("ULTRATAP", { 76, 62, 300, 34 }, 26);
        u.print ("MULTI-TAP DELAY", { 260, 66, 300, 28 }, 15, Look::Sans, 0xff9a9a9a);
        u.graph (GraphKind::Taps, { 610, 64, 496, 194 }, { "time", "rep", "slp", "pdel" }, 1);
        u.section ({ 50, 112, 540, 146 }, "TAPS");
        u.controls ({ "time", "rep", "slp", "fact" }, { 56, 120, 528, 134 });
        u.section ({ 50, 276, 446, 140 }, "SPACE");
        u.controls ({ "pdel", "mode", "wid", "diff" }, { 56, 284, 434, 128 });
        u.section ({ 506, 276, 300, 140 }, "FILTER");
        u.controls ({ "lc", "hc" }, { 512, 284, 288, 128 });
        return u.take();
    }

    Design tapeEcho()
    {
        Kit k;
        k.panel = 0xff1d2a22;
        k.texture = Texture::Rough;
        k.wear = 0.7f;
        k.ink = 0xffe8e8e0;
        k.knob = Look::BlackDot;
        k.maxKnob = 96.0f;
        Unit u (models::Fx, "TAPE-DL", k);
        u.d.box (40, 66, 1076, 50, Look::Fill, 0xffc8ccc8, 0xff606060, 2.0f);
        u.print ("TAPE ECHO", { 60, 70, 400, 42 }, 30, Look::Sans, 0xff1a2a20);
        u.controls ({ "time", "sust", "drv", "wf" }, { 50, 140, 640, 260 });
        u.graph (GraphKind::Taps, { 710, 132, 396, 268 }, { "time", "sust" });
        return u.take();
    }

    Design oilcanDelay()
    {
        Kit k = lightRack (0xffd6ccb0);
        k.texture = Texture::Grunge;
        k.knob = Look::BlackSkirt;
        k.maxKnob = 96.0f;
        k.screws = true;
        Unit u (models::Fx, "OILCAN", k);
        u.d.box (30, 56, 60, 368, Look::Wood, 0);
        u.d.box (1066, 56, 60, 368, Look::Wood, 0);
        u.print ("OIL CAN ECHO", { 110, 66, 500, 42 }, 30, Look::SerifBold, 0xff5a2a10);
        u.controls ({ "time", "sust", "wb", "tone" }, { 110, 130, 580, 270 });
        auto& g = u.graph (GraphKind::Taps, { 710, 124, 340, 276 }, { "time", "sust" });
        g.dbMin = 60.0f; // TIME 0..10: 60 .. 480 ms
        g.range = 480.0f;
        return u.take();
    }

    Design bbdDelay()
    {
        Design d;
        pedalboard (d);
        const juce::uint32 ink = 0xff141414;
        Kit k;
        k.ink = ink;
        k.knob = Look::BlackSkirt;
        k.maxKnob = 80.0f;
        Unit u (models::Fx, "BBD-DL", k);
        u.d = d;
        pedal (u.d, { 160, 78, 360, 320 }, 0xffe070b8, "ANALOG DELAY", ink);
        u.controls ({ "dly", "fb" }, { 180, 96, 320, 170 });
        u.d.box (580, 92, 500, 296, Look::Fill, 0xff1a1a1a, 0xff050505, 3.0f);
        auto& g = u.graph (GraphKind::Taps, { 600, 110, 460, 260 }, { "dly", "fb" });
        g.dbMin = 20.0f; // DELAY 0..100: 20 .. 600 ms
        g.range = 600.0f;
        return u.take();
    }

    Design stereoPitch()
    {
        Kit k;
        k.panel = 0xff25272d;
        k.texture = Texture::Brushed;
        k.ink = 0xffe0e0e0;
        k.knob = Look::BlackDome;
        k.valueFace = Look::Dot;
        k.valueInk = 0xffff4a30;
        k.valueBg = 0xff0c0505;
        Unit u (models::Fx, "PITCH", k);
        u.print ("STEREO PITCH", { 76, 62, 400, 34 }, 26);
        u.print ("HARMONIZER", { 330, 66, 300, 28 }, 15, Look::Sans, 0xff9a9a9a);
        u.section ({ 50, 112, 540, 300 }, "PITCH");
        u.controls ({ "semi", "cent" }, { 56, 124, 528, 280 }, 0, -1, 1.2f);
        u.section ({ 610, 112, 496, 300 }, "DELAY / FILTER / MIX");
        u.controls ({ "dly", "lc", "hc", "mix" }, { 616, 124, 484, 280 }, 2);
        return u.take();
    }

    Design dualPitch()
    {
        Kit k;
        k.panel = 0xffc9ccd1;
        k.texture = Texture::Brushed;
        k.ink = 0xff1a1a1a;
        k.line = 0x80303030;
        k.knob = Look::ChromeSmall;
        k.valueFace = Look::Dot;
        k.valueInk = 0xffff4a30;
        k.valueBg = 0xff140808;
        Unit u (models::Fx, "D-PITCH", k);
        u.print ("DUAL PITCH", { 76, 62, 400, 34 }, 26);
        u.section ({ 50, 112, 820, 140 }, "VOICE 1");
        u.controls ({ "semi1", "cent1", "dly1", "pan1", "lvl1" }, { 56, 120, 808, 128 });
        u.section ({ 50, 272, 820, 140 }, "VOICE 2");
        u.controls ({ "semi2", "cent2", "dly2", "pan2", "lvl2" }, { 56, 280, 808, 128 });
        u.section ({ 890, 112, 216, 300 }, "FILTER");
        u.controls ({ "lc", "hc" }, { 896, 124, 204, 280 }, 1);
        return u.take();
    }

    Design vss3()
    {
        Kit k;
        k.panel = 0xff1c1f24;
        k.texture = Texture::Brushed;
        k.wear = 0.4f;
        k.ink = 0xffdfe4ea;
        k.knob = Look::BlackDome;
        k.accent = 0xff6ac8ff;
        k.lit = 0xff6ac8ff;
        k.valueInk = 0xff9ad8ff;
        Unit u (models::Fx, "VSS3", k);
        u.print ("VSS3", { 76, 60, 200, 38 }, 30, Look::Sans);
        u.print ("REVERB", { 150, 66, 200, 28 }, 16, Look::Sans, 0xff9a9a9a);
        u.tabs ({ "EARLY REF", "REVERB", "DECAY", "EQ / MOD" }, 300, 64, 110, 28, 0xffb8c4d0);
        u.graph (GraphKind::Decay, { 800, 112, 306, 300 }, { "rvpdly", "dcy" });
        // Early reflections and the preset.
        u.controls ({ "preset", "load", "ertype", "ersize", "erpos" }, { 50, 112, 730, 150 }, 0, 0);
        u.controls ({ "erpdly", "erbal", "erlc", "ercol", "erlvl" }, { 50, 268, 730, 146 }, 0, 0);
        // The reverb.
        u.controls ({ "rvtype", "rvwid", "rvpdly", "dcy" }, { 50, 112, 730, 180 }, 0, 1);
        u.controls ({ "diff", "rvbal", "rvlvl" }, { 50, 296, 730, 118 }, 0, 1);
        // Decay per band.
        u.controls ({ "ldcy", "lmdcy", "hmdcy", "hdcy" }, { 50, 112, 730, 150 }, 0, 2);
        u.controls ({ "hsoft", "lxo", "mxo", "hxo" }, { 50, 268, 730, 146 }, 0, 2);
        // EQ and modulation.
        u.controls ({ "lshv", "lsdmp", "hcut" }, { 50, 112, 730, 150 }, 0, 3);
        u.controls ({ "mtype", "mrate", "mwid", "view" }, { 50, 268, 730, 146 }, 0, 3);
        return u.take();
    }

    //==========================================================================
    // Standard devices

    /** A graphic EQ: faders under the response. `o` = index of the first band. */
    Design graphicEq()
    {
        Kit k;
        k.panel = 0xff36435e;
        k.texture = Texture::Brushed;
        k.wear = 0.5f;
        k.ink = 0xffe6eaf2;
        k.accent = 0xffe8c040;
        Unit u (models::Fx, "GEQ", k);
        u.d.plate = { 20.0f, 46.0f, 1116.0f, 386.0f };
        u.print ("GRAPHIC EQ", { 44, 52, 300, 30 }, 22);
        u.graph (GraphKind::Response, { 300, 54, 690, 112 });
        u.controls ({ "type" }, { 1000, 50, 120, 120 });
        std::vector<int> bands;
        for (int i = 1; i <= 31; ++i)
            bands.push_back (i);
        u.faders (bands, { 36, 178, 1030, 240 }, 0xffe8c040, false);
        u.faders ({ "TRIM" }, { 1070, 178, 56, 240 }, 0xffd8d8d8, true);
        return u.take();
    }

    /** The 10-band slider EQ: the FX device (MIX first) or the bus EQ model. */
    Design piaEq (models::Table table)
    {
        Kit k;
        k.panel = 0xff4b5663;
        k.texture = Texture::Brushed;
        k.wear = 0.6f;
        k.ink = 0xffeef0f2;
        k.knob = Look::BlackSkirt;
        Unit u (table, "PIA", k);
        u.print ("PIA 560", { 76, 62, 300, 34 }, 26, Look::Sans);
        u.print ("GRAPHIC EQUALIZER", { 220, 66, 300, 28 }, 15, Look::Sans, 0xffb0b8c0);
        u.d.box (50, 106, 700, 310, Look::Inset, 0xff3a4450);
        u.faders ({ "31", "63", "125", "250", "500", "1k", "2k", "4k", "8k", "16k", "g" }, { 60, 112, 680, 300 }, 0xff151515);
        u.graph (GraphKind::Response, { 772, 104, 334, table == models::Fx ? 180.0f : 300.0f });
        if (table == models::Fx)
            u.controls ({ "mix" }, { 772, 296, 334, 120 });
        return u.take();
    }

    Design doubleVocal()
    {
        Kit k;
        k.panel = 0xff24272d;
        k.texture = Texture::Rough;
        k.wear = 0.4f;
        k.ink = 0xffe8e8e8;
        k.knob = Look::ColourDome;
        k.cap = 0xffff8a20;
        k.accent = 0xffff8a20;
        k.maxKnob = 110.0f;
        Unit u (models::Fx, "DOUBLE", k);
        u.print ("DOUBLE VOCAL", { 76, 62, 400, 34 }, 26);
        u.section ({ 50, 112, 300, 300 }, "MODE");
        u.controls ({ "mode" }, { 56, 120, 288, 284 });
        u.controls ({ "mix", "sprd" }, { 380, 112, 726, 300 });
        return u.take();
    }

    Design pitchFix()
    {
        Kit k;
        k.panel = 0xff1e2d31;
        k.texture = Texture::Rough;
        k.wear = 0.4f;
        k.ink = 0xffe0f0f0;
        k.knob = Look::ColourDome;
        k.cap = 0xff2bb3c0;
        Unit u (models::Fx, "PCORR", k);
        u.print ("PITCH FIX", { 76, 62, 300, 34 }, 26);
        u.section ({ 50, 112, 380, 300 }, "CORRECTION");
        u.controls ({ "spd", "amnt", "a4" }, { 56, 124, 368, 280 });
        // The scale: one octave of keys, each note on or off.
        u.section ({ 450, 112, 656, 300 }, "SCALE");
        const char* white[] = { "sw_c", "sw_d", "sw_e", "sw_f", "sw_g", "sw_a", "sw_b" };
        const char* names[] = { "C", "D", "E", "F", "G", "A", "B" };
        const float kw = 86.0f, x0 = 478.0f;
        for (int i = 0; i < 7; ++i)
            u.d.key (u.p (white[i]), x0 + kw * (float) i, 140, kw - 6.0f, 250, Look::LitSquare, names[i], 0xfff4f4f0).size = 18.0f;
        const std::pair<const char*, float> black[] = { { "sw_db", 1 }, { "sw_eb", 2 }, { "sw_gb", 4 }, { "sw_ab", 5 }, { "sw_bb", 6 } };
        for (auto [id, at] : black)
            u.d.key (u.p (id), x0 + kw * at - 30.0f, 140, 54, 150, Look::LitSquare, "", 0xff3fa0ff);
        return u.take();
    }

    Design precisionLimiter()
    {
        Kit k = lightRack (0xffd8dadc);
        k.knob = Look::ChromeBar;
        k.key = Look::LitSquare;
        k.lit = 0xff40d040;
        Unit u (models::Fx, "LIMITER", k);
        u.print ("PRECISION LIMITER", { 76, 62, 400, 34 }, 26, Look::Sans);
        u.section ({ 50, 112, 640, 146 }, "LEVEL");
        u.controls ({ "gin", "gout", "sqz", "knee" }, { 56, 120, 628, 134 });
        u.section ({ 50, 276, 640, 140 }, "TIMING");
        u.controls ({ "att", "rel", "again" }, { 56, 284, 628, 128 });
        u.graph (GraphKind::Transfer, { 720, 104, 290, 312 }, { "gin", "gout", "knee" }, 1);
        u.d.leds (1052, 116, 16, 290, "0|2|4|6|8|10|12|14|16|18", { 0.5f, 2, 4, 6, 8, 10, 12, 14, 16, 18 }, true, 0xffff3020).dots = false; // a bar meter
        return u.take();
    }

    Design dualDeEsser()
    {
        Kit k;
        k.panel = 0xff22324a;
        k.texture = Texture::Brushed;
        k.ink = 0xffe6ecf4;
        k.knob = Look::ColourCap;
        k.cap = 0xff2a2a2a;
        k.accent = 0xff40b0ff;
        Unit u (models::Fx, "DE-S2", k);
        u.print ("DUAL DE-ESSER", { 76, 62, 400, 34 }, 26);
        u.section ({ 50, 112, 620, 300 }, "THRESHOLDS");
        u.controls ({ "lo", "hi", "los", "his" }, { 56, 124, 608, 280 }, 2);
        u.section ({ 690, 112, 300, 300 }, "VOICE");
        u.controls ({ "gdr", "mode" }, { 696, 124, 288, 280 });
        u.d.leds (1040, 116, 16, 290, "0|2|4|6|8|10|12|15|20", { 0.5f, 2, 4, 6, 8, 10, 12, 15, 20 }, true, 0xffffc020).dots = false; // a bar meter
        return u.take();
    }

    Design ultraEnhancer()
    {
        Kit k;
        k.panel = 0xff1c2030;
        k.texture = Texture::Brushed;
        k.ink = 0xffe6e8f0;
        k.knob = Look::ColourDome;
        k.cap = 0xffff7a20;
        k.key = Look::LitSquare;
        k.lit = 0xffffa020;
        Unit u (models::Fx, "ENHANCE", k);
        u.print ("ULTRA ENHANCER", { 76, 62, 400, 34 }, 26);
        u.section ({ 50, 112, 1056, 140 }, "STEREO / MONO SPREAD");
        u.controls ({ "stlvl", "lmf", "mlvl", "st", "m" }, { 56, 120, 1044, 128 });
        u.section ({ 50, 272, 820, 140 }, "ENHANCE");
        u.controls ({ "bass", "bassf", "mid", "midq", "high", "highf" }, { 56, 280, 808, 128 });
        u.section ({ 890, 272, 216, 140 }, "OUTPUT");
        u.controls ({ "g", "solo" }, { 896, 280, 204, 128 });
        return u.take();
    }

    Design exciter()
    {
        Kit k;
        k.panel = 0xff161616;
        k.texture = Texture::Rough;
        k.ink = 0xffe8e8e8;
        k.knob = Look::Knurled;
        k.cap = 0xff2fae4a;
        k.key = Look::LitSquare;
        k.lit = 0xff2fae4a;
        Unit u (models::Fx, "EXCITER", k);
        u.print ("EXCITER", { 76, 62, 400, 34 }, 26);
        u.print ("AURAL", { 220, 66, 200, 28 }, 15, Look::Sans, 0xff2fae4a);
        u.section ({ 50, 112, 640, 300 }, "HARMONICS");
        u.controls ({ "tune", "peak", "zero", "tmb", "hrm", "mix" }, { 56, 124, 628, 280 }, 3);
        u.section ({ 710, 112, 396, 300 }, "OUTPUT");
        u.controls ({ "solo" }, { 716, 124, 384, 280 });
        return u.take();
    }

    Design psychoBass()
    {
        Kit k;
        k.panel = 0xff1b2340;
        k.texture = Texture::Brushed;
        k.ink = 0xffe6eaf6;
        k.knob = Look::BlackDome;
        k.key = Look::LitSquare;
        k.lit = 0xff4a8af0;
        k.maxKnob = 100.0f;
        Unit u (models::Fx, "P-BASS", k);
        u.print ("PSYCHO BASS", { 76, 62, 400, 34 }, 26);
        u.controls ({ "int", "bass", "xf", "solo" }, { 50, 120, 1056, 290 });
        return u.take();
    }

    Design rotary()
    {
        Kit k;
        k.panel = 0xff5a3420;
        k.texture = Texture::Grunge;
        k.wear = 0.5f;
        k.ink = 0xfff0e6d0;
        k.knob = Look::Chrome;
        k.accent = 0xffe0c080;
        Unit u (models::Fx, "ROTARY", k);
        u.d.box (30, 56, 60, 368, Look::Wood, 0);
        u.d.box (1066, 56, 60, 368, Look::Wood, 0);
        u.print ("ROTARY SPEAKER", { 110, 62, 400, 34 }, 26, Look::Serif);
        u.section ({ 110, 112, 210, 300 }, "SPEED");
        u.controls ({ "sw" }, { 116, 124, 198, 280 });
        u.section ({ 340, 112, 706, 140 }, "ROTORS");
        u.controls ({ "lo", "hi", "dac", "hac" }, { 346, 120, 694, 128 });
        u.section ({ 340, 272, 706, 140 }, "MICS");
        u.controls ({ "bal", "dist", "mix" }, { 346, 280, 694, 128 });
        return u.take();
    }

    Design phaser()
    {
        Kit k;
        k.panel = 0xff4e6a8c;
        k.texture = Texture::Brushed;
        k.ink = 0xfff2f4f8;
        k.knob = Look::WhiteDome;
        Unit u (models::Fx, "PHASER", k);
        u.print ("PHASER", { 76, 62, 400, 34 }, 26);
        u.graph (GraphKind::Lfo, { 760, 104, 346, 150 }, { "spd", "depth", nullptr, "phase", "wave" });
        u.section ({ 50, 112, 690, 146 }, "LFO");
        u.controls ({ "spd", "wave", "phase", "depth", "mix" }, { 56, 120, 678, 134 });
        u.section ({ 50, 276, 446, 140 }, "ENVELOPE");
        u.controls ({ "emod", "att", "hld", "rel" }, { 56, 284, 434, 128 });
        u.section ({ 506, 276, 600, 140 }, "STAGES");
        u.controls ({ "range", "stg", "reso" }, { 512, 284, 588, 128 });
        return u.take();
    }

    Design tremolo()
    {
        Kit k = lightRack (0xff8a8c90);
        k.knob = Look::Chrome;
        k.ink = 0xfff4f4f4;
        k.line = 0x80ffffff;
        Unit u (models::Fx, "PANNER", k);
        u.print ("TREMOLO / PANNER", { 76, 62, 400, 34 }, 26);
        u.graph (GraphKind::Lfo, { 640, 104, 466, 150 }, { "spd", "depth", nullptr, "phase", "wave" });
        u.section ({ 50, 112, 570, 146 }, "LFO");
        u.controls ({ "spd", "phase", "wave", "depth" }, { 56, 120, 558, 134 });
        u.section ({ 50, 276, 1056, 140 }, "ENVELOPE");
        u.controls ({ "att", "hld", "rel", "espd", "edep" }, { 56, 284, 1044, 128 });
        return u.take();
    }

    Design tapeMachine()
    {
        Kit k = lightRack (0xffd0d0cc);
        k.knob = Look::ChromeSmall;
        k.key = Look::LitSquare;
        k.lit = 0xffffb020;
        k.maxKnob = 96.0f;
        Unit u (models::Fx, "TAPE", k);
        u.print ("TAPE MACHINE", { 76, 62, 400, 34 }, 26, Look::Sans);
        u.controls ({ "drv", "spd", "out" }, { 50, 112, 640, 180 });
        u.controls ({ "low", "hi" }, { 50, 300, 640, 112 });
        u.d.box (720, 112, 386, 300, Look::Fill, 0xff1a1a1a, 0xff050505, 3.0f);
        u.d.vu (750, 140, 326, 200, Look::VuCream, "-20|-10|-7|-5|-3|0|+3", { 20, 10, 7, 5, 3, 0, -3 }, "VU").vuZero = 0.75f;
        u.print ("LEVEL", { 720, 360, 386, 30 }, 15, Look::Sans, 0xffd0d0d0, J::centred);
        return u.take();
    }

    Design moodFilter()
    {
        Kit k;
        k.panel = 0xff141414;
        k.texture = Texture::Rough;
        k.ink = 0xffe8e8e8;
        k.knob = Look::BlackSkirt;
        k.accent = 0xff6aa8ff;
        Unit u (models::Fx, "MOOD", k);
        u.d.box (30, 56, 50, 368, Look::Wood, 0);
        u.d.box (1076, 56, 50, 368, Look::Wood, 0);
        u.print ("MOOD FILTER", { 100, 62, 400, 34 }, 26);
        u.graph (GraphKind::Filter, { 690, 64, 366, 186 }, { "fbase", "filt", "slope", "reso" });
        u.section ({ 100, 112, 570, 146 }, "FILTER");
        u.controls ({ "fbase", "filt", "slope", "reso", "drv" }, { 106, 120, 558, 134 });
        u.section ({ 100, 276, 380, 140 }, "ENVELOPE");
        u.controls ({ "env", "att", "hld", "rel" }, { 106, 284, 368, 128 });
        u.section ({ 490, 276, 476, 140 }, "LFO");
        u.controls ({ "lfo", "spd", "phase", "wave" }, { 496, 284, 464, 128 });
        u.controls ({ "mix" }, { 976, 276, 90, 140 });
        return u.take();
    }

    Design subOctaver()
    {
        Design d;
        pedalboard (d);
        const juce::uint32 ink = 0xff141414;
        Kit k;
        k.ink = ink;
        k.knob = Look::BlackSkirt;
        k.accent = 0xff3a6ad8;
        k.maxKnob = 70.0f;
        Unit u (models::Fx, "SUB", k);
        u.d = d;
        pedal (u.d, { 360, 78, 436, 320 }, 0xff4a7ab8, "OCTAVE", ink);
        u.controls ({ "oct1", "oct2" }, { 380, 92, 260, 180 });
        u.controls ({ "rng" }, { 640, 88, 140, 196 });
        return u.take();
    }

    /** A guitar amp head: control panel under a tolex top with the logo. */
    Design ampHead (const char* token, const juce::String& logo, juce::uint32 panel, juce::uint32 ink, Look knob, juce::uint32 cap,
                    std::initializer_list<const char*> knobs, std::initializer_list<const char*> switches, Look logoFace = Look::Script,
                    juce::uint32 logoColour = 0xfff0f0f0)
    {
        Kit k;
        k.panel = 0xff141414;
        k.texture = Texture::Rough;
        k.wear = 0.9f;
        k.ink = ink;
        k.knob = knob;
        k.cap = cap;
        k.key = Look::LitSquare;
        k.lit = 0xffff3020;
        k.maxKnob = 72.0f;
        k.screws = false;
        Unit u (models::Fx, token, k);
        u.print (logo, { 60, 70, 1036, 84 }, 56, logoFace, logoColour, J::centred);
        u.d.box (40, 196, 1076, 210, Look::Bevel, panel);
        const int n = (int) knobs.size();
        const float knobW = switches.size() > 0 ? 820.0f : 1056.0f;
        u.controls (knobs, { 50, 214, knobW, 180 });
        if (switches.size() > 0)
            u.controls (switches, { 50 + knobW, 214, 1056 - knobW, 180 }, (int) switches.size() > 2 ? 2 : 1);
        juce::ignoreUnused (n);
        return u.take();
    }

    Design bodyRez()
    {
        Kit k;
        k.panel = 0xff3a2a20;
        k.texture = Texture::Grunge;
        k.ink = 0xfff0e0c8;
        k.knob = Look::Knurled;
        k.cap = 0xffd0a060;
        k.maxKnob = 150.0f;
        Unit u (models::Fx, "BODY", k);
        u.d.box (30, 56, 300, 368, Look::Wood, 0);
        u.d.box (826, 56, 300, 368, Look::Wood, 0);
        u.print ("BODYREZ", { 350, 62, 456, 40 }, 28, Look::Sans, 0, J::centred);
        u.print ("ACOUSTIC PICKUP ENHANCER", { 350, 100, 456, 22 }, 13, Look::Sans, 0xffb8a890, J::centred);
        u.controls ({ "body" }, { 350, 130, 456, 280 });
        return u.take();
    }

    Design combinator()
    {
        Kit k = lightRack (0xff9a9ca0);
        k.knob = Look::ColourDome;
        k.cap = 0xff7a4ad0;
        k.accent = 0xff9a6af0;
        k.key = Look::LitSquare;
        k.lit = 0xff9a6af0;
        k.maxKnob = 60.0f;
        Unit u (models::Fx, "C5-CMB", k);
        u.print ("COMBINATOR", { 76, 62, 300, 34 }, 26, Look::Sans);
        u.tabs ({ "MAIN", "BANDS", "BAND SET" }, 330, 64, 104, 28);
        // The five bands: threshold offsets over gains.
        u.graph (GraphKind::Levels, { 820, 112, 286, 144 }, { "thr_1", "thr_2", "thr_3", "thr_4", "thr_5" });
        u.graph (GraphKind::Levels, { 820, 268, 286, 144 }, { "gain_1", "gain_2", "gain_3", "gain_4", "gain_5" });
        u.controls ({ "thr", "gain", "ratio", "slope", "bandsel" }, { 50, 112, 750, 150 }, 0, 0);
        u.controls ({ "att", "rel", "arel", "sbc", "sbcon", "mix" }, { 50, 270, 750, 144 }, 0, 0);
        u.controls ({ "thr_1", "thr_2", "thr_3", "thr_4", "thr_5" }, { 50, 112, 750, 150 }, 0, 1);
        u.controls ({ "gain_1", "gain_2", "gain_3", "gain_4", "gain_5" }, { 50, 270, 750, 144 }, 0, 1);
        u.controls ({ "byp_1", "byp_2", "byp_3", "byp_4", "byp_5" }, { 50, 112, 750, 150 }, 0, 2);
        u.controls ({ "width_1", "width_2", "width_3", "width_4", "width_5" }, { 50, 270, 750, 144 }, 0, 2);
        return u.take();
    }

    Design subMonster()
    {
        Kit k;
        k.panel = 0xff5d6e86;
        k.texture = Texture::Brushed;
        k.ink = 0xfff0f2f6;
        k.knob = Look::WhiteDome;
        Unit u (models::Fx, "SUB-M", k);
        u.print ("SUB MONSTER", { 76, 62, 400, 34 }, 26);
        u.section ({ 50, 112, 260, 300 }, "SUB");
        u.controls ({ "freq", "mix" }, { 56, 124, 248, 280 }, 1);
        u.d.box (330, 106, 500, 310, Look::Inset, 0xff4a5a70);
        u.faders ({ "bd1", "bd2", "bd3", "bd4", "bd5" }, { 340, 112, 480, 300 }, 0xff1a2a40);
        u.graph (GraphKind::Levels, { 850, 112, 256, 300 }, { "bd1", "bd2", "bd3", "bd4", "bd5" });
        return u.take();
    }

    Design velvetImager()
    {
        Kit k;
        k.panel = 0xff5a1a26;
        k.texture = Texture::Brushed;
        k.ink = 0xfff6e8ec;
        k.knob = Look::BlackDome;
        k.accent = 0xffff6080;
        k.key = Look::LitSquare;
        k.lit = 0xffff4060;
        k.maxKnob = 96.0f;
        Unit u (models::Fx, "V-IMG", k);
        u.print ("VELVET IMAGER", { 76, 62, 400, 34 }, 26);
        u.controls ({ "wid", "st", "g" }, { 50, 112, 700, 300 });
        u.section ({ 770, 112, 336, 300 }, "MODE");
        u.controls ({ "mode", "deep" }, { 776, 124, 324, 280 });
        return u.take();
    }

    Design speakerManager()
    {
        Kit k;
        k.panel = 0xff1d2b48;
        k.texture = Texture::Brushed;
        k.ink = 0xffe6ecf8;
        k.knob = Look::BlackDome;
        k.accent = 0xff40a0ff;
        k.key = Look::LitSquare;
        k.lit = 0xff40a0ff;
        k.maxKnob = 64.0f;
        Unit u (models::Fx, "SPKMAN", k);
        u.print ("SPEAKER MANAGER", { 76, 62, 300, 34 }, 24);
        u.tabs ({ "X-OVER", "ALIGN", "DYN EQ", "LIMITER" }, 330, 64, 104, 28);
        u.graph (GraphKind::Response, { 790, 112, 316, 300 });
        u.controls ({ "hpf", "hptype", "lpf", "lptype" }, { 50, 112, 720, 150 }, 0, 0);
        u.controls ({ "tiltf", "tiltg" }, { 50, 270, 720, 144 }, 0, 0);
        u.controls ({ "phase", "invert", "dist", "pos" }, { 50, 112, 720, 300 }, 2, 1);
        u.controls ({ "dyneq", "deqthr", "deqratio", "deqatt", "deqrel" }, { 50, 112, 720, 150 }, 0, 2);
        u.controls ({ "deqfilt", "deqg", "deqf", "deqq", "deqmode" }, { 50, 270, 720, 144 }, 0, 2);
        u.controls ({ "lim", "limthr", "limrms", "limrel" }, { 50, 112, 720, 300 }, 2, 3);
        return u.take();
    }

    /** A channel strip: a page per section, the EQ with its response, the dynamics with their curve. */
    struct StripPage
    {
        juce::String name;
        std::vector<int> params;
        GraphKind graph = GraphKind::Levels;
        std::vector<int> graphParams;
        int variant = -1;
        bool hasGraph = false;
    };

    StripPage stripPage (const juce::String& name, std::vector<int> params) { return { name, std::move (params), GraphKind::Levels, {}, -1, false }; }

    StripPage stripPage (const juce::String& name, std::vector<int> params, GraphKind graph, std::vector<int> graphParams = {}, int variant = -1)
    {
        return { name, std::move (params), graph, std::move (graphParams), variant, true };
    }

    Design channelStrip (Unit& u, const juce::String& title, std::vector<StripPage> pages)
    {
        u.print (title, { 76, 62, 300, 34 }, 24);
        juce::StringArray names;
        for (auto& pg : pages)
            names.add (pg.name);
        u.tabs (names, 330, 64, 116, 28);
        for (int i = 0; i < (int) pages.size(); ++i)
        {
            const auto& pg = pages[(size_t) i];
            const float width = pg.hasGraph ? 740.0f : 1056.0f;
            const int n = (int) pg.params.size();
            const int cols = n <= 6 ? n : (n + 1) / 2;
            u.controls (pg.params, { 50, 112, width, 300 }, cols, i);
            if (pg.hasGraph)
                u.graph (pg.graph, { 810, 112, 296, 300 }, pg.graphParams, pg.variant, i);
        }
        return u.take();
    }

    Design evenChannel()
    {
        Kit k;
        k.panel = 0xff2e3d36;
        k.texture = Texture::Grunge;
        k.ink = 0xffeef2ee;
        k.knob = Look::ColourCap;
        k.cap = 0xff3fb8e0;
        k.key = Look::LedKey;
        k.lit = 0xff40e040;
        k.maxKnob = 60.0f;
        Unit u (models::Fx, "*EVEN*", k);
        return channelStrip (u, "EVEN CHANNEL",
                             { stripPage ("GATE", u.withPrefix ("g_")),
                               stripPage ("EQ", u.withPrefix ("", { "g_", "d_" }), GraphKind::Response),
                               stripPage ("COMP / LIM", u.withPrefix ("d_"), GraphKind::Transfer, u.ids ({ "d_cthr", "d_ratio" }), 0) });
    }

    Design soulChannel()
    {
        Kit k;
        k.panel = 0xff4a4c50;
        k.texture = Texture::Brushed;
        k.ink = 0xfff2f2f2;
        k.knob = Look::BlackSkirt;
        k.key = Look::LedKey;
        k.lit = 0xffff3020;
        k.maxKnob = 60.0f;
        Unit u (models::Fx, "*SOUL*", k);
        return channelStrip (u, "SOUL CHANNEL",
                             { stripPage ("GATE", u.withPrefix ("g_")),
                               stripPage ("EQ", u.withPrefix ("", { "g_", "d_" }), GraphKind::Response),
                               stripPage ("DYNAMICS", u.withPrefix ("d_"), GraphKind::Transfer, u.ids ({ "d_thr", "d_ratio" }), 0) });
    }

    Design vintageChannel()
    {
        Kit k;
        k.panel = 0xff1a1a1a;
        k.texture = Texture::Brushed;
        k.ink = 0xffe8e4d8;
        k.knob = Look::ChromeSmall;
        k.key = Look::LedKey;
        k.lit = 0xffffb020;
        k.accent = 0xffd0b060;
        k.maxKnob = 60.0f;
        Unit u (models::Fx, "*VINTAGE*", k);
        return channelStrip (u, "VINTAGE CHANNEL",
                             { stripPage ("76 LIMITER", u.withPrefix ("d_")),
                               stripPage ("PULSAR EQ", u.withPrefix ("", { "d_", "l_" }), GraphKind::Response),
                               stripPage ("LEVELER", u.withPrefix ("l_")) });
    }

    Design busChannel()
    {
        Kit k;
        k.panel = 0xff202226;
        k.texture = Texture::Brushed;
        k.ink = 0xffe8ecf2;
        k.knob = Look::ColourCap;
        k.cap = 0xff3fa0ff;
        k.key = Look::LedKey;
        k.lit = 0xff40d0ff;
        k.accent = 0xff3fa0ff;
        k.maxKnob = 60.0f;
        Unit u (models::Fx, "*BUS*", k);
        return channelStrip (u, "BUS CHANNEL",
                             { stripPage ("WARMTH", u.withPrefix ("w_")),
                               stripPage ("EQ", u.withPrefix ("", { "w_", "d_" }), GraphKind::Response),
                               stripPage ("BUS COMP", u.withPrefix ("d_"), GraphKind::Transfer, u.ids ({ "d_thr", "d_ratio" }), 0) });
    }

    Design masteringChannel()
    {
        Kit k = lightRack (0xffcdd0d4);
        k.knob = Look::ChromeSmall;
        k.key = Look::LedKey;
        k.lit = 0xff30c040;
        k.accent = 0xff2a6ad8;
        k.maxKnob = 58.0f;
        Unit u (models::Fx, "*MASTER*", k);
        return channelStrip (u, "MASTERING",
                             { stripPage ("TAPE", u.withPrefix ("t_")),
                               stripPage ("EQ4", u.withPrefix ("", { "t_", "e_", "l_" }), GraphKind::Response),
                               stripPage ("ENHANCER", u.withPrefix ("e_")),
                               stripPage ("LIMITER", u.withPrefix ("l_"), GraphKind::Transfer, u.ids ({ "l_gin", "l_gout", "l_knee" }), 1) });
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
    put ("F:RACKAMP", ampHead ("RACKAMP", "RACK AMP", 0xff2a2a2e, 0xffe8e8e8, Look::ColourCap, 0xffe8c020,
                               { "pre", "buzz", "punch", "crunch", "drive", "leq", "heq", "out" }, { "cab" }, Look::Sans, 0xffe8c020));
    put ("F:UKROCK", ampHead ("UKROCK", "UK Rock", 0xffc9a64a, 0xff1a1a1a, Look::Knurled, 0xffd8b860,
                              { "gain", "bass", "mid", "treb", "pres", "mstr", "out", "sag" }, { "cab" }));
    put ("F:ANGEL", ampHead ("ANGEL", "Angel", 0xff1e1e1e, 0xffe8e8e8, Look::BlackSkirt, 0,
                             { "gain", "bass", "mid", "treb", "pres", "mstr", "out", "sag" }, { "cab", "midb", "bri", "bot" }, Look::Script, 0xffe02020));
    put ("F:JAZZC", ampHead ("JAZZC", "JAZZ CLEAN", 0xffb8babc, 0xff1a1a1a, Look::BlackDot, 0,
                             { "vol", "bass", "mid", "treb", "out" }, { "bri", "cab" }, Look::Sans, 0xffd8d8d8));
    put ("F:DELUXE", ampHead ("DELUXE", "Deluxe", 0xff111111, 0xffeeeeee, Look::BlackSkirt, 0,
                              { "vol", "bass", "treb", "out", "sag" }, { "cab" }, Look::Script, 0xffd8d8d8));
    put ("F:BODY", bodyRez());
    put ("F:C5-CMB", combinator());
    put ("F:SUB-M", subMonster());
    put ("F:V-IMG", velvetImager());
    put ("F:SPKMAN", speakerManager());
    put ("F:*EVEN*", evenChannel());
    put ("F:*SOUL*", soulChannel());
    put ("F:*VINTAGE*", vintageChannel());
    put ("F:*BUS*", busChannel());
    put ("F:*MASTER*", masteringChannel());
    put ("D:CMB", pseLaCombo());
}

} // namespace livedaw::ui::fp
