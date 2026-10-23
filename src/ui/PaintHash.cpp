#include "ui/PaintHash.h"

#include <cstring>

namespace livedaw::ui
{
namespace
{
    /** A LowLevelGraphicsContext that draws nothing: each call is folded into
        a 64-bit FNV-1a hash. It keeps the clip's bounds so components outside
        it are skipped as they would be on screen. */
    class HashingContext final : public juce::LowLevelGraphicsContext
    {
    public:
        explicit HashingContext (juce::Rectangle<int> bounds)
        {
            stack.push_back ({ {}, bounds.toFloat() });
        }

        juce::uint64 result() const noexcept { return hash; }

        bool isVectorDevice() const override { return false; }
        float getPhysicalPixelScaleFactor() const override { return 1.0f; }
        uint64_t getFrameId() const override { return 0; }
        std::unique_ptr<juce::ImageType> getPreferredImageTypeForTemporaryImages() const override
        {
            return std::make_unique<juce::SoftwareImageType>();
        }

        void setOrigin (juce::Point<int> o) override { addTransform (juce::AffineTransform::translation ((float) o.x, (float) o.y)); }
        void addTransform (const juce::AffineTransform& t) override
        {
            add ('T');
            addTransformValues (t);
            state().transform = t.followedBy (state().transform);
        }

        bool clipToRectangle (const juce::Rectangle<int>& r) override
        {
            add ('C');
            addRect (r.toFloat());
            return intersectClip (r.toFloat());
        }
        bool clipToRectangleList (const juce::RectangleList<int>& list) override
        {
            add ('L');
            for (auto& r : list)
                addRect (r.toFloat());
            return intersectClip (list.getBounds().toFloat());
        }
        void excludeClipRectangle (const juce::Rectangle<int>& r) override
        {
            add ('X');
            addRect (r.toFloat());
        }
        void clipToPath (const juce::Path& p, const juce::AffineTransform& t) override
        {
            add ('P');
            addPath (p);
            addTransformValues (t);
            intersectClip (p.getBoundsTransformed (t));
        }
        void clipToImageAlpha (const juce::Image& image, const juce::AffineTransform& t) override
        {
            add ('A');
            addImage (image);
            addTransformValues (t);
            intersectClip (image.getBounds().toFloat().transformedBy (t));
        }
        bool clipRegionIntersects (const juce::Rectangle<int>& r) override { return getClipBounds().intersects (r); }
        juce::Rectangle<int> getClipBounds() const override
        {
            const auto& s = stack.back();
            return s.clip.transformedBy (s.transform.inverted()).getSmallestIntegerContainer();
        }
        bool isClipEmpty() const override { return stack.back().clip.isEmpty(); }

        void saveState() override
        {
            add ('S');
            stack.push_back (stack.back());
        }
        void restoreState() override
        {
            add ('R');
            if (stack.size() > 1)
                stack.pop_back();
        }
        void beginTransparencyLayer (float opacity) override
        {
            add ('B');
            addFloat (opacity);
            saveState();
        }
        void endTransparencyLayer() override
        {
            add ('E');
            restoreState();
        }

        void setFill (const juce::FillType& f) override
        {
            add ('F');
            add (f.colour.getARGB());
            if (f.gradient != nullptr)
            {
                const auto& gr = *f.gradient;
                addFloat (gr.point1.x);
                addFloat (gr.point1.y);
                addFloat (gr.point2.x);
                addFloat (gr.point2.y);
                add (gr.isRadial ? 1u : 0u);
                for (int i = 0; i < gr.getNumColours(); ++i)
                {
                    add (gr.getColour (i).getARGB());
                    addFloat ((float) gr.getColourPosition (i));
                }
            }
            if (f.image.isValid())
                addImage (f.image);
            addTransformValues (f.transform);
        }
        void setOpacity (float o) override
        {
            add ('O');
            addFloat (o);
        }
        void setInterpolationQuality (juce::Graphics::ResamplingQuality q) override
        {
            add ('Q');
            add ((juce::uint32) q);
        }

        void fillRect (const juce::Rectangle<int>& r, bool replace) override
        {
            add (replace ? 'r' : 'i');
            addRect (r.toFloat());
        }
        void fillRect (const juce::Rectangle<float>& r) override
        {
            add ('f');
            addRect (r);
        }
        void fillRectList (const juce::RectangleList<float>& list) override
        {
            add ('l');
            for (auto& r : list)
                addRect (r);
        }
        void fillPath (const juce::Path& p, const juce::AffineTransform& t) override
        {
            add ('p');
            addPath (p);
            addTransformValues (t);
        }
        void strokePath (const juce::Path& p, const juce::PathStrokeType& stroke, const juce::AffineTransform& t) override
        {
            // Hashes the outline's inputs instead of stroking it.
            add ('s');
            addPath (p);
            addFloat (stroke.getStrokeThickness());
            add ((juce::uint32) stroke.getJointStyle() * 16u + (juce::uint32) stroke.getEndStyle());
            addTransformValues (t);
        }
        void drawImage (const juce::Image& image, const juce::AffineTransform& t) override
        {
            add ('m');
            addImage (image);
            addTransformValues (t);
        }
        void drawLine (const juce::Line<float>& line) override
        {
            add ('n');
            addFloat (line.getStartX());
            addFloat (line.getStartY());
            addFloat (line.getEndX());
            addFloat (line.getEndY());
        }

        void setFont (const juce::Font& f) override
        {
            state().font = f;
            add ('t');
            addString (f.getTypefaceName());
            addString (f.getTypefaceStyle());
            addFloat (f.getHeight());
            addFloat (f.getHorizontalScale());
            addFloat (f.getExtraKerningFactor());
            add (f.isUnderlined() ? 1u : 0u);
        }
        const juce::Font& getFont() override { return state().font; }

        void drawGlyphs (juce::Span<const uint16_t> glyphs, juce::Span<const juce::Point<float>> positions, const juce::AffineTransform& t) override
        {
            add ('g');
            for (auto glyph : glyphs)
                add ((juce::uint32) glyph);
            for (auto p : positions)
            {
                addFloat (p.x);
                addFloat (p.y);
            }
            addTransformValues (t);
        }

    private:
        struct State
        {
            juce::AffineTransform transform;
            juce::Rectangle<float> clip; // in the context's own coordinates
            juce::Font font { juce::FontOptions() };
        };

        State& state() { return stack.back(); }

        bool intersectClip (juce::Rectangle<float> local)
        {
            auto& s = state();
            s.clip = s.clip.getIntersection (local.transformedBy (s.transform));
            return ! s.clip.isEmpty();
        }

        void addBytes (const void* data, size_t size) noexcept
        {
            auto* p = static_cast<const juce::uint8*> (data);
            for (size_t i = 0; i < size; ++i)
                hash = (hash ^ p[i]) * 1099511628211ull;
        }
        void add (juce::uint32 v) noexcept { addBytes (&v, sizeof (v)); }
        void addFloat (float v) noexcept
        {
            juce::uint32 bits;
            std::memcpy (&bits, &v, sizeof (bits));
            add (bits);
        }
        void addRect (juce::Rectangle<float> r) noexcept
        {
            addFloat (r.getX());
            addFloat (r.getY());
            addFloat (r.getWidth());
            addFloat (r.getHeight());
        }
        void addTransformValues (const juce::AffineTransform& t) noexcept
        {
            for (float v : { t.mat00, t.mat01, t.mat02, t.mat10, t.mat11, t.mat12 })
                addFloat (v);
        }
        void addString (const juce::String& s) noexcept { addBytes (s.toRawUTF8(), s.getNumBytesAsUTF8()); }
        void addPath (const juce::Path& p)
        {
            add (p.isUsingNonZeroWinding() ? 1u : 0u);
            for (juce::Path::Iterator it (p); it.next();)
            {
                add ((juce::uint32) it.elementType);
                for (float v : { it.x1, it.y1, it.x2, it.y2, it.x3, it.y3 })
                    addFloat (v);
            }
        }
        void addImage (const juce::Image& image)
        {
            add ((juce::uint32) image.getWidth());
            add ((juce::uint32) image.getHeight());
            if (! image.isValid())
                return;
            const juce::Image::BitmapData data (image, juce::Image::BitmapData::readOnly);
            const auto rowBytes = (size_t) (data.width * data.pixelStride);
            for (int y = 0; y < data.height; ++y)
                addBytes (data.getLinePointer (y), rowBytes);
        }

        std::vector<State> stack;
        juce::uint64 hash = 14695981039346656037ull;
    };
} // namespace

juce::uint64 paintHash (juce::Component& c)
{
    HashingContext context (c.getLocalBounds());
    juce::Graphics g (context);
    c.paintEntireComponent (g, true);
    return context.result();
}

} // namespace livedaw::ui
