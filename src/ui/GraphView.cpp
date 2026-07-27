#include "ui/GraphView.h"

#include "core/dsp/Registry.h"
#include "core/ir/IrValidator.h"
#include "ui/ForgeLookAndFeel.h"

#include <map>

namespace forge {

void GraphView::setInstrument(const ir::Instrument* instrument) {
    hasInstrument_ = instrument != nullptr;
    if (hasInstrument_) instrument_ = *instrument;
    layout();
    repaint();
}

void GraphView::resized() { layout(); }

void GraphView::layout() {
    boxes_.clear();
    if (!hasInstrument_ || getWidth() < 40 || getHeight() < 40) return;

    // Column = distance from the furthest upstream source, so the drawing reads
    // left-to-right in signal order.
    std::map<std::string, int> column;
    for (Scope scope : {Scope::Voice, Scope::Global}) {
        std::vector<int> order;
        if (!ir::topoSort(instrument_, scope, order)) continue;
        for (int idx : order) {
            const auto& node = instrument_.nodes[static_cast<size_t>(idx)];
            int c = 0;
            for (const auto& conn : instrument_.audio) {
                if (conn.to != node.id) continue;
                auto it = column.find(conn.from);
                if (it != column.end()) c = juce::jmax(c, it->second + 1);
            }
            column[node.id] = c;
        }
    }

    int maxColumn = 0;
    for (const auto& kv : column) maxColumn = juce::jmax(maxColumn, kv.second);

    std::map<int, int> usedRows;
    const float colWidth = static_cast<float>(getWidth() - 20) / static_cast<float>(maxColumn + 1);
    const float boxH = 22.0f;
    const float boxW = juce::jmin(colWidth - 10.0f, 120.0f);

    const auto& registry = Registry::instance();
    for (const auto& node : instrument_.nodes) {
        auto it = column.find(node.id);
        if (it == column.end()) continue;

        const int c = it->second;
        const int row = usedRows[c]++;

        Box box;
        box.id    = juce::String(node.id);
        box.type  = juce::String(node.type);
        box.voice = node.scope == Scope::Voice;
        if (const auto* man = registry.find(node.type)) box.modSource = man->isModSource;

        box.bounds = juce::Rectangle<float>(
            10.0f + static_cast<float>(c) * colWidth,
            10.0f + static_cast<float>(row) * (boxH + 8.0f),
            juce::jmax(56.0f, boxW), boxH);
        boxes_.push_back(std::move(box));
    }
}

void GraphView::paint(juce::Graphics& g) {
    g.setColour(ForgeLookAndFeel::panel());
    g.fillRoundedRectangle(getLocalBounds().toFloat(), 8.0f);

    if (!hasInstrument_ || boxes_.empty()) {
        g.setColour(ForgeLookAndFeel::textSecondary());
        g.setFont(juce::Font(juce::FontOptions(13.0f)));
        g.drawFittedText("No instrument loaded.", getLocalBounds(),
                         juce::Justification::centred, 1);
        return;
    }

    auto boxFor = [this](const std::string& id) -> const Box* {
        for (const auto& b : boxes_) if (b.id == juce::String(id)) return &b;
        return nullptr;
    };

    // Audio connections.
    g.setColour(ForgeLookAndFeel::outline().brighter(0.35f));
    for (const auto& conn : instrument_.audio) {
        const Box* from = boxFor(conn.from);
        const Box* to   = boxFor(conn.to);
        if (from == nullptr || to == nullptr) continue;
        juce::Path p;
        const auto a = juce::Point<float>(from->bounds.getRight(), from->bounds.getCentreY());
        const auto b = juce::Point<float>(to->bounds.getX(), to->bounds.getCentreY());
        p.startNewSubPath(a);
        p.cubicTo({a.x + 18.0f, a.y}, {b.x - 18.0f, b.y}, b);
        g.strokePath(p, juce::PathStrokeType(1.4f));
    }

    // Modulation routes, dashed and in the accent colour so they read as a
    // different kind of connection entirely.
    g.setColour(accent_.withAlpha(0.65f));
    for (const auto& route : instrument_.mod) {
        const Box* from = boxFor(route.source);
        const Box* to   = boxFor(route.targetNode);
        if (from == nullptr || to == nullptr) continue;
        juce::Path p;
        const auto a = juce::Point<float>(from->bounds.getCentreX(), from->bounds.getBottom());
        const auto b = juce::Point<float>(to->bounds.getCentreX(), to->bounds.getY());
        p.startNewSubPath(a);
        p.cubicTo({a.x, a.y + 22.0f}, {b.x, b.y - 22.0f}, b);
        const float dashes[] = {3.0f, 3.0f};
        juce::Path dashed;
        juce::PathStrokeType(1.2f).createDashedStroke(dashed, p, dashes, 2);
        g.fillPath(dashed);
    }

    for (const auto& box : boxes_) {
        g.setColour(box.modSource ? accent_.withAlpha(0.18f)
                                  : (box.voice ? ForgeLookAndFeel::panelRaised()
                                               : ForgeLookAndFeel::panelRaised().darker(0.25f)));
        g.fillRoundedRectangle(box.bounds, 4.0f);
        g.setColour(box.modSource ? accent_.withAlpha(0.8f) : ForgeLookAndFeel::outline());
        g.drawRoundedRectangle(box.bounds, 4.0f, 1.0f);

        g.setColour(ForgeLookAndFeel::textPrimary());
        g.setFont(juce::Font(juce::FontOptions(10.5f)));
        g.drawFittedText(box.type, box.bounds.toNearestInt().reduced(4, 1),
                         juce::Justification::centredLeft, 1);
    }

    g.setColour(ForgeLookAndFeel::textSecondary());
    g.setFont(juce::Font(juce::FontOptions(11.0f)));
    g.drawText(juce::String(static_cast<int>(instrument_.nodes.size())) + " modules  -  "
                   + juce::String(static_cast<int>(instrument_.audio.size())) + " audio  -  "
                   + juce::String(static_cast<int>(instrument_.mod.size())) + " modulation",
               getLocalBounds().removeFromBottom(18).reduced(10, 0),
               juce::Justification::centredLeft);
}

} // namespace forge
