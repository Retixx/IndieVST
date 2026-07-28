#pragma once

#include "plugin/PluginProcessor.h"
#include "ui/ControlFactory.h"
#include "ui/GraphView.h"
#include "ui/Visualisers.h"

#include <juce_gui_basics/juce_gui_basics.h>

#include <set>
#include <vector>

namespace forge {

/// The playable rack.
///
/// Tabbed rather than a single wall of knobs: a full architecture carries far
/// more controls than fit on one screen at a readable size, and every hardware
/// synth and soft synth worth using pages them. Each tab carries its own
/// graphs - envelopes you can drag, LFO shapes, the wavetable - because those
/// are shapes, and reading a shape as four numbers is work the eye should not
/// have to do.
class InstrumentView final : public juce::Component {
public:
    enum class Mode { Controls, Graph };

    explicit InstrumentView(ForgeAudioProcessor&);
    ~InstrumentView() override;

    void rebuild();
    void setMode(Mode);

    /// Height this tab needs to show every control at a comfortable size.
    /// The editor resizes the window to match, so a 36-control page and a
    /// 67-control page are not forced into the same box.
    int  preferredContentHeight(int availableWidth) const;
    /// Fired when the active tab changes, so the window can resize to it.
    std::function<void()> onLayoutChanged;
    Mode mode() const noexcept { return mode_; }
    void setAccent(juce::Colour);

    void resized() override;
    void paint(juce::Graphics&) override;

private:
    /// A labelled cluster inside a tab: "TAPE", "FILTER 1", "ENV 2".
    ///
    /// A panel may own a graph, drawn above its knobs rather than in a separate
    /// strip - an envelope curve belongs directly over the four knobs that
    /// shape it, not in a row of graphs at the top of the page.
    struct Panel {
        juce::String              title;
        std::vector<CellControl*> controls;
        juce::Rectangle<int>      bounds;
        int                       titleH = 0;   ///< set by the layout pass
        juce::Component*          visual = nullptr;
        juce::Colour              tint;
        /// Colour the model chose for this block, if it chose one. Transparent
        /// means "fall back to the generated rhythm of accent and grey".
        juce::Colour              authored;
        /// Drawn larger: the panels the model nominated as the face of the
        /// instrument. A page where every block is the same size has no focus.
        bool                      featured = false;
    };

    struct Tab {
        juce::String                    id, title;
        std::vector<CellControl*>       controls;
        std::vector<Panel>              panels;
        std::vector<juce::Component*>   visuals;
        /// Nodes this tab's controls touch. Graphs are chosen from these rather
        /// than from the tab's name - a section called "AMP" still owns an
        /// envelope, and keying off the title meant its curve never appeared.
        std::set<std::string>           nodeIds;
    };

    void buildTabButtons();
    void setActiveTab(int index);
    void layoutActiveTab();
    ForgeParameter* slotForBinding(const std::string& nodeId, const std::string& moduleParam) const;
    void buildVisualsFor(Tab&, const ir::Instrument&);

    ForgeAudioProcessor& processor_;
    Mode                 mode_ = Mode::Controls;
    juce::Colour         accent_{0xffe4572e};

    juce::OwnedArray<juce::TextButton> tabButtons_;
    juce::OwnedArray<CellControl>      controls_;
    juce::OwnedArray<juce::Component>  visuals_;
    std::vector<Tab>                   tabs_;
    int                                activeTab_ = 0;

    GraphView   graphView_;
    juce::Label emptyLabel_;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(InstrumentView)
};

} // namespace forge
