#include "ui/Icons.h"

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
            p.addEllipse (r.reduced (s * 0.12f));
            g.strokePath (p, stroke);
            for (auto pt : { P (0.38f, 0.42f), P (0.62f, 0.42f), P (0.5f, 0.64f) })
                g.fillEllipse (pt.x - s * 0.06f, pt.y - s * 0.06f, s * 0.12f, s * 0.12f);
            return;
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
            const auto c = P (0.5f, 0.54f);
            p.addCentredArc (c.x, c.y, s * 0.32f, s * 0.32f, 0.0f, 0.75f, juce::MathConstants<float>::twoPi - 0.75f, true);
            p.startNewSubPath (P (0.5f, 0.12f));
            p.lineTo (P (0.5f, 0.5f));
            g.strokePath (p, stroke);
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
            p.startNewSubPath (P (0.12f, 0.62f));
            p.cubicTo (P (0.3f, 0.62f), P (0.32f, 0.3f), P (0.5f, 0.3f));
            p.cubicTo (P (0.68f, 0.3f), P (0.7f, 0.62f), P (0.88f, 0.62f));
            p.startNewSubPath (P (0.12f, 0.78f));
            p.lineTo (P (0.88f, 0.78f));
            g.strokePath (p, stroke);
            return;
        case Icon::AllPass90:
        case Icon::AllPass180:
        {
            p.startNewSubPath (P (0.12f, 0.35f));
            p.lineTo (P (0.88f, 0.35f));
            g.strokePath (p, stroke);
            p.clear();
            const float end = icon == Icon::AllPass90 ? 0.55f : 0.78f;
            p.startNewSubPath (P (0.12f, 0.55f));
            p.cubicTo (P (0.45f, 0.55f), P (0.55f, end), P (0.88f, end));
            g.strokePath (p, stroke);
            g.setFont (juce::FontOptions (s * 0.3f, juce::Font::bold));
            g.drawText (icon == Icon::AllPass90 ? "90" : "180", juce::Rectangle<float> (P (0.12f, 0.0f).x, P (0.0f, 0.02f).y, s * 0.76f, s * 0.3f),
                        juce::Justification::centred);
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
    }
}

} // namespace livedaw::ui
