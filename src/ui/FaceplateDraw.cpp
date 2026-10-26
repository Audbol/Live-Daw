#include "ui/FaceplateDesign.h"
#include "ui/LookAndFeel.h"
#include "ui/ModelParams.h"

#include <cmath>
#include <functional>
#include <map>

namespace livedaw::ui::fp
{

namespace
{
    using juce::Colour;
    using juce::Point;
    using juce::Rectangle;

    constexpr float kPi = juce::MathConstants<float>::pi;
    constexpr float kTwoPi = juce::MathConstants<float>::twoPi;

    float rad (float degrees) { return degrees * kPi / 180.0f; }

    /** The point at `r` from `c` in direction `a` (radians, 0 = up, clockwise). */
    Point<float> polar (Point<float> c, float r, float a) { return { c.x + std::sin (a) * r, c.y - std::cos (a) * r }; }

    Rectangle<float> circle (Point<float> c, float r) { return { c.x - r, c.y - r, r * 2.0f, r * 2.0f }; }

    /** Look spans knobs, keys, meters, boxes and type faces; each painter
        switches on the index and handles its own range. */
    constexpr int lookIndex (Look l) { return static_cast<int> (l); }

    juce::Font face (Look look, float size)
    {
        size *= 1.18f; // the panel print reads larger than the screen face
        switch (lookIndex (look))
        {
            case lookIndex (Look::SansLight): return silkFont (size, false);
            case lookIndex (Look::Serif): return juce::Font (juce::FontOptions (juce::Font::getDefaultSerifFontName(), size, juce::Font::plain));
            case lookIndex (Look::SerifBold): return juce::Font (juce::FontOptions (juce::Font::getDefaultSerifFontName(), size, juce::Font::bold));
            case lookIndex (Look::SerifItalic): return juce::Font (juce::FontOptions (juce::Font::getDefaultSerifFontName(), size, juce::Font::italic));
            case lookIndex (Look::Script): return juce::Font (juce::FontOptions (juce::Font::getDefaultSerifFontName(), size, juce::Font::bold | juce::Font::italic));
            case lookIndex (Look::Segment): return juce::Font (juce::FontOptions (juce::Font::getDefaultMonospacedFontName(), size, juce::Font::bold));
            case lookIndex (Look::Dot): return juce::Font (juce::FontOptions (juce::Font::getDefaultMonospacedFontName(), size, juce::Font::plain));
            default: return silkFont (size, true);
        }
    }

    //==========================================================================
    // Textures: shared overlays drawn on top of a panel's colour.

    juce::Image makeBackdrop (float scale)
    {
        const int w = (int) kFrameW, h = (int) kFrameH;
        juce::Image img (juce::Image::RGB, juce::jmax (1, juce::roundToInt ((float) w * scale)), juce::jmax (1, juce::roundToInt ((float) h * scale)), true);
        juce::Graphics g (img);
        g.addTransform (juce::AffineTransform::scale (scale));
        juce::ColourGradient bg (Colour (0xff1c1c1c), 0, 0, Colour (0xff090909), 0, (float) h, false);
        bg.addColour (0.5, Colour (0xff141414));
        g.setGradientFill (bg);
        g.fillAll();
        juce::Random rnd (77);
        // Mottled stains.
        for (int i = 0; i < 260; ++i)
        {
            const float x = rnd.nextFloat() * (float) w, y = rnd.nextFloat() * (float) h, r = 4.0f + rnd.nextFloat() * 40.0f;
            g.setColour ((rnd.nextBool() ? juce::Colours::white : juce::Colours::black).withAlpha (0.004f + rnd.nextFloat() * 0.008f));
            g.fillEllipse (x - r, y - r, r * 2.2f, r * 1.4f);
        }
        // Scratches: thin light strokes, a few long and curved.
        for (int i = 0; i < 180; ++i)
        {
            const float x = rnd.nextFloat() * (float) w, y = rnd.nextFloat() * (float) h;
            const float len = 6.0f + std::pow (rnd.nextFloat(), 3.0f) * 160.0f, a = rnd.nextFloat() * kTwoPi;
            juce::Path p;
            p.startNewSubPath (x, y);
            const auto e = polar ({ x, y }, len, a), m = polar ({ x, y }, len * 0.5f, a + 0.15f * (rnd.nextFloat() - 0.5f));
            p.quadraticTo (m, e);
            g.setColour (juce::Colours::white.withAlpha (0.03f + rnd.nextFloat() * 0.09f));
            g.strokePath (p, juce::PathStrokeType (0.6f + rnd.nextFloat() * 0.6f));
        }
        return img;
    }

    /** Bitmaps are rendered at the pixel scale they are shown at (in quarter
        steps), so they stay sharp on large screens; a few scales are kept. */
    template <typename Make>
    const juce::Image& cachedAtScale (std::map<int, juce::Image>& cache, float scale, Make make)
    {
        const int key = juce::jlimit (1, 24, juce::roundToInt (scale * 4.0f));
        if (auto it = cache.find (key); it != cache.end())
            return it->second;
        if (cache.size() > 3)
            cache.clear();
        return cache[key] = make ((float) key / 4.0f);
    }

    const juce::Image& backdropImage (float scale)
    {
        static std::map<int, juce::Image> cache;
        return cachedAtScale (cache, scale, [] (float s) { return makeBackdrop (s); });
    }

    /** The frame's corner screws and FX mark (drawn as vectors). */
    void drawFrameMarks (juce::Graphics& g)
    {
        // Corner screws of the frame.
        for (auto c : { Point<float> (26, 30), Point<float> (1128, 30), Point<float> (26, 452), Point<float> (1128, 452) })
        {
            g.setColour (juce::Colours::black.withAlpha (0.6f));
            g.fillEllipse (circle (c.translated (1, 2), 10));
            juce::ColourGradient sg (Colour (0xff4a4a4a), c.x - 6, c.y - 6, Colour (0xff141414), c.x + 6, c.y + 8, false);
            g.setGradientFill (sg);
            g.fillEllipse (circle (c, 9));
            g.setColour (Colour (0xff0a0a0a));
            g.drawLine (c.x - 6, c.y + 2, c.x + 6, c.y - 2, 2.0f);
            g.drawLine (c.x - 2, c.y - 6, c.x + 2, c.y + 6, 2.0f);
        }
        // The console's FX mark.
        g.setFont (juce::Font (juce::FontOptions (juce::Font::getDefaultSerifFontName(), 36.0f, juce::Font::bold)));
        g.setColour (Colour (0xff3a3a3a));
        g.drawText ("FX", Rectangle<float> (118, 10, 60, 40), juce::Justification::centred);
        g.setColour (Colour (0xff222222));
        g.drawText ("FX", Rectangle<float> (120, 12, 60, 40), juce::Justification::centred);
    }

    /** An overlay (white and black marks with alpha) for a texture, plate sized. */
    juce::Image makeTexture (Texture t, float scale)
    {
        const int w = 1100, h = 340;
        juce::Image img (juce::Image::ARGB, juce::jmax (1, juce::roundToInt ((float) w * scale)), juce::jmax (1, juce::roundToInt ((float) h * scale)), true);
        juce::Graphics g (img);
        g.addTransform (juce::AffineTransform::scale (scale));
        juce::Random rnd (t == Texture::Brushed ? 11 : (t == Texture::Rough ? 23 : 5));
        if (t == Texture::Brushed)
        {
            for (int y = 0; y < h; ++y)
            {
                float x = 0.0f;
                while (x < (float) w)
                {
                    const float len = 20.0f + rnd.nextFloat() * 300.0f;
                    g.setColour ((rnd.nextBool() ? juce::Colours::white : juce::Colours::black).withAlpha (0.03f + rnd.nextFloat() * 0.06f));
                    g.drawHorizontalLine (y, x, x + len);
                    x += len;
                }
            }
            return img;
        }
        // Mottling.
        for (int i = 0; i < (t == Texture::Rough ? 3000 : 400); ++i)
        {
            const float x = rnd.nextFloat() * (float) w, y = rnd.nextFloat() * (float) h;
            const float r = t == Texture::Rough ? 0.6f + rnd.nextFloat() * 1.4f : 6.0f + rnd.nextFloat() * 50.0f;
            g.setColour ((rnd.nextBool() ? juce::Colours::white : juce::Colours::black).withAlpha (t == Texture::Rough ? 0.06f : 0.008f));
            g.fillEllipse (x - r, y - r, r * 2.0f, r * 1.6f);
        }
        if (t == Texture::Smooth)
            return img;
        // Wear: fine scratches, mostly near the edges.
        for (int i = 0; i < 140; ++i)
        {
            float x = rnd.nextFloat() * (float) w, y = rnd.nextFloat() * (float) h;
            if (rnd.nextFloat() < 0.6f)
                x = rnd.nextBool() ? rnd.nextFloat() * 160.0f : (float) w - rnd.nextFloat() * 160.0f;
            const float len = 4.0f + std::pow (rnd.nextFloat(), 2.0f) * 90.0f, a = rnd.nextFloat() * kTwoPi;
            const auto e = polar ({ x, y }, len, a);
            g.setColour ((rnd.nextFloat() < 0.7f ? juce::Colours::white : juce::Colours::black).withAlpha (0.05f + rnd.nextFloat() * 0.12f));
            g.drawLine (x, y, e.x, e.y, 0.5f + rnd.nextFloat() * 0.6f);
        }
        return img;
    }

    const juce::Image& textureImage (Texture t, float scale)
    {
        static std::map<int, juce::Image> caches[4];
        return cachedAtScale (caches[juce::jlimit (0, 3, static_cast<int> (t))], scale, [t] (float s) { return makeTexture (t, s); });
    }

    void drawScrew (juce::Graphics& g, Point<float> c, float r, Colour panel)
    {
        g.setColour (juce::Colours::black.withAlpha (0.45f));
        g.fillEllipse (circle (c.translated (0.0f, r * 0.25f), r * 1.12f));
        const bool light = panel.getPerceivedBrightness() > 0.5f;
        juce::ColourGradient sg (light ? Colour (0xffd8d8d8) : Colour (0xff5a5a5a), c.x - r, c.y - r, light ? Colour (0xff6a6a6a) : Colour (0xff111111),
                                 c.x + r, c.y + r, false);
        g.setGradientFill (sg);
        g.fillEllipse (circle (c, r));
        g.setColour (Colour (light ? 0xff3a3a3a : 0xff050505));
        g.drawLine (c.x - r * 0.65f, c.y + r * 0.25f, c.x + r * 0.65f, c.y - r * 0.25f, r * 0.3f);
        g.drawLine (c.x - r * 0.25f, c.y - r * 0.65f, c.x + r * 0.25f, c.y + r * 0.65f, r * 0.3f);
    }

    void drawPlate (juce::Graphics& g, const Design& d, bool thumbnail)
    {
        const auto r = d.plate;
        // Drop shadow on the backdrop.
        g.setColour (juce::Colours::black.withAlpha (0.55f));
        g.fillRect (r.translated (0.0f, 6.0f).expanded (2.0f, 0.0f));
        juce::ColourGradient pg (d.panel.brighter (0.08f), r.getX(), r.getY(), d.panel.darker (0.18f), r.getX(), r.getBottom(), false);
        g.setGradientFill (pg);
        g.fillRect (r);
        juce::ignoreUnused (thumbnail);
        if (d.wear > 0.0f)
        {
            g.setOpacity (juce::jlimit (0.0f, 1.0f, d.wear));
            g.drawImage (textureImage (d.texture, g.getInternalContext().getPhysicalPixelScaleFactor()), r, juce::RectanglePlacement::stretchToFit);
            g.setOpacity (1.0f);
        }
        // Edge bevel.
        g.setColour (juce::Colours::white.withAlpha (0.22f));
        g.drawHorizontalLine (juce::roundToInt (r.getY()), r.getX(), r.getRight());
        g.setColour (juce::Colours::black.withAlpha (0.5f));
        g.drawHorizontalLine (juce::roundToInt (r.getBottom() - 1.0f), r.getX(), r.getRight());
        g.drawVerticalLine (juce::roundToInt (r.getRight() - 1.0f), r.getY(), r.getBottom());
        if (d.screws)
            for (auto c : { Point<float> (r.getX() + 24, r.getY() + 26), Point<float> (r.getRight() - 24, r.getY() + 26),
                            Point<float> (r.getX() + 24, r.getBottom() - 24), Point<float> (r.getRight() - 24, r.getBottom() - 24) })
                drawScrew (g, c, 7.0f, d.panel);
    }

    //==========================================================================
    // Knobs

    /** A conical brushed-metal face: radial base with light wedges. */
    void brushedCone (juce::Graphics& g, Point<float> c, float r, float bright = 1.0f)
    {
        juce::ColourGradient base (Colour (0xffe6e6e6).withMultipliedBrightness (bright), c.x, c.y, Colour (0xff8c8e92).withMultipliedBrightness (bright),
                                   c.x + r, c.y, true);
        g.setGradientFill (base);
        g.fillEllipse (circle (c, r));
        const int n = 48;
        for (int i = 0; i < n; ++i)
        {
            const float a0 = kTwoPi * (float) i / (float) n, a1 = kTwoPi * (float) (i + 1) / (float) n;
            const float k = std::cos (2.0f * (a0 + 0.6f));
            juce::Path w;
            w.startNewSubPath (c);
            w.lineTo (polar (c, r, a0));
            w.lineTo (polar (c, r, a1));
            w.closeSubPath();
            g.setColour (k > 0 ? juce::Colours::white.withAlpha (0.28f * k) : juce::Colours::black.withAlpha (-0.22f * k));
            g.fillPath (w);
        }
        g.setColour (juce::Colours::white.withAlpha (0.5f));
        g.fillEllipse (circle (c, r * 0.06f));
    }

    void knurl (juce::Graphics& g, Point<float> c, float r0, float r1, int n, Colour dark, Colour light)
    {
        g.setColour (dark);
        g.fillEllipse (circle (c, r1));
        for (int i = 0; i < n; ++i)
        {
            const float a = kTwoPi * (float) i / (float) n;
            const float shade = 0.5f + 0.5f * std::cos (a + 0.8f);
            g.setColour (light.withAlpha (0.35f + 0.6f * shade));
            g.fillEllipse (circle (polar (c, (r0 + r1) * 0.5f, a), (r1 - r0) * 0.38f));
        }
    }

    void shadow (juce::Graphics& g, Point<float> c, float r, float strength = 0.55f)
    {
        juce::ColourGradient sh (juce::Colours::black.withAlpha (strength), c.x + r * 0.08f, c.y + r * 0.18f, juce::Colours::transparentBlack,
                                 c.x + r * 0.08f + r * 1.18f, c.y + r * 0.18f, true);
        g.setGradientFill (sh);
        g.fillEllipse (circle (c.translated (r * 0.08f, r * 0.18f), r * 1.18f));
    }

    void glossyDome (juce::Graphics& g, Point<float> c, float r, Colour top, Colour bottom)
    {
        juce::ColourGradient dg (top, c.x - r * 0.3f, c.y - r * 0.6f, bottom, c.x + r * 0.3f, c.y + r * 0.9f, false);
        g.setGradientFill (dg);
        g.fillEllipse (circle (c, r));
        juce::ColourGradient hl (juce::Colours::white.withAlpha (0.35f), c.x - r * 0.25f, c.y - r * 0.55f, juce::Colours::white.withAlpha (0.0f),
                                 c.x - r * 0.25f, c.y + r * 0.1f, false);
        g.setGradientFill (hl);
        g.fillEllipse (Rectangle<float> (r * 1.3f, r * 0.85f).withCentre (c.translated (-r * 0.1f, -r * 0.42f)));
    }

    void pointerLine (juce::Graphics& g, Point<float> c, float r0, float r1, float a, float width, Colour colour)
    {
        g.setColour (colour);
        const auto p0 = polar (c, r0, a), p1 = polar (c, r1, a);
        g.drawLine ({ p0, p1 }, width);
    }

    void drawScale (juce::Graphics& g, const Element& e, Point<float> c, float r)
    {
        const float half = rad (e.sweep) * 0.5f, mid = rad (e.centre);
        if (! e.band.isTransparent())
        {
            juce::Path band;
            band.addCentredArc (c.x, c.y, r * 1.12f, r * 1.12f, 0.0f, mid - half, mid + half, true);
            g.setColour (e.band);
            g.strokePath (band, juce::PathStrokeType (r * 0.2f));
        }
        if (e.ticks > 1)
        {
            for (int i = 0; i < e.ticks; ++i)
            {
                const float a = mid - half + 2.0f * half * (float) i / (float) (e.ticks - 1);
                if (e.skipLabelled && i < e.labels.size() && e.labels[i].isNotEmpty())
                    continue;
                g.setColour (e.scaleColour);
                if (e.dots)
                    g.fillEllipse (circle (polar (c, r * e.tickRadius, a), juce::jmax (1.4f, r * 0.055f)));
                else
                {
                    const bool major = e.labels.size() > 1 && (e.ticks - 1) % (e.labels.size() - 1) == 0
                                       && i % ((e.ticks - 1) / (e.labels.size() - 1)) == 0;
                    g.drawLine ({ polar (c, r * e.tickRadius, a), polar (c, r * (e.tickRadius + (major ? 0.2f : 0.12f)), a) }, major ? 1.8f : 1.1f);
                }
            }
        }
        if (! e.labels.isEmpty())
        {
            g.setColour (e.scaleColour);
            g.setFont (face (Look::Sans, e.labelSize));
            const int n = e.labels.size();
            for (int i = 0; i < n; ++i)
            {
                const float a = mid + (n == 1 ? 0.0f : -half + 2.0f * half * (float) i / (float) (n - 1));
                const auto p = polar (c, r * e.labelRadius, a);
                if (e.rotate)
                {
                    g.saveState();
                    g.addTransform (juce::AffineTransform::rotation (a, p.x, p.y));
                    g.drawText (e.labels[i], Rectangle<float> (60.0f, e.labelSize * 1.2f).withCentre (p), juce::Justification::centred);
                    g.restoreState();
                }
                else
                    g.drawText (e.labels[i], Rectangle<float> (60.0f, e.labelSize * 1.2f).withCentre (p), juce::Justification::centred);
            }
        }
    }

    void drawKnob (juce::Graphics& g, const Element& e, float pos)
    {
        const Point<float> c (e.x, e.y);
        const float r = e.w * 0.5f;
        const float a = rad (e.centre - e.sweep * 0.5f + e.sweep * juce::jlimit (0.0f, 1.0f, pos));
        if (e.look != Look::NumberDial)
            drawScale (g, e, c, r);

        switch (lookIndex (e.look))
        {
            case lookIndex (Look::ChromeCollar):
            {
                shadow (g, c, r);
                glossyDome (g, c, r, Colour (0xff3a3a3a), Colour (0xff050505));
                g.setColour (juce::Colours::white.withAlpha (0.18f));
                g.drawEllipse (circle (c, r - 1.0f), 1.2f);
                pointerLine (g, c, r * 0.8f, r * 0.98f, a, juce::jmax (2.0f, r * 0.07f), Colour (0xfff2f2f2));
                knurl (g, c, r * 0.68f, r * 0.8f, 56, Colour (0xff1e1e1e), Colour (0xffc8c8c8));
                brushedCone (g, c, r * 0.68f);
                break;
            }
            case lookIndex (Look::ChromeSmall):
            {
                shadow (g, c, r);
                knurl (g, c, r * 0.78f, r, 40, Colour (0xff1a1a1a), Colour (0xffd0d0d0));
                brushedCone (g, c, r * 0.8f);
                g.setColour (Colour (0xff111111));
                g.drawEllipse (circle (polar (c, r * 0.52f, a), r * 0.14f), juce::jmax (1.0f, r * 0.05f));
                break;
            }
            case lookIndex (Look::BlackFluted):
            {
                shadow (g, c, r, 0.6f);
                // Skirt: dark metal with a white index line.
                juce::ColourGradient sk (Colour (0xff4a4a4a), c.x - r, c.y - r, Colour (0xff050505), c.x + r * 0.6f, c.y + r, false);
                g.setGradientFill (sk);
                g.fillEllipse (circle (c, r));
                g.setColour (juce::Colours::white.withAlpha (0.25f));
                g.drawEllipse (circle (c, r - 1.0f), 1.0f);
                pointerLine (g, c, r * 0.66f, r * 0.97f, a, juce::jmax (2.0f, r * 0.06f), Colour (0xffeeeeee));
                // Fluted cap.
                juce::Path flute;
                const float rr = r * 0.66f;
                for (int i = 0; i <= 180; ++i)
                {
                    const float t = kTwoPi * (float) i / 180.0f;
                    const float rf = rr * (0.93f + 0.07f * std::cos (12.0f * (t - a)));
                    const auto p = polar (c, rf, t);
                    if (i == 0)
                        flute.startNewSubPath (p);
                    else
                        flute.lineTo (p);
                }
                flute.closeSubPath();
                g.setColour (juce::Colours::black.withAlpha (0.6f));
                g.fillPath (flute, juce::AffineTransform::translation (r * 0.04f, r * 0.08f));
                // Black, or the cap colour when the design gives a grey one.
                const bool grey = e.colour.getSaturation() < 0.15f;
                juce::ColourGradient fg (grey ? e.colour.brighter (0.3f) : Colour (0xff3c3c3c), c.x - rr * 0.4f, c.y - rr * 0.8f,
                                         grey ? e.colour.darker (0.7f) : Colour (0xff030303), c.x + rr * 0.3f, c.y + rr, false);
                g.setGradientFill (fg);
                g.fillPath (flute);
                juce::ColourGradient hl (juce::Colours::white.withAlpha (0.28f), c.x, c.y - rr, juce::Colours::white.withAlpha (0.0f), c.x, c.y, false);
                g.setGradientFill (hl);
                g.fillEllipse (Rectangle<float> (rr * 1.4f, rr * 0.9f).withCentre (c.translated (0.0f, -rr * 0.45f)));
                g.setColour (juce::Colour (0xff111111));
                g.drawEllipse (circle (c, rr * 0.45f), 1.0f);
                break;
            }
            case lookIndex (Look::BlackDome):
            {
                shadow (g, c, r);
                juce::ColourGradient ring (Colour (0xffe0e0e0), c.x, c.y - r, Colour (0xff6a6a6a), c.x, c.y + r, false);
                g.setGradientFill (ring);
                g.fillEllipse (circle (c, r));
                glossyDome (g, c, r * 0.86f, Colour (0xff3e3e3e), Colour (0xff020202));
                pointerLine (g, c, r * 0.2f, r * 0.82f, a, juce::jmax (1.8f, r * 0.1f), Colour (0xfff4f4f4));
                break;
            }
            case lookIndex (Look::ColourCap):
            {
                shadow (g, c, r);
                juce::ColourGradient bz (Colour (0xfff4f4f4), c.x - r * 0.5f, c.y - r, Colour (0xff70747a), c.x + r * 0.4f, c.y + r, false);
                g.setGradientFill (bz);
                g.fillEllipse (circle (c, r));
                g.setColour (juce::Colours::black.withAlpha (0.35f));
                g.drawEllipse (circle (c, r * 0.84f), 1.0f);
                juce::ColourGradient cg (e.colour.brighter (0.55f), c.x - r * 0.35f, c.y - r * 0.55f, e.colour.darker (0.55f), c.x + r * 0.4f, c.y + r * 0.8f, true);
                g.setGradientFill (cg);
                g.fillEllipse (circle (c, r * 0.82f));
                juce::ColourGradient hl (juce::Colours::white.withAlpha (0.4f), c.x, c.y - r * 0.8f, juce::Colours::white.withAlpha (0.0f), c.x, c.y - r * 0.1f, false);
                g.setGradientFill (hl);
                g.fillEllipse (Rectangle<float> (r * 1.1f, r * 0.7f).withCentre (c.translated (0.0f, -r * 0.42f)));
                pointerLine (g, c, r * 0.12f, r * 0.8f, a, juce::jmax (1.8f, r * 0.11f), e.colour.getPerceivedBrightness() > 0.75f ? Colour (0xff111111) : Colour (0xfff8f8f8));
                break;
            }
            case lookIndex (Look::ChromeBar):
            {
                shadow (g, c, r);
                brushedCone (g, c, r, 0.95f);
                g.setColour (juce::Colours::black.withAlpha (0.5f));
                g.drawEllipse (circle (c, r), 1.2f);
                // Ribbed band across the knob, turning with it.
                juce::Path band;
                band.addRoundedRectangle (-r * 0.98f, -r * 0.24f, r * 1.96f, r * 0.48f, r * 0.12f);
                const auto t = juce::AffineTransform::rotation (a + kPi * 0.5f).translated (c);
                g.saveState();
                g.reduceClipRegion (band, t);
                for (int i = -10; i <= 10; ++i)
                {
                    juce::Path rib;
                    rib.addRectangle ((float) i * r * 0.1f, -r * 0.3f, r * 0.05f, r * 0.6f);
                    g.setColour (juce::Colours::black.withAlpha (0.35f));
                    g.fillPath (rib, t);
                }
                g.restoreState();
                g.setColour (juce::Colours::black.withAlpha (0.4f));
                g.strokePath (band, juce::PathStrokeType (1.0f), t);
                pointerLine (g, c, r * 0.55f, r * 0.95f, a, juce::jmax (2.0f, r * 0.08f), Colour (0xff111111));
                break;
            }
            case lookIndex (Look::Chicken):
            {
                shadow (g, c, r * 0.6f);
                juce::Path head;
                head.addEllipse (-r * 0.42f, -r * 0.42f, r * 0.84f, r * 0.84f);
                head.startNewSubPath (-r * 0.2f, -r * 0.2f);
                head.lineTo (-r * 0.07f, -r * 0.98f);
                head.lineTo (r * 0.07f, -r * 0.98f);
                head.lineTo (r * 0.2f, -r * 0.2f);
                head.closeSubPath();
                head.startNewSubPath (-r * 0.3f, r * 0.1f);
                head.lineTo (-r * 0.12f, r * 0.62f);
                head.lineTo (r * 0.12f, r * 0.62f);
                head.lineTo (r * 0.3f, r * 0.1f);
                head.closeSubPath();
                const auto t = juce::AffineTransform::rotation (a).translated (c);
                g.setColour (juce::Colours::black.withAlpha (0.5f));
                g.fillPath (head, t.translated (r * 0.05f, r * 0.1f));
                juce::ColourGradient hg (Colour (0xff4a4a4a), c.x - r * 0.5f, c.y - r * 0.5f, Colour (0xff050505), c.x + r * 0.5f, c.y + r * 0.6f, false);
                g.setGradientFill (hg);
                g.fillPath (head, t);
                pointerLine (g, c, r * 0.35f, r * 0.9f, a, juce::jmax (1.5f, r * 0.05f), Colour (0xffeeeeee));
                break;
            }
            case lookIndex (Look::BlackSkirt):
            case lookIndex (Look::Pedal):
            {
                shadow (g, c, r);
                juce::Path ribbed;
                const int n = e.look == Look::Pedal ? 40 : 28;
                for (int i = 0; i <= n * 4; ++i)
                {
                    const float t = kTwoPi * (float) i / (float) (n * 4);
                    const float rf = r * ((i / 2) % 2 == 0 ? 1.0f : 0.95f);
                    const auto p = polar (c, rf, t);
                    if (i == 0)
                        ribbed.startNewSubPath (p);
                    else
                        ribbed.lineTo (p);
                }
                ribbed.closeSubPath();
                juce::ColourGradient rg (Colour (0xff3a3a3a), c.x, c.y - r, Colour (0xff050505), c.x, c.y + r, false);
                g.setGradientFill (rg);
                g.fillPath (ribbed);
                glossyDome (g, c, r * 0.78f, Colour (0xff353535), Colour (0xff080808));
                pointerLine (g, c, r * (e.look == Look::Pedal ? 0.35f : 0.15f), r * 0.92f, a, juce::jmax (2.0f, r * 0.08f), Colour (0xfff0f0f0));
                break;
            }
            case lookIndex (Look::WhiteDome):
            {
                shadow (g, c, r);
                glossyDome (g, c, r, Colour (0xfffbf8ef), Colour (0xffa9a49a));
                g.setColour (juce::Colours::black.withAlpha (0.3f));
                g.drawEllipse (circle (c, r), 1.0f);
                pointerLine (g, c, r * 0.2f, r * 0.9f, a, juce::jmax (2.0f, r * 0.09f), Colour (0xff181818));
                break;
            }
            case lookIndex (Look::Knurled):
            {
                shadow (g, c, r);
                knurl (g, c, r * 0.8f, r, 44, Colour (0xff151515), Colour (0xffbdbdbd));
                g.setColour (juce::Colours::black);
                g.drawEllipse (circle (c, r * 0.8f), 1.2f);
                glossyDome (g, c, r * 0.78f, e.colour.brighter (0.35f), e.colour.darker (0.8f));
                pointerLine (g, c, r * 0.15f, r * 0.75f, a, juce::jmax (2.0f, r * 0.09f), Colour (0xfff4f4f4));
                break;
            }
            case lookIndex (Look::ColourDome):
            {
                shadow (g, c, r);
                juce::ColourGradient ring (Colour (0xff3a3a3a), c.x, c.y - r, Colour (0xff050505), c.x, c.y + r, false);
                g.setGradientFill (ring);
                g.fillEllipse (circle (c, r));
                glossyDome (g, c, r * 0.8f, e.colour.brighter (0.45f), e.colour.darker (0.6f));
                pointerLine (g, c, r * 0.12f, r * 0.78f, a, juce::jmax (2.0f, r * 0.1f), Colour (0xfff8f8f8));
                break;
            }
            case lookIndex (Look::NumberDial):
            {
                shadow (g, c, r);
                juce::ColourGradient dial (Colour (0xfffafafa), c.x - r * 0.4f, c.y - r, Colour (0xffa8a8a8), c.x + r * 0.4f, c.y + r, false);
                g.setGradientFill (dial);
                g.fillEllipse (circle (c, r));
                g.setColour (juce::Colours::black.withAlpha (0.4f));
                g.drawEllipse (circle (c, r), 1.0f);
                // The numbers turn with the dial: the current value sits under the index at the top.
                const int n = e.labels.size();
                g.setColour (Colour (0xff141414));
                g.setFont (face (Look::Sans, e.labelSize));
                for (int i = 0; i < n; ++i)
                {
                    const float t = n == 1 ? 0.0f : (float) i / (float) (n - 1);
                    const float ang = rad (e.sweep * (t - juce::jlimit (0.0f, 1.0f, pos)));
                    const auto p = polar (c, r * 0.8f, ang);
                    g.saveState();
                    g.addTransform (juce::AffineTransform::rotation (ang, p.x, p.y));
                    g.drawText (e.labels[i], Rectangle<float> (40.0f, e.labelSize * 1.2f).withCentre (p), juce::Justification::centred);
                    g.restoreState();
                }
                g.setColour (Colour (0xff111111));
                g.fillEllipse (circle (c, r * 0.64f));
                juce::ColourGradient dome (Colour (0xfff4f4f4), c.x - r * 0.2f, c.y - r * 0.5f, Colour (0xff8e8e8e), c.x + r * 0.3f, c.y + r * 0.6f, false);
                g.setGradientFill (dome);
                g.fillEllipse (circle (c, r * 0.52f));
                break;
            }
            case lookIndex (Look::Lobed):
            {
                // A cream body with four rounded lobes (one is the pointer) and a coloured cap.
                juce::Path body;
                for (int i = 0; i <= 120; ++i)
                {
                    const float t = kTwoPi * (float) i / 120.0f;
                    const float rf = r * (0.8f + 0.2f * std::pow (std::abs (std::cos (2.0f * (t - a))), 3.0f));
                    const auto p = polar (c, rf, t);
                    if (i == 0)
                        body.startNewSubPath (p);
                    else
                        body.lineTo (p);
                }
                body.closeSubPath();
                g.setColour (juce::Colours::black.withAlpha (0.5f));
                g.fillPath (body, juce::AffineTransform::translation (r * 0.06f, r * 0.12f));
                juce::ColourGradient bg (Colour (0xfff6f2e6), c.x - r * 0.5f, c.y - r, Colour (0xffa8a498), c.x + r * 0.5f, c.y + r, false);
                g.setGradientFill (bg);
                g.fillPath (body);
                glossyDome (g, c, r * 0.58f, e.colour.brighter (0.4f), e.colour.darker (0.6f));
                g.setColour (Colour (0xff111111));
                g.fillEllipse (circle (polar (c, r * 0.9f, a), r * 0.06f));
                break;
            }
            case lookIndex (Look::BlackDot):
            {
                shadow (g, c, r);
                juce::ColourGradient ring (Colour (0xff3a3a3a), c.x, c.y - r, Colour (0xff060606), c.x, c.y + r, false);
                g.setGradientFill (ring);
                g.fillEllipse (circle (c, r));
                g.setColour (juce::Colours::white.withAlpha (0.12f));
                for (int i = 0; i < 36; ++i)
                {
                    const float t = kTwoPi * (float) i / 36.0f;
                    g.drawLine ({ polar (c, r * 0.8f, t), polar (c, r * 0.97f, t) }, 1.0f);
                }
                glossyDome (g, c, r * 0.62f, Colour (0xff3c3c3c), Colour (0xff030303));
                g.setColour (Colour (0xfff4f4f4));
                g.fillEllipse (circle (polar (c, r * 0.86f, a), juce::jmax (2.0f, r * 0.09f)));
                break;
            }
            case lookIndex (Look::SilverDial):
            {
                shadow (g, c, r);
                juce::ColourGradient rg (Colour (0xfff2f2f2), c.x, c.y - r, Colour (0xff9a9a9a), c.x, c.y + r, false);
                g.setGradientFill (rg);
                g.fillEllipse (circle (c, r));
                g.setColour (Colour (0xff202020));
                g.drawEllipse (circle (c, r * 0.97f), r * 0.05f);
                for (int i = 0; i < 24; ++i)
                {
                    const float t = a + kTwoPi * (float) i / 24.0f;
                    g.drawLine ({ polar (c, r * 0.8f, t), polar (c, r * 0.9f, t) }, 1.0f);
                }
                brushedCone (g, c, r * 0.66f);
                pointerLine (g, c, r * 0.66f, r * 0.95f, a, juce::jmax (2.0f, r * 0.07f), Colour (0xff111111));
                break;
            }
            case lookIndex (Look::Chrome):
            default:
            {
                shadow (g, c, r);
                g.setColour (Colour (0xff2a2a2a));
                g.fillEllipse (circle (c, r));
                brushedCone (g, c, r * 0.94f);
                pointerLine (g, c, r * 0.3f, r * 0.9f, a, juce::jmax (2.0f, r * 0.08f), Colour (0xff141414));
                break;
            }
        }
    }

    //==========================================================================
    // Switches, keys and lamps

    void drawToggle (juce::Graphics& g, const Element& e, bool up)
    {
        const Point<float> c (e.x, e.y);
        juce::Graphics::ScopedSaveState keep (g);
        if (! e.vertical) // sideways: "up" points right
            g.addTransform (juce::AffineTransform::rotation (kPi * 0.5f, c.x, c.y));
        const float r = e.w * 0.5f;
        // Hex nut and bushing.
        juce::Path nut;
        nut.addPolygon (c, 6, r, kPi / 6.0f);
        g.setColour (juce::Colours::black.withAlpha (0.5f));
        g.fillPath (nut, juce::AffineTransform::translation (1.5f, 3.0f));
        juce::ColourGradient ng (Colour (0xffeeeeee), c.x - r, c.y - r, Colour (0xff6c6c6c), c.x + r, c.y + r, false);
        g.setGradientFill (ng);
        g.fillPath (nut);
        g.setColour (Colour (0xff2a2a2a));
        g.fillEllipse (circle (c, r * 0.52f));
        // Lever: a chrome bat pointing up or down.
        const float dir = up ? -1.0f : 1.0f;
        const auto tip = c.translated (0.0f, dir * r * 1.15f);
        juce::Path lever;
        lever.startNewSubPath (c.x - r * 0.28f, c.y);
        lever.lineTo (tip.x - r * 0.2f, tip.y);
        lever.lineTo (tip.x + r * 0.2f, tip.y);
        lever.lineTo (c.x + r * 0.28f, c.y);
        lever.closeSubPath();
        juce::ColourGradient lg (Colour (0xff7a7a7a), c.x - r * 0.3f, c.y, Colour (0xff5a5a5a), c.x + r * 0.3f, c.y, false);
        lg.addColour (0.4, Colour (0xfff8f8f8));
        g.setGradientFill (lg);
        g.fillPath (lever);
        juce::ColourGradient bg (Colour (0xffffffff), tip.x - r * 0.1f, tip.y - r * 0.15f, Colour (0xff7a7a7a), tip.x + r * 0.2f, tip.y + r * 0.25f, true);
        g.setGradientFill (bg);
        g.fillEllipse (circle (tip, r * 0.3f));
    }

    void drawLed (juce::Graphics& g, Point<float> c, float r, Colour colour, bool on)
    {
        g.setColour (juce::Colours::black.withAlpha (0.7f));
        g.fillEllipse (circle (c, r * 1.25f));
        if (on)
        {
            g.setColour (colour.withAlpha (0.35f));
            g.fillEllipse (circle (c, r * 2.0f));
            juce::ColourGradient lg (colour.brighter (0.8f), c.x - r * 0.3f, c.y - r * 0.3f, colour, c.x + r, c.y + r, true);
            g.setGradientFill (lg);
        }
        else
        {
            juce::ColourGradient lg (Colour (0xff555555), c.x - r * 0.3f, c.y - r * 0.4f, Colour (0xff141414), c.x + r, c.y + r, true);
            g.setGradientFill (lg);
        }
        g.fillEllipse (circle (c, r));
    }

    void drawKey (juce::Graphics& g, const Element& e, bool on)
    {
        const Rectangle<float> r (e.x, e.y, e.w, e.h);
        switch (lookIndex (e.look))
        {
            case lookIndex (Look::SoulKey):
            {
                g.setColour (juce::Colours::black.withAlpha (0.6f));
                g.fillRect (r.translated (2.0f, 3.0f));
                juce::ColourGradient kg (Colour (0xff9a9a9a), r.getX(), r.getY(), Colour (0xff4a4a4a), r.getX(), r.getBottom(), false);
                g.setGradientFill (kg);
                g.fillRect (r);
                g.setColour (Colour (0xff2a2a2a));
                g.drawRect (r, 1.0f);
                g.setColour (juce::Colours::white.withAlpha (0.35f));
                g.drawHorizontalLine (juce::roundToInt (r.getY() + 1.0f), r.getX() + 1.0f, r.getRight() - 1.0f);
                g.setColour (Colour (0xff111111));
                g.setFont (face (Look::Sans, e.size));
                g.drawText (e.text, r, juce::Justification::centred);
                // The LED to the right, on a short stalk.
                const Point<float> lc (r.getRight() + r.getHeight() * 0.55f, r.getCentreY() + 1.0f);
                g.setColour (Colour (0xff1a1a1a));
                g.fillRect (Rectangle<float> (r.getRight(), lc.y - 2.0f, lc.x - r.getRight(), 4.0f));
                drawLed (g, lc, r.getHeight() * 0.24f, e.colour, on);
                break;
            }
            case lookIndex (Look::LitSquare):
            {
                g.setColour (Colour (0xffd8d8d8));
                g.drawRect (r.expanded (6.0f), 1.5f);
                g.setColour (juce::Colours::black);
                g.fillRect (r.expanded (2.0f));
                juce::ColourGradient kg (on ? e.colour.brighter (0.3f) : e.colour.darker (0.75f), r.getX(), r.getY(), on ? e.colour : e.colour.darker (0.9f),
                                         r.getX(), r.getBottom(), false);
                g.setGradientFill (kg);
                g.fillRect (r);
                g.setColour (Colour (0xff111111));
                g.setFont (face (Look::Sans, e.size));
                g.drawText (e.text, r, juce::Justification::centred);
                break;
            }
            case lookIndex (Look::StackKey):
            {
                g.setColour (Colour (0xff050505));
                g.fillRect (r.expanded (2.0f));
                juce::ColourGradient kg (on ? Colour (0xff1a1a1a) : Colour (0xff6a6a6a), r.getX(), r.getY(), on ? Colour (0xff2a2a2a) : Colour (0xff3a3a3a),
                                         r.getX(), r.getBottom(), false);
                g.setGradientFill (kg);
                g.fillRect (r);
                g.setColour (juce::Colours::white.withAlpha (on ? 0.05f : 0.3f));
                g.drawHorizontalLine (juce::roundToInt (r.getY() + 1.0f), r.getX() + 1.0f, r.getRight() - 1.0f);
                if (e.text.isNotEmpty())
                {
                    g.setColour (e.colour2);
                    g.setFont (face (Look::Sans, e.size));
                    g.drawText (e.text, r, juce::Justification::centred);
                }
                break;
            }
            case lookIndex (Look::RoundKey):
            {
                const auto c = r.getCentre();
                const float rr = juce::jmin (r.getWidth(), r.getHeight()) * 0.5f;
                shadow (g, c, rr);
                glossyDome (g, c, rr, Colour (0xff4a4a4a), Colour (0xff0a0a0a));
                g.setColour (juce::Colours::black);
                g.drawEllipse (circle (c, rr), 1.0f);
                if (on)
                {
                    g.setColour (e.colour.withAlpha (0.8f));
                    g.fillEllipse (circle (c, rr * 0.35f));
                }
                break;
            }
            case lookIndex (Look::LedKey):
            {
                g.setColour (juce::Colours::black.withAlpha (0.6f));
                g.fillRect (r.translated (1.5f, 2.5f));
                juce::ColourGradient kg (Colour (0xff6a6a6a), r.getX(), r.getY(), Colour (0xff2a2a2a), r.getX(), r.getBottom(), false);
                g.setGradientFill (kg);
                g.fillRect (r);
                g.setColour (Colour (0xff111111));
                g.drawRect (r, 1.0f);
                const float s = juce::jmin (r.getWidth(), r.getHeight());
                auto led = Rectangle<float> (s * 0.55f, s * 0.3f).withCentre (r.getCentre());
                g.setColour (on ? e.colour : Colour (0xff3a3a3a));
                g.fillRect (led);
                if (on)
                {
                    g.setColour (e.colour.withAlpha (0.3f));
                    g.fillRect (led.expanded (3.0f));
                }
                if (e.text.isNotEmpty())
                {
                    g.setColour (e.colour2);
                    g.setFont (face (Look::Sans, e.size));
                    g.drawText (e.text, r.withY (r.getBottom() + 2.0f).withHeight (e.size * 1.3f).expanded (40.0f, 0.0f), juce::Justification::centredTop);
                }
                break;
            }
            case lookIndex (Look::Hidden):
                break;
            case lookIndex (Look::TabKey):
            {
                const auto base = e.colour2.isOpaque() && e.colour2 != Colour (0xff000000) ? e.colour2 : Colour (0xffcfcfcf);
                juce::ColourGradient kg (on ? base.darker (0.15f) : base.brighter (0.1f), r.getX(), r.getY(), on ? base.darker (0.5f) : base.darker (0.3f),
                                         r.getX(), r.getBottom(), false);
                g.setGradientFill (kg);
                g.fillRoundedRectangle (r, 3.0f);
                g.setColour (on ? Colour (0xff2a6ad8) : Colour (0xff3a3a3a));
                g.drawRoundedRectangle (r, 3.0f, on ? 2.0f : 1.0f);
                g.setColour (Colour (0xff111111));
                g.setFont (face (Look::Sans, e.size));
                g.drawText (e.text, r, juce::Justification::centred);
                break;
            }
            case lookIndex (Look::FlatKey):
            default:
            {
                g.setColour (on ? e.colour : Colour (0xff2a2a2a));
                g.fillRoundedRectangle (r, 3.0f);
                g.setColour (on ? Colour (0xff111111) : Colour (0xffd0d0d0));
                g.setFont (face (Look::Sans, e.size));
                g.drawText (e.text, r, juce::Justification::centred);
                break;
            }
        }
    }

    //==========================================================================
    // Meters

    /** Position 0..1 along a VU arc for `db` of reduction (0 dB at `zero`, the
        VU law: the needle follows the voltage). */
    float vuPosition (float db, float zero)
    {
        const float v = std::pow (10.0f, -db / 20.0f); // 1 at 0 dB, 0.1 at -20
        return juce::jlimit (0.0f, 1.0f, (v - 0.1f) / 0.9f * zero);
    }

    void drawVu (juce::Graphics& g, const Element& e, float reduction)
    {
        const Rectangle<float> f (e.x, e.y, e.w, e.h);
        const bool amber = e.look != Look::VuWhite;
        const bool orange = e.look == Look::VuOrange;
        const bool round = e.look == Look::VuRound;
        // The face's outline: a rectangle, an arch (round top) or the upper part of a round window.
        juce::Path facePath;
        if (e.look == Look::VuCream)
            facePath.addRoundedRectangle (f.getX(), f.getY(), f.getWidth(), f.getHeight(), f.getWidth() * 0.5f, f.getWidth() * 0.5f, true, true, false, false);
        else if (round)
            facePath.addEllipse (f.getX(), f.getY(), f.getWidth(), f.getWidth());
        else
            facePath.addRectangle (f);

        // Housing / bezel.
        if (e.look == Look::VuAmber)
        {
            auto b = f.expanded (22.0f, 20.0f);
            g.setColour (juce::Colours::black.withAlpha (0.5f));
            g.fillRect (b.translated (3.0f, 5.0f));
            juce::ColourGradient bz (Colour (0xffe8e8e8), b.getX(), b.getY(), Colour (0xff5a5a5a), b.getRight(), b.getBottom(), false);
            bz.addColour (0.5, Colour (0xff9a9a9a));
            g.setGradientFill (bz);
            g.fillRect (b);
            g.setColour (juce::Colours::black.withAlpha (0.5f));
            g.drawRect (b, 1.0f);
            g.setColour (Colour (0xff6a6a6a));
            g.fillRect (f.expanded (6.0f));
            g.setColour (Colour (0xff2a2a2a));
            g.drawRect (f.expanded (6.0f), 1.0f);
        }
        else if (round)
        {
            const auto c = Point<float> (f.getCentreX(), f.getY() + f.getWidth() * 0.5f);
            const float r = f.getWidth() * 0.5f;
            g.setColour (juce::Colours::black.withAlpha (0.6f));
            g.fillEllipse (circle (c.translated (3.0f, 6.0f), r + 12.0f));
            juce::ColourGradient rim (e.colour.brighter (0.4f), c.x, c.y - r, e.colour.darker (0.6f), c.x, c.y + r, false);
            g.setGradientFill (rim);
            g.fillEllipse (circle (c, r + 10.0f));
            g.setColour (e.colour.withAlpha (0.35f));
            g.drawEllipse (circle (c, r + 14.0f), 4.0f);
            g.setColour (Colour (0xff050505));
            g.fillEllipse (circle (c, r + 2.0f));
        }
        else
        {
            auto b = f.expanded (e.look == Look::VuWhite ? 14.0f : 12.0f, 12.0f);
            b = b.withHeight (b.getHeight() + e.housing);
            g.setColour (juce::Colours::black.withAlpha (0.6f));
            g.fillRoundedRectangle (b.translated (3.0f, 6.0f), e.look == Look::VuCream ? 14.0f : 0.0f);
            juce::ColourGradient hg (Colour (0xff2e2e2e), b.getX(), b.getY(), Colour (0xff050505), b.getX(), b.getBottom(), false);
            g.setGradientFill (hg);
            g.fillRoundedRectangle (b, e.look == Look::VuCream ? 14.0f : 0.0f);
            g.setColour (juce::Colours::white.withAlpha (0.15f));
            g.drawRoundedRectangle (b, e.look == Look::VuCream ? 14.0f : 0.0f, 1.0f);
        }
        // Face.
        g.saveState();
        g.reduceClipRegion (facePath);
        const float faceH = round ? f.getWidth() * 0.5f : f.getHeight();
        if (amber)
        {
            juce::ColourGradient fg (orange ? Colour (0xfffdd27a) : Colour (0xfffbe9b4), f.getCentreX(), f.getY() + faceH * 0.45f,
                                     orange ? Colour (0xffe8701a) : Colour (0xffc58a2a), f.getX(), f.getY() + faceH, true);
            g.setGradientFill (fg);
        }
        else
        {
            juce::ColourGradient fg (Colour (0xff6a6a68), f.getCentreX(), f.getY(), Colour (0xff1e1e1e), f.getCentreX(), f.getBottom(), false);
            g.setGradientFill (fg);
        }
        g.fillRect (f.withHeight (faceH));
        if (round)
        {
            g.setColour (Colour (0xff100808));
            g.fillRect (f.withTrimmedTop (faceH).withHeight (f.getWidth()));
        }
        const auto ink = amber ? Colour (0xff1c1408) : Colour (0xfff2f2f2);

        // Arc: pivot below the face.
        const Point<float> pivot (f.getCentreX(), f.getY() + faceH * 1.35f);
        const float len = faceH * 1.02f;
        const float sweep = rad (e.sweep < 270.0f ? e.sweep : 92.0f);
        auto angleOf = [&] (float t) { return -sweep * 0.5f + sweep * t; };
        juce::Path arc;
        arc.addCentredArc (pivot.x, pivot.y, len * 0.72f, len * 0.72f, 0.0f, angleOf (0.0f), angleOf (1.0f), true);
        g.setColour (ink);
        g.strokePath (arc, juce::PathStrokeType (1.6f));
        const float zero = e.reverse ? 1.0f : e.vuZero; // 0 dB sits short of the end on some VU scales
        auto pos = [&] (float db)
        {
            if (e.dbMax > e.dbMin)
                return juce::jlimit (0.0f, 1.0f, (-db - e.dbMin) / (e.dbMax - e.dbMin));
            return e.reverse ? juce::jlimit (0.0f, 1.0f, db / e.range) : vuPosition (db, zero);
        };
        g.setFont (face (Look::Sans, e.labelSize > 12.0f ? e.labelSize : 15.0f));
        for (int i = 0; i < e.scale.size() && i < (int) e.marks.size(); ++i)
        {
            const float a = angleOf (pos (e.marks[(size_t) i]));
            g.setColour (ink);
            g.drawLine ({ polar (pivot, len * 0.72f, a), polar (pivot, len * 0.8f, a) }, 1.6f);
            const auto lp = polar (pivot, len * 0.88f, a);
            g.drawText (e.scale[i], Rectangle<float> (50.0f, 18.0f).withCentre (lp), juce::Justification::centred);
        }
        // Minor ticks between.
        for (int i = 0; i <= 20; ++i)
        {
            const float a = angleOf ((float) i / 20.0f * (e.reverse ? 1.0f : zero));
            g.drawLine ({ polar (pivot, len * 0.72f, a), polar (pivot, len * 0.75f, a) }, 1.0f);
        }
        if (! e.reverse && (zero < 0.99f || e.dbMax > 0.0f))
        {
            // The red zone past 0.
            juce::Path red;
            red.addCentredArc (pivot.x, pivot.y, len * 0.735f, len * 0.735f, 0.0f, angleOf (e.dbMax > e.dbMin ? pos (0.0f) : zero), angleOf (1.0f), true);
            g.setColour (Colour (0xffc02018));
            g.strokePath (red, juce::PathStrokeType (4.0f));
        }
        g.setColour (ink);
        g.setFont (face (Look::Sans, 15.0f));
        g.drawFittedText (e.text, Rectangle<float> (f.getX(), f.getY() + faceH * e.captionY - 10.0f, f.getWidth(), 40.0f).toNearestInt(),
                          juce::Justification::centredTop, 2, 0.9f);
        // Needle.
        const float na = angleOf (pos (reduction));
        g.setColour (amber ? Colour (0xff1a1a1a) : Colour (0xffeeeeee));
        g.drawLine ({ pivot, polar (pivot, len * 0.95f, na) }, 1.8f);
        // Glass sheen.
        juce::ColourGradient gl (juce::Colours::white.withAlpha (0.18f), f.getX(), f.getY(), juce::Colours::white.withAlpha (0.0f), f.getX(), f.getY() + faceH * 0.5f, false);
        g.setGradientFill (gl);
        g.fillRect (f.withHeight (faceH * 0.5f));
        g.restoreState();
        g.setColour (juce::Colours::black.withAlpha (0.35f));
        g.strokePath (facePath, juce::PathStrokeType (1.0f));
    }

    void drawLeds (juce::Graphics& g, const Element& e, float reduction)
    {
        const int n = (int) e.marks.size();
        if (n == 0)
            return;
        const float d = e.vertical ? e.w : e.h;
        for (int i = 0; i < n; ++i)
        {
            const float t = n == 1 ? 0.5f : (float) i / (float) (n - 1);
            const Point<float> c = e.vertical ? Point<float> (e.x + e.w * 0.5f, e.y + e.h * t) : Point<float> (e.x + e.w * t, e.y + e.h * 0.5f);
            const bool lit = reduction >= e.marks[(size_t) i] && e.marks[(size_t) i] > 0.0f;
            if (e.dots)
                drawLed (g, c, d * 0.5f, e.colour, lit);
            else
            {
                // Bar LEDs (a column of slots).
                const float bh = e.vertical ? e.h / (float) n * 0.55f : e.h, bw = e.vertical ? e.w : e.w / (float) n * 0.55f;
                const auto bar = Rectangle<float> (bw, bh).withCentre (c);
                g.setColour (Colour (0xff050505));
                g.fillRect (bar.expanded (1.5f));
                g.setColour (lit ? e.colour : e.colour.withMultipliedBrightness (0.22f).withMultipliedSaturation (0.6f));
                g.fillRect (bar);
                if (lit)
                {
                    g.setColour (e.colour.withAlpha (0.25f));
                    g.fillRect (bar.expanded (3.0f));
                }
            }
            if (i < e.scale.size() && e.scale[i].isNotEmpty())
            {
                g.setColour (e.scaleColour);
                g.setFont (face (Look::Sans, e.labelSize));
                if (e.vertical)
                    g.drawText (e.scale[i], Rectangle<float> (c.x - d * 0.9f - 40.0f, c.y - 9.0f, 40.0f, 18.0f), juce::Justification::centredRight);
                else
                    g.drawText (e.scale[i], Rectangle<float> (40.0f, 16.0f).withCentre (c.translated (0.0f, -d * 1.2f)), juce::Justification::centred);
            }
        }
    }

    void drawFader (juce::Graphics& g, const Element& e, float pos)
    {
        const Rectangle<float> r (e.x, e.y, e.w, e.h);
        const float capH = juce::jmin (e.w * 0.55f, 26.0f);
        auto slot = r.withSizeKeepingCentre (juce::jmax (4.0f, e.w * 0.08f), r.getHeight() - capH);
        g.setColour (Colour (0xff050505));
        g.fillRoundedRectangle (slot, 2.0f);
        const float y = slot.getBottom() - pos * slot.getHeight();
        auto cap = Rectangle<float> (e.w * 0.62f, capH).withCentre ({ r.getCentreX(), y });
        g.setColour (juce::Colours::black.withAlpha (0.5f));
        g.fillRect (cap.translated (2.0f, 4.0f));
        juce::ColourGradient cg (e.colour.brighter (0.6f), cap.getX(), cap.getY(), e.colour.darker (0.4f), cap.getX(), cap.getBottom(), false);
        cg.addColour (0.5, e.colour);
        g.setGradientFill (cg);
        g.fillRect (cap);
        g.setColour (e.colour.getPerceivedBrightness() > 0.5f ? Colour (0xff111111) : Colour (0xffeeeeee));
        g.drawHorizontalLine (juce::roundToInt (cap.getCentreY()), cap.getX() + 2.0f, cap.getRight() - 2.0f);
    }

    void drawBox (juce::Graphics& g, const Element& e)
    {
        const Rectangle<float> r (e.x, e.y, e.w, e.h);
        switch (lookIndex (e.look))
        {
            case lookIndex (Look::Bevel):
            {
                g.setColour (juce::Colours::black.withAlpha (0.45f));
                g.fillRect (r.translated (2.0f, 4.0f));
                juce::ColourGradient bg (e.colour.brighter (0.1f), r.getX(), r.getY(), e.colour.darker (0.15f), r.getX(), r.getBottom(), false);
                g.setGradientFill (bg);
                g.fillRect (r);
                g.setColour (juce::Colours::white.withAlpha (0.2f));
                g.drawHorizontalLine (juce::roundToInt (r.getY()), r.getX(), r.getRight());
                break;
            }
            case lookIndex (Look::Inset):
            {
                g.setColour (e.colour);
                g.fillRect (r);
                juce::ColourGradient ig (juce::Colours::black.withAlpha (0.55f), r.getX(), r.getY(), juce::Colours::transparentBlack, r.getX(), r.getY() + 10.0f, false);
                g.setGradientFill (ig);
                g.fillRect (r.withHeight (10.0f));
                g.setColour (juce::Colours::white.withAlpha (0.15f));
                g.drawHorizontalLine (juce::roundToInt (r.getBottom()), r.getX(), r.getRight());
                break;
            }
            case lookIndex (Look::Wood):
            {
                juce::ColourGradient wg (Colour (0xff7a4626), r.getX(), r.getY(), Colour (0xff3a1e0e), r.getRight(), r.getY(), false);
                wg.addColour (0.4, Colour (0xff8e5632));
                g.setGradientFill (wg);
                g.fillRect (r);
                juce::Random rnd (juce::roundToInt (r.getX() * 7.0f + r.getY()));
                for (int i = 0; i < 18; ++i)
                {
                    const float x = r.getX() + rnd.nextFloat() * r.getWidth();
                    g.setColour (juce::Colours::black.withAlpha (0.12f + rnd.nextFloat() * 0.15f));
                    juce::Path p;
                    p.startNewSubPath (x, r.getY());
                    p.cubicTo (x + 4.0f * (rnd.nextFloat() - 0.5f), r.getY() + r.getHeight() * 0.3f, x + 6.0f * (rnd.nextFloat() - 0.5f),
                               r.getY() + r.getHeight() * 0.7f, x, r.getBottom());
                    g.strokePath (p, juce::PathStrokeType (0.6f + rnd.nextFloat()));
                }
                break;
            }
            case lookIndex (Look::Line):
                g.setColour (e.colour);
                if (e.h <= 0.0f || e.w <= 0.0f)
                    g.drawLine (e.x, e.y, e.x + e.w, e.y + e.h, e.size);
                else
                    g.drawRoundedRectangle (r, 3.0f, e.size);
                break;
            case lookIndex (Look::Fill):
            default:
                g.setColour (e.colour);
                g.fillRoundedRectangle (r, 2.0f);
                if (! e.colour2.isTransparent())
                {
                    g.setColour (e.colour2);
                    g.drawRoundedRectangle (r, 2.0f, e.size);
                }
                break;
        }
    }

    void drawText (juce::Graphics& g, const Element& e)
    {
        const Rectangle<float> r (e.x, e.y, e.w, e.h);
        juce::Graphics::ScopedSaveState keep (g);
        if (e.centre != 0.0f) // turned print, around the box's centre
            g.addTransform (juce::AffineTransform::rotation (rad (e.centre), r.getCentreX(), r.getCentreY()));
        g.setColour (e.colour);
        g.setFont (e.kern != 0.0f ? face (e.look, e.size).withExtraKerningFactor (e.kern) : face (e.look, e.size));
        g.drawFittedText (e.text, r.toNearestInt(), e.just, 2, 0.8f);
    }

    void drawValueBox (juce::Graphics& g, const Element& e, const models::Param& p, float v)
    {
        const Rectangle<float> r (e.x, e.y, e.w, e.h);
        g.setColour (e.colour2);
        g.fillRect (r);
        auto text = consoleText (p, v);
        if (e.look == Look::Segment)
            text = text.upToFirstOccurrenceOf (" ", false, false);
        g.setColour (e.colour.withAlpha (0.12f));
        g.setFont (face (e.look, e.size));
        if (e.look == Look::Segment)
        {
            // Unlit segments behind the digits: every digit position shows an 8.
            juce::String ghost;
            for (auto c : text)
                ghost += juce::CharacterFunctions::isDigit (c) || c == '-' ? juce::juce_wchar ('8') : c;
            g.drawText (ghost, r.reduced (4.0f, 0.0f), e.just);
        }
        g.setColour (e.colour);
        g.drawText (text, r.reduced (4.0f, 0.0f), e.just);
    }

    void drawDisplay (juce::Graphics& g, const Element& e, const PaintContext& ctx)
    {
        const Rectangle<float> r (e.x, e.y, e.w, e.h);
        g.setColour (e.colour2);
        g.fillRect (r);
        g.setColour (juce::Colours::black.withAlpha (0.6f));
        g.drawRect (r, 2.0f);
        const int n = (int) e.params.size() + (e.text.isNotEmpty() ? 1 : 0);
        if (n == 0)
            return;
        const float w = (r.getWidth() - 20.0f) / (float) n;
        g.setFont (face (Look::Dot, e.size));
        for (int i = 0; i < n; ++i)
        {
            auto cell = Rectangle<float> (r.getX() + 10.0f + w * (float) i, r.getY() + 4.0f, w, r.getHeight() - 8.0f);
            auto top = cell.removeFromTop (cell.getHeight() * 0.5f);
            g.setColour (e.colour);
            if (i < (int) e.params.size())
            {
                const int pi = e.params[(size_t) i];
                if (ctx.spec != nullptr && pi >= 0 && pi < (int) ctx.spec->params.size())
                {
                    const auto& p = ctx.spec->params[(size_t) pi];
                    g.drawText (consoleLabel (p).toUpperCase(), top, juce::Justification::centredLeft);
                    g.drawText (consoleText (p, ctx.value (pi)).toUpperCase(), cell, juce::Justification::centredLeft);
                }
            }
            else
            {
                g.drawText (e.text.upToFirstOccurrenceOf ("|", false, false), top, juce::Justification::centredLeft);
                g.drawText (e.text.fromFirstOccurrenceOf ("|", false, false), cell, juce::Justification::centredLeft);
            }
        }
    }


    //==========================================================================
    // Graph screens

    /** The magnitude (dB) of one dynamic-EQ band at `hz`: the whole band
        (OFF = no filter), a bell, low or high shelves (filter types OFF, BP,
        LP6, LP12, HP6, HP12). */
    float bandDb (float hz, float f, float gain, float q, int type)
    {
        // The DSP's own curves (livedaw-dsp dynamics / dyneq, analog
        // prototypes): x + (g - 1) F(x), F the band filter at FREQ; the 12 dB
        // shelves are second order with Butterworth poles at FREQ.
        const float w = hz / juce::jmax (1.0f, f), w2 = w * w, w4 = w2 * w2;
        const float g = std::pow (10.0f, gain / 20.0f), g2 = g * g;
        const auto db = [] (float power) { return 10.0f * std::log10 (juce::jmax (1.0e-12f, power)); };
        switch (type)
        {
            case 0: return gain; // full band
            case 1:
            {
                // bell: (s^2 + s g/Q + 1) / (s^2 + s/Q + 1)
                const float re = (1.0f - w2) * (1.0f - w2), wq = w / juce::jmax (0.1f, q);
                return db ((re + g2 * wq * wq) / (re + wq * wq));
            }
            case 2: return db ((g2 + w2) / (1.0f + w2));        // low shelf, 6 dB/oct
            case 3: return db ((g2 + w4) / (1.0f + w4));        // low shelf, 12 dB/oct
            case 4: return db ((1.0f + g2 * w2) / (1.0f + w2)); // high shelf, 6 dB/oct
            case 5: return db ((1.0f + g2 * w4) / (1.0f + w4)); // high shelf, 12 dB/oct
            default: return 0.0f;
        }
    }

    const Colour kCurve { 0xfff0a030 }, kCurve2 { 0xff4a8af0 }, kCurve3 { 0xff60d080 }, kGridLine { 0xff3a3a3a }, kGridMain { 0xff6a6a6a },
        kGridText { 0xff9a9a9a };

    /** The screen of a graph: a bezel and a black glass. */
    void graphScreen (juce::Graphics& g, Rectangle<float> r)
    {
        g.setColour (Colour (0xff3a3a3a));
        g.fillRoundedRectangle (r.expanded (4.0f), 10.0f);
        g.setColour (Colour (0xff030303));
        g.fillRoundedRectangle (r, 8.0f);
    }

    /** A parameter a graph shows: its spec and value (nullptr past the end). */
    struct GraphInput
    {
        const PaintContext& ctx;
        const Element& e;

        const models::Param* param (size_t k) const
        {
            if (ctx.spec == nullptr || k >= e.params.size())
                return nullptr;
            const int i = e.params[k];
            return i >= 0 && i < (int) ctx.spec->params.size() ? &ctx.spec->params[(size_t) i] : nullptr;
        }
        bool has (size_t k) const { return param (k) != nullptr; }
        float value (size_t k, float fallback = 0.0f) const { return has (k) ? ctx.value (e.params[k]) : fallback; }
        /** 0..1 along the control's travel. */
        float fraction (size_t k, float fallback = 0.0f) const
        {
            const auto* p = param (k);
            return p != nullptr ? juce::jlimit (0.0f, 1.0f, p->toNormalised (value (k))) : fallback;
        }
        /** The value as a number: a choice's text ("1/2" = 0.5, "4.0", "ALL" = 100). */
        float number (size_t k, float fallback = 0.0f) const
        {
            const auto* p = param (k);
            if (p == nullptr)
                return fallback;
            if (p->isContinuous() || p->kind == models::ParamKind::Integer)
                return value (k);
            return parseNumber (consoleText (*p, value (k)), fallback);
        }
        juce::String text (size_t k) const
        {
            const auto* p = param (k);
            return p != nullptr ? consoleText (*p, value (k)) : juce::String();
        }
        /** A time in ms: the value of an ms or s parameter, else the travel
            mapped to 0..`full` ms (logarithmically from `lo` when that is > 0). */
        float ms (size_t k, float full, float lo = 0.0f) const
        {
            const auto* p = param (k);
            if (p == nullptr)
                return 0.0f;
            if (p->unit == "ms")
                return value (k);
            if (p->unit == "s")
                return value (k) * 1000.0f;
            return lo > 0.0f ? lo * std::pow (full / lo, fraction (k)) : fraction (k) * full;
        }
        /** A percentage as 0..1 (or the travel). */
        float amount (size_t k, float fallback = 0.0f) const
        {
            const auto* p = param (k);
            if (p == nullptr)
                return fallback;
            if (p->unit == "%")
                return juce::jlimit (0.0f, 1.25f, value (k) / 100.0f);
            return fraction (k);
        }

        static float parseNumber (const juce::String& s, float fallback)
        {
            const auto t = s.trim().upToFirstOccurrenceOf (" ", false, false);
            if (t.containsChar ('/'))
            {
                const float den = t.fromFirstOccurrenceOf ("/", false, false).getFloatValue();
                return den != 0.0f ? t.upToFirstOccurrenceOf ("/", false, false).getFloatValue() / den : fallback;
            }
            if (t.isEmpty() || ! (juce::CharacterFunctions::isDigit (t[0]) || t[0] == '-' || t[0] == '.'))
                return t.equalsIgnoreCase ("ALL") || t.equalsIgnoreCase ("LIM") ? 100.0f : fallback;
            return t.getFloatValue();
        }
    };

    void label (juce::Graphics& g, const juce::String& s, Rectangle<float> r, juce::Justification j = juce::Justification::centred,
                Colour c = kGridText)
    {
        g.setColour (c);
        g.setFont (face (Look::Sans, 11.0f));
        g.drawText (s, r, j, false);
    }

    /** Log frequency grid 20 Hz .. 20 kHz and a dB grid of `span` each way. */
    void frequencyGrid (juce::Graphics& g, Rectangle<float> r, float span, float step)
    {
        auto xOf = [&] (float hz) { return r.getX() + r.getWidth() * std::log10 (hz / 20.0f) / 3.0f; };
        g.setColour (kGridLine);
        for (float dec : { 20.0f, 200.0f, 2000.0f })
            for (int k = 1; k <= 9; ++k)
                g.drawVerticalLine (juce::roundToInt (xOf (dec * (float) k)), r.getY(), r.getBottom());
        for (float db = -span; db <= span + 0.01f; db += step)
        {
            const float y = r.getCentreY() - db / span * r.getHeight() * 0.5f;
            g.setColour (std::abs (db) < 0.01f ? kGridMain : kGridLine);
            g.drawHorizontalLine (juce::roundToInt (y), r.getX(), r.getRight());
        }
        g.setColour (kGridMain);
        for (float hz : { 100.0f, 1000.0f, 10000.0f })
            g.drawVerticalLine (juce::roundToInt (xOf (hz)), r.getY(), r.getBottom());
        for (auto [hz, s] : { std::pair { 100.0f, "100" }, { 1000.0f, "1k" }, { 10000.0f, "10k" } })
            label (g, s, { xOf (hz) + 3.0f, r.getBottom() - 16.0f, 40.0f, 14.0f }, juce::Justification::centredLeft);
        label (g, "+" + juce::String (juce::roundToInt (span)), { r.getX() + 4.0f, r.getY() + 2.0f, 40.0f, 14.0f }, juce::Justification::centredLeft);
        label (g, "-" + juce::String (juce::roundToInt (span)), { r.getX() + 4.0f, r.getBottom() - 16.0f, 40.0f, 14.0f },
               juce::Justification::centredLeft);
    }

    /** A curve over the frequency grid: `db(hz)`; filled down to 0 dB when `fill`. */
    juce::Path frequencyCurve (Rectangle<float> r, float span, const std::function<float (float)>& db)
    {
        juce::Path p;
        for (int i = 0; i <= 160; ++i)
        {
            const float hz = 20.0f * std::pow (1000.0f, (float) i / 160.0f);
            const float v = db (hz);
            const float y = juce::jlimit (r.getY(), r.getBottom(), r.getCentreY() - (std::isfinite (v) ? v : 0.0f) / span * r.getHeight() * 0.5f);
            const float x = r.getX() + r.getWidth() * (float) i / 160.0f;
            if (i == 0)
                p.startNewSubPath (x, y);
            else
                p.lineTo (x, y);
        }
        return p;
    }

    void fillToZero (juce::Graphics& g, const juce::Path& curve, Rectangle<float> r, Colour c)
    {
        juce::Path area (curve);
        area.lineTo (r.getRight(), r.getCentreY());
        area.lineTo (r.getX(), r.getCentreY());
        area.closeSubPath();
        g.setColour (c);
        g.fillPath (area);
    }

    void drawDynEq (juce::Graphics& g, Rectangle<float> r, const GraphInput& in)
    {
        constexpr float span = 18.0f;
        frequencyGrid (g, r, span, 6.0f);
        const Colour colours[] = { kCurve, kCurve2, kCurve3 };
        const int bands = (int) in.e.params.size() / 4;
        for (int b = bands - 1; b >= 0; --b)
        {
            const size_t o = (size_t) b * 4;
            const float f = in.value (o), gain = in.value (o + 1), q = in.value (o + 2);
            const int type = juce::roundToInt (in.value (o + 3) - (in.has (o + 3) ? in.param (o + 3)->min : 0.0f));
            const auto colour = colours[b % 3];
            // The range the band moves through, then where it is now: the
            // band's own live gain when the engine reports it, else (cuts
            // only) the slot's gain reduction.
            const auto full = frequencyCurve (r, span, [&] (float hz) { return bandDb (hz, f, gain, q, type); });
            fillToZero (g, full, r, colour.withAlpha (0.16f));
            juce::Path dashed;
            const float dashes[] = { 6.0f, 5.0f };
            juce::PathStrokeType (2.0f).createDashedStroke (dashed, full, dashes, 2);
            g.setColour (colour.withAlpha (0.7f));
            g.fillPath (dashed);
            const float now = in.ctx.bandGain ? juce::jlimit (juce::jmin (0.0f, gain), juce::jmax (0.0f, gain), in.ctx.bandGain (b))
                                              : (gain < 0.0f ? -juce::jmin (in.ctx.reduction, -gain) : 0.0f);
            const auto live = frequencyCurve (r, span, [&] (float hz) { return bandDb (hz, f, now, q, type); });
            g.setColour (colour);
            g.strokePath (live, juce::PathStrokeType (3.0f));
        }
    }

    /** The model's response, cached per model and values (a measured response takes a moment). */
    void drawResponse (juce::Graphics& g, Rectangle<float> r, const GraphInput& in)
    {
        constexpr float span = 18.0f;
        frequencyGrid (g, r, span, 6.0f);
        if (in.ctx.spec == nullptr)
            return;
        struct Cache
        {
            int table = -1, model = -1;
            std::vector<float> values, db;
            bool ok = false;
        };
        static Cache cache;
        static std::vector<float> freqs = []
        {
            std::vector<float> f (161);
            for (size_t i = 0; i < f.size(); ++i)
                f[i] = 20.0f * std::pow (1000.0f, (float) i / 160.0f);
            return f;
        }();
        const auto& spec = *in.ctx.spec;
        std::vector<float> values (spec.params.size());
        for (size_t i = 0; i < values.size(); ++i)
            values[i] = in.ctx.value ((int) i);
        if (cache.table != spec.table || cache.model != spec.index || cache.values != values)
        {
            cache.table = spec.table;
            cache.model = spec.index;
            cache.values = values;
            cache.db.assign (freqs.size(), 0.0f);
            cache.ok = models::modelResponse (static_cast<models::Table> (spec.table), spec.index, values.data(), (int) values.size(), freqs.data(),
                                              cache.db.data(), (int) freqs.size());
        }
        if (! cache.ok)
            return;
        auto curve = frequencyCurve (r, span, [&] (float hz)
        {
            const float pos = std::log10 (hz / 20.0f) / 3.0f * 160.0f;
            const int i = juce::jlimit (0, 159, (int) pos);
            return juce::jmap (pos - (float) i, cache.db[(size_t) i], cache.db[(size_t) i + 1]);
        });
        fillToZero (g, curve, r, kCurve.withAlpha (0.18f));
        g.setColour (kCurve);
        g.strokePath (curve, juce::PathStrokeType (3.0f));
    }

    /** A time axis 0..`window` ms with a few labelled ticks. */
    void timeGrid (juce::Graphics& g, Rectangle<float> r, float window)
    {
        float step = 1.0f;
        for (float s : { 1.0f, 2.0f, 5.0f, 10.0f, 20.0f, 50.0f, 100.0f, 200.0f, 500.0f, 1000.0f, 2000.0f, 5000.0f, 10000.0f })
            if (window / s <= 8.0f)
            {
                step = s;
                break;
            }
        for (float t = 0.0f; t <= window + 0.01f; t += step)
        {
            const float x = r.getX() + r.getWidth() * t / juce::jmax (1.0f, window);
            g.setColour (t == 0.0f ? kGridMain : kGridLine);
            g.drawVerticalLine (juce::roundToInt (x), r.getY(), r.getBottom());
            if (t > 0.0f)
                label (g, t >= 1000.0f ? juce::String (t / 1000.0f, t >= 10000.0f ? 0 : 1) + " s" : juce::String (juce::roundToInt (t)) + " ms",
                       { x + 3.0f, r.getBottom() - 16.0f, 70.0f, 14.0f }, juce::Justification::centredLeft);
        }
        for (int i = 1; i < 4; ++i)
        {
            g.setColour (kGridLine);
            g.drawHorizontalLine (juce::roundToInt (r.getY() + r.getHeight() * (float) i / 4.0f), r.getX(), r.getRight());
        }
    }

    /** Echoes: a stem per tap, height = level (0 .. -48 dB). */
    void drawTaps (juce::Graphics& g, Rectangle<float> r, const GraphInput& in)
    {
        struct Tap
        {
            float t, db;
            int lane;
        };
        std::vector<Tap> taps;
        const int variant = juce::jmax (0, in.e.choice);
        const float full = in.e.range > 20.0f ? in.e.range : 1000.0f;
        if (variant == 1)
        {
            // A row of taps over TIME after the pre-delay, levels along the slope.
            const float time = juce::jmax (1.0f, in.ms (0, full));
            const int n = juce::jlimit (1, 32, juce::roundToInt (in.value (1, 1.0f)));
            const float slope = in.value (2), pre = in.ms (3, 500.0f);
            for (int k = 1; k <= n; ++k)
            {
                const float along = n > 1 ? (float) (k - 1) / (float) (n - 1) : 0.0f;
                taps.push_back ({ pre + time * (float) k / (float) n, slope >= 0.0f ? -slope * (1.0f - along) : slope * along, 0 });
            }
        }
        else
        {
            const float time = juce::jmax (1.0f, in.ms (0, full, in.e.dbMin));
            const float fb = juce::jlimit (0.0f, 0.97f, in.amount (1));
            float left = time, right = time;
            if (variant == 2)
            {
                const float base = time * in.number (2, 1.0f);
                const auto pattern = in.text (3);
                const float a = GraphInput::parseNumber (pattern.upToFirstOccurrenceOf (":", false, false), 1.0f);
                const float b = GraphInput::parseNumber (pattern.fromFirstOccurrenceOf (":", false, false), 1.0f);
                left = juce::jmax (1.0f, base * a);
                right = juce::jmax (1.0f, base * b);
            }
            for (int lane = 0; lane < (variant == 2 ? 2 : 1); ++lane)
            {
                const float step = lane == 0 ? left : right;
                float db = 0.0f;
                for (int k = 1; k <= 24 && db > -48.0f; ++k)
                {
                    taps.push_back ({ step * (float) k, db, lane });
                    db += fb > 0.0f ? 20.0f * std::log10 (fb) : -100.0f;
                }
            }
        }
        float window = 100.0f;
        for (auto& t : taps)
            window = juce::jmax (window, t.t * 1.08f);
        window = juce::jmin (window, 12000.0f);
        timeGrid (g, r, window);
        const bool two = variant == 2;
        for (auto& t : taps)
        {
            const float x = r.getX() + r.getWidth() * t.t / window;
            if (x > r.getRight())
                continue;
            auto lane = two ? (t.lane == 0 ? r.withHeight (r.getHeight() * 0.5f) : r.withTrimmedTop (r.getHeight() * 0.5f)) : r;
            lane = lane.withTrimmedBottom (two && t.lane == 0 ? 0.0f : 18.0f).reduced (0.0f, 6.0f);
            const float h = lane.getHeight() * juce::jlimit (0.0f, 1.0f, 1.0f + t.db / 48.0f);
            const auto colour = t.lane == 0 ? kCurve : kCurve2;
            g.setColour (colour);
            g.drawLine (x, lane.getBottom(), x, lane.getBottom() - h, 3.0f);
            g.fillEllipse (Rectangle<float> (9.0f, 9.0f).withCentre ({ x, lane.getBottom() - h }));
        }
        if (two)
        {
            label (g, "L", { r.getRight() - 24.0f, r.getY() + 4.0f, 20.0f, 14.0f }, juce::Justification::centred, kCurve);
            label (g, "R", { r.getRight() - 24.0f, r.getCentreY() + 4.0f, 20.0f, 14.0f }, juce::Justification::centred, kCurve2);
        }
    }

    /** A reverb's envelope: pre-delay, then the tail (level 0 .. -60 dB). */
    void drawDecay (juce::Graphics& g, Rectangle<float> r, const GraphInput& in)
    {
        const int variant = juce::jmax (0, in.e.choice);
        const float pre = in.ms (0, 200.0f);
        const float decay = juce::jmax (10.0f, in.ms (variant == 0 ? 1 : 2, 1000.0f));
        const float window = juce::jmax (100.0f, (pre + decay * (variant == 0 ? 1.15f : 1.3f)));
        timeGrid (g, r, window);
        auto body = r.withTrimmedBottom (18.0f).reduced (0.0f, 8.0f);
        auto xOf = [&] (float t) { return body.getX() + body.getWidth() * t / window; };
        auto yOf = [&] (float db) { return body.getY() + body.getHeight() * juce::jlimit (0.0f, 1.0f, -db / 60.0f); };
        juce::Path env;
        env.startNewSubPath (xOf (0.0f), body.getBottom());
        env.lineTo (xOf (pre), body.getBottom());
        if (variant == 0)
        {
            for (int i = 0; i <= 60; ++i)
            {
                const float t = pre + decay * 1.1f * (float) i / 60.0f;
                env.lineTo (xOf (t), yOf (-60.0f * (t - pre) / decay));
            }
        }
        else
        {
            // Gated: a quick rise (ATTACK), held, cut at DECAY. Reverse: swells (RISE) to the cut.
            const float shape = in.fraction (1, 0.5f);
            const float rise = variant == 1 ? decay * (0.02f + 0.4f * shape) : decay * (0.5f + 0.5f * shape);
            env.lineTo (xOf (pre), yOf (variant == 1 ? -30.0f : -50.0f));
            env.lineTo (xOf (pre + rise), yOf (0.0f));
            env.lineTo (xOf (pre + decay), yOf (variant == 1 ? -4.0f : 0.0f));
            env.lineTo (xOf (pre + decay * 1.04f), yOf (-60.0f));
        }
        env.lineTo (env.getCurrentPosition().x, body.getBottom());
        env.closeSubPath();
        g.setGradientFill (juce::ColourGradient (kCurve.withAlpha (0.55f), body.getX(), body.getY(), kCurve.withAlpha (0.08f), body.getX(),
                                                 body.getBottom(), false));
        g.fillPath (env);
        g.setColour (kCurve);
        g.strokePath (env, juce::PathStrokeType (2.5f));
        // Early reflections after the pre-delay.
        if (variant == 0)
        {
            juce::Random rnd (3);
            for (int k = 0; k < 8; ++k)
            {
                const float t = pre + decay * 0.012f * (float) (k + 1) * (1.0f + rnd.nextFloat());
                g.setColour (kCurve2.withAlpha (0.9f));
                g.drawVerticalLine (juce::roundToInt (xOf (t)), yOf (-60.0f * (t - pre) / decay - 2.0f), body.getBottom());
            }
        }
        g.setColour (kGridText);
        g.drawVerticalLine (juce::roundToInt (xOf (pre)), body.getY(), body.getBottom());
        label (g, "PRE", { xOf (pre) + 3.0f, body.getY(), 40.0f, 14.0f }, juce::Justification::centredLeft);
    }

    /** One LFO cycle at `phase` (0..1): -1..1. `wave` picks the shape (choice index) or skews it (0..1, 0.5 = even). */
    float lfoShape (float phase, int choice, float skew)
    {
        phase -= std::floor (phase);
        switch (choice)
        {
            case 0: return 1.0f - 4.0f * std::abs (phase - 0.5f);           // TRI
            case 1: return std::sin (kTwoPi * phase);                       // SIN
            case 2: return 2.0f * phase - 1.0f;                             // SAW+
            case 3: return 1.0f - 2.0f * phase;                             // SAW-
            case 4: return phase < 0.5f ? 4.0f * phase - 1.0f : 1.0f;        // RMP
            case 5: return phase < 0.5f ? 1.0f : -1.0f;                      // SQU
            case 6: return std::sin (kTwoPi * std::floor (phase * 4.0f) * 0.37f + 1.3f); // RND (steps)
            default: break;
        }
        // Skewed triangle-sine: the rising part takes `rise` of the cycle.
        const float rise = juce::jlimit (0.05f, 0.95f, skew);
        const float t = phase < rise ? phase / rise * 0.5f : 0.5f + (phase - rise) / (1.0f - rise) * 0.5f;
        return -std::cos (kTwoPi * t);
    }

    void drawLfo (juce::Graphics& g, Rectangle<float> r, const GraphInput& in)
    {
        // params: speed (Hz), depth L, depth R (or -1), phase (deg), wave.
        const float hz = juce::jmax (0.01f, in.value (0, 1.0f));
        const float depthL = juce::jmax (0.08f, in.amount (1, 1.0f)), depthR = in.has (2) ? juce::jmax (0.08f, in.amount (2)) : depthL;
        const float phaseOffset = in.has (3) ? in.value (3) / 360.0f : 0.0f;
        int choice = -1;
        float skew = 0.5f;
        if (const auto* w = in.param (4))
        {
            if (w->kind == models::ParamKind::Choice)
                choice = juce::roundToInt (in.value (4) - w->min);
            else
                skew = in.fraction (4, 0.5f);
        }
        const float window = 2.0f / hz * 1000.0f; // two cycles
        timeGrid (g, r, window);
        auto body = r.withTrimmedBottom (18.0f).reduced (0.0f, 8.0f);
        g.setColour (kGridMain);
        g.drawHorizontalLine (juce::roundToInt (body.getCentreY()), body.getX(), body.getRight());
        for (int lane = 1; lane >= 0; --lane)
        {
            juce::Path p;
            const float depth = lane == 0 ? depthL : depthR;
            for (int i = 0; i <= 200; ++i)
            {
                const float ph = 2.0f * (float) i / 200.0f + (lane == 1 ? phaseOffset : 0.0f);
                const float y = body.getCentreY() - lfoShape (ph, choice, skew) * depth * body.getHeight() * 0.45f;
                const float x = body.getX() + body.getWidth() * (float) i / 200.0f;
                if (i == 0)
                    p.startNewSubPath (x, y);
                else
                    p.lineTo (x, y);
            }
            g.setColour (lane == 0 ? kCurve : kCurve2);
            g.strokePath (p, juce::PathStrokeType (lane == 0 ? 3.0f : 2.0f));
        }
        label (g, "L", { body.getRight() - 40.0f, body.getY(), 16.0f, 14.0f }, juce::Justification::centred, kCurve);
        label (g, "R", { body.getRight() - 22.0f, body.getY(), 16.0f, 14.0f }, juce::Justification::centred, kCurve2);
    }

    void drawTransfer (juce::Graphics& g, Rectangle<float> r, const GraphInput& in)
    {
        r = r.withSizeKeepingCentre (juce::jmin (r.getWidth(), r.getHeight()), juce::jmin (r.getWidth(), r.getHeight()));
        constexpr float range = 60.0f;
        g.setColour (kGridLine);
        for (int i = 1; i < 6; ++i)
        {
            g.drawVerticalLine (juce::roundToInt (r.getX() + r.getWidth() * (float) i / 6.0f), r.getY(), r.getBottom());
            g.drawHorizontalLine (juce::roundToInt (r.getY() + r.getHeight() * (float) i / 6.0f), r.getX(), r.getRight());
        }
        g.setColour (kGridMain);
        g.drawLine (r.getX(), r.getBottom(), r.getRight(), r.getY(), 1.0f);
        const bool limiter = in.e.choice == 1;
        const float thr = limiter ? -in.value (0) : in.value (0);
        const float ratio = limiter ? 1000.0f : juce::jmax (1.0f, in.number (1, 2.0f));
        const float knee = limiter ? in.value (2) * 0.6f : (in.has (2) ? in.value (2) * 2.0f : 0.0f);
        const float out = limiter ? in.value (1) : 0.0f;
        auto curve = [&] (float x)
        {
            const float over = x - thr;
            float y = x;
            if (knee > 0.0f && std::abs (over) <= knee * 0.5f)
                y = x + (1.0f / ratio - 1.0f) * std::pow (over + knee * 0.5f, 2.0f) / (2.0f * knee);
            else if (over > 0.0f)
                y = thr + over / ratio;
            return y + out;
        };
        juce::Path p;
        for (int i = 0; i <= 120; ++i)
        {
            const float x = -range + range * (float) i / 120.0f;
            const float y = juce::jlimit (-range, 0.0f, curve (x));
            const auto pt = Point<float> (r.getX() + r.getWidth() * (x + range) / range, r.getY() + r.getHeight() * -y / range);
            if (i == 0)
                p.startNewSubPath (pt);
            else
                p.lineTo (pt);
        }
        g.setColour (kCurve);
        g.strokePath (p, juce::PathStrokeType (3.0f));
        label (g, "IN", { r.getRight() - 30.0f, r.getBottom() - 16.0f, 26.0f, 14.0f });
        label (g, "OUT", { r.getX() + 4.0f, r.getY() + 2.0f, 30.0f, 14.0f });
    }

    void drawDeEss (juce::Graphics& g, Rectangle<float> r, const GraphInput& in)
    {
        constexpr float span = 18.0f;
        frequencyGrid (g, r, span, 6.0f);
        const float f = in.value (0, 5000.0f), range = juce::jmax (0.0f, in.value (1));
        const bool full = in.text (2).startsWithIgnoreCase ("FULL");
        // The band the detector listens to: everything above FREQ (12 dB/oct high pass).
        const auto band = frequencyCurve (r, span, [&] (float hz)
        {
            const float w4 = std::pow (hz / juce::jmax (1.0f, f), 4.0f);
            return 6.0f * w4 / (1.0f + w4);
        });
        fillToZero (g, band, r, kCurve2.withAlpha (0.25f));
        // HF ONLY: the DSP's 12 dB/oct high shelf, its corner a little under
        // FREQ so that at the full RANGE half the cut is at FREQ.
        const float corner = juce::jmax (1.0f, f) * std::pow (10.0f, -range * 0.25f / 20.0f);
        auto cut = [&] (float depth)
        {
            return frequencyCurve (r, span, [&] (float hz)
            {
                if (full)
                    return -depth;
                const float w4 = std::pow (hz / corner, 4.0f), g2 = std::pow (10.0f, -depth / 10.0f);
                return 10.0f * std::log10 ((1.0f + g2 * w4) / (1.0f + w4));
            });
        };
        juce::Path dashed;
        const float dashes[] = { 6.0f, 5.0f };
        juce::PathStrokeType (2.0f).createDashedStroke (dashed, cut (range), dashes, 2);
        g.setColour (kCurve.withAlpha (0.7f));
        g.fillPath (dashed);
        g.setColour (kCurve);
        g.strokePath (cut (juce::jmin (range, in.ctx.reduction)), juce::PathStrokeType (3.0f));
    }

    void drawFilter (juce::Graphics& g, Rectangle<float> r, const GraphInput& in)
    {
        constexpr float span = 24.0f;
        frequencyGrid (g, r, span, 12.0f);
        const float f = juce::jmax (10.0f, in.value (0, 1000.0f));
        const int type = in.has (1) ? juce::roundToInt (in.value (1) - in.param (1)->min) : 0;
        const bool steep = in.has (2) && juce::roundToInt (in.value (2) - in.param (2)->min) == 1;
        const float q = 0.7f + in.fraction (3) * 9.0f;
        auto curve = frequencyCurve (r, span, [&] (float hz)
        {
            const float w = hz / f;
            const float re = 1.0f - w * w, im = w / q;
            const float den = std::sqrt (re * re + im * im);
            float num = 1.0f;
            switch (type)
            {
                case 1: num = w * w; break;
                case 2: num = w / q; break;
                case 3: num = std::abs (1.0f - w * w); break;
                default: break;
            }
            const float db = 20.0f * std::log10 (juce::jmax (1.0e-5f, num / den));
            return steep ? db * 2.0f : db;
        });
        fillToZero (g, curve, r, kCurve.withAlpha (0.16f));
        g.setColour (kCurve);
        g.strokePath (curve, juce::PathStrokeType (3.0f));
    }

    void drawLevels (juce::Graphics& g, Rectangle<float> r, const GraphInput& in)
    {
        const int n = (int) in.e.params.size();
        if (n == 0)
            return;
        auto body = r.reduced (10.0f, 10.0f).withTrimmedBottom (14.0f);
        const float w = body.getWidth() / (float) n;
        for (int i = 0; i < n; ++i)
        {
            const auto* p = in.param ((size_t) i);
            if (p == nullptr)
                continue;
            const bool bipolar = p->min < 0.0f && p->max > 0.0f;
            const float pos = in.fraction ((size_t) i);
            auto col = Rectangle<float> (body.getX() + w * (float) i, body.getY(), w, body.getHeight()).reduced (w * 0.2f, 0.0f);
            g.setColour (kGridLine);
            g.fillRect (col);
            const float base = bipolar ? col.getCentreY() : col.getBottom();
            const float top = col.getBottom() - pos * col.getHeight();
            g.setColour (kCurve);
            g.fillRect (Rectangle<float>::leftTopRightBottom (col.getX(), juce::jmin (base, top) - 1.0f, col.getRight(), juce::jmax (base, top) + 1.0f));
            // The caption, or the bar's number where it doesn't fit.
            auto caption = consoleLabel (*p).toUpperCase();
            if (juce::GlyphArrangement::getStringWidth (face (Look::Sans, 11.0f), caption) > w - 2.0f)
                caption = juce::String (i + 1);
            label (g, caption, { col.getX() - 10.0f, col.getBottom() + 2.0f, col.getWidth() + 20.0f, 13.0f });
        }
    }

    void drawGraph (juce::Graphics& g, const Element& e, const PaintContext& ctx)
    {
        const Rectangle<float> r (e.x, e.y, e.w, e.h);
        graphScreen (g, r);
        juce::Graphics::ScopedSaveState keep (g);
        g.reduceClipRegion (r.reduced (2.0f).toNearestInt());
        const auto inner = r.reduced (6.0f, 4.0f);
        const GraphInput in { ctx, e };
        switch (e.graph)
        {
            case GraphKind::DynEq: drawDynEq (g, inner, in); break;
            case GraphKind::Response: drawResponse (g, inner, in); break;
            case GraphKind::Taps: drawTaps (g, inner, in); break;
            case GraphKind::Decay: drawDecay (g, inner, in); break;
            case GraphKind::Lfo: drawLfo (g, inner, in); break;
            case GraphKind::Transfer: drawTransfer (g, inner, in); break;
            case GraphKind::DeEss: drawDeEss (g, inner, in); break;
            case GraphKind::Filter: drawFilter (g, inner, in); break;
            case GraphKind::Levels: drawLevels (g, inner, in); break;
        }
    }

    void drawBubble (juce::Graphics& g, Rectangle<float> near, const juce::String& text)
    {
        g.setFont (face (Look::Sans, 22.0f));
        const float w = juce::GlyphArrangement::getStringWidth (g.getCurrentFont(), text) + 24.0f;
        auto b = Rectangle<float> (w, 32.0f).withCentre ({ near.getCentreX(), near.getY() - 22.0f });
        b = b.withX (juce::jlimit (4.0f, kFrameW - w - 4.0f, b.getX())).withY (juce::jmax (4.0f, b.getY()));
        g.setColour (juce::Colours::black.withAlpha (0.85f));
        g.fillRoundedRectangle (b, 5.0f);
        g.setColour (Colour (0xfff5e14a));
        g.drawRoundedRectangle (b, 5.0f, 1.5f);
        g.drawText (text, b, juce::Justification::centred);
    }
} // namespace

//==============================================================================
float controlPosition (const models::Param& p, float value)
{
    return juce::jlimit (0.0f, 1.0f, p.toNormalised (value));
}

juce::Rectangle<float> hitArea (const Element& e)
{
    switch (e.type)
    {
        case Type::Knob:
        {
            const float r = e.w * 0.5f * juce::jmax (1.25f, e.ticks > 0 || ! e.labels.isEmpty() ? 1.35f : 1.2f);
            return { e.x - r, e.y - r, r * 2.0f, r * 2.0f };
        }
        case Type::Toggle:
            return e.vertical ? Rectangle<float> (e.w * 1.6f, e.w * 2.8f).withCentre ({ e.x, e.y })
                              : Rectangle<float> (e.w * 2.8f, e.w * 1.6f).withCentre ({ e.x, e.y });
        case Type::Lamp: return Rectangle<float> (e.w * 2.0f, e.w * 2.0f).withCentre ({ e.x, e.y });
        case Type::Key: return Rectangle<float> (e.x, e.y, e.w, e.h).expanded (4.0f);
        case Type::Value: return { e.x, e.y, e.w, e.h };
        case Type::Fader: return Rectangle<float> (e.x, e.y, e.w, e.h);
        case Type::Graph:
        case Type::Vu:
        case Type::Leds:
        case Type::Text:
        case Type::Path:
        case Type::Box:
        case Type::Screw:
        case Type::Display: break;
    }
    return {};
}

bool elementOn (const Element& e, const models::Param& p, float value)
{
    const int v = juce::roundToInt (value);
    if (e.choice >= 0)
        return v == juce::roundToInt (p.min) + e.choice || (e.alsoOn >= 0 && v == juce::roundToInt (p.min) + e.alsoOn);
    const bool atMax = v == juce::roundToInt (juce::jmax (p.min, p.max));
    return e.invert ? ! atMax : atMax;
}

float clickValue (const Element& e, const models::Param& p, float value)
{
    const float lo = juce::jmin (p.min, p.max), hi = juce::jmax (p.min, p.max);
    const bool twoState = juce::roundToInt (hi - lo) == 1;
    if (e.choice >= 0 && ! twoState)
        return p.min + (float) e.choice;
    // Two states: flip.
    return juce::roundToInt (value) == juce::roundToInt (hi) ? lo : hi;
}

void paintDesign (juce::Graphics& g, const Design& d, const PaintContext& ctx)
{
    if (d.backdrop)
    {
        g.drawImage (backdropImage (g.getInternalContext().getPhysicalPixelScaleFactor()), { 0.0f, 0.0f, kFrameW, kFrameH },
                     juce::RectanglePlacement::stretchToFit);
        drawFrameMarks (g);
    }
    drawPlate (g, d, ctx.thumbnail);

    auto paramOf = [&] (const Element& e) -> const models::Param*
    {
        if (ctx.spec == nullptr || e.param < 0 || e.param >= (int) ctx.spec->params.size())
            return nullptr;
        return &ctx.spec->params[(size_t) e.param];
    };

    const Element* bubbleAt = nullptr;
    for (const auto& e : d.elements)
    {
        if (e.page >= 0 && e.page != ctx.page)
            continue;
        const auto* p = paramOf (e);
        const float v = p != nullptr ? ctx.value (e.param) : 0.0f;
        switch (e.type)
        {
            case Type::Knob: drawKnob (g, e, p != nullptr ? controlPosition (*p, v) : e.fixed); break;
            case Type::Toggle: drawToggle (g, e, p != nullptr ? elementOn (e, *p, v) : e.fixed > 0.5f); break;
            case Type::Key:
                drawKey (g, e, e.look == Look::TabKey ? e.choice == ctx.page : (p != nullptr ? elementOn (e, *p, v) : e.fixed > 0.5f));
                break;
            case Type::Lamp: drawLed (g, { e.x, e.y }, e.w * 0.5f, e.colour, p != nullptr ? elementOn (e, *p, v) : e.fixed > 0.5f); break;
            case Type::Fader: drawFader (g, e, p != nullptr ? controlPosition (*p, v) : e.fixed); break;
            case Type::Vu: drawVu (g, e, ctx.reduction); break;
            case Type::Leds: drawLeds (g, e, ctx.reduction); break;
            case Type::Text: drawText (g, e); break;
            case Type::Box: drawBox (g, e); break;
            case Type::Screw: drawScrew (g, { e.x, e.y }, e.w * 0.5f, d.panel); break;
            case Type::Display: drawDisplay (g, e, ctx); break;
            case Type::Graph: drawGraph (g, e, ctx); break;
            case Type::Value:
                if (p != nullptr)
                    drawValueBox (g, e, *p, v);
                break;
            case Type::Path:
            {
                auto path = juce::Drawable::parseSVGPath (e.text);
                g.setColour (e.colour);
                if (e.size > 0.0f)
                    g.strokePath (path, juce::PathStrokeType (e.size));
                else
                    g.fillPath (path);
                break;
            }
        }
        if (p != nullptr && e.param == ctx.touched && bubbleAt == nullptr && e.type != Type::Value)
            bubbleAt = &e;
    }
    if (ctx.showValue && bubbleAt != nullptr)
    {
        const auto& p = ctx.spec->params[(size_t) bubbleAt->param];
        drawBubble (g, hitArea (*bubbleAt), consoleText (p, ctx.value (bubbleAt->param)));
    }
}

} // namespace livedaw::ui::fp
