#include "ui/ControlFactory.h"

#include "ui/Fonts.h"
#include "ui/ForgeLookAndFeel.h"

namespace forge {

ParamControl::ParamControl(ForgeParameter& parameter, const ir::ParamSpec& spec, bool isMacro)
    : parameter_(parameter),
      id_(spec.id),
      label_(spec.label.empty() ? spec.id : spec.label),
      unit_(spec.unit),
      macro_(isMacro) {

    // The full name and range go in the tooltip; the cell itself stays terse.
    juce::String tip = label_;
    if (!spec.unit.empty())
        tip << "  (" << juce::String(spec.min, spec.max >= 100.0f ? 0 : 2) << " - "
            << juce::String(spec.max, spec.max >= 100.0f ? 0 : 2) << " " << juce::String(spec.unit) << ")";
    setTooltip(tip);

    if (spec.control == "switch") {
        toggle_ = std::make_unique<juce::ToggleButton>();
        toggle_->setButtonText({});
        addAndMakeVisible(*toggle_);
        buttonAttachment_ = std::make_unique<juce::ButtonParameterAttachment>(parameter, *toggle_, nullptr);
    } else if (spec.control == "combo") {
        combo_ = std::make_unique<juce::ComboBox>();
        for (int i = 0; i < 5; ++i)
            combo_->addItem(parameter.getText(static_cast<float>(i) / 4.0f, 10), i + 1);
        addAndMakeVisible(*combo_);
        comboAttachment_ = std::make_unique<juce::ComboBoxParameterAttachment>(parameter, *combo_, nullptr);
    } else {
        slider_ = std::make_unique<juce::Slider>();
        slider_->setSliderStyle(spec.control == "slider" ? juce::Slider::LinearVertical
                                                         : juce::Slider::RotaryVerticalDrag);
        slider_->setTextBoxStyle(juce::Slider::NoTextBox, false, 0, 0);
        slider_->setRange(0.0, 1.0, 0.0);
        slider_->setDoubleClickReturnValue(true, parameter.normalisedDefault());
        slider_->setRotaryParameters(juce::MathConstants<float>::pi * 1.25f,
                                     juce::MathConstants<float>::pi * 2.75f, true);
        slider_->onValueChange = [this] { refreshReadout(); };
        addAndMakeVisible(*slider_);
        sliderAttachment_ = std::make_unique<juce::SliderParameterAttachment>(parameter, *slider_, nullptr);
    }

    refreshReadout();
}

ParamControl::~ParamControl() = default;

void ParamControl::refreshReadout() {
    readout_ = parameter_.getText(parameter_.normalisedValue(), 8);
    if (unit_.isNotEmpty() && unit_ != "%") readout_ << " " << unit_;
    else if (unit_ == "%")                  readout_ << "%";
    repaint();
}

void ParamControl::resized() {
    auto bounds = getLocalBounds();
    bounds.removeFromTop(cell::nameH);
    bounds.removeFromBottom(cell::valueH);

    if (slider_ != nullptr) {
        if (slider_->getSliderStyle() == juce::Slider::LinearVertical)
            slider_->setBounds(bounds.withSizeKeepingCentre(18, bounds.getHeight()));
        else
            slider_->setBounds(bounds.withSizeKeepingCentre(cell::knob, cell::knob));
    }
    if (toggle_ != nullptr) toggle_->setBounds(bounds.withSizeKeepingCentre(22, 22));
    if (combo_  != nullptr) combo_->setBounds(bounds.withSizeKeepingCentre(cell::width - 8, 20));
}

void ParamControl::paint(juce::Graphics& g) {
    auto bounds = getLocalBounds();

    g.setFont(fonts::controlName());
    g.setColour(macro_ ? ForgeLookAndFeel::textPrimary() : ForgeLookAndFeel::textSecondary());
    g.drawFittedText(label_, bounds.removeFromTop(cell::nameH), juce::Justification::centred, 1);

    g.setFont(fonts::controlValue());
    g.setColour(ForgeLookAndFeel::textDim());
    g.drawFittedText(readout_, bounds.removeFromBottom(cell::valueH),
                     juce::Justification::centred, 1);
}

std::unique_ptr<ParamControl> makeControl(ForgeParameter* parameter,
                                          const ir::ParamSpec& spec, bool isMacro) {
    if (parameter == nullptr) return nullptr;
    return std::make_unique<ParamControl>(*parameter, spec, isMacro);
}

ir::ParamSpec specForMacro(const ir::MacroSpec& macro) {
    ir::ParamSpec spec;
    spec.id      = macro.id;
    spec.label   = macro.label.empty() ? macro.id : macro.label;
    spec.unit    = "%";
    spec.min     = 0.0f;
    spec.max     = 100.0f;
    spec.def     = clamp01(macro.def) * 100.0f;
    spec.taper   = Taper::Linear;
    spec.control = "knob";
    spec.group   = "Macros";
    return spec;
}

} // namespace forge
