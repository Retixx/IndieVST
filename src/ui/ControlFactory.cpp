#include "ui/ControlFactory.h"

#include <cmath>

#include "core/dsp/Registry.h"
#include "plugin/PluginProcessor.h"
#include "ui/Fonts.h"
#include "ui/ForgeLookAndFeel.h"

#include <map>

namespace forge {
namespace {

/// Raw setting values are terse because they are schema keys. "Lp" on screen
/// tells a producer nothing; "Low Pass" tells them everything. The mapping
/// lives here so the schema stays machine-friendly and the UI stays human.
juce::String humanise(const std::string& setting, const std::string& value) {
    static const std::map<std::string, std::map<std::string, juce::String>> table = {
        {"mode",  {{"lp", "Low Pass"}, {"hp", "High Pass"}, {"bp", "Band Pass"},
                   {"notch", "Notch"}, {"peak", "Peak"}}},
        {"slope", {{"12", "12 dB/oct"}, {"24", "24 dB/oct"}}},
        {"wave",  {{"saw", "Saw"}, {"square", "Square"}, {"pulse", "Pulse"},
                   {"triangle", "Triangle"}, {"sine", "Sine"}, {"tri", "Triangle"},
                   {"ramp", "Ramp"}, {"sh", "Sample & Hold"}, {"random", "Random"}}},
        {"color", {{"white", "White"}, {"pink", "Pink"}, {"brown", "Brown"}}},
        {"type",  {{"tanh", "Tanh"}, {"soft", "Soft Clip"}, {"fold", "Wavefolder"},
                   {"bitcrush", "Bitcrush"}, {"curve", "Custom Curve"}}},
        {"op",    {{"add", "Add"}, {"sub", "Subtract"}, {"mul", "Multiply"},
                   {"min", "Minimum"}, {"max", "Maximum"}, {"abs", "Absolute"}}},
        {"pattern", {{"random", "Random"}, {"up", "Up"}, {"down", "Down"},
                     {"alternate", "Alternate"}}},
        {"curve", {{"exp", "Exponential"}, {"linear", "Linear"}}},
    };

    auto s = table.find(setting);
    if (s != table.end()) {
        auto v = s->second.find(value);
        if (v != s->second.end()) return v->second;
    }
    juce::String fallback(value);
    return fallback.substring(0, 1).toUpperCase() + fallback.substring(1);
}

/// One-line explanation shown on hover. Producers should never have to guess
/// what a control does.
juce::String explain(const std::string& setting) {
    static const std::map<std::string, juce::String> help = {
        {"mode",  "Filter response. Low Pass removes highs, High Pass removes lows, "
                  "Band Pass keeps a band, Notch removes one, Peak boosts one."},
        {"slope", "How steeply the filter cuts. 24 dB/oct is twice as sharp as 12."},
        {"wave",  "Oscillator waveform. Saw is bright and buzzy, Square hollow, "
                  "Triangle soft, Sine pure."},
        {"color", "Noise spectrum, not a UI colour. White is flat and hissy, Pink is "
                  "darker and more natural, Brown is deep and rumbling."},
        {"type",  "Distortion character."},
        {"unison","Detuned copies stacked per note. More voices = wider and thicker, "
                  "and proportionally more CPU."},
        {"octave","Transposes this oscillator in octaves."},
        {"division","Note division when tempo sync is on."},
        {"sync",  "Lock the rate to host tempo instead of Hz."},
        {"steps", "How many steps the sequence cycles through."},
        {"pattern","Shape of the generated step sequence."},
        {"curve", "Envelope segment shape. Exponential is how analog envelopes behave."},
        {"pingpong","Bounce the repeats between left and right."},
    };
    auto it = help.find(setting);
    return it != help.end() ? it->second : juce::String();
}

} // namespace

// --- ParamControl ----------------------------------------------------------

ParamControl::ParamControl(ForgeParameter& parameter, const ir::ParamSpec& spec, bool isMacro)
    : parameter_(parameter),
      id_(spec.id),
      label_(spec.label.empty() ? spec.id : spec.label),
      unit_(spec.unit),
      macro_(isMacro) {

    // Every control explains itself on hover: what it is, its range, and which
    // module parameter it actually drives.
    juce::String tip = label_;
    tip << "\nRange " << juce::String(spec.min, spec.max >= 100.0f ? 0 : 2)
        << " to "     << juce::String(spec.max, spec.max >= 100.0f ? 0 : 2);
    if (!spec.unit.empty()) tip << " " << juce::String(spec.unit);
    if (spec.taper == Taper::Log)
        tip << "  (logarithmic - matches how pitch and time are heard)";
    if (!spec.bind.empty())
        tip << "\n\n" << juce::String(spec.bind.front().node) << "."
            << juce::String(spec.bind.front().param);
    tip << "\nDouble-click to reset  -  hold Shift to fine-tune";
    setTooltip(tip);

    kind_ = spec.control;

    if (spec.control == "switch" || spec.control == "toggle") {
        toggle_ = std::make_unique<juce::ToggleButton>();
        toggle_->setButtonText({});
        toggle_->setWantsKeyboardFocus(false);
        addAndMakeVisible(*toggle_);
        buttonAttachment_ = std::make_unique<juce::ButtonParameterAttachment>(parameter, *toggle_, nullptr);
    } else {
        slider_ = std::make_unique<juce::Slider>();
        // A pitch control is vertical because pitch is up and down; a blend is
        // horizontal because it is a travel between two things. Matching the
        // shape to the meaning is most of what makes a panel feel designed
        // rather than generated.
        slider_->setSliderStyle(
            (spec.control == "vslider" || spec.control == "slider")
                ? juce::Slider::LinearVertical
            : (spec.control == "hslider") ? juce::Slider::LinearHorizontal
                                          : juce::Slider::RotaryVerticalDrag);
        slider_->setTextBoxStyle(juce::Slider::NoTextBox, false, 0, 0);
        slider_->setRange(0.0, 1.0, 0.0);
        slider_->setDoubleClickReturnValue(true, parameter.normalisedDefault());
        slider_->setRotaryParameters(juce::MathConstants<float>::pi * 1.25f,
                                     juce::MathConstants<float>::pi * 2.75f, true);
        // Longer drag distance for the full range, so landing on a specific
        // value is a deliberate act rather than a lucky one. Shift gives a
        // further 10x for fine work.
        slider_->setMouseDragSensitivity(420);
        slider_->onValueChange = [this] { refreshReadout(); };
        // Critical for playability: without this, dragging a knob steals
        // keyboard focus and computer-key playing stops working.
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

float ParamControl::computeWidthUnits() const {
    // A horizontal fader is useless in one cell width - it has no travel.
    if (kind_ == "hslider") return 2.2f;
    // The name and the readout both have to fit; "1200.00 cents" under a knob
    // labelled "Detune" is wider than the knob itself.
    const auto font = fonts::get(11.0f, fonts::Weight::Medium);
    const float widest = juce::jmax(juce::GlyphArrangement::getStringWidth(font, label_),
                                    juce::GlyphArrangement::getStringWidth(font, readout_));
    return juce::jlimit(1.0f, 1.9f, (widest + 10.0f) / static_cast<float>(cell::width));
}

void ParamControl::resized() {
    if (slider_ != nullptr && (kind_ == "vslider" || kind_ == "hslider" || kind_ == "slider")) {
        auto bounds = getLocalBounds();
        bounds.removeFromTop(nameHeight());
        bounds.removeFromBottom(valueHeight());
        // Faders want the length; a vertical one is narrow, a horizontal one
        // shallow. Reducing the other axis is what stops them reading as a
        // stretched knob.
        if (kind_ == "hslider") slider_->setBounds(bounds.reduced(4, bounds.getHeight() / 4));
        else                    slider_->setBounds(bounds.withSizeKeepingCentre(
                                    juce::jmax(14, getWidth() / 3), bounds.getHeight()));
        return;
    }

    auto bounds = getLocalBounds();
    bounds.removeFromTop(nameHeight());
    bounds.removeFromBottom(valueHeight());

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

// --- SwitchControl ---------------------------------------------------------

SwitchControl::SwitchControl(ForgeAudioProcessor& processor, const ir::SwitchSpec& spec,
                             const ir::NodeSpec& node, const SettingDesc& desc)
    : processor_(processor),
      label_(spec.label.empty() ? spec.setting : spec.label),
      nodeId_(spec.node),
      settingId_(spec.setting) {

    juce::String tip = label_;
    const auto explanation = explain(spec.setting);
    if (explanation.isNotEmpty())     tip << "\n" << explanation;
    else if (!desc.help.empty())      tip << "\n" << juce::String(desc.help);
    tip << "\n\n" << juce::String(node.type) << "." << juce::String(spec.setting);
    setTooltip(tip);

    // Build the option list straight from the module manifest, so a module
    // gaining a new wave shape shows up here with no UI change.
    int itemId = 1;
    switch (desc.type) {
        case SettingDesc::Type::Enum:
            for (const auto& option : desc.options) {
                values_.push_back(option);
                combo_.addItem(humanise(spec.setting, option), itemId++);
            }
            break;
        case SettingDesc::Type::Bool:
            values_ = {false, true};
            combo_.addItem("Off", itemId++);
            combo_.addItem("On",  itemId++);
            break;
        case SettingDesc::Type::Float: {
            // Float settings had no branch at all, so they fell through to the
            // integer case and were enumerated as whole numbers. For an FM
            // ratio that is not a rounding detail - the NON-INTEGER ratios are
            // the entire reason FM can make a bell, and they were unreachable.
            static const double kRatios[] = {
                0.25, 0.5, 0.75, 1.0, 1.41, 1.5, 2.0, 2.41, 3.0, 3.5,
                4.0, 4.75, 5.0, 6.0, 7.0, 8.0, 9.5, 11.0, 14.0, 16.0
            };
            const bool isRatio = spec.setting.find("ratio") != std::string::npos;
            if (isRatio) {
                for (double v : kRatios) {
                    if (v < desc.min || v > desc.max) continue;
                    values_.push_back(v);
                    // Mark the inharmonic ones, because that is the musically
                    // meaningful distinction and it is invisible in the number.
                    const bool harmonic = std::abs(v - std::round(v)) < 0.01;
                    combo_.addItem(juce::String(v, harmonic ? 0 : 2)
                                       + (harmonic ? "" : "  (metallic)"), itemId++);
                }
            } else {
                for (int i = 0; i < 13; ++i) {
                    const double v = desc.min + (desc.max - desc.min) * i / 12.0;
                    values_.push_back(v);
                    combo_.addItem(juce::String(v, 2), itemId++);
                }
            }
            break;
        }
        case SettingDesc::Type::Int:
        default: {
            const int lo = static_cast<int>(desc.min), hi = static_cast<int>(desc.max);
            for (int v = lo; v <= hi && itemId <= 32; ++v) {
                values_.push_back(v);
                combo_.addItem(juce::String(v), itemId++);
            }
            break;
        }
    }

    // Select whatever the instrument currently has.
    nlohmann::json current = desc.def;
    if (node.settings.is_object()) {
        auto it = node.settings.find(spec.setting);
        if (it != node.settings.end()) current = *it;
    }
    for (size_t i = 0; i < values_.size(); ++i) {
        if (values_[i] == current) { combo_.setSelectedId(static_cast<int>(i) + 1,
                                                          juce::dontSendNotification); break; }
    }
    if (combo_.getSelectedId() == 0 && !values_.empty())
        combo_.setSelectedId(1, juce::dontSendNotification);

    combo_.setWantsKeyboardFocus(false);
    combo_.onChange = [this] {
        const int index = combo_.getSelectedId() - 1;
        if (index < 0 || index >= static_cast<int>(values_.size())) return;
        processor_.setNodeSetting(nodeId_, settingId_, values_[static_cast<size_t>(index)]);
    };
    addAndMakeVisible(combo_);
}

float SwitchControl::computeWidthUnits() const {
    // Measured from the widest option the box can actually show, plus the
    // arrow and the label above it - never guessed from the range.
    const auto font = fonts::get(12.0f, fonts::Weight::Regular);
    float widest = juce::GlyphArrangement::getStringWidth(font, label_);
    for (int i = 0; i < combo_.getNumItems(); ++i)
        widest = juce::jmax(widest, juce::GlyphArrangement::getStringWidth(
                                        font, combo_.getItemText(i)));
    const float needed = widest + 34.0f;   // arrow, inner padding, cell margin
    return juce::jlimit(1.0f, 2.4f, needed / static_cast<float>(cell::width));
}

void SwitchControl::resized() {
    auto bounds = getLocalBounds();
    bounds.removeFromTop(juce::roundToInt(getHeight() * cell::nameFrac));
    bounds.removeFromBottom(juce::roundToInt(getHeight() * cell::valueFrac));
    combo_.setBounds(bounds.withSizeKeepingCentre(getWidth() - 6,
                                                  juce::jlimit(18, 26, bounds.getHeight())));
}

void SwitchControl::paint(juce::Graphics& g) {
    const float scale = juce::jlimit(1.0f, cell::maxScale,
                                     static_cast<float>(getHeight()) / cell::height);
    g.setFont(fonts::get(juce::jlimit(9.0f, 14.0f, 10.0f * scale), fonts::Weight::Medium));
    g.setColour(ForgeLookAndFeel::textSecondary());
    g.drawFittedText(label_,
                     getLocalBounds().removeFromTop(juce::roundToInt(getHeight() * cell::nameFrac)),
                     juce::Justification::centred, 1);
}

// --- factories -------------------------------------------------------------

std::unique_ptr<ParamControl> makeControl(ForgeParameter* parameter,
                                          const ir::ParamSpec& spec, bool isMacro) {
    if (parameter == nullptr) return nullptr;
    return std::make_unique<ParamControl>(*parameter, spec, isMacro);
}

std::unique_ptr<SwitchControl> makeSwitch(ForgeAudioProcessor& processor,
                                          const ir::Instrument& instrument,
                                          const ir::SwitchSpec& spec) {
    const ir::NodeSpec* node = instrument.findNode(spec.node);
    if (node == nullptr) return nullptr;
    const ModuleManifest* man = Registry::instance().find(node->type);
    if (man == nullptr) return nullptr;
    const SettingDesc* desc = man->findSetting(spec.setting);
    if (desc == nullptr) return nullptr;
    if (desc->type != SettingDesc::Type::Enum && desc->type != SettingDesc::Type::Int
        && desc->type != SettingDesc::Type::Bool)
        return nullptr;
    return std::make_unique<SwitchControl>(processor, spec, *node, *desc);
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
