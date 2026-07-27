#include "core/ir/IrSafety.h"

#include "core/dsp/Registry.h"

#include <map>

namespace forge::ir {

CostEstimate estimateCost(const Instrument& inst) {
    const Registry& reg = Registry::instance();
    CostEstimate e;

    for (const auto& n : inst.nodes) {
        const ModuleManifest* man = reg.find(n.type);
        if (man == nullptr) continue;

        float cost = man->costWeight;
        // Unison multiplies oscillator cost almost linearly.
        if (n.type == "osc.analog") {
            const auto it = n.settings.find("unison");
            if (it != n.settings.end() && it->is_number())
                cost *= clampT(static_cast<float>(it->get<double>()), 1.0f, 7.0f);
        }
        // A 24 dB SVF runs the core twice.
        if (n.type == "filter.svf") {
            const auto it = n.settings.find("slope");
            if (it != n.settings.end() && it->is_string() && it->get<std::string>() == "24")
                cost *= 1.8f;
        }

        if (n.scope == Scope::Voice) e.voiceCost += cost;
        else                         e.globalCost += cost;
    }

    const float voices = static_cast<float>(clampT(inst.polyphony, 1, kMaxVoices));
    e.totalCost   = e.voiceCost * voices + e.globalCost;
    e.cpuFraction = e.totalCost / kCostUnitsPerCpuFraction;
    return e;
}

namespace {

std::string at(const char* field, size_t i) {
    return std::string(field) + "[" + std::to_string(i) + "]";
}

/// Clamps a node's numeric settings into their manifest ranges and replaces
/// unusable enum values with the module default.
void clampSettings(NodeSpec& n, const ModuleManifest& man, const std::string& path, IrReport& report) {
    if (!n.settings.is_object()) { n.settings = nlohmann::json::object(); return; }

    for (const auto& desc : man.settings) {
        auto it = n.settings.find(desc.id);
        if (it == n.settings.end()) continue;

        switch (desc.type) {
            case SettingDesc::Type::Float:
            case SettingDesc::Type::Int: {
                if (!it->is_number()) {
                    if (!desc.def.is_null()) *it = desc.def;
                    report.fixed(path + ".settings." + desc.id, "Non-numeric setting replaced with the default.");
                    break;
                }
                double d = it->get<double>();
                if (!(d == d)) d = static_cast<double>(desc.min);
                const double c = clampT(d, static_cast<double>(desc.min), static_cast<double>(desc.max));
                if (c != d) {
                    report.fixed(path + ".settings." + desc.id,
                                 "Clamped " + desc.id + " into its allowed range.");
                }
                if (desc.type == SettingDesc::Type::Int) *it = static_cast<long long>(std::llround(c));
                else                                     *it = c;
                break;
            }
            case SettingDesc::Type::Enum: {
                bool ok = it->is_string();
                if (ok) {
                    const std::string s = it->get<std::string>();
                    ok = false;
                    for (const auto& o : desc.options) if (o == s) { ok = true; break; }
                }
                if (!ok) {
                    *it = desc.def.is_null() ? nlohmann::json(desc.options.empty() ? "" : desc.options.front())
                                             : desc.def;
                    report.fixed(path + ".settings." + desc.id,
                                 "Replaced an unrecognised option with the module default.");
                }
                break;
            }
            case SettingDesc::Type::Bool:
                if (!it->is_boolean()) {
                    *it = desc.def.is_null() ? nlohmann::json(false) : desc.def;
                    report.fixed(path + ".settings." + desc.id, "Coerced to a boolean default.");
                }
                break;
            default:
                break;
        }
    }
}

} // namespace

bool applySafety(Instrument& inst, IrReport& report, float cpuBudget) {
    const Registry& reg = Registry::instance();

    // --- 1. per-node settings -------------------------------------------------
    for (size_t i = 0; i < inst.nodes.size(); ++i) {
        const ModuleManifest* man = reg.find(inst.nodes[i].type);
        if (man != nullptr) clampSettings(inst.nodes[i], *man, at("nodes", i), report);
    }

    // --- 2. connection gains --------------------------------------------------
    for (size_t i = 0; i < inst.audio.size(); ++i) {
        auto& c = inst.audio[i];
        const float g = clampT(sanitize(c.gain, 1.0f), -4.0f, 4.0f);
        if (g != c.gain) {
            report.fixed(at("audio", i) + ".gain", "Clamped connection gain into -4..4.");
            c.gain = g;
        }
    }

    // Gain staging: if many sources sum into one node the result can be far
    // hotter than any single path. Normalise anything summing past 8x.
    std::map<std::string, float> inboundGain;
    for (const auto& c : inst.audio) inboundGain[c.to] += std::abs(c.gain);
    for (const auto& kv : inboundGain) {
        if (kv.second <= 8.0f) continue;
        const float scale = 8.0f / kv.second;
        for (auto& c : inst.audio) if (c.to == kv.first) c.gain *= scale;
        report.fixed("audio", "Scaled down the gains summing into '" + kv.first
                              + "' - together they were " + std::to_string(kv.second) + "x.");
    }

    // --- 3. modulation depths -------------------------------------------------
    for (size_t i = 0; i < inst.mod.size(); ++i) {
        auto& r = inst.mod[i];
        const float d = clampT(sanitize(r.depth, 0.0f), -1.0f, 1.0f);
        if (d != r.depth) {
            report.fixed(at("mod", i) + ".depth", "Clamped modulation depth into -1..1.");
            r.depth = d;
        }
    }

    // --- 4. exposed parameter ranges -----------------------------------------
    for (size_t i = 0; i < inst.params.size(); ++i) {
        auto& p = inst.params[i];
        const std::string path = at("params", i);

        p.min = sanitize(p.min, 0.0f);
        p.max = sanitize(p.max, 1.0f);
        p.def = sanitize(p.def, p.min);

        if (p.max <= p.min) {
            p.max = p.min + 1.0f;
            report.fixed(path, "Widened a degenerate min/max range.");
        }
        if (p.taper == Taper::Log && p.min <= 0.0f) {
            p.taper = Taper::Exp;
            report.fixed(path + ".taper",
                         "A logarithmic taper needs min > 0; switched to an exponential taper.");
        }
        const float d = clampT(p.def, p.min, p.max);
        if (d != p.def) {
            report.fixed(path + ".default", "Clamped the default into min..max.");
            p.def = d;
        }

        // Keep a bound parameter inside the range the target module accepts.
        // Without this, an exposed "Feedback 0..1.5" could push a delay past
        // its safe ceiling before the runtime clamp ever saw it.
        for (auto& b : p.bind) {
            const NodeSpec* node = inst.findNode(b.node);
            if (node == nullptr) continue;
            const ModuleManifest* man = reg.find(node->type);
            if (man == nullptr) continue;
            const int pi = man->indexOfParam(b.param);
            if (pi < 0) continue;
            const auto& target = man->params[static_cast<size_t>(pi)];

            const float s = clampT(sanitize(b.scale, 1.0f), -4.0f, 4.0f);
            const float o = clampT(sanitize(b.offset, 0.0f), -2.0f, 2.0f);
            if (s != b.scale || o != b.offset) {
                report.fixed(path + ".bind", "Clamped a parameter bind scale/offset.");
                b.scale = s; b.offset = o;
            }

            if (p.min < target.min - 1e-4f || p.max > target.max + 1e-4f) {
                const float nmin = clampT(p.min, target.min, target.max);
                const float nmax = clampT(p.max, target.min, target.max);
                if (nmax > nmin) {
                    report.fixed(path, "Narrowed the range of '" + p.id + "' to what "
                                       + node->type + "." + b.param + " actually accepts.");
                    p.min = nmin; p.max = nmax;
                    p.def = clampT(p.def, p.min, p.max);
                }
            }
        }
    }

    // --- 5. macro depths ------------------------------------------------------
    for (auto& m : inst.macros) {
        m.def = clamp01(sanitize(m.def, 0.0f));
        for (auto& r : m.routes) r.depth = clampT(sanitize(r.depth, 0.0f), -1.0f, 1.0f);
    }

    // --- 6. voicing -----------------------------------------------------------
    inst.polyphony = clampT(inst.polyphony, 1, kMaxVoices);
    inst.glideMs   = clampT(sanitize(inst.glideMs, 0.0f), 0.0f, 5000.0f);
    if (inst.voicing != "poly" && inst.voicing != "mono" && inst.voicing != "legato")
        inst.voicing = "poly";
    if (inst.voicing != "poly" && inst.polyphony > 1) inst.polyphony = 1;

    // --- 7. CPU budget --------------------------------------------------------
    const float budget = clampT(cpuBudget, 0.05f, 0.9f) * kCostUnitsPerCpuFraction;
    CostEstimate est = estimateCost(inst);
    if (est.totalCost <= budget) return true;

    // Reduce rather than reject. A quieter instrument beats no instrument.
    while (est.totalCost > budget && inst.polyphony > 4) {
        inst.polyphony = std::max(4, inst.polyphony / 2);
        report.fixed("polyphony", "Reduced polyphony to " + std::to_string(inst.polyphony)
                                  + " to stay inside the CPU budget.");
        est = estimateCost(inst);
    }

    if (est.totalCost > budget) {
        bool reduced = false;
        for (auto& n : inst.nodes) {
            if (n.type != "osc.analog") continue;
            auto it = n.settings.find("unison");
            if (it != n.settings.end() && it->is_number() && it->get<double>() > 1.0) {
                *it = 1;
                reduced = true;
            }
        }
        if (reduced) {
            report.fixed("nodes", "Dropped oscillator unison to stay inside the CPU budget.");
            est = estimateCost(inst);
        }
    }

    if (est.totalCost > budget) {
        int reverbs = 0;
        for (size_t i = 0; i < inst.nodes.size();) {
            if (inst.nodes[i].type == "fx.reverb" && ++reverbs > 1) {
                const std::string id = inst.nodes[i].id;
                inst.nodes.erase(inst.nodes.begin() + static_cast<long>(i));
                // Bridge the removed node so the chain does not break.
                std::vector<AudioConn> bridged;
                for (const auto& c : inst.audio)
                    if (c.to == id)
                        for (const auto& d : inst.audio)
                            if (d.from == id) bridged.push_back({c.from, d.to, c.gain * d.gain});
                inst.audio.erase(std::remove_if(inst.audio.begin(), inst.audio.end(),
                                                [&](const AudioConn& c) { return c.from == id || c.to == id; }),
                                 inst.audio.end());
                for (auto& b : bridged) inst.audio.push_back(b);
                inst.mod.erase(std::remove_if(inst.mod.begin(), inst.mod.end(),
                                              [&](const ModRoute& r) { return r.source == id || r.targetNode == id; }),
                               inst.mod.end());
                report.fixed("nodes", "Removed a duplicate reverb to stay inside the CPU budget.");
                continue;
            }
            ++i;
        }
        est = estimateCost(inst);
    }

    if (est.totalCost > budget) {
        report.error("nodes", "This instrument needs an estimated "
                              + std::to_string(static_cast<int>(est.cpuFraction * 100.0f))
                              + "% of a CPU core, which is over the budget. Use fewer modules, "
                                "less unison, or a single reverb.");
        return false;
    }
    return true;
}

} // namespace forge::ir
