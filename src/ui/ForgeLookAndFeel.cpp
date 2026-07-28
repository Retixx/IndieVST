#include "ui/ForgeLookAndFeel.h"

#include "ui/Fonts.h"

// MidiKeyboardComponent lives in juce_audio_utils, not juce_gui_basics.
#include <juce_audio_utils/juce_audio_utils.h>

namespace forge {

ForgeLookAndFeel::ForgeLookAndFeel() {
    setColour(juce::ResizableWindow::backgroundColourId, background());
    setColour(juce::Label::textColourId,                 textPrimary());
    setColour(juce::TextEditor::backgroundColourId,      panelRaised());
    setColour(juce::TextEditor::textColourId,            textPrimary());
    setColour(juce::TextEditor::outlineColourId,         outline());
    setColour(juce::TextEditor::focusedOutlineColourId,  accent_);
    setColour(juce::TextEditor::highlightColourId,       accent_.withAlpha(0.28f));
    setColour(juce::TextEditor::highlightedTextColourId, textPrimary());
    setColour(juce::CaretComponent::caretColourId,       accent_);
    setColour(juce::TextButton::buttonColourId,          panelRaised());
    setColour(juce::TextButton::textColourOffId,         textSecondary());
    setColour(juce::TextButton::textColourOnId,          textPrimary());
    setColour(juce::ComboBox::backgroundColourId,        panelRaised());
    setColour(juce::ComboBox::textColourId,              textPrimary());
    setColour(juce::ComboBox::outlineColourId,           outline());
    setColour(juce::ComboBox::arrowColourId,             textSecondary());
    setColour(juce::PopupMenu::backgroundColourId,       panelRaised());
    setColour(juce::PopupMenu::textColourId,             textPrimary());
    setColour(juce::PopupMenu::headerTextColourId,       textDim());
    setColour(juce::PopupMenu::highlightedBackgroundColourId, accent_.withAlpha(0.22f));
    setColour(juce::PopupMenu::highlightedTextColourId,  textPrimary());
    setColour(juce::ScrollBar::thumbColourId,            outlineBright());
    setColour(juce::ScrollBar::trackColourId,            juce::Colours::transparentBlack);
    setColour(juce::TooltipWindow::backgroundColourId,   panelRaised());
    setColour(juce::TooltipWindow::outlineColourId,      outline());
    setColour(juce::TooltipWindow::textColourId,         textPrimary());
    setColour(juce::MidiKeyboardComponent::whiteNoteColourId,      juce::Colour(0xffcfcfd6));
    setColour(juce::MidiKeyboardComponent::blackNoteColourId,      juce::Colour(0xff141418));
    setColour(juce::MidiKeyboardComponent::keySeparatorLineColourId, outline());
    setColour(juce::MidiKeyboardComponent::shadowColourId,         juce::Colours::transparentBlack);
    setColour(juce::MidiKeyboardComponent::textLabelColourId,      textDim());
}

void ForgeLookAndFeel::setAccent(juce::Colour accent) {
    accent_ = accent;
    setColour(juce::TextEditor::focusedOutlineColourId, accent_);
    setColour(juce::TextEditor::highlightColourId, accent_.withAlpha(0.28f));
    setColour(juce::CaretComponent::caretColourId, accent_);
    setColour(juce::PopupMenu::highlightedBackgroundColourId, accent_.withAlpha(0.22f));
    setColour(juce::MidiKeyboardComponent::keyDownOverlayColourId, accent_.withAlpha(0.75f));
    setColour(juce::MidiKeyboardComponent::mouseOverKeyOverlayColourId, accent_.withAlpha(0.3f));
}

// --- knobs -----------------------------------------------------------------

/// Modelled on FL Studio's stock dials: a flat slate cap with a soft vertical
/// gradient, a thin recessed track, a restrained value arc and a short pointer.
///
/// The previous version had a glowing accent arc, a bloom pass and a tick
/// scale - all of which read as decoration. Hardware dials are understated;
/// the panel is meant to sit still while you work on it for eight hours.
void ForgeLookAndFeel::drawRotarySlider(juce::Graphics& g, int x, int y, int width, int height,
                                        float sliderPos, float rotaryStartAngle,
                                        float rotaryEndAngle, juce::Slider& slider) {
    const auto bounds = juce::Rectangle<int>(x, y, width, height).toFloat();
    const float diameter = juce::jmin(bounds.getWidth(), bounds.getHeight());
    const auto outer = juce::Rectangle<float>(diameter, diameter).withCentre(bounds.getCentre());
    const float cx = outer.getCentreX(), cy = outer.getCentreY();
    const float radius = diameter * 0.5f;
    const float angle = rotaryStartAngle + sliderPos * (rotaryEndAngle - rotaryStartAngle);
    const bool  on    = slider.isEnabled();
    const bool  hot   = slider.isMouseOverOrDragging();

    // --- style ------------------------------------------------------------
    //
    // These four numbers are what make a teal utilitarian instrument and a warm
    // Jamaican one genuinely different objects rather than the same geometry in
    // two palettes.
    const bool  sharp   = style_ == Style::Sharp;
    const bool  soft    = style_ == Style::Soft;
    const bool  vintage = style_ == Style::Vintage;
    const bool  minimal = style_ == Style::Minimal;

    const bool  wantTicks  = sharp || vintage;
    const float ringScale  = soft ? 1.55f : (minimal ? 0.55f : 1.0f);
    const float capScale   = soft ? 1.10f : (minimal ? 0.80f : (vintage ? 1.06f : 1.0f));
    const float pointerW   = soft ? 1.45f : (minimal ? 0.60f : 1.0f);

    const float ring      = juce::jmax(1.6f, diameter * 0.052f) * ringScale;
    const float arcRadius = radius - ring * 0.5f - 0.5f;
    const float bodyR     = (radius - ring * 2.3f) * capScale;
    const auto  body      = juce::Rectangle<float>(bodyR * 2.0f, bodyR * 2.0f).withCentre({cx, cy});

    const auto tint = slider.isColourSpecified(juce::Slider::rotarySliderFillColourId)
                          ? slider.findColour(juce::Slider::rotarySliderFillColourId)
                          : accent_;

    // --- tick marks --------------------------------------------------------
    // The thing every hardware synth has and every quick software knob skips.
    // They give the eye a scale to read the pointer against, so a value is
    // legible at a glance instead of needing the number underneath.
    if (wantTicks && diameter > 34.0f) {
        const int   ticks   = 11;
        const float tickIn  = radius * 0.99f;
        const float tickOut = radius * 1.10f;
        for (int i = 0; i < ticks; ++i) {
            const float t  = static_cast<float>(i) / (ticks - 1);
            const float a  = rotaryStartAngle + t * (rotaryEndAngle - rotaryStartAngle);
            const float sn = std::sin(a), cs = -std::cos(a);
            const bool  major = (i == 0 || i == ticks - 1 || i == ticks / 2);
            g.setColour(outline().withAlpha(major ? 0.85f : 0.42f));
            g.drawLine(cx + sn * tickIn, cy + cs * tickIn,
                       cx + sn * (major ? tickOut : tickOut - 1.5f),
                       cy + cs * (major ? tickOut : tickOut - 1.5f),
                       major ? 1.3f : 0.9f);
        }
    }

    // --- recessed track ----------------------------------------------------
    juce::Path track;
    track.addCentredArc(cx, cy, arcRadius, arcRadius, 0.0f,
                        rotaryStartAngle, rotaryEndAngle, true);
    g.setColour(juce::Colour(0xff0e0e12));
    g.strokePath(track, juce::PathStrokeType(ring, juce::PathStrokeType::curved,
                                             juce::PathStrokeType::rounded));

    // --- value arc ---------------------------------------------------------
    if (sliderPos > 0.003f) {
        juce::Path value;
        value.addCentredArc(cx, cy, arcRadius, arcRadius, 0.0f, rotaryStartAngle, angle, true);
        const auto lit = on ? tint.withMultipliedSaturation(hot ? 0.95f : 0.68f)
                                  .withMultipliedBrightness(hot ? 1.22f : 1.05f)
                            : outlineBright();
        // A soft bloom under the arc when the knob is live. Subtle - one pass,
        // low alpha - because a glow on every knob is a toy, and a glow on the
        // one you are touching is a product.
        if (hot && on) {
            g.setColour(lit.withAlpha(0.22f));
            g.strokePath(value, juce::PathStrokeType(ring * 2.4f, juce::PathStrokeType::curved,
                                                     juce::PathStrokeType::rounded));
        }
        g.setColour(lit);
        g.strokePath(value, juce::PathStrokeType(ring, juce::PathStrokeType::curved,
                                                 juce::PathStrokeType::rounded));
    }

    // --- cap ---------------------------------------------------------------
    // Contact shadow first, so the cap sits in the panel rather than on it.
    g.setColour(juce::Colours::black.withAlpha(0.35f));
    g.fillEllipse(body.translated(0.0f, juce::jmax(1.0f, bodyR * 0.07f)).expanded(0.8f));

    const juce::Colour capTop = vintage ? juce::Colour(0xffd9cdb4)
                              : soft    ? juce::Colour(0xff4a4038)
                                        : juce::Colour(0xff40444f);
    const juce::Colour capBot = vintage ? juce::Colour(0xffa8927a)
                              : soft    ? juce::Colour(0xff261f1a)
                                        : juce::Colour(0xff20232a);
    juce::ColourGradient cap(capTop, cx, cy - bodyR, capBot, cx, cy + bodyR, false);
    cap.addColour(0.55, capTop.interpolatedWith(capBot, 0.55f));
    g.setGradientFill(cap);
    g.fillEllipse(body);

    g.setColour(juce::Colour(0xff0b0b0f));
    g.drawEllipse(body.reduced(0.5f), 1.0f);
    g.setColour(juce::Colours::white.withAlpha(hot ? 0.13f : 0.07f));
    g.drawEllipse(body.reduced(1.4f), 1.0f);

    // --- pointer -----------------------------------------------------------
    const float sn = std::sin(angle), cs = -std::cos(angle);
    const float tipR  = bodyR * 0.82f;
    const float baseR = bodyR * 0.20f;
    const float w     = juce::jmax(1.8f, bodyR * 0.14f) * pointerW;

    g.setColour(juce::Colours::black.withAlpha(0.45f));
    g.drawLine(cx + sn * baseR, cy + cs * baseR + 1.0f,
               cx + sn * tipR,  cy + cs * tipR + 1.0f, w);
    g.setColour(!on ? textDim()
                    : vintage ? juce::Colour(0xff20180f)
                              : (hot ? juce::Colours::white : juce::Colour(0xffe6e8ee)));
    g.drawLine(cx + sn * baseR, cy + cs * baseR, cx + sn * tipR, cy + cs * tipR, w);
}

void ForgeLookAndFeel::drawLinearSlider(juce::Graphics& g, int x, int y, int width, int height,
                                        float sliderPos, float, float,
                                        juce::Slider::SliderStyle style, juce::Slider& slider) {
    const auto bounds = juce::Rectangle<int>(x, y, width, height).toFloat();
    const bool vertical = style == juce::Slider::LinearVertical;

    auto track = vertical ? bounds.withSizeKeepingCentre(4.0f, bounds.getHeight())
                          : bounds.withSizeKeepingCentre(bounds.getWidth(), 4.0f);
    g.setColour(outline());
    g.fillRoundedRectangle(track, 2.0f);

    auto filled = track;
    if (vertical) filled = filled.withTop(sliderPos);
    else          filled = filled.withRight(sliderPos);
    g.setColour(slider.isEnabled() ? accent_ : outlineBright());
    g.fillRoundedRectangle(filled, 2.0f);

    const auto centre = vertical ? juce::Point<float>(bounds.getCentreX(), sliderPos)
                                 : juce::Point<float>(sliderPos, bounds.getCentreY());
    g.setColour(textPrimary());
    g.fillRoundedRectangle(juce::Rectangle<float>(vertical ? 14.0f : 5.0f,
                                                  vertical ? 5.0f : 14.0f).withCentre(centre), 2.0f);
}

// --- buttons ---------------------------------------------------------------

void ForgeLookAndFeel::drawButtonBackground(juce::Graphics& g, juce::Button& button,
                                            const juce::Colour&, bool highlighted, bool down) {
    auto bounds = button.getLocalBounds().toFloat().reduced(0.5f);
    const bool on = button.getToggleState();

    g.setColour(on ? accent_.withAlpha(0.16f)
                   : (down ? panelRaised().brighter(0.10f)
                           : (highlighted ? panelRaised().brighter(0.05f) : panelRaised())));
    g.fillRoundedRectangle(bounds, 3.0f);
    g.setColour(on ? accent_.withAlpha(0.7f) : (highlighted ? outlineBright() : outline()));
    g.drawRoundedRectangle(bounds, 3.0f, 1.0f);
}

void ForgeLookAndFeel::drawButtonText(juce::Graphics& g, juce::TextButton& button,
                                      bool highlighted, bool) {
    g.setFont(fonts::get(11.0f, fonts::Weight::Medium));
    const bool on = button.getToggleState();
    g.setColour(!button.isEnabled() ? textDim()
                                    : (on || highlighted ? textPrimary() : textSecondary()));
    g.drawFittedText(button.getButtonText(), button.getLocalBounds(),
                     juce::Justification::centred, 1);
}

// --- combo boxes -----------------------------------------------------------

void ForgeLookAndFeel::drawComboBox(juce::Graphics& g, int width, int height, bool,
                                    int, int, int, int, juce::ComboBox& box) {
    auto bounds = juce::Rectangle<int>(0, 0, width, height).toFloat().reduced(0.5f);
    g.setColour(panelRaised());
    g.fillRoundedRectangle(bounds, 3.0f);
    g.setColour(box.isMouseOver() ? outlineBright() : outline());
    g.drawRoundedRectangle(bounds, 3.0f, 1.0f);

    juce::Path arrow;
    const float cx = static_cast<float>(width) - 12.0f;
    const float cy = static_cast<float>(height) * 0.5f;
    arrow.startNewSubPath(cx - 3.5f, cy - 1.5f);
    arrow.lineTo(cx, cy + 2.5f);
    arrow.lineTo(cx + 3.5f, cy - 1.5f);
    g.setColour(textSecondary());
    g.strokePath(arrow, juce::PathStrokeType(1.4f, juce::PathStrokeType::curved,
                                             juce::PathStrokeType::rounded));
}

void ForgeLookAndFeel::positionComboBoxText(juce::ComboBox& box, juce::Label& label) {
    label.setBounds(9, 0, box.getWidth() - 26, box.getHeight());
    label.setFont(getComboBoxFont(box));
}

// --- popup menus (the UI leans on these heavily) ---------------------------

void ForgeLookAndFeel::drawPopupMenuBackground(juce::Graphics& g, int width, int height) {
    const auto bounds = juce::Rectangle<int>(0, 0, width, height).toFloat().reduced(0.5f);
    g.setColour(panelRaised());
    g.fillRoundedRectangle(bounds, 4.0f);
    g.setColour(outlineBright());
    g.drawRoundedRectangle(bounds, 4.0f, 1.0f);
}

void ForgeLookAndFeel::drawPopupMenuItem(juce::Graphics& g, const juce::Rectangle<int>& area,
                                         bool isSeparator, bool isActive, bool isHighlighted,
                                         bool isTicked, bool hasSubMenu, const juce::String& text,
                                         const juce::String& shortcutKeyText,
                                         const juce::Drawable*, const juce::Colour*) {
    if (isSeparator) {
        g.setColour(outline());
        g.fillRect(area.reduced(8, 0).withHeight(1).withY(area.getCentreY()));
        return;
    }

    auto r = area.reduced(3, 1);
    if (isHighlighted && isActive) {
        g.setColour(accent_.withAlpha(0.18f));
        g.fillRoundedRectangle(r.toFloat(), 3.0f);
    }

    g.setColour(!isActive ? textDim() : (isHighlighted ? textPrimary() : textSecondary()));
    g.setFont(getPopupMenuFont());

    auto textArea = r.reduced(10, 0);
    if (isTicked) {
        g.setColour(accent_);
        g.fillEllipse(juce::Rectangle<float>(4.0f, 4.0f)
                          .withCentre({static_cast<float>(r.getX()) + 6.0f,
                                       static_cast<float>(r.getCentreY())}));
        g.setColour(textPrimary());
    }
    g.drawFittedText(text, textArea, juce::Justification::centredLeft, 1);

    if (hasSubMenu) {
        juce::Path arrow;
        const float cx = static_cast<float>(r.getRight()) - 10.0f;
        const float cy = static_cast<float>(r.getCentreY());
        arrow.startNewSubPath(cx - 2.0f, cy - 3.5f);
        arrow.lineTo(cx + 2.0f, cy);
        arrow.lineTo(cx - 2.0f, cy + 3.5f);
        g.setColour(textSecondary());
        g.strokePath(arrow, juce::PathStrokeType(1.3f));
    } else if (shortcutKeyText.isNotEmpty()) {
        g.setColour(textDim());
        g.drawFittedText(shortcutKeyText, textArea, juce::Justification::centredRight, 1);
    }
}

void ForgeLookAndFeel::getIdealPopupMenuItemSize(const juce::String& text, bool isSeparator,
                                                 int, int& idealWidth, int& idealHeight) {
    if (isSeparator) {
        idealWidth = 60;
        idealHeight = 9;
        return;
    }
    const auto font = getPopupMenuFont();
    idealWidth  = juce::GlyphArrangement::getStringWidthInt(font, text) + 44;
    idealHeight = 23;
}

// --- text editors ----------------------------------------------------------

void ForgeLookAndFeel::fillTextEditorBackground(juce::Graphics& g, int width, int height,
                                                juce::TextEditor&) {
    g.setColour(panelRaised());
    g.fillRoundedRectangle(juce::Rectangle<int>(0, 0, width, height).toFloat().reduced(0.5f), 3.0f);
}

void ForgeLookAndFeel::drawTextEditorOutline(juce::Graphics& g, int width, int height,
                                             juce::TextEditor& editor) {
    g.setColour(editor.hasKeyboardFocus(false) ? accent_.withAlpha(0.8f) : outline());
    g.drawRoundedRectangle(juce::Rectangle<int>(0, 0, width, height).toFloat().reduced(0.5f),
                           3.0f, 1.0f);
}

// --- fonts -----------------------------------------------------------------

juce::Font ForgeLookAndFeel::getLabelFont(juce::Label& label) {
    return fonts::get(juce::jlimit(9.0f, 14.0f, static_cast<float>(label.getHeight()) * 0.62f));
}
juce::Font ForgeLookAndFeel::getComboBoxFont(juce::ComboBox&)            { return fonts::bodyMedium(); }
juce::Font ForgeLookAndFeel::getTextButtonFont(juce::TextButton&, int)   { return fonts::get(11.0f, fonts::Weight::Medium); }
juce::Font ForgeLookAndFeel::getPopupMenuFont()                          { return fonts::get(12.0f); }

} // namespace forge
