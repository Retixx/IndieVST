#pragma once

#include "plugin/PluginProcessor.h"

#include <juce_gui_basics/juce_gui_basics.h>

namespace forge {

/// The screen the plugin opens on: describe an instrument, get an instrument.
///
/// Deliberately shows no conversation history. A scrolling transcript made the
/// panel feel like a chat app rather than an instrument, and on a second prompt
/// the thing you care about is the box you are typing into, not what you asked
/// for four instruments ago. Only the most recent result is echoed, on one line.
class ChatView final : public juce::Component,
                       public  juce::FileDragAndDropTarget,
                       private juce::Timer {
public:
    explicit ChatView(ForgeAudioProcessor&);
    ~ChatView() override;

    void resized() override;
    void paint(juce::Graphics&) override;

    void refresh();
    void setProgress(const juce::String&);
    void focusPrompt();
    void setAccent(juce::Colour accent) { accent_ = accent; repaint(); }

private:
    void submit();
    void timerCallback() override;

    // --- reference audio ---------------------------------------------------
    // Describing a sound is hard; playing one is easy. Dropping a recording
    // here has Forge measure it and design against the measurements.
    bool isInterestedInFileDrag(const juce::StringArray&) override;
    void fileDragEnter(const juce::StringArray&, int, int) override;
    void fileDragExit(const juce::StringArray&) override;
    void filesDropped(const juce::StringArray&, int, int) override;
    void chooseReference();
    void applyReference(const juce::File&);
    void paintReferenceSlot(juce::Graphics&) const;
    juce::Rectangle<int> referenceSlotBounds() const;
    void mouseDown(const juce::MouseEvent&) override;

    ForgeAudioProcessor& processor_;

    juce::TextEditor  prompt_;
    juce::TextButton  sendButton_{"Generate"};
    juce::TextButton  cancelButton_{"Cancel"};
    juce::OwnedArray<juce::TextButton> suggestions_;

    juce::TextButton     clearReference_{"x"};
    bool                 dragHover_ = false;
    juce::String         referenceError_;
    std::unique_ptr<juce::FileChooser> chooser_;

    juce::String progressText_, resultText_;
    juce::Colour accent_{0xffe4572e};
    int          spinnerPhase_ = 0;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(ChatView)
};

} // namespace forge
