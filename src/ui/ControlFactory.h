// ---------------------------------------------------------------------------
// Turning an IR parameter description into a real control.
//
// Entirely generic: it knows the schema, not any particular instrument. That is
// what lets a graph the model invented five seconds ago arrive with a usable,
// correctly-labelled panel.
//
// Every control occupies exactly one fixed cell. Uniform cells are the whole
// reason the panel reads as a grid instead of as scattered knobs.
// ---------------------------------------------------------------------------
#pragma once

#include "core/ir/Ir.h"
#include "plugin/ParameterPool.h"

#include <juce_gui_basics/juce_gui_basics.h>

namespace forge {

/// Base cell size. The rack scales these up to fill the window, so these are
/// proportions rather than fixed pixels - a control derives its knob diameter
/// and font sizes from whatever bounds it is given.
namespace cell {
inline constexpr int   width  = 58;
inline constexpr int   height = 62;
inline constexpr float nameFrac  = 0.19f;   ///< share of cell height for the name
inline constexpr float valueFrac = 0.17f;   ///< share of cell height for the readout
inline constexpr float maxScale  = 2.1f;    ///< beyond this, knobs look comical
} // namespace cell

class ParamControl final : public juce::Component,
                           public juce::SettableTooltipClient {
public:
    ParamControl(ForgeParameter& parameter, const ir::ParamSpec& spec, bool isMacro);
    ~ParamControl() override;

    void resized() override;
    void paint(juce::Graphics&) override;

    const juce::String& parameterId() const noexcept { return id_; }

private:
    void refreshReadout();
    int  nameHeight() const;
    int  valueHeight() const;

    ForgeParameter& parameter_;
    juce::String    id_, label_, unit_, readout_;
    bool            macro_ = false;

    std::unique_ptr<juce::Slider>       slider_;
    std::unique_ptr<juce::ToggleButton> toggle_;
    std::unique_ptr<juce::ComboBox>     combo_;

    std::unique_ptr<juce::SliderParameterAttachment>   sliderAttachment_;
    std::unique_ptr<juce::ButtonParameterAttachment>   buttonAttachment_;
    std::unique_ptr<juce::ComboBoxParameterAttachment> comboAttachment_;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(ParamControl)
};

std::unique_ptr<ParamControl> makeControl(ForgeParameter*, const ir::ParamSpec&, bool isMacro);

/// A macro is presented as a 0..100% knob, so it needs a synthetic ParamSpec.
ir::ParamSpec specForMacro(const ir::MacroSpec&);

} // namespace forge
