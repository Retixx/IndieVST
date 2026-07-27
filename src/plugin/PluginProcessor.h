#pragma once

#include "core/dsp/GraphInstance.h"
#include "core/ir/Ir.h"
#include "llm/GenerationSession.h"
#include "plugin/ForgeConfig.h"
#include "plugin/GraphPublisher.h"
#include "plugin/InstrumentLibrary.h"
#include "plugin/ParameterPool.h"

#include <juce_audio_processors/juce_audio_processors.h>

namespace forge {

/// One chat turn, kept so reopening the editor does not lose the conversation.
struct ChatTurn {
    bool         fromUser = true;
    juce::String text;
    juce::String instrumentId;   ///< set on the assistant turn that produced one
};

class ForgeAudioProcessor final : public juce::AudioProcessor,
                                  private juce::Timer {
public:
    ForgeAudioProcessor();
    ~ForgeAudioProcessor() override;

    // --- juce::AudioProcessor ---------------------------------------------
    void prepareToPlay(double sampleRate, int maximumExpectedSamplesPerBlock) override;
    void releaseResources() override;
    bool isBusesLayoutSupported(const BusesLayout&) const override;
    void processBlock(juce::AudioBuffer<float>&, juce::MidiBuffer&) override;

    juce::AudioProcessorEditor* createEditor() override;
    bool hasEditor() const override { return true; }

    const juce::String getName() const override { return "Forge"; }
    bool   acceptsMidi()  const override { return true; }
    bool   producesMidi() const override { return false; }
    bool   isMidiEffect() const override { return false; }
    double getTailLengthSeconds() const override { return 8.0; }

    int  getNumPrograms() override { return 1; }
    int  getCurrentProgram() override { return 0; }
    void setCurrentProgram(int) override {}
    const juce::String getProgramName(int) override { return "Default"; }
    void changeProgramName(int, const juce::String&) override {}

    void getStateInformation(juce::MemoryBlock&) override;
    void setStateInformation(const void*, int) override;

    // --- Forge API used by the editor --------------------------------------

    class Listener {
    public:
        virtual ~Listener() = default;
        /// The loaded instrument, the library, or the chat log changed.
        virtual void forgeStateChanged() = 0;
        /// Generation progress text ("Designing...", "Building...").
        virtual void forgeProgress(const juce::String&) = 0;
    };
    void addForgeListener(Listener*);
    void removeForgeListener(Listener*);

    /// `editCurrent` sends the loaded instrument along so the model modifies it
    /// rather than starting over.
    void generate(const juce::String& prompt, bool editCurrent);
    void cancelGeneration();
    bool isGenerating() const { return session_.isRunning(); }

    void loadInstrument(const juce::String& id);
    void startNewInstrument();

    ParameterPool&     parameterPool()      { return pool_; }
    InstrumentLibrary& library()            { return library_; }
    ForgeConfig&       config()             { return config_; }
    void               applyConfigChanges();

    const ir::Instrument* currentInstrument() const;
    juce::String          currentInstrumentId() const { return currentId_; }
    const std::vector<ChatTurn>& chatLog() const { return chat_; }

    juce::String lastStatus()        const { return lastStatus_; }
    juce::String lastRepairSummary() const { return lastRepairs_; }
    bool         lastUsedFallback()  const { return lastUsedFallback_; }
    /// Path of the log written by the most recent failed generation, if any.
    juce::String lastLogPath()       const { return lastLogPath_; }

    bool showingChat() const { return showChat_; }
    /// `editCurrent` distinguishes "modify what's loaded" (the Edit button)
    /// from "start something new" (the dropdown's Create New entry).
    void setShowingChat(bool shouldShowChat, bool editCurrent = false);
    bool chatEditsCurrent() const { return chatEditsCurrent_; }

    /// Shared with the editor's on-screen keyboard. Notes played there are
    /// merged into the incoming MIDI stream, so the plugin is playable with no
    /// controller attached - which matters both for the standalone build and
    /// for demoing on a stage where you have no hardware.
    juce::MidiKeyboardState& keyboardState() { return keyboardState_; }

    /// 0..1, for the editor's meter. Read from the message thread.
    float outputPeak() const { return peak_.load(std::memory_order_relaxed); }
    int   activeVoices() const { return activeVoices_.load(std::memory_order_relaxed); }

private:
    void timerCallback() override;
    void notifyListeners();
    void handleMidiEvent(GraphInstance* current, GraphInstance* retiring,
                         const juce::MidiMessage&) noexcept;
    void renderSegment(juce::AudioBuffer<float>&, int startSample, int numSamples,
                       GraphInstance* current, GraphInstance* retiring) noexcept;
    void rebuildCurrentGraph();
    void adoptInstrument(const ir::Instrument&, std::unique_ptr<GraphInstance>,
                         const juce::String& id);

    ForgeConfig       config_;
    ParameterPool     pool_;
    GraphPublisher    publisher_;
    InstrumentLibrary library_;
    GenerationSession session_;

    ir::Instrument       current_;
    bool                 hasCurrent_ = false;
    juce::String         currentId_;
    std::vector<ChatTurn> chat_;
    juce::String         lastStatus_, lastRepairs_, lastLogPath_;
    bool                 lastUsedFallback_ = false;
    bool                 showChat_ = true;
    bool                 chatEditsCurrent_ = false;

    juce::MidiKeyboardState  keyboardState_;
    juce::AudioBuffer<float> crossfadeScratch_;
    float                    crossfadeGain_ = 1.0f;
    float                    crossfadeInc_  = 1.0f;

    std::atomic<float> peak_{0.0f};
    std::atomic<int>   activeVoices_{0};

    juce::ListenerList<Listener> listeners_;
    double preparedSampleRate_ = 48000.0;

    /// Generation callbacks are delivered asynchronously on the message thread.
    /// If the user closes the project while a generation is in flight, the
    /// message can outlive this object - so every callback holds a weak handle
    /// to this token and bails if it has expired.
    std::shared_ptr<int> lifetime_ = std::make_shared<int>(0);

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(ForgeAudioProcessor)
};

} // namespace forge
