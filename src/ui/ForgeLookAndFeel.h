#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

#include <string>

namespace forge {

/// Near-black chrome with grey structure and white text, so the accent colour
/// the instrument chose is the only saturated thing on screen. This is how
/// plugin UIs read as instruments rather than as applications - the panel
/// recedes and the controls come forward.
class ForgeLookAndFeel final : public juce::LookAndFeel_V4 {
public:
    ForgeLookAndFeel();

    void setAccent(juce::Colour accent);
    juce::Colour accent() const { return accent_; }

    /// The instrument's visual language, chosen by the model.
    ///
    /// Colour alone is not enough. A cold technical instrument and a warm
    /// groovy one rendered with identical geometry read as the same product in
    /// two palettes - which is exactly the complaint this answers. Style
    /// changes the SHAPES: how blunt the pointer is, whether there are tick
    /// marks at all, how thick the arc is, how much chrome sits around it.
    enum class Style { Sharp, Soft, Vintage, Minimal };
    void  setStyle(Style s) { style_ = s; }
    Style style() const { return style_; }
    static Style styleFromString(const std::string& name) {
        if (name == "soft")    return Style::Soft;
        if (name == "vintage") return Style::Vintage;
        if (name == "minimal") return Style::Minimal;
        return Style::Sharp;
    }

    // Near-black base, greys for structure, near-white for content.
    static juce::Colour background()    { return juce::Colour(0xff0a0a0c); }
    static juce::Colour panel()         { return juce::Colour(0xff121216); }
    static juce::Colour panelRaised()   { return juce::Colour(0xff17171c); }
    static juce::Colour outline()       { return juce::Colour(0xff26262e); }
    static juce::Colour outlineBright() { return juce::Colour(0xff3a3a45); }
    static juce::Colour textPrimary()   { return juce::Colour(0xfff0f0f3); }
    static juce::Colour textSecondary() { return juce::Colour(0xff85858f); }
    static juce::Colour textDim()       { return juce::Colour(0xff55555e); }

    void drawRotarySlider(juce::Graphics&, int x, int y, int width, int height,
                          float sliderPos, float rotaryStartAngle, float rotaryEndAngle,
                          juce::Slider&) override;

    void drawLinearSlider(juce::Graphics&, int x, int y, int width, int height,
                          float sliderPos, float minSliderPos, float maxSliderPos,
                          juce::Slider::SliderStyle, juce::Slider&) override;

    void drawButtonBackground(juce::Graphics&, juce::Button&, const juce::Colour&,
                              bool highlighted, bool down) override;
    void drawButtonText(juce::Graphics&, juce::TextButton&, bool, bool) override;

    void drawComboBox(juce::Graphics&, int width, int height, bool isButtonDown,
                      int buttonX, int buttonY, int buttonW, int buttonH,
                      juce::ComboBox&) override;
    void positionComboBoxText(juce::ComboBox&, juce::Label&) override;

    void drawPopupMenuBackground(juce::Graphics&, int width, int height) override;
    void drawPopupMenuItem(juce::Graphics&, const juce::Rectangle<int>& area,
                           bool isSeparator, bool isActive, bool isHighlighted,
                           bool isTicked, bool hasSubMenu, const juce::String& text,
                           const juce::String& shortcutKeyText,
                           const juce::Drawable* icon, const juce::Colour* textColour) override;
    void getIdealPopupMenuItemSize(const juce::String& text, bool isSeparator,
                                   int standardMenuItemHeight, int& idealWidth,
                                   int& idealHeight) override;

    juce::Font getLabelFont(juce::Label&) override;
    juce::Font getComboBoxFont(juce::ComboBox&) override;
    juce::Font getTextButtonFont(juce::TextButton&, int buttonHeight) override;
    juce::Font getPopupMenuFont() override;

    void drawTextEditorOutline(juce::Graphics&, int width, int height, juce::TextEditor&) override;
    void fillTextEditorBackground(juce::Graphics&, int width, int height, juce::TextEditor&) override;

private:
    Style        style_ = Style::Sharp;
    juce::Colour accent_{0xffe4572e};
};

} // namespace forge
