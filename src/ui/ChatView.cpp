#include "ui/ChatView.h"

#include "ui/ForgeLookAndFeel.h"

namespace forge {
namespace {

const char* kPlaceholders[] = {
    "a warm analog bass with a slow filter sweep",
    "a glassy bell that decays into noise",
    "a wide evolving pad, cold and drifting",
    "a metallic pluck for arpeggios",
    "a mono acid lead with vibrato on the mod wheel",
};

const char* kQuickStarts[] = {
    "warm analog bass",
    "glassy pad",
    "metallic pluck",
};

} // namespace

ChatView::ChatView(ForgeAudioProcessor& processor) : processor_(processor) {
    transcript_.setMultiLine(true, true);
    transcript_.setReadOnly(true);
    transcript_.setScrollbarsShown(true);
    transcript_.setCaretVisible(false);
    transcript_.setPopupMenuEnabled(false);
    transcript_.setColour(juce::TextEditor::outlineColourId, juce::Colours::transparentBlack);
    transcript_.setColour(juce::TextEditor::backgroundColourId, ForgeLookAndFeel::panel());
    transcript_.setFont(juce::Font(juce::FontOptions(14.0f)));
    addAndMakeVisible(transcript_);

    prompt_.setMultiLine(false, false);
    prompt_.setReturnKeyStartsNewLine(false);
    prompt_.setTextToShowWhenEmpty(kPlaceholders[juce::Random::getSystemRandom().nextInt(5)],
                                   ForgeLookAndFeel::textSecondary());
    prompt_.setFont(juce::Font(juce::FontOptions(15.0f)));
    prompt_.onReturnKey = [this] { submit(); };
    addAndMakeVisible(prompt_);

    sendButton_.onClick = [this] { submit(); };
    addAndMakeVisible(sendButton_);

    cancelButton_.onClick = [this] { processor_.cancelGeneration(); };
    cancelButton_.setVisible(false);
    addChildComponent(cancelButton_);

    statusLabel_.setJustificationType(juce::Justification::centredLeft);
    statusLabel_.setColour(juce::Label::textColourId, ForgeLookAndFeel::textSecondary());
    addAndMakeVisible(statusLabel_);

    for (const char* quick : kQuickStarts) {
        auto* b = new juce::TextButton(quick);
        b->onClick = [this, quick] {
            prompt_.setText(quick, juce::dontSendNotification);
            submit();
        };
        suggestions_.add(b);
        addAndMakeVisible(b);
    }

    refresh();
    startTimerHz(8);
}

ChatView::~ChatView() { stopTimer(); }

void ChatView::submit() {
    const auto text = prompt_.getText().trim();
    if (text.isEmpty() || processor_.isGenerating()) return;
    prompt_.clear();
    // Editing applies only when the user arrived here via "Edit with chat".
    // Choosing "Create New Instrument" always starts from scratch.
    processor_.generate(text, processor_.chatEditsCurrent());
}

void ChatView::refresh() {
    juce::String text;
    for (const auto& turn : processor_.chatLog()) {
        text << (turn.fromUser ? "You:  " : "Forge:  ") << turn.text << "\n\n";
    }
    if (text.isEmpty())
        text = "Describe an instrument and Forge will build it.\n\n"
               "It picks the modules, wires the signal path, designs the modulation "
               "and lays out the controls. Then you can keep talking to it.\n";

    transcript_.setText(text, false);
    transcript_.moveCaretToEnd();

    const bool busy = processor_.isGenerating();
    sendButton_.setEnabled(!busy);
    cancelButton_.setVisible(busy);
    prompt_.setEnabled(!busy);
    for (auto* b : suggestions_) b->setEnabled(!busy);

    if (!busy) progressText_ = processor_.lastStatus();
    statusLabel_.setText(progressText_, juce::dontSendNotification);
}

void ChatView::setProgress(const juce::String& text) {
    progressText_ = text;
    statusLabel_.setText(text, juce::dontSendNotification);
    repaint();
}

void ChatView::focusPrompt() { prompt_.grabKeyboardFocus(); }

void ChatView::timerCallback() {
    if (!processor_.isGenerating()) return;
    spinnerPhase_ = (spinnerPhase_ + 1) % 4;
    const juce::String dots = juce::String::repeatedString(".", spinnerPhase_);
    statusLabel_.setText(progressText_.upToFirstOccurrenceOf("...", false, false) + dots,
                         juce::dontSendNotification);
}

void ChatView::paint(juce::Graphics& g) {
    g.setColour(ForgeLookAndFeel::panel());
    g.fillRoundedRectangle(getLocalBounds().toFloat().reduced(1.0f), 8.0f);
}

void ChatView::resized() {
    auto bounds = getLocalBounds().reduced(14);

    auto row = bounds.removeFromBottom(34);
    if (cancelButton_.isVisible()) {
        cancelButton_.setBounds(row.removeFromRight(90).reduced(2));
        row.removeFromRight(4);
    }
    sendButton_.setBounds(row.removeFromRight(110).reduced(2));
    row.removeFromRight(6);
    prompt_.setBounds(row.reduced(0, 2));

    bounds.removeFromBottom(6);
    statusLabel_.setBounds(bounds.removeFromBottom(20));

    bounds.removeFromBottom(6);
    auto quickRow = bounds.removeFromBottom(28);
    const int each = suggestions_.isEmpty() ? 0 : quickRow.getWidth() / suggestions_.size();
    for (auto* b : suggestions_) b->setBounds(quickRow.removeFromLeft(each).reduced(3, 2));

    bounds.removeFromBottom(8);
    transcript_.setBounds(bounds);
}

} // namespace forge
