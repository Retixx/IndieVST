// ---------------------------------------------------------------------------
// Turning IR control descriptions into real controls.
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

class ForgeAudioProcessor;

/// Base cell size. The rack scales these up to fill the window, so these are
/// proportions rather than fixed pixels - a control derives its knob diameter
/// and font sizes from whatever bounds it is given.
namespace cell {
inline constexpr int   width  = 58;
inline constexpr int   height = 62;
inline constexpr float nameFrac  = 0.19f;
inline constexpr float valueFrac = 0.17f;
inline constexpr float maxScale  = 2.1f;
} // namespace cell

/// Common base so a panel can hold knobs and selectors in the same grid.
class CellControl : public juce::Component,
                    public juce::SettableTooltipClient {
public:
    ~CellControl() override = default;

    /// How many cell widths this control needs for its text to be readable.
    ///
    /// A uniform grid is what makes a panel read as a panel, but forcing a
    /// selector reading "12 dB/oct" into the same box as a knob labelled "Reso"
    /// is how you end up shipping "Sq..." and "Ex...". The grid stays; the
    /// column that holds the wide control simply gets wider.
    ///
    /// CACHED, and that is not an optimisation detail - it is the difference
    /// between a responsive window and an unusable one. The layout search asks
    /// every control on the page for this hundreds of times per resize, and
    /// measuring a string means laying out its glyphs. Computing it on every
    /// call made dragging the window edge crawl.
    float widthUnits() const {
        if (cachedWidthUnits_ < 0.0f) cachedWidthUnits_ = computeWidthUnits();
        return cachedWidthUnits_;
    }

protected:
    virtual float computeWidthUnits() const { return 1.0f; }
    /// Call if the label or readout changes enough to alter the width.
    void invalidateWidthUnits() { cachedWidthUnits_ = -1.0f; }

private:
    mutable float cachedWidthUnits_ = -1.0f;
};

/// A knob, slider or toggle bound to one host parameter slot.
class ParamControl final : public CellControl {
public:
    ParamControl(ForgeParameter& parameter, const ir::ParamSpec& spec, bool isMacro);
    ~ParamControl() override;

    void resized() override;
    void paint(juce::Graphics&) override;

protected:
    float computeWidthUnits() const override;

private:
    void refreshReadout();
    int  nameHeight() const;
    int  valueHeight() const;

    ForgeParameter& parameter_;
    juce::String    id_, label_, unit_, readout_;
    std::string     kind_ = "knob";   ///< knob | vslider | hslider | toggle
    bool            macro_ = false;

    std::unique_ptr<juce::Slider>       slider_;
    std::unique_ptr<juce::ToggleButton> toggle_;

    std::unique_ptr<juce::SliderParameterAttachment> sliderAttachment_;
    std::unique_ptr<juce::ButtonParameterAttachment> buttonAttachment_;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(ParamControl)
};

/// A selector bound to a construction-time module setting. Changing it rebuilds
/// the graph, which the publisher covers with a 20 ms crossfade.
class SwitchControl final : public CellControl {
public:
    SwitchControl(ForgeAudioProcessor&, const ir::SwitchSpec&, const ir::NodeSpec&,
                  const SettingDesc&);

    void resized() override;
    void paint(juce::Graphics&) override;

protected:
    float computeWidthUnits() const override;

private:
    ForgeAudioProcessor& processor_;
    juce::String   label_, nodeId_, settingId_;
    juce::ComboBox combo_;
    std::vector<nlohmann::json> values_;   ///< parallel to combo item ids

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(SwitchControl)
};

std::unique_ptr<ParamControl> makeControl(ForgeParameter*, const ir::ParamSpec&, bool isMacro);

/// Returns nullptr if the switch does not resolve to a selectable setting.
std::unique_ptr<SwitchControl> makeSwitch(ForgeAudioProcessor&, const ir::Instrument&,
                                          const ir::SwitchSpec&);

/// A macro is presented as a 0..100% knob, so it needs a synthetic ParamSpec.
ir::ParamSpec specForMacro(const ir::MacroSpec&);

} // namespace forge
