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
        // Critical for playability: without this, dragging a knob steals
        // keyboard focus and the computer-key playing stops working until you
        // click the keyboard again.
        slider_->setWantsKeyboardFocus(false);
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

int ParamControl::nameHeight()  const { return juce::roundToInt(getHeight() * cell::nameFrac); }
int ParamControl::valueHeight() const { return juce::roundToInt(getHeight() * cell::valueFrac); }

void ParamControl::resized() {
    auto bounds = getLocalBounds();
    bounds.removeFromTop(nameHeight());
    bounds.removeFromBottom(valueHeight());

    // Everything sizes off the cell it was given, so the rack can scale the
    // whole panel up to fill the window without anything drifting off-grid.
    if (slider_ != nullptr) {
        if (slider_->getSliderStyle() == juce::Slider::LinearVertical)
            slider_->setBounds(bounds.withSizeKeepingCentre(
                juce::jmax(12, getWidth() / 3), bounds.getHeight()));
        else {
            const int d = juce::jmin(bounds.getHeight(), getWidth() - 8);
            slider_->setBounds(bounds.withSizeKeepingCentre(d, d));
        }
    }
    if (toggle_ != nullptr) {
        const int d = juce::jmin(bounds.getHeight(), getWidth() / 2);
        toggle_->setBounds(bounds.withSizeKeepingCentre(d, d));
    }
    if (combo_ != nullptr)
        combo_->setBounds(bounds.withSizeKeepingCentre(getWidth() - 8,
                                                       juce::jmin(bounds.getHeight(), 22)));
}

void ParamControl::paint(juce::Graphics& g) {
    auto bounds = getLocalBounds();
    const float scale = juce::jlimit(1.0f, cell::maxScale,
                                     static_cast<float>(getHeight()) / cell::height);

    g.setFont(fonts::get(juce::jlimit(9.0f, 14.0f, 10.0f * scale), fonts::Weight::Medium));
    g.setColour(macro_ ? ForgeLookAndFeel::textPrimary() : ForgeLookAndFeel::textSecondary());
    g.drawFittedText(label_, bounds.removeFromTop(nameHeight()), juce::Justification::centred, 1);

    g.setFont(fonts::get(juce::jlimit(8.5f, 13.0f, 9.5f * scale)));
    g.setColour(ForgeLookAndFeel::textDim());
    g.drawFittedText(readout_, bounds.removeFromBottom(valueHeight()),
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
