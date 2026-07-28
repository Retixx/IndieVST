#include "ui/InstrumentView.h"

#include "core/dsp/AssetBaker.h"
#include "core/dsp/Registry.h"
#include "ui/Fonts.h"
#include "ui/ForgeLookAndFeel.h"

#include <map>

namespace forge {
namespace {
constexpr int kTabBarH    = 26;
constexpr int kVisualH    = 104;
constexpr int kCellW      = 50;   // denser than the old 58x62: a full rack needs it
constexpr int kCellH      = 56;
constexpr int kGap        = 4;
constexpr int kPanelPad   = 6;
constexpr int kPanelTitleH = 15;
} // namespace

InstrumentView::InstrumentView(ForgeAudioProcessor& processor) : processor_(processor) {
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
    for (auto* b : tabButtons_) b->setVisible(!graph);
    if (graph) {
        for (auto* c : controls_) c->setVisible(false);
        for (auto* v : visuals_)  v->setVisible(false);
    } else {
        setActiveTab(activeTab_);
    }
    resized();
    repaint();
}

ForgeParameter* InstrumentView::slotForBinding(const std::string& nodeId,
                                               const std::string& moduleParam) const {
    const auto* instrument = processor_.currentInstrument();
    if (instrument == nullptr) return nullptr;
    auto& pool = processor_.parameterPool();

    for (size_t i = 0; i < instrument->params.size()
                       && i < static_cast<size_t>(ParameterPool::kNumParams); ++i)
        for (const auto& b : instrument->params[i].bind)
            if (b.node == nodeId && b.param == moduleParam)
                return pool.paramSlot(static_cast<int>(i));
    return nullptr;
}

void InstrumentView::buildVisualsFor(Tab& tab, const ir::Instrument& instrument) {
    const auto& registry = Registry::instance();

    // Which panel owns this node? A graph is attached to the panel holding the
    // knobs it belongs to, so they sit together.
    auto panelForNode = [&](const std::string& nodeId) -> Panel* {
        for (auto& panel : tab.panels)
            for (auto* c : panel.controls) {
                juce::ignoreUnused(c);
                // Panels are named after their module, and the architecture
                // groups a node's controls into exactly one panel, so the
                // first panel containing any control bound to this node wins.
                for (const auto& p : instrument.params)
                    if (p.panel == panel.title.toStdString())
                        for (const auto& b : p.bind)
                            if (b.node == nodeId) return &panel;
                break;
            }
        return nullptr;
    };

    for (const auto& node : instrument.nodes) {
        const auto* man = registry.find(node.type);
        if (man == nullptr) continue;
        // Only draw graphs for nodes this tab actually controls.
        if (tab.nodeIds.count(node.id) == 0) continue;
        Panel* owner = panelForNode(node.id);

        if (node.type == "env.adsr") {
            EnvelopeGraph::Bindings bind;
            bind.attack  = slotForBinding(node.id, "attack_ms");
            bind.decay   = slotForBinding(node.id, "decay_ms");
            bind.sustain = slotForBinding(node.id, "sustain");
            bind.release = slotForBinding(node.id, "release_ms");
            if (bind.attack == nullptr && bind.release == nullptr) continue;
            auto* graph = new EnvelopeGraph(juce::String(node.id), bind, accent_);
            visuals_.add(graph);
            addChildComponent(graph);
            tab.visuals.push_back(graph);
            if (owner != nullptr) owner->visual = graph;
        }

        if (node.type == "lfo") {
            juce::String shape = "sine";
            if (node.settings.is_object()) {
                auto it = node.settings.find("wave");
                if (it != node.settings.end() && it->is_string())
                    shape = juce::String(it->get<std::string>());
            }
            auto* graph = new LfoGraph(juce::String(node.id), shape,
                                       slotForBinding(node.id, "rate_hz"), accent_);
            visuals_.add(graph);
            addChildComponent(graph);
            tab.visuals.push_back(graph);
            if (owner != nullptr) owner->visual = graph;
        }

        if (node.type == "fx.eq3") {
            EqCurve::Band low{ slotForBinding(node.id, "low_gain_db"),
                               slotForBinding(node.id, "low_freq"), nullptr };
            EqCurve::Band mid{ slotForBinding(node.id, "mid_gain_db"),
                               slotForBinding(node.id, "mid_freq"),
                               slotForBinding(node.id, "mid_q") };
            EqCurve::Band high{ slotForBinding(node.id, "high_gain_db"),
                                slotForBinding(node.id, "high_freq"), nullptr };
            if (low.gain != nullptr || mid.gain != nullptr || high.gain != nullptr) {
                auto* curve = new EqCurve(low, mid, high, accent_, processor_.getSampleRate());
                visuals_.add(curve);
                addChildComponent(curve);
                tab.visuals.push_back(curve);
                if (owner != nullptr) owner->visual = curve;
            }
        }

        if (node.type == "osc.wavetable") {
            // Re-bake the table from its recipe: the baker is framework-free
            // core code, so the picture is guaranteed to match what the DSP
            // is actually reading.
            std::string tableId;
            if (node.settings.is_object()) {
                auto it = node.settings.find("table");
                if (it != node.settings.end() && it->is_string()) tableId = it->get<std::string>();
            }
            for (const auto& asset : instrument.assets) {
                if (asset.id != tableId || asset.kind != "wavetable") continue;
                Wavetable table;
                bakeWavetable(asset.spec, table);
                auto* graph = new WavetableGraph("Wavetable", table,
                                                 slotForBinding(node.id, "position"), accent_);
                visuals_.add(graph);
                addChildComponent(graph);
                tab.visuals.push_back(graph);
                if (owner != nullptr) owner->visual = graph;
                break;
            }
        }
    }
}

void InstrumentView::rebuild() {
    controls_.clear();
    visuals_.clear();
    tabButtons_.clear();
    tabs_.clear();

    const auto* instrument = processor_.currentInstrument();
    graphView_.setInstrument(instrument);
    emptyLabel_.setVisible(instrument == nullptr);
    if (instrument == nullptr) { resized(); repaint(); return; }

    auto& pool = processor_.parameterPool();

    std::map<std::string, int> paramIndex;
    for (size_t i = 0; i < instrument->params.size(); ++i)
        paramIndex[instrument->params[i].id] = static_cast<int>(i);

    // The model authors panel colour and prominence; this turns those into
    // something the layout pass can use.
    auto makePanel = [&](const juce::String& title) {
        Panel panel;
        panel.title = title;
        for (const auto& [name, hex] : instrument->ui.panelAccent)
            if (title.equalsIgnoreCase(juce::String(name))) {
                auto text = juce::String(hex).trim().removeCharacters("#");
                if (text.length() == 6) text = "ff" + text;          // opaque RGB
                const auto c = juce::Colour::fromString(text);
                if (!c.isTransparent()) panel.authored = c;
                break;
            }
        for (const auto& name : instrument->ui.featuredPanels)
            if (title.equalsIgnoreCase(juce::String(name))) { panel.featured = true; break; }
        return panel;
    };

    // One tab per UI section.
    for (const auto& uiSection : instrument->ui.sections) {
        Tab tab;
        tab.title = juce::String(uiSection.title);
        tab.id    = tab.title.toLowerCase().toStdString();

        for (const auto& id : uiSection.params) {
            auto it = paramIndex.find(id);
            if (it == paramIndex.end() || it->second >= ParameterPool::kNumParams) continue;
            auto control = makeControl(pool.paramSlot(it->second),
                                       instrument->params[static_cast<size_t>(it->second)], false);
            if (control == nullptr) continue;
            auto* raw = control.release();
            controls_.add(raw);
            addChildComponent(raw);
            tab.controls.push_back(raw);
            for (const auto& bind : instrument->params[static_cast<size_t>(it->second)].bind)
                tab.nodeIds.insert(bind.node);

            // Cluster into the named sub-panel. This is what makes "Wow" legible:
            // it sits under a TAPE header instead of floating in a flat grid.
            const juce::String panelName(
                instrument->params[static_cast<size_t>(it->second)].panel);
            Panel* panel = nullptr;
            for (auto& p : tab.panels) if (p.title == panelName) { panel = &p; break; }
            if (panel == nullptr) {
                tab.panels.push_back(makePanel(panelName));
                panel = &tab.panels.back();
            }
            panel->controls.push_back(raw);
        }
        tabs_.push_back(std::move(tab));
    }

    // Selectors join the tab matching their group.
    for (const auto& sw : instrument->switches) {
        auto control = makeSwitch(processor_, *instrument, sw);
        if (control == nullptr) continue;
        auto* raw = control.release();
        controls_.add(raw);
        addChildComponent(raw);

        const juce::String group(sw.group.empty() ? "General" : sw.group);
        Tab* target = nullptr;
        for (auto& t : tabs_) if (t.title.equalsIgnoreCase(group)) { target = &t; break; }
        if (target == nullptr) {
            Tab tab;
            tab.title = group;
            tab.id    = group.toLowerCase().toStdString();
            tabs_.push_back(std::move(tab));
            target = &tabs_.back();
        }
        target->controls.push_back(raw);
        target->nodeIds.insert(sw.node);

        const juce::String panelName(sw.panel);
        Panel* panel = nullptr;
        for (auto& p : target->panels) if (p.title == panelName) { panel = &p; break; }
        if (panel == nullptr) {
            target->panels.push_back(makePanel(panelName));
            panel = &target->panels.back();
        }
        panel->controls.push_back(raw);
    }

    // Macros live on MASTER rather than a tab of their own - five knobs did not
    // justify a whole page, and output level belongs beside them anyway.
    if (!instrument->macros.empty()) {
        Tab* target = nullptr;
        for (auto& t : tabs_)
            if (t.title.equalsIgnoreCase("master") || t.title.equalsIgnoreCase("macros")) {
                target = &t; break;
            }
        // With the model naming its own pages there may be no tab called
        // MASTER. Adding one would insert furniture it did not design, so the
        // macros join the last page instead.
        if (target == nullptr && !tabs_.empty()) target = &tabs_.back();
        if (target == nullptr) {
            Tab tab; tab.title = "MASTER"; tab.id = "master";
            tabs_.push_back(std::move(tab));
            target = &tabs_.back();
        }
        for (size_t i = 0; i < instrument->macros.size()
                           && i < static_cast<size_t>(ParameterPool::kNumMacros); ++i) {
            const auto spec = specForMacro(instrument->macros[i]);
            auto control = makeControl(pool.macroSlot(static_cast<int>(i)), spec, true);
            if (control == nullptr) continue;
            auto* raw = control.release();
            controls_.add(raw);
            addChildComponent(raw);
            target->controls.push_back(raw);
            Panel* macroPanel = nullptr;
            for (auto& pnl : target->panels) if (pnl.title == "MACROS") { macroPanel = &pnl; break; }
            if (macroPanel == nullptr) {
                target->panels.push_back(makePanel("MACROS"));
                macroPanel = &target->panels.back();
            }
            macroPanel->controls.push_back(raw);
        }
    }

    for (auto& tab : tabs_) buildVisualsFor(tab, *instrument);

    tabs_.erase(std::remove_if(tabs_.begin(), tabs_.end(),
                               [](const Tab& t) { return t.controls.empty() && t.visuals.empty(); }),
                tabs_.end());

    buildTabButtons();
    activeTab_ = juce::jlimit(0, juce::jmax(0, static_cast<int>(tabs_.size()) - 1), activeTab_);
    setActiveTab(activeTab_);
    resized();
    repaint();
}

void InstrumentView::buildTabButtons() {
    for (size_t i = 0; i < tabs_.size(); ++i) {
        auto* b = new juce::TextButton(tabs_[i].title.toUpperCase());
        b->setClickingTogglesState(false);
        b->setWantsKeyboardFocus(false);
        b->onClick = [this, i] { setActiveTab(static_cast<int>(i)); };
        tabButtons_.add(b);
        addAndMakeVisible(b);
    }
}

void InstrumentView::setActiveTab(int index) {
    if (tabs_.empty()) return;
    activeTab_ = juce::jlimit(0, static_cast<int>(tabs_.size()) - 1, index);

    for (size_t t = 0; t < tabs_.size(); ++t) {
        const bool active = static_cast<int>(t) == activeTab_ && mode_ == Mode::Controls;
        for (auto* c : tabs_[t].controls) c->setVisible(active);
        for (auto* v : tabs_[t].visuals)  v->setVisible(active);
        if (t < static_cast<size_t>(tabButtons_.size()))
            tabButtons_[static_cast<int>(t)]->setToggleState(static_cast<int>(t) == activeTab_,
                                                             juce::dontSendNotification);
    }
    layoutActiveTab();
    repaint();
    if (onLayoutChanged) onLayoutChanged();
}

int InstrumentView::preferredContentHeight(int availableWidth) const {
    if (tabs_.empty() || mode_ == Mode::Graph) return 320;
    const auto& tab = tabs_[static_cast<size_t>(activeTab_)];
    if (tab.panels.empty()) return 320;

    // Target a comfortable knob size, then report whatever height that needs.
    // Sizing the window to the page is the only way a 36-control tab and a
    // 67-control tab can both fill their space.
    constexpr float kTargetScale = 1.25f;
    const int cw     = juce::roundToInt(kCellW * kTargetScale);
    const int pad    = juce::roundToInt(kPanelPad * kTargetScale);
    const int titleH = juce::roundToInt(kPanelTitleH * kTargetScale);
    const int ch     = juce::roundToInt(kCellH * kTargetScale);
    const int visH   = juce::roundToInt(kVisualH * kTargetScale * 0.62f);

    bool anyVisual = false;
    for (const auto& panel : tab.panels) if (panel.visual != nullptr) anyVisual = true;

    // Pick the row count that comes closest to a pleasing 2.6:1 page.
    int bestRows = 2, bestHeight = 0;
    double bestError = 1e18;
    for (int rows = 1; rows <= 4; ++rows) {
        int panelH = 0;
        for (const auto& panel : tab.panels) {
            const float sc = panel.featured ? 1.32f : 1.0f;
            const int   r  = panel.featured ? juce::jmax(1, rows - 1) : rows;
            panelH = juce::jmax(panelH, titleH + (anyVisual ? visH + kGap : 0)
                                        + r * (juce::roundToInt(ch * sc) + kGap) + pad);
        }
        int x = 0, shelves = 1;
        for (const auto& panel : tab.panels) {
            const float sc  = panel.featured ? 1.32f : 1.0f;
            const int   r   = panel.featured ? juce::jmax(1, rows - 1) : rows;
            const int   pcw = juce::roundToInt(cw * sc);
            const int   n    = static_cast<int>(panel.controls.size());
            const int   cols = juce::jmax(1, (n + r - 1) / r);
            // Mirror the real per-column widths, or the window is sized for a
            // narrower page than the one that actually gets drawn.
            std::vector<int> colw(static_cast<size_t>(cols), pcw);
            for (int c = 0; c < n; ++c) {
                auto& w = colw[static_cast<size_t>(c / r)];
                w = juce::jmax(w, juce::roundToInt(
                        pcw * panel.controls[static_cast<size_t>(c)]->widthUnits()));
            }
            int pw = pad * 2 - kGap;
            for (int w : colw) pw += w + kGap;
            if (panel.visual != nullptr) pw = juce::jmax(pw, juce::roundToInt(pcw * 3.4f));
            if (x + pw > availableWidth && x > 0) { x = 0; ++shelves; }
            x += pw + kGap;
        }
        const int total = shelves * (panelH + kGap) - kGap;
        const double ratio = static_cast<double>(availableWidth) / juce::jmax(1, total);
        const double error = std::abs(ratio - 2.6);
        if (error < bestError) { bestError = error; bestRows = rows; bestHeight = total; }
    }
    juce::ignoreUnused(bestRows);
    return juce::jlimit(240, 900, bestHeight + 8);
}

void InstrumentView::layoutActiveTab() {
    if (tabs_.empty() || mode_ != Mode::Controls) return;
    auto& tab = tabs_[static_cast<size_t>(activeTab_)];

    auto area = getLocalBounds().withTrimmedTop(kTabBarH + 4).reduced(4, 2);

    if (tab.panels.empty() || tab.controls.empty()) return;

    // Measure the REAL wrapped layout for a given rows/scale rather than
    // estimating it. Estimating is what let panels run off the bottom of the
    // FX tab: the guess said it fit, the actual wrap did not.
    struct Metrics { int cw, ch, pad, titleH, visualH, panelH, totalH, shelves; };
    // A featured panel gets bigger controls spread over fewer rows, so it
    // occupies the page the way the model said it should.
    auto cellScale = [](const Panel& panel) { return panel.featured ? 1.32f : 1.0f; };
    auto rowsFor   = [](const Panel& panel, int rows) {
        return panel.featured ? juce::jmax(1, rows - 1) : rows;
    };
    // Column widths, not one uniform cell width. A column holding "12 dB/oct"
    // gets the room it needs and the knobs beside it are left alone; that is
    // what stops selectors rendering as "Sq..." and "Ex...".
    //
    // Computed ONCE per (panel, row count), before the search begins. The
    // search evaluates hundreds of candidate layouts, and rebuilding these
    // vectors inside that loop meant allocating and re-measuring text on every
    // one - which is what made resizing the window crawl.
    constexpr int kMaxRows = 6;
    std::vector<std::array<std::vector<float>, kMaxRows + 1>> colUnits(tab.panels.size());
    for (size_t pi = 0; pi < tab.panels.size(); ++pi) {
        const auto& panel = tab.panels[pi];
        const int   n     = static_cast<int>(panel.controls.size());
        for (int rows = 1; rows <= kMaxRows; ++rows) {
            const int r    = rowsFor(panel, rows);
            const int cols = juce::jmax(1, (n + r - 1) / r);
            auto& units = colUnits[pi][static_cast<size_t>(rows)];
            units.assign(static_cast<size_t>(cols), 1.0f);
            for (int c = 0; c < n; ++c) {
                auto& u = units[static_cast<size_t>(c / r)];
                u = juce::jmax(u, panel.controls[static_cast<size_t>(c)]->widthUnits());
            }
        }
    }

    auto columnWidths = [&](size_t pi, int rows, int cw) {
        const float      pcw = cw * cellScale(tab.panels[pi]);
        const auto&      units = colUnits[pi][static_cast<size_t>(rows)];
        std::vector<int> widths(units.size());
        for (size_t i = 0; i < units.size(); ++i)
            widths[i] = juce::roundToInt(pcw * units[i]);
        return widths;
    };
    auto panelWidth = [&](size_t pi, int rows, int cw, int pad) {
        const float pcw = cw * cellScale(tab.panels[pi]);
        int pw = pad * 2 - kGap;
        for (float u : colUnits[pi][static_cast<size_t>(rows)])
            pw += juce::roundToInt(pcw * u) + kGap;
        // A panel carrying a graph needs enough width for the graph to read.
        if (tab.panels[pi].visual != nullptr)
            pw = juce::jmax(pw, juce::roundToInt(pcw * 3.4f));
        return pw;
    };

    auto measure = [&](int rows, float scale) {
        Metrics m{};
        m.cw      = juce::roundToInt(kCellW * scale);
        m.ch      = juce::roundToInt(kCellH * scale);
        m.pad     = juce::roundToInt(kPanelPad * scale);
        m.titleH  = juce::roundToInt(kPanelTitleH * scale);
        m.visualH = juce::roundToInt(kVisualH * scale * 0.62f);
        m.shelves = 1;

        bool anyVisual = false;
        for (const auto& panel : tab.panels) if (panel.visual != nullptr) anyVisual = true;
        // Shelves share a height so the page stays on a grid; a shelf with any
        // graph on it sets the height for all of them.
        // The tallest panel sets the shelf height, so an emphasised block with
        // larger knobs does not overrun the row beneath it.
        m.panelH = 0;
        for (const auto& panel : tab.panels) {
            const int pch = juce::roundToInt(m.ch * cellScale(panel));
            const int h   = m.titleH + (anyVisual ? m.visualH + kGap : 0)
                          + rowsFor(panel, rows) * (pch + kGap) + m.pad;
            m.panelH = juce::jmax(m.panelH, h);
        }

        int x = 0;
        for (size_t pi = 0; pi < tab.panels.size(); ++pi) {
            const int pw = panelWidth(pi, rows, m.cw, m.pad);
            if (x + pw > area.getWidth() && x > 0) { x = 0; ++m.shelves; }
            x += pw + kGap;
        }
        m.totalH = m.shelves * (m.panelH + kGap) - kGap;
        return m;
    };

    // Largest scale that genuinely fits, across every sensible row count.
    //
    // Coarse pass then a fine pass around the winner. The old exhaustive sweep
    // evaluated ~900 candidate layouts on every single resize event, which is
    // what made dragging the window edge unusable; this finds the same answer
    // in about a tenth of the work.
    int     bestRows  = 1;
    float   bestScale = 0.0f;
    Metrics best      = measure(1, 1.0f);

    int     fallbackRows  = kMaxRows;
    float   fallbackScale = 0.34f;
    Metrics fallback      = measure(kMaxRows, 0.34f);
    int     leastOverflow = fallback.totalH - area.getHeight();

    auto sweep = [&](float from, float to, float step) {
        for (int rows = 1; rows <= kMaxRows; ++rows) {
            for (float scale = from; scale >= to; scale -= step) {
                const auto m = measure(rows, scale);
                const int  overflow = m.totalH - area.getHeight();
                if (overflow <= 0) {
                    if (scale > bestScale) { bestScale = scale; bestRows = rows; best = m; }
                    break;                   // largest scale that fits for this row count
                }
                if (overflow < leastOverflow) {
                    leastOverflow = overflow;
                    fallbackRows  = rows;
                    fallbackScale = scale;
                    fallback      = m;
                }
            }
        }
    };

    sweep(3.4f, 0.34f, 0.12f);
    if (bestScale > 0.0f) {
        // Refine upward from just under the coarse winner.
        const float from = juce::jmin(3.4f, bestScale + 0.12f);
        bestScale = 0.0f;
        sweep(from, juce::jmax(0.34f, from - 0.14f), 0.02f);
    }
    if (bestScale <= 0.0f) { bestRows = fallbackRows; bestScale = fallbackScale; best = fallback; }

    const int rows = bestRows;

    // --- justify -----------------------------------------------------------
    //
    // Work out the shelves first, then SPREAD them to fill the page. Laying
    // panels out left to right at their natural width and centring the result
    // is what left a page with three knobs sitting in one small box in the
    // corner with 80% of the window empty around it - which reads as a broken
    // plugin, not a sparse one.
    //
    // Each shelf shares its leftover width among its panels, and the shelves
    // share the leftover height. Growth is capped so one small panel on a wide
    // shelf becomes a generous block rather than an absurd stretched bar.
    struct Shelf { size_t first, last; int natural; };
    std::vector<Shelf> shelves;
    {
        int used = 0;
        for (size_t i = 0; i < tab.panels.size(); ++i) {
            const int pw = panelWidth(i, rows, best.cw, best.pad);
            if (shelves.empty() || (used + kGap + pw > area.getWidth() && used > 0)) {
                shelves.push_back({i, i, pw});
                used = pw;
            } else {
                shelves.back().last = i;
                shelves.back().natural += kGap + pw;
                used += kGap + pw;
            }
        }
    }
    if (shelves.empty()) return;

    constexpr float kMaxStretch = 2.1f;
    const int shelfCount = static_cast<int>(shelves.size());
    const int naturalH   = shelfCount * (best.panelH + kGap) - kGap;
    const int extraH     = juce::jmax(0, area.getHeight() - naturalH);
    const int panelH     = best.panelH + juce::jmin(extraH / shelfCount,
                                                    juce::roundToInt(best.panelH * 0.85f));

    int x = area.getX();
    int y = area.getY();
    {
        const int finalH = shelfCount * (panelH + kGap) - kGap;
        y += juce::jmax(0, (area.getHeight() - finalH) / 2);
    }

    // Width each panel actually gets, after sharing out the slack.
    std::vector<int> widths(tab.panels.size(), 0);
    std::vector<int> shelfOffset(shelves.size(), 0);
    for (size_t si = 0; si < shelves.size(); ++si) {
        const auto& shelf = shelves[si];
        const int count = static_cast<int>(shelf.last - shelf.first + 1);
        const int slack = juce::jmax(0, area.getWidth() - shelf.natural);
        int assigned = 0;
        for (size_t i = shelf.first; i <= shelf.last; ++i) {
            const int natural = panelWidth(i, rows, best.cw, best.pad);
            const int share   = slack * natural / juce::jmax(1, shelf.natural - kGap * (count - 1));
            widths[i] = juce::jmin(natural + share,
                                   juce::roundToInt(natural * kMaxStretch));
            assigned += widths[i] + kGap;
        }
        // Anything the cap refused goes back as centring for that shelf.
        shelfOffset[si] = juce::jmax(0, (area.getWidth() - (assigned - kGap)) / 2);
    }

    size_t shelfIndex = 0;
    int panelIndex = 0;
    for (size_t pi = 0; pi < tab.panels.size(); ++pi) {
        auto& panel = tab.panels[pi];
        const int n  = static_cast<int>(panel.controls.size());

        if (pi == shelves[shelfIndex].first) {
            x = area.getX() + shelfOffset[shelfIndex];
            if (shelfIndex > 0) y += panelH + kGap;
        }
        const int pw = widths[pi];
        panel.bounds = {x, y, pw, panelH};
        panel.titleH = best.titleH;

        // Vary the hue slightly per panel. A single flat accent across ninety
        // knobs is what made the rack read as monochrome; rotating the hue a
        // few degrees per module keeps it coherent but no longer uniform.
        // Alternate tinted and near-grey panels. An accent on every panel is
        // still monochrome - it is the contrast between coloured and neutral
        // blocks that gives a page rhythm.
        if (!panel.authored.isTransparent()) {
            panel.tint = panel.authored;            // the model chose this one
        } else if (panelIndex % 3 == 1) {
            panel.tint = juce::Colour(0xff8a8a96).withRotatedHue(0.02f * (panelIndex % 5));
        } else {
            panel.tint = accent_.withRotatedHue(0.03f * static_cast<float>(panelIndex % 5) - 0.06f)
                                .withMultipliedSaturation(1.0f + 0.08f * ((panelIndex % 3) - 1));
        }

        const int pr    = rowsFor(panel, rows);
        const int pch   = juce::roundToInt(best.ch * cellScale(panel));
        const auto cols = columnWidths(pi, rows, best.cw);

        // Centre the contents in whatever height this panel ended up with.
        // Panels grow to fill the page, so anchoring content to the top would
        // just move the dead space from around the panel to inside it.
        const int visualH  = panel.visual != nullptr ? best.visualH + kGap : 0;
        const int contentH = visualH + pr * (pch + kGap) - kGap;
        int contentY = y + best.titleH
                     + juce::jmax(0, (panelH - best.titleH - contentH) / 2);

        if (panel.visual != nullptr) {
            panel.visual->setBounds(x + best.pad, contentY,
                                    pw - best.pad * 2, best.visualH);
            contentY += best.visualH + kGap;
        }

        int gridW = -kGap;
        for (int w : cols) gridW += w + kGap;
        // Centre the knob block under a graph that is wider than it.
        const int startX = x + juce::jmax(best.pad, (pw - gridW) / 2);

        std::vector<int> colX(cols.size(), 0);
        for (size_t i = 1; i < cols.size(); ++i)
            colX[i] = colX[i - 1] + cols[i - 1] + kGap;

        for (int c = 0; c < n; ++c) {
            const size_t col = static_cast<size_t>(c / pr);
            const int    row = c % pr;
            auto* control = panel.controls[static_cast<size_t>(c)];
            control->setBounds(startX + colX[col], contentY + row * (pch + kGap),
                               cols[col], pch);
            control->setColour(juce::Slider::rotarySliderFillColourId, panel.tint);
        }
        x += pw + kGap;
        ++panelIndex;
        if (shelfIndex + 1 < shelves.size() && pi == shelves[shelfIndex].last) ++shelfIndex;
    }
}

void InstrumentView::resized() {
    auto bounds = getLocalBounds();
    emptyLabel_.setBounds(bounds);
    graphView_.setBounds(bounds);

    auto bar = bounds.removeFromTop(kTabBarH).reduced(2, 2);
    const int n = tabButtons_.size();
    if (n > 0) {
        const int each = juce::jmin(96, bar.getWidth() / n);
        for (auto* b : tabButtons_) b->setBounds(bar.removeFromLeft(each).reduced(1, 0));
    }
    layoutActiveTab();
}

void InstrumentView::paint(juce::Graphics& g) {
    if (mode_ == Mode::Graph || tabs_.empty()) return;

    // Panel behind the active tab's contents.
    auto area = getLocalBounds().withTrimmedTop(kTabBarH).reduced(2, 0);
    g.setColour(ForgeLookAndFeel::panel());
    g.fillRoundedRectangle(area.toFloat(), 4.0f);
    g.setColour(ForgeLookAndFeel::outline());
    g.drawRoundedRectangle(area.toFloat().reduced(0.5f), 4.0f, 1.0f);

    // Accent underline on the selected tab.
    if (activeTab_ < tabButtons_.size()) {
        const auto b = tabButtons_[activeTab_]->getBounds();
        g.setColour(accent_);
        g.fillRect(b.getX() + 2, b.getBottom() - 1, b.getWidth() - 4, 2);
    }

    // Sub-panels, each with its header. A control's label only makes sense
    // under the name of the module it belongs to.
    const auto& tab = tabs_[static_cast<size_t>(activeTab_)];
    for (const auto& panel : tab.panels) {
        if (panel.bounds.isEmpty()) continue;
        const auto r = panel.bounds.toFloat();
        const auto tint = panel.tint.isTransparent() ? accent_ : panel.tint;

        // A flat fill reads as a wireframe. Panels get a shallow top-lit
        // gradient, a contact shadow and a coloured header rule, which is what
        // separates a mock-up from something that looks manufactured.
        g.setColour(juce::Colours::black.withAlpha(0.30f));
        g.fillRoundedRectangle(r.translated(0.0f, 1.5f), 5.0f);

        juce::ColourGradient face(ForgeLookAndFeel::panelRaised().withAlpha(0.92f),
                                  r.getCentreX(), r.getY(),
                                  ForgeLookAndFeel::panel().withAlpha(0.96f),
                                  r.getCentreX(), r.getBottom(), false);
        g.setGradientFill(face);
        g.fillRoundedRectangle(r, 5.0f);

        // The faintest wash of the panel's own colour, so a tinted panel is
        // tinted rather than merely captioned in colour.
        g.setColour(tint.withAlpha(panel.featured ? 0.10f : 0.05f));
        g.fillRoundedRectangle(r, 5.0f);

        g.setColour(juce::Colours::white.withAlpha(0.055f));
        g.drawRoundedRectangle(r.reduced(0.5f), 5.0f, 1.0f);
        g.setColour(ForgeLookAndFeel::outline().withAlpha(0.9f));
        g.drawRoundedRectangle(r.reduced(0.5f), 5.0f, 1.0f);

        // Header rule in the panel's colour: a featured panel gets a solid bar,
        // the rest a hairline. This is the page's visual hierarchy.
        const int titleBar = panel.titleH > 0 ? panel.titleH : kPanelTitleH;
        g.setColour(tint.withAlpha(panel.featured ? 0.85f : 0.45f));
        g.fillRect(r.getX() + 1.0f, r.getY() + titleBar - 1.0f,
                   r.getWidth() - 2.0f, panel.featured ? 1.8f : 1.0f);

        // The header has to grow with the panel. Pinned at 8.5px it disappeared
        // on a large page, which is what made a panel look untitled.
        const int titleH = panel.titleH > 0 ? panel.titleH : kPanelTitleH;
        g.setFont(fonts::get(juce::jlimit(8.5f, 15.0f, titleH * 0.52f),
                             fonts::Weight::SemiBold));
        g.setColour((panel.tint.isTransparent() ? accent_ : panel.tint).withAlpha(0.9f));
        fonts::drawTracked(g, panel.title.toUpperCase(),
                           panel.bounds.withHeight(titleH)
                                       .reduced(juce::roundToInt(titleH * 0.45f), 0),
                           juce::Justification::centredLeft, 1.0f);
    }
}

} // namespace forge
