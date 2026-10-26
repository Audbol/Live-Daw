#pragma once

#include <juce_core/juce_core.h>

#include <array>

namespace livedaw::ui
{

/** The surface lock (manual chapter 10 p. 91): "Hold HOME. Alternatively,
    hold HOME together with any other combination of buttons to serve as
    'password'"; it ends by holding HOME with the same combination. The lock
    covers the control surface and the touchscreen, never the audio or the
    remotes; the keys beside the display stay live for unlocking.

    The combination is the WING's lock string (`/$ctl/$stat/cnslock`): 19
    characters '0' or '1', chars 1-7 the keys left of the display starting
    with HOME, chars 18 and 19 the keys right of it (CLR SOLO, the blank key);
    empty while unlocked. */
class SurfaceLock
{
public:
    static constexpr int kCodeLength = 19;
    static constexpr int kLeftKeys = 7;

    /** The lock string of the keys held with HOME (`left[0]` is HOME itself,
        always part of it). */
    static juce::String code (const std::array<bool, kLeftKeys>& left, bool clearSolo, bool blank)
    {
        juce::String s;
        for (int i = 0; i < kCodeLength; ++i)
        {
            bool held = false;
            if (i < kLeftKeys)
                held = i == 0 || left[(size_t) i];
            else if (i == kCodeLength - 2)
                held = clearSolo;
            else if (i == kCodeLength - 1)
                held = blank;
            s << (held ? '1' : '0');
        }
        return s;
    }

    /** HOME held alone (also Ctrl+L). */
    static juce::String homeOnly() { return code ({}, false, false); }

    enum class Result
    {
        Locked,
        Unlocked,
        WrongCode // still locked: another combination than the lock's
    };

    /** HOME held (with the keys of `held`, a lock string): locks with that
        combination, or unlocks when it is the lock's. */
    Result holdHome (const juce::String& held)
    {
        if (lockCode.isEmpty())
        {
            lockCode = held;
            return Result::Locked;
        }
        if (held != lockCode)
            return Result::WrongCode;
        lockCode.clear();
        return Result::Unlocked;
    }

    bool isLocked() const noexcept { return lockCode.isNotEmpty(); }
    /** The lock string, empty while unlocked. */
    const juce::String& getCode() const noexcept { return lockCode; }

private:
    juce::String lockCode;
};

} // namespace livedaw::ui
