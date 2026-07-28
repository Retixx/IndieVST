// ---------------------------------------------------------------------------
// Forge - shared primitive types, constants and real-time-safe DSP helpers.
//
// Nothing in this header allocates, locks, or touches the OS. Everything here
// is safe to call from the audio thread (SPEC invariant I2).
// ---------------------------------------------------------------------------
#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <string>

namespace forge {

// --- Engine-wide constants -------------------------------------------------

/// Audio is always processed as stereo internally. Uniformity beats the ~2x
/// cost: every module has the same signature and there is no mono/stereo
/// adaptation logic to get wrong.
inline constexpr int kNumChannels = 2;

/// Modulation is resolved once per control block. 16 samples is 0.33 ms at
/// 48 kHz - fast enough that a 2 ms attack still gets ~6 updates, cheap enough
/// that the matrix cost disappears next to the DSP.
inline constexpr int kControlBlock = 16;

/// Because every node processes in kControlBlock-sized chunks, every internal
/// buffer is only kControlBlock samples long. A 64-node graph needs ~16 KB.
inline constexpr int kNodeBufferSamples = kControlBlock;

// Schema caps.
//
// Raised from the MVP values (32 nodes / 24 params) to carry a full fixed
// synth architecture: multiple oscillators, two filters, four envelopes, four
// LFOs, a modulation matrix and a complete effects chain. An instrument with
// sixteen knobs is a toy; a producer expects to reach for any part of the
// signal path, which means the schema has to be able to describe all of it.
inline constexpr int kMaxNodes            = 96;
inline constexpr int kMaxExposedParams    = 224;
inline constexpr int kMaxMacros           = 16;
inline constexpr int kMaxAudioConnections = 256;
inline constexpr int kMaxModRoutes        = 192;
inline constexpr int kMaxAssets           = 24;
inline constexpr int kMaxParamsPerModule  = 12;

inline constexpr int kMaxVoices     = 32;
inline constexpr int kDefaultVoices = 16;

/// Master ceiling. The limiter in out.master targets this and it cannot be
/// bypassed by generated content (SPEC 8.3).
inline constexpr float kMasterCeilingDb = -0.3f;

// --- Enums -----------------------------------------------------------------

enum class Scope { Voice, Global };

enum class Taper {
    Linear, ///< v = min + n * (max - min)
    Log,    ///< v = min * (max/min)^n     - frequencies, times. Requires min > 0.
    Exp     ///< v = min + (max - min) * n^2 - amounts that should feel fine near zero.
};

const char* toString(Scope) noexcept;
const char* toString(Taper) noexcept;
bool parseScope(const std::string&, Scope& out) noexcept;
bool parseTaper(const std::string&, Taper& out) noexcept;

// --- Small math ------------------------------------------------------------

template <typename T>
inline constexpr T clampT(T v, T lo, T hi) noexcept {
    return v < lo ? lo : (v > hi ? hi : v);
}

inline float clamp01(float v) noexcept { return clampT(v, 0.0f, 1.0f); }

inline bool isFinite(float v) noexcept {
    return v == v && v > -std::numeric_limits<float>::infinity()
                  && v <  std::numeric_limits<float>::infinity();
}

/// Replaces NaN/Inf with a safe fallback. Used at every boundary where a value
/// derived from generated content enters the DSP.
inline float sanitize(float v, float fallback = 0.0f) noexcept {
    return isFinite(v) ? v : fallback;
}

inline float dbToGain(float db) noexcept { return std::pow(10.0f, db * 0.05f); }
inline float gainToDb(float g)  noexcept { return 20.0f * std::log10(std::max(g, 1.0e-9f)); }

inline float lerp(float a, float b, float t) noexcept { return a + (b - a) * t; }

/// Maps a normalised 0..1 control position onto engineering units.
inline float applyTaper(float norm, float mn, float mx, Taper taper) noexcept {
    norm = clamp01(sanitize(norm));
    switch (taper) {
        case Taper::Log: {
            // Guard against a generated min of 0 or a sign flip.
            const float lo = (mn > 1.0e-6f) ? mn : 1.0e-6f;
            const float hi = (mx > lo) ? mx : lo * 2.0f;
            return lo * std::pow(hi / lo, norm);
        }
        case Taper::Exp:  return mn + (mx - mn) * norm * norm;
        case Taper::Linear:
        default:          return mn + (mx - mn) * norm;
    }
}

/// Inverse of applyTaper - needed to turn an IR default expressed in
/// engineering units back into a 0..1 control position.
inline float removeTaper(float value, float mn, float mx, Taper taper) noexcept {
    value = sanitize(value, mn);
    switch (taper) {
        case Taper::Log: {
            const float lo = (mn > 1.0e-6f) ? mn : 1.0e-6f;
            const float hi = (mx > lo) ? mx : lo * 2.0f;
            const float v  = clampT(value, lo, hi);
            return clamp01(std::log(v / lo) / std::log(hi / lo));
        }
        case Taper::Exp: {
            if (std::abs(mx - mn) < 1.0e-12f) return 0.0f;
            return clamp01(std::sqrt(clamp01((value - mn) / (mx - mn))));
        }
        case Taper::Linear:
        default:
            if (std::abs(mx - mn) < 1.0e-12f) return 0.0f;
            return clamp01((value - mn) / (mx - mn));
    }
}

inline float midiNoteToHz(float note) noexcept {
    return 440.0f * std::pow(2.0f, (note - 69.0f) / 12.0f);
}

// --- Deterministic RNG -----------------------------------------------------

/// xorshift64*. Seeded per voice/node from the instrument seed so that the same
/// instrument, the same notes and the same host block size always produce
/// bit-identical audio (SPEC invariant I4).
class Rng {
public:
    explicit Rng(uint64_t seed = 0x9E3779B97F4A7C15ull) noexcept
        : state_(seed ? seed : 0x9E3779B97F4A7C15ull) {}

    void reseed(uint64_t seed) noexcept { state_ = seed ? seed : 0x9E3779B97F4A7C15ull; }

    uint64_t nextU64() noexcept {
        state_ ^= state_ >> 12;
        state_ ^= state_ << 25;
        state_ ^= state_ >> 27;
        return state_ * 0x2545F4914F6CDD1Dull;
    }

    /// Uniform in [-1, 1).
    float nextBipolar() noexcept {
        return static_cast<float>(static_cast<int32_t>(nextU64() >> 32))
             * (1.0f / 2147483648.0f);
    }

    /// Uniform in [0, 1).
    float nextUnipolar() noexcept {
        return static_cast<float>(nextU64() >> 40) * (1.0f / 16777216.0f);
    }

private:
    uint64_t state_;
};

// --- Real-time-safe building blocks ---------------------------------------

/// Linear ramp between control-block targets. This is what keeps parameter
/// changes (from mod, macros or a knob being dragged) from zippering.
class Smoother {
public:
    void reset(float value = 0.0f) noexcept { current_ = target_ = value; step_ = 0.0f; }

    void setTarget(float target, int samples) noexcept {
        target_ = sanitize(target, current_);
        step_   = (samples > 0) ? (target_ - current_) / static_cast<float>(samples) : 0.0f;
        if (samples <= 0) current_ = target_;
    }

    float next() noexcept {
        if (step_ == 0.0f) return current_;
        current_ += step_;
        if ((step_ > 0.0f && current_ >= target_) || (step_ < 0.0f && current_ <= target_)) {
            current_ = target_;
            step_    = 0.0f;
        }
        return current_;
    }

    float value() const noexcept { return current_; }

private:
    float current_ = 0.0f, target_ = 0.0f, step_ = 0.0f;
};

/// One-pole low-pass, used for control smoothing and filter damping.
class OnePole {
public:
    void setCutoff(float hz, double sampleRate) noexcept {
        const float fc = clampT(hz, 0.05f, static_cast<float>(sampleRate) * 0.45f);
        a_ = 1.0f - std::exp(-2.0f * 3.14159265358979f * fc / static_cast<float>(sampleRate));
    }
    void reset(float v = 0.0f) noexcept { z_ = v; }
    float process(float x) noexcept { z_ += a_ * (x - z_); return z_; }
    float value() const noexcept { return z_; }

private:
    float a_ = 0.1f, z_ = 0.0f;
};

/// Removes any DC the generated signal path introduced. Mandatory after every
/// waveshaper and inside out.master (SPEC 8.3).
class DcBlocker {
public:
    void reset() noexcept { x1_ = y1_ = 0.0f; }
    float process(float x) noexcept {
        const float y = x - x1_ + 0.9975f * y1_;
        x1_ = x; y1_ = y;
        return y;
    }
private:
    float x1_ = 0.0f, y1_ = 0.0f;
};

/// Bounded, C1-continuous soft clip. Never returns more than +/-1.
inline float softClip(float x) noexcept {
    x = sanitize(x);
    if (x >= 1.5f)  return 1.0f;
    if (x <= -1.5f) return -1.0f;
    return x - (4.0f / 27.0f) * x * x * x;
}

inline float fastTanh(float x) noexcept {
    x = clampT(sanitize(x), -8.0f, 8.0f);
    const float x2 = x * x;
    return x * (27.0f + x2) / (27.0f + 9.0f * x2);
}

/// PolyBLEP residual for band-limiting hard edges in the analog oscillator.
inline float polyBlep(float t, float dt) noexcept {
    if (dt <= 0.0f) return 0.0f;
    if (t < dt)          { const float u = t / dt;        return u + u - u * u - 1.0f; }
    if (t > 1.0f - dt)   { const float u = (t - 1.0f)/dt; return u * u + u + u + 1.0f; }
    return 0.0f;
}

} // namespace forge
