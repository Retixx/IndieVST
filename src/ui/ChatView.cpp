#include "ui/ChatView.h"

#include "ui/Fonts.h"
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

const char* kQuickStarts[] = { "warm analog bass", "glassy pad", "metallic pluck", "FM bell" };

} // namespace

ChatView::ChatView(ForgeAudioProcessor& processor) : processor_(processor) {
    prompt_.setMultiLine(false, false);
    prompt_.setReturnKeyStartsNewLine(false);
    prompt_.setJustification(juce::Justification::centredLeft);
    prompt_.setTextToShowWhenEmpty(kPlaceholders[juce::Random::getSystemRandom().nextInt(5)],
                                   ForgeLookAndFeel::textDim());
    prompt_.setFont(fonts::prompt());
    prompt_.setIndents(10, 0);
    prompt_.onReturnKey = [this] { submit(); };
    addAndMakeVisible(prompt_);

    sendButton_.onClick = [this] { submit(); };
    addAndMakeVisible(sendButton_);

    cancelButton_.onClick = [this] { processor_.cancelGeneration(); };
    addChildComponent(cancelButton_);

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
    processor_.generate(text, processor_.chatEditsCurrent());
}

void ChatView::refresh() {
    const bool busy = processor_.isGenerating();
    sendButton_.setVisible(!busy);
    cancelButton_.setVisible(busy);
    prompt_.setEnabled(!busy);
    for (auto* b : suggestions_) b->setEnabled(!busy);

    // Echo only the latest exchange, as one line.
    resultText_.clear();
    const auto& log = processor_.chatLog();
    for (auto it = log.rbegin(); it != log.rend(); ++it) {
        if (!it->fromUser) { resultText_ = it->text; break; }
    }
    if (!busy) progressText_ = processor_.lastStatus();

    resized();
    repaint();
}

void ChatView::setProgress(const juce::String& text) {
    progressText_ = text;
    repaint();
}

void ChatView::focusPrompt() { prompt_.grabKeyboardFocus(); }

void ChatView::timerCallback() {
    if (processor_.isGenerating()) {
        spinnerPhase_ = (spinnerPhase_ + 1) % 4;
        repaint();
    }
}

void ChatView::paint(juce::Graphics& g) {
    // Everything positions off the input field rather than off fractions of
    // the panel, so nothing can land on top of it at any window size.
    const int fieldTop = prompt_.getY();

    const juce::Rectangle<int> headline(0, fieldTop - 62, getWidth(), 26);
    g.setFont(fonts::get(19.0f, fonts::Weight::SemiBold));
    g.setColour(ForgeLookAndFeel::textPrimary());
    g.drawFittedText("Describe an instrument.", headline, juce::Justification::centred, 1);

    const juce::Rectangle<int> subtitle(0, fieldTop - 34, getWidth(), 16);
    g.setFont(fonts::caption());
    g.setColour(ForgeLookAndFeel::textDim());
    g.drawFittedText("Forge picks the modules, wires the signal path and lays out the controls.",
                     subtitle, juce::Justification::centred, 1);

    // Status line under the input: progress while working, result when done.
    const bool busy = processor_.isGenerating();
    const auto strip = juce::Rectangle<int>(0, prompt_.getBottom() + 9, getWidth(), 16);

    if (busy) {
        g.setFont(fonts::caption());
        g.setColour(accent_);
        const auto base = progressText_.upToFirstOccurrenceOf("...", false, false);
        g.drawFittedText(base + juce::String::repeatedString(".", spinnerPhase_),
                         strip, juce::Justification::centred, 1);
    } else if (resultText_.isNotEmpty()) {
        g.setFont(fonts::caption());
        g.setColour(ForgeLookAndFeel::textSecondary());
        g.drawFittedText(resultText_, strip.reduced(30, 0), juce::Justification::centred, 1);
    }
}

void ChatView::resized() {
    auto bounds = getLocalBounds();
    const int centreY = bounds.getCentreY();
    const int fieldW  = juce::jmin(560, bounds.getWidth() - 80);
    const int fieldX  = bounds.getCentreX() - fieldW / 2;

    prompt_.setBounds(fieldX, centreY - 34, fieldW - 104, 32);
    sendButton_.setBounds(prompt_.getRight() + 6, centreY - 34, 98, 32);
    cancelButton_.setBounds(sendButton_.getBounds());

    // Quick-start chips, evenly spaced under the status line.
    const int chipY = prompt_.getBottom() + 33;
    const int count = suggestions_.size();
    if (count > 0) {
        const int chipW = juce::jmin(130, (fieldW - (count - 1) * 6) / count);
        int x = bounds.getCentreX() - (chipW * count + 6 * (count - 1)) / 2;
        for (auto* b : suggestions_) {
            b->setBounds(x, chipY, chipW, 24);
            x += chipW + 6;
        }
    }
}

} // namespace forge
