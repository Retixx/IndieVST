#include "core/dsp/GraphBuilder.h"

#include "core/dsp/AssetBaker.h"
#include "core/ir/IrValidator.h"

#include <unordered_map>

namespace forge {
namespace {

/// Fills a scope's flat parameter tables and records each node's offset.
void buildScopePlan(const ir::Instrument& inst, const std::vector<int>& order,
                    ScopePlan& plan, std::unordered_map<int, int>& irToPlan) {
    const Registry& reg = Registry::instance();
    plan.nodes.clear();
    plan.totalParams = 0;

    for (int irIndex : order) {
        const auto& node = inst.nodes[static_cast<size_t>(irIndex)];
        const ModuleManifest* man = reg.find(node.type);
        if (man == nullptr) continue;

        PlanNode pn;
        pn.irIndex       = irIndex;
        pn.man           = man;
        pn.paramOffset   = plan.totalParams;
        pn.numParams     = static_cast<int>(man->params.size());
        pn.producesAudio = man->audioOuts > 0;
        pn.isModSource   = man->isModSource;
        pn.holdsVoice    = man->holdsVoice;

        for (const auto& p : man->params) {
            plan.paramMin.push_back(p.min);
            plan.paramMax.push_back(p.max);
            plan.paramTaper.push_back(p.taper);
            plan.paramDefaultNorm.push_back(removeTaper(p.def, p.min, p.max, p.taper));
            plan.paramModBaseNorm.push_back(
                p.hasModBase ? removeTaper(p.modBase, p.min, p.max, p.taper)
                             : removeTaper(p.def,     p.min, p.max, p.taper));
            plan.paramMulMod.push_back(p.multiplicativeMod ? 1 : 0);
        }
        plan.totalParams += pn.numParams;

        irToPlan[irIndex] = static_cast<int>(plan.nodes.size());
        plan.nodes.push_back(std::move(pn));
    }

    plan.baseNorm     = plan.paramDefaultNorm;
    plan.modAccumInit.assign(static_cast<size_t>(plan.totalParams), 0.0f);
}

} // namespace

std::unique_ptr<GraphInstance> GraphBuilder::build(const ir::Instrument& inst,
                                                   double sampleRate,
                                                   ir::IrReport& report) {
    ir::ValidationResult vr;
    if (!ir::validate(inst, report, &vr)) return nullptr;

    auto graph = std::unique_ptr<GraphInstance>(new GraphInstance());
    graph->ir_ = inst;
    graph->global_.sampleRate = clampT(sampleRate, 8000.0, 384000.0);
    graph->global_.seed       = inst.seed;

    bakeAssets(inst.assets, graph->assets_, report);

    // --- plans ---
    std::unordered_map<int, int> voiceMap, globalMap;
    buildScopePlan(inst, vr.voiceOrder,  graph->voicePlan_,  voiceMap);
    buildScopePlan(inst, vr.globalOrder, graph->globalPlan_, globalMap);

    if (graph->globalPlan_.nodes.empty()) {
        report.error("nodes", "No global-scope nodes; there is nothing to output through.");
        return nullptr;
    }

    for (size_t i = 0; i < graph->globalPlan_.nodes.size(); ++i) {
        const auto& node = inst.nodes[static_cast<size_t>(graph->globalPlan_.nodes[i].irIndex)];
        if (node.type == "out.master") graph->masterPlanIndex_ = static_cast<int>(i);
    }
    if (graph->masterPlanIndex_ < 0) {
        report.error("nodes", "No out.master node in the global section.");
        return nullptr;
    }

    // --- audio wiring ---
    for (const auto& c : inst.audio) {
        const int fi = inst.indexOfNode(c.from);
        const int ti = inst.indexOfNode(c.to);
        if (fi < 0 || ti < 0) continue;
        const Scope fs = inst.nodes[static_cast<size_t>(fi)].scope;
        const Scope ts = inst.nodes[static_cast<size_t>(ti)].scope;

        if (fs == Scope::Voice && ts == Scope::Voice) {
            auto f = voiceMap.find(fi), t = voiceMap.find(ti);
            if (f != voiceMap.end() && t != voiceMap.end())
                graph->voicePlan_.nodes[static_cast<size_t>(t->second)]
                     .sources.emplace_back(f->second, c.gain);
        } else if (fs == Scope::Global && ts == Scope::Global) {
            auto f = globalMap.find(fi), t = globalMap.find(ti);
            if (f != globalMap.end() && t != globalMap.end())
                graph->globalPlan_.nodes[static_cast<size_t>(t->second)]
                     .sources.emplace_back(f->second, c.gain);
        } else if (fs == Scope::Voice && ts == Scope::Global) {
            auto f = voiceMap.find(fi), t = globalMap.find(ti);
            if (f != voiceMap.end() && t != globalMap.end())
                graph->voiceTaps_.push_back({f->second, t->second, c.gain});
        }
        // global -> voice is rejected by the validator.
    }

    // --- modulation matrix ---
    for (const auto& r : inst.mod) {
        const int si = inst.indexOfNode(r.source);
        const int ti = inst.indexOfNode(r.targetNode);
        if (si < 0 || ti < 0) continue;

        const auto& srcNode = inst.nodes[static_cast<size_t>(si)];
        const auto& dstNode = inst.nodes[static_cast<size_t>(ti)];
        const ModuleManifest* dstMan = Registry::instance().find(dstNode.type);
        if (dstMan == nullptr) continue;
        const int pi = dstMan->indexOfParam(r.targetParam);
        if (pi < 0) continue;

        RtModRoute rt;
        rt.srcGlobal = (srcNode.scope == Scope::Global);
        rt.dstGlobal = (dstNode.scope == Scope::Global);
        rt.depth     = clampT(r.depth, -1.0f, 1.0f);
        rt.curve     = r.curve;
        rt.bipolar   = r.bipolar;

        auto& srcMap = rt.srcGlobal ? globalMap : voiceMap;
        auto& dstMap = rt.dstGlobal ? globalMap : voiceMap;
        auto s = srcMap.find(si), d = dstMap.find(ti);
        if (s == srcMap.end() || d == dstMap.end()) continue;

        rt.srcPlanIndex = s->second;
        const ScopePlan& dstPlan = rt.dstGlobal ? graph->globalPlan_ : graph->voicePlan_;
        rt.dstFlatParam = dstPlan.nodes[static_cast<size_t>(d->second)].paramOffset + pi;
        graph->modRoutes_.push_back(rt);
    }

    // --- exposed parameter binds ---
    for (size_t p = 0; p < inst.params.size(); ++p) {
        for (const auto& b : inst.params[p].bind) {
            const int ni = inst.indexOfNode(b.node);
            if (ni < 0) continue;
            const auto& node = inst.nodes[static_cast<size_t>(ni)];
            const ModuleManifest* man = Registry::instance().find(node.type);
            if (man == nullptr) continue;
            const int pi = man->indexOfParam(b.param);
            if (pi < 0) continue;

            RtBind rb;
            rb.exposedIndex = static_cast<int>(p);
            rb.dstGlobal    = (node.scope == Scope::Global);
            rb.scale        = b.scale;
            rb.offset       = b.offset;

            auto& map = rb.dstGlobal ? globalMap : voiceMap;
            auto it = map.find(ni);
            if (it == map.end()) continue;
            const ScopePlan& plan = rb.dstGlobal ? graph->globalPlan_ : graph->voicePlan_;
            rb.dstFlatParam = plan.nodes[static_cast<size_t>(it->second)].paramOffset + pi;
            graph->binds_.push_back(rb);
        }
    }

    // --- resolve the "modulated but unbound" base values ---
    //
    // A vca gain sitting at its default of 1.0 with an envelope added on top
    // would clamp at maximum for the whole note. Any parameter that declares a
    // modBase and is being modulated without a knob bound to it starts from
    // that instead. This is what makes "env -> vca.gain" mean what a musician
    // expects it to mean.
    {
        auto applyModBases = [](ScopePlan& plan,
                                const std::vector<RtModRoute>& routes,
                                const std::vector<RtBind>& binds,
                                bool wantGlobal) {
            if (plan.totalParams == 0) return;
            std::vector<char> modulated(static_cast<size_t>(plan.totalParams), 0);
            std::vector<char> bound(static_cast<size_t>(plan.totalParams), 0);
            for (const auto& r : routes)
                if (r.dstGlobal == wantGlobal && r.dstFlatParam >= 0)
                    modulated[static_cast<size_t>(r.dstFlatParam)] = 1;
            for (const auto& b : binds)
                if (b.dstGlobal == wantGlobal && b.dstFlatParam >= 0)
                    bound[static_cast<size_t>(b.dstFlatParam)] = 1;
            for (size_t i = 0; i < modulated.size(); ++i) {
                // Multiplication only applies where something is actually
                // routed in; an unmodulated gain must stay at its knob value,
                // not collapse to a product of nothing.
                if (!modulated[i]) plan.paramMulMod[i] = 0;
                plan.modAccumInit[i] = plan.paramMulMod[i] ? 1.0f : 0.0f;

                // A parameter that SCALES needs no silent base: the product is
                // already zero whenever the envelope is, and dropping the base
                // to zero as well would multiply everything away.
                if (modulated[i] && !bound[i] && !plan.paramMulMod[i])
                    plan.paramDefaultNorm[i] = plan.paramModBaseNorm[i];
            }
            plan.baseNorm = plan.paramDefaultNorm;
        };
        applyModBases(graph->voicePlan_,  graph->modRoutes_, graph->binds_, false);
        applyModBases(graph->globalPlan_, graph->modRoutes_, graph->binds_, true);
    }

    // --- exposed values and their ranges ---
    graph->exposedNorm_.resize(inst.params.size());
    graph->effExposed_.resize(inst.params.size());
    graph->exposedMin_.resize(inst.params.size());
    graph->exposedMax_.resize(inst.params.size());
    graph->exposedTaper_.resize(inst.params.size());
    for (size_t i = 0; i < inst.params.size(); ++i) {
        graph->exposedNorm_[i]  = inst.params[i].defaultNormalised();
        graph->exposedMin_[i]   = inst.params[i].min;
        graph->exposedMax_[i]   = inst.params[i].max;
        graph->exposedTaper_[i] = inst.params[i].taper;
    }
    graph->macroNorm_.resize(inst.macros.size());
    graph->macroDefaultNorm_.resize(inst.macros.size());
    for (size_t i = 0; i < inst.macros.size(); ++i) {
        graph->macroNorm_[i]        = clamp01(inst.macros[i].def);
        graph->macroDefaultNorm_[i] = graph->macroNorm_[i];
    }

    // --- macro routes, resolved to indices ---
    for (size_t m = 0; m < inst.macros.size(); ++m) {
        for (const auto& r : inst.macros[m].routes) {
            int exposedIndex = -1;
            for (size_t p = 0; p < inst.params.size(); ++p)
                if (inst.params[p].id == r.param) { exposedIndex = static_cast<int>(p); break; }
            if (exposedIndex < 0) continue;
            graph->macroRoutes_.push_back({static_cast<int>(m), exposedIndex,
                                           clampT(r.depth, -1.0f, 1.0f), r.curve});
        }
    }

    // --- instantiate global modules ---
    const size_t gCount = graph->globalPlan_.nodes.size();
    graph->globalModules_.resize(gCount);
    for (size_t i = 0; i < gCount; ++i) {
        const auto& pn = graph->globalPlan_.nodes[i];
        const auto& node = inst.nodes[static_cast<size_t>(pn.irIndex)];
        auto mod = pn.man->factory ? pn.man->factory() : nullptr;
        if (mod == nullptr) {
            report.error("nodes", "Could not instantiate module type '" + node.type + "'.");
            return nullptr;
        }
        ModulePrepareInfo info;
        info.sampleRate = graph->global_.sampleRate;
        info.maxBlock   = kControlBlock;
        info.settings   = &node.settings;
        info.assets     = &graph->assets_;
        info.seed       = inst.seed ^ (static_cast<uint64_t>(pn.irIndex + 1) * 2654435761ull);
        info.scope      = Scope::Global;
        mod->prepare(info);
        mod->reset();
        graph->globalModules_[i] = std::move(mod);
    }

    const size_t bufPerNode = static_cast<size_t>(kNumChannels) * kControlBlock;
    graph->globalInBuf_.assign(gCount * bufPerNode, 0.0f);
    graph->globalOutBuf_.assign(gCount * bufPerNode, 0.0f);
    graph->voiceBus_.assign(gCount * bufPerNode, 0.0f);
    graph->globalSrcValue_.assign(gCount, 0.0f);
    graph->globalModAccum_.assign(static_cast<size_t>(graph->globalPlan_.totalParams), 0.0f);

    // --- instantiate voices ---
    const size_t vCount = graph->voicePlan_.nodes.size();
    const int polyphony = clampT(inst.polyphony, 1, kMaxVoices);
    graph->voices_.resize(static_cast<size_t>(polyphony));

    for (int v = 0; v < polyphony; ++v) {
        Voice& voice = graph->voices_[static_cast<size_t>(v)];
        voice.modules.resize(vCount);
        for (size_t i = 0; i < vCount; ++i) {
            const auto& pn = graph->voicePlan_.nodes[i];
            const auto& node = inst.nodes[static_cast<size_t>(pn.irIndex)];
            auto mod = pn.man->factory ? pn.man->factory() : nullptr;
            if (mod == nullptr) {
                report.error("nodes", "Could not instantiate module type '" + node.type + "'.");
                return nullptr;
            }
            ModulePrepareInfo info;
            info.sampleRate = graph->global_.sampleRate;
            info.maxBlock   = kControlBlock;
            info.settings   = &node.settings;
            info.assets     = &graph->assets_;
            // Distinct per voice so unison stacks and noise sources do not
            // phase-align, but still fully deterministic for a given seed.
            info.seed = inst.seed
                      ^ (static_cast<uint64_t>(pn.irIndex + 1) * 2654435761ull)
                      ^ (static_cast<uint64_t>(v + 1) * 40503ull);
            info.scope = Scope::Voice;
            mod->prepare(info);
            mod->reset();
            voice.modules[i] = std::move(mod);
        }
        voice.inBuf.assign(vCount * bufPerNode, 0.0f);
        voice.outBuf.assign(vCount * bufPerNode, 0.0f);
        voice.srcValue.assign(vCount, 0.0f);
        voice.modAccum.assign(static_cast<size_t>(graph->voicePlan_.totalParams), 0.0f);
    }

    // Portamento coefficient, evaluated once per control block.
    const float glideSec = std::max(inst.glideMs, 0.0f) * 0.001f;
    const float dt = static_cast<float>(kControlBlock / graph->global_.sampleRate);
    graph->glideCoeff_ = (glideSec < 1.0e-4f) ? 1.0f : clamp01(1.0f - std::exp(-dt / glideSec));

    graph->baseDirty_ = true;
    return graph;
}

} // namespace forge
