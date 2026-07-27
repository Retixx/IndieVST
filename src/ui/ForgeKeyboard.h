// ---------------------------------------------------------------------------
// The on-screen keyboard.
//
// Two things the stock component does not give you: the full MIDI range, and a
// way to reach it from a computer keyboard. JUCE maps roughly one octave of
// letter keys at a fixed base, so without an octave shift most of the range is
// simply unplayable without hardware.
// ---------------------------------------------------------------------------
#pragma once

#include "ui/Fonts.h"
#include "ui/ForgeLookAndFeel.h"

#include <juce_audio_utils/juce_audio_utils.h>

namespace forge {

class ForgeKeyboard final : public juce::MidiKeyboardComponent,
                            public juce::SettableTooltipClient {
public:
    /// Parameter is named `sharedState` because MidiKeyboardComponent already
    /// has a `state` member.
    explicit ForgeKeyboard(juce::MidiKeyboardState& sharedState)
        : juce::MidiKeyboardComponent(sharedState,
                                      juce::MidiKeyboardComponent::horizontalKeyboard) {

        // Full MIDI range. Note that MIDI stops at 127, so with FL Studio's
        // convention (middle C = C5) the top of the range is G10, not C11 -
        // C11 would be note 132 and does not exist in the protocol.
        setOctaveForMiddleC(5);
        setAvailableRange(0, 127);
        setLowestVisibleKey(0);
        setScrollButtonsVisible(false);
        setWantsKeyboardFocus(true);
        setKeyPressBaseOctave(computerKeyOctave_);
        updateTooltip();
    }

    /// Octave of the lowest note the computer keys play.
    int computerKeyOctave() const noexcept { return computerKeyOctave_; }

    /// JUCE draws the octave label inside the key rectangle, so at 13 px per
    /// white key "C10" gets clipped to "C1". We suppress the built-in label and
    /// draw our own afterwards, free to overhang into the neighbouring key -
    /// which is what hardware and every DAW piano roll does anyway.
    juce::String getWhiteNoteText(int) override { return {}; }

    void paint(juce::Graphics& g) override {
        juce::MidiKeyboardComponent::paint(g);

        g.setFont(fonts::get(8.5f, fonts::Weight::Medium));
        for (int note = 0; note <= 127; note += 12) {
            const auto key = getRectangleForKey(note);
            if (key.isEmpty()) continue;
            // With octaveForMiddleC(5), note 60 is C5, so the octave is
            // simply note / 12.
            g.setColour(juce::Colour(0xff4a4a52));
            g.drawText("C" + juce::String(note / 12),
                       juce::Rectangle<float>(key.getX() + 1.5f,
                                              key.getBottom() - 12.0f,
                                              26.0f, 10.0f).toNearestInt(),
                       juce::Justification::centredLeft, false);
        }
    }

    bool keyPressed(const juce::KeyPress& key) override {
        const auto c = juce::CharacterFunctions::toLowerCase(key.getTextCharacter());
        if (c == 'z' || key.getKeyCode() == juce::KeyPress::leftKey)  { shiftOctave(-1); return true; }
        if (c == 'x' || key.getKeyCode() == juce::KeyPress::rightKey) { shiftOctave(+1); return true; }
        return juce::MidiKeyboardComponent::keyPressed(key);
    }

    /// Clicking anywhere on the keyboard should also hand it the keystrokes.
    void mouseDown(const juce::MouseEvent& e) override {
        grabKeyboardFocus();
        juce::MidiKeyboardComponent::mouseDown(e);
    }

private:
    void shiftOctave(int delta) {
        // JUCE clamps the base octave to 0..10 internally; mirroring that here
        // keeps our label honest.
        computerKeyOctave_ = juce::jlimit(0, 10, computerKeyOctave_ + delta);
        setKeyPressBaseOctave(computerKeyOctave_);
        updateTooltip();
        if (onOctaveChanged) onOctaveChanged();
    }

    void updateTooltip() {
        setTooltip("Computer keys play from C" + juce::String(computerKeyOctave_)
                   + "  -  A W S E D F T G Y H U J K.  Z / X shifts octave.");
    }

    int computerKeyOctave_ = 4;

public:
    std::function<void()> onOctaveChanged;
};

} // namespace forge
