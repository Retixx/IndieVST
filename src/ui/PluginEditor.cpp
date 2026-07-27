#include "ui/PluginEditor.h"

#include "llm/HttpLlmProvider.h"
#include "ui/Fonts.h"

namespace forge {
namespace {

constexpr int kHeaderH   = 34;
constexpr int kStatusH   = 16;
constexpr int kKeyboardH = 66;
constexpr int kCreateNewItemId = 9000;

juce::Colour parseAccent(const std::string& hex) {
    juce::String s = juce::String(hex).trim();
    if (s.startsWithChar('#')) s = s.substring(1);
    if (s.length() == 6) return juce::Colour::fromString("ff" + s);
    if (s.length() == 8) return juce::Colour::fromString(s);
    return juce::Colour(0xffe4572e);
}

// --- settings ---------------------------------------------------------------

class SettingsPanel final : public juce::Component {
public:
    explicit SettingsPanel(ForgeAudioProcessor& processor) : processor_(processor) {
        auto& config = processor_.config();

        providerBox_.addItem("Anthropic", 1);
        providerBox_.addItem("OpenAI", 2);
        providerBox_.addItem("Ollama (local)", 3);
        providerBox_.addItem("Offline library only", 4);
        providerBox_.setSelectedId(static_cast<int>(config.provider) + 1, juce::dontSendNotification);
        addAndMakeVisible(providerBox_);

        keyEditor_.setTextToShowWhenEmpty("sk-...", ForgeLookAndFeel::textDim());
        keyEditor_.setPasswordCharacter(juce::juce_wchar('*'));
        keyEditor_.setText(config.apiKey, false);
        keyEditor_.setFont(fonts::body());
        addAndMakeVisible(keyEditor_);

        modelEditor_.setTextToShowWhenEmpty(config.effectiveModel(), ForgeLookAndFeel::textDim());
        modelEditor_.setText(config.model, false);
        modelEditor_.setFont(fonts::body());
        addAndMakeVisible(modelEditor_);

        offlineToggle_.setButtonText("Force offline");
        offlineToggle_.setToggleState(config.forceOffline, juce::dontSendNotification);
        addAndMakeVisible(offlineToggle_);

        for (auto* label : {&providerLabel_, &keyLabel_, &modelLabel_, &noteLabel_}) {
            label->setColour(juce::Label::textColourId, ForgeLookAndFeel::textDim());
            label->setFont(fonts::caption());
            addAndMakeVisible(*label);
        }
        providerLabel_.setText("PROVIDER", juce::dontSendNotification);
        keyLabel_.setText("API KEY", juce::dontSendNotification);
        modelLabel_.setText("MODEL", juce::dontSendNotification);
        noteLabel_.setText("Saved to " + ForgeConfig::configFile().getFullPathName(),
                           juce::dontSendNotification);

        statusLabel_.setFont(fonts::caption());
        statusLabel_.setJustificationType(juce::Justification::centredLeft);
        statusLabel_.setColour(juce::Label::textColourId, ForgeLookAndFeel::textSecondary());
        addAndMakeVisible(statusLabel_);

        saveButton_.onClick = [this] { commit(); statusLabel_.setText("Saved.", juce::dontSendNotification); };
        addAndMakeVisible(saveButton_);

        closeButton_.onClick = [this] {
            commit();
            if (auto* dw = findParentComponentOfClass<juce::DialogWindow>()) dw->closeButtonPressed();
        };
        addAndMakeVisible(closeButton_);

        // A real request against the real endpoint, so "is my key working" gets
        // a definite answer rather than an inference from a failed generation.
        testButton_.onClick = [this] { runConnectionTest(); };
        addAndMakeVisible(testButton_);

        setSize(430, 272);
    }

    void commit() {
        auto& cfg = processor_.config();
        cfg.provider     = static_cast<ForgeConfig::Provider>(providerBox_.getSelectedId() - 1);
        cfg.apiKey       = keyEditor_.getText().trim();
        cfg.model        = modelEditor_.getText().trim();
        cfg.forceOffline = offlineToggle_.getToggleState();
        processor_.applyConfigChanges();
    }

    void runConnectionTest() {
        commit();
        const ForgeConfig cfg = processor_.config();

        if (cfg.provider == ForgeConfig::Provider::Canned || cfg.forceOffline) {
            statusLabel_.setText("Offline mode - nothing to test.", juce::dontSendNotification);
            return;
        }
        statusLabel_.setText("Testing...", juce::dontSendNotification);
        testButton_.setEnabled(false);

        juce::Component::SafePointer<SettingsPanel> safe(this);
        juce::Thread::launch([cfg, safe] {
            llm::GenerationRequest request;
            request.systemPrompt = "Reply with exactly {\"ok\":true} and nothing else.";
            request.userMessage  = "ping";
            request.maxTokens    = 16;
            request.temperature  = 0.0f;
            request.timeoutMs    = juce::jmin(cfg.timeoutMs, 15000);

            llm::HttpLlmProvider provider(cfg);
            const auto result = provider.generate(request, [] { return false; });

            const juce::String message =
                result.ok ? "Connected. Key works."
                          : juce::String("Failed: ") + juce::String(result.errorMessage);

            juce::MessageManager::callAsync([safe, message] {
                if (safe == nullptr) return;
                safe->statusLabel_.setText(message, juce::dontSendNotification);
                safe->testButton_.setEnabled(true);
            });
        });
    }

    void resized() override {
        auto bounds = getLocalBounds().reduced(16, 14);
        auto row = [&bounds](int h, int gap = 5) {
            auto r = bounds.removeFromTop(h);
            bounds.removeFromTop(gap);
            return r;
        };
        providerLabel_.setBounds(row(13, 2));
        providerBox_.setBounds(row(26));
        keyLabel_.setBounds(row(13, 2));
        keyEditor_.setBounds(row(26));
        modelLabel_.setBounds(row(13, 2));
        modelEditor_.setBounds(row(26));
        offlineToggle_.setBounds(row(22));
        noteLabel_.setBounds(row(14));

        auto buttons = bounds.removeFromBottom(26);
        closeButton_.setBounds(buttons.removeFromRight(74));
        buttons.removeFromRight(6);
        saveButton_.setBounds(buttons.removeFromRight(66));
        buttons.removeFromRight(6);
        testButton_.setBounds(buttons.removeFromRight(74));
        buttons.removeFromRight(10);
        statusLabel_.setBounds(buttons);
    }

    void paint(juce::Graphics& g) override { g.fillAll(ForgeLookAndFeel::background()); }

private:
    ForgeAudioProcessor& processor_;
    juce::ComboBox     providerBox_;
    juce::TextEditor   keyEditor_, modelEditor_;
    juce::ToggleButton offlineToggle_;
    juce::Label        providerLabel_, keyLabel_, modelLabel_, noteLabel_, statusLabel_;
    juce::TextButton   saveButton_{"Save"}, closeButton_{"Done"}, testButton_{"Test key"};
};

} // namespace

// ---------------------------------------------------------------------------

ForgeEditor::ForgeEditor(ForgeAudioProcessor& owner)
    : juce::AudioProcessorEditor(&owner),
      processor_(owner),
      chatView_(owner),
      instrumentView_(owner),
      keyboard_(owner.keyboardState()) {

    setLookAndFeel(&lookAndFeel_);

    instrumentBox_.setTextWhenNothingSelected("No instrument");
    instrumentBox_.onChange = [this] {
        if (updatingMenu_) return;
        const int id = instrumentBox_.getSelectedId();
        if (id == kCreateNewItemId)               processor_.startNewInstrument();
        else if (id > 0 && id <= menuIds_.size()) processor_.loadInstrument(menuIds_[id - 1]);
    };
    addAndMakeVisible(instrumentBox_);

    menuButton_.onClick = [this] { showMainMenu(); };
    addAndMakeVisible(menuButton_);

    addChildComponent(chatView_);
    addChildComponent(instrumentView_);

    keyboard_.onOctaveChanged = [this] { repaint(0, getHeight() - kStatusH, getWidth(), kStatusH); };
    addChildComponent(keyboard_);

    processor_.addForgeListener(this);
    refreshInstrumentMenu();
    applyScreenState();

    setResizable(true, true);
    // Lock the 7:3 proportions: resizing should scale the panel, not reshape it.
    if (auto* bounds = getConstrainer()) {
        bounds->setFixedAspectRatio(7.0 / 3.0);
        bounds->setSizeLimits(770, 330, 1610, 690);
    }
    setSize(1050, 450);
    startTimerHz(30);
}

ForgeEditor::~ForgeEditor() {
    stopTimer();
    processor_.removeForgeListener(this);
    setLookAndFeel(nullptr);
}

// ---------------------------------------------------------------------------

void ForgeEditor::timerCallback() {
    const float peak = processor_.outputPeak();
    // Fast attack, slow release: a meter that falls instantly is unreadable.
    meterLevel_ = peak > meterLevel_ ? peak : meterLevel_ * 0.88f;
    repaint(getWidth() - 96, 0, 96, kHeaderH);
}

void ForgeEditor::forgeStateChanged() {
    refreshInstrumentMenu();
    instrumentView_.rebuild();
    chatView_.refresh();
    applyScreenState();
    repaint();
}

void ForgeEditor::forgeProgress(const juce::String& text) {
    progressText_ = text;
    chatView_.setProgress(text);
    repaint(0, getHeight() - kStatusH, getWidth(), kStatusH);
}

void ForgeEditor::applyScreenState() {
    const bool chat = processor_.showingChat() || processor_.currentInstrument() == nullptr;
    chatView_.setVisible(chat);
    instrumentView_.setVisible(!chat);
    keyboard_.setVisible(!chat);

    if (const auto* instrument = processor_.currentInstrument()) {
        const auto accent = parseAccent(instrument->ui.accent);
        lookAndFeel_.setAccent(accent);
        instrumentView_.setAccent(accent);
        chatView_.setAccent(accent);
        instrumentBox_.setTooltip(juce::String(instrument->description));
    }

    resized();
    if (chat) chatView_.focusPrompt();
    else      keyboard_.grabKeyboardFocus();
}

void ForgeEditor::refreshInstrumentMenu() {
    updatingMenu_ = true;
    instrumentBox_.clear(juce::dontSendNotification);
    menuIds_.clear();

    bool sessionHeader = false, libraryHeader = false;
    for (const auto& entry : processor_.library().entries()) {
        if (!entry.onDisk && !sessionHeader) { instrumentBox_.addSectionHeading("This project"); sessionHeader = true; }
        if (entry.onDisk && !libraryHeader)  { instrumentBox_.addSectionHeading("Library");      libraryHeader = true; }
        menuIds_.add(entry.id);
        instrumentBox_.addItem(entry.name, menuIds_.size());
    }
    instrumentBox_.addSeparator();
    instrumentBox_.addItem("+ Create New Instrument", kCreateNewItemId);

    const int index = menuIds_.indexOf(processor_.currentInstrumentId());
    if (index >= 0) instrumentBox_.setSelectedId(index + 1, juce::dontSendNotification);
    updatingMenu_ = false;
}

// ---------------------------------------------------------------------------

void ForgeEditor::showMainMenu() {
    const bool haveInstrument = processor_.currentInstrument() != nullptr;
    const bool graphMode = instrumentView_.mode() == InstrumentView::Mode::Graph;
    const bool chat = processor_.showingChat() || !haveInstrument;

    juce::PopupMenu menu;
    menu.setLookAndFeel(&lookAndFeel_);

    menu.addItem(1, "Edit with chat", haveInstrument && !chat);
    menu.addItem(2, "New instrument", true);
    menu.addSeparator();
    menu.addItem(3, "Controls", haveInstrument && !chat, !graphMode);
    menu.addItem(4, "How it's built", haveInstrument && !chat, graphMode);
    menu.addSeparator();
    menu.addItem(5, "Rename instrument...", haveInstrument);
    menu.addItem(6, "Remove from library", haveInstrument);
    menu.addSeparator();
    menu.addItem(8, "Open last generation log", processor_.lastLogPath().isNotEmpty());
    menu.addItem(9, "Open instrument folder", true);
    menu.addSeparator();
    menu.addItem(7, "Settings...", true);

    menu.showMenuAsync(juce::PopupMenu::Options()
                           .withTargetComponent(&menuButton_)
                           .withMinimumWidth(170),
                       [this](int result) {
                           switch (result) {
                               case 1: processor_.setShowingChat(true, true); break;
                               case 2: processor_.startNewInstrument(); break;
                               case 3: instrumentView_.setMode(InstrumentView::Mode::Controls); break;
                               case 4: instrumentView_.setMode(InstrumentView::Mode::Graph); break;
                               case 5: showRenameDialog(); break;
                               case 6:
                                   processor_.library().remove(processor_.currentInstrumentId());
                                   refreshInstrumentMenu();
                                   break;
                               case 7: showSettings(); break;
                               case 8: juce::File(processor_.lastLogPath()).revealToUser(); break;
                               case 9: ForgeConfig::instrumentsDirectory().revealToUser(); break;
                               default: break;
                           }
                       });
}

void ForgeEditor::showRenameDialog() {
    const auto* entry = processor_.library().find(processor_.currentInstrumentId());
    if (entry == nullptr) return;

    auto* editor = new juce::AlertWindow("Rename instrument", {}, juce::MessageBoxIconType::NoIcon);
    editor->setLookAndFeel(&lookAndFeel_);
    editor->addTextEditor("name", entry->name);
    editor->addButton("Rename", 1, juce::KeyPress(juce::KeyPress::returnKey));
    editor->addButton("Cancel", 0, juce::KeyPress(juce::KeyPress::escapeKey));
    editor->enterModalState(true, juce::ModalCallbackFunction::create(
        [this, editor](int result) {
            if (result == 1) {
                const auto name = editor->getTextEditorContents("name").trim();
                if (name.isNotEmpty()) {
                    processor_.library().rename(processor_.currentInstrumentId(), name);
                    refreshInstrumentMenu();
                }
            }
            editor->setLookAndFeel(nullptr);
            delete editor;
        }), false);
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

// ---------------------------------------------------------------------------

void ForgeEditor::drawMeter(juce::Graphics& g, juce::Rectangle<int> area) const {
    g.setColour(ForgeLookAndFeel::panelRaised());
    g.fillRoundedRectangle(area.toFloat(), 2.0f);

    const float level = juce::jlimit(0.0f, 1.0f, meterLevel_);
    if (level > 0.001f) {
        auto filled = area.toFloat().withWidth(area.toFloat().getWidth() * level);
        g.setColour(level > 0.98f ? juce::Colour(0xffe0554b) : lookAndFeel_.accent());
        g.fillRoundedRectangle(filled, 2.0f);
    }
    g.setColour(ForgeLookAndFeel::outline());
    g.drawRoundedRectangle(area.toFloat().reduced(0.5f), 2.0f, 1.0f);
}

void ForgeEditor::paint(juce::Graphics& g) {
    g.fillAll(ForgeLookAndFeel::background());

    // Header.
    auto header = getLocalBounds().removeFromTop(kHeaderH);
    g.setColour(ForgeLookAndFeel::panel());
    g.fillRect(header);
    g.setColour(ForgeLookAndFeel::outline());
    g.fillRect(header.removeFromBottom(1));

    auto wordmark = juce::Rectangle<int>(14, 0, 90, kHeaderH);
    g.setFont(fonts::title());
    g.setColour(ForgeLookAndFeel::textPrimary());
    fonts::drawTracked(g, "FORGE", wordmark, juce::Justification::left, 2.0f);

    // Accent tick beside the wordmark: the only saturated pixel in the chrome.
    g.setColour(lookAndFeel_.accent());
    g.fillRect(juce::Rectangle<int>(14, kHeaderH / 2 + 8, 22, 2));

    drawMeter(g, juce::Rectangle<int>(getWidth() - 86, kHeaderH / 2 - 3, 70, 6));

    // Status strip: description on the left, activity on the right.
    auto status = getLocalBounds().removeFromBottom(kStatusH);
    g.setColour(ForgeLookAndFeel::panel());
    g.fillRect(status);
    g.setColour(ForgeLookAndFeel::outline());
    g.fillRect(status.removeFromTop(1));

    status = status.reduced(14, 0);
    g.setFont(fonts::caption());

    // Carve the strip into three non-overlapping regions before drawing
    // anything into it. Drawing all three into the full width is what let the
    // hint and the status text collide.
    const int total = status.getWidth();
    auto rightArea = status.removeFromRight(juce::jmin(210, total / 3));
    juce::Rectangle<int> hintArea;
    if (keyboard_.isVisible() && status.getWidth() > 220) {
        status.removeFromRight(10);
        hintArea = status.removeFromRight(juce::jmin(260, status.getWidth() / 2));
    }
    auto descArea = status;

    if (const auto* instrument = processor_.currentInstrument()) {
        g.setColour(ForgeLookAndFeel::textDim());
        g.drawFittedText(juce::String(instrument->description), descArea,
                         juce::Justification::centredLeft, 1);
    }

    if (!hintArea.isEmpty()) {
        g.setColour(ForgeLookAndFeel::textDim());
        g.drawFittedText("A W S E D F T G Y H U J K  from C"
                             + juce::String(keyboard_.computerKeyOctave())
                             + "   -   Z / X octave",
                         hintArea, juce::Justification::centred, 1);
    }

    const auto right = processor_.isGenerating() ? progressText_ : processor_.lastStatus();
    g.setColour(processor_.isGenerating() ? lookAndFeel_.accent() : ForgeLookAndFeel::textDim());
    g.drawFittedText(right, rightArea, juce::Justification::centredRight, 1);
}

void ForgeEditor::mouseDown(const juce::MouseEvent&) {
    // Knobs take focus when you drag them, which silently kills computer-key
    // playing. Clicking any dead space hands it back.
    if (keyboard_.isVisible()) keyboard_.grabKeyboardFocus();
}

void ForgeEditor::resized() {
    auto bounds = getLocalBounds();

    auto header = bounds.removeFromTop(kHeaderH);
    header.removeFromLeft(112);                 // wordmark
    header.removeFromRight(96);                 // meter
    header = header.reduced(0, 5);
    menuButton_.setBounds(header.removeFromRight(64));
    header.removeFromRight(6);
    instrumentBox_.setBounds(header.removeFromLeft(juce::jmin(230, header.getWidth())));

    bounds.removeFromBottom(kStatusH);

    if (keyboard_.isVisible()) {
        // Scale the keyboard so the range exactly spans the window instead of
        // leaving a slab of empty white at the right-hand end.
        const int height = juce::jlimit(46, 96, getHeight() / 6);
        auto area = bounds.removeFromBottom(height).reduced(10, 5);
        keyboard_.setBounds(area);

        int whiteKeys = 0;
        for (int note = 0; note <= 127; ++note) {
            const int pitchClass = note % 12;
            if (pitchClass == 0 || pitchClass == 2 || pitchClass == 4 || pitchClass == 5
                || pitchClass == 7 || pitchClass == 9 || pitchClass == 11)
                ++whiteKeys;
        }
        keyboard_.setKeyWidth(juce::jmax(6.0f, static_cast<float>(area.getWidth() - 1)
                                                   / static_cast<float>(whiteKeys)));
    }

    const auto content = bounds.reduced(10, 6);
    chatView_.setBounds(content);
    instrumentView_.setBounds(content);
}

} // namespace forge
