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

    ForgeAudioProcessor& processor_;

    juce::TextEditor  prompt_;
    juce::TextButton  sendButton_{"Generate"};
    juce::TextButton  cancelButton_{"Cancel"};
    juce::OwnedArray<juce::TextButton> suggestions_;

    juce::String progressText_, resultText_;
    juce::Colour accent_{0xffe4572e};
    int          spinnerPhase_ = 0;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(ChatView)
};

} // namespace forge
