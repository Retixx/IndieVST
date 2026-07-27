// ---------------------------------------------------------------------------
// The runtime DSP graph.
//
// A GraphInstance is built entirely on a worker thread (every allocation, every
// wavetable, every voice) and then handed to the audio thread as a single
// pointer. From that moment it is read-only structure with mutable state, and
// process() touches nothing but its own memory.
//
// Signal flow:
//     voice scope (x polyphony)  ->  per-target voice bus  ->  global scope
//                                                            ->  out.master
//
// Everything runs in kControlBlock-sized chunks. That is what makes modulation
// resolution cheap, keeps every internal buffer 16 samples long, and gives the
// whole engine a single, uniform 0.33 ms modulation latency instead of a
// scattering of special cases.
// ---------------------------------------------------------------------------
#pragma once

#include "core/dsp/Module.h"
#include "core/dsp/Registry.h"
#include "core/ir/Ir.h"

#include <memory>
#include <vector>

namespace forge {

struct PlanNode {
    int                   irIndex = -1;
    const ModuleManifest* man     = nullptr;
    /// (plan index within the same scope, gain)
    std::vector<std::pair<int, float>> sources;
    int  paramOffset = 0;   ///< into the scope's flat parameter arrays
    int  numParams   = 0;
    bool producesAudio = false;
    bool isModSource   = false;
    bool holdsVoice    = false;
};

/// One scope's static plan plus its flat parameter tables.
struct ScopePlan {
    std::vector<PlanNode> nodes;            ///< already in execution order
    int                   totalParams = 0;
    std::vector<float>    paramMin, paramMax;
    std::vector<Taper>    paramTaper;
    std::vector<float>    paramDefaultNorm;
    /// Base to use when a parameter is modulated but has no knob bound to it.
    /// Equal to paramDefaultNorm except where ParamDesc::hasModBase is set.
    std::vector<float>    paramModBaseNorm;
    /// Recomputed once per control block from the exposed parameters and
    /// macros. Shared by every voice, because exposed parameters are global.
    std::vector<float>    baseNorm;
};

/// Where a voice's audio lands in the global section.
struct VoiceTap {
    int   voicePlanIndex  = -1;
    int   globalPlanIndex = -1;
    float gain            = 1.0f;
};

struct RtModRoute {
    bool  srcGlobal    = false;
    int   srcPlanIndex = -1;
    bool  dstGlobal    = false;
    int   dstFlatParam = -1;   ///< index into the destination scope's flat arrays
    float depth        = 0.0f;
    Taper curve        = Taper::Linear;
    bool  bipolar      = false;
};

/// One exposed parameter driving one module parameter.
/// moduleValue = exposedEngineeringValue * scale + offset
struct RtBind {
    int   exposedIndex = -1;
    bool  dstGlobal    = false;
    int   dstFlatParam = -1;
    float scale        = 1.0f;
    float offset       = 0.0f;
};

/// A macro offsets an exposed parameter in that parameter's normalised domain.
struct RtMacroRoute {
    int   macroIndex   = -1;
    int   exposedIndex = -1;
    float depth        = 0.0f;
    Taper curve        = Taper::Linear;
};

struct Voice {
    std::vector<std::unique_ptr<Module>> modules;
    std::vector<float> inBuf;     ///< nodes * kNumChannels * kControlBlock
    std::vector<float> outBuf;
    std::vector<float> srcValue;  ///< one modOut per node, captured last block
    std::vector<float> modAccum;  ///< totalParams

    VoiceContext ctx;
    bool     active   = false;
    bool     held     = false;
    int      note     = -1;
    uint64_t order    = 0;
    float    fade     = 1.0f;   ///< anti-click ramp used when a voice is stolen
    float    fadeInc  = 0.0f;
    int      quietBlocks = 0;
};

class GraphInstance {
public:
    ~GraphInstance();

    // --- audio thread ------------------------------------------------------

    /// Renders `numSamples` frames, replacing the contents of out[0..1].
    void process(float* const* out, int numSamples) noexcept;

    void noteOn(int midiNote, float velocity) noexcept;
    void noteOff(int midiNote) noexcept;
    void allNotesOff(bool immediate) noexcept;
    void sustainPedal(bool down) noexcept;

    void setPitchBendSemis(float semis) noexcept { pitchBend_ = clampT(semis, -48.0f, 48.0f); }
    void setModWheel(float v)   noexcept { modWheel_ = clamp01(v); }
    void setAftertouch(float v) noexcept { aftertouch_ = clamp01(v); }
    void setTransport(double bpm, bool playing) noexcept {
        global_.bpm = clampT(bpm, 20.0, 300.0);
        global_.playing = playing;
    }

    /// Normalised 0..1, indexed as in ir().params / ir().macros.
    void setExposedParam(int index, float norm) noexcept;
    void setMacro(int index, float norm) noexcept;

    void reset() noexcept;

    /// Peak level of the last processed block, for the UI meter.
    float lastPeak() const noexcept { return lastPeak_; }
    /// Set when the NaN guard fired. Sticky; the UI surfaces it once.
    bool  nanGuardTripped() const noexcept { return nanTripped_; }

    int activeVoiceCount() const noexcept;

    // --- worker thread -----------------------------------------------------

    const ir::Instrument& ir()  const noexcept { return ir_; }
    const BakedAssets&    assets() const noexcept { return assets_; }
    double sampleRate() const noexcept { return global_.sampleRate; }

private:
    friend class GraphBuilder;
    GraphInstance() = default;

    void processControlBlock(int numSamples) noexcept;
    void resolveBaseParams() noexcept;
    void resolveVoiceMod(Voice&) noexcept;
    void resolveGlobalMod() noexcept;
    void renderVoice(Voice&, int numSamples) noexcept;
    void renderGlobal(int numSamples) noexcept;
    int  allocateVoice(int midiNote) noexcept;

    float* voiceIn (Voice& v, int node) noexcept {
        return v.inBuf.data()  + static_cast<size_t>(node) * kNumChannels * kControlBlock;
    }
    float* voiceOut(Voice& v, int node) noexcept {
        return v.outBuf.data() + static_cast<size_t>(node) * kNumChannels * kControlBlock;
    }
    float* globalIn (int node) noexcept {
        return globalInBuf_.data()  + static_cast<size_t>(node) * kNumChannels * kControlBlock;
    }
    float* globalOut(int node) noexcept {
        return globalOutBuf_.data() + static_cast<size_t>(node) * kNumChannels * kControlBlock;
    }
    float* voiceBus(int globalNode) noexcept {
        return voiceBus_.data() + static_cast<size_t>(globalNode) * kNumChannels * kControlBlock;
    }

    // --- plan (immutable once built) ---
    ir::Instrument            ir_;
    BakedAssets               assets_;
    ScopePlan                 voicePlan_, globalPlan_;
    std::vector<VoiceTap>     voiceTaps_;
    std::vector<RtModRoute>   modRoutes_;
    std::vector<RtBind>       binds_;
    std::vector<RtMacroRoute> macroRoutes_;
    int                       masterPlanIndex_ = -1;
    GlobalContext             global_;

    // Exposed-parameter ranges, cached so the audio thread never reads ir_.
    std::vector<float> exposedMin_, exposedMax_;
    std::vector<Taper> exposedTaper_;
    /// Scratch: exposed values after macro offsets. Sized once at build time.
    std::vector<float> effExposed_;

    // --- mutable state ---
    std::vector<Voice>                   voices_;
    std::vector<std::unique_ptr<Module>> globalModules_;
    std::vector<float>                   globalInBuf_, globalOutBuf_, voiceBus_;
    std::vector<float>                   globalSrcValue_, globalModAccum_;
    std::vector<float>                   exposedNorm_, macroNorm_;

    float    pitchBend_ = 0.0f, modWheel_ = 0.0f, aftertouch_ = 0.0f;
    bool     sustain_   = false;
    uint64_t noteCounter_ = 0;
    float    glideCoeff_ = 1.0f;
    float    lastPeak_   = 0.0f;
    bool     nanTripped_ = false;
    bool     baseDirty_  = true;
};

} // namespace forge
