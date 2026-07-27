#include "core/ir/IrValidator.h"

#include "core/dsp/Registry.h"

#include <set>
#include <unordered_map>
#include <unordered_set>

namespace forge::ir {

bool isLatencyNode(const std::string& type) {
    return type == "fx.delay" || type == "filter.comb";
}

namespace {

std::string at(const char* field, size_t i) {
    return std::string(field) + "[" + std::to_string(i) + "]";
}

/// Checks one settings object against the module manifest.
void validateSettings(const NodeSpec& node, const ModuleManifest& man,
                      const Instrument& inst, const std::string& path, IrReport& report) {
    if (!node.settings.is_object()) {
        if (!node.settings.is_null())
            report.error(path + ".settings", "'settings' must be an object.");
        return;
    }

    for (auto it = node.settings.begin(); it != node.settings.end(); ++it) {
        const std::string key = it.key();
        const SettingDesc* desc = man.findSetting(key);
        if (desc == nullptr) {
            report.warn(path + ".settings." + key,
                        "Unknown setting for module type '" + node.type + "'; it will be ignored.");
            continue;
        }
        const auto& v = it.value();
        switch (desc->type) {
            case SettingDesc::Type::Enum: {
                if (!v.is_string()) {
                    report.error(path + ".settings." + key, "Expected a string.");
                    break;
                }
                const std::string s = v.get<std::string>();
                bool found = false;
                for (const auto& o : desc->options) if (o == s) { found = true; break; }
                if (!found) {
                    std::string opts;
                    for (size_t i = 0; i < desc->options.size(); ++i)
                        opts += (i ? ", " : "") + desc->options[i];
                    report.error(path + ".settings." + key,
                                 "'" + s + "' is not a valid option. Allowed: " + opts + ".");
                }
                break;
            }
            case SettingDesc::Type::Float:
            case SettingDesc::Type::Int: {
                if (!v.is_number()) { report.error(path + ".settings." + key, "Expected a number."); break; }
                const double d = v.get<double>();
                if (!(d == d)) { report.error(path + ".settings." + key, "Value is NaN."); break; }
                if (d < desc->min - 1e-6 || d > desc->max + 1e-6)
                    report.error(path + ".settings." + key,
                                 "Value " + std::to_string(d) + " is outside the allowed range "
                                 + std::to_string(desc->min) + ".." + std::to_string(desc->max) + ".");
                break;
            }
            case SettingDesc::Type::Bool:
                if (!v.is_boolean()) report.error(path + ".settings." + key, "Expected true or false.");
                break;
            case SettingDesc::Type::AssetWavetable:
            case SettingDesc::Type::AssetCurve:
            case SettingDesc::Type::AssetEnvelope: {
                if (!v.is_string()) { report.error(path + ".settings." + key, "Expected an asset id string."); break; }
                const std::string id = v.get<std::string>();
                if (id.empty()) break;   // optional asset reference left blank
                const char* wanted = (desc->type == SettingDesc::Type::AssetWavetable) ? "wavetable"
                                   : (desc->type == SettingDesc::Type::AssetCurve)     ? "curve"
                                                                                       : "envelope";
                const AssetSpec* found = nullptr;
                for (const auto& a : inst.assets) if (a.id == id) { found = &a; break; }
                if (found == nullptr)
                    report.error(path + ".settings." + key,
                                 "References asset '" + id + "' which is not declared in 'assets'.");
                else if (found->kind != wanted)
                    report.error(path + ".settings." + key,
                                 "Asset '" + id + "' has kind '" + found->kind
                                 + "' but this setting requires kind '" + wanted + "'.");
                break;
            }
        }
    }

    // Required asset references must actually be present.
    for (const auto& s : man.settings) {
        const bool isAsset = s.type == SettingDesc::Type::AssetWavetable
                          || s.type == SettingDesc::Type::AssetCurve
                          || s.type == SettingDesc::Type::AssetEnvelope;
        if (!isAsset) continue;
        const bool required = (node.type == "osc.wavetable" && s.id == "table")
                           || (node.type == "env.multi"     && s.id == "shape");
        if (!required) continue;
        auto it = node.settings.find(s.id);
        if (it == node.settings.end() || !it->is_string() || it->get<std::string>().empty())
            report.error(path + ".settings." + s.id,
                         "Module type '" + node.type + "' requires a '" + s.id + "' asset reference.");
    }
}

} // namespace

// ---------------------------------------------------------------------------

bool topoSort(const Instrument& inst, Scope scope, std::vector<int>& orderOut,
              std::vector<std::pair<int, int>>* cycleEdgesOut) {
    orderOut.clear();

    std::unordered_map<std::string, int> idToIndex;
    for (size_t i = 0; i < inst.nodes.size(); ++i)
        idToIndex[inst.nodes[i].id] = static_cast<int>(i);

    std::vector<int> members;
    std::unordered_map<int, int> local;   // global index -> local index
    for (size_t i = 0; i < inst.nodes.size(); ++i) {
        if (inst.nodes[i].scope == scope) {
            local[static_cast<int>(i)] = static_cast<int>(members.size());
            members.push_back(static_cast<int>(i));
        }
    }
    if (members.empty()) return true;

    const size_t n = members.size();
    std::vector<std::vector<int>> adj(n);
    std::vector<int> indegree(n, 0);
    std::vector<std::pair<int, int>> keptEdges;

    for (const auto& c : inst.audio) {
        auto fi = idToIndex.find(c.from);
        auto ti = idToIndex.find(c.to);
        if (fi == idToIndex.end() || ti == idToIndex.end()) continue;
        if (inst.nodes[static_cast<size_t>(fi->second)].scope != scope) continue;
        if (inst.nodes[static_cast<size_t>(ti->second)].scope != scope) continue;

        // An edge INTO a latency node does not constrain ordering: that node
        // reads the previous control block, which is precisely what makes
        // delay and comb feedback both legal and schedulable.
        if (isLatencyNode(inst.nodes[static_cast<size_t>(ti->second)].type)) continue;

        const int lf = local[fi->second];
        const int lt = local[ti->second];
        if (lf == lt) continue;              // self-loop on a non-latency node: caught by validate()
        adj[static_cast<size_t>(lf)].push_back(lt);
        ++indegree[static_cast<size_t>(lt)];
        keptEdges.emplace_back(lf, lt);
    }

    std::vector<int> queue;
    for (size_t i = 0; i < n; ++i) if (indegree[i] == 0) queue.push_back(static_cast<int>(i));

    size_t head = 0;
    while (head < queue.size()) {
        const int u = queue[head++];
        orderOut.push_back(members[static_cast<size_t>(u)]);
        for (int v : adj[static_cast<size_t>(u)])
            if (--indegree[static_cast<size_t>(v)] == 0) queue.push_back(v);
    }

    if (orderOut.size() != n) {
        if (cycleEdgesOut != nullptr) {
            cycleEdgesOut->clear();
            for (const auto& e : keptEdges)
                if (indegree[static_cast<size_t>(e.second)] > 0)
                    cycleEdgesOut->emplace_back(members[static_cast<size_t>(e.first)],
                                                members[static_cast<size_t>(e.second)]);
        }
        orderOut.clear();
        return false;
    }
    return true;
}

// ---------------------------------------------------------------------------

bool validate(const Instrument& inst, IrReport& report, ValidationResult* result) {
    const Registry& reg = Registry::instance();
    ValidationResult local;
    ValidationResult& res = (result != nullptr) ? *result : local;
    res = ValidationResult{};

    // --- version ---
    if (inst.irVersion != kCurrentIrVersion)
        report.warn("ir_version", "Specification declares ir_version '" + inst.irVersion
                                  + "'; this build understands '" + kCurrentIrVersion + "'.");

    // --- counts ---
    if (inst.nodes.empty())
        report.error("nodes", "An instrument needs at least one node.");
    if (static_cast<int>(inst.nodes.size()) > kMaxNodes)
        report.error("nodes", "Too many nodes (" + std::to_string(inst.nodes.size())
                              + "), the limit is " + std::to_string(kMaxNodes) + ".");
    if (static_cast<int>(inst.params.size()) > kMaxExposedParams)
        report.error("params", "Too many exposed parameters, the limit is "
                               + std::to_string(kMaxExposedParams) + ".");
    if (static_cast<int>(inst.macros.size()) > kMaxMacros)
        report.error("macros", "Too many macros, the limit is " + std::to_string(kMaxMacros) + ".");
    if (static_cast<int>(inst.audio.size()) > kMaxAudioConnections)
        report.error("audio", "Too many audio connections, the limit is "
                              + std::to_string(kMaxAudioConnections) + ".");
    if (static_cast<int>(inst.mod.size()) > kMaxModRoutes)
        report.error("mod", "Too many modulation routes, the limit is "
                            + std::to_string(kMaxModRoutes) + ".");
    if (static_cast<int>(inst.assets.size()) > kMaxAssets)
        report.error("assets", "Too many assets, the limit is " + std::to_string(kMaxAssets) + ".");

    // --- assets ---
    std::unordered_set<std::string> assetIds;
    for (size_t i = 0; i < inst.assets.size(); ++i) {
        const auto& a = inst.assets[i];
        const std::string path = at("assets", i);
        if (!isValidIdentifier(a.id))
            report.error(path + ".id", "Asset id '" + a.id
                                       + "' must match ^[a-z][a-z0-9_]{0,31}$.");
        else if (!assetIds.insert(a.id).second)
            report.error(path + ".id", "Duplicate asset id '" + a.id + "'.");

        if (a.kind != "wavetable" && a.kind != "curve" && a.kind != "envelope")
            report.error(path + ".kind",
                         "Asset kind must be 'wavetable', 'curve' or 'envelope'; got '" + a.kind + "'.");
        if (!a.spec.is_object())
            report.error(path + ".spec", "Asset 'spec' must be an object.");
    }

    // --- nodes ---
    std::unordered_set<std::string> nodeIds;
    int voiceNodeCount = 0, oscCount = 0, filterCount = 0, reverbCount = 0, delayCount = 0;
    bool hasVoiceHold = false;

    for (size_t i = 0; i < inst.nodes.size(); ++i) {
        const auto& n = inst.nodes[i];
        const std::string path = at("nodes", i);

        if (!isValidIdentifier(n.id))
            report.error(path + ".id", "Node id '" + n.id + "' must match ^[a-z][a-z0-9_]{0,31}$.");
        else if (!nodeIds.insert(n.id).second)
            report.error(path + ".id", "Duplicate node id '" + n.id + "'.");

        const ModuleManifest* man = reg.find(n.type);
        if (man == nullptr) {
            report.error(path + ".type", "Unknown module type '" + n.type
                                         + "'. Only types listed in the capability manifest exist.");
            continue;
        }

        if (n.scope == Scope::Voice && !man->allowVoice)
            report.error(path + ".scope", "Module type '" + n.type + "' cannot run in voice scope.");
        if (n.scope == Scope::Global && !man->allowGlobal)
            report.error(path + ".scope", "Module type '" + n.type + "' cannot run in global scope.");

        validateSettings(n, *man, inst, path, report);

        if (n.scope == Scope::Voice) ++voiceNodeCount;
        if (man->holdsVoice && n.scope == Scope::Voice) hasVoiceHold = true;
        if (man->category == "source" && n.scope == Scope::Voice) ++oscCount;
        if (n.type.rfind("filter.", 0) == 0) ++filterCount;
        if (n.type == "fx.reverb") ++reverbCount;
        if (n.type == "fx.delay")  ++delayCount;

        if (n.type == "out.master") {
            if (res.masterIndex >= 0)
                report.error(path, "More than one out.master node; there must be exactly one.");
            res.masterIndex = static_cast<int>(i);
        }
    }

    // MVP complexity caps (SPEC 5.2).
    if (oscCount > 6)    report.error("nodes", "At most 6 voice-scope source modules are allowed.");
    if (filterCount > 4) report.error("nodes", "At most 4 filter modules are allowed.");
    if (reverbCount > 2) report.error("nodes", "At most 2 fx.reverb modules are allowed.");
    if (delayCount > 3)  report.error("nodes", "At most 3 fx.delay modules are allowed.");

    if (voiceNodeCount == 0)
        report.error("nodes", "No voice-scope nodes: this instrument could never make a sound "
                              "in response to a note.");
    if (!hasVoiceHold)
        report.warn("nodes", "No envelope in voice scope. A gate envelope will be inserted "
                             "automatically so notes can end.");

    // --- audio connections ---
    std::set<std::pair<std::string, std::string>> seenEdges;
    for (size_t i = 0; i < inst.audio.size(); ++i) {
        const auto& c = inst.audio[i];
        const std::string path = at("audio", i);

        const NodeSpec* from = inst.findNode(c.from);
        const NodeSpec* to   = inst.findNode(c.to);
        if (from == nullptr) { report.error(path + ".from", "Unknown node id '" + c.from + "'."); continue; }
        if (to   == nullptr) { report.error(path + ".to",   "Unknown node id '" + c.to   + "'."); continue; }

        const ModuleManifest* fm = reg.find(from->type);
        const ModuleManifest* tm = reg.find(to->type);
        if (fm == nullptr || tm == nullptr) continue;

        if (fm->audioOuts == 0)
            report.error(path + ".from", "Module '" + c.from + "' (" + from->type
                                         + ") has no audio output. It is a modulation source - "
                                           "connect it in the 'mod' array instead.");
        if (tm->audioIns == 0)
            report.error(path + ".to", "Module '" + c.to + "' (" + to->type
                                       + ") has no audio input.");

        if (from->scope == Scope::Global && to->scope == Scope::Voice)
            report.error(path, "A global-scope node cannot feed a voice-scope node. "
                               "Signal flows voice -> global only.");

        if (c.from == c.to && !isLatencyNode(to->type))
            report.error(path, "Node '" + c.from + "' feeds itself without a delay in the loop.");

        if (!seenEdges.insert({c.from, c.to}).second)
            report.warn(path, "Duplicate connection " + c.from + " -> " + c.to + ".");

        if (!isFinite(c.gain) || std::abs(c.gain) > 4.0f)
            report.error(path + ".gain", "Connection gain must be finite and within -4..4.");
    }

    // --- modulation routes ---
    for (size_t i = 0; i < inst.mod.size(); ++i) {
        const auto& r = inst.mod[i];
        const std::string path = at("mod", i);

        const NodeSpec* src = inst.findNode(r.source);
        if (src == nullptr) {
            report.error(path + ".source", "Unknown modulation source node '" + r.source + "'.");
            continue;
        }
        const ModuleManifest* sm = reg.find(src->type);
        if (sm == nullptr) continue;
        if (!sm->isModSource)
            report.error(path + ".source", "Module '" + r.source + "' (" + src->type
                                           + ") is not a modulation source.");

        const NodeSpec* tgt = inst.findNode(r.targetNode);
        if (tgt == nullptr) {
            report.error(path + ".target", "Unknown target node '" + r.targetNode + "'.");
            continue;
        }
        const ModuleManifest* tm = reg.find(tgt->type);
        if (tm == nullptr) continue;
        const int pi = tm->indexOfParam(r.targetParam);
        if (pi < 0) {
            report.error(path + ".target", "Module type '" + tgt->type + "' has no parameter '"
                                           + r.targetParam + "'.");
            continue;
        }
        if (!tm->params[static_cast<size_t>(pi)].modulatable)
            report.error(path + ".target", "Parameter '" + r.targetParam + "' is not modulatable.");

        // A voice-scope source cannot drive a global-scope target: there are N
        // voices and only one global node, so the value would be ambiguous.
        if (src->scope == Scope::Voice && tgt->scope == Scope::Global)
            report.error(path, "A voice-scope modulator cannot drive a global-scope parameter "
                               "(there is one global node but many voices). Move the modulator "
                               "to global scope, or the target into voice scope.");

        if (!isFinite(r.depth) || std::abs(r.depth) > 1.0f)
            report.error(path + ".depth", "Modulation depth must be finite and within -1..1.");
    }

    // --- exposed parameters ---
    std::unordered_set<std::string> paramIds;
    for (size_t i = 0; i < inst.params.size(); ++i) {
        const auto& p = inst.params[i];
        const std::string path = at("params", i);

        if (!isValidIdentifier(p.id))
            report.error(path + ".id", "Parameter id '" + p.id + "' must match ^[a-z][a-z0-9_]{0,31}$.");
        else if (!paramIds.insert(p.id).second)
            report.error(path + ".id", "Duplicate parameter id '" + p.id + "'.");

        if (!isFinite(p.min) || !isFinite(p.max) || !isFinite(p.def))
            report.error(path, "min, max and default must all be finite numbers.");
        else if (p.max <= p.min)
            report.error(path, "max must be greater than min.");
        else if (p.def < p.min - 1e-4f || p.def > p.max + 1e-4f)
            report.error(path + ".default", "Default is outside min..max.");

        if (p.taper == Taper::Log && p.min <= 0.0f)
            report.error(path + ".taper", "A logarithmic taper requires min > 0.");

        if (p.control != "knob" && p.control != "slider" && p.control != "switch" && p.control != "combo")
            report.warn(path + ".control", "Unknown control type '" + p.control + "', a knob will be used.");

        if (p.bind.empty())
            report.error(path + ".bind", "Parameter '" + p.id + "' controls nothing. "
                                         "Bind it to at least one module parameter.");

        for (size_t b = 0; b < p.bind.size(); ++b) {
            const auto& bind = p.bind[b];
            const std::string bpath = path + ".bind[" + std::to_string(b) + "]";
            const NodeSpec* node = inst.findNode(bind.node);
            if (node == nullptr) {
                report.error(bpath + ".node", "Unknown node id '" + bind.node + "'.");
                continue;
            }
            const ModuleManifest* man = reg.find(node->type);
            if (man == nullptr) continue;
            if (man->indexOfParam(bind.param) < 0)
                report.error(bpath + ".param", "Module type '" + node->type
                                               + "' has no parameter '" + bind.param + "'.");
        }
    }

    // --- macros ---
    std::unordered_set<std::string> macroIds;
    for (size_t i = 0; i < inst.macros.size(); ++i) {
        const auto& m = inst.macros[i];
        const std::string path = at("macros", i);
        if (!isValidIdentifier(m.id))
            report.error(path + ".id", "Macro id '" + m.id + "' must match ^[a-z][a-z0-9_]{0,31}$.");
        else if (!macroIds.insert(m.id).second)
            report.error(path + ".id", "Duplicate macro id '" + m.id + "'.");
        if (m.routes.empty())
            report.warn(path, "Macro '" + m.id + "' has no routes and will do nothing.");
        for (size_t r = 0; r < m.routes.size(); ++r) {
            if (inst.findParam(m.routes[r].param) == nullptr)
                report.error(path + ".routes[" + std::to_string(r) + "].param",
                             "Macro targets exposed parameter '" + m.routes[r].param
                             + "', which does not exist.");
        }
    }

    // --- ui references ---
    for (size_t i = 0; i < inst.ui.sections.size(); ++i)
        for (const auto& pid : inst.ui.sections[i].params)
            if (inst.findParam(pid) == nullptr)
                report.warn(at("ui.sections", i), "References unknown parameter '" + pid
                                                  + "'; it will be skipped.");
    for (const auto& mid : inst.ui.macroRow)
        if (macroIds.find(mid) == macroIds.end())
            report.warn("ui.macro_row", "References unknown macro '" + mid + "'; it will be skipped.");

    // --- voicing ---
    if (inst.voicing != "poly" && inst.voicing != "mono" && inst.voicing != "legato")
        report.warn("voicing", "Unknown voicing '" + inst.voicing + "', 'poly' will be used.");
    if (inst.polyphony < 1 || inst.polyphony > kMaxVoices)
        report.error("polyphony", "Polyphony must be between 1 and " + std::to_string(kMaxVoices) + ".");

    // --- graph legality ---
    std::vector<std::pair<int, int>> cycleEdges;
    if (!topoSort(inst, Scope::Voice, res.voiceOrder, &cycleEdges)) {
        report.error("audio", "The voice-scope audio graph contains a feedback loop that does not "
                              "pass through fx.delay or filter.comb. Feedback needs a delay in it.");
    }
    if (!topoSort(inst, Scope::Global, res.globalOrder, &cycleEdges)) {
        report.error("audio", "The global-scope audio graph contains a feedback loop that does not "
                              "pass through fx.delay or filter.comb.");
    }

    // --- voice -> global handoff ---
    for (const auto& c : inst.audio) {
        const NodeSpec* from = inst.findNode(c.from);
        const NodeSpec* to   = inst.findNode(c.to);
        if (from && to && from->scope == Scope::Voice && to->scope == Scope::Global) {
            const int fi = inst.indexOfNode(c.from);
            if (fi >= 0) res.voiceTerminals.push_back(fi);
        }
    }
    if (res.voiceTerminals.empty() && voiceNodeCount > 0)
        report.warn("audio", "No voice-scope node feeds the global section. The last node in the "
                             "voice chain will be connected to the output automatically.");

    if (res.masterIndex < 0)
        report.warn("nodes", "No out.master node; one will be added automatically.");

    res.ok = !report.hasErrors();
    return res.ok;
}

} // namespace forge::ir
