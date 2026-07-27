#include "plugin/PluginProcessor.h"

#include "core/dsp/GraphBuilder.h"
#include "core/ir/IrRepair.h"
#include "core/ir/IrSafety.h"
#include "ui/PluginEditor.h"

namespace forge {
namespace {
/// Length of the equal-power-ish crossfade applied when swapping instruments.
/// Short enough to feel instant, long enough that a held chord never clicks.
constexpr double kCrossfadeSeconds = 0.020;
} // namespace

// ---------------------------------------------------------------------------

ForgeAudioProcessor::ForgeAudioProcessor()
    : juce::AudioProcessor(BusesProperties().withOutput("Output", juce::AudioChannelSet::stereo(), true)) {
    config_ = ForgeConfig::load();

    // Declared once, before the host scans. See ParameterPool.h for why the
    // pool is static even though the instruments are not.
    pool_.createParameters(*this);

    library_.seedIfEmpty();
    library_.loadFromDisk();

    startTimerHz(10);   // graph GC + meter refresh
}

ForgeAudioProcessor::~ForgeAudioProcessor() {
    stopTimer();
    session_.cancel();
    publisher_.clear();
}

// ---------------------------------------------------------------------------

void ForgeAudioProcessor::prepareToPlay(double sampleRate, int maximumExpectedSamplesPerBlock) {
    preparedSampleRate_ = sampleRate;
    crossfadeScratch_.setSize(2, juce::jmax(64, maximumExpectedSamplesPerBlock), false, true, true);
    crossfadeGain_ = 1.0f;
    crossfadeInc_  = static_cast<float>(1.0 / juce::jmax(1.0, kCrossfadeSeconds * sampleRate));

    // A sample rate change invalidates every filter coefficient and delay line
    // in the graph, so it has to be rebuilt. This runs on the message thread
    // before playback resumes, which is the one place a rebuild is free.
    if (hasCurrent_) rebuildCurrentGraph();
}

void ForgeAudioProcessor::releaseResources() {
    publisher_.collectGarbage();
}

bool ForgeAudioProcessor::isBusesLayoutSupported(const BusesLayout& layouts) const {
    return layouts.getMainOutputChannelSet() == juce::AudioChannelSet::stereo();
}

// ---------------------------------------------------------------------------

void ForgeAudioProcessor::handleMidiEvent(GraphInstance* current, GraphInstance* retiring,
                                          const juce::MidiMessage& m) noexcept {
    GraphInstance* graphs[2] = { current, retiring };
    const float bendRange = pool_.bendRange() != nullptr ? pool_.bendRange()->get() : 2.0f;

    for (GraphInstance* g : graphs) {
        if (g == nullptr) continue;
        if (m.isNoteOn())            g->noteOn(m.getNoteNumber(), m.getFloatVelocity());
        else if (m.isNoteOff())      g->noteOff(m.getNoteNumber());
        else if (m.isAllNotesOff() || m.isAllSoundOff())
                                     g->allNotesOff(m.isAllSoundOff());
        else if (m.isPitchWheel())
            g->setPitchBendSemis((m.getPitchWheelValue() - 8192) / 8192.0f * bendRange);
        else if (m.isController()) {
            if (m.getControllerNumber() == 1)
                g->setModWheel(m.getControllerValue() / 127.0f);
            else if (m.getControllerNumber() == 64)
                g->sustainPedal(m.getControllerValue() >= 64);
        }
        else if (m.isChannelPressure())
            g->setAftertouch(m.getChannelPressureValue() / 127.0f);
        else if (m.isAftertouch())
            g->setAftertouch(m.getAfterTouchValue() / 127.0f);
    }
}

void ForgeAudioProcessor::renderSegment(juce::AudioBuffer<float>& buffer,
                                        int startSample, int numSamples,
                                        GraphInstance* current, GraphInstance* retiring) noexcept {
    if (numSamples <= 0) return;

    float* out[kNumChannels] = { buffer.getWritePointer(0) + startSample,
                                 buffer.getWritePointer(1) + startSample };
    current->process(out, numSamples);

    if (retiring != nullptr && crossfadeGain_ < 1.0f
        && crossfadeScratch_.getNumSamples() >= numSamples) {
        float* old[kNumChannels] = { crossfadeScratch_.getWritePointer(0),
                                     crossfadeScratch_.getWritePointer(1) };
        retiring->process(old, numSamples);

        float g = crossfadeGain_;
        for (int i = 0; i < numSamples; ++i) {
            g = juce::jlimit(0.0f, 1.0f, g + crossfadeInc_);
            // Equal-power keeps the perceived level constant across the swap.
            const float a = std::sin(g * 1.5707963f);
            const float b = std::cos(g * 1.5707963f);
            for (int ch = 0; ch < kNumChannels; ++ch)
                out[ch][i] = out[ch][i] * a + old[ch][i] * b;
        }
        crossfadeGain_ = g;
        if (crossfadeGain_ >= 1.0f) publisher_.finishRetiring(retiring);
    }
}

void ForgeAudioProcessor::processBlock(juce::AudioBuffer<float>& buffer,
                                       juce::MidiBuffer& midi) {
    juce::ScopedNoDenormals noDenormals;

    const int numSamples = buffer.getNumSamples();
    for (int ch = getTotalNumInputChannels(); ch < getTotalNumOutputChannels(); ++ch)
        buffer.clear(ch, 0, numSamples);
    buffer.clear();

    publisher_.tick();

    // Merge anything played on the editor's on-screen keyboard into the
    // incoming MIDI stream, so hardware and mouse/computer-key input take
    // exactly the same path from here on.
    keyboardState_.processNextMidiBuffer(midi, 0, numSamples, true);

    GraphInstance* current  = publisher_.current();
    GraphInstance* retiring = publisher_.retiring();

    if (current == nullptr) {
        peak_.store(0.0f, std::memory_order_relaxed);
        activeVoices_.store(0, std::memory_order_relaxed);
        return;
    }

    // Host transport, for tempo-synced LFOs and delays.
    double bpm = 120.0;
    bool playing = false;
    if (auto* transport = getPlayHead()) {
        if (auto position = transport->getPosition()) {
            if (auto hostBpm = position->getBpm()) bpm = *hostBpm;
            playing = position->getIsPlaying();
        }
    }
    current->setTransport(bpm, playing);
    if (retiring != nullptr) retiring->setTransport(bpm, playing);

    pool_.pushTo(*current);

    // Sample-accurate MIDI: render up to each event, apply it, continue.
    int cursor = 0;
    for (const auto metadata : midi) {
        const int eventTime = juce::jlimit(0, numSamples, metadata.samplePosition);
        if (eventTime > cursor) {
            renderSegment(buffer, cursor, eventTime - cursor, current, retiring);
            cursor = eventTime;
        }
        handleMidiEvent(current, retiring, metadata.getMessage());
    }
    if (cursor < numSamples)
        renderSegment(buffer, cursor, numSamples - cursor, current, retiring);

    const float master = pool_.masterVolume() != nullptr ? pool_.masterVolume()->get() : 0.85f;
    if (master != 1.0f) buffer.applyGain(master);

    peak_.store(buffer.getMagnitude(0, numSamples), std::memory_order_relaxed);
    activeVoices_.store(current->activeVoiceCount(), std::memory_order_relaxed);
}

// ---------------------------------------------------------------------------

void ForgeAudioProcessor::timerCallback() {
    publisher_.collectGarbage();
}

void ForgeAudioProcessor::addForgeListener(Listener* l)    { listeners_.add(l); }
void ForgeAudioProcessor::removeForgeListener(Listener* l) { listeners_.remove(l); }

void ForgeAudioProcessor::notifyListeners() {
    listeners_.call([](Listener& l) { l.forgeStateChanged(); });
}

const ir::Instrument* ForgeAudioProcessor::currentInstrument() const {
    return hasCurrent_ ? &current_ : nullptr;
}

void ForgeAudioProcessor::setShowingChat(bool shouldShowChat, bool editCurrent) {
    showChat_ = shouldShowChat;
    chatEditsCurrent_ = editCurrent && hasCurrent_;
    notifyListeners();
}

void ForgeAudioProcessor::applyConfigChanges() {
    config_.save();
}

void ForgeAudioProcessor::adoptInstrument(const ir::Instrument& instrument,
                                          std::unique_ptr<GraphInstance> graph,
                                          const juce::String& id) {
    current_    = instrument;
    hasCurrent_ = true;
    currentId_  = id;

    pool_.bind(current_, *this);

    // Seed the new graph with the values the pool now holds, so the first block
    // it renders already matches what the user sees on the knobs.
    if (graph != nullptr) pool_.pushTo(*graph);

    crossfadeGain_ = 0.0f;   // the audio thread fades into the new graph
    publisher_.publish(std::move(graph));

    showChat_ = false;
    notifyListeners();
}

void ForgeAudioProcessor::rebuildCurrentGraph() {
    if (!hasCurrent_) return;
    ir::IrReport report;
    auto graph = GraphBuilder::build(current_, preparedSampleRate_, report);
    if (graph == nullptr) return;
    pool_.pushTo(*graph);
    crossfadeGain_ = 1.0f;   // same instrument, no need to fade
    publisher_.publish(std::move(graph));
}

void ForgeAudioProcessor::generate(const juce::String& prompt, bool editCurrent) {
    if (prompt.trim().isEmpty() || session_.isRunning()) return;

    chat_.push_back({true, prompt.trim(), {}});
    lastStatus_  = "Designing...";
    lastRepairs_ = {};
    notifyListeners();

    juce::String currentJson;
    if (editCurrent && hasCurrent_)
        currentJson = juce::String(ir::toJson(current_).dump());

    std::weak_ptr<int> alive = lifetime_;

    session_.start(
        config_, preparedSampleRate_, prompt.trim(), currentJson,
        [this, alive](const juce::String& text) {
            if (alive.expired()) return;
            lastStatus_ = text;
            listeners_.call([&text](Listener& l) { l.forgeProgress(text); });
        },
        [this, alive, prompt](GenerationSession::Result& result) {
            if (alive.expired()) return;
            lastStatus_       = result.message;
            lastRepairs_      = result.repairSummary;
            lastUsedFallback_ = result.usedFallback;

            if (!result.ok) {
                chat_.push_back({false, result.message, {}});
                notifyListeners();
                return;
            }

            const auto id = library_.add(result.instrument, prompt, true);
            juce::String reply = juce::String(result.instrument.name);
            if (!result.instrument.description.empty())
                reply << " - " << juce::String(result.instrument.description);
            chat_.push_back({false, reply, id});

            adoptInstrument(result.instrument, std::move(result.graph), id);
        });
}

void ForgeAudioProcessor::cancelGeneration() {
    session_.cancel();
    lastStatus_ = "Cancelled.";
    notifyListeners();
}

void ForgeAudioProcessor::loadInstrument(const juce::String& id) {
    const auto* entry = library_.find(id);
    if (entry == nullptr) return;

    ir::IrReport report;
    auto graph = GraphBuilder::build(entry->instrument, preparedSampleRate_, report);
    if (graph == nullptr) {
        lastStatus_ = "That instrument could not be loaded.";
        notifyListeners();
        return;
    }
    lastStatus_  = "Loaded " + entry->name + ".";
    lastRepairs_ = {};
    adoptInstrument(entry->instrument, std::move(graph), id);
}

void ForgeAudioProcessor::startNewInstrument() {
    showChat_ = true;
    chatEditsCurrent_ = false;
    notifyListeners();
}

// ---------------------------------------------------------------------------

void ForgeAudioProcessor::getStateInformation(juce::MemoryBlock& destData) {
    auto* root = new juce::DynamicObject();
    root->setProperty("forge_state_version", 1);
    root->setProperty("activeInstrumentId", currentId_);
    root->setProperty("showChat", showChat_);
    root->setProperty("instruments", library_.toState());

    // The full specification of whatever is loaded travels with the project, so
    // opening it on a machine with an empty library still plays correctly.
    if (hasCurrent_)
        root->setProperty("currentIr", juce::String(ir::toJson(current_).dump()));

    juce::Array<juce::var> chat;
    for (const auto& turn : chat_) {
        auto* obj = new juce::DynamicObject();
        obj->setProperty("user", turn.fromUser);
        obj->setProperty("text", turn.text);
        obj->setProperty("instrumentId", turn.instrumentId);
        chat.add(juce::var(obj));
    }
    root->setProperty("chat", juce::var(chat));

    // Parameter values. Stored by slot index, which is stable by construction.
    juce::Array<juce::var> values;
    for (auto* p : getParameters())
        values.add(p->getValue());
    root->setProperty("parameters", juce::var(values));

    const auto text = juce::JSON::toString(juce::var(root), true);
    destData.replaceAll(text.toRawUTF8(), text.getNumBytesAsUTF8());
}

void ForgeAudioProcessor::setStateInformation(const void* data, int sizeInBytes) {
    if (data == nullptr || sizeInBytes <= 0) return;

    const auto parsed = juce::JSON::parse(juce::String::createStringFromData(data, sizeInBytes));
    auto* root = parsed.getDynamicObject();
    if (root == nullptr) return;

    library_.fromState(root->getProperty("instruments"));
    library_.loadFromDisk();

    chat_.clear();
    if (const auto* turns = root->getProperty("chat").getArray()) {
        for (const auto& t : *turns) {
            if (auto* obj = t.getDynamicObject())
                chat_.push_back({static_cast<bool>(obj->getProperty("user")),
                                 obj->getProperty("text").toString(),
                                 obj->getProperty("instrumentId").toString()});
        }
    }

    showChat_  = static_cast<bool>(root->getProperty("showChat"));
    currentId_ = root->getProperty("activeInstrumentId").toString();

    const auto irText = root->getProperty("currentIr").toString();
    if (irText.isNotEmpty()) {
        ir::Instrument inst;
        ir::IrReport report;
        if (ir::parse(irText.toStdString(), inst, report)) {
            ir::repair(inst, report);
            ir::applySafety(inst, report, config_.cpuBudget);
            ir::IrReport buildReport;
            if (auto graph = GraphBuilder::build(inst, preparedSampleRate_, buildReport)) {
                current_    = inst;
                hasCurrent_ = true;
                pool_.bind(current_, *this);
                crossfadeGain_ = 1.0f;
                publisher_.publish(std::move(graph));
            }
        }
    }

    // Parameter values are restored AFTER bind(), because bind() resets every
    // slot to the instrument's defaults.
    if (const auto* values = root->getProperty("parameters").getArray()) {
        const auto& params = getParameters();
        for (int i = 0; i < juce::jmin(values->size(), params.size()); ++i)
            params[i]->setValueNotifyingHost(
                juce::jlimit(0.0f, 1.0f, static_cast<float>(static_cast<double>((*values)[i]))));
    }

    notifyListeners();
}

juce::AudioProcessorEditor* ForgeAudioProcessor::createEditor() {
    return new ForgeEditor(*this);
}

} // namespace forge

// ---------------------------------------------------------------------------

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter() {
    return new forge::ForgeAudioProcessor();
}
