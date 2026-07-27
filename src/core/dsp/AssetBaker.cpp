#include "core/dsp/AssetBaker.h"

#include <algorithm>
#include <vector>

namespace forge {
namespace {

constexpr float kTwoPi = 6.28318530717958647692f;

struct Partial { int n = 1; float amp = 0.0f; float phase = 0.0f; };

/// Harmonic series for the classic analog shapes, used when a recipe asks for
/// a named waveform instead of listing partials.
std::vector<Partial> partialsForShape(const std::string& shape, int maxHarmonic) {
    std::vector<Partial> out;
    maxHarmonic = clampT(maxHarmonic, 1, Wavetable::kBaseHarmonics);
    for (int n = 1; n <= maxHarmonic; ++n) {
        float amp = 0.0f;
        float phase = 0.0f;
        if (shape == "saw")           amp = 1.0f / static_cast<float>(n);
        else if (shape == "square")   amp = (n % 2 == 1) ? 1.0f / static_cast<float>(n) : 0.0f;
        else if (shape == "triangle") {
            if (n % 2 == 1) {
                amp   = 1.0f / static_cast<float>(n * n);
                phase = ((n - 1) / 2 % 2 == 0) ? 0.0f : 0.5f;
            }
        }
        else if (shape == "sine")     amp = (n == 1) ? 1.0f : 0.0f;
        else if (shape == "odd")      amp = (n % 2 == 1) ? 1.0f / static_cast<float>(n) : 0.0f;
        else if (shape == "even")     amp = (n % 2 == 0) ? 1.0f / static_cast<float>(n) : 0.0f;
        else                          amp = (n == 1) ? 1.0f : 0.0f;
        if (amp != 0.0f) out.push_back({n, amp, phase});
    }
    return out;
}

std::vector<Partial> readPartials(const nlohmann::json& spec) {
    std::vector<Partial> out;
    auto it = spec.find("partials");
    if (it != spec.end() && it->is_array()) {
        for (const auto& p : *it) {
            Partial q;
            if (p.is_number()) {                     // bare amplitude list: [1, 0.5, 0.25]
                q.n   = static_cast<int>(out.size()) + 1;
                q.amp = sanitize(p.get<float>(), 0.0f);
            } else if (p.is_object()) {
                auto n = p.find("n");
                auto a = p.find("amp");
                auto h = p.find("phase");
                q.n   = (n != p.end() && n->is_number()) ? static_cast<int>(n->get<double>())
                                                         : static_cast<int>(out.size()) + 1;
                q.amp = (a != p.end() && a->is_number()) ? sanitize(a->get<float>(), 0.0f) : 0.0f;
                q.phase = (h != p.end() && h->is_number()) ? sanitize(h->get<float>(), 0.0f) : 0.0f;
            } else {
                continue;
            }
            q.n = clampT(q.n, 1, Wavetable::kBaseHarmonics);
            q.amp = clampT(q.amp, -4.0f, 4.0f);
            if (q.amp != 0.0f) out.push_back(q);
            if (out.size() >= 256) break;   // recipes past this add nothing audible
        }
    }
    return out;
}

/// Additive synthesis of one frame at one mip level, plus normalisation and DC
/// removal. This is where the band-limiting guarantee actually lives.
void renderFrame(float* dest, const std::vector<Partial>& partials, int harmonicLimit) {
    const int N = Wavetable::kFrameSize;
    for (int i = 0; i <= N; ++i) dest[i] = 0.0f;

    for (const auto& p : partials) {
        if (p.n > harmonicLimit) continue;
        const float w = kTwoPi * static_cast<float>(p.n) / static_cast<float>(N);
        const float ph = kTwoPi * p.phase;
        for (int i = 0; i < N; ++i)
            dest[i] += p.amp * std::sin(w * static_cast<float>(i) + ph);
    }

    // Remove DC, then normalise to unity peak.
    double sum = 0.0;
    for (int i = 0; i < N; ++i) sum += dest[i];
    const float dc = static_cast<float>(sum / N);
    float peak = 0.0f;
    for (int i = 0; i < N; ++i) { dest[i] -= dc; peak = std::max(peak, std::abs(dest[i])); }
    const float g = (peak > 1.0e-6f) ? (1.0f / peak) : 0.0f;
    for (int i = 0; i < N; ++i) dest[i] *= g;

    dest[N] = dest[0];   // wrapped guard sample
}

float readNumber(const nlohmann::json& j, const char* key, float def) {
    auto it = j.find(key);
    return (it != j.end() && it->is_number()) ? sanitize(it->get<float>(), def) : def;
}
std::string readString(const nlohmann::json& j, const char* key, const char* def) {
    auto it = j.find(key);
    return (it != j.end() && it->is_string()) ? it->get<std::string>() : def;
}
bool readBool(const nlohmann::json& j, const char* key, bool def) {
    auto it = j.find(key);
    return (it != j.end() && it->is_boolean()) ? it->get<bool>() : def;
}

float catmullRom(float p0, float p1, float p2, float p3, float t) {
    const float t2 = t * t, t3 = t2 * t;
    return 0.5f * ((2.0f * p1)
                 + (-p0 + p2) * t
                 + (2.0f * p0 - 5.0f * p1 + 4.0f * p2 - p3) * t2
                 + (-p0 + 3.0f * p1 - 3.0f * p2 + p3) * t3);
}

} // namespace

// ---------------------------------------------------------------------------

void bakeWavetable(const nlohmann::json& spec, Wavetable& out) {
    const std::string method = readString(spec, "method", "harmonics");
    int frames = clampT(static_cast<int>(readNumber(spec, "frames", 1.0f)), 1, 8);

    std::vector<Partial> partials;
    if (method == "shape" || spec.contains("shape")) {
        partials = partialsForShape(readString(spec, "shape", "saw"),
                                    static_cast<int>(readNumber(spec, "max_harmonic", 64.0f)));
    }
    if (partials.empty()) partials = readPartials(spec);
    if (partials.empty()) partials = partialsForShape("saw", 32);

    const std::string morph = readString(spec, "morph", frames > 1 ? "lowpass_sweep" : "none");
    if (morph == "none") frames = 1;

    out.allocate(frames);

    for (int f = 0; f < out.numFrames; ++f) {
        // Frame morphing is expressed as a per-frame harmonic budget: frame 0
        // is the brightest, later frames progressively duller (or the reverse).
        const float t = (out.numFrames == 1) ? 0.0f
                      : static_cast<float>(f) / static_cast<float>(out.numFrames - 1);

        std::vector<Partial> framePartials = partials;
        if (morph == "lowpass_sweep") {
            const float cut = lerp(static_cast<float>(Wavetable::kBaseHarmonics), 2.0f, t);
            for (auto& p : framePartials) {
                const float roll = clamp01(1.0f - (static_cast<float>(p.n) - cut * 0.5f) / std::max(cut, 1.0f));
                p.amp *= roll * roll;
            }
        } else if (morph == "harmonic_fade") {
            for (auto& p : framePartials)
                if (p.n > 1) p.amp *= (1.0f - t);
        } else if (morph == "odd_to_even") {
            for (auto& p : framePartials)
                p.amp *= (p.n % 2 == 1) ? (1.0f - t * 0.9f) : (0.1f + t * 0.9f);
        }

        int limit = Wavetable::kBaseHarmonics;
        for (int mip = 0; mip < Wavetable::kNumMips; ++mip) {
            renderFrame(out.frameData(f, mip), framePartials, std::max(limit, 1));
            limit /= 2;
        }
    }
}

void bakeCurve(const nlohmann::json& spec, Curve& out) {
    const std::string method = readString(spec, "method", "breakpoints");
    out.table.assign(Curve::kSize + 1, 0.0f);

    if (method == "polynomial") {
        std::vector<float> coeffs;
        auto it = spec.find("coeffs");
        if (it != spec.end() && it->is_array())
            for (const auto& c : *it)
                if (c.is_number()) coeffs.push_back(clampT(sanitize(c.get<float>(), 0.0f), -8.0f, 8.0f));
        if (coeffs.empty()) coeffs = {0.0f, 1.0f};
        for (int i = 0; i <= Curve::kSize; ++i) {
            const float x = 2.0f * static_cast<float>(i) / static_cast<float>(Curve::kSize) - 1.0f;
            float y = 0.0f, xp = 1.0f;
            for (float c : coeffs) { y += c * xp; xp *= x; }
            out.table[static_cast<size_t>(i)] = clampT(sanitize(y), -1.0f, 1.0f);
        }
    } else {
        // Breakpoints, the common case.
        std::vector<std::pair<float, float>> pts;
        auto it = spec.find("points");
        if (it != spec.end() && it->is_array()) {
            for (const auto& p : *it) {
                if (p.is_array() && p.size() >= 2 && p[0].is_number() && p[1].is_number())
                    pts.emplace_back(clampT(sanitize(p[0].get<float>()), -1.0f, 1.0f),
                                     clampT(sanitize(p[1].get<float>()), -1.0f, 1.0f));
                else if (p.is_object() && p.contains("x") && p.contains("y"))
                    pts.emplace_back(clampT(sanitize(p["x"].get<float>()), -1.0f, 1.0f),
                                     clampT(sanitize(p["y"].get<float>()), -1.0f, 1.0f));
            }
        }
        if (pts.size() < 2) pts = {{-1.0f, -1.0f}, {0.0f, 0.0f}, {1.0f, 1.0f}};

        std::sort(pts.begin(), pts.end(),
                  [](const auto& a, const auto& b) { return a.first < b.first; });
        // Guarantee coverage of the full input range.
        if (pts.front().first > -1.0f) pts.insert(pts.begin(), {-1.0f, pts.front().second});
        if (pts.back().first  <  1.0f) pts.push_back({1.0f, pts.back().second});

        const bool smooth = readString(spec, "interp", "linear") == "catmullrom";
        for (int i = 0; i <= Curve::kSize; ++i) {
            const float x = 2.0f * static_cast<float>(i) / static_cast<float>(Curve::kSize) - 1.0f;
            size_t k = 0;
            while (k + 2 < pts.size() && pts[k + 1].first < x) ++k;
            const float x0 = pts[k].first, x1 = pts[k + 1].first;
            const float t = (x1 > x0) ? clamp01((x - x0) / (x1 - x0)) : 0.0f;
            float y;
            if (smooth) {
                const auto& pm = pts[k > 0 ? k - 1 : k];
                const auto& pp = pts[std::min(k + 2, pts.size() - 1)];
                y = catmullRom(pm.second, pts[k].second, pts[k + 1].second, pp.second, t);
            } else {
                y = lerp(pts[k].second, pts[k + 1].second, t);
            }
            out.table[static_cast<size_t>(i)] = clampT(sanitize(y), -1.0f, 1.0f);
        }
    }
}

void bakeEnvelope(const nlohmann::json& spec, EnvelopeShape& out) {
    out = EnvelopeShape{};
    out.loop = readBool(spec, "loop", false);

    auto it = spec.find("stages");
    if (it != spec.end() && it->is_array()) {
        for (const auto& s : *it) {
            if (!s.is_object()) continue;
            EnvelopeShape::Stage stage;
            stage.level  = clamp01(readNumber(s, "level", 0.0f));
            stage.timeMs = clampT(readNumber(s, "time_ms", 100.0f), 0.1f, 20000.0f);
            Taper t = Taper::Exp;
            auto c = s.find("curve");
            if (c != s.end() && c->is_string()) parseTaper(c->get<std::string>(), t);
            stage.curve = t;
            out.stages.push_back(stage);
            if (out.stages.size() >= 12) break;
        }
    }
    if (out.stages.empty())
        out.stages = { {1.0f, 5.0f, Taper::Linear}, {0.0f, 400.0f, Taper::Exp} };

    const int sustain = static_cast<int>(readNumber(spec, "sustain_stage", -1.0f));
    out.sustainStage = (sustain >= 0 && sustain < static_cast<int>(out.stages.size())) ? sustain : -1;
}

// ---------------------------------------------------------------------------

void bakeAssets(const std::vector<ir::AssetSpec>& specs, BakedAssets& out, ir::IrReport& report) {
    for (const auto& a : specs) {
        if (a.id.empty()) continue;
        const nlohmann::json spec = a.spec.is_object() ? a.spec : nlohmann::json::object();

        if (a.kind == "wavetable") {
            Wavetable wt;
            bakeWavetable(spec, wt);
            out.wavetables[a.id] = std::move(wt);
        } else if (a.kind == "curve") {
            Curve c;
            bakeCurve(spec, c);
            out.curves[a.id] = std::move(c);
        } else if (a.kind == "envelope") {
            EnvelopeShape e;
            bakeEnvelope(spec, e);
            out.envelopes[a.id] = std::move(e);
        } else {
            report.fixed("assets." + a.id, "Ignored an asset of unknown kind '" + a.kind + "'.");
        }
    }
}

} // namespace forge
