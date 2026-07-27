// ---------------------------------------------------------------------------
// The "How it's built" panel.
//
// This is a pitch weapon as much as a debugging tool: it is the direct answer
// to "this is just presets with extra steps". Two prompts produce visibly
// different topologies, and you can point at them.
// ---------------------------------------------------------------------------
#pragma once

#include "core/ir/Ir.h"

#include <juce_gui_basics/juce_gui_basics.h>

namespace forge {

class GraphView final : public juce::Component {
public:
    GraphView() = default;

    void setInstrument(const ir::Instrument*);
    void setAccent(juce::Colour accent) { accent_ = accent; repaint(); }

    void paint(juce::Graphics&) override;
    void resized() override;

private:
    struct Box {
        juce::Rectangle<float> bounds;
        juce::String           id, type;
        bool                   voice = true;
        bool                   modSource = false;
    };

    void layout();

    ir::Instrument     instrument_;
    bool               hasInstrument_ = false;
    std::vector<Box>   boxes_;
    juce::Colour       accent_{0xffe4572e};

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(GraphView)
};

} // namespace forge
