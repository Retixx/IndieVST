#pragma once

#include "plugin/PluginProcessor.h"
#include "ui/ChatView.h"
#include "ui/ForgeKeyboard.h"
#include "ui/ForgeLookAndFeel.h"
#include "ui/InstrumentView.h"

#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_audio_utils/juce_audio_utils.h>

namespace forge {

/// 7:3 horizontal, the proportions instrument plugins actually use. Everything
/// that is not a control lives in one 34px header and one 16px status strip;
/// configuration hides behind a single menu rather than a row of buttons.
class ForgeEditor final : public juce::AudioProcessorEditor,
                          private ForgeAudioProcessor::Listener,
                          private juce::Timer {
public:
    explicit ForgeEditor(ForgeAudioProcessor& owner);
    ~ForgeEditor() override;

    void paint(juce::Graphics&) override;
    void resized() override;
    /// Clicking dead space hands the keystrokes back to the keyboard.
    void mouseDown(const juce::MouseEvent&) override;

private:
    void forgeStateChanged() override;
    void forgeProgress(const juce::String&) override;
    void timerCallback() override;

    void refreshInstrumentMenu();
    void showMainMenu();
    void showSettings();
    void showRenameDialog();
    void applyScreenState();
    void drawMeter(juce::Graphics&, juce::Rectangle<int>) const;

    ForgeAudioProcessor& processor_;
    ForgeLookAndFeel     lookAndFeel_;

    juce::ComboBox   instrumentBox_;
    juce::TextButton menuButton_{"Menu"};

    ChatView         chatView_;
    InstrumentView   instrumentView_;

    ForgeKeyboard       keyboard_;
    juce::TooltipWindow tooltips_{this, 500};

    juce::StringArray menuIds_;
    bool              updatingMenu_ = false;
    juce::String      progressText_;
    float             meterLevel_ = 0.0f;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(ForgeEditor)
};

} // namespace forge
