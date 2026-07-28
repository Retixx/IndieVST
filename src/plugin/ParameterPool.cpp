#include "plugin/ParameterPool.h"

namespace forge {

// --- ForgeParameter --------------------------------------------------------

ForgeParameter::ForgeParameter(const juce::String& paramId, const juce::String& fallbackName)
    : juce::AudioParameterFloat(juce::ParameterID{paramId, 1}, fallbackName,
                                juce::NormalisableRange<float>(0.0f, 1.0f), 0.0f),
      fallbackName_(fallbackName) {}

void ForgeParameter::assign(const ir::ParamSpec& spec) {
    displayName_ = spec.label.empty() ? juce::String(spec.id) : juce::String(spec.label);
    unit_        = juce::String(spec.unit);
    min_.store(spec.min, std::memory_order_relaxed);
    max_.store(spec.max, std::memory_order_relaxed);
    taper_.store(static_cast<int>(spec.taper), std::memory_order_relaxed);
    isMacro_.store(false, std::memory_order_relaxed);
    defaultNorm_.store(clamp01(spec.defaultNormalised()), std::memory_order_relaxed);
    assigned_.store(true, std::memory_order_release);
}

void ForgeParameter::assignMacro(const ir::MacroSpec& spec) {
    displayName_ = spec.label.empty() ? juce::String(spec.id) : juce::String(spec.label);
    unit_        = "%";
    min_.store(0.0f, std::memory_order_relaxed);
    max_.store(100.0f, std::memory_order_relaxed);
    taper_.store(static_cast<int>(Taper::Linear), std::memory_order_relaxed);
    isMacro_.store(true, std::memory_order_relaxed);
    defaultNorm_.store(clamp01(spec.def), std::memory_order_relaxed);
    assigned_.store(true, std::memory_order_release);
}

void ForgeParameter::unassign() {
    displayName_ = {};
    unit_        = {};
    assigned_.store(false, std::memory_order_release);
}

float ForgeParameter::engineeringValue() const noexcept {
    return applyTaper(normalisedValue(),
                      min_.load(std::memory_order_relaxed),
                      max_.load(std::memory_order_relaxed),
                      static_cast<Taper>(taper_.load(std::memory_order_relaxed)));
}

void ForgeParameter::setEngineeringValue(float engineering) {
    const float norm = removeTaper(engineering,
                                   min_.load(std::memory_order_relaxed),
                                   max_.load(std::memory_order_relaxed),
                                   static_cast<Taper>(taper_.load(std::memory_order_relaxed)));
    beginChangeGesture();
    setValueNotifyingHost(juce::jlimit(0.0f, 1.0f, norm));
    endChangeGesture();
}

juce::String ForgeParameter::getName(int maximumStringLength) const {
    const juce::String& n = displayName_.isNotEmpty() ? displayName_ : fallbackName_;
    return n.substring(0, maximumStringLength);
}

juce::String ForgeParameter::getLabel() const { return unit_; }

juce::String ForgeParameter::getText(float normalisedValue, int maximumStringLength) const {
    if (!assigned_.load(std::memory_order_acquire))
        return juce::String(normalisedValue, 2).substring(0, maximumStringLength);

    const float mn = min_.load(std::memory_order_relaxed);
    const float mx = max_.load(std::memory_order_relaxed);
    const auto taper = static_cast<Taper>(taper_.load(std::memory_order_relaxed));
    // NB: named 'engineering' rather than 'value' because AudioParameterFloat
    // already has a member called value, and shadowing it here would be an easy
    // way to read the wrong number later.
    const float engineering = applyTaper(juce::jlimit(0.0f, 1.0f, normalisedValue), mn, mx, taper);

    // Digits come from the VALUE, not the range. Deriving them from the range
    // is what produced "901.246 ms" and "4879.46 Hz" - readings with more
    // precision than anyone can dial in, on a control where the last two digits
    // are noise. Nobody sets a release time to a thousandth of a millisecond.
    const float magnitude = std::abs(engineering);
    int digits = 2;
    if (unit_ == "%")            digits = 0;
    else if (magnitude >= 100.0f) digits = 0;
    else if (magnitude >= 10.0f)  digits = 1;

    return juce::String(engineering, digits).substring(0, maximumStringLength);
}

float ForgeParameter::getValueForText(const juce::String& text) const {
    const float typed = text.getFloatValue();
    if (!assigned_.load(std::memory_order_acquire)) return juce::jlimit(0.0f, 1.0f, typed);
    return removeTaper(typed, min_.load(std::memory_order_relaxed),
                       max_.load(std::memory_order_relaxed),
                       static_cast<Taper>(taper_.load(std::memory_order_relaxed)));
}

// --- ParameterPool ---------------------------------------------------------

void ParameterPool::createParameters(juce::AudioProcessor& processor) {
    // Order matters: the host records these indices. Never reorder or remove
    // them, or every saved automation lane in every existing project breaks.
    for (int i = 0; i < kNumMacros; ++i) {
        auto* p = new ForgeParameter(juce::String("MACRO") + juce::String(i + 1),
                                     juce::String("Macro ") + juce::String(i + 1));
        macros_.push_back(p);
        processor.addParameter(p);
    }
    for (int i = 0; i < kNumParams; ++i) {
        auto* p = new ForgeParameter(juce::String("P") + juce::String(i + 1).paddedLeft('0', 2),
                                     juce::String("Param ") + juce::String(i + 1));
        params_.push_back(p);
        processor.addParameter(p);
    }

    masterVolume_ = new juce::AudioParameterFloat(
        juce::ParameterID{"MASTER", 1}, "Master",
        juce::NormalisableRange<float>(0.0f, 1.0f), 0.85f);
    processor.addParameter(masterVolume_);

    bendRange_ = new juce::AudioParameterFloat(
        juce::ParameterID{"BEND", 1}, "Bend Range",
        juce::NormalisableRange<float>(0.0f, 24.0f, 1.0f), 2.0f);
    processor.addParameter(bendRange_);
}

void ParameterPool::bind(const ir::Instrument& instrument, juce::AudioProcessor& processor) {
    numAssignedParams_ = juce::jmin(static_cast<int>(instrument.params.size()), kNumParams);
    numAssignedMacros_ = juce::jmin(static_cast<int>(instrument.macros.size()), kNumMacros);

    for (int i = 0; i < kNumParams; ++i) {
        if (i < numAssignedParams_) params_[static_cast<size_t>(i)]->assign(instrument.params[static_cast<size_t>(i)]);
        else                        params_[static_cast<size_t>(i)]->unassign();
    }
    for (int i = 0; i < kNumMacros; ++i) {
        if (i < numAssignedMacros_) macros_[static_cast<size_t>(i)]->assignMacro(instrument.macros[static_cast<size_t>(i)]);
        else                        macros_[static_cast<size_t>(i)]->unassign();
    }

    pullDefaultsFrom(instrument);

    // Ask the host to re-read names, units and text. Reaper and FL honour this
    // promptly; Ableton caches and may keep showing the generic slot name. The
    // plugin's own editor is always correct either way, which is what matters
    // on stage.
    processor.updateHostDisplay(
        juce::AudioProcessorListener::ChangeDetails{}.withParameterInfoChanged(true));
}

void ParameterPool::pullDefaultsFrom(const ir::Instrument& instrument) {
    // No begin/endChangeGesture here on purpose: this is a programmatic load,
    // not the user moving a knob, and wrapping it in a gesture makes some hosts
    // record an automation write for every parameter on every instrument load.
    for (int i = 0; i < numAssignedParams_; ++i)
        params_[static_cast<size_t>(i)]->setValueNotifyingHost(
            juce::jlimit(0.0f, 1.0f, instrument.params[static_cast<size_t>(i)].defaultNormalised()));

    for (int i = 0; i < numAssignedMacros_; ++i)
        macros_[static_cast<size_t>(i)]->setValueNotifyingHost(
            clamp01(instrument.macros[static_cast<size_t>(i)].def));
}

void ParameterPool::pushTo(GraphInstance& graph) const noexcept {
    for (int i = 0; i < numAssignedParams_; ++i)
        graph.setExposedParam(i, params_[static_cast<size_t>(i)]->normalisedValue());
    for (int i = 0; i < numAssignedMacros_; ++i)
        graph.setMacro(i, macros_[static_cast<size_t>(i)]->normalisedValue());
}

ForgeParameter* ParameterPool::paramSlot(int index) const noexcept {
    return (index >= 0 && index < static_cast<int>(params_.size()))
             ? params_[static_cast<size_t>(index)] : nullptr;
}

ForgeParameter* ParameterPool::macroSlot(int index) const noexcept {
    return (index >= 0 && index < static_cast<int>(macros_.size()))
             ? macros_[static_cast<size_t>(index)] : nullptr;
}

} // namespace forge
