#pragma once

#include "plugin/PluginProcessor.h"
#include "ui/ChatView.h"
#include "ui/ForgeLookAndFeel.h"
#include "ui/InstrumentView.h"

#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_audio_utils/juce_audio_utils.h>

namespace forge {

class ForgeEditor final : public juce::AudioProcessorEditor,
                          private ForgeAudioProcessor::Listener {
public:
    /// Parameter is named `owner` rather than `processor` because
    /// AudioProcessorEditor already has a `processor` member.
    explicit ForgeEditor(ForgeAudioProcessor& owner);
    ~ForgeEditor() override;

    void paint(juce::Graphics&) override;
    void resized() override;

private:
    void forgeStateChanged() override;
    void forgeProgress(const juce::String&) override;

    void refreshInstrumentMenu();
    void showSettings();
    void showScreenForState();

    ForgeAudioProcessor& processor_;
    ForgeLookAndFeel     lookAndFeel_;

    juce::ComboBox   instrumentBox_;
    juce::TextButton editButton_{"Edit with chat"};
    juce::TextButton settingsButton_{"Settings"};
    juce::Label      titleLabel_, subtitleLabel_, statusLabel_;

    ChatView         chatView_;
    InstrumentView   instrumentView_;

    /// Playable with the mouse, and with the computer keyboard (A W S E D ...)
    /// whenever it holds focus. Shown only on the instrument screen - the chat
    /// screen needs those keystrokes for typing.
    juce::MidiKeyboardComponent keyboard_;
    juce::Label                 keyboardHint_;

    juce::TooltipWindow tooltips_{this, 600};

    juce::StringArray menuIds_;   ///< parallel to the combo box item ids
    bool              updatingMenu_ = false;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(ForgeEditor)
};

} // namespace forge
