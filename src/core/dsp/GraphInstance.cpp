#include "core/dsp/GraphInstance.h"

#include <algorithm>
#include <cstring>

namespace forge {
namespace {

/// Shapes a modulation value through the route's curve. Applied to the
/// magnitude so that a bipolar source keeps its sign.
inline float shapeMod(float v, Taper curve) noexcept {
    const float s = (v < 0.0f) ? -1.0f : 1.0f;
    const float a = std::abs(clampT(v, -1.0f, 1.0f));
    switch (curve) {
        case Taper::Exp: return s * a * a;
        case Taper::Log: return s * std::sqrt(a);
        default:         return s * a;
    }
}

inline void zeroNodeBuffer(float* base) noexcept {
    std::memset(base, 0, sizeof(float) * kNumChannels * kControlBlock);
}

} // namespace

GraphInstance::~GraphInstance() = default;

// ---------------------------------------------------------------------------

void GraphInstance::setExposedParam(int index, float norm) noexcept {
    if (index < 0 || index >= static_cast<int>(exposedNorm_.size())) return;
    const float v = clamp01(sanitize(norm));
    if (exposedNorm_[static_cast<size_t>(index)] != v) {
        exposedNorm_[static_cast<size_t>(index)] = v;
        baseDirty_ = true;
    }
}

void GraphInstance::setMacro(int index, float norm) noexcept {
    if (index < 0 || index >= static_cast<int>(macroNorm_.size())) return;
    const float v = clamp01(sanitize(norm));
    if (macroNorm_[static_cast<size_t>(index)] != v) {
        macroNorm_[static_cast<size_t>(index)] = v;
        baseDirty_ = true;
    }
}

void GraphInstance::reset() noexcept {
    for (auto& v : voices_) {
        for (auto& m : v.modules) if (m) m->reset();
        std::fill(v.inBuf.begin(), v.inBuf.end(), 0.0f);
        std::fill(v.outBuf.begin(), v.outBuf.end(), 0.0f);
        std::fill(v.srcValue.begin(), v.srcValue.end(), 0.0f);
        std::copy(voicePlan_.modAccumInit.begin(), voicePlan_.modAccumInit.end(),
                  v.modAccum.begin());
        v.active = false; v.held = false; v.note = -1;
        v.fade = 1.0f; v.fadeInc = 0.0f; v.quietBlocks = 0;
    }
    for (auto& m : globalModules_) if (m) m->reset();
    std::fill(globalInBuf_.begin(), globalInBuf_.end(), 0.0f);
    std::fill(globalOutBuf_.begin(), globalOutBuf_.end(), 0.0f);
    std::fill(voiceBus_.begin(), voiceBus_.end(), 0.0f);
    std::fill(globalSrcValue_.begin(), globalSrcValue_.end(), 0.0f);
    sustain_ = false;
    lastPeak_ = 0.0f;
    baseDirty_ = true;
}

int GraphInstance::activeVoiceCount() const noexcept {
    int n = 0;
    for (const auto& v : voices_) if (v.active) ++n;
    return n;
}

// ---------------------------------------------------------------------------
// Parameter resolution
// ---------------------------------------------------------------------------

void GraphInstance::resolveBaseParams() noexcept {
    // Start from the manifest defaults, so any module parameter nobody exposed
    // sits at a sensible value rather than zero.
    std::copy(voicePlan_.paramDefaultNorm.begin(), voicePlan_.paramDefaultNorm.end(),
              voicePlan_.baseNorm.begin());
    std::copy(globalPlan_.paramDefaultNorm.begin(), globalPlan_.paramDefaultNorm.end(),
              globalPlan_.baseNorm.begin());

    // Macros offset exposed parameters in the exposed parameter's own
    // normalised domain, then everything is clamped once.
    //
    // The offset is measured FROM THE MACRO'S DEFAULT POSITION, so a macro that
    // nobody has touched contributes exactly nothing. Without that subtraction
    // the stock macro row silently rewrote every instrument ever generated: the
    // "Body" macro sits at 0.4 with a +0.5 route to the sub oscillator, so
    // every patch got a sub-octave at 0.2 no matter what it asked for - loud
    // enough to dominate a plucked string and make it sound an octave low. The
    // same went for Space adding reverb to sounds the model had deliberately
    // kept dry, and Drive adding grit to clean ones.
    //
    // It also made the macros feel dead. Half their travel was spent undoing an
    // offset that should never have been applied, and the parameters they moved
    // were already sitting near the top of their range.
    std::copy(exposedNorm_.begin(), exposedNorm_.end(), effExposed_.begin());
    for (const auto& mr : macroRoutes_) {
        const size_t mi = static_cast<size_t>(mr.macroIndex);
        const float  now  = shapeMod(macroNorm_[mi], mr.curve);
        const float  rest = mi < macroDefaultNorm_.size()
                              ? shapeMod(macroDefaultNorm_[mi], mr.curve) : 0.0f;
        effExposed_[static_cast<size_t>(mr.exposedIndex)] += mr.depth * (now - rest);
    }
    for (auto& e : effExposed_) e = clamp01(e);

    for (const auto& b : binds_) {
        const size_t e = static_cast<size_t>(b.exposedIndex);
        const float value = applyTaper(effExposed_[e], exposedMin_[e], exposedMax_[e],
                                       exposedTaper_[e]);
        const float mapped = value * b.scale + b.offset;

        ScopePlan& plan = b.dstGlobal ? globalPlan_ : voicePlan_;
        const size_t f = static_cast<size_t>(b.dstFlatParam);
        plan.baseNorm[f] = removeTaper(mapped, plan.paramMin[f], plan.paramMax[f], plan.paramTaper[f]);
    }
}

namespace {

/// One route's contribution to a parameter whose modulation multiplies.
///
/// Unipolar sources scale between (1 - depth) and 1, so an envelope at full
/// depth IS the envelope and everything else can only ever pull the level
/// down. Bipolar sources swing around unity, which is what tremolo means.
inline float multiplicativeFactor(float shaped, float depth, bool bipolar) noexcept {
    return clamp01(1.0f + depth * (bipolar ? shaped : shaped - 1.0f));
}

} // namespace

void GraphInstance::resolveVoiceMod(Voice& v) noexcept {
    std::copy(voicePlan_.modAccumInit.begin(), voicePlan_.modAccumInit.end(),
              v.modAccum.begin());
    for (const auto& r : modRoutes_) {
        if (r.dstGlobal) continue;
        float s = r.srcGlobal ? globalSrcValue_[static_cast<size_t>(r.srcPlanIndex)]
                              : v.srcValue[static_cast<size_t>(r.srcPlanIndex)];
        if (r.bipolar) s = s * 2.0f - 1.0f;
        const size_t f = static_cast<size_t>(r.dstFlatParam);
        const float shaped = shapeMod(s, r.curve);
        if (voicePlan_.paramMulMod[f])
            v.modAccum[f] *= multiplicativeFactor(shaped, r.depth, r.bipolar);
        else
            v.modAccum[f] += r.depth * shaped;
    }
}

void GraphInstance::resolveGlobalMod() noexcept {
    std::copy(globalPlan_.modAccumInit.begin(), globalPlan_.modAccumInit.end(),
              globalModAccum_.begin());
    for (const auto& r : modRoutes_) {
        if (!r.dstGlobal) continue;
        // Voice-scope sources cannot reach global targets; the validator
        // rejects those routes, so anything here is global -> global.
        if (!r.srcGlobal) continue;
        float s = globalSrcValue_[static_cast<size_t>(r.srcPlanIndex)];
        if (r.bipolar) s = s * 2.0f - 1.0f;
        const size_t f = static_cast<size_t>(r.dstFlatParam);
        const float shaped = shapeMod(s, r.curve);
        if (globalPlan_.paramMulMod[f])
            globalModAccum_[f] *= multiplicativeFactor(shaped, r.depth, r.bipolar);
        else
            globalModAccum_[f] += r.depth * shaped;
    }
}

// ---------------------------------------------------------------------------
// Rendering
// ---------------------------------------------------------------------------

void GraphInstance::renderVoice(Voice& v, int numSamples) noexcept {
    const size_t n = voicePlan_.nodes.size();

    // Portamento, and the per-block snapshot of performance state.
    v.ctx.glideFreqHz += (v.ctx.baseFreqHz - v.ctx.glideFreqHz) * glideCoeff_;
    v.ctx.pitchBendSemis = pitchBend_;
    v.ctx.modWheel       = modWheel_;
    v.ctx.aftertouch     = aftertouch_;

    for (size_t i = 0; i < n; ++i) {
        const PlanNode& pn = voicePlan_.nodes[i];
        float* inBase  = voiceIn(v, static_cast<int>(i));
        float* outBase = voiceOut(v, static_cast<int>(i));
        zeroNodeBuffer(inBase);
        zeroNodeBuffer(outBase);

        for (const auto& src : pn.sources) {
            const float* s = voiceOut(v, src.first);
            const float g = src.second;
            for (int ch = 0; ch < kNumChannels; ++ch) {
                const float* sc = s + ch * kControlBlock;
                float* dc = inBase + ch * kControlBlock;
                for (int k = 0; k < numSamples; ++k) dc[k] += sc[k] * g;
            }
        }

        Module* mod = v.modules[i].get();
        for (int pi = 0; pi < pn.numParams; ++pi) {
            const size_t f = static_cast<size_t>(pn.paramOffset + pi);
            const float norm = voicePlan_.paramMulMod[f]
                                 ? clamp01(voicePlan_.baseNorm[f] * v.modAccum[f])
                                 : clamp01(voicePlan_.baseNorm[f] + v.modAccum[f]);
            mod->setParam(pi, applyTaper(norm, voicePlan_.paramMin[f], voicePlan_.paramMax[f],
                                         voicePlan_.paramTaper[f]));
        }

        const float* inPtr[kNumChannels]  = { inBase,  inBase  + kControlBlock };
        float*       outPtr[kNumChannels] = { outBase, outBase + kControlBlock };
        ProcessArgs args;
        args.in = inPtr; args.out = outPtr; args.numSamples = numSamples;
        args.g = &global_; args.v = &v.ctx;
        mod->process(args);
    }

    for (size_t i = 0; i < n; ++i)
        v.srcValue[i] = sanitize(v.modules[i]->modOut());
}

void GraphInstance::renderGlobal(int numSamples) noexcept {
    const size_t n = globalPlan_.nodes.size();

    for (size_t i = 0; i < n; ++i) {
        const PlanNode& pn = globalPlan_.nodes[i];
        float* inBase  = globalIn(static_cast<int>(i));
        float* outBase = globalOut(static_cast<int>(i));
        zeroNodeBuffer(outBase);

        // The voice bus already holds everything the voices routed here.
        std::memcpy(inBase, voiceBus(static_cast<int>(i)),
                    sizeof(float) * kNumChannels * kControlBlock);

        for (const auto& src : pn.sources) {
            const float* s = globalOut(src.first);
            const float g = src.second;
            for (int ch = 0; ch < kNumChannels; ++ch) {
                const float* sc = s + ch * kControlBlock;
                float* dc = inBase + ch * kControlBlock;
                for (int k = 0; k < numSamples; ++k) dc[k] += sc[k] * g;
            }
        }

        Module* mod = globalModules_[i].get();
        for (int pi = 0; pi < pn.numParams; ++pi) {
            const size_t f = static_cast<size_t>(pn.paramOffset + pi);
            const float norm = globalPlan_.paramMulMod[f]
                                 ? clamp01(globalPlan_.baseNorm[f] * globalModAccum_[f])
                                 : clamp01(globalPlan_.baseNorm[f] + globalModAccum_[f]);
            mod->setParam(pi, applyTaper(norm, globalPlan_.paramMin[f], globalPlan_.paramMax[f],
                                         globalPlan_.paramTaper[f]));
        }

        const float* inPtr[kNumChannels]  = { inBase,  inBase  + kControlBlock };
        float*       outPtr[kNumChannels] = { outBase, outBase + kControlBlock };
        ProcessArgs args;
        args.in = inPtr; args.out = outPtr; args.numSamples = numSamples;
        args.g = &global_; args.v = nullptr;
        mod->process(args);
    }

    for (size_t i = 0; i < n; ++i)
        globalSrcValue_[i] = sanitize(globalModules_[i]->modOut());
}

void GraphInstance::processControlBlock(int numSamples) noexcept {
    if (baseDirty_) { resolveBaseParams(); baseDirty_ = false; }

    std::fill(voiceBus_.begin(), voiceBus_.end(), 0.0f);

    for (auto& v : voices_) {
        if (!v.active) continue;

        resolveVoiceMod(v);
        renderVoice(v, numSamples);

        // Mix this voice into the global section, applying the anti-click
        // fade that a stolen voice starts life with.
        float peak = 0.0f;
        for (const auto& tap : voiceTaps_) {
            const float* src = voiceOut(v, tap.voicePlanIndex);
            float* dst = voiceBus(tap.globalPlanIndex);
            for (int ch = 0; ch < kNumChannels; ++ch) {
                const float* sc = src + ch * kControlBlock;
                float* dc = dst + ch * kControlBlock;
                for (int k = 0; k < numSamples; ++k) {
                    const float f = clamp01(v.fade + v.fadeInc * static_cast<float>(k));
                    const float s = sc[k] * tap.gain * f;
                    dc[k] += s;
                    peak = std::max(peak, std::abs(s));
                }
            }
        }
        v.fade = clamp01(v.fade + v.fadeInc * static_cast<float>(numSamples));
        if (v.fade >= 1.0f) v.fadeInc = 0.0f;

        // A voice stays alive while an envelope holds it, then until it has
        // actually gone quiet - which is what lets long reverb-like tails in
        // the voice chain finish properly instead of being cut off.
        bool holding = false;
        for (const auto& m : v.modules) if (m->holdsVoice()) { holding = true; break; }
        if (holding) {
            v.quietBlocks = 0;
        } else if (peak < 1.0e-4f) {
            if (++v.quietBlocks > 16) {
                v.active = false;
                v.held = false;
                v.note = -1;
            }
        } else {
            v.quietBlocks = 0;
        }
    }

    resolveGlobalMod();
    renderGlobal(numSamples);
}

void GraphInstance::process(float* const* out, int numSamples) noexcept {
    if (masterPlanIndex_ < 0 || out == nullptr) return;

    int offset = 0;
    while (offset < numSamples) {
        const int m = std::min(kControlBlock, numSamples - offset);
        processControlBlock(m);

        const float* master = globalOut(masterPlanIndex_);
        for (int ch = 0; ch < kNumChannels; ++ch)
            std::memcpy(out[ch] + offset, master + ch * kControlBlock, sizeof(float) * static_cast<size_t>(m));
        offset += m;
    }

    // NaN / peak guard. Cheap enough to run unconditionally, and it is the
    // last line of defence between generated content and a pair of monitors.
    float peak = 0.0f;
    bool bad = false;
    for (int ch = 0; ch < kNumChannels; ++ch) {
        for (int k = 0; k < numSamples; ++k) {
            const float s = out[ch][k];
            if (!isFinite(s)) { bad = true; break; }
            peak = std::max(peak, std::abs(s));
        }
        if (bad) break;
    }

    if (bad) {
        for (int ch = 0; ch < kNumChannels; ++ch)
            std::memset(out[ch], 0, sizeof(float) * static_cast<size_t>(numSamples));
        reset();
        nanTripped_ = true;
        lastPeak_ = 0.0f;
        return;
    }

    lastPeak_ = peak;
}

} // namespace forge
