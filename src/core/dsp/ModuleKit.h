// ---------------------------------------------------------------------------
// Shared scaffolding for writing DSP primitives.
//
// Manifest builders, settings readers and the small reusable DSP blocks that
// more than one module needs. Adding a primitive should be about the DSP, not
// about boilerplate - that is what keeps the library cheap to extend, which is
// the whole extensibility argument in SPEC 6.5.
// ---------------------------------------------------------------------------
#pragma once

#include "core/dsp/Module.h"

#include <array>
#include <cstring>

namespace forge::kit {

// --- manifest construction -------------------------------------------------

inline ParamDesc P(std::string id, std::string label, std::string unit,
                   float mn, float mx, float def,
                   Taper taper = Taper::Linear, bool modulatable = true,
                   std::string help = {}) {
    ParamDesc p;
    p.id = std::move(id); p.label = std::move(label); p.unit = std::move(unit);
    p.min = mn; p.max = mx; p.def = def; p.taper = taper;
    p.modulatable = modulatable; p.help = std::move(help);
    return p;
}

inline SettingDesc SEnum(std::string id, std::vector<std::string> options, std::string def,
                         std::string help = {}) {
    SettingDesc s;
    s.id = std::move(id); s.type = SettingDesc::Type::Enum;
    s.options = std::move(options); s.def = std::move(def); s.help = std::move(help);
    return s;
}
inline SettingDesc SFloat(std::string id, float mn, float mx, float def, std::string help = {}) {
    SettingDesc s;
    s.id = std::move(id); s.type = SettingDesc::Type::Float;
    s.min = mn; s.max = mx; s.def = def; s.help = std::move(help);
    return s;
}
inline SettingDesc SInt(std::string id, int mn, int mx, int def, std::string help = {}) {
    SettingDesc s;
    s.id = std::move(id); s.type = SettingDesc::Type::Int;
    s.min = static_cast<float>(mn); s.max = static_cast<float>(mx);
    s.def = def; s.help = std::move(help);
    return s;
}
inline SettingDesc SBool(std::string id, bool def, std::string help = {}) {
    SettingDesc s;
    s.id = std::move(id); s.type = SettingDesc::Type::Bool; s.def = def;
    s.help = std::move(help);
    return s;
}
inline SettingDesc SAsset(std::string id, SettingDesc::Type t, std::string help = {}) {
    SettingDesc s;
    s.id = std::move(id); s.type = t; s.def = std::string{}; s.help = std::move(help);
    return s;
}

template <class T> ModuleFactory mk() {
    return []() -> std::unique_ptr<Module> { return std::make_unique<T>(); };
}

// --- settings readers (worker thread, tolerant by design) ------------------

inline std::string getStr(const nlohmann::json* s, const char* key, const char* def) {
    if (s && s->is_object()) {
        auto it = s->find(key);
        if (it != s->end() && it->is_string()) return it->get<std::string>();
    }
    return def;
}
inline float getF(const nlohmann::json* s, const char* key, float def) {
    if (s && s->is_object()) {
        auto it = s->find(key);
        if (it != s->end() && it->is_number()) return sanitize(it->get<float>(), def);
    }
    return def;
}
inline int getI(const nlohmann::json* s, const char* key, int def) {
    if (s && s->is_object()) {
        auto it = s->find(key);
        if (it != s->end() && it->is_number()) return static_cast<int>(std::lround(it->get<double>()));
    }
    return def;
}
inline bool getB(const nlohmann::json* s, const char* key, bool def) {
    if (s && s->is_object()) {
        auto it = s->find(key);
        if (it != s->end() && it->is_boolean()) return it->get<bool>();
        if (it != s->end() && it->is_number())  return it->get<double>() != 0.0;
    }
    return def;
}

// --- reusable DSP blocks ---------------------------------------------------

class DelayLine {
public:
    void prepare(int maxSamples) {
        buf_.assign(static_cast<size_t>(std::max(8, maxSamples)), 0.0f);
        idx_ = 0;
    }
    void clear() noexcept { std::fill(buf_.begin(), buf_.end(), 0.0f); idx_ = 0; }

    void write(float v) noexcept {
        if (buf_.empty()) return;
        buf_[static_cast<size_t>(idx_)] = sanitize(v);
        if (++idx_ >= static_cast<int>(buf_.size())) idx_ = 0;
    }

    float read(float delaySamples) const noexcept {
        const int n = static_cast<int>(buf_.size());
        if (n < 4) return 0.0f;
        const float d = clampT(sanitize(delaySamples, 1.0f), 1.0f, static_cast<float>(n - 2));
        float pos = static_cast<float>(idx_) - d;
        while (pos < 0.0f) pos += static_cast<float>(n);
        int i0 = static_cast<int>(pos);
        const float t = pos - static_cast<float>(i0);
        if (i0 >= n) i0 -= n;
        int i1 = i0 + 1; if (i1 >= n) i1 -= n;
        return lerp(buf_[static_cast<size_t>(i0)], buf_[static_cast<size_t>(i1)], t);
    }

    int size() const noexcept { return static_cast<int>(buf_.size()); }

private:
    std::vector<float> buf_;
    int idx_ = 0;
};

/// Second-order TPT state-variable core, reused by the EQ, the crossover and
/// the formant bank. Zero-delay feedback, stable at any cutoff.
class Svf2 {
public:
    void reset() noexcept { ic1_ = ic2_ = 0.0f; }

    void setCutoff(float hz, double sampleRate, float q = 0.7071f) noexcept {
        const float fc = clampT(hz, 10.0f, static_cast<float>(sampleRate) * 0.45f);
        g_ = std::tan(3.14159265358979f * fc / static_cast<float>(sampleRate));
        k_ = 1.0f / clampT(q, 0.05f, 40.0f);
        a1_ = 1.0f / (1.0f + g_ * (g_ + k_));
        a2_ = g_ * a1_;
        a3_ = g_ * a2_;
    }

    struct Out { float lp, bp, hp; };

    Out process(float x) noexcept {
        const float v3 = x - ic2_;
        const float v1 = a1_ * ic1_ + a2_ * v3;
        const float v2 = ic2_ + a2_ * ic1_ + a3_ * v3;
        ic1_ = clampT(2.0f * v1 - ic1_, -64.0f, 64.0f);
        ic2_ = clampT(2.0f * v2 - ic2_, -64.0f, 64.0f);
        return { v2, v1, x - k_ * v1 - v2 };
    }

    float k() const noexcept { return k_; }

private:
    float g_ = 0.1f, k_ = 1.4142f, a1_ = 1.0f, a2_ = 0.0f, a3_ = 0.0f;
    float ic1_ = 0.0f, ic2_ = 0.0f;
};

/// Peak/notch and shelving biquads for the EQ, in RBJ form.
class Biquad {
public:
    void reset() noexcept { x1_ = x2_ = y1_ = y2_ = 0.0f; }

    void setPeak(float hz, float q, float gainDb, double sr) noexcept {
        const float A = std::pow(10.0f, gainDb / 40.0f);
        const float w = 6.28318530718f * clampT(hz, 20.0f, static_cast<float>(sr) * 0.45f)
                      / static_cast<float>(sr);
        const float alpha = std::sin(w) / (2.0f * clampT(q, 0.1f, 12.0f));
        setCoeffs(1.0f + alpha * A, -2.0f * std::cos(w), 1.0f - alpha * A,
                  1.0f + alpha / A, -2.0f * std::cos(w), 1.0f - alpha / A);
    }

    /// RBJ low pass. The cabinet needs a genuinely steep top end, and a
    /// one-pole cannot get anywhere near it.
    void setLowPass(float hz, float q, double sr) noexcept {
        const float w = 6.28318530718f * clampT(hz, 20.0f, static_cast<float>(sr) * 0.45f)
                      / static_cast<float>(sr);
        const float cw = std::cos(w), sw = std::sin(w);
        const float alpha = sw / (2.0f * clampT(q, 0.1f, 12.0f));
        setCoeffs((1.0f - cw) * 0.5f, 1.0f - cw, (1.0f - cw) * 0.5f,
                  1.0f + alpha, -2.0f * cw, 1.0f - alpha);
    }

    void setHighPass(float hz, float q, double sr) noexcept {
        const float w = 6.28318530718f * clampT(hz, 20.0f, static_cast<float>(sr) * 0.45f)
                      / static_cast<float>(sr);
        const float cw = std::cos(w), sw = std::sin(w);
        const float alpha = sw / (2.0f * clampT(q, 0.1f, 12.0f));
        setCoeffs((1.0f + cw) * 0.5f, -(1.0f + cw), (1.0f + cw) * 0.5f,
                  1.0f + alpha, -2.0f * cw, 1.0f - alpha);
    }

    void setLowShelf(float hz, float gainDb, double sr) noexcept {
        const float A = std::pow(10.0f, gainDb / 40.0f);
        const float w = 6.28318530718f * clampT(hz, 20.0f, static_cast<float>(sr) * 0.45f)
                      / static_cast<float>(sr);
        const float cw = std::cos(w), sw = std::sin(w);
        const float beta = std::sqrt(A) / 0.9f;
        const float b0 =      A * ((A + 1.0f) - (A - 1.0f) * cw + beta * sw);
        const float b1 = 2.0f * A * ((A - 1.0f) - (A + 1.0f) * cw);
        const float b2 =      A * ((A + 1.0f) - (A - 1.0f) * cw - beta * sw);
        const float a0 =           (A + 1.0f) + (A - 1.0f) * cw + beta * sw;
        const float a1 =   -2.0f * ((A - 1.0f) + (A + 1.0f) * cw);
        const float a2 =           (A + 1.0f) + (A - 1.0f) * cw - beta * sw;
        setCoeffs(b0, b1, b2, a0, a1, a2);
    }

    void setHighShelf(float hz, float gainDb, double sr) noexcept {
        const float A = std::pow(10.0f, gainDb / 40.0f);
        const float w = 6.28318530718f * clampT(hz, 20.0f, static_cast<float>(sr) * 0.45f)
                      / static_cast<float>(sr);
        const float cw = std::cos(w), sw = std::sin(w);
        const float beta = std::sqrt(A) / 0.9f;
        const float b0 =      A * ((A + 1.0f) + (A - 1.0f) * cw + beta * sw);
        const float b1 = -2.0f * A * ((A - 1.0f) + (A + 1.0f) * cw);
        const float b2 =      A * ((A + 1.0f) + (A - 1.0f) * cw - beta * sw);
        const float a0 =           (A + 1.0f) - (A - 1.0f) * cw + beta * sw;
        const float a1 =    2.0f * ((A - 1.0f) - (A + 1.0f) * cw);
        const float a2 =           (A + 1.0f) - (A - 1.0f) * cw - beta * sw;
        setCoeffs(b0, b1, b2, a0, a1, a2);
    }

    float process(float x) noexcept {
        const float y = b0_ * x + b1_ * x1_ + b2_ * x2_ - a1_ * y1_ - a2_ * y2_;
        x2_ = x1_; x1_ = x;
        y2_ = y1_; y1_ = clampT(sanitize(y), -64.0f, 64.0f);
        return y1_;
    }

    struct Coeffs { float b0, b1, b2, a1, a2; };
    Coeffs coeffs() const noexcept { return {b0_, b1_, b2_, a1_, a2_}; }

    /// Magnitude response in dB at `hz`. Exposed so a UI curve can be drawn
    /// from the very same coefficients the audio thread is running - a
    /// display that recomputes its own version of the maths eventually
    /// disagrees with the sound, and then it is worse than no display.
    static float magnitudeDb(const Coeffs& c, float hz, double sampleRate) noexcept {
        const double w = 2.0 * 3.14159265358979 * clampT(static_cast<double>(hz), 1.0,
                                                         sampleRate * 0.49) / sampleRate;
        const double cw = std::cos(w), sw = std::sin(w);
        const double cw2 = std::cos(2.0 * w), sw2 = std::sin(2.0 * w);

        const double numRe = c.b0 + c.b1 * cw + c.b2 * cw2;
        const double numIm =      - c.b1 * sw - c.b2 * sw2;
        const double denRe = 1.0  + c.a1 * cw + c.a2 * cw2;
        const double denIm =      - c.a1 * sw - c.a2 * sw2;

        const double num = std::sqrt(numRe * numRe + numIm * numIm);
        const double den = std::sqrt(denRe * denRe + denIm * denIm);
        return static_cast<float>(20.0 * std::log10(std::max(num / std::max(den, 1e-12), 1e-9)));
    }

private:
    void setCoeffs(float b0, float b1, float b2, float a0, float a1, float a2) noexcept {
        const float inv = 1.0f / (std::abs(a0) < 1.0e-9f ? 1.0e-9f : a0);
        b0_ = b0 * inv; b1_ = b1 * inv; b2_ = b2 * inv;
        a1_ = a1 * inv; a2_ = a2 * inv;
    }
    float b0_ = 1.0f, b1_ = 0.0f, b2_ = 0.0f, a1_ = 0.0f, a2_ = 0.0f;
    float x1_ = 0.0f, x2_ = 0.0f, y1_ = 0.0f, y2_ = 0.0f;
};

/// Stores parameters in a fixed array; every module gets its values via
/// setParam(index, engineeringValue) once per control block.
template <int NumParams>
class ParamHolder : public Module {
public:
    void setParam(int index, float value) noexcept override {
        if (index >= 0 && index < NumParams) p_[static_cast<size_t>(index)] = sanitize(value);
    }
protected:
    float p(int i) const noexcept { return p_[static_cast<size_t>(i)]; }
    std::array<float, NumParams> p_{};
};

} // namespace forge::kit
