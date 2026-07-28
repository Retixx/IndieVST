// ---------------------------------------------------------------------------
// The static parameter pool (SPEC 9.1).
//
// This is the sharpest constraint in the whole project. VST3 hosts enumerate a
// plugin's parameters when they scan it and do not tolerate parameters
// appearing or disappearing later - but the entire point of Forge is that every
// generated instrument has a different set of knobs.
//
// The resolution: declare a fixed pool of generic parameters up front, and
// re-point them at whichever instrument is loaded. The plugin's own GUI always
// shows correct names and ranges; the host's automation list shows whatever it
// cached, which some hosts refresh on request and some do not. Automation still
// works either way, because slot N is always slot N.
//
// Phase 3 export (SPEC 14.3) removes the limitation entirely by baking the
// names in at export time.
// ---------------------------------------------------------------------------
#pragma once

#include "core/dsp/GraphInstance.h"
#include "core/ir/Ir.h"

#include <juce_audio_processors/juce_audio_processors.h>

#include <atomic>

namespace forge {

/// A pool slot. Always normalised 0..1 to the host; the display value, name,
/// unit and taper are re-pointed when an instrument loads.
class ForgeParameter final : public juce::AudioParameterFloat {
public:
    ForgeParameter(const juce::String& paramId, const juce::String& fallbackName);

    /// Message thread only. Re-points this slot at an instrument parameter.
    void assign(const ir::ParamSpec& spec);
    /// Message thread only. Re-points this slot at a macro.
    void assignMacro(const ir::MacroSpec& spec);
    /// Message thread only. Returns the slot to its inert generic state.
    void unassign();

    bool isAssigned() const noexcept { return assigned_.load(std::memory_order_relaxed); }

    /// AudioParameterFloat keeps getValue()/getDefaultValue() private, so the
    /// pool exposes its own accessors. The slot's range is always 0..1, which
    /// makes get() and the host-normalised value the same number.
    float normalisedValue()   const noexcept { return get(); }
    float normalisedDefault() const noexcept { return defaultNorm_.load(std::memory_order_relaxed); }

    /// Value in the instrument's own units - Hz, dB, ms. Graph widgets work in
    /// engineering units, not in 0..1, so they need both directions.
    float engineeringValue() const noexcept;
    void  setEngineeringValue(float engineering);

    // --- juce::AudioProcessorParameter -------------------------------------
    juce::String getName(int maximumStringLength) const override;
    juce::String getLabel() const override;
    juce::String getText(float normalisedValue, int maximumStringLength) const override;
    float getValueForText(const juce::String& text) const override;

private:
    juce::String fallbackName_;

    // Display state. Written on the message thread when an instrument loads,
    // read by the host when it asks for names and text. VST3 hosts query these
    // from the message thread; the atomics cover the numeric formatting path
    // in case a host asks from elsewhere.
    juce::String       displayName_, unit_;
    std::atomic<bool>  assigned_{false};
    std::atomic<float> min_{0.0f}, max_{1.0f};
    std::atomic<int>   taper_{static_cast<int>(Taper::Linear)};
    std::atomic<bool>  isMacro_{false};
    std::atomic<float> defaultNorm_{0.0f};

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(ForgeParameter)
};

/// Owns the pool and the mapping between pool slots and the loaded instrument.
class ParameterPool {
public:
    // Sized for a full architecture rather than a single generated patch.
    // These are declared once at scan time and re-pointed per instrument, so
    // the cost of a large pool is a few hundred floats, not complexity.
    static constexpr int kNumMacros = 16;
    static constexpr int kNumParams = 224;

    /// Called once from the processor constructor, before the host scans.
    void createParameters(juce::AudioProcessor& processor);

    /// Message thread. Re-points every slot at the new instrument and notifies
    /// the host that parameter info changed.
    void bind(const ir::Instrument& instrument, juce::AudioProcessor& processor);

    /// Audio thread. Pushes current host values into the graph.
    void pushTo(GraphInstance& graph) const noexcept;

    /// Message thread. Reads the graph's current values back into the host
    /// parameters, so a freshly generated instrument shows its own defaults.
    void pullDefaultsFrom(const ir::Instrument& instrument);

    ForgeParameter* paramSlot(int index) const noexcept;
    ForgeParameter* macroSlot(int index) const noexcept;

    int assignedParamCount() const noexcept { return numAssignedParams_; }
    int assignedMacroCount() const noexcept { return numAssignedMacros_; }

    juce::AudioParameterFloat* masterVolume() const noexcept { return masterVolume_; }
    juce::AudioParameterFloat* bendRange()    const noexcept { return bendRange_; }

private:
    std::vector<ForgeParameter*> params_, macros_;
    juce::AudioParameterFloat*   masterVolume_ = nullptr;
    juce::AudioParameterFloat*   bendRange_    = nullptr;

    int numAssignedParams_ = 0;
    int numAssignedMacros_ = 0;
};

} // namespace forge
