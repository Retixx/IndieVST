#include "ui/Fonts.h"

#include <BinaryData.h>

namespace forge::fonts {
namespace {

struct Faces {
    juce::Typeface::Ptr regular, medium, semiBold;

    Faces() {
        regular  = juce::Typeface::createSystemTypefaceFor(BinaryData::InterRegular_ttf,
                                                           BinaryData::InterRegular_ttfSize);
        medium   = juce::Typeface::createSystemTypefaceFor(BinaryData::InterMedium_ttf,
                                                           BinaryData::InterMedium_ttfSize);
        semiBold = juce::Typeface::createSystemTypefaceFor(BinaryData::InterSemiBold_ttf,
                                                           BinaryData::InterSemiBold_ttfSize);
    }
};

const Faces& faces() {
    static const Faces f;
    return f;
}

} // namespace

juce::Font get(float height, Weight weight) {
    const auto& f = faces();
    juce::Typeface::Ptr face = f.regular;
    if (weight == Weight::Medium)        face = f.medium;
    else if (weight == Weight::SemiBold) face = f.semiBold;

    // If embedding ever fails, fall back rather than drawing nothing.
    if (face == nullptr) return juce::Font(juce::FontOptions(height));
    return juce::Font(juce::FontOptions(face).withHeight(height));
}

void drawTracked(juce::Graphics& g, const juce::String& text, juce::Rectangle<int> area,
                 juce::Justification justification, float tracking) {
    const auto font = g.getCurrentFont();

    float total = 0.0f;
    for (int i = 0; i < text.length(); ++i)
        total += juce::GlyphArrangement::getStringWidth(font, text.substring(i, i + 1)) + tracking;
    total = juce::jmax(0.0f, total - tracking);

    float x = static_cast<float>(area.getX());
    if (justification.testFlags(juce::Justification::horizontallyCentred))
        x = static_cast<float>(area.getCentreX()) - total * 0.5f;
    else if (justification.testFlags(juce::Justification::right))
        x = static_cast<float>(area.getRight()) - total;

    const float baseline = static_cast<float>(area.getCentreY())
                         + font.getAscent() * 0.5f - font.getDescent() * 0.25f;

    for (int i = 0; i < text.length(); ++i) {
        const auto glyph = text.substring(i, i + 1);
        g.drawSingleLineText(glyph, juce::roundToInt(x), juce::roundToInt(baseline));
        x += juce::GlyphArrangement::getStringWidth(font, glyph) + tracking;
    }
}

} // namespace forge::fonts
