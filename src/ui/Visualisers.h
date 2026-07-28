// ---------------------------------------------------------------------------
// Editable graphs.
//
// A knob labelled "Attack 5 ms" tells you a number. A curve tells you the
// shape, and lets you grab it. Every serious synth puts the envelope, the LFO
// and the wavetable on screen because those three are shapes, not values, and
// reading them as numbers is work the eye should not have to do.
// ---------------------------------------------------------------------------
#pragma once

#include "core/dsp/Assets.h"
#include "plugin/ParameterPool.h"

#include <juce_gui_basics/juce_gui_basics.h>

namespace forge {

/// ADSR curve with draggable breakpoints, wired straight to the host
/// parameters so dragging the graph and turning the knob are the same edit.
class EnvelopeGraph final : public juce::Component,
                            public juce::SettableTooltipClient,
                            private juce::Timer {
public:
    struct Bindings {
        ForgeParameter* attack  = nullptr;
        ForgeParameter* decay   = nullptr;
        ForgeParameter* sustain = nullptr;
        ForgeParameter* release = nullptr;
    };

    EnvelopeGraph(juce::String title, Bindings bindings, juce::Colour accent);
    ~EnvelopeGraph() override;

    void paint(juce::Graphics&) override;
    void mouseDown(const juce::MouseEvent&) override;
    void mouseDrag(const juce::MouseEvent&) override;
    void mouseUp(const juce::MouseEvent&) override;
    void mouseMove(const juce::MouseEvent&) override;

private:
    enum class Handle { None, Attack, Decay, Sustain, Release };

    void timerCallback() override;
    juce::Rectangle<float> plotArea() const;
    /// Normalised 0..1 positions of the four breakpoints along the curve.
    void breakpoints(juce::Point<float> out[4]) const;
    Handle hitTest(juce::Point<float>) const;

    juce::String  title_;
    Bindings      bind_;
    juce::Colour  accent_;
    Handle        dragging_ = Handle::None;
    Handle        hover_    = Handle::None;
    juce::Point<float> dragStart_;
    float         dragStartValue_ = 0.0f;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(EnvelopeGraph)
};

/// One cycle of the LFO's current shape, with the rate written under it.
class LfoGraph final : public juce::Component,
                       public juce::SettableTooltipClient,
                       private juce::Timer {
public:
    LfoGraph(juce::String title, juce::String shape, ForgeParameter* rate, juce::Colour accent);
    ~LfoGraph() override;

    void setShape(juce::String);
    void paint(juce::Graphics&) override;

private:
    void timerCallback() override;
    float sample(float phase) const;

    juce::String    title_, shape_;
    ForgeParameter* rate_ = nullptr;
    juce::Colour    accent_;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(LfoGraph)
};

/// Frequency response with draggable bands, in the manner of Fruity
/// Equalizer 2. Drag a node horizontally for frequency, vertically for gain;
/// the mid band takes Q on the mouse wheel. The curve is computed from the
/// same biquad coefficients the audio thread runs.
class EqCurve final : public juce::Component,
                      public juce::SettableTooltipClient,
                      private juce::Timer {
public:
    struct Band {
        ForgeParameter* gain = nullptr;
        ForgeParameter* freq = nullptr;
        ForgeParameter* q    = nullptr;   ///< peak band only
    };

    EqCurve(Band low, Band mid, Band high, juce::Colour accent, double sampleRate);
    ~EqCurve() override;

    void paint(juce::Graphics&) override;
    void mouseDown(const juce::MouseEvent&) override;
    void mouseDrag(const juce::MouseEvent&) override;
    void mouseUp(const juce::MouseEvent&) override;
    void mouseMove(const juce::MouseEvent&) override;
    void mouseWheelMove(const juce::MouseEvent&, const juce::MouseWheelDetails&) override;

private:
    void timerCallback() override;
    juce::Rectangle<float> plot() const;
    float freqToX(float hz) const;
    float xToFreq(float x) const;
    float dbToY(float db) const;
    float yToDb(float y) const;
    float responseDbAt(float hz) const;
    juce::Point<float> handleFor(int band) const;
    int hitTest(juce::Point<float>) const;

    Band   bands_[3];
    juce::Colour accent_;
    double sampleRate_ = 48000.0;
    int    dragging_ = -1;
    int    hover_    = -1;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(EqCurve)
};

/// The wavetable's frames drawn back-to-front in a shallow oblique projection -
/// the stacked-ribbon view every wavetable synth uses, because it shows how the
/// timbre changes across the morph in one glance.
class WavetableGraph final : public juce::Component,
                             public juce::SettableTooltipClient,
                             private juce::Timer {
public:
    WavetableGraph(juce::String title, const Wavetable&, ForgeParameter* position,
                   juce::Colour accent);
    ~WavetableGraph() override;

    void paint(juce::Graphics&) override;

private:
    void timerCallback() override;

    juce::String    title_;
    Wavetable       table_;
    ForgeParameter* position_ = nullptr;
    juce::Colour    accent_;
    float           lastPosition_ = -1.0f;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(WavetableGraph)
};

} // namespace forge
