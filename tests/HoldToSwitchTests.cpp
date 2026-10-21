#include "ui/HoldToSwitch.h"

using livedaw::ui::HoldToSwitch;

/** The press-and-hold timing of the +48V keys: a tap or a short hold does
    nothing, a hold of kPhantomHoldSeconds switches once. */
class HoldToSwitchTests final : public juce::UnitTest
{
public:
    HoldToSwitchTests() : juce::UnitTest ("Hold to switch", "LiveDAW") {}

    void runTest() override
    {
        constexpr double hold = livedaw::ui::kPhantomHoldSeconds;

        beginTest ("+48V needs a 2 s hold");
        expectEquals (hold, 2.0);

        beginTest ("Holding for the hold time switches once");
        {
            HoldToSwitch h;
            expect (! h.update (100.0));
            expectEquals (h.progress (100.0), 0.0f);
            h.press (100.0);
            expect (h.isCounting());
            expect (! h.update (100.0 + hold * 0.5));
            expectWithinAbsoluteError (h.progress (100.0 + hold * 0.5), 0.5f, 1.0e-4f);
            expect (h.update (100.0 + hold));
            // Kept held: nothing more, and the progress is back at 0.
            expect (! h.isCounting());
            expect (! h.update (100.0 + hold * 3.0));
            expectEquals (h.progress (100.0 + hold * 3.0), 0.0f);
            h.release();
            expect (! h.update (100.0 + hold * 4.0));
        }

        beginTest ("A tap or a shorter hold does nothing");
        {
            HoldToSwitch h;
            h.press (0.0);
            h.release();
            expect (! h.update (hold * 2.0));
            h.press (10.0);
            expect (! h.update (10.0 + hold * 0.5)); // 1 s
            h.release();
            expect (! h.update (10.0 + hold * 2.0));
            expectEquals (h.progress (10.0 + hold), 0.0f);
        }

        beginTest ("Leaving the key cancels; pressing again starts over");
        {
            HoldToSwitch h;
            h.press (0.0);
            expect (! h.update (hold * 0.9));
            h.release(); // dragged off the key
            h.press (hold);
            expect (! h.update (hold * 1.9));
            expectWithinAbsoluteError (h.progress (hold * 1.5), 0.5f, 1.0e-4f);
            expect (h.update (hold * 2.0));
            // A new press after switching counts again.
            h.release();
            h.press (hold * 3.0);
            expect (h.update (hold * 4.0));
        }

        beginTest ("The hold time is the key's own");
        {
            HoldToSwitch h (0.5);
            h.press (0.0);
            expect (! h.update (0.4));
            expect (h.update (0.5));
        }
    }
};

static HoldToSwitchTests holdToSwitchTests;
