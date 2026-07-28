#pragma once

#include "core/dsp/GraphInstance.h"
#include "core/ir/Ir.h"
#include "llm/GenerationSession.h"
#include "plugin/ForgeConfig.h"
#include "plugin/GraphPublisher.h"
#include "plugin/InstrumentLibrary.h"
#include "core/audio/ReferenceAnalysis.h"
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

    const juce::String getName() const override { return "IndieVST"; }
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

    // --- reference audio ---------------------------------------------------
    //
    // Optional. The musician drops in a recording; Forge measures it and hands
    // the measurements to the model alongside the text. The audio itself is
    // never stored in the instrument and never reaches the audio thread - this
    // is analysis, not sampling, which matters legally as much as musically.

    /// Decodes and analyses the file. Returns false and fills `error` if the
    /// format is unsupported or the audio is unusable. Message thread.
    bool loadReference(const juce::File&, juce::String& error);
    void clearReference();
    bool hasReference() const { return reference_.valid; }
    const audio::ReferenceFeatures& reference() const { return reference_; }
    juce::String referenceName() const { return referenceName_; }
    void cancelGeneration();
    bool isGenerating() const { return session_.isRunning(); }

    void loadInstrument(const juce::String& id);
    void startNewInstrument();

    /// Loads the complete fixed architecture with its defaults - every
    /// oscillator, filter, envelope, LFO and effect the engine has. Useful as
    /// a starting point, and as the reference for what the model is filling in.
    void loadFullArchitecture();

    /// Changes a construction-time module setting (oscillator wave, filter
    /// mode...) and rebuilds the graph. Message thread only; the swap is
    /// covered by the publisher's crossfade so it is seamless.
    void setNodeSetting(const juce::String& nodeId, const juce::String& setting,
                        const nlohmann::json& value);

    /// File extensions the decoder accepts, for the file chooser and the
    /// drag-and-drop filter.
    static juce::String referenceFileFilter();

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

    /// The words the musician actually typed for the loaded instrument, so the
    /// UI can show the request back alongside what was built from it.
    juce::String promptForCurrent() const;

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

    /// The measurements from the optional reference recording. Only the
    /// measurements - the decoded audio is discarded as soon as it is analysed.
    audio::ReferenceFeatures reference_;
    juce::String             referenceName_;
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
