#include "ui/PluginEditor.h"

namespace forge {
namespace {

constexpr int kCreateNewItemId = 9999;

juce::Colour parseAccent(const std::string& hex) {
    juce::String s = juce::String(hex).trim();
    if (s.startsWithChar('#')) s = s.substring(1);
    if (s.length() == 6) return juce::Colour::fromString("ff" + s);
    if (s.length() == 8) return juce::Colour::fromString(s);
    return juce::Colour(0xffe4572e);
}

// --- settings panel --------------------------------------------------------

class SettingsPanel final : public juce::Component {
public:
    explicit SettingsPanel(ForgeAudioProcessor& processor) : processor_(processor) {
        auto& config = processor_.config();

        providerBox_.addItem("Anthropic", 1);
        providerBox_.addItem("OpenAI", 2);
        providerBox_.addItem("Ollama (local)", 3);
        providerBox_.addItem("Offline library only", 4);
        providerBox_.setSelectedId(static_cast<int>(config.provider) + 1,
                                   juce::dontSendNotification);
        addAndMakeVisible(providerBox_);

        keyEditor_.setTextToShowWhenEmpty("API key", ForgeLookAndFeel::textSecondary());
        keyEditor_.setPasswordCharacter(juce::juce_wchar('*'));
        keyEditor_.setText(config.apiKey, false);
        addAndMakeVisible(keyEditor_);

        modelEditor_.setTextToShowWhenEmpty(config.effectiveModel(),
                                            ForgeLookAndFeel::textSecondary());
        modelEditor_.setText(config.model, false);
        addAndMakeVisible(modelEditor_);

        offlineToggle_.setButtonText("Force offline (rehearse the demo without a network)");
        offlineToggle_.setToggleState(config.forceOffline, juce::dontSendNotification);
        addAndMakeVisible(offlineToggle_);

        for (auto* label : {&providerLabel_, &keyLabel_, &modelLabel_, &noteLabel_}) {
            label->setColour(juce::Label::textColourId, ForgeLookAndFeel::textSecondary());
            label->setFont(juce::Font(juce::FontOptions(12.0f)));
            addAndMakeVisible(*label);
        }
        providerLabel_.setText("Provider", juce::dontSendNotification);
        keyLabel_.setText("API key", juce::dontSendNotification);
        modelLabel_.setText("Model", juce::dontSendNotification);
        noteLabel_.setText("Stored in your user folder, never in the project file.",
                           juce::dontSendNotification);

        saveButton_.onClick = [this] {
            auto& cfg = processor_.config();
            cfg.provider     = static_cast<ForgeConfig::Provider>(providerBox_.getSelectedId() - 1);
            cfg.apiKey       = keyEditor_.getText().trim();
            cfg.model        = modelEditor_.getText().trim();
            cfg.forceOffline = offlineToggle_.getToggleState();
            processor_.applyConfigChanges();
            if (auto* dw = findParentComponentOfClass<juce::DialogWindow>()) dw->closeButtonPressed();
        };
        addAndMakeVisible(saveButton_);

        setSize(420, 250);
    }

    void resized() override {
        auto bounds = getLocalBounds().reduced(14);
        auto row = [&bounds](int h) { auto r = bounds.removeFromTop(h); bounds.removeFromTop(6); return r; };

        providerLabel_.setBounds(row(16));
        providerBox_.setBounds(row(28));
        keyLabel_.setBounds(row(16));
        keyEditor_.setBounds(row(28));
        modelLabel_.setBounds(row(16));
        modelEditor_.setBounds(row(28));
        offlineToggle_.setBounds(row(24));
        noteLabel_.setBounds(row(16));
        saveButton_.setBounds(bounds.removeFromBottom(30).removeFromRight(110));
    }

    void paint(juce::Graphics& g) override { g.fillAll(ForgeLookAndFeel::background()); }

private:
    ForgeAudioProcessor& processor_;
    juce::ComboBox     providerBox_;
    juce::TextEditor   keyEditor_, modelEditor_;
    juce::ToggleButton offlineToggle_;
    juce::Label        providerLabel_, keyLabel_, modelLabel_, noteLabel_;
    juce::TextButton   saveButton_{"Save"};
};

} // namespace

// ---------------------------------------------------------------------------

ForgeEditor::ForgeEditor(ForgeAudioProcessor& owner)
    : juce::AudioProcessorEditor(&owner),
      processor_(owner),
      chatView_(owner),
      instrumentView_(owner),
      keyboard_(owner.keyboardState(), juce::MidiKeyboardComponent::horizontalKeyboard) {

    setLookAndFeel(&lookAndFeel_);

    keyboard_.setAvailableRange(24, 96);
    keyboard_.setLowestVisibleKey(36);
    keyboard_.setOctaveForMiddleC(4);
    keyboard_.setWantsKeyboardFocus(true);
    keyboard_.setColour(juce::MidiKeyboardComponent::shadowColourId,
                        juce::Colours::transparentBlack);
    addChildComponent(keyboard_);

    keyboardHint_.setText("Click the keys, or use A W S E D F T G Y H U J K",
                          juce::dontSendNotification);
    keyboardHint_.setJustificationType(juce::Justification::centredLeft);
    keyboardHint_.setColour(juce::Label::textColourId, ForgeLookAndFeel::textSecondary());
    keyboardHint_.setFont(juce::Font(juce::FontOptions(11.0f)));
    keyboardHint_.setInterceptsMouseClicks(false, false);
    addChildComponent(keyboardHint_);

    titleLabel_.setText("FORGE", juce::dontSendNotification);
    titleLabel_.setFont(juce::Font(juce::FontOptions(17.0f, juce::Font::bold)));
    addAndMakeVisible(titleLabel_);

    subtitleLabel_.setColour(juce::Label::textColourId, ForgeLookAndFeel::textSecondary());
    subtitleLabel_.setFont(juce::Font(juce::FontOptions(12.0f)));
    addAndMakeVisible(subtitleLabel_);

    statusLabel_.setColour(juce::Label::textColourId, ForgeLookAndFeel::textSecondary());
    statusLabel_.setFont(juce::Font(juce::FontOptions(11.5f)));
    statusLabel_.setJustificationType(juce::Justification::centredRight);
    addAndMakeVisible(statusLabel_);

    instrumentBox_.setTextWhenNothingSelected("No instrument");
    instrumentBox_.onChange = [this] {
        if (updatingMenu_) return;
        const int id = instrumentBox_.getSelectedId();
        if (id == kCreateNewItemId) {
            processor_.startNewInstrument();
        } else if (id > 0 && id <= menuIds_.size()) {
            processor_.loadInstrument(menuIds_[id - 1]);
        }
    };
    addAndMakeVisible(instrumentBox_);

    editButton_.onClick = [this] { processor_.setShowingChat(true, true); };
    addAndMakeVisible(editButton_);

    settingsButton_.onClick = [this] { showSettings(); };
    addAndMakeVisible(settingsButton_);

    addChildComponent(chatView_);
    addChildComponent(instrumentView_);

    processor_.addForgeListener(this);
    refreshInstrumentMenu();
    showScreenForState();

    setResizable(true, true);
    setResizeLimits(760, 520, 1800, 1200);
    setSize(940, 620);
}

ForgeEditor::~ForgeEditor() {
    processor_.removeForgeListener(this);
    setLookAndFeel(nullptr);
}

void ForgeEditor::forgeStateChanged() {
    refreshInstrumentMenu();
    instrumentView_.rebuild();
    chatView_.refresh();
    showScreenForState();
    repaint();
}

void ForgeEditor::forgeProgress(const juce::String& text) {
    chatView_.setProgress(text);
    statusLabel_.setText(text, juce::dontSendNotification);
}

void ForgeEditor::showScreenForState() {
    const bool chat = processor_.showingChat() || processor_.currentInstrument() == nullptr;
    chatView_.setVisible(chat);
    instrumentView_.setVisible(!chat);
    keyboard_.setVisible(!chat);
    keyboardHint_.setVisible(!chat);
    editButton_.setEnabled(processor_.currentInstrument() != nullptr && chat == false);

    if (const auto* instrument = processor_.currentInstrument()) {
        lookAndFeel_.setAccent(parseAccent(instrument->ui.accent));
        instrumentView_.setColour(juce::Label::textColourId, ForgeLookAndFeel::textPrimary());
        subtitleLabel_.setText(juce::String(instrument->description), juce::dontSendNotification);
    } else {
        subtitleLabel_.setText("Describe an instrument to begin.", juce::dontSendNotification);
    }

    statusLabel_.setText(processor_.lastStatus(), juce::dontSendNotification);
    resized();

    // Focus follows the screen: the chat needs keystrokes for typing, the
    // instrument screen needs them for playing notes.
    if (chat) chatView_.focusPrompt();
    else      keyboard_.grabKeyboardFocus();
}

void ForgeEditor::refreshInstrumentMenu() {
    updatingMenu_ = true;
    instrumentBox_.clear(juce::dontSendNotification);
    menuIds_.clear();

    const auto& entries = processor_.library().entries();
    bool addedSessionHeader = false, addedLibraryHeader = false;

    for (const auto& entry : entries) {
        if (!entry.onDisk && !addedSessionHeader) {
            instrumentBox_.addSectionHeading("This project");
            addedSessionHeader = true;
        }
        if (entry.onDisk && !addedLibraryHeader) {
            instrumentBox_.addSectionHeading("Library");
            addedLibraryHeader = true;
        }
        menuIds_.add(entry.id);
        instrumentBox_.addItem(entry.name, menuIds_.size());
    }

    instrumentBox_.addSeparator();
    instrumentBox_.addItem("+ Create New Instrument", kCreateNewItemId);

    const int index = menuIds_.indexOf(processor_.currentInstrumentId());
    if (index >= 0) instrumentBox_.setSelectedId(index + 1, juce::dontSendNotification);
    updatingMenu_ = false;
}

void ForgeEditor::showSettings() {
    juce::DialogWindow::LaunchOptions options;
    options.content.setOwned(new SettingsPanel(processor_));
    options.dialogTitle = "Forge settings";
    options.dialogBackgroundColour = ForgeLookAndFeel::background();
    options.escapeKeyTriggersCloseButton = true;
    options.useNativeTitleBar = true;
    options.resizable = false;
    options.launchAsync();
}

void ForgeEditor::paint(juce::Graphics& g) {
    g.fillAll(ForgeLookAndFeel::background());

    auto header = getLocalBounds().removeFromTop(56);
    g.setColour(ForgeLookAndFeel::panel());
    g.fillRect(header);
    g.setColour(lookAndFeel_.accent());
    g.fillRect(header.removeFromTop(2));
}

void ForgeEditor::resized() {
    auto bounds = getLocalBounds();

    auto header = bounds.removeFromTop(56).reduced(12, 8);
    auto left = header.removeFromLeft(150);
    titleLabel_.setBounds(left.removeFromTop(22));
    subtitleLabel_.setBounds(header.removeFromLeft(juce::jmax(0, header.getWidth() - 430)));

    settingsButton_.setBounds(header.removeFromRight(90).reduced(2));
    header.removeFromRight(6);
    editButton_.setBounds(header.removeFromRight(120).reduced(2));
    header.removeFromRight(6);
    instrumentBox_.setBounds(header.removeFromRight(190).reduced(2));

    auto footer = bounds.removeFromBottom(22).reduced(12, 2);
    statusLabel_.setBounds(footer);

    if (keyboard_.isVisible()) {
        keyboardHint_.setBounds(bounds.removeFromBottom(16).reduced(12, 0));
        keyboard_.setBounds(bounds.removeFromBottom(72).reduced(12, 4));
    }

    const auto content = bounds.reduced(12, 8);
    chatView_.setBounds(content);
    instrumentView_.setBounds(content);
}

} // namespace forge
