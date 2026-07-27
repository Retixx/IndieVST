#include "ui/ForgeLookAndFeel.h"

#include "ui/Fonts.h"

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

void ForgeLookAndFeel::drawRotarySlider(juce::Graphics& g, int x, int y, int width, int height,
                                        float sliderPos, float rotaryStartAngle,
                                        float rotaryEndAngle, juce::Slider& slider) {
    const auto bounds = juce::Rectangle<int>(x, y, width, height).toFloat();
    const float diameter = juce::jmin(bounds.getWidth(), bounds.getHeight());
    const auto circle = juce::Rectangle<float>(diameter, diameter).withCentre(bounds.getCentre());
    const float radius = diameter * 0.5f;
    const float cx = circle.getCentreX(), cy = circle.getCentreY();
    const float ring = juce::jmax(2.0f, diameter * 0.085f);
    const float arcRadius = radius - ring * 0.5f - 0.5f;
    const float angle = rotaryStartAngle + sliderPos * (rotaryEndAngle - rotaryStartAngle);

    // Body: a barely-there disc so the knob has weight against the black.
    g.setColour(panelRaised().brighter(0.06f));
    g.fillEllipse(circle.reduced(ring * 0.9f));
    g.setColour(outline());
    g.drawEllipse(circle.reduced(ring * 0.9f), 1.0f);

    juce::Path track;
    track.addCentredArc(cx, cy, arcRadius, arcRadius, 0.0f,
                        rotaryStartAngle, rotaryEndAngle, true);
    g.setColour(outline());
    g.strokePath(track, juce::PathStrokeType(ring, juce::PathStrokeType::curved,
                                             juce::PathStrokeType::butt));

    if (sliderPos > 0.002f) {
        juce::Path value;
        value.addCentredArc(cx, cy, arcRadius, arcRadius, 0.0f, rotaryStartAngle, angle, true);
        g.setColour(slider.isEnabled() ? accent_ : outlineBright());
        g.strokePath(value, juce::PathStrokeType(ring, juce::PathStrokeType::curved,
                                                 juce::PathStrokeType::butt));
    }

    juce::Path pointer;
    const float w = juce::jmax(1.5f, ring * 0.45f);
    pointer.addRoundedRectangle(-w * 0.5f, -arcRadius + ring * 0.6f,
                                w, radius * 0.42f, w * 0.5f);
    pointer.applyTransform(juce::AffineTransform::rotation(angle).translated(cx, cy));
    g.setColour(textPrimary());
    g.fillPath(pointer);
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
    idealWidth  = juce::roundToInt(font.getStringWidthFloat(text)) + 44;
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
