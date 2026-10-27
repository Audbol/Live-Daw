#include "ui/Icons.h"

#include "ui/LookAndFeel.h"

namespace livedaw::ui
{

void drawIcon (juce::Graphics& g, Icon icon, juce::Rectangle<float> area, juce::Colour colour)
{
    const float s = juce::jmin (area.getWidth(), area.getHeight());
    const auto r = area.withSizeKeepingCentre (s, s);
    const float x = r.getX(), y = r.getY();
    auto P = [x, y, s] (float u, float v) { return juce::Point<float> (x + u * s, y + v * s); };
    const float w = juce::jmax (1.2f, s * 0.08f);
    const juce::PathStrokeType stroke (w, juce::PathStrokeType::curved, juce::PathStrokeType::rounded);
    g.setColour (colour);
    juce::Path p;

    switch (icon)
    {
        case Icon::Gear:
        {
            const auto c = P (0.5f, 0.5f);
            for (int i = 0; i < 8; ++i)
            {
                const float a = (float) i * juce::MathConstants<float>::twoPi / 8.0f;
                juce::Path tooth;
                tooth.addRectangle (-s * 0.07f, -s * 0.46f, s * 0.14f, s * 0.14f);
                p.addPath (tooth, juce::AffineTransform::rotation (a).translated (c));
            }
            p.addEllipse (c.x - s * 0.33f, c.y - s * 0.33f, s * 0.66f, s * 0.66f);
            p.addEllipse (c.x - s * 0.13f, c.y - s * 0.13f, s * 0.26f, s * 0.26f);
            p.setUsingNonZeroWinding (false);
            g.fillPath (p);
            return;
        }
        case Icon::Tools:
            p.addRoundedRectangle (P (0.22f, 0.12f).x, P (0.22f, 0.12f).y, s * 0.44f, s * 0.58f, s * 0.05f);
            p.addRoundedRectangle (P (0.34f, 0.3f).x, P (0.34f, 0.3f).y, s * 0.44f, s * 0.58f, s * 0.05f);
            g.strokePath (p, stroke);
            return;
        case Icon::Wrench:
            p.startNewSubPath (P (0.2f, 0.8f));
            p.lineTo (P (0.58f, 0.42f));
            g.strokePath (p, juce::PathStrokeType (s * 0.14f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
            p.clear();
            p.addCentredArc (P (0.68f, 0.32f).x, P (0.68f, 0.32f).y, s * 0.18f, s * 0.18f, 0.0f, 0.9f, 5.6f, true);
            g.strokePath (p, juce::PathStrokeType (s * 0.1f));
            return;
        case Icon::Xlr:
        {
            // The female XLR as the WING draws it: the shell with its latch
            // bump at the top and three hollow pins.
            const auto c = P (0.5f, 0.55f);
            p.addEllipse (c.x - s * 0.36f, c.y - s * 0.36f, s * 0.72f, s * 0.72f);
            for (auto pt : { P (0.365f, 0.47f), P (0.635f, 0.47f), P (0.5f, 0.69f) })
                p.addEllipse (pt.x - s * 0.068f, pt.y - s * 0.068f, s * 0.136f, s * 0.136f);
            g.strokePath (p, juce::PathStrokeType (juce::jmax (1.1f, s * 0.055f)));
            const auto bump = P (0.5f, 0.19f);
            g.fillEllipse (bump.x - s * 0.075f, bump.y - s * 0.06f, s * 0.15f, s * 0.12f);
            return;
        }
        case Icon::Jack:
            p.addRoundedRectangle (P (0.4f, 0.1f).x, P (0.4f, 0.1f).y, s * 0.2f, s * 0.5f, s * 0.04f);
            p.startNewSubPath (P (0.5f, 0.6f));
            p.lineTo (P (0.5f, 0.9f));
            g.strokePath (p, stroke);
            return;
        case Icon::Bus:
            for (float v : { 0.25f, 0.5f, 0.75f })
            {
                p.startNewSubPath (P (0.12f, v));
                p.lineTo (P (0.55f, 0.5f));
            }
            p.lineTo (P (0.9f, 0.5f));
            g.strokePath (p, stroke);
            return;
        case Icon::Speakers:
            for (float u : { 0.08f, 0.54f })
            {
                p.addRoundedRectangle (P (u, 0.15f).x, P (u, 0.15f).y, s * 0.38f, s * 0.7f, s * 0.04f);
                p.addEllipse (P (u + 0.09f, 0.48f).x, P (u + 0.09f, 0.48f).y, s * 0.2f, s * 0.2f);
            }
            g.strokePath (p, stroke);
            return;
        case Icon::Matrix:
            for (int i = 0; i < 3; ++i)
                for (int j = 0; j < 3; ++j)
                    g.fillRect (P (0.14f + (float) i * 0.26f, 0.14f + (float) j * 0.26f).x, P (0.14f + (float) i * 0.26f, 0.14f + (float) j * 0.26f).y, s * 0.2f, s * 0.2f);
            return;
        case Icon::Headphones:
            p.addCentredArc (P (0.5f, 0.55f).x, P (0.5f, 0.55f).y, s * 0.36f, s * 0.36f, 0.0f, -1.7f, 1.7f, true);
            g.strokePath (p, stroke);
            g.fillRoundedRectangle (P (0.1f, 0.5f).x, P (0.1f, 0.5f).y, s * 0.18f, s * 0.32f, s * 0.05f);
            g.fillRoundedRectangle (P (0.72f, 0.5f).x, P (0.72f, 0.5f).y, s * 0.18f, s * 0.32f, s * 0.05f);
            return;
        case Icon::Speaker:
            p.addRoundedRectangle (P (0.22f, 0.08f).x, P (0.22f, 0.08f).y, s * 0.56f, s * 0.84f, s * 0.05f);
            p.addEllipse (P (0.38f, 0.18f).x, P (0.38f, 0.18f).y, s * 0.24f, s * 0.24f);
            p.addEllipse (P (0.33f, 0.5f).x, P (0.33f, 0.5f).y, s * 0.34f, s * 0.34f);
            g.strokePath (p, stroke);
            return;
        case Icon::LockOpen:
        case Icon::LockClosed:
            g.fillRoundedRectangle (P (0.2f, 0.45f).x, P (0.2f, 0.45f).y, s * 0.6f, s * 0.45f, s * 0.06f);
            if (icon == Icon::LockClosed)
                p.addCentredArc (P (0.5f, 0.45f).x, P (0.5f, 0.45f).y, s * 0.2f, s * 0.24f, 0.0f, -1.57f, 1.57f, true);
            else
                p.addCentredArc (P (0.5f, 0.38f).x, P (0.5f, 0.38f).y, s * 0.2f, s * 0.22f, 0.0f, -1.57f, 1.2f, true);
            g.strokePath (p, juce::PathStrokeType (s * 0.1f));
            return;
        case Icon::Plus:
            p.addEllipse (r.reduced (s * 0.1f));
            p.startNewSubPath (P (0.5f, 0.3f));
            p.lineTo (P (0.5f, 0.7f));
            p.startNewSubPath (P (0.3f, 0.5f));
            p.lineTo (P (0.7f, 0.5f));
            g.strokePath (p, stroke);
            return;
        case Icon::LowCut:
            p.startNewSubPath (P (0.15f, 0.85f));
            p.quadraticTo (P (0.3f, 0.3f), P (0.5f, 0.3f));
            p.lineTo (P (0.88f, 0.3f));
            g.strokePath (p, stroke);
            return;
        case Icon::HighCut:
            p.startNewSubPath (P (0.12f, 0.3f));
            p.lineTo (P (0.5f, 0.3f));
            p.quadraticTo (P (0.7f, 0.3f), P (0.85f, 0.85f));
            g.strokePath (p, stroke);
            return;
        case Icon::Tilt:
            p.startNewSubPath (P (0.12f, 0.7f));
            p.lineTo (P (0.88f, 0.3f));
            p.startNewSubPath (P (0.12f, 0.5f));
            p.lineTo (P (0.88f, 0.5f));
            g.strokePath (p, stroke);
            return;
        case Icon::Gate:
            p.startNewSubPath (P (0.18f, 0.85f));
            p.lineTo (P (0.45f, 0.85f));
            p.lineTo (P (0.5f, 0.45f));
            p.lineTo (P (0.82f, 0.15f));
            g.strokePath (p, stroke);
            return;
        case Icon::Compressor:
            p.startNewSubPath (P (0.18f, 0.85f));
            p.lineTo (P (0.5f, 0.45f));
            p.quadraticTo (P (0.62f, 0.32f), P (0.82f, 0.25f));
            p.startNewSubPath (P (0.38f, 0.85f));
            p.lineTo (P (0.62f, 0.5f));
            g.strokePath (p, stroke);
            return;
        case Icon::Insert:
            p.startNewSubPath (P (0.12f, 0.32f));
            p.lineTo (P (0.88f, 0.32f));
            p.startNewSubPath (P (0.2f, 0.55f));
            p.lineTo (P (0.35f, 0.72f));
            p.lineTo (P (0.5f, 0.55f));
            p.lineTo (P (0.65f, 0.72f));
            p.lineTo (P (0.8f, 0.55f));
            g.strokePath (p, stroke);
            return;
        case Icon::Eq:
            p.startNewSubPath (P (0.1f, 0.7f));
            p.cubicTo (P (0.2f, 0.7f), P (0.22f, 0.25f), P (0.32f, 0.25f));
            p.cubicTo (P (0.42f, 0.25f), P (0.42f, 0.7f), P (0.5f, 0.7f));
            p.cubicTo (P (0.58f, 0.7f), P (0.6f, 0.25f), P (0.68f, 0.25f));
            p.cubicTo (P (0.78f, 0.25f), P (0.8f, 0.7f), P (0.9f, 0.7f));
            g.strokePath (p, stroke);
            return;
        case Icon::Folder:
            p.startNewSubPath (P (0.1f, 0.25f));
            p.lineTo (P (0.4f, 0.25f));
            p.lineTo (P (0.48f, 0.35f));
            p.lineTo (P (0.9f, 0.35f));
            p.lineTo (P (0.9f, 0.8f));
            p.lineTo (P (0.1f, 0.8f));
            p.closeSubPath();
            g.strokePath (p, stroke);
            return;
        case Icon::Check:
            p.addRectangle (r.reduced (s * 0.12f));
            p.startNewSubPath (P (0.3f, 0.5f));
            p.lineTo (P (0.45f, 0.66f));
            p.lineTo (P (0.75f, 0.32f));
            g.strokePath (p, stroke);
            return;
        case Icon::Reset:
        {
            const auto c = P (0.5f, 0.52f);
            p.addCentredArc (c.x, c.y, s * 0.32f, s * 0.32f, 0.0f, juce::MathConstants<float>::pi * 0.35f, juce::MathConstants<float>::pi * 1.9f, true);
            g.strokePath (p, stroke);
            juce::Path head;
            const auto tip = P (0.5f, 0.2f);
            head.addTriangle (tip.x + s * 0.02f, tip.y - s * 0.12f, tip.x + s * 0.02f, tip.y + s * 0.12f, tip.x - s * 0.16f, tip.y);
            g.fillPath (head);
            return;
        }
        case Icon::Edit:
            p.startNewSubPath (P (0.2f, 0.8f));
            p.lineTo (P (0.24f, 0.62f));
            p.lineTo (P (0.66f, 0.2f));
            p.lineTo (P (0.8f, 0.34f));
            p.lineTo (P (0.38f, 0.76f));
            p.closeSubPath();
            p.startNewSubPath (P (0.58f, 0.28f));
            p.lineTo (P (0.72f, 0.42f));
            g.strokePath (p, stroke);
            return;
        case Icon::Trash:
            p.startNewSubPath (P (0.2f, 0.27f));
            p.lineTo (P (0.8f, 0.27f));
            p.startNewSubPath (P (0.4f, 0.27f));
            p.lineTo (P (0.42f, 0.16f));
            p.lineTo (P (0.58f, 0.16f));
            p.lineTo (P (0.6f, 0.27f));
            p.startNewSubPath (P (0.27f, 0.27f));
            p.lineTo (P (0.32f, 0.85f));
            p.lineTo (P (0.68f, 0.85f));
            p.lineTo (P (0.73f, 0.27f));
            for (float u : { 0.42f, 0.58f })
            {
                p.startNewSubPath (P (u, 0.4f));
                p.lineTo (P (u, 0.72f));
            }
            g.strokePath (p, stroke);
            return;
        case Icon::Play:
            p.addTriangle (P (0.28f, 0.18f), P (0.28f, 0.82f), P (0.82f, 0.5f));
            g.fillPath (p);
            return;
        case Icon::Stop:
            g.fillRoundedRectangle (r.reduced (s * 0.2f), s * 0.04f);
            return;
        case Icon::ChevronLeft:
        case Icon::ChevronRight:
        {
            const bool left = icon == Icon::ChevronLeft;
            p.startNewSubPath (P (left ? 0.62f : 0.38f, 0.18f));
            p.lineTo (P (left ? 0.32f : 0.68f, 0.5f));
            p.lineTo (P (left ? 0.62f : 0.38f, 0.82f));
            g.strokePath (p, juce::PathStrokeType (w * 1.4f, juce::PathStrokeType::mitered, juce::PathStrokeType::rounded));
            return;
        }
        case Icon::Disk:
            p.startNewSubPath (P (0.18f, 0.16f));
            p.lineTo (P (0.7f, 0.16f));
            p.lineTo (P (0.84f, 0.3f));
            p.lineTo (P (0.84f, 0.84f));
            p.lineTo (P (0.18f, 0.84f));
            p.closeSubPath();
            p.addRectangle (P (0.32f, 0.16f).x, P (0.32f, 0.16f).y, s * 0.32f, s * 0.2f);
            p.addRectangle (P (0.3f, 0.56f).x, P (0.3f, 0.56f).y, s * 0.42f, s * 0.28f);
            g.strokePath (p, stroke);
            return;

        case Icon::Power:
        {
            // WING 3.1 (+48V): a bold ring nine tenths of the icon across.
            const auto c = P (0.5f, 0.55f);
            p.addCentredArc (c.x, c.y, s * 0.4f, s * 0.4f, 0.0f, 0.7f, juce::MathConstants<float>::twoPi - 0.7f, true);
            p.startNewSubPath (P (0.5f, 0.06f));
            p.lineTo (P (0.5f, 0.5f));
            g.strokePath (p, juce::PathStrokeType (juce::jmax (1.5f, s * 0.13f), juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
            return;
        }
        case Icon::RemoveCircle:
        case Icon::MinusCircle:
        case Icon::ClearCircle:
        case Icon::Info:
        {
            p.addEllipse (r.reduced (s * 0.13f));
            g.strokePath (p, stroke);
            p.clear();
            if (icon == Icon::RemoveCircle)
            {
                g.fillEllipse (juce::Rectangle<float> (s * 0.26f, s * 0.26f).withCentre (P (0.5f, 0.5f)));
                return;
            }
            if (icon == Icon::MinusCircle)
            {
                p.startNewSubPath (P (0.32f, 0.5f));
                p.lineTo (P (0.68f, 0.5f));
            }
            else if (icon == Icon::ClearCircle)
            {
                p.startNewSubPath (P (0.36f, 0.36f));
                p.lineTo (P (0.64f, 0.64f));
                p.startNewSubPath (P (0.64f, 0.36f));
                p.lineTo (P (0.36f, 0.64f));
            }
            else
            {
                g.fillEllipse (juce::Rectangle<float> (s * 0.1f, s * 0.1f).withCentre (P (0.5f, 0.33f)));
                p.startNewSubPath (P (0.5f, 0.46f));
                p.lineTo (P (0.5f, 0.7f));
            }
            g.strokePath (p, stroke);
            return;
        }
        case Icon::Skip:
            p.addTriangle (P (0.2f, 0.2f), P (0.2f, 0.8f), P (0.62f, 0.5f));
            p.addRectangle (P (0.68f, 0.2f).x, P (0.68f, 0.2f).y, s * 0.12f, s * 0.6f);
            g.fillPath (p);
            return;
        case Icon::Link:
        {
            juce::Path link;
            link.addRoundedRectangle (-s * 0.22f, -s * 0.11f, s * 0.44f, s * 0.22f, s * 0.11f);
            const auto rotation = juce::AffineTransform::rotation (-juce::MathConstants<float>::pi * 0.25f);
            p.addPath (link, rotation.translated (P (0.37f, 0.63f)));
            p.addPath (link, rotation.translated (P (0.63f, 0.37f)));
            g.strokePath (p, stroke);
            return;
        }
        case Icon::Tag:
            p.startNewSubPath (P (0.12f, 0.3f));
            p.lineTo (P (0.62f, 0.3f));
            p.lineTo (P (0.88f, 0.5f));
            p.lineTo (P (0.62f, 0.7f));
            p.lineTo (P (0.12f, 0.7f));
            p.closeSubPath();
            p.addEllipse (juce::Rectangle<float> (s * 0.1f, s * 0.1f).withCentre (P (0.64f, 0.5f)));
            g.strokePath (p, stroke);
            return;
        case Icon::Storage:
            p.addRoundedRectangle (P (0.12f, 0.24f).x, P (0.12f, 0.24f).y, s * 0.76f, s * 0.52f, s * 0.05f);
            for (float u : { 0.26f, 0.36f, 0.46f, 0.56f })
            {
                p.startNewSubPath (P (u, 0.36f));
                p.lineTo (P (u, 0.64f));
            }
            g.strokePath (p, stroke);
            g.fillEllipse (juce::Rectangle<float> (s * 0.1f, s * 0.1f).withCentre (P (0.74f, 0.5f)));
            return;
        case Icon::ShowFile:
            p.addRectangle (r.reduced (s * 0.14f, s * 0.18f));
            g.strokePath (p, stroke);
            for (float u : { 0.24f, 0.4f, 0.56f, 0.72f })
                for (float v : { 0.24f, 0.72f })
                    g.fillRect (P (u, v).x, P (u, v).y, s * 0.07f, s * 0.05f);
            p.clear();
            p.addTriangle (P (0.42f, 0.38f), P (0.42f, 0.62f), P (0.62f, 0.5f));
            g.fillPath (p);
            return;
        case Icon::GoPrev:
        case Icon::GoNext:
        {
            p.addTriangle (P (0.08f, 0.28f), P (0.08f, 0.72f), P (0.42f, 0.5f));
            g.fillPath (p);
            p.clear();
            const bool back = icon == Icon::GoPrev;
            p.startNewSubPath (P (back ? 0.82f : 0.58f, 0.24f));
            p.lineTo (P (back ? 0.58f : 0.82f, 0.5f));
            p.lineTo (P (back ? 0.82f : 0.58f, 0.76f));
            g.strokePath (p, juce::PathStrokeType (w * 1.3f, juce::PathStrokeType::mitered, juce::PathStrokeType::rounded));
            return;
        }
        case Icon::SortDate:
            p.addRectangle (P (0.12f, 0.2f).x, P (0.12f, 0.2f).y, s * 0.56f, s * 0.54f);
            p.startNewSubPath (P (0.12f, 0.36f));
            p.lineTo (P (0.68f, 0.36f));
            for (float u : { 0.26f, 0.54f })
            {
                p.startNewSubPath (P (u, 0.12f));
                p.lineTo (P (u, 0.26f));
            }
            g.strokePath (p, stroke);
            p.clear();
            p.addEllipse (juce::Rectangle<float> (s * 0.4f, s * 0.4f).withCentre (P (0.7f, 0.7f)));
            g.strokePath (p, stroke);
            p.clear();
            p.startNewSubPath (P (0.7f, 0.58f));
            p.lineTo (P (0.7f, 0.7f));
            p.lineTo (P (0.79f, 0.76f));
            g.strokePath (p, stroke);
            return;
        case Icon::Overview:
            for (int i = 0; i < 4; ++i)
                for (int j = 0; j < 4; ++j)
                    g.fillRect (P (0.12f + (float) i * 0.2f, 0.12f + (float) j * 0.2f).x, P (0.12f + (float) i * 0.2f, 0.12f + (float) j * 0.2f).y,
                                s * 0.14f, s * 0.14f);
            return;
        case Icon::Faders:
        {
            const float caps[] = { 0.62f, 0.3f, 0.5f, 0.38f };
            for (int i = 0; i < 4; ++i)
            {
                const float u = 0.17f + (float) i * 0.22f;
                p.startNewSubPath (P (u, 0.1f));
                p.lineTo (P (u, 0.9f));
                g.fillRect (juce::Rectangle<float> (s * 0.16f, s * 0.1f).withCentre (P (u, caps[i])));
            }
            g.strokePath (p, juce::PathStrokeType (juce::jmax (1.0f, w * 0.6f)));
            return;
        }
        case Icon::Sends:
        {
            const float caps[] = { 0.62f, 0.36f, 0.72f };
            for (int i = 0; i < 3; ++i)
            {
                const float v = 0.25f + (float) i * 0.25f;
                p.startNewSubPath (P (0.1f, v));
                p.lineTo (P (0.9f, v));
                g.fillRect (juce::Rectangle<float> (s * 0.1f, s * 0.16f).withCentre (P (caps[i], v)));
            }
            g.strokePath (p, juce::PathStrokeType (juce::jmax (1.0f, w * 0.6f)));
            return;
        }
        case Icon::Layers:
        case Icon::LayersGear:
        {
            const float scale = icon == Icon::Layers ? 1.0f : 0.72f;
            auto L = [&] (float u, float v) { return P (u * scale, v * scale); };
            p.startNewSubPath (L (0.5f, 0.12f));
            p.lineTo (L (0.9f, 0.32f));
            p.lineTo (L (0.5f, 0.52f));
            p.lineTo (L (0.1f, 0.32f));
            p.closeSubPath();
            for (float dv : { 0.18f, 0.36f })
            {
                p.startNewSubPath (L (0.1f, 0.32f + dv));
                p.lineTo (L (0.5f, 0.52f + dv));
                p.lineTo (L (0.9f, 0.32f + dv));
            }
            g.strokePath (p, stroke);
            if (icon == Icon::LayersGear)
            {
                juce::Path gear;
                const auto c = P (0.74f, 0.74f);
                for (int i = 0; i < 6; ++i)
                {
                    juce::Path tooth;
                    tooth.addRectangle (-s * 0.04f, -s * 0.22f, s * 0.08f, s * 0.08f);
                    gear.addPath (tooth, juce::AffineTransform::rotation ((float) i * juce::MathConstants<float>::twoPi / 6.0f).translated (c));
                }
                gear.addEllipse (c.x - s * 0.16f, c.y - s * 0.16f, s * 0.32f, s * 0.32f);
                gear.addEllipse (c.x - s * 0.06f, c.y - s * 0.06f, s * 0.12f, s * 0.12f);
                gear.setUsingNonZeroWinding (false);
                g.fillPath (gear);
            }
            return;
        }
        case Icon::MuteGroup:
            p.startNewSubPath (P (0.1f, 0.38f));
            p.lineTo (P (0.26f, 0.38f));
            p.lineTo (P (0.46f, 0.2f));
            p.lineTo (P (0.46f, 0.8f));
            p.lineTo (P (0.26f, 0.62f));
            p.lineTo (P (0.1f, 0.62f));
            p.closeSubPath();
            p.startNewSubPath (P (0.6f, 0.38f));
            p.lineTo (P (0.84f, 0.62f));
            p.startNewSubPath (P (0.84f, 0.38f));
            p.lineTo (P (0.6f, 0.62f));
            g.strokePath (p, stroke);
            return;
        case Icon::Monitors:
            p.addRoundedRectangle (P (0.1f, 0.18f).x, P (0.1f, 0.18f).y, s * 0.46f, s * 0.66f, s * 0.04f);
            p.addEllipse (juce::Rectangle<float> (s * 0.26f, s * 0.26f).withCentre (P (0.33f, 0.6f)));
            p.addEllipse (juce::Rectangle<float> (s * 0.1f, s * 0.1f).withCentre (P (0.33f, 0.32f)));
            p.addCentredArc (P (0.75f, 0.56f).x, P (0.75f, 0.56f).y, s * 0.14f, s * 0.16f, 0.0f, -1.6f, 1.6f, true);
            g.strokePath (p, stroke);
            g.fillRect (P (0.6f, 0.56f).x, P (0.6f, 0.56f).y, s * 0.06f, s * 0.14f);
            g.fillRect (P (0.84f, 0.56f).x, P (0.84f, 0.56f).y, s * 0.06f, s * 0.14f);
            return;
        case Icon::CcEdit:
        {
            const auto c = P (0.42f, 0.46f);
            p.addEllipse (c.x - s * 0.28f, c.y - s * 0.28f, s * 0.56f, s * 0.56f);
            p.startNewSubPath (c);
            p.lineTo (P (0.42f, 0.22f));
            p.startNewSubPath (P (0.62f, 0.9f));
            p.lineTo (P (0.9f, 0.62f));
            g.strokePath (p, stroke);
            return;
        }
        case Icon::Pause:
            g.fillRect (P (0.26f, 0.2f).x, P (0.26f, 0.2f).y, s * 0.16f, s * 0.6f);
            g.fillRect (P (0.58f, 0.2f).x, P (0.58f, 0.2f).y, s * 0.16f, s * 0.6f);
            return;
        case Icon::Record:
            g.fillEllipse (r.reduced (s * 0.24f));
            return;
        case Icon::SaveTray:
        case Icon::LoadTray:
        {
            p.startNewSubPath (P (0.14f, 0.58f));
            p.lineTo (P (0.14f, 0.84f));
            p.lineTo (P (0.86f, 0.84f));
            p.lineTo (P (0.86f, 0.58f));
            const bool down = icon == Icon::SaveTray;
            p.startNewSubPath (P (0.5f, 0.12f));
            p.lineTo (P (0.5f, 0.66f));
            p.startNewSubPath (P (0.32f, down ? 0.48f : 0.3f));
            p.lineTo (P (0.5f, down ? 0.66f : 0.12f));
            p.lineTo (P (0.68f, down ? 0.48f : 0.3f));
            g.strokePath (p, stroke);
            return;
        }
        case Icon::SourcesIn:
        case Icon::OutputsOut:
        {
            const bool in = icon == Icon::SourcesIn;
            p.addCentredArc (P (0.58f, 0.5f).x, P (0.58f, 0.5f).y, s * 0.3f, s * 0.3f, 0.0f, in ? -2.6f : 0.55f, in ? 2.6f : 5.73f, true);
            p.startNewSubPath (P (in ? 0.06f : 0.36f, 0.5f));
            p.lineTo (P (in ? 0.62f : 0.94f, 0.5f));
            const float tip = in ? 0.62f : 0.94f;
            p.startNewSubPath (P (tip - 0.16f, 0.36f));
            p.lineTo (P (tip, 0.5f));
            p.lineTo (P (tip - 0.16f, 0.64f));
            g.strokePath (p, stroke);
            return;
        }
        case Icon::ChevronUp:
        case Icon::ChevronDown:
        {
            const bool up = icon == Icon::ChevronUp;
            p.startNewSubPath (P (0.18f, up ? 0.64f : 0.36f));
            p.lineTo (P (0.5f, up ? 0.32f : 0.68f));
            p.lineTo (P (0.82f, up ? 0.64f : 0.36f));
            g.strokePath (p, juce::PathStrokeType (w * 1.4f, juce::PathStrokeType::mitered, juce::PathStrokeType::rounded));
            return;
        }
        case Icon::GainArrows:
        case Icon::WidthArrows:
        {
            // A double-headed arrow, vertical or horizontal.
            const bool vertical = icon == Icon::GainArrows;
            auto Q = [&] (float along, float across) { return vertical ? P (0.5f + across, along) : P (along, 0.5f + across); };
            p.startNewSubPath (Q (0.16f, 0.0f));
            p.lineTo (Q (0.84f, 0.0f));
            for (float end : { 0.16f, 0.84f })
            {
                const float back = end < 0.5f ? 0.16f : -0.16f;
                p.startNewSubPath (Q (end + back, -0.13f));
                p.lineTo (Q (end, 0.0f));
                p.lineTo (Q (end + back, 0.13f));
            }
            g.strokePath (p, stroke);
            return;
        }
        case Icon::FreqMarker:
            p.startNewSubPath (P (0.5f, 0.16f));
            p.lineTo (P (0.5f, 0.84f));
            p.startNewSubPath (P (0.3f, 0.38f));
            p.lineTo (P (0.18f, 0.5f));
            p.lineTo (P (0.3f, 0.62f));
            p.startNewSubPath (P (0.7f, 0.38f));
            p.lineTo (P (0.82f, 0.5f));
            p.lineTo (P (0.7f, 0.62f));
            g.strokePath (p, stroke);
            return;
        case Icon::Bell:
            p.startNewSubPath (P (0.08f, 0.78f));
            p.cubicTo (P (0.36f, 0.78f), P (0.38f, 0.2f), P (0.5f, 0.2f));
            p.cubicTo (P (0.62f, 0.2f), P (0.64f, 0.78f), P (0.92f, 0.78f));
            g.strokePath (p, stroke);
            return;
        case Icon::LowShelf:
        case Icon::HighShelf:
        {
            const bool low = icon == Icon::LowShelf;
            p.startNewSubPath (P (0.08f, low ? 0.3f : 0.72f));
            p.lineTo (P (0.3f, low ? 0.3f : 0.72f));
            p.cubicTo (P (0.5f, low ? 0.3f : 0.72f), P (0.5f, low ? 0.72f : 0.3f), P (0.7f, low ? 0.72f : 0.3f));
            p.lineTo (P (0.92f, low ? 0.72f : 0.3f));
            g.strokePath (p, stroke);
            return;
        }
        case Icon::Usb:
            p.startNewSubPath (P (0.5f, 0.12f));
            p.lineTo (P (0.5f, 0.82f));
            p.startNewSubPath (P (0.5f, 0.62f));
            p.lineTo (P (0.28f, 0.48f));
            p.lineTo (P (0.28f, 0.36f));
            p.startNewSubPath (P (0.5f, 0.52f));
            p.lineTo (P (0.72f, 0.4f));
            p.lineTo (P (0.72f, 0.3f));
            g.strokePath (p, stroke);
            g.fillEllipse (P (0.42f, 0.78f).x, P (0.42f, 0.78f).y, s * 0.16f, s * 0.16f);
            g.fillRect (P (0.66f, 0.2f).x, P (0.66f, 0.2f).y, s * 0.12f, s * 0.1f);
            return;
        case Icon::Circle:
            p.addEllipse (r.reduced (s * 0.18f));
            g.strokePath (p, stroke);
            return;
        case Icon::Copy:
            p.addRoundedRectangle (P (0.18f, 0.12f).x, P (0.18f, 0.12f).y, s * 0.42f, s * 0.54f, s * 0.04f);
            p.addRoundedRectangle (P (0.4f, 0.34f).x, P (0.4f, 0.34f).y, s * 0.42f, s * 0.54f, s * 0.04f);
            g.strokePath (p, stroke);
            return;
        case Icon::Maxer:
            // Three bold slanted bars [3.1].
            for (int i = 0; i < 3; ++i)
            {
                const float left = 0.2f + 0.24f * (float) i;
                p.startNewSubPath (P (left, 0.74f));
                p.lineTo (P (left + 0.14f, 0.26f));
            }
            g.strokePath (p, juce::PathStrokeType (s * 0.13f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
            return;
        case Icon::AllPass90:
        case Icon::AllPass180:
        {
            // The phase falling by 90 degrees (a gentle S) or 180 (a deeper S, flat at both ends) [3.1].
            const bool deep = icon == Icon::AllPass180;
            p.startNewSubPath (P (0.12f, deep ? 0.2f : 0.28f));
            if (deep)
            {
                p.lineTo (P (0.3f, 0.2f));
                p.cubicTo (P (0.5f, 0.2f), P (0.42f, 0.8f), P (0.62f, 0.8f));
                p.lineTo (P (0.88f, 0.8f));
            }
            else
                p.cubicTo (P (0.4f, 0.28f), P (0.55f, 0.72f), P (0.88f, 0.72f));
            g.strokePath (p, stroke);
            return;
        }
        case Icon::DeltaT:
            p.addTriangle (P (0.08f, 0.8f), P (0.32f, 0.22f), P (0.56f, 0.8f));
            g.strokePath (p, stroke);
            p.clear();
            p.startNewSubPath (P (0.62f, 0.4f));
            p.lineTo (P (0.92f, 0.4f));
            p.startNewSubPath (P (0.77f, 0.28f));
            p.lineTo (P (0.77f, 0.8f));
            g.strokePath (p, stroke);
            return;
        case Icon::Invert:
            p.addEllipse (r.reduced (s * 0.18f));
            p.startNewSubPath (P (0.78f, 0.12f));
            p.lineTo (P (0.22f, 0.88f));
            g.strokePath (p, stroke);
            return;
        case Icon::Cancel:
            p.addEllipse (r.reduced (s * 0.1f));
            p.startNewSubPath (P (0.34f, 0.34f));
            p.lineTo (P (0.66f, 0.66f));
            p.startNewSubPath (P (0.66f, 0.34f));
            p.lineTo (P (0.34f, 0.66f));
            g.strokePath (p, stroke);
            return;
        case Icon::UsbStick:
            p.addRoundedRectangle (P (0.32f, 0.36f).x, P (0.32f, 0.36f).y, s * 0.36f, s * 0.54f, s * 0.05f);
            p.addRectangle (P (0.38f, 0.1f).x, P (0.38f, 0.1f).y, s * 0.24f, s * 0.26f);
            g.strokePath (p, stroke);
            return;
        case Icon::ReturnToStart:
            g.fillRect (P (0.2f, 0.22f).x, P (0.2f, 0.22f).y, s * 0.1f, s * 0.56f);
            p.addTriangle (P (0.34f, 0.5f), P (0.8f, 0.22f), P (0.8f, 0.78f));
            g.fillPath (p);
            return;
        case Icon::Rewind:
            p.addTriangle (P (0.08f, 0.5f), P (0.48f, 0.24f), P (0.48f, 0.76f));
            p.addTriangle (P (0.48f, 0.5f), P (0.88f, 0.24f), P (0.88f, 0.76f));
            g.fillPath (p);
            return;
        case Icon::FastForward:
            p.addTriangle (P (0.12f, 0.24f), P (0.12f, 0.76f), P (0.52f, 0.5f));
            p.addTriangle (P (0.52f, 0.24f), P (0.52f, 0.76f), P (0.92f, 0.5f));
            g.fillPath (p);
            return;
        case Icon::Loop:
        {
            p.addRoundedRectangle (P (0.14f, 0.3f).x, P (0.14f, 0.3f).y, s * 0.72f, s * 0.4f, s * 0.2f);
            g.strokePath (p, stroke);
            juce::Path heads;
            heads.addTriangle (P (0.5f, 0.2f), P (0.64f, 0.3f), P (0.5f, 0.4f));
            heads.addTriangle (P (0.5f, 0.6f), P (0.36f, 0.7f), P (0.5f, 0.8f));
            g.fillPath (heads);
            return;
        }
        case Icon::MarkerAdd:
        case Icon::MarkerPrev:
        case Icon::MarkerNext:
        {
            // The WING-LIVE marker keys: an I-beam with a plus, or with a triangle.
            const float cx = icon == Icon::MarkerAdd ? 0.6f : (icon == Icon::MarkerPrev ? 0.24f : 0.76f);
            p.startNewSubPath (P (cx, 0.18f));
            p.lineTo (P (cx, 0.82f));
            p.startNewSubPath (P (cx - 0.12f, 0.18f));
            p.lineTo (P (cx + 0.12f, 0.18f));
            p.startNewSubPath (P (cx - 0.12f, 0.82f));
            p.lineTo (P (cx + 0.12f, 0.82f));
            if (icon == Icon::MarkerAdd)
            {
                p.startNewSubPath (P (0.12f, 0.38f));
                p.lineTo (P (0.36f, 0.38f));
                p.startNewSubPath (P (0.24f, 0.26f));
                p.lineTo (P (0.24f, 0.5f));
            }
            g.strokePath (p, stroke);
            if (icon != Icon::MarkerAdd)
            {
                juce::Path tri;
                if (icon == Icon::MarkerPrev)
                    tri.addTriangle (P (0.42f, 0.5f), P (0.8f, 0.26f), P (0.8f, 0.74f));
                else
                    tri.addTriangle (P (0.58f, 0.5f), P (0.2f, 0.26f), P (0.2f, 0.74f));
                g.fillPath (tri);
            }
            return;
        }
        case Icon::SdCard:
            p.startNewSubPath (P (0.3f, 0.1f));
            p.lineTo (P (0.72f, 0.1f));
            p.lineTo (P (0.72f, 0.9f));
            p.lineTo (P (0.24f, 0.9f));
            p.lineTo (P (0.24f, 0.2f));
            p.closeSubPath();
            g.strokePath (p, stroke);
            return;
        case Icon::Waveform:
        {
            const float heights[] = { 0.16f, 0.34f, 0.24f, 0.44f, 0.3f, 0.18f, 0.38f, 0.22f, 0.12f };
            for (int i = 0; i < 9; ++i)
            {
                const float u = 0.1f + (float) i * 0.1f;
                p.startNewSubPath (P (u, 0.5f - heights[i]));
                p.lineTo (P (u, 0.5f + heights[i]));
            }
            g.strokePath (p, stroke);
            return;
        }
        case Icon::Sine:
        {
            // One cycle of a sine: the oscillator (OSC sources).
            p.startNewSubPath (P (0.1f, 0.5f));
            for (int i = 1; i <= 24; ++i)
            {
                const float t = (float) i / 24.0f;
                p.lineTo (P (0.1f + 0.8f * t, 0.5f - 0.3f * std::sin (t * juce::MathConstants<float>::twoPi)));
            }
            g.strokePath (p, stroke);
            return;
        }
        case Icon::Premium:
        {
            // A cut gem: the crown's outline and its girdle line.
            p.startNewSubPath (P (0.08f, 0.36f));
            p.lineTo (P (0.28f, 0.14f));
            p.lineTo (P (0.72f, 0.14f));
            p.lineTo (P (0.92f, 0.36f));
            p.lineTo (P (0.5f, 0.88f));
            p.closeSubPath();
            p.startNewSubPath (P (0.08f, 0.36f));
            p.lineTo (P (0.92f, 0.36f));
            g.strokePath (p, juce::PathStrokeType (juce::jmax (1.0f, s * 0.06f), juce::PathStrokeType::mitered));
            return;
        }
        case Icon::Sun:
        {
            const auto c = P (0.5f, 0.5f);
            const float rad = s * 0.2f;
            p.addEllipse (c.x - rad, c.y - rad, rad * 2.0f, rad * 2.0f);
            for (int i = 0; i < 8; ++i)
            {
                const float a = (float) i * juce::MathConstants<float>::pi * 0.25f;
                const float r0 = rad * 1.5f, r1 = s * 0.47f;
                p.startNewSubPath (c.x + std::sin (a) * r0, c.y - std::cos (a) * r0);
                p.lineTo (c.x + std::sin (a) * r1, c.y - std::cos (a) * r1);
            }
            g.strokePath (p, stroke);
            return;
        }
        case Icon::ListNone:
        case Icon::ListAll:
        {
            // Three rows: a box (empty or filled) and a line.
            const float d = s * 0.2f;
            for (int row = 0; row < 3; ++row)
            {
                const float v = 0.2f + 0.3f * (float) row;
                const auto box = juce::Rectangle<float> (d, d).withCentre (P (0.16f, v));
                if (icon == Icon::ListAll)
                    g.fillRoundedRectangle (box, s * 0.03f);
                else
                    g.drawRoundedRectangle (box, s * 0.03f, juce::jmax (1.0f, s * 0.05f));
                g.fillRect (juce::Rectangle<float> (P (0.36f, v - 0.03f), P (0.95f, v + 0.03f)));
            }
            return;
        }
        case Icon::PaintBucket:
        {
            // A tipped bucket (a rotated square with its handle) pouring a drop.
            juce::Path bucket;
            bucket.addRectangle (-s * 0.2f, -s * 0.2f, s * 0.4f, s * 0.4f);
            p.addPath (bucket, juce::AffineTransform::rotation (juce::MathConstants<float>::pi * 0.25f).translated (P (0.42f, 0.52f)));
            p.startNewSubPath (P (0.24f, 0.34f));
            p.cubicTo (P (0.2f, 0.06f), P (0.5f, 0.06f), P (0.48f, 0.3f));
            g.strokePath (p, stroke);
            juce::Path drop;
            drop.startNewSubPath (P (0.82f, 0.5f));
            drop.cubicTo (P (0.94f, 0.7f), P (0.9f, 0.8f), P (0.82f, 0.8f));
            drop.cubicTo (P (0.74f, 0.8f), P (0.7f, 0.7f), P (0.82f, 0.5f));
            g.fillPath (drop);
            return;
        }
        case Icon::Warning:
        {
            p.startNewSubPath (P (0.5f, 0.1f));
            p.lineTo (P (0.94f, 0.88f));
            p.lineTo (P (0.06f, 0.88f));
            p.closeSubPath();
            g.strokePath (p, juce::PathStrokeType (w, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
            g.fillRect (juce::Rectangle<float> (P (0.46f, 0.36f), P (0.54f, 0.64f)));
            g.fillEllipse (juce::Rectangle<float> (s * 0.09f, s * 0.09f).withCentre (P (0.5f, 0.75f)));
            return;
        }
        case Icon::KeyboardEdit:
        {
            // A keyboard (two rows of keys and a space bar) with a pencil over its corner.
            const auto body = juce::Rectangle<float> (P (0.06f, 0.42f), P (0.78f, 0.9f));
            g.drawRoundedRectangle (body, s * 0.05f, juce::jmax (1.0f, s * 0.06f));
            const float k = s * 0.08f;
            for (int row = 0; row < 2; ++row)
                for (int col = 0; col < 4; ++col)
                    g.fillRect (juce::Rectangle<float> (k, k).withCentre (P (0.17f + 0.165f * (float) col, 0.54f + 0.14f * (float) row)));
            g.fillRect (juce::Rectangle<float> (P (0.26f, 0.79f), P (0.58f, 0.83f)));
            p.startNewSubPath (P (0.56f, 0.5f));
            p.lineTo (P (0.58f, 0.38f));
            p.lineTo (P (0.86f, 0.1f));
            p.lineTo (P (0.96f, 0.2f));
            p.lineTo (P (0.68f, 0.48f));
            p.closeSubPath();
            g.strokePath (p, juce::PathStrokeType (juce::jmax (1.0f, s * 0.06f), juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
            return;
        }
        case Icon::Spill:
        {
            // A fader (slot and cap) with arrows spreading out to both sides.
            g.fillRect (juce::Rectangle<float> (P (0.47f, 0.1f), P (0.53f, 0.9f)));
            g.fillRoundedRectangle (juce::Rectangle<float> (P (0.36f, 0.42f), P (0.64f, 0.58f)), s * 0.03f);
            for (int side = -1; side <= 1; side += 2)
            {
                const float u0 = 0.5f + 0.2f * (float) side, u1 = 0.5f + 0.44f * (float) side;
                p.startNewSubPath (P (u0, 0.5f));
                p.lineTo (P (u1, 0.5f));
                p.startNewSubPath (P (u1 - 0.1f * (float) side, 0.38f));
                p.lineTo (P (u1, 0.5f));
                p.lineTo (P (u1 - 0.1f * (float) side, 0.62f));
            }
            g.strokePath (p, stroke);
            return;
        }
        case Icon::Maximize:
        case Icon::Restore:
        {
            // Four corner brackets: at the edges pointing out, or near the
            // centre pointing in.
            const bool out = icon == Icon::Maximize;
            for (int cx = 0; cx < 2; ++cx)
                for (int cy = 0; cy < 2; ++cy)
                {
                    const float sx = cx == 0 ? -1.0f : 1.0f, sy = cy == 0 ? -1.0f : 1.0f;
                    const float corner = out ? 0.36f : 0.1f, arm = 0.2f;
                    const float u = 0.5f + sx * corner, v = 0.5f + sy * corner;
                    const float d = out ? -arm : arm; // arms run back towards the centre (out) or away from it (in)
                    p.startNewSubPath (P (u + sx * d, v));
                    p.lineTo (P (u, v));
                    p.lineTo (P (u, v + sy * d));
                }
            g.strokePath (p, stroke);
            return;
        }
        case Icon::Plug:
            // A cable into a round plug body with two pins, on one line.
            p.startNewSubPath (P (0.06f, 0.5f));
            p.lineTo (P (0.3f, 0.5f));
            p.addEllipse (juce::Rectangle<float> (s * 0.22f, s * 0.22f).withCentre (P (0.41f, 0.5f)));
            p.startNewSubPath (P (0.52f, 0.4f));
            p.lineTo (P (0.68f, 0.4f));
            p.startNewSubPath (P (0.52f, 0.6f));
            p.lineTo (P (0.68f, 0.6f));
            p.startNewSubPath (P (0.68f, 0.36f));
            p.lineTo (P (0.68f, 0.64f));
            p.startNewSubPath (P (0.68f, 0.5f));
            p.lineTo (P (0.94f, 0.5f));
            g.strokePath (p, stroke);
            return;
        case Icon::FilterBand:
            p.startNewSubPath (P (0.08f, 0.68f));
            p.quadraticTo (P (0.24f, 0.32f), P (0.36f, 0.32f));
            p.lineTo (P (0.64f, 0.32f));
            p.quadraticTo (P (0.76f, 0.32f), P (0.92f, 0.68f));
            g.strokePath (p, stroke);
            return;
        case Icon::FaderCap:
            p.startNewSubPath (P (0.5f, 0.06f));
            p.lineTo (P (0.5f, 0.94f));
            g.strokePath (p, stroke);
            g.fillRoundedRectangle (juce::Rectangle<float> (s * 0.44f, s * 0.2f).withCentre (P (0.5f, 0.42f)), s * 0.04f);
            return;
        case Icon::PanKnob:
        {
            const auto c = P (0.5f, 0.5f);
            p.addEllipse (juce::Rectangle<float> (s * 0.8f, s * 0.8f).withCentre (c));
            g.strokePath (p, stroke);
            juce::Path knob;
            knob.addEllipse (juce::Rectangle<float> (s * 0.5f, s * 0.5f).withCentre (c));
            knob.addEllipse (juce::Rectangle<float> (s * 0.14f, s * 0.14f).withCentre (P (0.64f, 0.36f)));
            knob.setUsingNonZeroWinding (false);
            g.fillPath (knob);
            return;
        }
    }
}


//==============================================================================
// The WING's channel icons (/ch/n/icon), drawn as white line art on a 24-unit
// grid in the style of the console's ICON/COLOR gallery.

namespace
{
    struct WingIconName
    {
        int number;
        const char* name;
    };

    // The console's icon numbers and names (as the Behringer Companion module lists them).
    constexpr WingIconName wingIconNames[] = {
        { 0, "Empty" }, { 1, "XLR Connector" }, { 2, "Jack Connector" }, { 3, "Mini Jack Connector" }, { 4, "RCA Connector" },
        { 5, "Fader" }, { 6, "FX" }, { 7, "Matrix" }, { 8, "Bass Clef" }, { 9, "Treble Clef" }, { 10, "Routing" }, { 11, "Bus" },
        { 12, "Sum" }, { 13, "Smile" }, { 14, "Wing Logo" },
        { 100, "Microphone" }, { 101, "Wireless Microphone" }, { 102, "Handheld Microphone" }, { 103, "Condenser Microphone" },
        { 104, "Boundary Microphone" }, { 105, "Lapel Microphone" }, { 106, "Boundary Microphone (Alternative)" },
        { 107, "Headset Microphone" }, { 108, "Overhead Microphone" }, { 109, "Speaker Icon" }, { 110, "Studio Microphone" },
        { 111, "Podcast Microphone" }, { 112, "Choir" }, { 113, "Female Vocal" }, { 114, "Male Vocal" },
        { 200, "Kick In" }, { 201, "Kick Out" }, { 202, "Snare Top" }, { 203, "Snare Bottom" }, { 204, "Snare Drum" }, { 205, "Hi-Hat" },
        { 206, "High Tom" }, { 207, "Mid Tom" }, { 208, "Low Tom" }, { 209, "Floor Tom" }, { 210, "Drum" }, { 211, "Crash Cymbal" },
        { 212, "Ride Cymbal" }, { 213, "Cowbell" }, { 214, "Tambourine" }, { 215, "Bongos" }, { 216, "Conga" }, { 217, "Timpani" },
        { 218, "Cajon" }, { 219, "Shaker" }, { 220, "Marimba" }, { 221, "Hang Drum" }, { 222, "Triangle" }, { 223, "Drum Machine" },
        { 224, "Claps" },
        { 300, "Electric Guitar 1" }, { 301, "Electric Guitar 2" }, { 302, "Violin" }, { 303, "Oud" }, { 304, "Acoustic Guitar" },
        { 305, "Electric Guitar 3" }, { 306, "Electric Guitar 4" }, { 307, "Electric Guitar 5" }, { 308, "Double Bass" }, { 309, "Viola" },
        { 310, "Cello" }, { 311, "Baritone" }, { 312, "Saxophone" }, { 313, "Trombone" }, { 314, "Trumpet" }, { 315, "Harp" },
        { 316, "Organ" }, { 317, "Harmonica" }, { 318, "Transverse Flute" }, { 319, "Clarinet" },
        { 400, "Grand Piano" }, { 401, "Upright Piano" }, { 402, "Synthesizer 1" }, { 403, "Synthesizer 2" }, { 404, "Synthesizer 3" },
        { 405, "Synthesizer 4" }, { 406, "Keytar" }, { 407, "Piano Stand" }, { 408, "Piano Cross Stand" }, { 409, "Hammond Organ" },
        { 500, "Full Amplifier" }, { 501, "2x2 Cabinet" }, { 502, "Cabinet" }, { 503, "Speaker" }, { 504, "Speaker Pair" },
        { 505, "Speaker with Microphone Set" }, { 506, "Subwoofer" }, { 507, "Studio Monitor" }, { 508, "Studio Monitor 2" },
        { 509, "Column Speaker" }, { 510, "Wall Speaker" }, { 511, "Hanging Speaker" }, { 512, "Speaker Stand" },
        { 513, "Speaker Stand Pair" }, { 514, "Delay Speakers" }, { 515, "Left Monitor" }, { 516, "Right Monitor" }, { 517, "Monitor" },
        { 518, "Large Monitor" }, { 519, "Large Monitor Pair" }, { 520, "Left Wedge Monitor" }, { 521, "Right Wedge Monitor" },
        { 522, "Center Wedge Monitor" }, { 523, "Line Array" }, { 524, "Ceiling Speaker" },
        { 600, "Hand" }, { 601, "Ear" }, { 602, "Headphones" }, { 603, "Speaker A" }, { 604, "Speaker B" }, { 605, "Laptop" },
        { 606, "MP3 Player" }, { 607, "Smartphone" }, { 608, "USB Drive" }, { 609, "SD Card" }, { 610, "CD" }, { 611, "Turntable" },
        { 612, "Reel-to-Reel Player" }, { 613, "Cassette Player" }, { 614, "Patchbay" },
    };

    /** Line art on a 24-unit grid fitted into a square. */
    class Art
    {
    public:
        Art (juce::Graphics& graphics, juce::Rectangle<float> area, juce::Colour c)
            : g (graphics), u (juce::jmin (area.getWidth(), area.getHeight()) / 24.0f),
              origin (area.getCentre() - juce::Point<float> (12.0f * u, 12.0f * u)),
              stroke (juce::jmax (1.1f, u * 1.5f), juce::PathStrokeType::curved, juce::PathStrokeType::rounded)
        {
            g.setColour (c);
        }

        ~Art() { draw(); }

        juce::Point<float> P (float x, float y) const { return origin + juce::Point<float> (x * u, y * u); }

        void line (float x0, float y0, float x1, float y1)
        {
            p.startNewSubPath (P (x0, y0));
            p.lineTo (P (x1, y1));
        }

        void poly (std::initializer_list<float> xy, bool close = false)
        {
            const auto* v = xy.begin();
            p.startNewSubPath (P (v[0], v[1]));
            for (size_t i = 2; i + 1 < xy.size(); i += 2)
                p.lineTo (P (v[i], v[i + 1]));
            if (close)
                p.closeSubPath();
        }

        void curve (float x0, float y0, float x1, float y1, float x2, float y2, float x3, float y3)
        {
            p.startNewSubPath (P (x0, y0));
            p.cubicTo (P (x1, y1), P (x2, y2), P (x3, y3));
        }

        void circle (float cx, float cy, float r) { p.addEllipse (P (cx - r, cy - r).x, P (cx - r, cy - r).y, 2.0f * r * u, 2.0f * r * u); }
        void ellipse (float cx, float cy, float rx, float ry) { p.addEllipse (P (cx - rx, cy - ry).x, P (cx - rx, cy - ry).y, 2.0f * rx * u, 2.0f * ry * u); }
        void rect (float x, float y, float w, float h, float corner = 0.0f) { p.addRoundedRectangle (P (x, y).x, P (x, y).y, w * u, h * u, corner * u); }

        /** An arc, angles in degrees clockwise from 12 o'clock. */
        void arc (float cx, float cy, float rx, float ry, float from, float to)
        {
            p.addCentredArc (P (cx, cy).x, P (cx, cy).y, rx * u, ry * u, 0.0f, juce::degreesToRadians (from), juce::degreesToRadians (to), true);
        }

        /** A filled polygon. */
        void fill (std::initializer_list<float> xy)
        {
            draw();
            juce::Path shape;
            const auto* v = xy.begin();
            shape.startNewSubPath (P (v[0], v[1]));
            for (size_t i = 2; i + 1 < xy.size(); i += 2)
                shape.lineTo (P (v[i], v[i + 1]));
            shape.closeSubPath();
            g.fillPath (shape);
        }

        void dot (float cx, float cy, float r) { g.fillEllipse (P (cx - r, cy - r).x, P (cx - r, cy - r).y, 2.0f * r * u, 2.0f * r * u); }
        void block (float x, float y, float w, float h) { g.fillRect (P (x, y).x, P (x, y).y, w * u, h * u); }

        void text (const juce::String& t, float x, float y, float w, float h)
        {
            draw();
            g.setFont (wingFontPx (h * u * 0.78f, WingWeight::SemiBold));
            g.drawText (t, juce::Rectangle<float> (P (x, y).x, P (x, y).y, w * u, h * u), juce::Justification::centred, false);
        }

        void draw()
        {
            if (! p.isEmpty())
                g.strokePath (p, stroke);
            p.clear();
        }

        // Shapes several icons share.
        void stand (float top) // a mic stand: stem and base
        {
            line (12.0f, top, 12.0f, 21.0f);
            line (8.0f, 21.0f, 16.0f, 21.0f);
        }

        void drumSide (float cx, float top, float rx, float height) // a drum seen from the side
        {
            ellipse (cx, top, rx, rx * 0.3f);
            line (cx - rx, top, cx - rx, top + height);
            line (cx + rx, top, cx + rx, top + height);
            arc (cx, top + height, rx, rx * 0.3f, 90.0f, 270.0f);
        }

        void cymbal (float cx, float cy, float rx, float tilt) // a cymbal and its stand
        {
            juce::Path c;
            c.addEllipse (-rx * u, -rx * 0.18f * u, 2.0f * rx * u, 0.36f * rx * u);
            p.addPath (c, juce::AffineTransform::rotation (juce::degreesToRadians (tilt)).translated (P (cx, cy)));
            line (cx, cy, cx, 20.5f);
            line (cx, 20.5f, cx - 3.5f, 23.0f);
            line (cx, 20.5f, cx + 3.5f, 23.0f);
        }

        void speakerBox (float x, float y, float w, float h) // a box with a woofer and a tweeter
        {
            rect (x, y, w, h, 0.8f);
            const float r = w * 0.3f;
            circle (x + w * 0.5f, y + h - r - w * 0.18f, r);
            circle (x + w * 0.5f, y + w * 0.32f, w * 0.13f);
        }

        void wedge()
        {
            poly ({ 3.0f, 20.0f, 21.0f, 20.0f, 21.0f, 15.0f, 9.0f, 7.0f, 3.0f, 9.0f }, true);
            line (8.5f, 10.5f, 18.0f, 16.5f);
        }

        void synth()
        {
            rect (2.0f, 6.0f, 20.0f, 12.0f, 1.0f);
            line (2.0f, 11.0f, 22.0f, 11.0f);
            for (int i = 1; i < 8; ++i)
                line (2.0f + 2.5f * (float) i, 11.0f, 2.0f + 2.5f * (float) i, 18.0f);
            for (float x : { 4.5f, 7.0f, 12.0f, 14.5f, 17.0f })
                block (x - 0.6f, 11.0f, 1.2f, 4.0f);
            for (float x : { 5.0f, 8.0f, 11.0f })
                circle (x, 8.5f, 1.0f);
        }

        void electricGuitar()
        {
            p.startNewSubPath (P (11.0f, 12.5f));
            p.cubicTo (P (13.5f, 15.0f), P (12.0f, 17.5f), P (10.0f, 18.0f));
            p.cubicTo (P (11.5f, 21.5f), P (8.5f, 23.5f), P (5.0f, 22.0f));
            p.cubicTo (P (1.5f, 20.5f), P (1.5f, 16.0f), P (4.5f, 14.5f));
            p.cubicTo (P (4.5f, 11.5f), P (7.0f, 10.0f), P (9.0f, 11.0f));
            p.closeSubPath();
            line (9.5f, 14.5f, 20.0f, 4.0f);
            poly ({ 19.0f, 3.5f, 21.0f, 1.5f, 22.5f, 3.0f, 20.5f, 5.0f }, true);
            line (5.5f, 16.0f, 8.0f, 18.5f);
        }

        void bowed (float cx, float top, float scale, bool endpin) // violin family: two bouts, neck, f-holes
        {
            const auto Y = [&] (float v) { return top + v * scale; };
            p.startNewSubPath (P (cx, Y (0.0f)));
            p.lineTo (P (cx, Y (4.0f)));
            ellipse (cx, Y (7.5f), 3.6f * scale, 3.4f * scale);
            ellipse (cx, Y (14.0f), 4.6f * scale, 4.4f * scale);
            line (cx - 1.6f * scale, Y (10.5f), cx - 1.6f * scale, Y (13.0f));
            line (cx + 1.6f * scale, Y (10.5f), cx + 1.6f * scale, Y (13.0f));
            if (endpin)
                line (cx, Y (18.4f), cx, Y (20.0f));
        }

        void person (float cx, float headY, float headR)
        {
            circle (cx, headY, headR);
            arc (cx, headY + headR + 7.5f, headR * 2.0f, 6.0f, -70.0f, 70.0f);
        }

    private:
        juce::Graphics& g;
        float u;
        juce::Point<float> origin;
        juce::PathStrokeType stroke;
        juce::Path p;
    };
} // namespace

juce::String wingIconName (int number)
{
    for (auto& i : wingIconNames)
        if (i.number == number)
            return i.name;
    return {};
}

std::vector<int> wingIconNumbers (int category)
{
    std::vector<int> numbers;
    for (auto& i : wingIconNames)
        if (category < 0 || i.number / 100 == category)
            numbers.push_back (i.number);
    return numbers;
}

bool drawWingIcon (juce::Graphics& g, int number, juce::Rectangle<float> area, juce::Colour colour)
{
    // Icons this set draws with a shared design: their category's.
    switch (number)
    {
        case 1:   drawIcon (g, Icon::Xlr, area, colour); return true;
        case 109: drawIcon (g, Icon::Speaker, area, colour); return true;
        case 602: drawIcon (g, Icon::Headphones, area, colour); return true;
        case 608: drawIcon (g, Icon::UsbStick, area, colour); return true;
        case 609: drawIcon (g, Icon::SdCard, area, colour); return true;
        default:  break;
    }

    if (number <= 0 || number > 699)
        return false;

    Art a (g, area, colour);
    switch (number)
    {
        // General [I13]: line art as the WING's gallery draws it.
        case 2: // TRS jack: the ringed tip, the shaft, the open sleeve
            a.poly ({ 11.0f, 5.0f, 11.0f, 2.6f, 12.0f, 1.2f, 13.0f, 2.6f, 13.0f, 5.0f }, true);
            a.line (11.0f, 3.6f, 13.0f, 3.6f);
            a.rect (10.6f, 5.0f, 2.8f, 7.5f);
            a.line (10.6f, 8.5f, 13.4f, 8.5f);
            a.poly ({ 8.5f, 21.0f, 8.5f, 12.5f, 15.5f, 12.5f, 15.5f, 21.0f });
            return true;
        case 3: // TS jack, smaller
            a.poly ({ 11.2f, 8.5f, 11.2f, 6.6f, 12.0f, 5.4f, 12.8f, 6.6f, 12.8f, 8.5f }, true);
            a.rect (10.9f, 8.5f, 2.2f, 6.0f);
            a.line (10.9f, 11.0f, 13.1f, 11.0f);
            a.poly ({ 9.6f, 21.0f, 9.6f, 14.5f, 14.4f, 14.5f, 14.4f, 21.0f });
            return true;
        case 4: // a small connector
            a.rect (11.0f, 13.0f, 2.0f, 3.5f, 1.0f);
            a.poly ({ 9.6f, 21.0f, 9.6f, 16.5f, 14.4f, 16.5f, 14.4f, 21.0f });
            return true;
        case 5: // three faders
            for (auto [x, y] : { std::pair { 5.5f, 15.0f }, { 12.0f, 10.5f }, { 18.5f, 5.0f } })
            {
                a.line (x, 3.0f, x, y);
                a.rect (x - 1.8f, y, 3.6f, 4.0f, 0.4f);
                a.line (x, y + 4.0f, x, 21.0f);
            }
            return true;
        case 6: // FX: an F with a hooked top and an X
            a.curve (2.5f, 9.5f, 3.0f, 6.8f, 4.5f, 6.0f, 11.0f, 6.0f);
            a.line (7.0f, 6.0f, 7.0f, 19.0f);
            a.line (7.0f, 12.0f, 10.5f, 12.0f);
            a.line (11.5f, 6.0f, 20.5f, 19.0f);
            a.line (20.0f, 6.0f, 12.0f, 19.0f);
            return true;
        case 7: // routing: one box feeding a row of three
            a.rect (10.0f, 4.0f, 4.0f, 4.0f, 0.4f);
            a.line (12.0f, 8.0f, 12.0f, 12.0f);
            for (float x : { 3.0f, 10.0f, 17.0f })
                a.rect (x, 12.0f, 4.0f, 4.0f, 0.4f);
            a.line (7.0f, 14.0f, 10.0f, 14.0f);
            a.line (14.0f, 14.0f, 17.0f, 14.0f);
            return true;
        case 8:
            a.curve (7.0f, 10.0f, 6.5f, 4.5f, 16.5f, 3.5f, 17.0f, 9.0f);
            a.curve (17.0f, 9.0f, 17.5f, 15.0f, 11.0f, 19.5f, 5.5f, 21.0f);
            a.dot (7.6f, 10.0f, 1.9f);
            a.dot (20.5f, 7.5f, 1.2f);
            a.dot (20.5f, 12.5f, 1.2f);
            return true;
        case 9:
            a.curve (13.0f, 21.5f, 15.5f, 21.5f, 15.5f, 18.0f, 13.0f, 18.0f);
            a.line (13.0f, 18.0f, 12.0f, 2.5f);
            a.curve (12.0f, 2.5f, 17.0f, 5.0f, 16.0f, 9.0f, 10.5f, 12.5f);
            a.curve (10.5f, 12.5f, 5.5f, 16.0f, 8.0f, 21.0f, 13.0f, 20.0f);
            a.curve (13.0f, 20.0f, 18.0f, 19.0f, 17.0f, 12.5f, 12.5f, 13.0f);
            return true;
        case 10: // the routing matrix: lines crossing, a node on some crossings
            for (float y : { 6.0f, 10.0f, 14.0f, 18.0f })
                a.line (2.5f, y, 21.5f, y);
            for (float x : { 7.0f, 12.0f, 17.0f })
                a.line (x, 3.0f, x, 21.0f);
            for (auto [x, y] : { std::pair { 7.0f, 4.5f }, { 17.0f, 8.5f }, { 12.0f, 12.5f }, { 17.0f, 16.5f } })
                a.rect (x - 1.5f, y, 3.0f, 3.0f, 0.4f);
            return true;
        case 11: // a bus: rows into a spine, the top one an arrow
            a.line (7.0f, 4.0f, 7.0f, 20.0f);
            a.line (3.0f, 4.0f, 20.0f, 4.0f);
            a.poly ({ 18.5f, 2.5f, 21.0f, 4.0f, 18.5f, 5.5f }, true);
            for (float x : { 10.0f, 13.0f })
                a.line (x, 4.0f, x, 8.0f);
            for (float y : { 8.0f, 12.0f, 16.0f, 20.0f })
            {
                a.line (3.0f, y, 19.5f, y);
                a.dot (20.0f, y, 1.2f);
            }
            return true;
        case 12: // a sum: four rows into a column of nodes
            for (float y : { 5.5f, 10.0f, 14.5f, 19.0f })
            {
                a.line (3.0f, y, 16.5f, y);
                a.rect (16.5f, y - 1.5f, 3.0f, 3.0f, 0.4f);
            }
            a.line (18.0f, 2.5f, 18.0f, 4.0f);
            for (float y : { 7.0f, 11.5f, 16.0f })
                a.line (18.0f, y, 18.0f, y + 1.5f);
            a.line (18.0f, 20.5f, 18.0f, 22.0f);
            return true;
        case 13:
            a.circle (12.0f, 12.0f, 9.0f);
            a.dot (9.0f, 9.5f, 1.2f);
            a.dot (15.0f, 9.5f, 1.2f);
            a.arc (12.0f, 12.0f, 5.0f, 5.0f, 115.0f, 245.0f);
            return true;
        case 14: // a plain bold W
            a.fill ({ 1.5f, 6.0f, 5.6f, 6.0f, 8.3f, 15.0f, 10.6f, 8.0f, 13.4f, 8.0f, 15.7f, 15.0f, 18.4f, 6.0f, 22.5f, 6.0f,
                      17.8f, 19.0f, 14.0f, 19.0f, 12.0f, 12.6f, 10.0f, 19.0f, 6.2f, 19.0f });
            return true;

        // Vocals and microphones
        case 101:
            a.circle (12.0f, 6.0f, 3.5f);
            a.poly ({ 9.6f, 9.0f, 14.4f, 9.0f, 13.4f, 21.0f, 10.6f, 21.0f }, true);
            a.arc (12.0f, 6.0f, 6.5f, 6.5f, 25.0f, 75.0f);
            a.arc (12.0f, 6.0f, 9.0f, 9.0f, 25.0f, 75.0f);
            return true;
        case 102:
        case 111:
            a.circle (16.0f, 8.0f, 4.5f);
            a.line (12.8f, 4.8f, 19.2f, 11.2f);
            a.poly ({ 12.3f, 10.0f, 14.0f, 11.7f, 6.0f, 21.0f, 3.5f, 18.5f }, true);
            return true;
        case 103:
            a.rect (8.0f, 2.0f, 8.0f, 13.0f, 4.0f);
            a.line (8.0f, 8.5f, 16.0f, 8.5f);
            a.arc (12.0f, 10.0f, 6.0f, 6.0f, 100.0f, 260.0f);
            a.stand (16.0f);
            return true;
        case 104:
        case 106:
            a.poly ({ 3.0f, 18.0f, 21.0f, 18.0f, 18.0f, 13.0f, 6.0f, 13.0f }, true);
            a.circle (12.0f, 10.5f, 2.5f);
            return true;
        case 105:
            a.rect (10.5f, 3.0f, 3.0f, 7.0f, 1.5f);
            a.rect (9.0f, 10.0f, 6.0f, 3.0f, 0.8f);
            a.curve (12.0f, 13.0f, 12.0f, 18.0f, 8.0f, 19.0f, 8.0f, 22.0f);
            return true;
        case 107:
            a.circle (12.0f, 11.0f, 6.0f);
            a.arc (12.0f, 11.0f, 8.5f, 8.5f, -85.0f, 85.0f);
            a.curve (4.0f, 13.0f, 4.0f, 17.5f, 6.5f, 19.0f, 10.0f, 18.5f);
            a.dot (10.5f, 18.5f, 1.4f);
            return true;
        case 108:
            a.line (2.5f, 4.0f, 15.5f, 4.0f);
            a.line (15.5f, 4.0f, 15.5f, 6.0f);
            a.rect (14.0f, 6.0f, 3.0f, 11.0f, 1.5f);
            a.arc (15.5f, 18.5f, 4.0f, 3.0f, 120.0f, 240.0f);
            return true;
        case 110:
            a.rect (9.0f, 3.0f, 6.0f, 10.0f, 3.0f);
            a.circle (12.0f, 8.0f, 6.5f);
            a.stand (14.5f);
            return true;
        case 112:
            a.person (6.0f, 8.5f, 2.5f);
            a.person (18.0f, 8.5f, 2.5f);
            a.person (12.0f, 6.5f, 2.8f);
            return true;
        case 113:
            a.circle (12.0f, 8.0f, 4.0f);
            a.curve (8.0f, 8.0f, 7.0f, 12.0f, 7.0f, 14.0f, 6.0f, 15.5f);
            a.curve (16.0f, 8.0f, 17.0f, 12.0f, 17.0f, 14.0f, 18.0f, 15.5f);
            a.arc (12.0f, 23.0f, 8.0f, 6.5f, -70.0f, 70.0f);
            return true;
        case 114:
            a.circle (12.0f, 8.0f, 4.2f);
            a.arc (12.0f, 23.0f, 8.5f, 7.0f, -70.0f, 70.0f);
            return true;

        // Drums
        case 200:
            a.circle (12.0f, 12.0f, 9.5f);
            a.circle (12.0f, 15.0f, 3.0f);
            a.line (12.0f, 4.0f, 12.0f, 10.5f);
            a.dot (12.0f, 10.5f, 1.4f);
            return true;
        case 201:
            a.circle (10.5f, 11.0f, 8.5f);
            a.circle (10.5f, 13.5f, 2.6f);
            a.rect (19.0f, 15.0f, 3.0f, 6.5f, 1.5f);
            return true;
        case 202:
        case 203:
            a.drumSide (11.0f, 10.5f, 8.0f, 6.0f);
            if (number == 202)
                a.rect (15.0f, 1.5f, 3.0f, 6.0f, 1.5f);
            else
                a.rect (15.0f, 17.5f, 3.0f, 6.0f, 1.5f);
            return true;
        case 204:
            a.drumSide (12.0f, 10.0f, 9.0f, 7.0f);
            for (float x : { 6.0f, 12.0f, 18.0f })
                a.line (x, 12.0f, x, 16.0f);
            return true;
        case 205:
            a.ellipse (12.0f, 7.0f, 9.0f, 1.6f);
            a.ellipse (12.0f, 10.0f, 9.0f, 1.6f);
            a.line (12.0f, 5.0f, 12.0f, 20.5f);
            a.line (12.0f, 20.5f, 8.5f, 23.0f);
            a.line (12.0f, 20.5f, 15.5f, 23.0f);
            return true;
        case 206: a.drumSide (12.0f, 9.0f, 6.0f, 6.0f); return true;
        case 207: a.drumSide (12.0f, 8.5f, 7.0f, 7.0f); return true;
        case 208: a.drumSide (12.0f, 7.0f, 8.0f, 9.0f); return true;
        case 209:
            a.drumSide (12.0f, 5.0f, 8.0f, 11.0f);
            a.line (4.0f, 16.0f, 3.0f, 22.5f);
            a.line (20.0f, 16.0f, 21.0f, 22.5f);
            return true;
        case 211: a.cymbal (12.0f, 7.0f, 9.0f, -14.0f); return true;
        case 212:
            a.cymbal (12.0f, 7.0f, 10.0f, 0.0f);
            a.arc (12.0f, 6.5f, 2.2f, 1.4f, -90.0f, 90.0f);
            return true;

        // Strings and winds
        case 302:
        case 309:
            a.bowed (12.0f, 2.0f, 1.0f, false);
            return true;
        case 303:
        case 304:
            a.circle (8.5f, 16.0f, 5.5f);
            a.arc (12.0f, 11.5f, 4.0f, 4.0f, -30.0f, 170.0f);
            a.circle (9.5f, 14.5f, 1.6f);
            a.line (12.5f, 10.5f, 20.0f, 3.5f);
            a.poly ({ 19.0f, 3.0f, 21.0f, 1.0f, 22.5f, 2.5f, 20.5f, 4.5f }, true);
            return true;
        case 308:
        case 310:
            a.bowed (12.0f, 1.0f, 1.12f, true);
            return true;
        case 312:
        case 311:
            a.poly ({ 6.0f, 2.5f, 9.0f, 3.5f, 10.0f, 6.0f, 10.0f, 16.0f });
            a.curve (10.0f, 16.0f, 10.0f, 21.5f, 17.0f, 21.5f, 17.0f, 16.0f);
            a.line (17.0f, 16.0f, 17.0f, 12.0f);
            a.ellipse (17.0f, 11.0f, 3.0f, 1.2f);
            a.dot (10.0f, 9.5f, 1.0f);
            a.dot (10.0f, 12.5f, 1.0f);
            return true;
        case 313:
        case 314:
            a.line (2.0f, 12.0f, 4.0f, 12.0f);
            a.line (4.0f, 11.0f, 15.0f, 11.0f);
            a.line (4.0f, 13.0f, 15.0f, 13.0f);
            a.poly ({ 15.0f, 11.0f, 21.5f, 7.0f, 21.5f, 17.0f, 15.0f, 13.0f });
            for (float x : { 7.0f, 9.5f, 12.0f })
                a.rect (x, 6.0f, 1.5f, 5.0f);
            a.rect (6.0f, 13.0f, 8.0f, 5.0f, 2.5f);
            return true;

        // Keys
        case 400:
            a.poly ({ 4.0f, 4.0f, 20.0f, 4.0f, 20.0f, 10.0f });
            a.curve (20.0f, 10.0f, 20.0f, 15.0f, 14.0f, 14.0f, 12.0f, 20.0f);
            a.poly ({ 12.0f, 20.0f, 4.0f, 20.0f, 4.0f, 4.0f });
            a.line (4.0f, 7.5f, 20.0f, 7.5f);
            for (float x : { 7.0f, 10.0f, 13.0f, 16.0f })
                a.line (x, 4.0f, x, 7.5f);
            return true;
        case 401:
            a.rect (4.0f, 3.0f, 16.0f, 12.0f, 0.8f);
            a.rect (2.5f, 13.0f, 19.0f, 3.0f);
            a.line (5.0f, 16.0f, 5.0f, 21.0f);
            a.line (19.0f, 16.0f, 19.0f, 21.0f);
            return true;
        case 409:
            a.rect (4.0f, 11.5f, 16.0f, 10.0f, 0.8f);
            a.rect (3.0f, 6.0f, 18.0f, 2.8f);
            a.rect (3.0f, 8.8f, 18.0f, 2.8f);
            for (float x : { 7.0f, 9.0f, 11.0f, 13.0f, 15.0f })
                a.line (x, 3.0f, x, 6.0f);
            return true;

        // Speakers
        case 500:
            a.rect (3.0f, 2.5f, 18.0f, 6.0f, 0.8f);
            for (float x : { 6.0f, 9.0f, 12.0f })
                a.circle (x, 5.5f, 1.0f);
            a.rect (3.0f, 9.5f, 18.0f, 12.0f, 0.8f);
            a.circle (8.0f, 15.5f, 3.4f);
            a.circle (16.0f, 15.5f, 3.4f);
            return true;
        case 501:
            a.rect (3.0f, 3.0f, 18.0f, 18.0f, 0.8f);
            for (float x : { 8.0f, 16.0f })
                for (float y : { 8.0f, 16.0f })
                    a.circle (x, y, 3.3f);
            return true;
        case 502:
        case 506:
            a.rect (3.5f, 5.0f, 17.0f, 15.0f, 0.8f);
            a.circle (12.0f, 12.5f, 5.5f);
            return true;
        case 504:
        case 513:
        case 514:
        case 519:
            a.speakerBox (2.0f, 4.0f, 9.0f, 16.0f);
            a.speakerBox (13.0f, 4.0f, 9.0f, 16.0f);
            return true;
        case 507:
        case 508:
            a.speakerBox (7.0f, 4.0f, 10.0f, 15.0f);
            return true;
        case 509:
            a.rect (9.0f, 1.5f, 6.0f, 21.0f, 0.8f);
            for (float y : { 5.0f, 9.5f, 14.0f, 18.5f })
                a.circle (12.0f, y, 1.8f);
            return true;
        case 515:
        case 516:
        case 517:
        case 518:
        case 520:
        case 521:
        case 522:
        {
            a.wedge();
            const char* letter = number == 515 || number == 520 ? "L" : (number == 516 || number == 521 ? "R" : (number == 522 ? "C" : ""));
            if (*letter != 0)
                a.text (letter, 14.0f, 1.0f, 8.0f, 8.0f);
            return true;
        }
        case 523:
            a.line (12.0f, 0.5f, 12.0f, 3.0f);
            a.rect (5.0f, 3.0f, 14.0f, 3.5f);
            a.rect (5.5f, 7.0f, 13.0f, 3.5f);
            a.rect (6.5f, 11.0f, 11.0f, 3.5f);
            a.rect (8.0f, 15.0f, 8.0f, 3.5f);
            return true;
        case 524:
            a.line (3.0f, 4.0f, 21.0f, 4.0f);
            a.circle (12.0f, 13.0f, 8.0f);
            for (float y : { 10.0f, 13.0f, 16.0f })
                a.line (6.5f, y, 17.5f, y);
            return true;

        // Specials
        case 600:
            a.rect (7.0f, 11.0f, 10.0f, 10.0f, 3.0f);
            a.rect (7.0f, 4.0f, 2.2f, 8.0f, 1.1f);
            a.rect (9.6f, 2.5f, 2.2f, 9.0f, 1.1f);
            a.rect (12.2f, 3.0f, 2.2f, 8.5f, 1.1f);
            a.rect (14.8f, 5.0f, 2.2f, 7.0f, 1.1f);
            a.line (7.0f, 16.0f, 3.5f, 12.0f);
            return true;
        case 601:
            a.curve (8.0f, 9.5f, 8.0f, 3.0f, 18.0f, 2.0f, 18.0f, 9.0f);
            a.curve (18.0f, 9.0f, 18.0f, 13.0f, 14.0f, 14.0f, 14.0f, 17.0f);
            a.curve (14.0f, 17.0f, 14.0f, 21.5f, 9.0f, 22.0f, 8.0f, 19.0f);
            a.curve (11.0f, 10.0f, 11.0f, 6.0f, 15.0f, 6.0f, 15.0f, 9.5f);
            a.line (15.0f, 9.5f, 13.0f, 12.0f);
            return true;
        case 603:
        case 604:
            a.speakerBox (3.0f, 3.0f, 10.0f, 18.0f);
            a.text (number == 603 ? "A" : "B", 13.5f, 2.0f, 9.0f, 10.0f);
            return true;
        case 605:
            a.rect (5.0f, 5.0f, 14.0f, 10.0f, 1.0f);
            a.poly ({ 2.0f, 17.0f, 22.0f, 17.0f, 20.0f, 19.5f, 4.0f, 19.5f }, true);
            return true;
        case 606:
            a.rect (7.0f, 2.0f, 10.0f, 20.0f, 2.0f);
            a.rect (9.0f, 4.0f, 6.0f, 6.0f);
            a.circle (12.0f, 15.5f, 3.2f);
            a.circle (12.0f, 15.5f, 1.0f);
            return true;
        case 607:
            a.rect (7.0f, 2.0f, 10.0f, 20.0f, 2.0f);
            a.rect (8.5f, 4.5f, 7.0f, 13.0f);
            a.circle (12.0f, 19.8f, 0.9f);
            return true;
        case 610:
            a.circle (12.0f, 12.0f, 9.5f);
            a.circle (12.0f, 12.0f, 2.5f);
            a.arc (12.0f, 12.0f, 6.0f, 6.0f, 200.0f, 250.0f);
            return true;
        case 611:
            a.rect (2.0f, 4.0f, 20.0f, 16.0f, 1.5f);
            a.circle (10.0f, 12.0f, 6.0f);
            a.dot (10.0f, 12.0f, 0.9f);
            a.poly ({ 19.0f, 6.0f, 18.5f, 15.0f, 15.0f, 17.0f });
            return true;
        case 612:
            a.circle (7.0f, 8.0f, 4.5f);
            a.circle (17.0f, 8.0f, 4.5f);
            a.rect (3.0f, 14.0f, 18.0f, 7.0f, 1.0f);
            return true;
        case 613:
            a.rect (2.0f, 5.0f, 20.0f, 14.0f, 1.5f);
            a.circle (8.0f, 11.0f, 2.2f);
            a.circle (16.0f, 11.0f, 2.2f);
            a.poly ({ 6.0f, 19.0f, 7.5f, 15.5f, 16.5f, 15.5f, 18.0f, 19.0f });
            return true;
        case 614:
            a.rect (2.0f, 7.0f, 20.0f, 10.0f, 1.0f);
            for (int i = 0; i < 6; ++i)
                for (float y : { 10.0f, 14.0f })
                    a.circle (4.5f + 3.0f * (float) i, y, 0.9f);
            return true;
        default:
            break;
    }

    // Other numbers: their category's icon.
    switch (number / 100)
    {
        case 1:
            a.rect (9.0f, 2.0f, 6.0f, 9.0f, 3.0f);
            a.line (9.0f, 6.5f, 15.0f, 6.5f);
            a.arc (12.0f, 8.5f, 5.5f, 5.5f, 95.0f, 265.0f);
            a.stand (14.0f);
            return true;
        case 2:
            a.drumSide (12.0f, 10.0f, 8.0f, 8.0f);
            a.line (6.0f, 2.5f, 12.5f, 8.5f);
            a.line (18.0f, 2.5f, 11.5f, 8.5f);
            return true;
        case 3:  a.electricGuitar(); return true;
        case 4:  a.synth(); return true;
        case 5:  a.speakerBox (6.0f, 2.0f, 12.0f, 20.0f); return true;
        default: break;
    }
    return false;
}

} // namespace livedaw::ui
