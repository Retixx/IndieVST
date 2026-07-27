#pragma once

#include "plugin/PluginProcessor.h"
#include "ui/ControlFactory.h"
#include "ui/GraphView.h"

#include <juce_gui_basics/juce_gui_basics.h>

namespace forge {

/// The playable panel: a horizontal rack of fixed-width module panels, laid out
/// on a strict cell grid so nothing floats. Sections, controls, labels, units
/// and accent colour all come from the generated instrument.
class InstrumentView final : public juce::Component {
public:
    enum class Mode { Controls, Graph };

    explicit InstrumentView(ForgeAudioProcessor&);
    ~InstrumentView() override;

    void rebuild();
    void setMode(Mode);
    Mode mode() const noexcept { return mode_; }

    void setAccent(juce::Colour);

    void resized() override;
    void paint(juce::Graphics&) override;

private:
    struct Panel {
        juce::String               title;
        std::vector<ParamControl*> controls;
        juce::Rectangle<int>       bounds;   ///< in canvas coordinates
        bool                       isMacroPanel = false;
    };

    class Canvas final : public juce::Component {
    public:
        explicit Canvas(InstrumentView& o) : owner(o) {}
        void paint(juce::Graphics& g) override { owner.paintPanels(g); }
        InstrumentView& owner;
    };

    void paintPanels(juce::Graphics&);
    void layoutPanels();

    ForgeAudioProcessor&           processor_;
    Mode                           mode_ = Mode::Controls;
    juce::Colour                   accent_{0xffe4572e};

    juce::Viewport                 viewport_;
    Canvas                         canvas_{*this};
    juce::OwnedArray<ParamControl> controls_;
    std::vector<Panel>             panels_;

    GraphView                      graphView_;
    juce::Label                    emptyLabel_;

    // Scaled by layoutPanels() so the painted chrome matches the laid-out cells.
    int panelTitleH_ = 17;
    int panelPadX_   = 9;

    friend class Canvas;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(InstrumentView)
};

} // namespace forge
