// ---------------------------------------------------------------------------
// Forge built-in DSP primitives.
//
// Every class here is hand-written, compiled ahead of time and unit tested.
// The LLM selects and wires them; it never writes DSP. That separation is what
// lets generated instruments be open-ended AND provably safe (SPEC 6).
//
// Each primitive is declared immediately next to its manifest so that the
// parameter enum and the manifest ordering can never drift apart.
// ---------------------------------------------------------------------------
#include "core/dsp/ModuleKit.h"
#include "core/dsp/Registry.h"

#include <array>
#include <cstring>

namespace forge {
namespace {

// Manifest builders, settings readers, DelayLine and ParamHolder all live in
// ModuleKit.h so the effects library can share them.
using namespace kit;

// ===========================================================================
// SOURCES
// ===========================================================================

// --- osc.analog ------------------------------------------------------------
class OscAnalog final : public ParamHolder<5> {
public:
    enum { kLevel, kDetune, kPulseWidth, kFineCents, kPitchModSemis };

    void prepare(const ModulePrepareInfo& info) override {
        sr_        = info.sampleRate;
        wave_      = getStr(info.settings, "wave", "saw");
        unison_    = clampT(getI(info.settings, "unison", 1), 1, 7);
        octave_    = clampT(getI(info.settings, "octave", 0), -4, 4);
        semis_     = clampT(getI(info.settings, "semitones", 0), -24, 24);
        retrigger_ = getB(info.settings, "retrigger", true);
        rng_.reseed(info.seed ^ 0xA1B2C3D4ull);
        reset();
        // A fixed random phase spread per unison voice keeps the stack from
        // starting phase-aligned (which sounds thin and clicks on retrigger).
        for (int i = 0; i < 7; ++i) startPhase_[i] = rng_.nextUnipolar();
    }

    void reset() noexcept override {
        for (int i = 0; i < 7; ++i) { phase_[i] = startPhase_[i]; triState_[i] = 0.0f; }
    }

    void noteOn(float, float) noexcept override {
        if (retrigger_) reset();
    }

    void process(const ProcessArgs& a) noexcept override {
        const float level  = clampT(p(kLevel), 0.0f, 1.0f);
        const float pw     = clampT(p(kPulseWidth), 0.02f, 0.98f);
        const float spread = clampT(p(kDetune), 0.0f, 60.0f);
        const float semiOffset = static_cast<float>(octave_ * 12 + semis_)
                               + p(kFineCents) * 0.01f
                               + clampT(p(kPitchModSemis), -48.0f, 48.0f)
                               + (a.v ? a.v->pitchBendSemis : 0.0f);

        const float base = (a.v ? a.v->glideFreqHz : 220.0f) * std::pow(2.0f, semiOffset / 12.0f);
        const float f0   = clampT(base, 0.05f, static_cast<float>(sr_) * 0.48f);

        // Equal-power-ish normalisation so unison does not get louder.
        const float norm = level / std::sqrt(static_cast<float>(unison_));

        for (int u = 0; u < unison_; ++u) {
            // Symmetric detune spread in cents: -spread .. +spread.
            const float pos    = (unison_ == 1) ? 0.0f
                               : (2.0f * static_cast<float>(u) / static_cast<float>(unison_ - 1) - 1.0f);
            const float cents  = pos * spread;
            const float f      = clampT(f0 * std::pow(2.0f, cents / 1200.0f),
                                        0.05f, static_cast<float>(sr_) * 0.48f);
            const float dt     = f / static_cast<float>(sr_);
            const float ampL   = 0.5f * (1.0f - pos * 0.7f);
            const float ampR   = 0.5f * (1.0f + pos * 0.7f);

            float ph = phase_[u];
            float tri = triState_[u];

            for (int n = 0; n < a.numSamples; ++n) {
                float s = 0.0f;
                if (wave_ == "sine") {
                    s = std::sin(6.28318530718f * ph);
                } else if (wave_ == "saw") {
                    s = 2.0f * ph - 1.0f - polyBlep(ph, dt);
                } else if (wave_ == "square" || wave_ == "pulse") {
                    const float width = (wave_ == "square") ? 0.5f : pw;
                    s = (ph < width) ? 1.0f : -1.0f;
                    s += polyBlep(ph, dt);
                    float ph2 = ph + (1.0f - width);
                    ph2 -= std::floor(ph2);
                    s -= polyBlep(ph2, dt);
                } else { // triangle: leaky-integrated square is naturally band-limited
                    float sq = (ph < 0.5f) ? 1.0f : -1.0f;
                    sq += polyBlep(ph, dt);
                    float ph2 = ph + 0.5f; ph2 -= std::floor(ph2);
                    sq -= polyBlep(ph2, dt);
                    tri = tri * 0.9995f + 4.0f * dt * sq;
                    s = clampT(tri, -1.5f, 1.5f);
                }

                a.out[0][n] += s * norm * ampL * 2.0f;
                a.out[1][n] += s * norm * ampR * 2.0f;

                ph += dt;
                if (ph >= 1.0f) ph -= 1.0f;
            }
            phase_[u]    = ph;
            triState_[u] = tri;
        }
    }

private:
    double      sr_ = 48000.0;
    std::string wave_ = "saw";
    int         unison_ = 1, octave_ = 0, semis_ = 0;
    bool        retrigger_ = true;
    float       phase_[7]{}, startPhase_[7]{}, triState_[7]{};
    Rng         rng_;
};

ModuleManifest manifestOscAnalog() {
    ModuleManifest m;
    m.type = "osc.analog"; m.category = "source";
    m.summary = "Band-limited analog-style oscillator with up to 7-voice unison. The workhorse for basses, leads and pads.";
    m.allowVoice = true; m.allowGlobal = false;
    m.audioIns = 0; m.audioOuts = 1; m.costWeight = 1.0f;
    m.params = {
        P("level",           "Level",     "",      0.0f,   1.0f,   0.8f, Taper::Linear),
        P("detune_cents",    "Detune",    "cents", 0.0f,  60.0f,   8.0f, Taper::Linear, true,
          "Unison spread. Has no effect when unison is 1."),
        P("pulse_width",     "Width",     "",      0.02f,  0.98f,  0.5f, Taper::Linear, true,
          "Only used when wave is 'pulse'."),
        P("fine_cents",      "Fine",      "cents", -100.0f, 100.0f, 0.0f, Taper::Linear),
        P("pitch_mod_semis", "Pitch Mod", "st",    -48.0f, 48.0f,  0.0f, Taper::Linear, true,
          "Modulation target for vibrato and pitch envelopes."),
    };
    m.settings = {
        SEnum("wave", {"saw", "square", "pulse", "triangle", "sine"}, "saw"),
        SInt ("unison", 1, 7, 1, "More voices = wider and thicker, and proportionally more CPU."),
        SInt ("octave", -4, 4, 0),
        SInt ("semitones", -24, 24, 0),
        SBool("retrigger", true, "Reset phase on note-on. Tight for basses, thin for pads."),
    };
    m.factory = mk<OscAnalog>();
    return m;
}

// --- osc.wavetable ---------------------------------------------------------
class OscWavetable final : public ParamHolder<4> {
public:
    enum { kLevel, kPosition, kFineCents, kPitchModSemis };

    void prepare(const ModulePrepareInfo& info) override {
        sr_     = info.sampleRate;
        octave_ = clampT(getI(info.settings, "octave", 0), -4, 4);
        semis_  = clampT(getI(info.settings, "semitones", 0), -24, 24);
        retrigger_ = getB(info.settings, "retrigger", true);
        const std::string tableId = getStr(info.settings, "table", "");
        table_ = info.assets ? info.assets->findWavetable(tableId) : nullptr;
        reset();
    }

    void reset() noexcept override { phase_ = 0.0f; }
    void noteOn(float, float) noexcept override { if (retrigger_) phase_ = 0.0f; }

    void process(const ProcessArgs& a) noexcept override {
        if (table_ == nullptr) return; // validator guarantees this is rare; be silent, not loud
        const float level = clampT(p(kLevel), 0.0f, 1.0f);
        const float pos   = clamp01(p(kPosition)) * static_cast<float>(table_->numFrames - 1);
        const float semiOffset = static_cast<float>(octave_ * 12 + semis_)
                               + p(kFineCents) * 0.01f
                               + clampT(p(kPitchModSemis), -48.0f, 48.0f)
                               + (a.v ? a.v->pitchBendSemis : 0.0f);
        const float f  = clampT((a.v ? a.v->glideFreqHz : 220.0f)
                                 * std::pow(2.0f, semiOffset / 12.0f),
                                0.05f, static_cast<float>(sr_) * 0.48f);
        const float dt  = f / static_cast<float>(sr_);
        const int   mip = table_->mipForIncrement(dt);

        for (int n = 0; n < a.numSamples; ++n) {
            const float s = table_->read(pos, mip, phase_) * level;
            a.out[0][n] += s;
            a.out[1][n] += s;
            phase_ += dt;
            if (phase_ >= 1.0f) phase_ -= 1.0f;
        }
    }

private:
    double           sr_ = 48000.0;
    const Wavetable* table_ = nullptr;
    int              octave_ = 0, semis_ = 0;
    bool             retrigger_ = true;
    float            phase_ = 0.0f;
};

ModuleManifest manifestOscWavetable() {
    ModuleManifest m;
    m.type = "osc.wavetable"; m.category = "source";
    m.summary = "Plays a generated wavetable asset. Morph across frames with 'position' for evolving timbres.";
    m.allowVoice = true; m.allowGlobal = false;
    m.audioIns = 0; m.audioOuts = 1; m.costWeight = 1.2f;
    m.params = {
        P("level",           "Level",    "",      0.0f, 1.0f, 0.8f),
        P("position",        "Position", "",      0.0f, 1.0f, 0.0f, Taper::Linear, true,
          "Morphs between wavetable frames. A great modulation target."),
        P("fine_cents",      "Fine",     "cents", -100.0f, 100.0f, 0.0f),
        P("pitch_mod_semis", "Pitch Mod","st",    -48.0f, 48.0f, 0.0f),
    };
    m.settings = {
        SAsset("table", SettingDesc::Type::AssetWavetable,
               "Id of a 'wavetable' asset declared in the assets array. Required."),
        SInt ("octave", -4, 4, 0),
        SInt ("semitones", -24, 24, 0),
        SBool("retrigger", true),
    };
    m.factory = mk<OscWavetable>();
    return m;
}

// --- osc.noise -------------------------------------------------------------
class OscNoise final : public ParamHolder<1> {
public:
    enum { kLevel };

    void prepare(const ModulePrepareInfo& info) override {
        color_ = getStr(info.settings, "color", "white");
        rng_.reseed(info.seed ^ 0x5EED1234ull);
        reset();
    }
    void reset() noexcept override {
        std::memset(pink_, 0, sizeof(pink_));
        brown_ = 0.0f;
        dc_.reset();
    }

    void process(const ProcessArgs& a) noexcept override {
        const float level = clampT(p(kLevel), 0.0f, 1.0f);
        for (int n = 0; n < a.numSamples; ++n) {
            float w = rng_.nextBipolar();
            float s = w;
            if (color_ == "pink") {
                // Paul Kellet's economy pink filter.
                pink_[0] = 0.99886f * pink_[0] + w * 0.0555179f;
                pink_[1] = 0.99332f * pink_[1] + w * 0.0750759f;
                pink_[2] = 0.96900f * pink_[2] + w * 0.1538520f;
                pink_[3] = 0.86650f * pink_[3] + w * 0.3104856f;
                pink_[4] = 0.55000f * pink_[4] + w * 0.5329522f;
                pink_[5] = -0.7616f * pink_[5] - w * 0.0168980f;
                s = (pink_[0] + pink_[1] + pink_[2] + pink_[3] + pink_[4] + pink_[5]
                     + pink_[6] + w * 0.5362f) * 0.18f;
                pink_[6] = w * 0.115926f;
            } else if (color_ == "brown") {
                brown_ = clampT(brown_ + w * 0.02f, -1.0f, 1.0f);
                s = brown_ * 3.0f;
            }
            // Pink and brown filters both have a DC component, and feeding DC
            // into a resonator downstream amplifies it by 1/(1-feedback).
            // Block it at the source rather than hoping out.master mops it up.
            if (color_ != "white") s = dc_.process(s);
            s = clampT(s, -1.5f, 1.5f) * level;
            a.out[0][n] += s;
            a.out[1][n] += s;
        }
    }

private:
    std::string color_ = "white";
    Rng       rng_;
    float     pink_[7]{};
    float     brown_ = 0.0f;
    DcBlocker dc_;
};

ModuleManifest manifestOscNoise() {
    ModuleManifest m;
    m.type = "osc.noise"; m.category = "source";
    m.summary = "Noise source. Use for breath, texture, percussive transients and wind.";
    m.allowVoice = true; m.allowGlobal = true;
    m.audioIns = 0; m.audioOuts = 1; m.costWeight = 0.4f;
    m.params = { P("level", "Level", "", 0.0f, 1.0f, 0.3f) };
    m.settings = { SEnum("color", {"white", "pink", "brown"}, "white") };
    m.factory = mk<OscNoise>();
    return m;
}

// --- osc.fm2 ---------------------------------------------------------------
class OscFm2 final : public ParamHolder<5> {
public:
    enum { kLevel, kIndex, kFeedback, kFineCents, kPitchModSemis };

    void prepare(const ModulePrepareInfo& info) override {
        sr_     = info.sampleRate;
        ratio_  = clampT(getF(info.settings, "ratio", 2.0f), 0.25f, 16.0f);
        octave_ = clampT(getI(info.settings, "octave", 0), -4, 4);
        semis_  = clampT(getI(info.settings, "semitones", 0), -24, 24);
        reset();
    }
    void reset() noexcept override { cph_ = mph_ = 0.0f; fbz_ = 0.0f; }
    void noteOn(float, float) noexcept override { reset(); }

    void process(const ProcessArgs& a) noexcept override {
        const float level = clampT(p(kLevel), 0.0f, 1.0f);
        const float index = clampT(p(kIndex), 0.0f, 12.0f);
        const float fb    = clampT(p(kFeedback), 0.0f, 0.9f);
        const float semiOffset = static_cast<float>(octave_ * 12 + semis_)
                               + p(kFineCents) * 0.01f
                               + clampT(p(kPitchModSemis), -48.0f, 48.0f)
                               + (a.v ? a.v->pitchBendSemis : 0.0f);
        const float fc = clampT((a.v ? a.v->glideFreqHz : 220.0f)
                                 * std::pow(2.0f, semiOffset / 12.0f),
                                0.05f, static_cast<float>(sr_) * 0.48f);
        const float fm = clampT(fc * ratio_, 0.05f, static_cast<float>(sr_) * 0.48f);
        const float dc = fc / static_cast<float>(sr_);
        const float dm = fm / static_cast<float>(sr_);

        for (int n = 0; n < a.numSamples; ++n) {
            const float mod = std::sin(6.28318530718f * (mph_ + fb * fbz_));
            fbz_ = mod;
            const float s = std::sin(6.28318530718f * (cph_ + index * mod * 0.15915494f)) * level;
            a.out[0][n] += s;
            a.out[1][n] += s;
            cph_ += dc; if (cph_ >= 1.0f) cph_ -= 1.0f;
            mph_ += dm; if (mph_ >= 1.0f) mph_ -= 1.0f;
        }
    }

private:
    double sr_ = 48000.0;
    float  ratio_ = 2.0f;
    int    octave_ = 0, semis_ = 0;
    float  cph_ = 0.0f, mph_ = 0.0f, fbz_ = 0.0f;
};

ModuleManifest manifestOscFm2() {
    ModuleManifest m;
    m.type = "osc.fm2"; m.category = "source";
    m.summary = "Two-operator FM pair. The cheapest route to bells, metallic tones, electric pianos and gritty basses.";
    m.allowVoice = true; m.allowGlobal = false;
    m.audioIns = 0; m.audioOuts = 1; m.costWeight = 0.9f;
    m.params = {
        P("level",           "Level",    "",   0.0f,  1.0f,  0.8f),
        P("index",           "FM Index", "",   0.0f, 12.0f,  2.0f, Taper::Exp, true,
          "Modulation depth. Higher = brighter and more inharmonic. Great envelope target."),
        P("feedback",        "Feedback", "",   0.0f,  0.9f,  0.0f),
        P("fine_cents",      "Fine",     "cents", -100.0f, 100.0f, 0.0f),
        P("pitch_mod_semis", "Pitch Mod","st", -48.0f, 48.0f, 0.0f),
    };
    m.settings = {
        SFloat("ratio", 0.25f, 16.0f, 2.0f,
               "Modulator:carrier frequency ratio. Whole numbers are harmonic, fractions are bell-like."),
        SInt("octave", -4, 4, 0),
        SInt("semitones", -24, 24, 0),
    };
    m.factory = mk<OscFm2>();
    return m;
}

// ===========================================================================
// MODULATORS
// ===========================================================================

// --- env.adsr --------------------------------------------------------------
class EnvAdsr final : public ParamHolder<5> {
public:
    enum { kAttack, kDecay, kSustain, kRelease, kVelAmount };

    void prepare(const ModulePrepareInfo& info) override {
        sr_ = info.sampleRate;
        expCurve_ = getStr(info.settings, "curve", "exp") == "exp";
        reset();
    }

    void reset() noexcept override { stage_ = Idle; value_ = 0.0f; vel_ = 1.0f; }

    void noteOn(float, float velocity) noexcept override {
        vel_   = clamp01(velocity);
        stage_ = Attack;
    }
    void noteOff() noexcept override {
        if (stage_ != Idle) stage_ = Release;
    }

    bool holdsVoice() const noexcept override { return stage_ != Idle; }
    float modOut() const noexcept override {
        const float velScale = lerp(1.0f, vel_, clamp01(p(kVelAmount)));
        return clamp01(value_) * velScale;
    }

    void process(const ProcessArgs& a) noexcept override {
        // Envelopes advance at control rate. With kControlBlock = 16 that is a
        // 0.33 ms grid at 48 kHz, so even a 1 ms attack gets sampled properly.
        const float dt = static_cast<float>(a.numSamples) / static_cast<float>(sr_);
        switch (stage_) {
            case Attack: {
                const float t = std::max(p(kAttack), 0.2f) * 0.001f;
                value_ += dt / t;
                if (value_ >= 1.0f) { value_ = 1.0f; stage_ = Decay; }
                break;
            }
            case Decay: {
                const float s = clamp01(p(kSustain));
                const float t = std::max(p(kDecay), 0.5f) * 0.001f;
                value_ = approach(value_, s, dt, t);
                if (std::abs(value_ - s) < 1.0e-4f) { value_ = s; stage_ = Sustain; }
                break;
            }
            case Sustain:
                value_ = clamp01(p(kSustain));
                break;
            case Release: {
                const float t = std::max(p(kRelease), 0.5f) * 0.001f;
                value_ = approach(value_, 0.0f, dt, t);
                if (value_ < 1.0e-4f) { value_ = 0.0f; stage_ = Idle; }
                break;
            }
            case Idle:
            default:
                value_ = 0.0f;
                break;
        }
    }

private:
    enum Stage { Idle, Attack, Decay, Sustain, Release };

    float approach(float from, float to, float dt, float timeConstSec) const noexcept {
        if (expCurve_) {
            // 99% of the way in the stated time - the shape musicians expect.
            const float k = 1.0f - std::exp(-4.6f * dt / std::max(timeConstSec, 1.0e-5f));
            return from + (to - from) * clamp01(k);
        }
        const float step = dt / std::max(timeConstSec, 1.0e-5f);
        return (to > from) ? std::min(to, from + step) : std::max(to, from - step);
    }

    double sr_ = 48000.0;
    bool   expCurve_ = true;
    Stage  stage_ = Idle;
    float  value_ = 0.0f, vel_ = 1.0f;
};

ModuleManifest manifestEnvAdsr() {
    ModuleManifest m;
    m.type = "env.adsr"; m.category = "modulator";
    m.summary = "Classic ADSR envelope. Every instrument needs at least one driving a vca, or notes never stop.";
    m.allowVoice = true; m.allowGlobal = false;
    m.audioIns = 0; m.audioOuts = 0; m.isModSource = true; m.holdsVoice = true;
    m.costWeight = 0.15f;
    m.params = {
        P("attack_ms",       "Attack",   "ms", 0.2f,  8000.0f,    5.0f, Taper::Log),
        P("decay_ms",        "Decay",    "ms", 0.5f,  8000.0f,  250.0f, Taper::Log),
        P("sustain",         "Sustain",  "",   0.0f,     1.0f,    0.7f, Taper::Linear),
        P("release_ms",      "Release",  "ms", 0.5f, 12000.0f,  350.0f, Taper::Log),
        P("velocity_amount", "Vel Amt",  "",   0.0f,     1.0f,    0.4f, Taper::Linear,  true,
          "How much MIDI velocity scales the envelope output."),
    };
    m.settings = { SEnum("curve", {"exp", "linear"}, "exp") };
    m.factory = mk<EnvAdsr>();
    return m;
}

// --- env.multi -------------------------------------------------------------
class EnvMulti final : public ParamHolder<2> {
public:
    enum { kTimeScale, kVelAmount };

    void prepare(const ModulePrepareInfo& info) override {
        sr_    = info.sampleRate;
        loop_  = getB(info.settings, "loop", false);
        shape_ = info.assets ? info.assets->findEnvelope(getStr(info.settings, "shape", "")) : nullptr;
        reset();
    }
    void reset() noexcept override {
        stage_ = -1; value_ = 0.0f; stageStart_ = 0.0f; t_ = 0.0f; vel_ = 1.0f; released_ = false;
    }
    void noteOn(float, float velocity) noexcept override {
        vel_ = clamp01(velocity); stage_ = 0; t_ = 0.0f; stageStart_ = value_; released_ = false;
    }
    void noteOff() noexcept override { released_ = true; }

    bool holdsVoice() const noexcept override { return stage_ >= 0; }
    float modOut() const noexcept override {
        return clamp01(value_) * lerp(1.0f, vel_, clamp01(p(kVelAmount)));
    }

    void process(const ProcessArgs& a) noexcept override {
        if (shape_ == nullptr || shape_->stages.empty() || stage_ < 0) { value_ = 0.0f; return; }
        const float scale = clampT(p(kTimeScale), 0.05f, 20.0f);
        const float dt    = static_cast<float>(a.numSamples) / static_cast<float>(sr_);

        int idx = clampT(stage_, 0, static_cast<int>(shape_->stages.size()) - 1);
        const auto& st = shape_->stages[static_cast<size_t>(idx)];
        const float dur = std::max(st.timeMs * 0.001f * scale, 1.0e-4f);

        t_ += dt;
        float u = clamp01(t_ / dur);
        if (st.curve == Taper::Exp)      u = u * u;
        else if (st.curve == Taper::Log) u = std::sqrt(u);
        value_ = lerp(stageStart_, st.level, u);

        const bool sustainHere = (shape_->sustainStage == idx) && !released_;
        if (t_ >= dur && !sustainHere) {
            stageStart_ = st.level;
            t_ = 0.0f;
            ++stage_;
            if (stage_ >= static_cast<int>(shape_->stages.size())) {
                if (shape_->loop || loop_) { stage_ = 0; }
                else { stage_ = -1; value_ = shape_->stages.back().level; }
            }
        }
        if (sustainHere) t_ = dur;
    }

private:
    double               sr_ = 48000.0;
    const EnvelopeShape* shape_ = nullptr;
    bool                 loop_ = false, released_ = false;
    int                  stage_ = -1;
    float                value_ = 0.0f, stageStart_ = 0.0f, t_ = 0.0f, vel_ = 1.0f;
};

ModuleManifest manifestEnvMulti() {
    ModuleManifest m;
    m.type = "env.multi"; m.category = "modulator";
    m.summary = "Multi-stage envelope following a generated 'envelope' asset. Use for complex plucks, swells and evolving shapes.";
    m.allowVoice = true; m.allowGlobal = false;
    m.audioIns = 0; m.audioOuts = 0; m.isModSource = true; m.holdsVoice = true;
    m.costWeight = 0.2f;
    m.params = {
        P("time_scale",      "Time",    "x", 0.05f, 20.0f, 1.0f, Taper::Log),
        P("velocity_amount", "Vel Amt", "",  0.0f,   1.0f, 0.4f),
    };
    m.settings = {
        SAsset("shape", SettingDesc::Type::AssetEnvelope,
               "Id of an 'envelope' asset declared in the assets array. Required."),
        SBool("loop", false),
    };
    m.factory = mk<EnvMulti>();
    return m;
}

// --- lfo -------------------------------------------------------------------
class Lfo final : public ParamHolder<3> {
public:
    enum { kRateHz, kPhaseOffset, kSmooth };

    void prepare(const ModulePrepareInfo& info) override {
        sr_        = info.sampleRate;
        wave_      = getStr(info.settings, "wave", "sine");
        bipolar_   = getB(info.settings, "bipolar", true);
        retrigger_ = getB(info.settings, "retrigger", false);
        sync_      = getB(info.settings, "sync", false);
        division_  = getStr(info.settings, "division", "1/4");
        rng_.reseed(info.seed ^ 0x10F0ull);
        smoother_.setCutoff(20.0f, sr_);
        reset();
    }
    void reset() noexcept override { phase_ = 0.0f; sh_ = rng_.nextBipolar(); out_ = 0.0f; smoother_.reset(0.0f); }
    void noteOn(float, float) noexcept override { if (retrigger_) { phase_ = 0.0f; } }

    float modOut() const noexcept override { return out_; }

    void process(const ProcessArgs& a) noexcept override {
        float rate = clampT(p(kRateHz), 0.005f, 60.0f);
        if (sync_ && a.g) rate = syncedRate(a.g->bpm);

        const float dt = rate * static_cast<float>(a.numSamples) / static_cast<float>(sr_);
        const float prev = phase_;
        phase_ += dt;
        if (phase_ >= 1.0f) {
            phase_ -= std::floor(phase_);
            sh_ = rng_.nextBipolar();  // sample & hold / random step on wrap
        }
        (void) prev;

        float ph = phase_ + clamp01(p(kPhaseOffset));
        ph -= std::floor(ph);

        float v = 0.0f;
        if (wave_ == "sine")        v = std::sin(6.28318530718f * ph);
        else if (wave_ == "tri")    v = 4.0f * std::abs(ph - 0.5f) - 1.0f;
        else if (wave_ == "saw")    v = 1.0f - 2.0f * ph;
        else if (wave_ == "ramp")   v = 2.0f * ph - 1.0f;
        else if (wave_ == "square") v = (ph < 0.5f) ? 1.0f : -1.0f;
        else if (wave_ == "sh")     v = sh_;
        else if (wave_ == "random") v = sh_;

        // 'smooth' rounds off steps - turns S&H into a wandering drift source.
        const float sm = clamp01(p(kSmooth));
        if (sm > 0.001f) {
            smoother_.setCutoff(lerp(30.0f, 0.4f, sm), sr_);
            v = smoother_.process(v);
        }

        out_ = bipolar_ ? clampT(v, -1.0f, 1.0f) : clamp01(v * 0.5f + 0.5f);
    }

private:
    float syncedRate(double bpm) const noexcept {
        const double beats = beatsForDivision();
        const double secondsPerBeat = 60.0 / clampT(bpm, 20.0, 300.0);
        return static_cast<float>(1.0 / std::max(0.01, beats * secondsPerBeat));
    }
    double beatsForDivision() const noexcept {
        if (division_ == "1/1")  return 4.0;
        if (division_ == "1/2")  return 2.0;
        if (division_ == "1/4")  return 1.0;
        if (division_ == "1/8")  return 0.5;
        if (division_ == "1/16") return 0.25;
        if (division_ == "1/4T") return 1.0 / 1.5;
        if (division_ == "1/8T") return 0.5 / 1.5;
        if (division_ == "1/4D") return 1.5;
        if (division_ == "1/8D") return 0.75;
        return 1.0;
    }

    double      sr_ = 48000.0;
    std::string wave_ = "sine", division_ = "1/4";
    bool        bipolar_ = true, retrigger_ = false, sync_ = false;
    float       phase_ = 0.0f, sh_ = 0.0f, out_ = 0.0f;
    OnePole     smoother_;
    Rng         rng_;
};

ModuleManifest manifestLfo() {
    ModuleManifest m;
    m.type = "lfo"; m.category = "modulator";
    m.summary = "Low frequency oscillator. Free-running or tempo-synced. Use for vibrato, filter motion, tremolo and drift.";
    m.allowVoice = true; m.allowGlobal = true;
    m.audioIns = 0; m.audioOuts = 0; m.isModSource = true;
    m.costWeight = 0.15f;
    m.params = {
        P("rate_hz",      "Rate",   "Hz", 0.005f, 60.0f, 2.0f, Taper::Log, true,
          "Ignored when the 'sync' setting is true."),
        P("phase_offset", "Phase",  "",   0.0f,    1.0f, 0.0f),
        P("smooth",       "Smooth", "",   0.0f,    1.0f, 0.0f, Taper::Linear, true,
          "Rounds off steps. Turns sample-and-hold into slow analog-style drift."),
    };
    m.settings = {
        SEnum("wave", {"sine", "tri", "saw", "ramp", "square", "sh", "random"}, "sine"),
        SBool("bipolar", true, "true = -1..1 (vibrato). false = 0..1 (one-directional sweeps)."),
        SBool("retrigger", false),
        SBool("sync", false, "Lock the rate to host tempo using 'division'."),
        SEnum("division", {"1/1","1/2","1/4","1/8","1/16","1/4T","1/8T","1/4D","1/8D"}, "1/4"),
    };
    m.factory = mk<Lfo>();
    return m;
}

// --- MIDI-derived modulation sources --------------------------------------
enum class MidiSourceKind { Velocity, KeyTrack, ModWheel, Aftertouch };

template <MidiSourceKind Kind>
class MidiModSource final : public ParamHolder<1> {
public:
    enum { kBias };
    void prepare(const ModulePrepareInfo&) override { reset(); }
    void reset() noexcept override { out_ = 0.0f; }
    float modOut() const noexcept override { return out_; }

    void process(const ProcessArgs& a) noexcept override {
        float v = 0.0f;
        if (a.v != nullptr) {
            switch (Kind) {
                case MidiSourceKind::Velocity:   v = a.v->velocity;   break;
                case MidiSourceKind::KeyTrack:   v = a.v->keyTrack01; break;
                case MidiSourceKind::ModWheel:   v = a.v->modWheel;   break;
                case MidiSourceKind::Aftertouch: v = a.v->aftertouch; break;
            }
        }
        out_ = clampT(v + p(kBias), -1.0f, 1.0f);
    }
private:
    float out_ = 0.0f;
};

template <MidiSourceKind Kind>
ModuleManifest manifestMidiSource(const char* type, const char* summary) {
    ModuleManifest m;
    m.type = type; m.category = "modulator"; m.summary = summary;
    m.allowVoice = true; m.allowGlobal = false;
    m.audioIns = 0; m.audioOuts = 0; m.isModSource = true;
    m.costWeight = 0.05f;
    m.params = { P("bias", "Bias", "", -1.0f, 1.0f, 0.0f) };
    m.factory = mk<MidiModSource<Kind>>();
    return m;
}

// ===========================================================================
// PROCESSORS
// ===========================================================================

// --- filter.svf ------------------------------------------------------------
class FilterSvf final : public ParamHolder<4> {
public:
    enum { kCutoff, kResonance, kKeyTrack, kDrive };

    void prepare(const ModulePrepareInfo& info) override {
        sr_    = info.sampleRate;
        mode_  = getStr(info.settings, "mode", "lp");
        poles_ = (getStr(info.settings, "slope", "12") == "24") ? 2 : 1;
        reset();
    }
    void reset() noexcept override { for (auto& s : st_) { s.ic1 = s.ic2 = 0.0f; } }

    void process(const ProcessArgs& a) noexcept override {
        float cutoff = clampT(p(kCutoff), 20.0f, 20000.0f);
        const float kt = clamp01(p(kKeyTrack));
        if (kt > 0.001f && a.v != nullptr) {
            // 1.0 = full key tracking: cutoff follows the note, referenced to C4.
            const float ratio = clampT(a.v->baseFreqHz / 261.63f, 0.03f, 32.0f);
            cutoff *= std::pow(ratio, kt);
        }
        cutoff = clampT(cutoff, 20.0f, static_cast<float>(sr_) * 0.45f);

        const float res   = clampT(p(kResonance), 0.0f, 0.98f);
        const float drive = clampT(p(kDrive), 1.0f, 4.0f);

        const float g  = std::tan(3.14159265358979f * cutoff / static_cast<float>(sr_));
        const float k  = 2.0f - 1.96f * res;
        const float a1 = 1.0f / (1.0f + g * (g + k));
        const float a2 = g * a1;
        const float a3 = g * a2;

        for (int ch = 0; ch < kNumChannels; ++ch) {
            for (int n = 0; n < a.numSamples; ++n) {
                float x = a.in[ch][n] * drive;
                if (drive > 1.001f) x = fastTanh(x);
                for (int s = 0; s < poles_; ++s) x = tick(st_[ch * 2 + s], x, a1, a2, a3, k);
                a.out[ch][n] = sanitize(x);
            }
        }
    }

private:
    struct State { float ic1 = 0.0f, ic2 = 0.0f; };

    float tick(State& s, float x, float a1, float a2, float a3, float k) noexcept {
        const float v3 = x - s.ic2;
        const float v1 = a1 * s.ic1 + a2 * v3;
        const float v2 = s.ic2 + a2 * s.ic1 + a3 * v3;
        s.ic1 = clampT(2.0f * v1 - s.ic1, -64.0f, 64.0f);
        s.ic2 = clampT(2.0f * v2 - s.ic2, -64.0f, 64.0f);

        const float lp = v2;
        const float bp = v1;
        const float hp = x - k * v1 - v2;
        if (mode_ == "lp")    return lp;
        if (mode_ == "hp")    return hp;
        if (mode_ == "bp")    return bp;
        if (mode_ == "notch") return hp + lp;
        if (mode_ == "peak")  return hp - lp;
        return lp;
    }

    double      sr_ = 48000.0;
    std::string mode_ = "lp";
    int         poles_ = 1;
    State       st_[4];
};

ModuleManifest manifestFilterSvf() {
    ModuleManifest m;
    m.type = "filter.svf"; m.category = "processor";
    m.summary = "Clean zero-delay state-variable filter. The default choice for shaping any source.";
    m.allowVoice = true; m.allowGlobal = true;
    m.audioIns = 1; m.audioOuts = 1; m.costWeight = 0.8f;
    m.params = {
        P("cutoff",     "Cutoff",   "Hz", 20.0f, 20000.0f, 1200.0f, Taper::Log, true,
          "The single most useful modulation target in the whole system."),
        P("resonance",  "Reso",     "",    0.0f,     0.98f,   0.15f),
        P("keytrack",   "Key Track","",    0.0f,     1.0f,    0.0f, Taper::Linear, true,
          "1.0 makes the cutoff follow pitch, so high notes stay as bright as low ones."),
        P("drive",      "Drive",    "",    1.0f,     4.0f,    1.0f),
    };
    m.settings = {
        SEnum("mode",  {"lp", "hp", "bp", "notch", "peak"}, "lp"),
        SEnum("slope", {"12", "24"}, "12"),
    };
    m.factory = mk<FilterSvf>();
    return m;
}

// --- filter.ladder ---------------------------------------------------------
class FilterLadder final : public ParamHolder<4> {
public:
    enum { kCutoff, kResonance, kDrive, kKeyTrack };

    void prepare(const ModulePrepareInfo& info) override { sr_ = info.sampleRate; reset(); }
    void reset() noexcept override { std::memset(z_, 0, sizeof(z_)); }

    void process(const ProcessArgs& a) noexcept override {
        float cutoff = clampT(p(kCutoff), 20.0f, 20000.0f);
        const float kt = clamp01(p(kKeyTrack));
        if (kt > 0.001f && a.v != nullptr) {
            const float ratio = clampT(a.v->baseFreqHz / 261.63f, 0.03f, 32.0f);
            cutoff *= std::pow(ratio, kt);
        }
        cutoff = clampT(cutoff, 20.0f, static_cast<float>(sr_) * 0.42f);

        const float res   = clampT(p(kResonance), 0.0f, 0.95f);
        const float drive = clampT(p(kDrive), 1.0f, 6.0f);
        const float g  = 1.0f - std::exp(-6.28318530718f * cutoff / static_cast<float>(sr_));
        const float fb = res * 4.2f;

        for (int ch = 0; ch < kNumChannels; ++ch) {
            float* z = z_[ch];
            for (int n = 0; n < a.numSamples; ++n) {
                const float x = a.in[ch][n] * drive;
                // Saturating the feedback node is what keeps a 4-pole ladder
                // stable at high resonance instead of exploding.
                float u = fastTanh(x - fb * z[3]);
                z[0] += g * (u    - z[0]);
                z[1] += g * (z[0] - z[1]);
                z[2] += g * (z[1] - z[2]);
                z[3] += g * (z[2] - z[3]);
                for (int i = 0; i < 4; ++i) z[i] = clampT(sanitize(z[i]), -8.0f, 8.0f);
                a.out[ch][n] = z[3] * (1.0f + res * 0.6f);
            }
        }
    }

private:
    double sr_ = 48000.0;
    float  z_[kNumChannels][4]{};
};

ModuleManifest manifestFilterLadder() {
    ModuleManifest m;
    m.type = "filter.ladder"; m.category = "processor";
    m.summary = "Saturating 4-pole ladder low-pass. Fatter and more coloured than filter.svf - the classic bass filter.";
    m.allowVoice = true; m.allowGlobal = true;
    m.audioIns = 1; m.audioOuts = 1; m.costWeight = 1.1f;
    m.params = {
        P("cutoff",    "Cutoff",    "Hz", 20.0f, 20000.0f, 900.0f, Taper::Log),
        P("resonance", "Reso",      "",    0.0f,     0.95f,  0.2f),
        P("drive",     "Drive",     "",    1.0f,     6.0f,   1.2f),
        P("keytrack",  "Key Track", "",    0.0f,     1.0f,   0.0f),
    };
    m.factory = mk<FilterLadder>();
    return m;
}

// --- filter.comb -----------------------------------------------------------
class FilterComb final : public ParamHolder<3> {
public:
    enum { kFreq, kFeedback, kMix };

    void prepare(const ModulePrepareInfo& info) override {
        sr_ = info.sampleRate;
        const int maxDelay = static_cast<int>(sr_ / 20.0) + 8;
        for (auto& d : lines_) d.prepare(maxDelay);
        reset();
    }
    void reset() noexcept override { for (auto& d : lines_) d.clear(); }

    void process(const ProcessArgs& a) noexcept override {
        const float freq = clampT(p(kFreq), 20.0f, 5000.0f);
        // Hard ceiling on feedback - a comb is a resonator and generated
        // content is not allowed to push it into self-oscillation (SPEC 8.3).
        const float fb   = clampT(p(kFeedback), 0.0f, 0.95f);
        const float mix  = clamp01(p(kMix));
        const float delaySamples = clampT(static_cast<float>(sr_) / freq,
                                          2.0f, static_cast<float>(lines_[0].size() - 2));

        for (int ch = 0; ch < kNumChannels; ++ch) {
            for (int n = 0; n < a.numSamples; ++n) {
                const float x = a.in[ch][n];
                const float d = lines_[ch].read(delaySamples);
                lines_[ch].write(x + d * fb);
                a.out[ch][n] = lerp(x, d, mix);
            }
        }
    }

private:
    double    sr_ = 48000.0;
    DelayLine lines_[kNumChannels];
};

ModuleManifest manifestFilterComb() {
    ModuleManifest m;
    m.type = "filter.comb"; m.category = "processor";
    m.summary = "Tuned comb resonator. Karplus-Strong style plucks, metallic resonance, robotic textures.";
    m.allowVoice = true; m.allowGlobal = true;
    m.audioIns = 1; m.audioOuts = 1; m.costWeight = 0.7f;
    m.params = {
        P("freq",     "Freq",     "Hz", 20.0f, 5000.0f, 220.0f, Taper::Log),
        P("feedback", "Feedback", "",    0.0f,   0.95f,   0.6f),
        P("mix",      "Mix",      "",    0.0f,   1.0f,    1.0f),
    };
    m.factory = mk<FilterComb>();
    return m;
}

// --- shaper.wave -----------------------------------------------------------
class ShaperWave final : public ParamHolder<3> {
public:
    enum { kDrive, kMix, kBits };

    void prepare(const ModulePrepareInfo& info) override {
        type_  = getStr(info.settings, "type", "tanh");
        curve_ = info.assets ? info.assets->findCurve(getStr(info.settings, "curve", "")) : nullptr;
        if (type_ == "curve" && curve_ == nullptr) type_ = "tanh"; // graceful, never silent-broken
        reset();
    }
    void reset() noexcept override { for (auto& d : dc_) d.reset(); }

    void process(const ProcessArgs& a) noexcept override {
        const float drive = clampT(p(kDrive), 1.0f, 20.0f);
        const float mix   = clamp01(p(kMix));
        const float bits  = clampT(p(kBits), 1.0f, 16.0f);
        const float steps = std::pow(2.0f, bits) * 0.5f;

        for (int ch = 0; ch < kNumChannels; ++ch) {
            for (int n = 0; n < a.numSamples; ++n) {
                const float dry = a.in[ch][n];
                const float x   = dry * drive;
                float y;
                if (type_ == "tanh")          y = fastTanh(x);
                else if (type_ == "soft")     y = softClip(x);
                else if (type_ == "fold")     y = std::sin(clampT(x, -8.0f, 8.0f) * 1.5707963f);
                else if (type_ == "bitcrush") y = std::round(clampT(x, -1.0f, 1.0f) * steps) / steps;
                else if (type_ == "curve")    y = curve_->lookup(clampT(x, -1.0f, 1.0f));
                else                          y = fastTanh(x);

                // Asymmetric curves introduce DC. Removing it here is mandatory.
                y = dc_[ch].process(y / std::sqrt(drive));
                a.out[ch][n] = lerp(dry, y, mix);
            }
        }
    }

private:
    std::string  type_ = "tanh";
    const Curve* curve_ = nullptr;
    DcBlocker    dc_[kNumChannels];
};

ModuleManifest manifestShaperWave() {
    ModuleManifest m;
    m.type = "shaper.wave"; m.category = "processor";
    m.summary = "Waveshaper / distortion. 'curve' mode uses a generated transfer curve asset for a bespoke character.";
    m.allowVoice = true; m.allowGlobal = true;
    m.audioIns = 1; m.audioOuts = 1; m.costWeight = 0.5f;
    m.params = {
        P("drive", "Drive", "", 1.0f, 20.0f, 2.0f, Taper::Log),
        P("mix",   "Mix",   "", 0.0f,  1.0f, 1.0f),
        P("bits",  "Bits",  "", 1.0f, 16.0f, 8.0f, Taper::Linear, true,
          "Only used when type is 'bitcrush'."),
    };
    m.settings = {
        SEnum("type", {"tanh", "soft", "fold", "bitcrush", "curve"}, "tanh"),
        SAsset("curve", SettingDesc::Type::AssetCurve,
               "Id of a 'curve' asset. Only needed when type is 'curve'."),
    };
    m.factory = mk<ShaperWave>();
    return m;
}

// --- dyn.compressor --------------------------------------------------------
class DynCompressor final : public ParamHolder<5> {
public:
    enum { kThresholdDb, kRatio, kAttackMs, kReleaseMs, kMakeupDb };

    void prepare(const ModulePrepareInfo& info) override { sr_ = info.sampleRate; reset(); }
    void reset() noexcept override { env_ = 0.0f; }

    void process(const ProcessArgs& a) noexcept override {
        const float thr   = clampT(p(kThresholdDb), -60.0f, 0.0f);
        const float ratio = clampT(p(kRatio), 1.0f, 20.0f);
        const float atk   = std::exp(-1.0f / (std::max(p(kAttackMs), 0.1f)  * 0.001f * static_cast<float>(sr_)));
        const float rel   = std::exp(-1.0f / (std::max(p(kReleaseMs), 1.0f) * 0.001f * static_cast<float>(sr_)));
        const float makeup = dbToGain(clampT(p(kMakeupDb), 0.0f, 24.0f));

        for (int n = 0; n < a.numSamples; ++n) {
            const float peak = std::max(std::abs(a.in[0][n]), std::abs(a.in[1][n]));
            const float coef = (peak > env_) ? atk : rel;
            env_ = peak + coef * (env_ - peak);
            const float db   = gainToDb(std::max(env_, 1.0e-7f));
            const float over = db - thr;
            const float gr   = (over > 0.0f) ? -over * (1.0f - 1.0f / ratio) : 0.0f;
            const float g    = dbToGain(clampT(gr, -48.0f, 0.0f)) * makeup;
            a.out[0][n] = a.in[0][n] * g;
            a.out[1][n] = a.in[1][n] * g;
        }
    }

private:
    double sr_ = 48000.0;
    float  env_ = 0.0f;
};

ModuleManifest manifestDynCompressor() {
    ModuleManifest m;
    m.type = "dyn.compressor"; m.category = "processor";
    m.summary = "Feed-forward peak compressor. Glue, punch and pumping.";
    m.allowVoice = false; m.allowGlobal = true;
    m.audioIns = 1; m.audioOuts = 1; m.costWeight = 0.6f;
    m.params = {
        P("threshold_db", "Threshold", "dB", -60.0f,   0.0f, -18.0f),
        P("ratio",        "Ratio",     ":1",   1.0f,  20.0f,   4.0f, Taper::Log),
        P("attack_ms",    "Attack",    "ms",   0.1f, 200.0f,  10.0f, Taper::Log),
        P("release_ms",   "Release",   "ms",   1.0f,1000.0f, 120.0f, Taper::Log),
        P("makeup_db",    "Makeup",    "dB",   0.0f,  24.0f,   0.0f),
    };
    m.factory = mk<DynCompressor>();
    return m;
}

// --- vca -------------------------------------------------------------------
class Vca final : public ParamHolder<2> {
public:
    enum { kGain, kPan };

    void prepare(const ModulePrepareInfo&) override { reset(); }
    void reset() noexcept override { gain_.reset(0.0f); pan_.reset(0.0f); }

    void process(const ProcessArgs& a) noexcept override {
        // The VCA is where the amp envelope lands, so this is the one place
        // stepped control-rate values would be audible. Ramp per sample.
        gain_.setTarget(clampT(p(kGain), 0.0f, 1.0f), a.numSamples);
        pan_.setTarget(clampT(p(kPan), -1.0f, 1.0f), a.numSamples);

        for (int n = 0; n < a.numSamples; ++n) {
            const float g = gain_.next();
            const float pn = pan_.next();
            const float l = std::sqrt(clamp01(0.5f * (1.0f - pn))) * 1.41421356f;
            const float r = std::sqrt(clamp01(0.5f * (1.0f + pn))) * 1.41421356f;
            a.out[0][n] = a.in[0][n] * g * l;
            a.out[1][n] = a.in[1][n] * g * r;
        }
    }

private:
    Smoother gain_, pan_;
};

ModuleManifest manifestVca() {
    ModuleManifest m;
    m.type = "vca"; m.category = "processor";
    m.summary = "Voltage-controlled amplifier. Drive 'gain' from an env.adsr to give a voice its amplitude shape. Nearly every instrument needs one.";
    m.allowVoice = true; m.allowGlobal = true;
    m.audioIns = 1; m.audioOuts = 1; m.costWeight = 0.2f;
    m.params = {
        P("gain", "Gain", "", 0.0f,  1.0f, 1.0f, Taper::Linear, true,
          "Modulate this with an amplitude envelope."),
        P("pan",  "Pan",  "", -1.0f, 1.0f, 0.0f),
    };
    // Open by default, but silent-by-default the moment an envelope is driving
    // it - otherwise the envelope would be adding on top of an already-open
    // VCA and every note would be a rectangle.
    m.params[0].hasModBase = true;
    m.params[0].modBase    = 0.0f;
    m.factory = mk<Vca>();
    return m;
}

// --- mixer -----------------------------------------------------------------
class Mixer final : public ParamHolder<1> {
public:
    enum { kGain };
    void prepare(const ModulePrepareInfo&) override { reset(); }
    void reset() noexcept override { g_.reset(1.0f); }

    void process(const ProcessArgs& a) noexcept override {
        g_.setTarget(clampT(p(kGain), 0.0f, 2.0f), a.numSamples);
        for (int n = 0; n < a.numSamples; ++n) {
            const float g = g_.next();
            a.out[0][n] = a.in[0][n] * g;
            a.out[1][n] = a.in[1][n] * g;
        }
    }
private:
    Smoother g_;
};

ModuleManifest manifestMixer() {
    ModuleManifest m;
    m.type = "mixer"; m.category = "utility";
    m.summary = "Sums everything routed into it and applies a gain. Connections into any node already sum, so use this only when you need an explicit level control.";
    m.allowVoice = true; m.allowGlobal = true;
    m.audioIns = 1; m.audioOuts = 1; m.costWeight = 0.1f;
    m.params = { P("gain", "Gain", "", 0.0f, 2.0f, 1.0f) };
    m.factory = mk<Mixer>();
    return m;
}

// --- pan -------------------------------------------------------------------
class Pan final : public ParamHolder<2> {
public:
    enum { kPan, kWidth };
    void prepare(const ModulePrepareInfo&) override { reset(); }
    void reset() noexcept override { pan_.reset(0.0f); }

    void process(const ProcessArgs& a) noexcept override {
        pan_.setTarget(clampT(p(kPan), -1.0f, 1.0f), a.numSamples);
        const float width = clampT(p(kWidth), 0.0f, 2.0f);
        for (int n = 0; n < a.numSamples; ++n) {
            const float pn = pan_.next();
            const float mid  = 0.5f * (a.in[0][n] + a.in[1][n]);
            const float side = 0.5f * (a.in[0][n] - a.in[1][n]) * width;
            const float l = (mid + side) * std::sqrt(clamp01(0.5f * (1.0f - pn))) * 1.41421356f;
            const float r = (mid - side) * std::sqrt(clamp01(0.5f * (1.0f + pn))) * 1.41421356f;
            a.out[0][n] = l;
            a.out[1][n] = r;
        }
    }
private:
    Smoother pan_;
};

ModuleManifest manifestPan() {
    ModuleManifest m;
    m.type = "pan"; m.category = "utility";
    m.summary = "Stereo placement and width. width > 1 widens, width 0 collapses to mono.";
    m.allowVoice = true; m.allowGlobal = true;
    m.audioIns = 1; m.audioOuts = 1; m.costWeight = 0.15f;
    m.params = {
        P("pan",   "Pan",   "", -1.0f, 1.0f, 0.0f),
        P("width", "Width", "",  0.0f, 2.0f, 1.0f),
    };
    m.factory = mk<Pan>();
    return m;
}

// --- utility modulation nodes ---------------------------------------------
class UtilConst final : public ParamHolder<1> {
public:
    enum { kValue };
    void prepare(const ModulePrepareInfo&) override {}
    void reset() noexcept override {}
    float modOut() const noexcept override { return clampT(p(kValue), -1.0f, 1.0f); }
    void process(const ProcessArgs&) noexcept override {}
};

ModuleManifest manifestUtilConst() {
    ModuleManifest m;
    m.type = "util.const"; m.category = "utility";
    m.summary = "Constant modulation source. Useful as an offset, or as a macro-controlled amount feeding util.math.";
    m.allowVoice = true; m.allowGlobal = true;
    m.audioIns = 0; m.audioOuts = 0; m.isModSource = true; m.costWeight = 0.02f;
    m.params = { P("value", "Value", "", -1.0f, 1.0f, 1.0f) };
    m.factory = mk<UtilConst>();
    return m;
}

class UtilMath final : public ParamHolder<2> {
public:
    enum { kA, kB };
    void prepare(const ModulePrepareInfo& info) override {
        op_ = getStr(info.settings, "op", "mul"); reset();
    }
    void reset() noexcept override { out_ = 0.0f; }
    float modOut() const noexcept override { return out_; }
    void process(const ProcessArgs&) noexcept override {
        const float a = clampT(p(kA), -1.0f, 1.0f);
        const float b = clampT(p(kB), -1.0f, 1.0f);
        float v = 0.0f;
        if (op_ == "add")      v = a + b;
        else if (op_ == "sub") v = a - b;
        else if (op_ == "mul") v = a * b;
        else if (op_ == "min") v = std::min(a, b);
        else if (op_ == "max") v = std::max(a, b);
        else if (op_ == "abs") v = std::abs(a);
        out_ = clampT(v, -1.0f, 1.0f);
    }
private:
    std::string op_ = "mul";
    float out_ = 0.0f;
};

ModuleManifest manifestUtilMath() {
    ModuleManifest m;
    m.type = "util.math"; m.category = "utility";
    m.summary = "Combines two modulation values. Route sources into 'a' and 'b' and read the result as a modulation source - e.g. an LFO multiplied by an envelope for a fade-in vibrato.";
    m.allowVoice = true; m.allowGlobal = true;
    m.audioIns = 0; m.audioOuts = 0; m.isModSource = true; m.costWeight = 0.03f;
    m.params = {
        P("a", "A", "", -1.0f, 1.0f, 0.0f),
        P("b", "B", "", -1.0f, 1.0f, 0.0f),
    };
    m.settings = { SEnum("op", {"add", "sub", "mul", "min", "max", "abs"}, "mul") };
    m.factory = mk<UtilMath>();
    return m;
}

class UtilSmooth final : public ParamHolder<2> {
public:
    enum { kIn, kTimeMs };
    void prepare(const ModulePrepareInfo& info) override { sr_ = info.sampleRate; reset(); }
    void reset() noexcept override { z_ = 0.0f; }
    float modOut() const noexcept override { return z_; }
    void process(const ProcessArgs& a) noexcept override {
        const float t  = std::max(p(kTimeMs), 1.0f) * 0.001f;
        const float dt = static_cast<float>(a.numSamples) / static_cast<float>(sr_);
        const float k  = clamp01(1.0f - std::exp(-dt / t));
        z_ = clampT(z_ + (clampT(p(kIn), -1.0f, 1.0f) - z_) * k, -1.0f, 1.0f);
    }
private:
    double sr_ = 48000.0;
    float  z_ = 0.0f;
};

ModuleManifest manifestUtilSmooth() {
    ModuleManifest m;
    m.type = "util.smooth"; m.category = "utility";
    m.summary = "Slews a modulation value. Turns a stepped or jumpy source into a gliding one.";
    m.allowVoice = true; m.allowGlobal = true;
    m.audioIns = 0; m.audioOuts = 0; m.isModSource = true; m.costWeight = 0.03f;
    m.params = {
        P("in",      "In",   "",   -1.0f,    1.0f,  0.0f),
        P("time_ms", "Time", "ms",  1.0f, 2000.0f, 50.0f, Taper::Log),
    };
    m.factory = mk<UtilSmooth>();
    return m;
}

// ===========================================================================
// EFFECTS (global scope)
// ===========================================================================

// --- fx.delay --------------------------------------------------------------
class FxDelay final : public ParamHolder<5> {
public:
    enum { kTimeMs, kFeedback, kMix, kDampingHz, kSpread };

    void prepare(const ModulePrepareInfo& info) override {
        sr_       = info.sampleRate;
        pingpong_ = getB(info.settings, "pingpong", false);
        sync_     = getB(info.settings, "sync", false);
        division_ = getStr(info.settings, "division", "1/8");
        const int maxSamples = static_cast<int>(sr_ * 2.2) + 8;
        for (int ch = 0; ch < kNumChannels; ++ch) {
            lines_[ch].prepare(maxSamples);
            damp_[ch].reset();
        }
        reset();
    }
    void reset() noexcept override {
        for (int ch = 0; ch < kNumChannels; ++ch) { lines_[ch].clear(); damp_[ch].reset(); }
    }

    void process(const ProcessArgs& a) noexcept override {
        float timeMs = clampT(p(kTimeMs), 1.0f, 2000.0f);
        if (sync_ && a.g) timeMs = syncedMs(a.g->bpm);

        // Hard feedback ceiling: generated content cannot build a runaway
        // delay (SPEC 8.3).
        const float fb    = clampT(p(kFeedback), 0.0f, 0.92f);
        const float mix   = clamp01(p(kMix));
        const float sprd  = clamp01(p(kSpread));
        const float dampHz = clampT(p(kDampingHz), 200.0f, 18000.0f);
        for (auto& d : damp_) d.setCutoff(dampHz, sr_);

        const float baseDelay = clampT(timeMs * 0.001f * static_cast<float>(sr_),
                                       2.0f, static_cast<float>(lines_[0].size() - 4));
        const float dL = baseDelay;
        const float dR = clampT(baseDelay * (1.0f + sprd * 0.35f),
                                2.0f, static_cast<float>(lines_[0].size() - 4));

        for (int n = 0; n < a.numSamples; ++n) {
            const float inL = a.in[0][n], inR = a.in[1][n];
            const float tL = lines_[0].read(dL);
            const float tR = lines_[1].read(dR);

            if (pingpong_) {
                lines_[0].write(inL + damp_[0].process(tR) * fb);
                lines_[1].write(inR + damp_[1].process(tL) * fb);
            } else {
                lines_[0].write(inL + damp_[0].process(tL) * fb);
                lines_[1].write(inR + damp_[1].process(tR) * fb);
            }
            a.out[0][n] = lerp(inL, tL, mix);
            a.out[1][n] = lerp(inR, tR, mix);
        }
    }

private:
    float syncedMs(double bpm) const noexcept {
        double beats = 0.5;
        if (division_ == "1/1")       beats = 4.0;
        else if (division_ == "1/2")  beats = 2.0;
        else if (division_ == "1/4")  beats = 1.0;
        else if (division_ == "1/8")  beats = 0.5;
        else if (division_ == "1/16") beats = 0.25;
        else if (division_ == "1/4T") beats = 1.0 / 1.5;
        else if (division_ == "1/8T") beats = 0.5 / 1.5;
        else if (division_ == "1/4D") beats = 1.5;
        else if (division_ == "1/8D") beats = 0.75;
        const double ms = beats * (60000.0 / clampT(bpm, 20.0, 300.0));
        return static_cast<float>(clampT(ms, 1.0, 2000.0));
    }

    double      sr_ = 48000.0;
    bool        pingpong_ = false, sync_ = false;
    std::string division_ = "1/8";
    DelayLine   lines_[kNumChannels];
    OnePole     damp_[kNumChannels];
};

ModuleManifest manifestFxDelay() {
    ModuleManifest m;
    m.type = "fx.delay"; m.category = "effect";
    m.summary = "Stereo delay with damping and optional ping-pong. Tempo-syncable.";
    m.allowVoice = false; m.allowGlobal = true;
    m.audioIns = 1; m.audioOuts = 1; m.costWeight = 1.0f;
    m.params = {
        P("time_ms",    "Time",     "ms",   1.0f, 2000.0f,  375.0f, Taper::Log, true,
          "Ignored when 'sync' is true."),
        P("feedback",   "Feedback", "",     0.0f,    0.92f,   0.35f),
        P("mix",        "Mix",      "",     0.0f,    1.0f,    0.25f),
        P("damping_hz", "Damping",  "Hz", 200.0f,18000.0f, 6000.0f, Taper::Log, true,
          "Lower values make each repeat darker."),
        P("spread",     "Spread",   "",     0.0f,    1.0f,    0.2f),
    };
    m.settings = {
        SBool("pingpong", false),
        SBool("sync", false),
        SEnum("division", {"1/1","1/2","1/4","1/8","1/16","1/4T","1/8T","1/4D","1/8D"}, "1/8"),
    };
    m.factory = mk<FxDelay>();
    return m;
}

// --- fx.reverb -------------------------------------------------------------
/// Schroeder/Freeverb topology: 8 parallel damped combs into 4 series allpasses
/// per channel, with a stereo offset. Cheap, stable, and good enough that the
/// generated instruments sit in a mix. A proper FDN is Phase 1 (SPEC 14.1).
class FxReverb final : public ParamHolder<5> {
public:
    enum { kSize, kDamping, kMix, kWidth, kPredelayMs };

    void prepare(const ModulePrepareInfo& info) override {
        sr_ = info.sampleRate;
        const double scale = sr_ / 44100.0;
        static const int combTuning[kNumCombs] =
            { 1116, 1188, 1277, 1356, 1422, 1491, 1557, 1617 };
        static const int apTuning[kNumAllpass] = { 556, 441, 341, 225 };
        const int stereoSpread = static_cast<int>(23 * scale);

        for (int ch = 0; ch < kNumChannels; ++ch) {
            for (int i = 0; i < kNumCombs; ++i) {
                combLen_[ch][i] = static_cast<int>(combTuning[i] * scale) + ch * stereoSpread;
                comb_[ch][i].prepare(combLen_[ch][i] + 4);
                combDamp_[ch][i].reset();
            }
            for (int i = 0; i < kNumAllpass; ++i) {
                apLen_[ch][i] = static_cast<int>(apTuning[i] * scale) + ch * stereoSpread;
                ap_[ch][i].prepare(apLen_[ch][i] + 4);
            }
            pre_[ch].prepare(static_cast<int>(sr_ * 0.25) + 8);
        }
        reset();
    }

    void reset() noexcept override {
        for (int ch = 0; ch < kNumChannels; ++ch) {
            for (auto& c : comb_[ch]) c.clear();
            for (auto& d : combDamp_[ch]) d.reset();
            for (auto& p : ap_[ch]) p.clear();
            pre_[ch].clear();
        }
    }

    void process(const ProcessArgs& a) noexcept override {
        const float size    = clamp01(p(kSize));
        const float damping = clamp01(p(kDamping));
        const float mix     = clamp01(p(kMix));
        const float width   = clamp01(p(kWidth));
        const float feedback = 0.70f + size * 0.28f;   // capped below 1.0 by construction
        const float dampHz  = lerp(16000.0f, 800.0f, damping);
        const float preDelay = clampT(p(kPredelayMs) * 0.001f * static_cast<float>(sr_),
                                      1.0f, static_cast<float>(pre_[0].size() - 4));

        for (int ch = 0; ch < kNumChannels; ++ch)
            for (auto& d : combDamp_[ch]) d.setCutoff(dampHz, sr_);

        for (int n = 0; n < a.numSamples; ++n) {
            const float inL = a.in[0][n], inR = a.in[1][n];
            const float mono = (inL + inR) * 0.25f;

            float wet[kNumChannels] = { 0.0f, 0.0f };
            for (int ch = 0; ch < kNumChannels; ++ch) {
                pre_[ch].write(mono);
                const float src = pre_[ch].read(preDelay);

                float acc = 0.0f;
                for (int i = 0; i < kNumCombs; ++i) {
                    const float d = comb_[ch][i].read(static_cast<float>(combLen_[ch][i]));
                    comb_[ch][i].write(src + combDamp_[ch][i].process(d) * feedback);
                    acc += d;
                }
                acc *= 0.125f;

                for (int i = 0; i < kNumAllpass; ++i) {
                    const float d = ap_[ch][i].read(static_cast<float>(apLen_[ch][i]));
                    ap_[ch][i].write(acc + d * 0.5f);
                    acc = d - acc;
                }
                wet[ch] = clampT(sanitize(acc), -4.0f, 4.0f);
            }

            const float wl = lerp(0.5f * (wet[0] + wet[1]), wet[0], width);
            const float wr = lerp(0.5f * (wet[0] + wet[1]), wet[1], width);
            a.out[0][n] = lerp(inL, wl, mix);
            a.out[1][n] = lerp(inR, wr, mix);
        }
    }

private:
    static constexpr int kNumCombs   = 8;
    static constexpr int kNumAllpass = 4;

    double    sr_ = 48000.0;
    DelayLine comb_[kNumChannels][kNumCombs];
    OnePole   combDamp_[kNumChannels][kNumCombs];
    int       combLen_[kNumChannels][kNumCombs]{};
    DelayLine ap_[kNumChannels][kNumAllpass];
    int       apLen_[kNumChannels][kNumAllpass]{};
    DelayLine pre_[kNumChannels];
};

ModuleManifest manifestFxReverb() {
    ModuleManifest m;
    m.type = "fx.reverb"; m.category = "effect";
    m.summary = "Stereo reverb. Essential for pads and ambience; use sparingly on basses.";
    m.allowVoice = false; m.allowGlobal = true;
    m.audioIns = 1; m.audioOuts = 1; m.costWeight = 2.5f;
    m.params = {
        P("size",        "Size",     "",   0.0f,   1.0f,  0.6f),
        P("damping",     "Damping",  "",   0.0f,   1.0f,  0.4f),
        P("mix",         "Mix",      "",   0.0f,   1.0f,  0.25f),
        P("width",       "Width",    "",   0.0f,   1.0f,  1.0f),
        P("predelay_ms", "Predelay", "ms", 0.0f, 200.0f, 10.0f),
    };
    m.factory = mk<FxReverb>();
    return m;
}

// --- fx.chorus -------------------------------------------------------------
class FxChorus final : public ParamHolder<5> {
public:
    enum { kRateHz, kDepth, kMix, kDelayMs, kFeedback };

    void prepare(const ModulePrepareInfo& info) override {
        sr_ = info.sampleRate;
        for (auto& d : lines_) d.prepare(static_cast<int>(sr_ * 0.08) + 8);
        reset();
    }
    void reset() noexcept override { for (auto& d : lines_) d.clear(); phase_ = 0.0f; }

    void process(const ProcessArgs& a) noexcept override {
        const float rate  = clampT(p(kRateHz), 0.01f, 8.0f);
        const float depth = clamp01(p(kDepth));
        const float mix   = clamp01(p(kMix));
        const float base  = clampT(p(kDelayMs), 1.0f, 40.0f) * 0.001f * static_cast<float>(sr_);
        const float fb    = clampT(p(kFeedback), 0.0f, 0.7f);
        const float inc   = rate / static_cast<float>(sr_);
        const float sweep = base * 0.6f * depth;

        for (int n = 0; n < a.numSamples; ++n) {
            phase_ += inc;
            if (phase_ >= 1.0f) phase_ -= 1.0f;
            const float mL = std::sin(6.28318530718f * phase_);
            const float mR = std::sin(6.28318530718f * (phase_ + 0.25f));

            const float dL = clampT(base + mL * sweep, 2.0f, static_cast<float>(lines_[0].size() - 4));
            const float dR = clampT(base + mR * sweep, 2.0f, static_cast<float>(lines_[1].size() - 4));
            const float tL = lines_[0].read(dL);
            const float tR = lines_[1].read(dR);
            lines_[0].write(a.in[0][n] + tL * fb);
            lines_[1].write(a.in[1][n] + tR * fb);
            a.out[0][n] = lerp(a.in[0][n], tL, mix);
            a.out[1][n] = lerp(a.in[1][n], tR, mix);
        }
    }

private:
    double    sr_ = 48000.0;
    DelayLine lines_[kNumChannels];
    float     phase_ = 0.0f;
};

ModuleManifest manifestFxChorus() {
    ModuleManifest m;
    m.type = "fx.chorus"; m.category = "effect";
    m.summary = "Stereo chorus / ensemble. Instantly widens thin sources; the classic pad thickener.";
    m.allowVoice = false; m.allowGlobal = true;
    m.audioIns = 1; m.audioOuts = 1; m.costWeight = 0.8f;
    m.params = {
        P("rate_hz",  "Rate",     "Hz", 0.01f, 8.0f,  0.6f, Taper::Log),
        P("depth",    "Depth",    "",   0.0f,  1.0f,  0.4f),
        P("mix",      "Mix",      "",   0.0f,  1.0f,  0.4f),
        P("delay_ms", "Delay",    "ms", 1.0f, 40.0f, 12.0f),
        P("feedback", "Feedback", "",   0.0f,  0.7f,  0.0f),
    };
    m.factory = mk<FxChorus>();
    return m;
}

// --- fx.phaser -------------------------------------------------------------
class FxPhaser final : public ParamHolder<5> {
public:
    enum { kRateHz, kDepth, kFeedback, kMix, kCenterHz };

    void prepare(const ModulePrepareInfo& info) override { sr_ = info.sampleRate; reset(); }
    void reset() noexcept override {
        std::memset(z_, 0, sizeof(z_)); phase_ = 0.0f; fbz_[0] = fbz_[1] = 0.0f;
    }

    void process(const ProcessArgs& a) noexcept override {
        const float rate   = clampT(p(kRateHz), 0.01f, 8.0f);
        const float depth  = clamp01(p(kDepth));
        const float fb     = clampT(p(kFeedback), 0.0f, 0.9f);
        const float mix    = clamp01(p(kMix));
        const float center = clampT(p(kCenterHz), 100.0f, 8000.0f);
        const float inc    = rate / static_cast<float>(sr_);

        for (int n = 0; n < a.numSamples; ++n) {
            phase_ += inc; if (phase_ >= 1.0f) phase_ -= 1.0f;
            const float mod = std::sin(6.28318530718f * phase_);
            const float f   = clampT(center * std::pow(2.0f, mod * depth * 2.0f),
                                     30.0f, static_cast<float>(sr_) * 0.4f);
            const float g   = std::tan(3.14159265358979f * f / static_cast<float>(sr_));
            const float c   = (g - 1.0f) / (g + 1.0f);

            for (int ch = 0; ch < kNumChannels; ++ch) {
                float x = a.in[ch][n] + fbz_[ch] * fb;
                for (int s = 0; s < kStages; ++s) {
                    const float y = c * x + z_[ch][s];
                    z_[ch][s] = clampT(sanitize(x - c * y), -16.0f, 16.0f);
                    x = y;
                }
                fbz_[ch] = clampT(sanitize(x), -4.0f, 4.0f);
                a.out[ch][n] = lerp(a.in[ch][n], 0.5f * (a.in[ch][n] + x), mix * 2.0f > 1.0f ? 1.0f : mix * 2.0f);
            }
        }
    }

private:
    static constexpr int kStages = 6;
    double sr_ = 48000.0;
    float  z_[kNumChannels][kStages]{};
    float  fbz_[kNumChannels]{};
    float  phase_ = 0.0f;
};

ModuleManifest manifestFxPhaser() {
    ModuleManifest m;
    m.type = "fx.phaser"; m.category = "effect";
    m.summary = "Six-stage phaser. Sweeping notches for movement on sustained sounds.";
    m.allowVoice = false; m.allowGlobal = true;
    m.audioIns = 1; m.audioOuts = 1; m.costWeight = 0.9f;
    m.params = {
        P("rate_hz",   "Rate",     "Hz",   0.01f,    8.0f,   0.3f, Taper::Log),
        P("depth",     "Depth",    "",     0.0f,     1.0f,   0.7f),
        P("feedback",  "Feedback", "",     0.0f,     0.9f,   0.3f),
        P("mix",       "Mix",      "",     0.0f,     1.0f,   0.5f),
        P("center_hz", "Center",   "Hz", 100.0f,  8000.0f, 700.0f, Taper::Log),
    };
    m.factory = mk<FxPhaser>();
    return m;
}

// --- fx.drive --------------------------------------------------------------
/// 2x oversampled saturation. The upsample/decimate pair is simple but it
/// removes most of the audible aliasing that naive distortion produces.
class FxDrive final : public ParamHolder<4> {
public:
    enum { kDrive, kToneHz, kMix, kOutput };

    void prepare(const ModulePrepareInfo& info) override {
        sr_ = info.sampleRate;
        reset();
    }
    void reset() noexcept override {
        for (int ch = 0; ch < kNumChannels; ++ch) {
            up_[ch] = 0.0f;
            for (auto& s : dec_[ch]) s = 0.0f;
            tone_[ch].reset(); dc_[ch].reset();
        }
    }

    void process(const ProcessArgs& a) noexcept override {
        const float drive = clampT(p(kDrive), 1.0f, 20.0f);
        const float mix   = clamp01(p(kMix));
        const float out   = clampT(p(kOutput), 0.0f, 1.0f);
        const float toneHz = clampT(p(kToneHz), 500.0f, 18000.0f);
        for (auto& t : tone_) t.setCutoff(toneHz, sr_ * 2.0);

        const float comp = 1.0f / std::sqrt(drive);

        for (int ch = 0; ch < kNumChannels; ++ch) {
            for (int n = 0; n < a.numSamples; ++n) {
                const float x = a.in[ch][n];
                float acc = 0.0f;
                // 2x upsample by linear interpolation, shape, low-pass, decimate.
                for (int k = 0; k < 2; ++k) {
                    const float xs = (k == 0) ? 0.5f * (up_[ch] + x) : x;
                    float y = fastTanh(xs * drive) * comp;
                    y = tone_[ch].process(y);
                    acc += y * 0.5f;
                }
                up_[ch] = x;
                const float wet = dc_[ch].process(acc);
                a.out[ch][n] = lerp(x, wet, mix) * out;
            }
        }
    }

private:
    double    sr_ = 48000.0;
    float     up_[kNumChannels]{};
    float     dec_[kNumChannels][2]{};
    OnePole   tone_[kNumChannels];
    DcBlocker dc_[kNumChannels];
};

ModuleManifest manifestFxDrive() {
    ModuleManifest m;
    m.type = "fx.drive"; m.category = "effect";
    m.summary = "Oversampled saturation. Adds harmonics and perceived loudness. Excellent on basses and leads.";
    m.allowVoice = true; m.allowGlobal = true;
    m.audioIns = 1; m.audioOuts = 1; m.costWeight = 1.0f;
    m.params = {
        P("drive",   "Drive",  "",     1.0f,    20.0f,    2.0f, Taper::Log),
        P("tone_hz", "Tone",   "Hz", 500.0f, 18000.0f, 8000.0f, Taper::Log),
        P("mix",     "Mix",    "",     0.0f,     1.0f,    1.0f),
        P("output",  "Output", "",     0.0f,     1.0f,    0.8f),
    };
    m.factory = mk<FxDrive>();
    return m;
}

// --- out.master ------------------------------------------------------------
/// Always present, always last, never removable and never bypassable by
/// generated content. This is the component that makes "the AI wrote it" safe
/// to put through a pair of monitors (SPEC 8.3).
class OutMaster final : public ParamHolder<1> {
public:
    enum { kVolume };

    void prepare(const ModulePrepareInfo& info) override {
        sr_ = info.sampleRate;
        release_ = std::exp(-1.0f / (0.120f * static_cast<float>(sr_)));
        reset();
    }
    void reset() noexcept override {
        for (int ch = 0; ch < kNumChannels; ++ch) dc_[ch].reset();
        gr_ = 1.0f;
        vol_.reset(0.85f);
    }

    void process(const ProcessArgs& a) noexcept override {
        vol_.setTarget(clampT(p(kVolume), 0.0f, 1.2f), a.numSamples);
        const float ceiling = dbToGain(kMasterCeilingDb);

        for (int n = 0; n < a.numSamples; ++n) {
            const float v = vol_.next();
            float l = dc_[0].process(sanitize(a.in[0][n])) * v;
            float r = dc_[1].process(sanitize(a.in[1][n])) * v;

            l = softClip(l);
            r = softClip(r);

            // Peak limiter with instant attack and a smooth release. Combined
            // with the soft clip above, nothing can leave this node hotter
            // than the ceiling.
            const float peak = std::max(std::abs(l), std::abs(r));
            const float want = (peak > ceiling) ? (ceiling / std::max(peak, 1.0e-6f)) : 1.0f;
            gr_ = (want < gr_) ? want : want + release_ * (gr_ - want);
            gr_ = clampT(gr_, 0.0f, 1.0f);

            a.out[0][n] = clampT(l * gr_, -1.0f, 1.0f);
            a.out[1][n] = clampT(r * gr_, -1.0f, 1.0f);
        }
    }

private:
    double    sr_ = 48000.0;
    DcBlocker dc_[kNumChannels];
    Smoother  vol_;
    float     gr_ = 1.0f, release_ = 0.999f;
};

ModuleManifest manifestOutMaster() {
    ModuleManifest m;
    m.type = "out.master"; m.category = "output";
    m.summary = "Mandatory final output stage: DC blocker, soft clip and brickwall limiter. Exactly one exists and it is added automatically - do not declare it yourself.";
    m.allowVoice = false; m.allowGlobal = true;
    m.audioIns = 1; m.audioOuts = 1; m.costWeight = 0.3f;
    m.params = { P("volume", "Volume", "", 0.0f, 1.2f, 0.85f) };
    m.factory = mk<OutMaster>();
    return m;
}

} // namespace

// ---------------------------------------------------------------------------

void registerBuiltinModules(std::vector<ModuleManifest>& out) {
    out.push_back(manifestOscAnalog());
    out.push_back(manifestOscWavetable());
    out.push_back(manifestOscNoise());
    out.push_back(manifestOscFm2());

    out.push_back(manifestEnvAdsr());
    out.push_back(manifestEnvMulti());
    out.push_back(manifestLfo());
    out.push_back(manifestMidiSource<MidiSourceKind::Velocity>(
        "mod.velocity", "MIDI velocity of the note, 0..1."));
    out.push_back(manifestMidiSource<MidiSourceKind::KeyTrack>(
        "mod.keytrack", "Note number scaled to 0..1. Use to make high notes brighter or shorter."));
    out.push_back(manifestMidiSource<MidiSourceKind::ModWheel>(
        "mod.modwheel", "MIDI CC1 mod wheel, 0..1."));
    out.push_back(manifestMidiSource<MidiSourceKind::Aftertouch>(
        "mod.aftertouch", "Channel aftertouch, 0..1."));

    out.push_back(manifestFilterSvf());
    out.push_back(manifestFilterLadder());
    out.push_back(manifestFilterComb());
    out.push_back(manifestShaperWave());
    out.push_back(manifestDynCompressor());
    out.push_back(manifestVca());
    out.push_back(manifestMixer());
    out.push_back(manifestPan());
    out.push_back(manifestUtilConst());
    out.push_back(manifestUtilMath());
    out.push_back(manifestUtilSmooth());

    out.push_back(manifestFxDelay());
    out.push_back(manifestFxReverb());
    out.push_back(manifestFxChorus());
    out.push_back(manifestFxPhaser());
    out.push_back(manifestFxDrive());

    out.push_back(manifestOutMaster());
}

} // namespace forge
