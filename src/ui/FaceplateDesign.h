#pragma once

#include "engine/Models.h"

#include <juce_gui_basics/juce_gui_basics.h>

#include <functional>
#include <vector>

/** Drawn front panels: a unit's layout as the console's screen shows it, in the
    screen's own faceplate frame (1156 x 479 px: a dark scratched backdrop with
    the unit's panel inset). Coordinates are pixels of that frame, so layouts can
    be measured straight off a screen picture. Used by Faceplate (controls bound
    to the model's parameters) and its thumbnails. */
namespace livedaw::ui::fp
{

constexpr float kFrameW = 1156.0f, kFrameH = 479.0f;

enum class Type
{
    Knob,    // rotary control; x, y = centre, w = diameter
    Toggle,  // bat-lever switch; x, y = centre, w = nut diameter
    Key,     // push key (look decides the style); x, y, w, h = rectangle
    Fader,   // vertical fader; x, y, w, h = slot area (cap travels inside)
    Vu,      // needle meter showing the gain reduction; x, y, w, h = face
    Leds,    // a row or column of LEDs showing the gain reduction
    Lamp,    // round indicator; x, y = centre, w = diameter
    Text,    // static print; x, y, w, h = box
    Path,    // decorative SVG path (text = path data)
    Box,     // decorative rectangle (sections, windows, cheeks)
    Screw,   // panel screw; x, y = centre, w = diameter
    Display, // alphanumeric display showing parameter values
    Value,   // a parameter's value printed (7-segment / LCD); x, y, w, h = box
    Graph    // EQ response: bands of `params` (freq, gain, q, filter type) in grid; x, y, w, h
};

enum class Look
{
    // Knobs
    ChromeCollar, // knurled silver cap inside a black collar with a white pointer
    ChromeSmall,  // small knurled silver knob with a ring pointer
    BlackFluted,  // fluted black knob on a black skirt with a white line
    BlackDome,    // black dome, light ring, white line
    ColourCap,    // coloured cap in a silver bezel, white line
    ChromeBar,    // chrome knob with a ribbed band
    Chicken,      // black chicken-head pointer
    BlackSkirt,   // ribbed black knob, white line
    Pedal,        // big black pedal knob
    WhiteDome,    // cream knob with a black line
    Chrome,       // plain silver cone with a dark line
    SilverDial,   // big silver dial with a dark ring
    BlackDot,     // glossy black knob in a ribbed ring, white dot marker
    Knurled,      // knurled ring around a glossy dome (colour), white line
    ColourDome,   // black ring around a glossy coloured dome, white line
    NumberDial,   // white dial whose printed numbers turn under a fixed index
    Lobed,        // cream lobed knob with a coloured cap
    // Keys
    SoulKey,    // grey raised key with a caption and an LED to its right
    LitSquare,  // square key that lights (colour) when on
    StackKey,   // one key of an interlocked stack (pressed = down)
    RoundKey,   // round dark push button with an LED above
    LedKey,     // small square key with an LED in it
    FlatKey,    // flat screen key, filled with colour when on
    Hidden,     // invisible key over printed text (a click area)
    TabKey,     // a tab that shows page `choice` of the panel
    // Vu faces
    VuAmber,    // backlit amber face, chrome bezel
    VuAmberDark,// backlit amber face, black housing
    VuWhite,    // dark face with white print
    VuCream,    // amber arched face in a black housing
    VuRound,    // round window: amber scale over black, glowing rim (colour)
    VuOrange,   // backlit orange face, black housing with a chrome edge
    // Boxes
    Fill,       // flat fill (colour), optional outline (colour2)
    Bevel,      // raised plate (colour) with a highlight and shadow
    Inset,      // recessed window (colour)
    Wood,       // wooden cheek
    Line,       // outline only (colour)
    // Text faces
    Sans,       // panel print (condensed sans, bold)
    SansLight,  // condensed sans, regular
    Serif,      // serif
    SerifBold,
    SerifItalic,
    Script,     // bold italic logo lettering
    Segment,    // 7-segment display digits
    Dot         // dot-matrix display
};

enum class Texture
{
    Smooth,
    Grunge,  // painted metal with wear
    Brushed, // horizontal brushing
    Rough    // powder coat
};

struct Element
{
    Type type = Type::Text;
    float x = 0, y = 0, w = 0, h = 0;
    int param = -1;      // bound parameter (index in the model), -1 = decoration
    Look look = Look::Sans;
    juce::Colour colour { 0xffe0e0e0 }, colour2 { 0xff000000 };
    juce::String text;
    float size = 14.0f;  // font size / line width
    juce::Justification just = juce::Justification::centred;
    float kern = 0.0f;   // extra letter spacing (text)

    // Knob scale
    int ticks = 0;          // number of scale marks (0 = none)
    bool dots = true;       // dots or lines
    float sweep = 270.0f;   // degrees
    float centre = 0.0f;    // direction of the middle of the sweep (degrees, 0 = up)
    juce::Colour band { 0x00000000 }; // a coloured ring under the scale
    float tickRadius = 1.22f, labelRadius = 1.55f; // in knob radii
    juce::StringArray labels; // spread evenly over the sweep
    float labelSize = 12.0f;
    juce::Colour scaleColour { 0xffe8e8e8 };
    bool skipLabelled = false; // no mark where a label sits
    bool rotate = false;       // labels follow the arc
    float fixed = 0.5f;     // position of a decorative knob / fader

    // Keys, toggles, lamps: the value index this element shows / sets (-1: on = max)
    int choice = -1;
    bool invert = false;    // toggle up / key lit when the value is at its minimum
    int alsoOn = -1;        // a second value index that lights the element

    // Meters
    juce::StringArray scale; // Vu labels / LED captions
    std::vector<float> marks; // Vu label positions (gain reduction dB) / LED thresholds
    float range = 20.0f;     // Vu: dB at the end of the scale
    bool vertical = true;    // Leds
    bool reverse = false;    // Vu: needle rises with reduction (compression meters)
    float vuZero = 1.0f;     // Vu: where 0 dB sits along the arc (0..1)
    float captionY = 0.74f;  // Vu: caption position (fraction of the face height)
    float housing = 0.0f;    // Vu: extra housing below the face (px)
    float dbMin = 0.0f, dbMax = 0.0f; // Vu: a linear dB scale (level = -reduction) instead of the VU law

    // Display: the parameters it shows; Graph: four per band (freq, gain, q, filter type)
    std::vector<int> params;
    int page = -1;            // shown only on this page of the panel (-1: always)

    // Chained setters
    Element& p (int i) { param = i; return *this; }
    Element& col (juce::uint32 c) { colour = juce::Colour (c); return *this; }
    Element& col2 (juce::uint32 c) { colour2 = juce::Colour (c); return *this; }
    Element& scaleOf (int n, const char* labelList = "", float sw = 270.0f, bool dotMarks = true)
    {
        ticks = n;
        labels = juce::StringArray::fromTokens (labelList, "|", "");
        if (juce::String (labelList).isEmpty())
            labels.clear();
        sweep = sw;
        dots = dotMarks;
        return *this;
    }
    Element& radii (float tick, float label) { tickRadius = tick; labelRadius = label; return *this; }
    Element& lsize (float s) { labelSize = s; return *this; }
    Element& scol (juce::uint32 c) { scaleColour = juce::Colour (c); return *this; }
    Element& at (float pos) { fixed = pos; return *this; }
    Element& sel (int c) { choice = c; return *this; }
    Element& inv() { invert = true; return *this; }
    Element& j (juce::Justification jj) { just = jj; return *this; }
    Element& kerning (float k) { kern = k; return *this; }
    Element& skip() { skipLabelled = true; return *this; }
    Element& turned() { rotate = true; return *this; }
    Element& linearDb (float lo, float hi) { dbMin = lo; dbMax = hi; return *this; }
    Element& centred (float degrees) { centre = degrees; return *this; }
    Element& banded (juce::uint32 c) { band = juce::Colour (c); return *this; }
    Element& sideways() { vertical = false; return *this; }
};

struct Design
{
    juce::String token;
    juce::Colour panel { 0xff2a2a2a };
    Texture texture = Texture::Grunge;
    float wear = 1.0f;                  // texture strength
    juce::Rectangle<float> plate { 30.0f, 70.0f, 1096.0f, 336.0f }; // the unit's panel
    bool screws = true;
    bool backdrop = true;               // the screen's scratched frame around it
    std::vector<Element> elements;

    Element& add (Type t, float x, float y, float w = 0, float h = 0)
    {
        Element e;
        e.type = t;
        e.x = x;
        e.y = y;
        e.w = w;
        e.h = h;
        elements.push_back (e);
        return elements.back();
    }
    Element& knob (int param, float cx, float cy, float d, Look look, juce::uint32 colour = 0xff2a6ad8)
    {
        auto& e = add (Type::Knob, cx, cy, d, d);
        e.param = param;
        e.look = look;
        e.colour = juce::Colour (colour);
        return e;
    }
    Element& text (const juce::String& s, float x, float y, float w, float h, float size, juce::uint32 colour,
                   Look face = Look::Sans, juce::Justification jj = juce::Justification::centred)
    {
        auto& e = add (Type::Text, x, y, w, h);
        e.text = s;
        e.size = size;
        e.colour = juce::Colour (colour);
        e.look = face;
        e.just = jj;
        return e;
    }
    /** A caption centred on cx, baseline box from y. */
    Element& caption (const juce::String& s, float cx, float y, float size, juce::uint32 colour, Look face = Look::Sans)
    {
        return text (s, cx - 120.0f, y, 240.0f, size * 1.3f, size, colour, face);
    }
    Element& key (int param, float x, float y, float w, float h, Look look, const juce::String& s = {}, juce::uint32 colour = 0xffd03020)
    {
        auto& e = add (Type::Key, x, y, w, h);
        e.param = param;
        e.look = look;
        e.text = s;
        e.colour = juce::Colour (colour);
        return e;
    }
    Element& toggle (int param, float cx, float cy, float d = 26.0f)
    {
        auto& e = add (Type::Toggle, cx, cy, d, d);
        e.param = param;
        return e;
    }
    Element& box (float x, float y, float w, float h, Look look, juce::uint32 colour, juce::uint32 colour2 = 0x00000000, float line = 1.5f)
    {
        auto& e = add (Type::Box, x, y, w, h);
        e.look = look;
        e.colour = juce::Colour (colour);
        e.colour2 = juce::Colour (colour2);
        e.size = line;
        return e;
    }
    Element& path (const juce::String& svg, juce::uint32 colour, float width)
    {
        auto& e = add (Type::Path, 0, 0);
        e.text = svg;
        e.colour = juce::Colour (colour);
        e.size = width;
        return e;
    }
    Element& vu (float x, float y, float w, float h, Look look, const char* labelList, std::vector<float> at, const juce::String& cap)
    {
        auto& e = add (Type::Vu, x, y, w, h);
        e.look = look;
        e.scale = juce::StringArray::fromTokens (labelList, "|", "");
        e.marks = std::move (at);
        e.text = cap;
        return e;
    }
    Element& leds (float x, float y, float w, float h, const char* labelList, std::vector<float> thresholds, bool vert, juce::uint32 colour = 0xffff3020)
    {
        auto& e = add (Type::Leds, x, y, w, h);
        e.scale = juce::StringArray::fromTokens (labelList, "|", "");
        e.marks = std::move (thresholds);
        e.vertical = vert;
        e.colour = juce::Colour (colour);
        return e;
    }
    Element& lamp (float cx, float cy, float d, juce::uint32 colour, int param = -1)
    {
        auto& e = add (Type::Lamp, cx, cy, d, d);
        e.colour = juce::Colour (colour);
        e.param = param;
        return e;
    }
    Element& fader (int param, float x, float y, float w, float h, juce::uint32 cap = 0xff202020)
    {
        auto& e = add (Type::Fader, x, y, w, h);
        e.param = param;
        e.colour = juce::Colour (cap);
        return e;
    }
};

/** The drawn panel of a model, or nullptr (the generic panel). */
const Design* findDesign (models::Table, const juce::String& token);

/** What the painter needs to know about the bound model. */
struct PaintContext
{
    const models::Model* spec = nullptr;
    std::function<float (int)> value;  // a parameter's value
    float reduction = 0.0f;            // gain reduction, dB (>= 0)
    int touched = -1;                  // the parameter last touched (value bubble)
    int page = 0;                      // the panel page shown (tabbed panels)
    bool showValue = false;
    bool thumbnail = false;            // small: skip fine detail
};

/** Paints the design in frame coordinates (the caller sets the transform). */
void paintDesign (juce::Graphics&, const Design&, const PaintContext&);

/** The position 0..1 a parameter's control shows. */
float controlPosition (const models::Param&, float value);

/** The screen area (frame coordinates) an element reacts to. */
juce::Rectangle<float> hitArea (const Element&);

/** Whether a key / toggle / lamp element shows "on" for this value. */
bool elementOn (const Element&, const models::Param&, float value);

/** The value a click on a key / toggle sets. */
float clickValue (const Element&, const models::Param&, float value);

} // namespace livedaw::ui::fp
