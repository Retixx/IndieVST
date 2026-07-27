// ---------------------------------------------------------------------------
// The production-grade effects and sources library.
//
// Everything here exists for one reason: the difference between a generated
// instrument that sounds like a synthesis demo and one that sounds like a
// record is almost never the oscillator. It is EQ, saturation, width, movement
// and space. These are the modules that let the model reach for those.
//
// Adding them required no changes to the validator, the prompt, or the UI - the
// registry drives all three (SPEC 6.5).
// ---------------------------------------------------------------------------
#include "core/dsp/ModuleKit.h"
#include "core/dsp/Registry.h"

namespace forge {
namespace {

using namespace kit;

constexpr float kTwoPi = 6.28318530717958647692f;

// ===========================================================================
// fx.eq3 - the single most useful "make it sound produced" module
// ===========================================================================
class FxEq3 final : public ParamHolder<7> {
public:
    enum { kLowGain, kLowFreq, kMidGain, kMidFreq, kMidQ, kHighGain, kHighFreq };

    void prepare(const ModulePrepareInfo& info) override { sr_ = info.sampleRate; reset(); }
    void reset() noexcept override {
        for (int ch = 0; ch < kNumChannels; ++ch) {
            low_[ch].reset(); mid_[ch].reset(); high_[ch].reset();
        }
    }

    void process(const ProcessArgs& a) noexcept override {
        for (int ch = 0; ch < kNumChannels; ++ch) {
            low_[ch].setLowShelf(clampT(p(kLowFreq), 30.0f, 1000.0f),
                                 clampT(p(kLowGain), -18.0f, 18.0f), sr_);
            mid_[ch].setPeak(clampT(p(kMidFreq), 100.0f, 12000.0f),
                             clampT(p(kMidQ), 0.2f, 8.0f),
                             clampT(p(kMidGain), -18.0f, 18.0f), sr_);
            high_[ch].setHighShelf(clampT(p(kHighFreq), 1000.0f, 18000.0f),
                                   clampT(p(kHighGain), -18.0f, 18.0f), sr_);

            for (int n = 0; n < a.numSamples; ++n)
                a.out[ch][n] = high_[ch].process(mid_[ch].process(low_[ch].process(a.in[ch][n])));
        }
    }

private:
    double sr_ = 48000.0;
    Biquad low_[kNumChannels], mid_[kNumChannels], high_[kNumChannels];
};

ModuleManifest manifestFxEq3() {
    ModuleManifest m;
    m.type = "fx.eq3"; m.category = "effect";
    m.summary = "Three-band EQ: low shelf, sweepable mid bell, high shelf. Reach for this to carve space, tame mud around 200-400 Hz, or add air above 8 kHz.";
    m.allowVoice = true; m.allowGlobal = true;
    m.audioIns = 1; m.audioOuts = 1; m.costWeight = 0.7f;
    m.params = {
        P("low_gain_db",  "Low",     "dB",  -18.0f,    18.0f,    0.0f),
        P("low_freq",     "Lo Freq", "Hz",   30.0f,  1000.0f,  120.0f, Taper::Log),
        P("mid_gain_db",  "Mid",     "dB",  -18.0f,    18.0f,    0.0f),
        P("mid_freq",     "Mid Frq", "Hz",  100.0f, 12000.0f, 1000.0f, Taper::Log),
        P("mid_q",        "Mid Q",   "",      0.2f,     8.0f,    0.9f, Taper::Log),
        P("high_gain_db", "High",    "dB",  -18.0f,    18.0f,    0.0f),
        P("high_freq",    "Hi Freq", "Hz", 1000.0f, 18000.0f, 6000.0f, Taper::Log),
    };
    m.factory = mk<FxEq3>();
    return m;
}

// ===========================================================================
// filter.crossover - band splitting without multi-output nodes
//
// Two of these fed from the same source, each into its own chain, then summed,
// gives multiband processing using only the existing single-output graph.
// ===========================================================================
class FilterCrossover final : public ParamHolder<1> {
public:
    enum { kFreq };

    void prepare(const ModulePrepareInfo& info) override {
        sr_ = info.sampleRate;
        high_ = getStr(info.settings, "band", "low") == "high";
        reset();
    }
    void reset() noexcept override { for (auto& s : stages_) { s[0].reset(); s[1].reset(); } }

    void process(const ProcessArgs& a) noexcept override {
        const float freq = clampT(p(kFreq), 20.0f, 20000.0f);
        for (int ch = 0; ch < kNumChannels; ++ch) {
            stages_[ch][0].setCutoff(freq, sr_, 0.7071f);
            stages_[ch][1].setCutoff(freq, sr_, 0.7071f);
            for (int n = 0; n < a.numSamples; ++n) {
                // Two cascaded Butterworth sections = Linkwitz-Riley 4th order,
                // so a low and a high band sum back to flat.
                const auto s1 = stages_[ch][0].process(a.in[ch][n]);
                const float v1 = high_ ? s1.hp : s1.lp;
                const auto s2 = stages_[ch][1].process(v1);
                a.out[ch][n] = high_ ? s2.hp : s2.lp;
            }
        }
    }

private:
    double sr_ = 48000.0;
    bool   high_ = false;
    Svf2   stages_[kNumChannels][2];
};

ModuleManifest manifestFilterCrossover() {
    ModuleManifest m;
    m.type = "filter.crossover"; m.category = "processor";
    m.summary = "Linkwitz-Riley band splitter. Feed one source into TWO of these (band 'low' and band 'high') at the same frequency, process each separately, then sum them back - that is how you build multiband distortion, or drive the bass without smearing the top.";
    m.allowVoice = true; m.allowGlobal = true;
    m.audioIns = 1; m.audioOuts = 1; m.costWeight = 0.6f;
    m.params = { P("freq", "Split", "Hz", 20.0f, 20000.0f, 800.0f, Taper::Log) };
    m.settings = { SEnum("band", {"low", "high"}, "low",
                         "Which side of the split this instance passes.") };
    m.factory = mk<FilterCrossover>();
    return m;
}

// ===========================================================================
// fx.freq_shifter - Bode-style, linear frequency shift (not pitch shift)
// ===========================================================================
class FxFreqShifter final : public ParamHolder<3> {
public:
    enum { kShiftHz, kMix, kFeedback };

    void prepare(const ModulePrepareInfo& info) override { sr_ = info.sampleRate; reset(); }
    void reset() noexcept override {
        std::memset(apA_, 0, sizeof(apA_));
        std::memset(apB_, 0, sizeof(apB_));
        phase_ = 0.0f;
        fb_[0] = fb_[1] = 0.0f;
    }

    void process(const ProcessArgs& a) noexcept override {
        const float shift = clampT(p(kShiftHz), -1000.0f, 1000.0f);
        const float mix   = clamp01(p(kMix));
        const float fbAmt = clampT(p(kFeedback), 0.0f, 0.85f);
        const float inc   = shift / static_cast<float>(sr_);

        for (int n = 0; n < a.numSamples; ++n) {
            phase_ += inc;
            phase_ -= std::floor(phase_);
            const float cosw = std::cos(kTwoPi * phase_);
            const float sinw = std::sin(kTwoPi * phase_);

            for (int ch = 0; ch < kNumChannels; ++ch) {
                const float dry = a.in[ch][n];
                const float x = dry + fb_[ch] * fbAmt;

                // Hilbert transform: two allpass chains 90 degrees apart give
                // the analytic signal, which is what makes the shift
                // single-sideband instead of ring modulation.
                const float re = chain(apA_[ch], x, kCoefA);
                const float im = chain(apB_[ch], x, kCoefB);
                const float wet = re * cosw - im * sinw;

                fb_[ch] = clampT(sanitize(wet), -2.0f, 2.0f);
                a.out[ch][n] = lerp(dry, wet, mix);
            }
        }
    }

private:
    static constexpr int kStages = 4;
    // Classic Olli Niemitalo half-band allpass coefficients.
    static constexpr float kCoefA[kStages] = {0.6923877778f, 0.9360654322f,
                                              0.9882295006f, 0.9987488452f};
    static constexpr float kCoefB[kStages] = {0.4021921162f, 0.8561710882f,
                                              0.9722909545f, 0.9952884791f};

    static float chain(float state[kStages * 2], float x, const float (&coef)[kStages]) noexcept {
        for (int i = 0; i < kStages; ++i) {
            const float c = coef[i] * coef[i];
            const float y = c * (x + state[i * 2 + 1]) - state[i * 2];
            state[i * 2]     = state[i * 2 + 1];
            state[i * 2 + 1] = clampT(sanitize(x), -32.0f, 32.0f);
            x = clampT(sanitize(y), -32.0f, 32.0f);
        }
        return x;
    }

    double sr_ = 48000.0;
    float  apA_[kNumChannels][kStages * 2]{};
    float  apB_[kNumChannels][kStages * 2]{};
    float  phase_ = 0.0f;
    float  fb_[kNumChannels]{};
};

ModuleManifest manifestFxFreqShifter() {
    ModuleManifest m;
    m.type = "fx.freq_shifter"; m.category = "effect";
    m.summary = "Bode-style frequency shifter. Shifts every partial by the same number of Hz rather than by a ratio, which makes it inharmonic and metallic - small shifts give shimmer and phasing, large shifts give clangorous bell tones. Not a pitch shifter.";
    m.allowVoice = false; m.allowGlobal = true;
    m.audioIns = 1; m.audioOuts = 1; m.costWeight = 1.1f;
    m.params = {
        P("shift_hz", "Shift",    "Hz", -1000.0f, 1000.0f, 0.0f, Taper::Linear, true,
          "Negative shifts down. Values under ~20 Hz give slow phasing."),
        P("mix",      "Mix",      "",       0.0f,    1.0f, 0.5f),
        P("feedback", "Feedback", "",       0.0f,   0.85f, 0.0f),
    };
    m.factory = mk<FxFreqShifter>();
    return m;
}

// ===========================================================================
// fx.tape - the "vintage" module: wow, flutter, saturation, head bump, hiss
// ===========================================================================
class FxTape final : public ParamHolder<5> {
public:
    enum { kDrive, kWow, kFlutter, kToneHz, kMix };

    void prepare(const ModulePrepareInfo& info) override {
        sr_ = info.sampleRate;
        rng_.reseed(info.seed ^ 0x7A9Eull);
        for (int ch = 0; ch < kNumChannels; ++ch) {
            lines_[ch].prepare(static_cast<int>(sr_ * 0.05) + 8);
            tone_[ch].reset();
            bump_[ch].reset();
        }
        reset();
    }
    void reset() noexcept override {
        for (int ch = 0; ch < kNumChannels; ++ch) {
            lines_[ch].clear(); tone_[ch].reset(); bump_[ch].reset(); dc_[ch].reset();
        }
        wowPhase_ = flutterPhase_ = 0.0f;
    }

    void process(const ProcessArgs& a) noexcept override {
        const float drive   = clampT(p(kDrive), 1.0f, 12.0f);
        const float wow     = clamp01(p(kWow));
        const float flutter = clamp01(p(kFlutter));
        const float mix     = clamp01(p(kMix));
        const float toneHz  = clampT(p(kToneHz), 1500.0f, 18000.0f);

        for (auto& t : tone_) t.setCutoff(toneHz, sr_);
        for (auto& b : bump_) b.setCutoff(90.0f, sr_);

        const float base    = 0.012f * static_cast<float>(sr_);   // ~12 ms nominal
        const float wowInc  = 0.7f  / static_cast<float>(sr_);
        const float flutInc = 7.5f  / static_cast<float>(sr_);
        const float comp    = 1.0f / std::sqrt(drive);

        for (int n = 0; n < a.numSamples; ++n) {
            wowPhase_     += wowInc;     if (wowPhase_ >= 1.0f)     wowPhase_ -= 1.0f;
            flutterPhase_ += flutInc;    if (flutterPhase_ >= 1.0f) flutterPhase_ -= 1.0f;

            // Wow is slow pitch drift, flutter is fast: both are delay
            // modulation, which is exactly how the transport caused them.
            const float mod = std::sin(kTwoPi * wowPhase_) * wow * 0.004f
                            + std::sin(kTwoPi * flutterPhase_) * flutter * 0.0008f;
            const float delay = clampT(base * (1.0f + mod), 2.0f,
                                       static_cast<float>(lines_[0].size() - 4));

            for (int ch = 0; ch < kNumChannels; ++ch) {
                const float dry = a.in[ch][n];
                lines_[ch].write(dry);
                float y = lines_[ch].read(delay);

                y = fastTanh(y * drive) * comp;      // tape compression
                y = tone_[ch].process(y);            // high-frequency loss
                y += bump_[ch].process(y) * 0.25f;   // low-end head bump
                y = dc_[ch].process(y);

                a.out[ch][n] = lerp(dry, y, mix);
            }
        }
    }

private:
    double    sr_ = 48000.0;
    DelayLine lines_[kNumChannels];
    OnePole   tone_[kNumChannels], bump_[kNumChannels];
    DcBlocker dc_[kNumChannels];
    float     wowPhase_ = 0.0f, flutterPhase_ = 0.0f;
    Rng       rng_;
};

ModuleManifest manifestFxTape() {
    ModuleManifest m;
    m.type = "fx.tape"; m.category = "effect";
    m.summary = "Tape machine: soft compression, high-frequency loss, low-end head bump, and wow/flutter pitch drift. The go-to for 'vintage', 'warm', 'lofi' or 'analog' requests - it glues and dirties in a way EQ alone cannot.";
    m.allowVoice = false; m.allowGlobal = true;
    m.audioIns = 1; m.audioOuts = 1; m.costWeight = 1.2f;
    m.params = {
        P("drive",   "Drive",   "",     1.0f,    12.0f,    2.0f, Taper::Log),
        P("wow",     "Wow",     "",     0.0f,     1.0f,    0.25f, Taper::Linear, true,
          "Slow pitch drift. Subtle amounts read as 'analog'; high amounts as 'broken'."),
        P("flutter", "Flutter", "",     0.0f,     1.0f,    0.2f),
        P("tone_hz", "Tone",    "Hz", 1500.0f, 18000.0f, 9000.0f, Taper::Log),
        P("mix",     "Mix",     "",     0.0f,     1.0f,    1.0f),
    };
    m.factory = mk<FxTape>();
    return m;
}

// ===========================================================================
// fx.transient - attack/sustain shaping
// ===========================================================================
class FxTransient final : public ParamHolder<3> {
public:
    enum { kAttack, kSustain, kMix };

    void prepare(const ModulePrepareInfo& info) override {
        sr_ = info.sampleRate;
        fastCoef_ = std::exp(-1.0f / (0.002f * static_cast<float>(sr_)));
        slowCoef_ = std::exp(-1.0f / (0.050f * static_cast<float>(sr_)));
        reset();
    }
    void reset() noexcept override { fast_ = slow_ = 0.0f; }

    void process(const ProcessArgs& a) noexcept override {
        const float attack  = clampT(p(kAttack), -1.0f, 1.0f);
        const float sustain = clampT(p(kSustain), -1.0f, 1.0f);
        const float mix     = clamp01(p(kMix));

        for (int n = 0; n < a.numSamples; ++n) {
            const float peak = std::max(std::abs(a.in[0][n]), std::abs(a.in[1][n]));
            fast_ = peak + fastCoef_ * (fast_ - peak);
            slow_ = peak + slowCoef_ * (slow_ - peak);

            // Where the fast follower outruns the slow one, we are in a
            // transient; where it lags, we are in the tail.
            const float diff = fast_ - slow_;
            const float gain = clampT(1.0f + (diff > 0.0f ? diff * attack * 6.0f
                                                          : diff * sustain * 6.0f),
                                      0.05f, 6.0f);
            for (int ch = 0; ch < kNumChannels; ++ch)
                a.out[ch][n] = lerp(a.in[ch][n], a.in[ch][n] * gain, mix);
        }
    }

private:
    double sr_ = 48000.0;
    float  fast_ = 0.0f, slow_ = 0.0f, fastCoef_ = 0.9f, slowCoef_ = 0.99f;
};

ModuleManifest manifestFxTransient() {
    ModuleManifest m;
    m.type = "fx.transient"; m.category = "effect";
    m.summary = "Transient designer. Positive attack sharpens the initial hit, negative softens it; sustain lengthens or shortens the tail. Makes plucks snap and pads breathe without touching the envelope.";
    m.allowVoice = false; m.allowGlobal = true;
    m.audioIns = 1; m.audioOuts = 1; m.costWeight = 0.5f;
    m.params = {
        P("attack",  "Attack",  "", -1.0f, 1.0f, 0.0f),
        P("sustain", "Sustain", "", -1.0f, 1.0f, 0.0f),
        P("mix",     "Mix",     "",  0.0f, 1.0f, 1.0f),
    };
    m.factory = mk<FxTransient>();
    return m;
}

// ===========================================================================
// fx.width - mid/side width with a mono bass guard
// ===========================================================================
class FxWidth final : public ParamHolder<3> {
public:
    enum { kWidth, kBassMonoHz, kTilt };

    void prepare(const ModulePrepareInfo& info) override { sr_ = info.sampleRate; reset(); }
    void reset() noexcept override { for (auto& f : split_) f.reset(); }

    void process(const ProcessArgs& a) noexcept override {
        const float width = clampT(p(kWidth), 0.0f, 2.0f);
        const float monoHz = clampT(p(kBassMonoHz), 20.0f, 500.0f);
        const float tilt = clampT(p(kTilt), -1.0f, 1.0f);
        split_[0].setCutoff(monoHz, sr_, 0.7071f);

        for (int n = 0; n < a.numSamples; ++n) {
            const float l = a.in[0][n], r = a.in[1][n];
            float mid  = 0.5f * (l + r);
            float side = 0.5f * (l - r);

            // Keep the lows mono: wide bass is what makes a mix collapse on a
            // club system, and generated instruments do not know that.
            const auto lowSide = split_[0].process(side);
            side = (side - lowSide.lp) * width;

            mid  *= (1.0f - tilt * 0.3f);
            side *= (1.0f + tilt * 0.3f);

            a.out[0][n] = clampT(mid + side, -4.0f, 4.0f);
            a.out[1][n] = clampT(mid - side, -4.0f, 4.0f);
        }
    }

private:
    double sr_ = 48000.0;
    Svf2   split_[1];
};

ModuleManifest manifestFxWidth() {
    ModuleManifest m;
    m.type = "fx.width"; m.category = "effect";
    m.summary = "Mid/side stereo width with the low end kept mono. Width above 1 widens, 0 collapses to mono. Use on pads and leads; leave basses near 1.";
    m.allowVoice = false; m.allowGlobal = true;
    m.audioIns = 1; m.audioOuts = 1; m.costWeight = 0.4f;
    m.params = {
        P("width",        "Width",     "",   0.0f,   2.0f,   1.3f),
        P("bass_mono_hz", "Mono Below","Hz", 20.0f, 500.0f, 140.0f, Taper::Log),
        P("tilt",         "Tilt",      "",  -1.0f,   1.0f,   0.0f, Taper::Linear, true,
          "Balances energy between the centre and the sides."),
    };
    m.factory = mk<FxWidth>();
    return m;
}

// ===========================================================================
// fx.dimension - four-tap BBD ensemble, the Juno/Dimension-D sound
// ===========================================================================
class FxDimension final : public ParamHolder<3> {
public:
    enum { kDepth, kRate, kMix };

    void prepare(const ModulePrepareInfo& info) override {
        sr_ = info.sampleRate;
        for (auto& l : lines_) l.prepare(static_cast<int>(sr_ * 0.06) + 8);
        reset();
    }
    void reset() noexcept override { for (auto& l : lines_) l.clear(); phase_ = 0.0f; }

    void process(const ProcessArgs& a) noexcept override {
        const float depth = clamp01(p(kDepth));
        const float mix   = clamp01(p(kMix));
        const float rate  = clampT(p(kRate), 0.05f, 3.0f);
        const float inc   = rate / static_cast<float>(sr_);
        const float base  = 0.011f * static_cast<float>(sr_);
        const float sweep = base * 0.45f * depth;

        for (int n = 0; n < a.numSamples; ++n) {
            phase_ += inc; if (phase_ >= 1.0f) phase_ -= 1.0f;

            const float mono = 0.5f * (a.in[0][n] + a.in[1][n]);
            lines_[0].write(mono);
            lines_[1].write(mono);

            // Four taps at quadrature phases: two per side, in anti-phase, which
            // is what gives the effect its width without obvious wobble.
            float wet[kNumChannels] = {0.0f, 0.0f};
            for (int tap = 0; tap < 4; ++tap) {
                const float ph = phase_ + static_cast<float>(tap) * 0.25f;
                const float d  = clampT(base + std::sin(kTwoPi * ph) * sweep,
                                        2.0f, static_cast<float>(lines_[0].size() - 4));
                const float s = lines_[tap % 2].read(d);
                wet[tap % 2] += s * ((tap < 2) ? 0.5f : -0.5f);
            }

            a.out[0][n] = lerp(a.in[0][n], a.in[0][n] * 0.7f + wet[0], mix);
            a.out[1][n] = lerp(a.in[1][n], a.in[1][n] * 0.7f + wet[1], mix);
        }
    }

private:
    double    sr_ = 48000.0;
    DelayLine lines_[kNumChannels];
    float     phase_ = 0.0f;
};

ModuleManifest manifestFxDimension() {
    ModuleManifest m;
    m.type = "fx.dimension"; m.category = "effect";
    m.summary = "Four-tap BBD ensemble - the Juno chorus / Dimension-D sound. Thicker and wider than fx.chorus with far less audible wobble. Excellent on pads, electric pianos and anything thin.";
    m.allowVoice = false; m.allowGlobal = true;
    m.audioIns = 1; m.audioOuts = 1; m.costWeight = 1.0f;
    m.params = {
        P("depth", "Depth", "",   0.0f, 1.0f, 0.5f),
        P("rate",  "Rate",  "Hz", 0.05f, 3.0f, 0.5f, Taper::Log),
        P("mix",   "Mix",   "",   0.0f, 1.0f, 0.5f),
    };
    m.factory = mk<FxDimension>();
    return m;
}

// ===========================================================================
// fx.pitch - delay-line pitch shifter
// ===========================================================================
class FxPitch final : public ParamHolder<3> {
public:
    enum { kSemitones, kMix, kFeedback };

    void prepare(const ModulePrepareInfo& info) override {
        sr_ = info.sampleRate;
        window_ = 0.050f * static_cast<float>(sr_);
        for (auto& l : lines_) l.prepare(static_cast<int>(window_ * 2.5f) + 16);
        reset();
    }
    void reset() noexcept override {
        for (auto& l : lines_) l.clear();
        phase_ = 0.0f;
        fb_[0] = fb_[1] = 0.0f;
    }

    void process(const ProcessArgs& a) noexcept override {
        const float semis = clampT(p(kSemitones), -24.0f, 24.0f);
        const float mix   = clamp01(p(kMix));
        const float fbAmt = clampT(p(kFeedback), 0.0f, 0.8f);
        const float ratio = std::pow(2.0f, semis / 12.0f);
        const float rate  = (ratio - 1.0f) / window_;

        for (int n = 0; n < a.numSamples; ++n) {
            phase_ -= rate;
            phase_ -= std::floor(phase_);

            for (int ch = 0; ch < kNumChannels; ++ch) {
                lines_[ch].write(a.in[ch][n] + fb_[ch] * fbAmt);

                // Two read heads half a window apart, crossfaded by a raised
                // cosine, so the wrap point is masked instead of clicking.
                const float p1 = phase_;
                const float p2 = phase_ + 0.5f - std::floor(phase_ + 0.5f);
                const float s1 = lines_[ch].read(2.0f + p1 * window_);
                const float s2 = lines_[ch].read(2.0f + p2 * window_);
                const float g1 = 0.5f - 0.5f * std::cos(kTwoPi * p1);
                const float g2 = 0.5f - 0.5f * std::cos(kTwoPi * p2);

                const float wet = s1 * g1 + s2 * g2;
                fb_[ch] = clampT(sanitize(wet), -2.0f, 2.0f);
                a.out[ch][n] = lerp(a.in[ch][n], wet, mix);
            }
        }
    }

private:
    double    sr_ = 48000.0;
    DelayLine lines_[kNumChannels];
    float     window_ = 2048.0f, phase_ = 0.0f;
    float     fb_[kNumChannels]{};
};

ModuleManifest manifestFxPitch() {
    ModuleManifest m;
    m.type = "fx.pitch"; m.category = "effect";
    m.summary = "Pitch shifter. Use for octave doubling (-12 or +12), detuned thickening (a few cents via small fractional values), or shimmer reverb when fed back at +12. Granular, so extreme settings smear - that is often the point.";
    m.allowVoice = false; m.allowGlobal = true;
    m.audioIns = 1; m.audioOuts = 1; m.costWeight = 1.1f;
    m.params = {
        P("semitones", "Pitch",    "st", -24.0f, 24.0f, 12.0f),
        P("mix",       "Mix",      "",     0.0f,  1.0f,  0.4f),
        P("feedback",  "Feedback", "",     0.0f,  0.8f,  0.0f, Taper::Linear, true,
          "Feed back into itself for cascading shimmer."),
    };
    m.factory = mk<FxPitch>();
    return m;
}

// ===========================================================================
// filter.formant - vowel resonances
// ===========================================================================
class FilterFormant final : public ParamHolder<3> {
public:
    enum { kMorph, kResonance, kMix };

    void prepare(const ModulePrepareInfo& info) override { sr_ = info.sampleRate; reset(); }
    void reset() noexcept override {
        for (int ch = 0; ch < kNumChannels; ++ch)
            for (auto& f : bands_[ch]) f.reset();
    }

    void process(const ProcessArgs& a) noexcept override {
        // A E I O U, first three formants of each.
        static const float table[5][3] = {
            { 730.0f, 1090.0f, 2440.0f },   // a
            { 530.0f, 1840.0f, 2480.0f },   // e
            { 270.0f, 2290.0f, 3010.0f },   // i
            { 570.0f,  840.0f, 2410.0f },   // o
            { 300.0f,  870.0f, 2240.0f },   // u
        };

        const float morph = clamp01(p(kMorph)) * 4.0f;
        const int   i0 = clampT(static_cast<int>(morph), 0, 4);
        const int   i1 = clampT(i0 + 1, 0, 4);
        const float t  = morph - static_cast<float>(i0);
        const float q  = 2.0f + clamp01(p(kResonance)) * 18.0f;
        const float mix = clamp01(p(kMix));

        for (int ch = 0; ch < kNumChannels; ++ch)
            for (int b = 0; b < 3; ++b)
                bands_[ch][b].setCutoff(lerp(table[i0][b], table[i1][b], t), sr_, q);

        for (int ch = 0; ch < kNumChannels; ++ch) {
            for (int n = 0; n < a.numSamples; ++n) {
                const float dry = a.in[ch][n];
                float wet = 0.0f;
                for (int b = 0; b < 3; ++b)
                    wet += bands_[ch][b].process(dry).bp * (b == 0 ? 1.0f : (b == 1 ? 0.7f : 0.4f));
                a.out[ch][n] = lerp(dry, clampT(wet * 0.8f, -4.0f, 4.0f), mix);
            }
        }
    }

private:
    double sr_ = 48000.0;
    Svf2   bands_[kNumChannels][3];
};

ModuleManifest manifestFilterFormant() {
    ModuleManifest m;
    m.type = "filter.formant"; m.category = "processor";
    m.summary = "Vowel formant bank. Morph sweeps A-E-I-O-U. Gives talking, vocal and 'wah' character; modulate morph from an LFO for the classic talk-box effect.";
    m.allowVoice = true; m.allowGlobal = true;
    m.audioIns = 1; m.audioOuts = 1; m.costWeight = 1.0f;
    m.params = {
        P("morph",     "Vowel", "", 0.0f, 1.0f, 0.3f, Taper::Linear, true,
          "0 = A, 0.25 = E, 0.5 = I, 0.75 = O, 1 = U."),
        P("resonance", "Reso",  "", 0.0f, 1.0f, 0.6f),
        P("mix",       "Mix",   "", 0.0f, 1.0f, 1.0f),
    };
    m.factory = mk<FilterFormant>();
    return m;
}

// ===========================================================================
// osc.karplus - plucked string physical model
// ===========================================================================
class OscKarplus final : public ParamHolder<4> {
public:
    enum { kLevel, kDamping, kBrightness, kDecay };

    void prepare(const ModulePrepareInfo& info) override {
        sr_ = info.sampleRate;
        rng_.reseed(info.seed ^ 0xC0FFEEull);
        for (auto& l : lines_) l.prepare(static_cast<int>(sr_ / 20.0) + 8);
        for (auto& d : damp_) d.reset();
        reset();
    }
    void reset() noexcept override {
        for (auto& l : lines_) l.clear();
        for (auto& d : damp_) d.reset();
        exciteLeft_ = 0;
    }

    void noteOn(float, float velocity) noexcept override {
        // Excite with a short burst; length scales with velocity so hard hits
        // are brighter, exactly as a harder pluck would be.
        exciteLeft_ = static_cast<int>(clampT(velocity, 0.1f, 1.0f) * 0.004f * static_cast<float>(sr_));
        for (auto& l : lines_) l.clear();
    }

    void process(const ProcessArgs& a) noexcept override {
        const float level = clamp01(p(kLevel));
        const float decay = clampT(p(kDecay), 0.80f, 0.9995f);
        const float bright = clampT(p(kBrightness), 500.0f, 16000.0f);
        const float damping = clamp01(p(kDamping));

        const float freq = clampT(a.v ? a.v->glideFreqHz : 220.0f, 20.0f,
                                  static_cast<float>(sr_) * 0.25f);
        const float delay = clampT(static_cast<float>(sr_) / freq, 2.0f,
                                   static_cast<float>(lines_[0].size() - 4));
        for (auto& d : damp_) d.setCutoff(lerp(bright, 400.0f, damping), sr_);

        for (int n = 0; n < a.numSamples; ++n) {
            const float burst = (exciteLeft_ > 0) ? rng_.nextBipolar() : 0.0f;
            if (exciteLeft_ > 0) --exciteLeft_;

            for (int ch = 0; ch < kNumChannels; ++ch) {
                const float s = lines_[ch].read(delay);
                lines_[ch].write(burst + damp_[ch].process(s) * decay);
                a.out[ch][n] += clampT(s, -2.0f, 2.0f) * level;
            }
        }
    }

    bool holdsVoice() const noexcept override { return exciteLeft_ > 0; }

private:
    double    sr_ = 48000.0;
    DelayLine lines_[kNumChannels];
    OnePole   damp_[kNumChannels];
    Rng       rng_;
    int       exciteLeft_ = 0;
};

ModuleManifest manifestOscKarplus() {
    ModuleManifest m;
    m.type = "osc.karplus"; m.category = "source";
    m.summary = "Karplus-Strong plucked string. A physical model, not a sample: gives guitars, harps, kotos and mallets a realism no oscillator stack reaches. Still needs a vca and amplitude envelope after it.";
    m.allowVoice = true; m.allowGlobal = false;
    m.audioIns = 0; m.audioOuts = 1; m.costWeight = 0.8f;
    m.params = {
        P("level",      "Level",  "",   0.0f,     1.0f,    0.8f),
        P("damping",    "Damp",   "",   0.0f,     1.0f,    0.3f, Taper::Linear, true,
          "Higher damping = duller and shorter, like a muted string."),
        P("brightness", "Bright", "Hz", 500.0f, 16000.0f, 6000.0f, Taper::Log),
        P("decay",      "Decay",  "",   0.80f,   0.9995f, 0.99f, Taper::Linear, true,
          "How long the string rings. Above 0.995 sustains for many seconds."),
    };
    m.factory = mk<OscKarplus>();
    return m;
}

// ===========================================================================
// mod.sequencer - stepped modulation
// ===========================================================================
class ModSequencer final : public ParamHolder<3> {
public:
    enum { kRateHz, kSmooth, kDepth };

    void prepare(const ModulePrepareInfo& info) override {
        sr_ = info.sampleRate;
        steps_    = clampT(getI(info.settings, "steps", 8), 2, 16);
        sync_     = getB(info.settings, "sync", true);
        division_ = getStr(info.settings, "division", "1/8");
        pattern_  = getStr(info.settings, "pattern", "random");

        Rng rng(info.seed ^ 0x5E9Cull);
        for (int i = 0; i < steps_; ++i) {
            const float t = static_cast<float>(i) / static_cast<float>(std::max(1, steps_ - 1));
            if (pattern_ == "up")            values_[i] = t;
            else if (pattern_ == "down")     values_[i] = 1.0f - t;
            else if (pattern_ == "alternate")values_[i] = (i % 2 == 0) ? 1.0f : 0.25f;
            else                             values_[i] = rng.nextUnipolar();
        }
        smoother_.setCutoff(30.0f, sr_);
        reset();
    }
    void reset() noexcept override { phase_ = 0.0f; step_ = 0; out_ = 0.0f; smoother_.reset(0.0f); }
    void noteOn(float, float) noexcept override { phase_ = 0.0f; step_ = 0; }

    float modOut() const noexcept override { return out_; }

    void process(const ProcessArgs& a) noexcept override {
        float rate = clampT(p(kRateHz), 0.05f, 40.0f);
        if (sync_ && a.g != nullptr) {
            double beats = 0.5;
            if (division_ == "1/1") beats = 4.0;
            else if (division_ == "1/2") beats = 2.0;
            else if (division_ == "1/4") beats = 1.0;
            else if (division_ == "1/8") beats = 0.5;
            else if (division_ == "1/16") beats = 0.25;
            const double spb = 60.0 / clampT(a.g->bpm, 20.0, 300.0);
            rate = static_cast<float>(1.0 / std::max(0.01, beats * spb));
        }

        phase_ += rate * static_cast<float>(a.numSamples) / static_cast<float>(sr_);
        while (phase_ >= 1.0f) {
            phase_ -= 1.0f;
            step_ = (step_ + 1) % steps_;
        }

        const float target = values_[step_] * clamp01(p(kDepth));
        const float sm = clamp01(p(kSmooth));
        if (sm > 0.001f) {
            smoother_.setCutoff(lerp(60.0f, 1.0f, sm), sr_);
            out_ = clamp01(smoother_.process(target));
        } else {
            out_ = clamp01(target);
        }
    }

private:
    double      sr_ = 48000.0;
    std::string division_ = "1/8", pattern_ = "random";
    bool        sync_ = true;
    int         steps_ = 8, step_ = 0;
    float       values_[16]{}, phase_ = 0.0f, out_ = 0.0f;
    OnePole     smoother_;
};

ModuleManifest manifestModSequencer() {
    ModuleManifest m;
    m.type = "mod.sequencer"; m.category = "modulator";
    m.summary = "Stepped modulation source, tempo-syncable. Route it at a filter cutoff for trance gates and rhythmic movement, or at pitch for arpeggios. This is what makes an instrument feel performed rather than held.";
    m.allowVoice = true; m.allowGlobal = true;
    m.audioIns = 0; m.audioOuts = 0; m.isModSource = true; m.costWeight = 0.1f;
    m.params = {
        P("rate_hz", "Rate",   "Hz", 0.05f, 40.0f, 4.0f, Taper::Log, true,
          "Ignored when 'sync' is true."),
        P("smooth",  "Smooth", "",   0.0f,   1.0f, 0.0f, Taper::Linear, true,
          "0 = hard steps (gates). Higher glides between them."),
        P("depth",   "Depth",  "",   0.0f,   1.0f, 1.0f),
    };
    m.settings = {
        SInt ("steps", 2, 16, 8),
        SEnum("pattern", {"random", "up", "down", "alternate"}, "random"),
        SBool("sync", true),
        SEnum("division", {"1/1", "1/2", "1/4", "1/8", "1/16"}, "1/8"),
    };
    m.factory = mk<ModSequencer>();
    return m;
}

} // namespace

void registerProductionModules(std::vector<ModuleManifest>& out) {
    out.push_back(manifestFxEq3());
    out.push_back(manifestFilterCrossover());
    out.push_back(manifestFilterFormant());
    out.push_back(manifestFxFreqShifter());
    out.push_back(manifestFxTape());
    out.push_back(manifestFxTransient());
    out.push_back(manifestFxWidth());
    out.push_back(manifestFxDimension());
    out.push_back(manifestFxPitch());
    out.push_back(manifestOscKarplus());
    out.push_back(manifestModSequencer());
}

} // namespace forge
