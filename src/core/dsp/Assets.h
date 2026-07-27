// ---------------------------------------------------------------------------
// Baked assets: the concrete tables the DSP reads, expanded on a worker thread
// from the compact generative descriptions in the IR (SPEC 5.4).
//
// The LLM never emits raw sample data. It emits a recipe; the AssetBaker turns
// that recipe into band-limited, normalised, DC-free tables.
// ---------------------------------------------------------------------------
#pragma once

#include "core/Common.h"

#include <map>
#include <vector>

namespace forge {

/// Multi-frame, mip-mapped wavetable.
///
/// Mip level m contains only harmonics up to `kBaseHarmonics >> m`, so playing
/// a high note simply selects a lower mip instead of aliasing.
struct Wavetable {
    static constexpr int kFrameSize     = 1024;
    static constexpr int kNumMips       = 8;
    static constexpr int kBaseHarmonics = kFrameSize / 2;

    int numFrames = 1;
    /// Layout: [frame][mip][kFrameSize + 1], the +1 being a wrapped guard
    /// sample so interpolation needs no branch.
    std::vector<float> data;

    void allocate(int frames) {
        numFrames = clampT(frames, 1, 8);
        data.assign(static_cast<size_t>(numFrames) * kNumMips * (kFrameSize + 1), 0.0f);
    }

    float* frameData(int frame, int mip) noexcept {
        const size_t idx = (static_cast<size_t>(frame) * kNumMips + static_cast<size_t>(mip))
                         * (kFrameSize + 1);
        return data.data() + idx;
    }
    const float* frameData(int frame, int mip) const noexcept {
        return const_cast<Wavetable*>(this)->frameData(frame, mip);
    }

    /// Picks the highest-detail mip that will not alias for this phase step.
    int mipForIncrement(float phaseIncrement) const noexcept {
        // phaseIncrement is cycles-per-sample. The highest safe harmonic is
        // 0.5 / phaseIncrement.
        const float inc = std::max(phaseIncrement, 1.0e-7f);
        const float maxHarm = 0.5f / inc;
        int mip = 0;
        float allowed = static_cast<float>(kBaseHarmonics);
        while (mip < kNumMips - 1 && allowed > maxHarm) { allowed *= 0.5f; ++mip; }
        return mip;
    }

    /// Linear interpolation within a frame, linear morph between frames.
    float read(float framePos, int mip, float phase01) const noexcept {
        if (data.empty()) return 0.0f;
        mip = clampT(mip, 0, kNumMips - 1);
        const float fp   = clampT(framePos, 0.0f, static_cast<float>(numFrames - 1));
        const int   f0   = static_cast<int>(fp);
        const int   f1   = std::min(f0 + 1, numFrames - 1);
        const float fmix = fp - static_cast<float>(f0);

        const float p  = (phase01 - std::floor(phase01)) * static_cast<float>(kFrameSize);
        const int   i0 = static_cast<int>(p);
        const float t  = p - static_cast<float>(i0);

        const float* a = frameData(f0, mip);
        const float* b = frameData(f1, mip);
        const float va = lerp(a[i0], a[i0 + 1], t);
        const float vb = lerp(b[i0], b[i0 + 1], t);
        return lerp(va, vb, fmix);
    }
};

/// Waveshaper / modulation transfer curve, sampled over the input range -1..1.
struct Curve {
    static constexpr int kSize = 1024;
    std::vector<float> table; ///< kSize + 1 entries

    float lookup(float x) const noexcept {
        if (table.empty()) return clampT(x, -1.0f, 1.0f);
        const float n = (clampT(sanitize(x), -1.0f, 1.0f) + 1.0f) * 0.5f
                      * static_cast<float>(kSize);
        const int   i = clampT(static_cast<int>(n), 0, kSize - 1);
        return lerp(table[static_cast<size_t>(i)], table[static_cast<size_t>(i) + 1],
                    n - static_cast<float>(i));
    }
};

/// Multi-stage envelope shape used by env.multi.
struct EnvelopeShape {
    struct Stage {
        float level  = 0.0f;   ///< target level at the end of the stage, 0..1
        float timeMs = 100.0f; ///< duration
        Taper curve  = Taper::Exp;
    };
    std::vector<Stage> stages;
    bool  loop        = false;
    int   sustainStage = -1;   ///< -1 = no sustain point (one-shot)
};

/// Everything a graph needs, owned by the GraphInstance and immutable once
/// published to the audio thread.
struct BakedAssets {
    std::map<std::string, Wavetable>     wavetables;
    std::map<std::string, Curve>         curves;
    std::map<std::string, EnvelopeShape> envelopes;

    const Wavetable*     findWavetable(const std::string& id) const {
        auto it = wavetables.find(id); return it == wavetables.end() ? nullptr : &it->second;
    }
    const Curve*         findCurve(const std::string& id) const {
        auto it = curves.find(id); return it == curves.end() ? nullptr : &it->second;
    }
    const EnvelopeShape* findEnvelope(const std::string& id) const {
        auto it = envelopes.find(id); return it == envelopes.end() ? nullptr : &it->second;
    }
};

} // namespace forge
