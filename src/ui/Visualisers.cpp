#include "ui/Visualisers.h"

#include "core/dsp/ModuleKit.h"
#include "ui/Fonts.h"
#include "ui/ForgeLookAndFeel.h"

namespace forge {
namespace {

constexpr float kTitleH = 14.0f;
constexpr float kFootH  = 12.0f;
constexpr float kHandleR = 3.4f;

/// Envelope times are logarithmic over a huge range, so plotting them linearly
/// would squash every usable value into the first pixel. Compress to a fraction
/// of the width instead.
float timeToWidthFraction(float normalised) {
    return juce::jlimit(0.02f, 1.0f, std::sqrt(juce::jlimit(0.0f, 1.0f, normalised)));
}

float paramNorm(const ForgeParameter* p, float fallback = 0.5f) {
    return p != nullptr ? p->normalisedValue() : fallback;
}

void setParamNorm(ForgeParameter* p, float value) {
    if (p == nullptr) return;
    p->beginChangeGesture();
    p->setValueNotifyingHost(juce::jlimit(0.0f, 1.0f, value));
    p->endChangeGesture();
}

} // namespace

// ===========================================================================
// EnvelopeGraph
// ===========================================================================

EnvelopeGraph::EnvelopeGraph(juce::String title, Bindings bindings, juce::Colour accent)
    : title_(std::move(title)), bind_(bindings), accent_(accent) {
    setTooltip("Drag the breakpoints to shape the envelope. Attack, decay and "
               "release move horizontally; sustain moves vertically.");
    startTimerHz(20);
}

EnvelopeGraph::~EnvelopeGraph() { stopTimer(); }

void EnvelopeGraph::timerCallback() { repaint(); }

juce::Rectangle<float> EnvelopeGraph::plotArea() const {
    return getLocalBounds().toFloat()
               .withTrimmedTop(kTitleH)
               .withTrimmedBottom(kFootH)
               .reduced(6.0f, 4.0f);
}

void EnvelopeGraph::breakpoints(juce::Point<float> out[4]) const {
    const auto r = plotArea();
    const float a = timeToWidthFraction(paramNorm(bind_.attack,  0.1f));
    const float d = timeToWidthFraction(paramNorm(bind_.decay,   0.3f));
    const float s = juce::jlimit(0.0f, 1.0f, paramNorm(bind_.sustain, 0.7f));
    const float rel = timeToWidthFraction(paramNorm(bind_.release, 0.3f));

    // Four segments share the width: attack, decay, a fixed sustain plateau,
    // then release. Normalising keeps the whole curve on screen whatever the
    // times are.
    const float total = a + d + 0.30f + rel;
    const float unit  = r.getWidth() / juce::jmax(0.001f, total);

    const float x0 = r.getX();
    const float x1 = x0 + a * unit;
    const float x2 = x1 + d * unit;
    const float x3 = x2 + 0.30f * unit;

    out[0] = {x1, r.getY()};                                   // peak
    out[1] = {x2, r.getBottom() - s * r.getHeight()};           // sustain entry
    out[2] = {x3, r.getBottom() - s * r.getHeight()};           // sustain exit
    out[3] = {juce::jmin(r.getRight(), x3 + rel * unit), r.getBottom()};
}

EnvelopeGraph::Handle EnvelopeGraph::hitTest(juce::Point<float> p) const {
    juce::Point<float> bp[4];
    breakpoints(bp);
    const float grab = 9.0f;
    if (p.getDistanceFrom(bp[0]) < grab) return Handle::Attack;
    if (p.getDistanceFrom(bp[1]) < grab) return Handle::Decay;
    if (p.getDistanceFrom(bp[2]) < grab) return Handle::Sustain;
    if (p.getDistanceFrom(bp[3]) < grab) return Handle::Release;
    return Handle::None;
}

void EnvelopeGraph::mouseMove(const juce::MouseEvent& e) {
    const auto h = hitTest(e.position);
    if (h != hover_) { hover_ = h; repaint(); }
    setMouseCursor(h == Handle::None ? juce::MouseCursor::NormalCursor
                                     : juce::MouseCursor::DraggingHandCursor);
}

void EnvelopeGraph::mouseDown(const juce::MouseEvent& e) {
    dragging_ = hitTest(e.position);
    dragStart_ = e.position;
    switch (dragging_) {
        case Handle::Attack:  dragStartValue_ = paramNorm(bind_.attack);  break;
        case Handle::Decay:   dragStartValue_ = paramNorm(bind_.decay);   break;
        case Handle::Sustain: dragStartValue_ = paramNorm(bind_.sustain); break;
        case Handle::Release: dragStartValue_ = paramNorm(bind_.release); break;
        default: break;
    }
}

void EnvelopeGraph::mouseDrag(const juce::MouseEvent& e) {
    if (dragging_ == Handle::None) return;
    const auto r = plotArea();

    if (dragging_ == Handle::Sustain) {
        const float dy = (dragStart_.y - e.position.y) / juce::jmax(1.0f, r.getHeight());
        setParamNorm(bind_.sustain, dragStartValue_ + dy);
    } else {
        // Undo the sqrt compression so a drag feels linear on screen.
        const float dx = (e.position.x - dragStart_.x) / juce::jmax(1.0f, r.getWidth());
        const float startCompressed = std::sqrt(juce::jlimit(0.0f, 1.0f, dragStartValue_));
        const float moved = juce::jlimit(0.0f, 1.0f, startCompressed + dx * 1.6f);
        const float value = moved * moved;
        switch (dragging_) {
            case Handle::Attack:  setParamNorm(bind_.attack,  value); break;
            case Handle::Decay:   setParamNorm(bind_.decay,   value); break;
            case Handle::Release: setParamNorm(bind_.release, value); break;
            default: break;
        }
    }
    repaint();
}

void EnvelopeGraph::mouseUp(const juce::MouseEvent&) { dragging_ = Handle::None; }

void EnvelopeGraph::paint(juce::Graphics& g) {
    const auto bounds = getLocalBounds().toFloat();
    g.setColour(juce::Colour(0xff0c0c10));
    g.fillRoundedRectangle(bounds, 3.0f);
    g.setColour(ForgeLookAndFeel::outline());
    g.drawRoundedRectangle(bounds.reduced(0.5f), 3.0f, 1.0f);

    g.setFont(fonts::get(9.0f, fonts::Weight::SemiBold));
    g.setColour(ForgeLookAndFeel::textDim());
    g.drawText(title_.toUpperCase(), getLocalBounds().removeFromTop(static_cast<int>(kTitleH))
                                          .reduced(6, 0),
               juce::Justification::centredLeft, false);

    const auto r = plotArea();
    g.setColour(ForgeLookAndFeel::outline().withAlpha(0.5f));
    for (int i = 1; i < 4; ++i) {
        const float y = r.getY() + r.getHeight() * static_cast<float>(i) / 4.0f;
        g.drawHorizontalLine(juce::roundToInt(y), r.getX(), r.getRight());
    }

    juce::Point<float> bp[4];
    breakpoints(bp);

    juce::Path curve;
    curve.startNewSubPath(r.getX(), r.getBottom());
    curve.lineTo(bp[0]);
    curve.lineTo(bp[1]);
    curve.lineTo(bp[2]);
    curve.lineTo(bp[3]);

    juce::Path fill = curve;
    fill.lineTo(bp[3].x, r.getBottom());
    fill.closeSubPath();
    g.setColour(accent_.withAlpha(0.16f));
    g.fillPath(fill);

    g.setColour(accent_);
    g.strokePath(curve, juce::PathStrokeType(1.6f, juce::PathStrokeType::curved,
                                             juce::PathStrokeType::rounded));

    for (int i = 0; i < 4; ++i) {
        const bool active = (hover_ == static_cast<Handle>(i + 1))
                         || (dragging_ == static_cast<Handle>(i + 1));
        g.setColour(active ? juce::Colours::white : accent_.brighter(0.3f));
        g.fillEllipse(juce::Rectangle<float>(kHandleR * 2.0f, kHandleR * 2.0f)
                          .withCentre(bp[i]));
    }

    // Readout: the four values, so the graph does not cost you the numbers.
    g.setFont(fonts::get(8.5f));
    g.setColour(ForgeLookAndFeel::textDim());
    juce::String foot;
    if (bind_.attack)  foot << "A " << bind_.attack->getText(bind_.attack->normalisedValue(), 6) << "  ";
    if (bind_.decay)   foot << "D " << bind_.decay->getText(bind_.decay->normalisedValue(), 6) << "  ";
    if (bind_.sustain) foot << "S " << bind_.sustain->getText(bind_.sustain->normalisedValue(), 4) << "  ";
    if (bind_.release) foot << "R " << bind_.release->getText(bind_.release->normalisedValue(), 6);
    g.drawText(foot, getLocalBounds().removeFromBottom(static_cast<int>(kFootH)).reduced(6, 0),
               juce::Justification::centredLeft, false);
}

// ===========================================================================
// LfoGraph
// ===========================================================================

LfoGraph::LfoGraph(juce::String title, juce::String shape, ForgeParameter* rate,
                   juce::Colour accent)
    : title_(std::move(title)), shape_(std::move(shape)), rate_(rate), accent_(accent) {
    setTooltip("Current LFO shape and rate.");
    startTimerHz(12);
}

LfoGraph::~LfoGraph() { stopTimer(); }

void LfoGraph::setShape(juce::String shape) { shape_ = std::move(shape); repaint(); }

void LfoGraph::timerCallback() { repaint(); }

float LfoGraph::sample(float phase) const {
    phase -= std::floor(phase);
    if (shape_ == "sine")   return std::sin(6.28318530718f * phase);
    if (shape_ == "tri")    return 4.0f * std::abs(phase - 0.5f) - 1.0f;
    if (shape_ == "saw")    return 1.0f - 2.0f * phase;
    if (shape_ == "ramp")   return 2.0f * phase - 1.0f;
    if (shape_ == "square") return phase < 0.5f ? 1.0f : -1.0f;
    if (shape_ == "sh" || shape_ == "random") {
        // Deterministic pseudo-steps so the picture is stable frame to frame.
        const int step = static_cast<int>(phase * 8.0f);
        return std::sin(static_cast<float>(step) * 12.9898f) * 0.9f;
    }
    return std::sin(6.28318530718f * phase);
}

void LfoGraph::paint(juce::Graphics& g) {
    const auto bounds = getLocalBounds().toFloat();
    g.setColour(juce::Colour(0xff0c0c10));
    g.fillRoundedRectangle(bounds, 3.0f);
    g.setColour(ForgeLookAndFeel::outline());
    g.drawRoundedRectangle(bounds.reduced(0.5f), 3.0f, 1.0f);

    g.setFont(fonts::get(9.0f, fonts::Weight::SemiBold));
    g.setColour(ForgeLookAndFeel::textDim());
    g.drawText(title_.toUpperCase() + "  " + shape_,
               getLocalBounds().removeFromTop(static_cast<int>(kTitleH)).reduced(6, 0),
               juce::Justification::centredLeft, false);

    const auto r = getLocalBounds().toFloat()
                       .withTrimmedTop(kTitleH).withTrimmedBottom(kFootH).reduced(6.0f, 3.0f);

    g.setColour(ForgeLookAndFeel::outline().withAlpha(0.6f));
    g.drawHorizontalLine(juce::roundToInt(r.getCentreY()), r.getX(), r.getRight());

    juce::Path path;
    const int steps = juce::jmax(8, static_cast<int>(r.getWidth()));
    for (int i = 0; i <= steps; ++i) {
        const float t = static_cast<float>(i) / static_cast<float>(steps);
        const float y = r.getCentreY() - sample(t * 2.0f) * r.getHeight() * 0.45f;
        const float x = r.getX() + t * r.getWidth();
        if (i == 0) path.startNewSubPath(x, y);
        else        path.lineTo(x, y);
    }
    g.setColour(accent_);
    g.strokePath(path, juce::PathStrokeType(1.5f));

    if (rate_ != nullptr) {
        g.setFont(fonts::get(8.5f));
        g.setColour(ForgeLookAndFeel::textDim());
        g.drawText(rate_->getText(rate_->normalisedValue(), 8) + " Hz",
                   getLocalBounds().removeFromBottom(static_cast<int>(kFootH)).reduced(6, 0),
                   juce::Justification::centredLeft, false);
    }
}

// ===========================================================================
// EqCurve
// ===========================================================================

namespace {
constexpr float kEqMinHz = 20.0f, kEqMaxHz = 20000.0f, kEqMaxDb = 18.0f;

float engineering(const ForgeParameter* p, float fallback) {
    return p != nullptr ? p->engineeringValue() : fallback;
}
} // namespace

EqCurve::EqCurve(Band low, Band mid, Band high, juce::Colour accent, double sampleRate)
    : accent_(accent), sampleRate_(sampleRate) {
    bands_[0] = low; bands_[1] = mid; bands_[2] = high;
    setTooltip("Drag a band left/right for frequency, up/down for gain. "
               "Scroll over the middle band to change its width (Q).");
    startTimerHz(20);
}

EqCurve::~EqCurve() { stopTimer(); }

void EqCurve::timerCallback() { repaint(); }

juce::Rectangle<float> EqCurve::plot() const {
    return getLocalBounds().toFloat().withTrimmedTop(kTitleH).reduced(8.0f, 6.0f);
}

float EqCurve::freqToX(float hz) const {
    const auto r = plot();
    const float t = std::log(juce::jlimit(kEqMinHz, kEqMaxHz, hz) / kEqMinHz)
                  / std::log(kEqMaxHz / kEqMinHz);
    return r.getX() + t * r.getWidth();
}
float EqCurve::xToFreq(float x) const {
    const auto r = plot();
    const float t = juce::jlimit(0.0f, 1.0f, (x - r.getX()) / juce::jmax(1.0f, r.getWidth()));
    return kEqMinHz * std::pow(kEqMaxHz / kEqMinHz, t);
}
float EqCurve::dbToY(float db) const {
    const auto r = plot();
    return r.getCentreY() - juce::jlimit(-kEqMaxDb, kEqMaxDb, db) / kEqMaxDb * r.getHeight() * 0.5f;
}
float EqCurve::yToDb(float y) const {
    const auto r = plot();
    return juce::jlimit(-kEqMaxDb, kEqMaxDb,
                        (r.getCentreY() - y) / juce::jmax(1.0f, r.getHeight() * 0.5f) * kEqMaxDb);
}

float EqCurve::responseDbAt(float hz) const {
    // Build the exact filters the DSP builds, then read their magnitude.
    kit::Biquad low, mid, high;
    low.setLowShelf(engineering(bands_[0].freq, 120.0f),
                    engineering(bands_[0].gain, 0.0f), sampleRate_);
    mid.setPeak(engineering(bands_[1].freq, 1000.0f),
                engineering(bands_[1].q, 0.9f),
                engineering(bands_[1].gain, 0.0f), sampleRate_);
    high.setHighShelf(engineering(bands_[2].freq, 6000.0f),
                      engineering(bands_[2].gain, 0.0f), sampleRate_);

    return kit::Biquad::magnitudeDb(low.coeffs(),  hz, sampleRate_)
         + kit::Biquad::magnitudeDb(mid.coeffs(),  hz, sampleRate_)
         + kit::Biquad::magnitudeDb(high.coeffs(), hz, sampleRate_);
}

juce::Point<float> EqCurve::handleFor(int band) const {
    const float defFreq = band == 0 ? 120.0f : (band == 1 ? 1000.0f : 6000.0f);
    return {freqToX(engineering(bands_[band].freq, defFreq)),
            dbToY(engineering(bands_[band].gain, 0.0f))};
}

int EqCurve::hitTest(juce::Point<float> p) const {
    for (int i = 0; i < 3; ++i)
        if (p.getDistanceFrom(handleFor(i)) < 11.0f) return i;
    return -1;
}

void EqCurve::mouseMove(const juce::MouseEvent& e) {
    const int h = hitTest(e.position);
    if (h != hover_) { hover_ = h; repaint(); }
    setMouseCursor(h < 0 ? juce::MouseCursor::NormalCursor
                         : juce::MouseCursor::DraggingHandCursor);
}

void EqCurve::mouseDown(const juce::MouseEvent& e) { dragging_ = hitTest(e.position); }
void EqCurve::mouseUp(const juce::MouseEvent&)     { dragging_ = -1; }

void EqCurve::mouseDrag(const juce::MouseEvent& e) {
    if (dragging_ < 0) return;
    auto& band = bands_[dragging_];
    if (band.freq != nullptr) band.freq->setEngineeringValue(xToFreq(e.position.x));
    if (band.gain != nullptr) band.gain->setEngineeringValue(yToDb(e.position.y));
    repaint();
}

void EqCurve::mouseWheelMove(const juce::MouseEvent& e, const juce::MouseWheelDetails& w) {
    const int band = hitTest(e.position);
    if (band < 0 || bands_[band].q == nullptr) return;
    const float current = engineering(bands_[band].q, 0.9f);
    bands_[band].q->setEngineeringValue(juce::jlimit(0.2f, 8.0f,
                                                     current * (1.0f + w.deltaY * 0.6f)));
    repaint();
}

void EqCurve::paint(juce::Graphics& g) {
    const auto bounds = getLocalBounds().toFloat();
    g.setColour(juce::Colour(0xff0c0c10));
    g.fillRoundedRectangle(bounds, 3.0f);
    g.setColour(ForgeLookAndFeel::outline());
    g.drawRoundedRectangle(bounds.reduced(0.5f), 3.0f, 1.0f);

    g.setFont(fonts::get(9.0f, fonts::Weight::SemiBold));
    g.setColour(ForgeLookAndFeel::textDim());
    g.drawText("EQ", getLocalBounds().removeFromTop(static_cast<int>(kTitleH)).reduced(8, 0),
               juce::Justification::centredLeft, false);

    const auto r = plot();

    // Decade grid, labelled where there is room.
    g.setFont(fonts::get(8.0f));
    for (float hz : {50.0f, 100.0f, 500.0f, 1000.0f, 5000.0f, 10000.0f}) {
        const float x = freqToX(hz);
        g.setColour(ForgeLookAndFeel::outline().withAlpha(0.45f));
        g.drawVerticalLine(juce::roundToInt(x), r.getY(), r.getBottom());
        g.setColour(ForgeLookAndFeel::textDim().withAlpha(0.7f));
        g.drawText(hz >= 1000.0f ? juce::String(hz / 1000.0f, 0) + "k" : juce::String(hz, 0),
                   juce::Rectangle<float>(x - 14.0f, r.getBottom() - 11.0f, 28.0f, 10.0f).toNearestInt(),
                   juce::Justification::centred, false);
    }
    for (float db : {-12.0f, 0.0f, 12.0f}) {
        const float y = dbToY(db);
        g.setColour(db == 0.0f ? ForgeLookAndFeel::outline().brighter(0.3f)
                               : ForgeLookAndFeel::outline().withAlpha(0.4f));
        g.drawHorizontalLine(juce::roundToInt(y), r.getX(), r.getRight());
    }

    // Response curve.
    juce::Path curve;
    const int steps = juce::jmax(24, static_cast<int>(r.getWidth()));
    for (int i = 0; i <= steps; ++i) {
        const float x  = r.getX() + r.getWidth() * static_cast<float>(i) / static_cast<float>(steps);
        const float y  = dbToY(responseDbAt(xToFreq(x)));
        if (i == 0) curve.startNewSubPath(x, y);
        else        curve.lineTo(x, y);
    }

    juce::Path fill = curve;
    fill.lineTo(r.getRight(), r.getCentreY());
    fill.lineTo(r.getX(),     r.getCentreY());
    fill.closeSubPath();
    g.setColour(accent_.withAlpha(0.18f));
    g.fillPath(fill);

    g.setColour(accent_);
    g.strokePath(curve, juce::PathStrokeType(1.8f, juce::PathStrokeType::curved,
                                             juce::PathStrokeType::rounded));

    static const char* names[3] = {"LOW", "MID", "HIGH"};
    for (int i = 0; i < 3; ++i) {
        const auto p = handleFor(i);
        const bool active = hover_ == i || dragging_ == i;
        g.setColour(accent_.withAlpha(active ? 1.0f : 0.75f));
        g.fillEllipse(juce::Rectangle<float>(active ? 11.0f : 8.0f,
                                             active ? 11.0f : 8.0f).withCentre(p));
        g.setColour(juce::Colours::black.withAlpha(0.8f));
        g.drawEllipse(juce::Rectangle<float>(active ? 11.0f : 8.0f,
                                             active ? 11.0f : 8.0f).withCentre(p), 1.0f);
        if (active) {
            g.setColour(ForgeLookAndFeel::textPrimary());
            g.setFont(fonts::get(9.0f, fonts::Weight::Medium));
            g.drawText(juce::String(names[i]) + "  "
                           + juce::String(engineering(bands_[i].freq, 0.0f), 0) + " Hz  "
                           + juce::String(engineering(bands_[i].gain, 0.0f), 1) + " dB",
                       juce::Rectangle<float>(p.x - 70.0f, p.y - 24.0f, 140.0f, 14.0f).toNearestInt(),
                       juce::Justification::centred, false);
        }
    }
}

// ===========================================================================
// WavetableGraph
// ===========================================================================

WavetableGraph::WavetableGraph(juce::String title, const Wavetable& table,
                               ForgeParameter* position, juce::Colour accent)
    : title_(std::move(title)), table_(table), position_(position), accent_(accent) {
    setTooltip("Wavetable frames. The highlighted trace is the current position.");
    startTimerHz(15);
}

WavetableGraph::~WavetableGraph() { stopTimer(); }

void WavetableGraph::timerCallback() {
    const float p = paramNorm(position_, 0.0f);
    if (std::abs(p - lastPosition_) > 0.001f) { lastPosition_ = p; repaint(); }
}

void WavetableGraph::paint(juce::Graphics& g) {
    const auto bounds = getLocalBounds().toFloat();
    g.setColour(juce::Colour(0xff0c0c10));
    g.fillRoundedRectangle(bounds, 3.0f);
    g.setColour(ForgeLookAndFeel::outline());
    g.drawRoundedRectangle(bounds.reduced(0.5f), 3.0f, 1.0f);

    g.setFont(fonts::get(9.0f, fonts::Weight::SemiBold));
    g.setColour(ForgeLookAndFeel::textDim());
    g.drawText(title_.toUpperCase(),
               getLocalBounds().removeFromTop(static_cast<int>(kTitleH)).reduced(6, 0),
               juce::Justification::centredLeft, false);

    const auto r = getLocalBounds().toFloat()
                       .withTrimmedTop(kTitleH).withTrimmedBottom(4.0f).reduced(8.0f, 4.0f);
    if (table_.data.empty() || table_.numFrames < 1) return;

    const float pos = paramNorm(position_, 0.0f) * static_cast<float>(table_.numFrames - 1);
    // Oblique projection: each frame steps up and to the right, back to front.
    const float stepX = r.getWidth() * 0.14f / juce::jmax(1.0f, static_cast<float>(table_.numFrames));
    const float stepY = r.getHeight() * 0.55f / juce::jmax(1.0f, static_cast<float>(table_.numFrames));
    const float traceW = r.getWidth() - stepX * static_cast<float>(table_.numFrames);
    const float traceH = r.getHeight() * 0.40f;

    for (int frame = table_.numFrames - 1; frame >= 0; --frame) {
        const float ox = r.getX() + stepX * static_cast<float>(frame);
        const float oy = r.getBottom() - stepY * static_cast<float>(frame) - traceH * 0.5f;

        juce::Path path;
        const int steps = juce::jmax(24, static_cast<int>(traceW / 2.0f));
        for (int i = 0; i <= steps; ++i) {
            const float t = static_cast<float>(i) / static_cast<float>(steps);
            const float v = table_.read(static_cast<float>(frame), 0, t);
            const float x = ox + t * traceW;
            const float y = oy - v * traceH * 0.5f;
            if (i == 0) path.startNewSubPath(x, y);
            else        path.lineTo(x, y);
        }

        const float distance = std::abs(static_cast<float>(frame) - pos);
        const bool  current  = distance < 0.5f;
        g.setColour(current ? accent_
                            : accent_.withAlpha(juce::jlimit(0.12f, 0.45f,
                                                             0.45f - distance * 0.12f)));
        g.strokePath(path, juce::PathStrokeType(current ? 1.7f : 1.0f));
    }
}

} // namespace forge
