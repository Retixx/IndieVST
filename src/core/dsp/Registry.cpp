#include "core/dsp/Registry.h"

#include <algorithm>
#include <sstream>

namespace forge {

// --- Common.h enum helpers -------------------------------------------------

const char* toString(Scope s) noexcept { return s == Scope::Voice ? "voice" : "global"; }

const char* toString(Taper t) noexcept {
    switch (t) {
        case Taper::Log: return "log";
        case Taper::Exp: return "exp";
        default:         return "linear";
    }
}

bool parseScope(const std::string& s, Scope& out) noexcept {
    if (s == "voice")  { out = Scope::Voice;  return true; }
    if (s == "global") { out = Scope::Global; return true; }
    return false;
}

bool parseTaper(const std::string& s, Taper& out) noexcept {
    if (s == "linear") { out = Taper::Linear; return true; }
    if (s == "log")    { out = Taper::Log;    return true; }
    if (s == "exp")    { out = Taper::Exp;    return true; }
    return false;
}

// --- Registry --------------------------------------------------------------

Registry::Registry() {
    modules_.reserve(64);
    registerBuiltinModules(modules_);
    registerProductionModules(modules_);

    // Group by category so both the prompt manifest and the printed docs read
    // as a coherent library rather than as two batches bolted together.
    static const auto rank = [](const std::string& category) {
        if (category == "source")    return 0;
        if (category == "modulator") return 1;
        if (category == "processor") return 2;
        if (category == "effect")    return 3;
        if (category == "utility")   return 4;
        return 5;   // output last
    };
    std::stable_sort(modules_.begin(), modules_.end(),
                     [](const ModuleManifest& a, const ModuleManifest& b) {
                         if (rank(a.category) != rank(b.category))
                             return rank(a.category) < rank(b.category);
                         return a.type < b.type;
                     });

    for (size_t i = 0; i < modules_.size(); ++i)
        index_[modules_[i].type] = i;
}

const Registry& Registry::instance() {
    // Magic-static: constructed once, thread-safe, and immutable afterwards,
    // which is what makes it legal to read from the audio thread.
    static const Registry reg;
    return reg;
}

const ModuleManifest* Registry::find(const std::string& type) const noexcept {
    auto it = index_.find(type);
    return it == index_.end() ? nullptr : &modules_[it->second];
}

std::unique_ptr<Module> Registry::create(const std::string& type) const {
    const auto* m = find(type);
    return (m && m->factory) ? m->factory() : nullptr;
}

namespace {

const char* settingTypeName(SettingDesc::Type t) {
    switch (t) {
        case SettingDesc::Type::Enum:           return "enum";
        case SettingDesc::Type::Float:          return "float";
        case SettingDesc::Type::Int:            return "int";
        case SettingDesc::Type::Bool:           return "bool";
        case SettingDesc::Type::AssetWavetable: return "asset:wavetable";
        case SettingDesc::Type::AssetCurve:     return "asset:curve";
        case SettingDesc::Type::AssetEnvelope:  return "asset:envelope";
    }
    return "float";
}

/// Trims trailing zeros so the manifest does not waste prompt tokens on
/// "0.80000000000000004".
nlohmann::json num(float v) {
    const double d = static_cast<double>(v);
    if (std::abs(d - std::round(d)) < 1e-6 && std::abs(d) < 1e9)
        return static_cast<long long>(std::llround(d));
    return std::round(d * 1000.0) / 1000.0;
}

} // namespace

nlohmann::json Registry::capabilityJson() const {
    using nlohmann::json;
    json modules = json::array();

    for (const auto& m : modules_) {
        json j;
        j["type"]     = m.type;
        j["category"] = m.category;
        j["summary"]  = m.summary;

        json scopes = json::array();
        if (m.allowVoice)  scopes.push_back("voice");
        if (m.allowGlobal) scopes.push_back("global");
        j["scope"] = scopes;

        j["audio_in"]  = m.audioIns;
        j["audio_out"] = m.audioOuts;
        if (m.isModSource) j["mod_source"] = true;
        if (m.holdsVoice)  j["holds_voice"] = true;

        if (!m.settings.empty()) {
            json settings = json::array();
            for (const auto& s : m.settings) {
                json sj;
                sj["id"]   = s.id;
                sj["type"] = settingTypeName(s.type);
                if (s.type == SettingDesc::Type::Enum) sj["options"] = s.options;
                if (s.type == SettingDesc::Type::Float || s.type == SettingDesc::Type::Int) {
                    sj["min"] = num(s.min);
                    sj["max"] = num(s.max);
                }
                if (!s.def.is_null()) sj["default"] = s.def;
                if (!s.help.empty())  sj["help"] = s.help;
                settings.push_back(std::move(sj));
            }
            j["settings"] = std::move(settings);
        }

        if (!m.params.empty()) {
            json params = json::array();
            for (const auto& p : m.params) {
                json pj;
                pj["id"]      = p.id;
                pj["min"]     = num(p.min);
                pj["max"]     = num(p.max);
                pj["default"] = num(p.def);
                pj["taper"]   = toString(p.taper);
                if (!p.unit.empty())  pj["unit"] = p.unit;
                if (!p.modulatable)   pj["modulatable"] = false;
                if (!p.help.empty())  pj["help"] = p.help;
                params.push_back(std::move(pj));
            }
            j["params"] = std::move(params);
        }

        modules.push_back(std::move(j));
    }

    json root;
    root["modules"] = std::move(modules);
    root["limits"] = {
        {"max_nodes",            kMaxNodes},
        {"max_params",           kMaxExposedParams},
        {"max_macros",           kMaxMacros},
        {"max_audio_connections", kMaxAudioConnections},
        {"max_mod_routes",       kMaxModRoutes},
        {"max_assets",           kMaxAssets},
    };
    return root;
}

std::string Registry::capabilityMarkdown() const {
    std::ostringstream os;
    os << "# Forge DSP primitives (" << modules_.size() << ")\n\n";
    std::string lastCategory;
    for (const auto& m : modules_) {
        if (m.category != lastCategory) {
            os << "\n## " << m.category << "\n\n";
            lastCategory = m.category;
        }
        os << "### `" << m.type << "`  ";
        os << "[scope: " << (m.allowVoice ? "voice" : "");
        if (m.allowVoice && m.allowGlobal) os << "+";
        os << (m.allowGlobal ? "global" : "") << "]";
        os << "  in:" << m.audioIns << " out:" << m.audioOuts;
        if (m.isModSource) os << " mod-source";
        os << "\n" << m.summary << "\n";
        if (!m.settings.empty()) {
            os << "- settings: ";
            for (size_t i = 0; i < m.settings.size(); ++i)
                os << (i ? ", " : "") << m.settings[i].id
                   << " (" << settingTypeName(m.settings[i].type) << ")";
            os << "\n";
        }
        if (!m.params.empty()) {
            os << "- params: ";
            for (size_t i = 0; i < m.params.size(); ++i)
                os << (i ? ", " : "") << m.params[i].id
                   << " [" << m.params[i].min << ".." << m.params[i].max << "]";
            os << "\n";
        }
    }
    return os.str();
}

} // namespace forge
