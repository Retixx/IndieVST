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

    const int availW = juce::jmax(120, viewport_.getWidth());
    const int availH = juce::jmax(80,  viewport_.getHeight());

    // Choose the row count and cell scale together. Trying every legal row
    // count and keeping whichever fills the window best is what stops a
    // four-knob instrument from sitting in a puddle of empty panel.
    int   bestRows  = 1;
    float bestScale = 0.0f;
    for (int rows = 1; rows <= kMaxRows; ++rows) {
        int columns = 0;
        for (const auto& panel : panels_)
            columns += (static_cast<int>(panel.controls.size()) + rows - 1) / rows;

        const float neededW = static_cast<float>(columns * cell::width)
                            + static_cast<float>(panels_.size()) * (kPanelPadX * 2 + kPanelGap)
                            + kPanelGap;
        const float neededH = static_cast<float>(kPanelTitleH + rows * cell::height + 10);
        const float scale = juce::jmin(static_cast<float>(availW) / neededW,
                                       static_cast<float>(availH) / neededH);
        if (scale > bestScale) { bestScale = scale; bestRows = rows; }
    }

    const int   rows  = bestRows;
    const float scale = juce::jlimit(0.75f, cell::maxScale, bestScale);
    const int   cellW = juce::roundToInt(cell::width  * scale);
    const int   cellH = juce::roundToInt(cell::height * scale);
    const int   padX  = juce::roundToInt(kPanelPadX * scale);
    const int   gap   = juce::roundToInt(kPanelGap * scale);
    const int   titleH = juce::roundToInt(kPanelTitleH * scale);

    // Panels always span the full height, so the rack reads as a rack.
    const int panelH = availH - 4;

    int totalWidth = gap;
    for (auto& panel : panels_) {
        const int cols = (static_cast<int>(panel.controls.size()) + rows - 1) / rows;
        const int width = cols * cellW + padX * 2;
        panel.bounds = juce::Rectangle<int>(totalWidth, 2, width, panelH);
        totalWidth += width + gap;
    }

    const int slack  = availW - totalWidth;
    const int offset = slack > 0 ? slack / 2 : 0;

    for (auto& panel : panels_) {
        panel.bounds.translate(offset, 0);
        const int count = static_cast<int>(panel.controls.size());
        const int gridX = panel.bounds.getX() + padX;
        // Centre the grid inside the full-height panel.
        const int gridY = panel.bounds.getY() + titleH
                        + juce::jmax(0, (panelH - titleH - rows * cellH) / 2);

        for (int i = 0; i < count; ++i) {
            const int col = i / rows;
            const int row = i % rows;
            // A short final column is centred against the others rather than
            // left hanging from the top.
            const int inThisCol = juce::jmin(rows, count - col * rows);
            const int colOffset = (rows - inThisCol) * cellH / 2;
            panel.controls[static_cast<size_t>(i)]->setBounds(
                gridX + col * cellW, gridY + colOffset + row * cellH, cellW, cellH);
        }
    }

    panelTitleH_ = titleH;
    panelPadX_   = padX;
    canvas_.setSize(juce::jmax(availW, totalWidth + offset), availH);
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

        auto titleArea = panel.bounds.withHeight(panelTitleH_).reduced(panelPadX_, 0);
        g.setFont(fonts::get(juce::jlimit(8.5f, 12.0f, panelTitleH_ * 0.52f),
                             fonts::Weight::SemiBold));
        g.setColour(panel.isMacroPanel ? accent_ : ForgeLookAndFeel::textDim());
        fonts::drawTracked(g, panel.title.toUpperCase(), titleArea,
                           juce::Justification::centredLeft, 1.1f);
    }
}

void InstrumentView::paint(juce::Graphics&) {
    // Panels are drawn by the canvas; nothing to do at this level.
}

} // namespace forge
