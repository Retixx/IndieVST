#include "core/ir/IrRepair.h"

#include "core/dsp/Registry.h"
#include "core/ir/IrValidator.h"

#include <algorithm>
#include <unordered_map>
#include <unordered_set>

namespace forge::ir {
namespace {

const ModuleManifest* manifestOf(const NodeSpec& n) {
    return Registry::instance().find(n.type);
}

std::string uniqueId(const std::string& wanted, const std::unordered_set<std::string>& taken) {
    if (taken.find(wanted) == taken.end()) return wanted;
    for (int i = 2; i < 1000; ++i) {
        std::string candidate = wanted + "_" + std::to_string(i);
        if (candidate.size() > 32) candidate = candidate.substr(0, 32);
        if (taken.find(candidate) == taken.end()) return candidate;
    }
    return wanted + "_x";
}

// --- step 0: identifiers ---------------------------------------------------

void normaliseIdentifiers(Instrument& inst, IrReport& report) {
    std::unordered_map<std::string, std::string> nodeRemap, paramRemap, macroRemap, assetRemap;

    auto pass = [&](auto& collection, std::unordered_map<std::string, std::string>& remap,
                    const char* what) {
        std::unordered_set<std::string> taken;
        for (auto& item : collection) {
            const std::string original = item.id;
            std::string id = isValidIdentifier(original) ? original : sanitiseIdentifier(original);
            id = uniqueId(id, taken);
            taken.insert(id);
            if (id != original) {
                report.fixed(std::string(what) + " '" + original + "'",
                             "Renamed to '" + id + "' so it matches the id rules.");
                remap[original] = id;
                item.id = id;
            }
        }
    };

    pass(inst.nodes,  nodeRemap,  "node");
    pass(inst.params, paramRemap, "parameter");
    pass(inst.macros, macroRemap, "macro");
    pass(inst.assets, assetRemap, "asset");

    auto remapStr = [](std::string& s, const std::unordered_map<std::string, std::string>& m) {
        auto it = m.find(s);
        if (it != m.end()) s = it->second;
    };

    for (auto& c : inst.audio) { remapStr(c.from, nodeRemap); remapStr(c.to, nodeRemap); }
    for (auto& r : inst.mod)   { remapStr(r.source, nodeRemap); remapStr(r.targetNode, nodeRemap); }
    for (auto& p : inst.params) for (auto& b : p.bind) remapStr(b.node, nodeRemap);
    for (auto& m : inst.macros) for (auto& r : m.routes) remapStr(r.param, paramRemap);
    for (auto& s : inst.ui.sections) for (auto& p : s.params) remapStr(p, paramRemap);
    for (auto& m : inst.ui.macroRow) remapStr(m, macroRemap);

    if (!assetRemap.empty()) {
        for (auto& n : inst.nodes) {
            if (!n.settings.is_object()) continue;
            for (auto it = n.settings.begin(); it != n.settings.end(); ++it) {
                if (!it->is_string()) continue;
                auto found = assetRemap.find(it->get<std::string>());
                if (found != assetRemap.end()) *it = found->second;
            }
        }
    }
}

// --- step 1: unknown module types -----------------------------------------

void dropUnknownNodes(Instrument& inst, IrReport& report) {
    std::unordered_set<std::string> removed;
    for (size_t i = 0; i < inst.nodes.size();) {
        if (manifestOf(inst.nodes[i]) == nullptr) {
            report.fixed("nodes", "Removed node '" + inst.nodes[i].id
                                  + "' because module type '" + inst.nodes[i].type
                                  + "' does not exist.");
            removed.insert(inst.nodes[i].id);
            inst.nodes.erase(inst.nodes.begin() + static_cast<long>(i));
        } else {
            ++i;
        }
    }
    if (removed.empty()) return;

    inst.audio.erase(std::remove_if(inst.audio.begin(), inst.audio.end(),
                                    [&](const AudioConn& c) {
                                        return removed.count(c.from) || removed.count(c.to);
                                    }), inst.audio.end());
    inst.mod.erase(std::remove_if(inst.mod.begin(), inst.mod.end(),
                                  [&](const ModRoute& r) {
                                      return removed.count(r.source) || removed.count(r.targetNode);
                                  }), inst.mod.end());
}

// --- step 2: dangling references ------------------------------------------

void dropDanglingReferences(Instrument& inst, IrReport& report) {
    auto nodeExists = [&](const std::string& id) { return inst.findNode(id) != nullptr; };

    const size_t audioBefore = inst.audio.size();
    inst.audio.erase(std::remove_if(inst.audio.begin(), inst.audio.end(),
        [&](const AudioConn& c) {
            if (!nodeExists(c.from) || !nodeExists(c.to)) return true;
            const auto* fm = manifestOf(*inst.findNode(c.from));
            const auto* tm = manifestOf(*inst.findNode(c.to));
            if (fm == nullptr || tm == nullptr) return true;
            if (fm->audioOuts == 0 || tm->audioIns == 0) return true;
            // Global cannot feed voice - signal flows one way.
            if (inst.findNode(c.from)->scope == Scope::Global
             && inst.findNode(c.to)->scope   == Scope::Voice) return true;
            return false;
        }), inst.audio.end());
    if (inst.audio.size() != audioBefore)
        report.fixed("audio", "Removed " + std::to_string(audioBefore - inst.audio.size())
                              + " connection(s) that referenced something that does not exist "
                                "or pointed the wrong way.");

    const size_t modBefore = inst.mod.size();
    inst.mod.erase(std::remove_if(inst.mod.begin(), inst.mod.end(),
        [&](const ModRoute& r) {
            const NodeSpec* src = inst.findNode(r.source);
            const NodeSpec* tgt = inst.findNode(r.targetNode);
            if (src == nullptr || tgt == nullptr) return true;
            const auto* sm = manifestOf(*src);
            const auto* tm = manifestOf(*tgt);
            if (sm == nullptr || tm == nullptr || !sm->isModSource) return true;
            const int pi = tm->indexOfParam(r.targetParam);
            if (pi < 0 || !tm->params[static_cast<size_t>(pi)].modulatable) return true;
            if (src->scope == Scope::Voice && tgt->scope == Scope::Global) return true;
            return false;
        }), inst.mod.end());
    if (inst.mod.size() != modBefore)
        report.fixed("mod", "Removed " + std::to_string(modBefore - inst.mod.size())
                            + " modulation route(s) with an invalid source or target.");

    for (auto& p : inst.params) {
        p.bind.erase(std::remove_if(p.bind.begin(), p.bind.end(),
            [&](const ParamBind& b) {
                const NodeSpec* n = inst.findNode(b.node);
                if (n == nullptr) return true;
                const auto* man = manifestOf(*n);
                return man == nullptr || man->indexOfParam(b.param) < 0;
            }), p.bind.end());
    }

    // Assets referenced by nothing are harmless but wasteful; assets that do
    // not exist are a hard error the module cannot recover from, so blank the
    // reference and let the module fall back.
    std::unordered_set<std::string> assetIds;
    for (const auto& a : inst.assets) assetIds.insert(a.id);
    for (auto& n : inst.nodes) {
        const auto* man = manifestOf(n);
        if (man == nullptr || !n.settings.is_object()) continue;
        for (const auto& s : man->settings) {
            const bool isAsset = s.type == SettingDesc::Type::AssetWavetable
                              || s.type == SettingDesc::Type::AssetCurve
                              || s.type == SettingDesc::Type::AssetEnvelope;
            if (!isAsset) continue;
            auto it = n.settings.find(s.id);
            if (it == n.settings.end() || !it->is_string()) continue;
            const std::string id = it->get<std::string>();
            if (!id.empty() && assetIds.count(id) == 0) {
                report.fixed("nodes." + n.id + ".settings." + s.id,
                             "Cleared a reference to asset '" + id + "', which was never declared.");
                *it = "";
            }
        }
    }
}

// --- step 3: cycles --------------------------------------------------------

void breakCycles(Instrument& inst, IrReport& report) {
    for (Scope scope : {Scope::Voice, Scope::Global}) {
        for (int guard = 0; guard < 64; ++guard) {
            std::vector<int> order;
            std::vector<std::pair<int, int>> cycleEdges;
            if (topoSort(inst, scope, order, &cycleEdges)) break;
            if (cycleEdges.empty()) break;

            // Remove the last edge that participates in the cycle. "Last" is
            // deterministic and, in practice, the edge the model added to close
            // the loop.
            const auto& e = cycleEdges.back();
            const std::string from = inst.nodes[static_cast<size_t>(e.first)].id;
            const std::string to   = inst.nodes[static_cast<size_t>(e.second)].id;
            const size_t before = inst.audio.size();
            inst.audio.erase(std::remove_if(inst.audio.begin(), inst.audio.end(),
                                            [&](const AudioConn& c) {
                                                return c.from == from && c.to == to;
                                            }), inst.audio.end());
            if (inst.audio.size() == before) break;   // nothing removed: bail rather than spin
            report.fixed("audio", "Broke a feedback loop by removing the connection "
                                  + from + " -> " + to
                                  + ". Feedback needs an fx.delay or filter.comb in the loop.");
        }
    }
}

// --- step 4: output path ---------------------------------------------------

std::unordered_set<std::string> nodesWithOutgoingAudio(const Instrument& inst) {
    std::unordered_set<std::string> s;
    for (const auto& c : inst.audio) s.insert(c.from);
    return s;
}

void ensureOutputPath(Instrument& inst, IrReport& report) {
    // 4a. exactly one out.master
    int masterCount = 0;
    std::string masterId;
    for (size_t i = 0; i < inst.nodes.size();) {
        if (inst.nodes[i].type == "out.master") {
            if (++masterCount == 1) { masterId = inst.nodes[i].id; ++i; continue; }
            const std::string dup = inst.nodes[i].id;
            // Re-point anything feeding the duplicate at the real master.
            for (auto& c : inst.audio) if (c.to == dup) c.to = masterId;
            inst.audio.erase(std::remove_if(inst.audio.begin(), inst.audio.end(),
                                            [&](const AudioConn& c) { return c.from == dup; }),
                             inst.audio.end());
            inst.mod.erase(std::remove_if(inst.mod.begin(), inst.mod.end(),
                                          [&](const ModRoute& r) {
                                              return r.source == dup || r.targetNode == dup;
                                          }), inst.mod.end());
            inst.nodes.erase(inst.nodes.begin() + static_cast<long>(i));
            report.fixed("nodes", "Merged a duplicate out.master into the single output stage.");
            continue;
        }
        ++i;
    }

    if (masterCount == 0) {
        std::unordered_set<std::string> taken;
        for (const auto& n : inst.nodes) taken.insert(n.id);
        NodeSpec master;
        master.id    = uniqueId("master", taken);
        master.type  = "out.master";
        master.scope = Scope::Global;
        masterId     = master.id;
        inst.nodes.push_back(std::move(master));
        report.fixed("nodes", "Added the mandatory out.master output stage.");
    }

    // 4b. every global sink reaches the master
    {
        const auto hasOut = nodesWithOutgoingAudio(inst);
        for (const auto& n : inst.nodes) {
            if (n.scope != Scope::Global || n.id == masterId) continue;
            const auto* man = manifestOf(n);
            if (man == nullptr || man->audioOuts == 0) continue;
            if (hasOut.count(n.id)) continue;
            inst.audio.push_back({n.id, masterId, 1.0f});
            report.fixed("audio", "Connected '" + n.id + "' to the output - it was producing "
                                  "audio that went nowhere.");
        }
    }

    // 4c. at least one voice-scope node must feed the global section
    bool voiceReachesGlobal = false;
    for (const auto& c : inst.audio) {
        const NodeSpec* f = inst.findNode(c.from);
        const NodeSpec* t = inst.findNode(c.to);
        if (f && t && f->scope == Scope::Voice && t->scope == Scope::Global) {
            voiceReachesGlobal = true;
            break;
        }
    }

    if (!voiceReachesGlobal) {
        // Find the entry point of the global FX chain, if there is one, so the
        // voices land in front of the effects rather than after them.
        std::string entry = masterId;
        std::unordered_set<std::string> hasGlobalInput;
        for (const auto& c : inst.audio) {
            const NodeSpec* f = inst.findNode(c.from);
            if (f && f->scope == Scope::Global) hasGlobalInput.insert(c.to);
        }
        for (const auto& n : inst.nodes) {
            const auto* man = manifestOf(n);
            if (n.scope != Scope::Global || n.id == masterId) continue;
            if (man == nullptr || man->audioIns == 0) continue;
            if (hasGlobalInput.count(n.id) == 0) { entry = n.id; break; }
        }

        // Voice sinks = voice nodes producing audio that nothing else consumes.
        std::unordered_set<std::string> consumed;
        for (const auto& c : inst.audio) {
            const NodeSpec* t = inst.findNode(c.to);
            if (t && t->scope == Scope::Voice) consumed.insert(c.from);
        }

        int connected = 0;
        for (const auto& n : inst.nodes) {
            if (n.scope != Scope::Voice) continue;
            const auto* man = manifestOf(n);
            if (man == nullptr || man->audioOuts == 0) continue;
            if (consumed.count(n.id)) continue;
            inst.audio.push_back({n.id, entry, 1.0f});
            ++connected;
        }
        if (connected > 0)
            report.fixed("audio", "Connected the end of the voice chain to the output; "
                                  "nothing was routed there.");
    }
}

// --- step 5: amplitude gate ------------------------------------------------

void ensureAmplitudeGate(Instrument& inst, IrReport& report) {
    bool hasHold = false;
    for (const auto& n : inst.nodes) {
        if (n.scope != Scope::Voice) continue;
        const auto* man = manifestOf(n);
        if (man != nullptr && man->holdsVoice) { hasHold = true; break; }
    }
    if (hasHold) return;

    bool hasVoiceAudio = false;
    for (const auto& n : inst.nodes) {
        const auto* man = manifestOf(n);
        if (n.scope == Scope::Voice && man && man->audioOuts > 0) { hasVoiceAudio = true; break; }
    }
    if (!hasVoiceAudio) return;   // nothing to gate; validator will error separately

    std::unordered_set<std::string> taken;
    for (const auto& n : inst.nodes) taken.insert(n.id);

    NodeSpec env;
    env.id    = uniqueId("amp_env", taken); taken.insert(env.id);
    env.type  = "env.adsr";
    env.scope = Scope::Voice;
    env.settings = nlohmann::json::object();

    NodeSpec vca;
    vca.id    = uniqueId("amp_vca", taken); taken.insert(vca.id);
    vca.type  = "vca";
    vca.scope = Scope::Voice;
    vca.settings = nlohmann::json::object();

    // Everything that used to leave the voice section now goes through the VCA.
    std::vector<AudioConn> rerouted;
    for (auto& c : inst.audio) {
        const NodeSpec* f = inst.findNode(c.from);
        const NodeSpec* t = inst.findNode(c.to);
        if (f && t && f->scope == Scope::Voice && t->scope == Scope::Global) {
            rerouted.push_back({c.from, vca.id, c.gain});
            c.from = vca.id;
            c.gain = 1.0f;
        }
    }
    for (const auto& c : rerouted) inst.audio.push_back(c);

    inst.mod.push_back({env.id, vca.id, "gain", 1.0f, Taper::Linear, false});

    inst.nodes.push_back(std::move(env));
    inst.nodes.push_back(std::move(vca));

    // Give the user knobs for it, if there is room.
    const std::string envId = inst.nodes[inst.nodes.size() - 2].id;
    struct { const char* id; const char* label; const char* param; const char* unit;
             float mn, mx, def; Taper taper; } ampParams[] = {
        {"amp_attack",  "Attack",  "attack_ms",  "ms", 0.2f,  8000.0f,   5.0f, Taper::Log},
        {"amp_decay",   "Decay",   "decay_ms",   "ms", 0.5f,  8000.0f, 300.0f, Taper::Log},
        {"amp_sustain", "Sustain", "sustain",    "",   0.0f,     1.0f,   0.7f, Taper::Linear},
        {"amp_release", "Release", "release_ms", "ms", 0.5f, 12000.0f, 400.0f, Taper::Log},
    };
    for (const auto& ap : ampParams) {
        if (static_cast<int>(inst.params.size()) >= kMaxExposedParams) break;
        if (inst.findParam(ap.id) != nullptr) continue;
        ParamSpec ps;
        ps.id = ap.id; ps.label = ap.label; ps.unit = ap.unit;
        ps.min = ap.mn; ps.max = ap.mx; ps.def = ap.def; ps.taper = ap.taper;
        ps.control = "knob"; ps.group = "Amp";
        ps.bind.push_back({envId, ap.param, 1.0f, 0.0f});
        inst.params.push_back(std::move(ps));
    }

    report.fixed("nodes", "Added an amplitude envelope and VCA. Without them a note would "
                          "start and never stop.");
}

// --- step 6: prune ---------------------------------------------------------

void pruneUnreachable(Instrument& inst, IrReport& report) {
    const NodeSpec* master = nullptr;
    for (const auto& n : inst.nodes) if (n.type == "out.master") { master = &n; break; }
    if (master == nullptr) return;

    std::unordered_map<std::string, std::vector<std::string>> reverse;
    for (const auto& c : inst.audio) reverse[c.to].push_back(c.from);

    std::unordered_set<std::string> keep;
    std::vector<std::string> stack{master->id};
    keep.insert(master->id);
    while (!stack.empty()) {
        const std::string id = stack.back();
        stack.pop_back();
        auto it = reverse.find(id);
        if (it == reverse.end()) continue;
        for (const auto& up : it->second)
            if (keep.insert(up).second) stack.push_back(up);
    }

    // A modulation source is worth keeping if it drives something we kept, and
    // that can chain (an LFO into a util.math into a filter).
    for (int guard = 0; guard < 16; ++guard) {
        bool changed = false;
        for (const auto& r : inst.mod)
            if (keep.count(r.targetNode) && keep.insert(r.source).second) changed = true;
        if (!changed) break;
    }

    std::vector<std::string> removed;
    for (size_t i = 0; i < inst.nodes.size();) {
        if (keep.count(inst.nodes[i].id) == 0) {
            removed.push_back(inst.nodes[i].id);
            inst.nodes.erase(inst.nodes.begin() + static_cast<long>(i));
        } else {
            ++i;
        }
    }
    if (removed.empty()) return;

    std::unordered_set<std::string> gone(removed.begin(), removed.end());
    inst.audio.erase(std::remove_if(inst.audio.begin(), inst.audio.end(),
                                    [&](const AudioConn& c) {
                                        return gone.count(c.from) || gone.count(c.to);
                                    }), inst.audio.end());
    inst.mod.erase(std::remove_if(inst.mod.begin(), inst.mod.end(),
                                  [&](const ModRoute& r) {
                                      return gone.count(r.source) || gone.count(r.targetNode);
                                  }), inst.mod.end());
    for (auto& p : inst.params)
        p.bind.erase(std::remove_if(p.bind.begin(), p.bind.end(),
                                    [&](const ParamBind& b) { return gone.count(b.node) > 0; }),
                     p.bind.end());

    std::string list;
    for (size_t i = 0; i < removed.size() && i < 6; ++i) list += (i ? ", " : "") + removed[i];
    if (removed.size() > 6) list += ", ...";
    report.fixed("nodes", "Removed " + std::to_string(removed.size())
                          + " node(s) whose audio never reached the output: " + list + ".");
}

// --- step 7: caps and tidy-up ---------------------------------------------

void enforceCapsAndTidy(Instrument& inst, IrReport& report) {
    // Parameters that now control nothing.
    const size_t paramsBefore = inst.params.size();
    inst.params.erase(std::remove_if(inst.params.begin(), inst.params.end(),
                                     [](const ParamSpec& p) { return p.bind.empty(); }),
                      inst.params.end());
    if (inst.params.size() != paramsBefore)
        report.fixed("params", "Removed " + std::to_string(paramsBefore - inst.params.size())
                               + " control(s) that were not connected to anything.");

    if (static_cast<int>(inst.params.size()) > kMaxExposedParams) {
        inst.params.resize(static_cast<size_t>(kMaxExposedParams));
        report.fixed("params", "Kept the first " + std::to_string(kMaxExposedParams)
                               + " controls and dropped the rest.");
    }

    // Macro routes to parameters that no longer exist.
    for (auto& m : inst.macros)
        m.routes.erase(std::remove_if(m.routes.begin(), m.routes.end(),
                                      [&](const MacroRoute& r) {
                                          return inst.findParam(r.param) == nullptr;
                                      }), m.routes.end());
    const size_t macrosBefore = inst.macros.size();
    inst.macros.erase(std::remove_if(inst.macros.begin(), inst.macros.end(),
                                     [](const MacroSpec& m) { return m.routes.empty(); }),
                      inst.macros.end());
    if (inst.macros.size() != macrosBefore)
        report.fixed("macros", "Removed macro(s) that no longer controlled anything.");

    if (static_cast<int>(inst.macros.size()) > kMaxMacros) {
        inst.macros.resize(static_cast<size_t>(kMaxMacros));
        report.fixed("macros", "Kept the first " + std::to_string(kMaxMacros) + " macros.");
    }

    // UI references.
    for (auto& s : inst.ui.sections)
        s.params.erase(std::remove_if(s.params.begin(), s.params.end(),
                                      [&](const std::string& id) {
                                          return inst.findParam(id) == nullptr;
                                      }), s.params.end());
    inst.ui.sections.erase(std::remove_if(inst.ui.sections.begin(), inst.ui.sections.end(),
                                          [](const UiSection& s) { return s.params.empty(); }),
                           inst.ui.sections.end());
    inst.ui.macroRow.erase(std::remove_if(inst.ui.macroRow.begin(), inst.ui.macroRow.end(),
        [&](const std::string& id) {
            for (const auto& m : inst.macros) if (m.id == id) return false;
            return true;
        }), inst.ui.macroRow.end());

    // Anything the model did not lay out still needs to be reachable, so fall
    // back to grouping by the parameter's own 'group' field.
    if (inst.ui.sections.empty() && !inst.params.empty()) {
        std::vector<std::string> groupOrder;
        for (const auto& p : inst.params)
            if (std::find(groupOrder.begin(), groupOrder.end(), p.group) == groupOrder.end())
                groupOrder.push_back(p.group);
        for (const auto& g : groupOrder) {
            UiSection s;
            s.title = g;
            for (const auto& p : inst.params) if (p.group == g) s.params.push_back(p.id);
            inst.ui.sections.push_back(std::move(s));
        }
        report.fixed("ui", "Built a control layout from the parameter groups - none was provided.");
    } else {
        std::unordered_set<std::string> laidOut;
        for (const auto& s : inst.ui.sections)
            for (const auto& p : s.params) laidOut.insert(p);
        std::vector<std::string> orphans;
        for (const auto& p : inst.params) if (!laidOut.count(p.id)) orphans.push_back(p.id);
        if (!orphans.empty()) {
            UiSection s;
            s.title = "More";
            s.params = orphans;
            inst.ui.sections.push_back(std::move(s));
        }
    }

    if (inst.ui.macroRow.empty())
        for (const auto& m : inst.macros) inst.ui.macroRow.push_back(m.id);
}

} // namespace

// ---------------------------------------------------------------------------

void repair(Instrument& inst, IrReport& report, const RepairOptions& opts) {
    normaliseIdentifiers(inst, report);
    dropUnknownNodes(inst, report);
    dropDanglingReferences(inst, report);
    breakCycles(inst, report);
    ensureOutputPath(inst, report);
    if (opts.insertGateIfMissing) ensureAmplitudeGate(inst, report);
    if (opts.pruneUnreachable)    pruneUnreachable(inst, report);
    if (opts.enforceCaps)         enforceCapsAndTidy(inst, report);

    if (inst.name.empty()) inst.name = "Untitled";
    if (inst.seed == 0)    inst.seed = 0x5EEDF00Dull;
}

} // namespace forge::ir
