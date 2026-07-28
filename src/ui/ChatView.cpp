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

    clearReference_.onClick = [this] {
        processor_.clearReference();
        referenceError_ = {};
        repaint();
    };
    clearReference_.setTooltip("Remove the reference recording");
    addChildComponent(clearReference_);

    refresh();
    startTimerHz(8);
}

// --- reference audio -------------------------------------------------------

juce::Rectangle<int> ChatView::referenceSlotBounds() const {
    const int w = juce::jmin(560, getWidth() - 80);
    const int x = getWidth() / 2 - w / 2;
    // Directly under the quick-start chips, so the eye reads
    // prompt -> suggestions -> "or drop a sound in".
    return {x, getHeight() / 2 + 42, w, 40};
}

bool ChatView::isInterestedInFileDrag(const juce::StringArray& files) {
    const auto filter = ForgeAudioProcessor::referenceFileFilter();
    for (const auto& f : files)
        if (juce::File(f).getFileName().matchesWildcard(filter, true)) return true;
    return false;
}

void ChatView::fileDragEnter(const juce::StringArray&, int, int) {
    dragHover_ = true;
    repaint();
}

void ChatView::fileDragExit(const juce::StringArray&) {
    dragHover_ = false;
    repaint();
}

void ChatView::filesDropped(const juce::StringArray& files, int, int) {
    dragHover_ = false;
    const auto filter = ForgeAudioProcessor::referenceFileFilter();
    for (const auto& f : files) {
        const juce::File file(f);
        if (file.getFileName().matchesWildcard(filter, true)) { applyReference(file); return; }
    }
    referenceError_ = "That is not an audio file Forge can read.";
    repaint();
}

void ChatView::applyReference(const juce::File& file) {
    juce::String error;
    if (!processor_.loadReference(file, error)) referenceError_ = error;
    else                                        referenceError_ = {};
    repaint();
}

void ChatView::chooseReference() {
    chooser_ = std::make_unique<juce::FileChooser>(
        "Choose a reference recording",
        juce::File::getSpecialLocation(juce::File::userMusicDirectory),
        ForgeAudioProcessor::referenceFileFilter());

    chooser_->launchAsync(juce::FileBrowserComponent::openMode
                              | juce::FileBrowserComponent::canSelectFiles,
                          [this](const juce::FileChooser& fc) {
                              const auto file = fc.getResult();
                              if (file.existsAsFile()) applyReference(file);
                          });
}

void ChatView::mouseDown(const juce::MouseEvent& event) {
    if (referenceSlotBounds().contains(event.getPosition()) && !processor_.hasReference())
        chooseReference();
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
    g.drawFittedText("IndieVST picks the modules, wires the signal path and lays out the controls.",
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

    paintReferenceSlot(g);
}

void ChatView::paintReferenceSlot(juce::Graphics& g) const {
    const auto slot = referenceSlotBounds().toFloat();
    const bool have = processor_.hasReference();

    // Dashed while empty, solid once it holds something. An empty dashed box
    // reads as "put something here" without needing a sentence to say so.
    g.setColour(have ? ForgeLookAndFeel::panelRaised()
                     : ForgeLookAndFeel::panel().withAlpha(dragHover_ ? 0.9f : 0.45f));
    g.fillRoundedRectangle(slot, 4.0f);

    const auto edge = dragHover_ ? accent_
                    : have       ? accent_.withAlpha(0.55f)
                                 : ForgeLookAndFeel::outline();
    g.setColour(edge);
    if (have || dragHover_) {
        g.drawRoundedRectangle(slot.reduced(0.5f), 4.0f, dragHover_ ? 1.6f : 1.0f);
    } else {
        juce::Path outline, dashed;
        outline.addRoundedRectangle(slot.reduced(0.5f), 4.0f);
        const float pattern[] = {4.0f, 3.0f};
        juce::PathStrokeType(1.0f).createDashedStroke(dashed, outline, pattern, 2);
        g.strokePath(dashed, juce::PathStrokeType(1.0f));
    }

    auto text = slot.toNearestInt().reduced(12, 0);
    if (processor_.hasReference()) text.removeFromRight(22);   // room for the x

    if (referenceError_.isNotEmpty() && !have) {
        g.setFont(fonts::caption());
        g.setColour(juce::Colour(0xffd9705a));
        g.drawFittedText(referenceError_, text, juce::Justification::centred, 2);
        return;
    }

    if (!have) {
        g.setFont(fonts::caption());
        g.setColour(ForgeLookAndFeel::textDim());
        g.drawFittedText(dragHover_ ? "Drop it here"
                                    : "Optional: drop a reference recording, or click to browse",
                         text, juce::Justification::centred, 1);
        return;
    }

    // Loaded: name on the left, what Forge actually heard on the right. Showing
    // the reading back matters - it is the difference between the musician
    // trusting the feature and wondering whether anything happened.
    const auto name = processor_.referenceName();
    auto left = text.removeFromLeft(juce::jmin(180, text.getWidth() / 3));
    g.setFont(fonts::get(11.0f, fonts::Weight::Medium));
    g.setColour(ForgeLookAndFeel::textPrimary());
    g.drawFittedText(name, left, juce::Justification::centredLeft, 1);

    g.setFont(fonts::caption());
    g.setColour(ForgeLookAndFeel::textSecondary());
    g.drawFittedText(juce::String(processor_.reference().toSummaryLine()),
                     text, juce::Justification::centredLeft, 2);
}

void ChatView::resized() {
    auto bounds = getLocalBounds();
    const int centreY = bounds.getCentreY();
    const int fieldW  = juce::jmin(560, bounds.getWidth() - 80);
    const int fieldX  = bounds.getCentreX() - fieldW / 2;

    prompt_.setBounds(fieldX, centreY - 34, fieldW - 104, 32);
    sendButton_.setBounds(prompt_.getRight() + 6, centreY - 34, 98, 32);
    cancelButton_.setBounds(sendButton_.getBounds());

    const auto slot = referenceSlotBounds();
    clearReference_.setBounds(slot.getRight() - 26, slot.getY() + 10, 20, 20);
    clearReference_.setVisible(processor_.hasReference());

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
