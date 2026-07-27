// ---------------------------------------------------------------------------
// The DSP primitive contract (SPEC 4.3).
//
// Every generated instrument is built exclusively from objects implementing
// this interface. They are hand-written, compiled ahead of time and unit
// tested - the LLM chooses and wires them, it never authors them.
//
// Threading contract, enforced by review and by the tests:
//   prepare / reset  -> worker thread only, may allocate.
//   setParam/process -> audio thread only, must be allocation- and lock-free.
// ---------------------------------------------------------------------------
#pragma once

#include "core/Common.h"
#include "core/dsp/Assets.h"

#include <functional>
#include <memory>
#include <nlohmann/json.hpp>
#include <string>
#include <vector>

namespace forge {

// --- Contexts --------------------------------------------------------------

struct GlobalContext {
    double   sampleRate = 48000.0;
    double   bpm        = 120.0;
    bool     playing    = false;
    uint64_t seed       = 1;
};

/// Per-voice performance state. Global-scope nodes receive nullptr.
struct VoiceContext {
    float baseFreqHz     = 440.0f;
    float velocity       = 1.0f;   ///< 0..1
    float gate           = 0.0f;   ///< 1 while the key is held
    float keyTrack01     = 0.5f;   ///< note / 127
    float pitchBendSemis = 0.0f;
    float modWheel       = 0.0f;   ///< 0..1
    float aftertouch     = 0.0f;   ///< 0..1
    float glideFreqHz    = 440.0f; ///< portamento-smoothed frequency
};

struct ProcessArgs {
    const float* const*  in         = nullptr; ///< 2 channels, always valid (zeroed if unconnected)
    float* const*        out        = nullptr; ///< 2 channels
    int                  numSamples = 0;       ///< always <= kControlBlock
    const GlobalContext* g          = nullptr;
    const VoiceContext*  v          = nullptr; ///< nullptr for Scope::Global
};

struct ModulePrepareInfo {
    double                sampleRate     = 48000.0;
    int                   maxBlock       = kControlBlock;
    const nlohmann::json* settings       = nullptr; ///< validated settings object
    const BakedAssets*    assets         = nullptr;
    uint64_t              seed           = 1;
    Scope                 scope          = Scope::Voice;
};

// --- Module ----------------------------------------------------------------

class Module {
public:
    virtual ~Module() = default;

    /// Worker thread. Allocate everything here; process() must never allocate.
    virtual void prepare(const ModulePrepareInfo&) = 0;

    /// Audio thread. Clears history without reallocating.
    virtual void reset() noexcept = 0;

    /// Audio thread. `value` is already in engineering units and already
    /// range-clamped by the graph.
    virtual void setParam(int index, float value) noexcept = 0;

    /// Audio thread. Reads `in`, writes `out`. Must be allocation-free,
    /// lock-free and must not produce NaN/Inf for any finite input.
    virtual void process(const ProcessArgs&) noexcept = 0;

    /// Value published to the modulation matrix, read on the next control
    /// block. Range is -1..1 for bipolar sources, 0..1 for unipolar.
    virtual float modOut() const noexcept { return 0.0f; }

    // --- Voice lifecycle. Only meaningful for Scope::Voice modules. ---
    virtual void noteOn(float /*freqHz*/, float /*velocity*/) noexcept {}
    virtual void noteOff() noexcept {}

    /// True while this module still needs the voice kept alive. Envelopes
    /// return true until they have fully released; everything else returns
    /// false. The VoiceManager frees a voice only when no module is holding it.
    virtual bool holdsVoice() const noexcept { return false; }
};

using ModuleFactory = std::function<std::unique_ptr<Module>()>;

// --- Manifests -------------------------------------------------------------
//
// The manifest is the single source of truth for what a module can do. It
// drives three things at once, which is why nothing here is hand-duplicated:
//   1. the validator (legal settings, legal ranges, legal mod targets),
//   2. the capability manifest injected into the LLM prompt (SPEC 6.5),
//   3. the UI's default labels and tapers.

struct ParamDesc {
    std::string id;
    std::string label;
    std::string unit;
    float       min         = 0.0f;
    float       max         = 1.0f;
    float       def         = 0.0f;
    Taper       taper       = Taper::Linear;
    bool        modulatable = true;
    std::string help;

    /// Modulation is additive on top of a base value. For most parameters the
    /// base should be the plain default - modulating a filter cutoff means
    /// "sweep away from where the knob is". But for a VCA gain it means the
    /// opposite: an amplitude envelope has to start from silence, or the
    /// envelope does nothing because the base is already at maximum.
    ///
    /// When `hasModBase` is set, a parameter that is modulated AND has no knob
    /// bound to it uses `modBase` as its base instead of `def`.
    bool  hasModBase = false;
    float modBase    = 0.0f;
};

struct SettingDesc {
    enum class Type { Enum, Float, Int, Bool, AssetWavetable, AssetCurve, AssetEnvelope };

    std::string              id;
    Type                     type = Type::Float;
    float                    min  = 0.0f;
    float                    max  = 1.0f;
    std::vector<std::string> options;   ///< for Type::Enum
    nlohmann::json           def;
    std::string              help;
};

struct ModuleManifest {
    std::string type;      ///< e.g. "osc.analog"
    std::string category;  ///< source | modulator | processor | effect | utility | output
    std::string summary;   ///< one line, shown to the LLM

    bool allowVoice  = true;
    bool allowGlobal = false;

    int  audioIns  = 0;
    int  audioOuts = 1;

    bool isModSource = false;  ///< publishes modOut()
    bool holdsVoice  = false;  ///< envelope-like; keeps a voice alive

    /// Relative CPU cost, used by the budget check (SPEC 8.4). Measured by
    /// tools/benchmark, not guessed.
    float costWeight = 1.0f;

    std::vector<ParamDesc>   params;
    std::vector<SettingDesc> settings;

    ModuleFactory factory;

    int indexOfParam(const std::string& id) const {
        for (size_t i = 0; i < params.size(); ++i)
            if (params[i].id == id) return static_cast<int>(i);
        return -1;
    }
    const SettingDesc* findSetting(const std::string& id) const {
        for (const auto& s : settings) if (s.id == id) return &s;
        return nullptr;
    }
};

} // namespace forge
