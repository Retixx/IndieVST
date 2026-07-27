#include "ui/InstrumentView.h"

#include "ui/Fonts.h"
#include "ui/ForgeLookAndFeel.h"

#include <map>

namespace forge {
namespace {
constexpr int kPanelPadX     = 9;   // inner horizontal padding of a panel
constexpr int kPanelGap      = 7;   // gap between panels
constexpr int kPanelTitleH   = 17;
constexpr int kMaxRows       = 4;
} // namespace

InstrumentView::InstrumentView(ForgeAudioProcessor& processor) : processor_(processor) {
    viewport_.setViewedComponent(&canvas_, false);
    viewport_.setScrollBarsShown(false, true, false, true);
    viewport_.setScrollBarThickness(7);
    addAndMakeVisible(viewport_);

    addChildComponent(graphView_);

    emptyLabel_.setJustificationType(juce::Justification::centred);
    emptyLabel_.setColour(juce::Label::textColourId, ForgeLookAndFeel::textDim());
    emptyLabel_.setText("No instrument loaded.", juce::dontSendNotification);
    addChildComponent(emptyLabel_);

    rebuild();
}

InstrumentView::~InstrumentView() = default;

void InstrumentView::setAccent(juce::Colour accent) {
    accent_ = accent;
    graphView_.setAccent(accent);
    repaint();
}

void InstrumentView::setMode(Mode mode) {
    mode_ = mode;
    const bool graph = mode_ == Mode::Graph;
    graphView_.setVisible(graph);
    viewport_.setVisible(!graph);
    resized();
    repaint();
}

void InstrumentView::rebuild() {
    controls_.clear();
    panels_.clear();
    canvas_.removeAllChildren();

    const auto* instrument = processor_.currentInstrument();
    graphView_.setInstrument(instrument);
    emptyLabel_.setVisible(instrument == nullptr);
    if (instrument == nullptr) { resized(); repaint(); return; }

    auto& pool = processor_.parameterPool();

    std::map<std::string, int> paramIndex;
    for (size_t i = 0; i < instrument->params.size(); ++i)
        paramIndex[instrument->params[i].id] = static_cast<int>(i);

    for (const auto& uiSection : instrument->ui.sections) {
        Panel panel;
        panel.title = juce::String(uiSection.title);
        for (const auto& id : uiSection.params) {
            auto it = paramIndex.find(id);
            if (it == paramIndex.end() || it->second >= ParameterPool::kNumParams) continue;
            auto control = makeControl(pool.paramSlot(it->second),
                                       instrument->params[static_cast<size_t>(it->second)], false);
            if (control == nullptr) continue;
            auto* raw = control.release();
            controls_.add(raw);
            canvas_.addAndMakeVisible(raw);
            panel.controls.push_back(raw);
        }
        if (!panel.controls.empty()) panels_.push_back(std::move(panel));
    }

    // Macros get their own panel at the end, tinted with the accent so they
    // read as performance controls rather than as more parameters.
    if (!instrument->macros.empty()) {
        Panel panel;
        panel.title = "Macros";
        panel.isMacroPanel = true;
        for (size_t i = 0; i < instrument->macros.size() && i < ParameterPool::kNumMacros; ++i) {
            const auto spec = specForMacro(instrument->macros[i]);
            auto control = makeControl(pool.macroSlot(static_cast<int>(i)), spec, true);
            if (control == nullptr) continue;
            auto* raw = control.release();
            controls_.add(raw);
            canvas_.addAndMakeVisible(raw);
            panel.controls.push_back(raw);
        }
        if (!panel.controls.empty()) panels_.push_back(std::move(panel));
    }

    resized();
    repaint();
}

void InstrumentView::layoutPanels() {
    if (panels_.empty()) { canvas_.setSize(viewport_.getWidth(), viewport_.getHeight()); return; }

    const int available = juce::jmax(60, viewport_.getHeight() - 8);
    const int rows = juce::jlimit(1, kMaxRows, (available - kPanelTitleH) / cell::height);
    const int gridH = rows * cell::height;
    const int panelH = kPanelTitleH + gridH + 8;

    int totalWidth = kPanelGap;
    for (auto& panel : panels_) {
        const int count = static_cast<int>(panel.controls.size());
        const int cols  = (count + rows - 1) / rows;
        const int width = cols * cell::width + kPanelPadX * 2;
        panel.bounds = juce::Rectangle<int>(totalWidth, 4, width, panelH);
        totalWidth += width + kPanelGap;
    }

    // Centre the rack when it does not fill the width, so a small instrument
    // does not sit awkwardly against the left edge.
    const int slack = viewport_.getWidth() - totalWidth;
    const int offset = slack > 0 ? slack / 2 : 0;

    for (auto& panel : panels_) {
        panel.bounds.translate(offset, 0);
        const int count = static_cast<int>(panel.controls.size());
        const int cols  = (count + rows - 1) / rows;
        const int gridX = panel.bounds.getX() + kPanelPadX;
        const int gridY = panel.bounds.getY() + kPanelTitleH;

        for (int i = 0; i < count; ++i) {
            // Fill column by column so a 5-control panel reads as 4 + 1, not
            // as a ragged two-row block.
            const int col = i / rows;
            const int row = i % rows;
            // Centre the final, possibly short, column's contents vertically.
            const int inThisCol = juce::jmin(rows, count - col * rows);
            const int colOffset = (rows - inThisCol) * cell::height / 2;
            panel.controls[static_cast<size_t>(i)]->setBounds(
                gridX + col * cell::width,
                gridY + colOffset + row * cell::height,
                cell::width, cell::height);
            juce::ignoreUnused(cols);
        }
    }

    canvas_.setSize(juce::jmax(viewport_.getWidth(), totalWidth + offset),
                    juce::jmax(viewport_.getHeight(), panelH + 8));
}

void InstrumentView::resized() {
    auto bounds = getLocalBounds();
    emptyLabel_.setBounds(bounds);
    graphView_.setBounds(bounds);
    viewport_.setBounds(bounds);
    layoutPanels();
}

void InstrumentView::paintPanels(juce::Graphics& g) {
    for (const auto& panel : panels_) {
        const auto r = panel.bounds.toFloat();

        g.setColour(ForgeLookAndFeel::panel());
        g.fillRoundedRectangle(r, 4.0f);
        g.setColour(panel.isMacroPanel ? accent_.withAlpha(0.45f) : ForgeLookAndFeel::outline());
        g.drawRoundedRectangle(r.reduced(0.5f), 4.0f, 1.0f);

        auto titleArea = panel.bounds.withHeight(kPanelTitleH).reduced(kPanelPadX, 0);
        g.setFont(fonts::sectionHead());
        g.setColour(panel.isMacroPanel ? accent_ : ForgeLookAndFeel::textDim());
        fonts::drawTracked(g, panel.title.toUpperCase(), titleArea,
                           juce::Justification::centredLeft, 1.1f);
    }
}

void InstrumentView::paint(juce::Graphics&) {
    // Panels are drawn by the canvas; nothing to do at this level.
}

} // namespace forge
