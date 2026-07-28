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
/// A one-pole allpass. Cascaded, these make a delay line frequency-dependent,
/// which is what turns a perfect harmonic comb into a real, slightly sharp
/// string.
class Allpass1 {
public:
    void reset() noexcept { x1_ = y1_ = 0.0f; }
    void setCoeff(float a) noexcept { a_ = clampT(a, -0.98f, 0.98f); }
    float process(float x) noexcept {
        const float y = -a_ * x + x1_ + a_ * y1_;
        x1_ = x; y1_ = sanitize(y);
        return y1_;
    }
private:
    float a_ = 0.0f, x1_ = 0.0f, y1_ = 0.0f;
};

/// What a plucked string actually starts from.
///
/// A player pulls the string aside at one point and lets go, so the initial
/// shape is a TRIANGLE with its apex at the pick point. Decomposed, that shape
/// gives harmonic k an amplitude proportional to sin(k*pi*beta) / k^2 - two
/// things at once, and both of them matter:
///
///   * sin(k*pi*beta) is the comb of missing harmonics at the pick position,
///   * 1/k^2 is why a real plucked note is dominated by its FUNDAMENTAL.
///
/// The model used to excite with flat white noise instead, which starts every
/// harmonic at full strength. That is the spectrum of a filtered sawtooth, and
/// measured on a bare string at E3 it put the 2nd and 3rd partials 13 dB ABOVE
/// the fundamental - so the ear heard the wrong note in the wrong octave and
/// read the whole thing as a synthesiser. No amount of filtering downstream
/// fixes an excitation that never had a fundamental in it.
///
/// A pickup, and a soundboard, respond to how fast the string is MOVING, not to
/// where it is - so what radiates is the derivative of that triangle, one power
/// of k brighter: `sin(k*pi*beta) / k`. Falling, fundamental-led, and with the
/// pick comb intact.
///
/// The derivative is taken HERE, analytically, on a shape that is known in
/// closed form. It was originally done the obvious way instead - first
/// difference of the output, scaled back up by sr/(2*pi*f0) to restore the
/// level - and that scale factor reaches 512 on a low note. Everything the
/// difference saw got multiplied by it, including the small step at every block
/// boundary where `delay` is recomputed from the glide, which turned into a
/// click every 256 samples and hard-clipped on top: audible as a crackle over
/// every note. Differentiating a rectangle instead needs no gain, no history
/// and no per-block state, and is the same filter.
///
/// The two levels are `1 - beta` and `-beta`, which sum to zero over the period
/// by construction - a string cannot hold a net offset, and at 6 ms the loop's
/// DC blocker is far too slow to take one out.
///
/// Normalised to a peak of 1 so the pick position sets the TIMBRE and not the
/// level. Without that, moving the pick towards the bridge quietly turns the
/// whole instrument up, and everything downstream - filter drive, the amp, the
/// speaker - is driven differently by a control that has no business doing it.
inline float pluckVelocity(float x, float beta) noexcept {
    const float b = clampT(beta, 0.02f, 0.98f);
    const float v = (x < b) ? (1.0f - b) : -b;
    return v / std::max(b, 1.0f - b);
}

class OscKarplus final : public ParamHolder<6> {
public:
    enum { kLevel, kDamping, kBrightness, kDecay, kPickPos, kStiffness };

    void prepare(const ModulePrepareInfo& info) override {
        sr_ = info.sampleRate;
        rng_.reseed(info.seed ^ 0xC0FFEEull);
        for (auto& l : lines_) l.prepare(static_cast<int>(sr_ / 20.0) + 8);
        reset();
    }
    void reset() noexcept override {
        for (auto& l : lines_) l.clear();
        for (auto& d : damp_)  d.reset();
        for (auto& b : block_) b.reset();
        for (auto& s : stiff_) { s[0].reset(); s[1].reset(); s[2].reset(); }
        pickFilt_.reset();
        for (auto& t : tone_) t.reset();
        exciteLeft_ = 0;
        exciteLen_  = 1;
        pending_    = false;
    }

    void noteOn(float, float velocity) noexcept override {
        velocity_ = clampT(velocity, 0.05f, 1.0f);
        pending_  = true;
    }

    void process(const ProcessArgs& a) noexcept override {
        const float level   = clamp01(p(kLevel));
        const float decay   = clampT(p(kDecay), 0.80f, 0.9995f);
        const float bright  = clampT(p(kBrightness), 500.0f, 16000.0f);
        const float damping = clamp01(p(kDamping));
        const float pickPos = clampT(p(kPickPos), 0.02f, 0.5f);
        const float stiff   = clamp01(p(kStiffness));

        const float freq = clampT(a.v ? a.v->glideFreqHz : 220.0f, 20.0f,
                                  static_cast<float>(sr_) * 0.25f);

        // The damping filter and the dispersion allpasses all sit inside the
        // loop, so their own delay is part of the loop length.
        // POSITIVE coefficient. A first-order allpass with a > 0 delays low
        // frequencies more than high ones, so the upper partials come round
        // the loop sooner and sit SHARP of the harmonic series - which is what
        // stiffness does to a real string. With the sign the other way the
        // partials go flat, which sounds like a detuned synth rather than a
        // string.
        const float apCoeff = 0.72f * stiff;

        // The allpasses add delay at the fundamental, and it has to come out of
        // the delay line or the whole string plays flat. The DC approximation
        // is not good enough - it left the string 24 cents flat at full
        // stiffness - so the phase delay is evaluated exactly at f0.
        //
        // For A(z) = (-a + z^-1) / (1 - a z^-1), the phase delay at w is
        // -arg(A(e^jw)) / w, computed here once per block.
        float apDelay = 0.0f;
        {
            // Note there is no "bypass at zero" shortcut: with a = 0 the
            // allpass is exactly one sample of delay, not a pass-through, and
            // skipping the compensation there left the string 21 cents flat.
            const float w  = 6.28318530718f * freq / static_cast<float>(sr_);
            const float cw = std::cos(w), sw = std::sin(w);
            const float nRe = -apCoeff + cw,   nIm = -sw;
            const float dRe = 1.0f - apCoeff * cw, dIm = apCoeff * sw;
            const float phase = std::atan2(nIm, nRe) - std::atan2(dIm, dRe);
            apDelay = kAllpassStages * (-phase / std::max(1.0e-6f, w));
        }
        const float loopDelay = static_cast<float>(sr_) / freq - kFilterDelay - apDelay;
        const float delay = clampT(loopDelay, 2.0f,
                                   static_cast<float>(lines_[0].size() - 4));

        // Brightness relative to the fundamental: the loop filter is applied
        // once per trip, so a fixed cutoff in Hz damps the treble far harder
        // than the bass.
        const float wanted = lerp(bright, 400.0f, damping);
        const float cutoff = clampT(std::max(wanted, freq * 4.0f),
                                    100.0f, static_cast<float>(sr_) * 0.45f);
        for (auto& d : damp_) d.setCutoff(cutoff, sr_);
        for (auto& t : tone_) t.setLowPass(kPickupTopHz, kPickupQ, sr_);

        // --- string stiffness --------------------------------------------
        //
        // A real string is not a pure delay. It is stiff, so high partials
        // travel faster and come back progressively SHARP of the harmonic
        // series. That inharmonicity is a large part of what the ear uses to
        // tell a string from an oscillator - a perfectly harmonic comb is
        // exactly what a filtered sawtooth already sounds like, which is why
        // the model on its own still read as a synth.
        for (auto& set : stiff_) for (auto& ap : set) ap.setCoeff(apCoeff);

        const float t60 = lerp(0.12f, 14.0f,
                               std::pow(clamp01((decay - 0.80f) / 0.1995f), 2.0f))
                        * (1.0f + 0.35f * clamp01(delay / 900.0f));
        const float loopGain = std::exp(-6.907755f
                                        / std::max(1.0f, t60 * static_cast<float>(sr_)));

        if (pending_) {
            pending_    = false;
            exciteLeft_ = static_cast<int>(delay) + 1;
            exciteLen_  = exciteLeft_;
            for (auto& l : lines_) l.clear();
            for (auto& d : damp_)  d.reset();
            for (auto& b : block_) b.reset();
            for (auto& set : stiff_) for (auto& ap : set) ap.reset();
            for (auto& t : tone_) t.reset();
            pickFilt_.reset();
            // A pick is a hard, bright scrape, not flat white noise. Opening
            // the excitation filter with velocity is what makes a hard pick
            // sound bright and a soft one sound round, which is most of the
            // dynamic life of a plucked instrument.
            //
            // Band-limited far harder than it looks like it needs to be,
            // because the velocity read-out below differentiates the output:
            // whatever noise is left up at 10 kHz gets lifted by 30 dB or more
            // on its way out, and at 1200-9000 Hz that turned every single note
            // into an audible burst of hiss.
            pickFilt_.setCutoff(lerp(700.0f, 4200.0f, velocity_), sr_);
        }

        // How much of the excitation is scrape rather than displacement. A
        // gentle pluck with the flesh of a finger is nearly pure displacement;
        // a hard pick drags across the winding first and adds a broadband
        // scratch. Keeping this proportional to velocity is what makes hard
        // playing brighter rather than merely louder.
        const float scrapeAmt = lerp(0.02f, 0.11f, velocity_);

        // Pickup position, in samples along the string. The excitation shape
        // already carries the comb of the PICK; this second, shallower comb is
        // the magnetic PICKUP reading the string at one point, which is part of
        // why an electric guitar is not just a string in a room.
        const float pickDelay = clampT(delay * pickPos, 1.0f, delay - 1.0f);

        for (int n = 0; n < a.numSamples; ++n) {
            float burst = 0.0f;
            if (exciteLeft_ > 0) {
                const float x = static_cast<float>(exciteLen_ - exciteLeft_)
                              / static_cast<float>(std::max(1, exciteLen_));
                const float body   = pluckVelocity(x, pickPos) * kPluckGain;
                const float scrape = pickFilt_.process(rng_.nextBipolar()) * scrapeAmt;
                // Remove any DC on the way in so none can accumulate in a
                // near-unity loop.
                burst = block_[0].process((body + scrape) * velocity_);
                --exciteLeft_;
            } else {
                burst = block_[0].process(0.0f);
            }

            for (int ch = 0; ch < kNumChannels; ++ch) {
                const float s = lines_[ch].read(delay);

                // --- pickup position --------------------------------------
                //
                // Reading the string at one point rather than integrating along
                // it notches every harmonic with a node there. Shallow on
                // purpose: the pluck shape above already removes the harmonics
                // physics says are missing, and a second full-depth comb on top
                // of it took the fundamental out with them.
                const float picked = s - lines_[ch].read(delay - pickDelay) * kPickupDepth;

                // --- the pickup's own rolloff ------------------------------
                //
                // The excitation already rises at +6 dB/oct, and nothing
                // physical does that for ever. A magnetic pickup is an inductor
                // with capacitance across it: it peaks somewhere between 2 and
                // 7 kHz and falls off a cliff above, and a soundboard behaves
                // much the same way. Without it the pick noise sat right up in
                // the top octave, and anything with no cabinet after it - an
                // acoustic guitar most of all - got the lot.
                //
                // Fixed in Hz on purpose. This one is electrical, not a
                // property of the string, so unlike the loop damping it must
                // NOT track the note.
                const float voiced = tone_[ch].process(picked);

                float fb = damp_[ch].process(s) * loopGain;
                for (auto& ap : stiff_[ch]) fb = ap.process(fb);
                lines_[ch].write(burst + fb);

                a.out[ch][n] += clampT(voiced, -2.0f, 2.0f) * level;
            }
        }
    }

    bool holdsVoice() const noexcept override { return pending_ || exciteLeft_ > 0; }

private:
    static constexpr float kFilterDelay   = 0.5f;
    static constexpr int   kAllpassStages = 3;
    /// Calibrated so the string PEAKS where every other source in the rack
    /// peaks at the same `level` - about 0.9.
    ///
    /// It was 1.7, carried over from an earlier excitation shape, and measured
    /// against the level of the fundamental rather than the peak. The string
    /// was putting out 1.27 where an oscillator put out 0.89: over full scale,
    /// clipping on its own output limiter, and driving everything after it 3 dB
    /// harder than the rack is calibrated for. On a patch with `dr_drive` up at
    /// 6.5 - which is what the model writes for anything gritty - a chord
    /// through that is intermodulation distortion, and it is heard as a
    /// crackle that no amount of work on the string itself will remove.
    static constexpr float kPluckGain     = 1.2f;
    /// Was 0.86, which cost the fundamental 2 dB and boosted the third partial
    /// by 5 - on top of an excitation that had no fundamental to begin with.
    static constexpr float kPickupDepth   = 0.42f;
    /// Where the pickup resonates, and immediately above it, stops responding.
    /// A single coil sits near 6 kHz, a humbucker lower; the PEAK is as much of
    /// the character as the rolloff is, and a plain one-pole in its place made
    /// every plucked instrument dull - measured centroid 824 Hz against 1424
    /// for a real recording of one.
    static constexpr float kPickupTopHz   = 6800.0f;
    static constexpr float kPickupQ       = 1.15f;

    double    sr_ = 48000.0;
    DelayLine lines_[kNumChannels];
    OnePole   damp_[kNumChannels];
    DcBlocker block_[kNumChannels];
    Allpass1  stiff_[kNumChannels][kAllpassStages];
    OnePole   pickFilt_;
    Biquad    tone_[kNumChannels];
    Rng       rng_;
    int       exciteLeft_ = 0;
    int       exciteLen_  = 1;
    bool      pending_    = false;
    float     velocity_   = 1.0f;
};

// ===========================================================================
// fx.cabinet - guitar / bass speaker cabinet
// ===========================================================================
//
// The missing piece that made every "electric guitar" sound like a synth.
//
// A guitar speaker is a small, heavily damped driver in a box, and it is not
// remotely flat: nothing below about 80 Hz, a broad presence peak around
// 2-4 kHz, and an extremely steep rolloff above 5 kHz. A distorted signal
// contains harmonics all the way to Nyquist, and it is precisely that
// 5-20 kHz fizz - which no real amp can produce - that the ear reads as
// "synthetic". You cannot make a convincing electric guitar without one.
//
// Modelled here as a bandpass pair plus two resonant peaks, which is cheap and
// captures the parts that matter perceptually.
class FxCabinet final : public ParamHolder<3> {
public:
    enum { kLowHz, kHighHz, kMix };

    void prepare(const ModulePrepareInfo& info) override {
        sr_ = info.sampleRate;
        reset();
    }
    void reset() noexcept override {
        for (auto& f : hp_)      f.reset();
        for (auto& f : lp1_)     f.reset();
        for (auto& f : lp2_)     f.reset();
        for (auto& f : presence_) f.reset();
        for (auto& f : body_)     f.reset();
    }

    void process(const ProcessArgs& a) noexcept override {
        const float lowHz  = clampT(p(kLowHz),  40.0f,  400.0f);
        const float highHz = clampT(p(kHighHz), 1500.0f, 12000.0f);
        const float mix    = clamp01(p(kMix));

        for (int ch = 0; ch < kNumChannels; ++ch) {
            hp_[ch].setHighPass(lowHz, 0.7f, sr_);
            // Two cascaded low passes: a single pole is nowhere near steep
            // enough, and the steepness is the whole point.
            lp1_[ch].setLowPass(highHz, 0.7f, sr_);
            lp2_[ch].setLowPass(highHz * 1.15f, 0.6f, sr_);
            presence_[ch].setPeak(2600.0f, 1.1f, 4.5f, sr_);
            body_[ch].setPeak(180.0f, 1.0f, 2.5f, sr_);
        }

        for (int ch = 0; ch < kNumChannels; ++ch) {
            for (int n = 0; n < a.numSamples; ++n) {
                const float dry = a.in[ch][n];
                float w = hp_[ch].process(dry);
                w = body_[ch].process(w);
                w = presence_[ch].process(w);
                w = lp1_[ch].process(w);
                w = lp2_[ch].process(w);
                a.out[ch][n] += lerp(dry, w, mix);
            }
        }
    }

private:
    double sr_ = 48000.0;
    Biquad hp_[kNumChannels], lp1_[kNumChannels], lp2_[kNumChannels];
    Biquad presence_[kNumChannels], body_[kNumChannels];
};

ModuleManifest manifestFxCabinet() {
    ModuleManifest m;
    m.type = "fx.cabinet"; m.category = "effect";
    m.summary = "Guitar / bass speaker cabinet. Steep rolloff above 5 kHz, a presence peak "
                "around 2-4 kHz and no deep bass - the thing that makes a driven signal "
                "sound like an amp in a room rather than a distorted oscillator. Essential "
                "on any electric guitar or bass; the fizz above 5 kHz that a real speaker "
                "cannot produce is what makes a modelled guitar sound synthetic.";
    m.allowVoice = false; m.allowGlobal = true;
    m.audioIns = 1; m.audioOuts = 1; m.costWeight = 0.5f;
    m.params = {
        P("low_hz",  "Low Cut",  "Hz",   40.0f,   400.0f,   85.0f, Taper::Log),
        P("high_hz", "Top",      "Hz", 1500.0f, 12000.0f, 4800.0f, Taper::Log, true,
          "Where the speaker stops. Real guitar cabs die around 5 kHz; anything "
          "above that is fizz no amp can make."),
        P("mix",     "Cabinet",  "",     0.0f,     1.0f,    0.0f,  Taper::Linear, true),
    };
    m.factory = mk<FxCabinet>();
    return m;
}

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
        P("decay",      "Decay",  "",   0.80f,   0.9995f, 0.90f, Taper::Linear, true,
          "How long the string rings. 0.88-0.93 is a real plucked instrument; "
          "above 0.96 it sustains for ten seconds or more, which is a pad."),
        P("pick_pos",   "Pick Pos", "", 0.02f,    0.5f,   0.14f, Taper::Linear, true,
          "Where along the string it is plucked. Near the bridge (0.05) is thin "
          "and nasal; over the neck (0.4) is round and full. This comb of missing "
          "harmonics is the most recognisable signature of a plucked instrument."),
        P("stiffness",  "Stiffness", "", 0.0f,    1.0f,   0.30f, Taper::Linear, true,
          "String stiffness. Real strings are not perfect delays - their high "
          "partials run sharp, and that inharmonicity is what the ear uses to "
          "tell a string from an oscillator."),
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

// ===========================================================================
// fx.flanger - short modulated delay with feedback
// ===========================================================================
class FxFlanger final : public ParamHolder<5> {
public:
    enum { kRateHz, kDepth, kFeedback, kMix, kDelayMs };

    void prepare(const ModulePrepareInfo& info) override {
        sr_ = info.sampleRate;
        for (auto& l : lines_) l.prepare(static_cast<int>(sr_ * 0.03) + 8);
        reset();
    }
    void reset() noexcept override {
        for (auto& l : lines_) l.clear();
        phase_ = 0.0f; fb_[0] = fb_[1] = 0.0f;
    }

    void process(const ProcessArgs& a) noexcept override {
        const float rate  = clampT(p(kRateHz), 0.01f, 8.0f);
        const float depth = clamp01(p(kDepth));
        // Negative feedback is what gives a flanger its hollow, metallic
        // through-zero character rather than sounding like a chorus.
        const float fbAmt = clampT(p(kFeedback), -0.95f, 0.95f);
        const float mix   = clamp01(p(kMix));
        const float base  = clampT(p(kDelayMs), 0.2f, 12.0f) * 0.001f * static_cast<float>(sr_);
        const float inc   = rate / static_cast<float>(sr_);
        const float sweep = base * 0.9f * depth;

        for (int n = 0; n < a.numSamples; ++n) {
            phase_ += inc; if (phase_ >= 1.0f) phase_ -= 1.0f;
            for (int ch = 0; ch < kNumChannels; ++ch) {
                const float lfo = std::sin(kTwoPi * (phase_ + (ch == 1 ? 0.25f : 0.0f)));
                const float d = clampT(base + lfo * sweep, 1.0f,
                                       static_cast<float>(lines_[ch].size() - 4));
                const float dry = a.in[ch][n];
                lines_[ch].write(dry + fb_[ch] * fbAmt);
                const float wet = lines_[ch].read(d);
                fb_[ch] = clampT(sanitize(wet), -2.0f, 2.0f);
                a.out[ch][n] = lerp(dry, (dry + wet) * 0.7f, mix);
            }
        }
    }

private:
    double    sr_ = 48000.0;
    DelayLine lines_[kNumChannels];
    float     phase_ = 0.0f, fb_[kNumChannels]{};
};

ModuleManifest manifestFxFlanger() {
    ModuleManifest m;
    m.type = "fx.flanger"; m.category = "effect";
    m.summary = "Flanger: a very short swept delay with feedback. Jet-plane whooshes and metallic sweeps. Negative feedback gives the hollow through-zero sound.";
    m.allowVoice = false; m.allowGlobal = true;
    m.audioIns = 1; m.audioOuts = 1; m.costWeight = 0.9f;
    m.params = {
        P("rate_hz",  "Rate",     "Hz",  0.01f, 8.0f,  0.2f, Taper::Log),
        P("depth",    "Depth",    "",    0.0f,  1.0f,  0.7f),
        P("feedback", "Feedback", "",   -0.95f, 0.95f, 0.5f),
        P("mix",      "Mix",      "",    0.0f,  1.0f,  0.0f),
        P("delay_ms", "Delay",    "ms",  0.2f, 12.0f,  2.0f, Taper::Log),
    };
    m.factory = mk<FxFlanger>();
    return m;
}

// ===========================================================================
// fx.ringmod - ring modulation against an internal carrier
// ===========================================================================
class FxRingMod final : public ParamHolder<3> {
public:
    enum { kFreqHz, kMix, kTrack };

    void prepare(const ModulePrepareInfo& info) override { sr_ = info.sampleRate; reset(); }
    void reset() noexcept override { phase_ = 0.0f; }

    void process(const ProcessArgs& a) noexcept override {
        float freq = clampT(p(kFreqHz), 1.0f, 8000.0f);
        const float track = clamp01(p(kTrack));
        if (track > 0.001f && a.v != nullptr)
            freq *= std::pow(clampT(a.v->baseFreqHz / 261.63f, 0.03f, 32.0f), track);
        freq = clampT(freq, 1.0f, static_cast<float>(sr_) * 0.45f);

        const float mix = clamp01(p(kMix));
        const float inc = freq / static_cast<float>(sr_);

        for (int n = 0; n < a.numSamples; ++n) {
            phase_ += inc; if (phase_ >= 1.0f) phase_ -= 1.0f;
            const float carrier = std::sin(kTwoPi * phase_);
            for (int ch = 0; ch < kNumChannels; ++ch)
                a.out[ch][n] = lerp(a.in[ch][n], a.in[ch][n] * carrier, mix);
        }
    }

private:
    double sr_ = 48000.0;
    float  phase_ = 0.0f;
};

ModuleManifest manifestFxRingMod() {
    ModuleManifest m;
    m.type = "fx.ringmod"; m.category = "effect";
    m.summary = "Ring modulator. Multiplies the signal by a sine, producing sum and difference tones - clangorous, bell-like and inharmonic. Key tracking keeps it musical across the keyboard.";
    m.allowVoice = true; m.allowGlobal = true;
    m.audioIns = 1; m.audioOuts = 1; m.costWeight = 0.4f;
    m.params = {
        P("freq_hz", "Freq",  "Hz", 1.0f, 8000.0f, 220.0f, Taper::Log),
        P("mix",     "Mix",   "",   0.0f,    1.0f,   0.0f),
        P("track",   "Track", "",   0.0f,    1.0f,   0.0f, Taper::Linear, true,
          "1.0 makes the carrier follow the played note, keeping it harmonic."),
    };
    m.factory = mk<FxRingMod>();
    return m;
}

// ===========================================================================
// fx.autopan - LFO stereo movement
// ===========================================================================
class FxAutoPan final : public ParamHolder<3> {
public:
    enum { kRateHz, kDepth, kShape };

    void prepare(const ModulePrepareInfo& info) override { sr_ = info.sampleRate; reset(); }
    void reset() noexcept override { phase_ = 0.0f; }

    void process(const ProcessArgs& a) noexcept override {
        const float rate  = clampT(p(kRateHz), 0.01f, 20.0f);
        const float depth = clamp01(p(kDepth));
        const float shape = clamp01(p(kShape));
        const float inc   = rate / static_cast<float>(sr_);

        for (int n = 0; n < a.numSamples; ++n) {
            phase_ += inc; if (phase_ >= 1.0f) phase_ -= 1.0f;
            const float sine   = std::sin(kTwoPi * phase_);
            const float square = phase_ < 0.5f ? 1.0f : -1.0f;
            const float pan    = lerp(sine, square, shape) * depth;
            const float l = std::sqrt(clamp01(0.5f * (1.0f - pan))) * 1.41421356f;
            const float r = std::sqrt(clamp01(0.5f * (1.0f + pan))) * 1.41421356f;
            a.out[0][n] = a.in[0][n] * l;
            a.out[1][n] = a.in[1][n] * r;
        }
    }

private:
    double sr_ = 48000.0;
    float  phase_ = 0.0f;
};

ModuleManifest manifestFxAutoPan() {
    ModuleManifest m;
    m.type = "fx.autopan"; m.category = "effect";
    m.summary = "Auto-panner. Sweeps the image left to right; shape morphs from a smooth sine to a hard square for chopped, gated movement.";
    m.allowVoice = false; m.allowGlobal = true;
    m.audioIns = 1; m.audioOuts = 1; m.costWeight = 0.3f;
    m.params = {
        P("rate_hz", "Rate",  "Hz", 0.01f, 20.0f, 1.0f, Taper::Log),
        P("depth",   "Depth", "",   0.0f,   1.0f, 0.0f),
        P("shape",   "Shape", "",   0.0f,   1.0f, 0.0f, Taper::Linear, true,
          "0 = smooth sine sweep, 1 = hard square chop."),
    };
    m.factory = mk<FxAutoPan>();
    return m;
}

// ===========================================================================
// fx.exciter - high-band harmonic generation
// ===========================================================================
class FxExciter final : public ParamHolder<3> {
public:
    enum { kFreqHz, kAmount, kMix };

    void prepare(const ModulePrepareInfo& info) override { sr_ = info.sampleRate; reset(); }
    void reset() noexcept override {
        for (int ch = 0; ch < kNumChannels; ++ch) { split_[ch].reset(); dc_[ch].reset(); }
    }

    void process(const ProcessArgs& a) noexcept override {
        const float freq   = clampT(p(kFreqHz), 500.0f, 12000.0f);
        const float amount = clampT(p(kAmount), 1.0f, 20.0f);
        const float mix    = clamp01(p(kMix));
        for (auto& s : split_) s.setCutoff(freq, sr_, 0.7071f);

        for (int ch = 0; ch < kNumChannels; ++ch) {
            for (int n = 0; n < a.numSamples; ++n) {
                const float dry = a.in[ch][n];
                // Saturate only the top band, then add it back. Distorting the
                // whole signal would just make it dirty; this makes it bright.
                const float high = split_[ch].process(dry).hp;
                const float exc  = dc_[ch].process(fastTanh(high * amount)) / std::sqrt(amount);
                a.out[ch][n] = dry + exc * mix;
            }
        }
    }

private:
    double    sr_ = 48000.0;
    Svf2      split_[kNumChannels];
    DcBlocker dc_[kNumChannels];
};

ModuleManifest manifestFxExciter() {
    ModuleManifest m;
    m.type = "fx.exciter"; m.category = "effect";
    m.summary = "Harmonic exciter. Saturates only the band above the crossover and adds it back, so the sound gets brighter and more present without getting dirty. Standard finishing move on vocals, keys and leads.";
    m.allowVoice = false; m.allowGlobal = true;
    m.audioIns = 1; m.audioOuts = 1; m.costWeight = 0.6f;
    m.params = {
        P("freq_hz", "Freq",   "Hz", 500.0f, 12000.0f, 3000.0f, Taper::Log),
        P("amount",  "Amount", "",     1.0f,    20.0f,    4.0f, Taper::Log),
        P("mix",     "Mix",    "",     0.0f,     1.0f,    0.0f),
    };
    m.factory = mk<FxExciter>();
    return m;
}

// ===========================================================================
// fx.gate - noise gate
// ===========================================================================
class FxGate final : public ParamHolder<4> {
public:
    enum { kThresholdDb, kAttackMs, kHoldMs, kReleaseMs };

    void prepare(const ModulePrepareInfo& info) override { sr_ = info.sampleRate; reset(); }
    void reset() noexcept override { env_ = 0.0f; gain_ = 0.0f; holdLeft_ = 0; }

    void process(const ProcessArgs& a) noexcept override {
        const float thr = dbToGain(clampT(p(kThresholdDb), -80.0f, 0.0f));
        const float atk = std::exp(-1.0f / (std::max(p(kAttackMs), 0.1f) * 0.001f * static_cast<float>(sr_)));
        const float rel = std::exp(-1.0f / (std::max(p(kReleaseMs), 1.0f) * 0.001f * static_cast<float>(sr_)));
        const int   hold = static_cast<int>(std::max(p(kHoldMs), 0.0f) * 0.001f * static_cast<float>(sr_));

        for (int n = 0; n < a.numSamples; ++n) {
            const float peak = std::max(std::abs(a.in[0][n]), std::abs(a.in[1][n]));
            env_ = peak > env_ ? peak : peak + rel * (env_ - peak);

            if (env_ > thr) holdLeft_ = hold;
            else if (holdLeft_ > 0) --holdLeft_;

            const float target = (env_ > thr || holdLeft_ > 0) ? 1.0f : 0.0f;
            const float coef = target > gain_ ? atk : rel;
            gain_ = target + coef * (gain_ - target);

            a.out[0][n] = a.in[0][n] * gain_;
            a.out[1][n] = a.in[1][n] * gain_;
        }
    }

private:
    double sr_ = 48000.0;
    float  env_ = 0.0f, gain_ = 0.0f;
    int    holdLeft_ = 0;
};

ModuleManifest manifestFxGate() {
    ModuleManifest m;
    m.type = "fx.gate"; m.category = "effect";
    m.summary = "Noise gate. Silences the signal below the threshold - tightens loose tails, removes hiss between notes, and with a fast release turns sustained sounds into stuttering rhythmic ones.";
    m.allowVoice = false; m.allowGlobal = true;
    m.audioIns = 1; m.audioOuts = 1; m.costWeight = 0.4f;
    m.params = {
        P("threshold_db", "Thresh",  "dB", -80.0f,    0.0f, -80.0f),
        P("attack_ms",    "Attack",  "ms",   0.1f,  100.0f,   1.0f, Taper::Log),
        P("hold_ms",      "Hold",    "ms",   0.0f,  500.0f,  20.0f, Taper::Log),
        P("release_ms",   "Release", "ms",   1.0f, 2000.0f, 120.0f, Taper::Log),
    };
    m.factory = mk<FxGate>();
    return m;
}

} // namespace

void registerProductionModules(std::vector<ModuleManifest>& out) {
    out.push_back(manifestFxFlanger());
    out.push_back(manifestFxRingMod());
    out.push_back(manifestFxAutoPan());
    out.push_back(manifestFxExciter());
    out.push_back(manifestFxGate());
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
    out.push_back(manifestFxCabinet());
    out.push_back(manifestModSequencer());
}

} // namespace forge
