#include "core/ir/Ir.h"

#include <cctype>
#include <sstream>

namespace forge::ir {

// --- IrReport --------------------------------------------------------------

bool IrReport::hasErrors() const {
    for (const auto& i : issues) if (i.level == IrIssue::Level::Error) return true;
    return false;
}

int IrReport::count(IrIssue::Level level) const {
    int n = 0;
    for (const auto& i : issues) if (i.level == level) ++n;
    return n;
}

std::string IrReport::toModelFeedback() const {
    std::ostringstream os;
    os << "Your instrument specification did not validate. Fix every item below and "
          "return the COMPLETE corrected JSON specification, not a patch.\n\n";
    int n = 0;
    for (const auto& i : issues) {
        if (i.level != IrIssue::Level::Error) continue;
        os << ++n << ". ";
        if (!i.path.empty()) os << "at `" << i.path << "`: ";
        os << i.message << "\n";
        if (n >= 40) { os << "(further errors omitted)\n"; break; }
    }
    return os.str();
}

std::string IrReport::toUserSummary() const {
    std::ostringstream os;
    for (const auto& i : issues) {
        if (i.level == IrIssue::Level::Fixed) {
            os << "- " << i.message;
            if (!i.path.empty()) os << "  (" << i.path << ")";
            os << "\n";
        }
    }
    return os.str();
}

// --- helpers ---------------------------------------------------------------

std::string stripPort(std::string endpoint) {
    // trim
    size_t b = endpoint.find_first_not_of(" \t\r\n");
    size_t e = endpoint.find_last_not_of(" \t\r\n");
    if (b == std::string::npos) return {};
    endpoint = endpoint.substr(b, e - b + 1);

    const size_t colon = endpoint.find(':');
    if (colon != std::string::npos) endpoint = endpoint.substr(0, colon);
    return endpoint;
}

bool splitTarget(const std::string& target, std::string& node, std::string& param) {
    const size_t dot = target.find('.');
    if (dot == std::string::npos || dot == 0 || dot + 1 >= target.size()) return false;
    node  = target.substr(0, dot);
    param = target.substr(dot + 1);
    return true;
}

bool isValidIdentifier(const std::string& id) {
    if (id.empty() || id.size() > 32) return false;
    if (!(id[0] >= 'a' && id[0] <= 'z')) return false;
    for (char c : id) {
        const bool ok = (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_';
        if (!ok) return false;
    }
    return true;
}

std::string sanitiseIdentifier(const std::string& id) {
    std::string out;
    out.reserve(id.size());
    for (char c : id) {
        if (c >= 'A' && c <= 'Z')                                   out += static_cast<char>(c - 'A' + 'a');
        else if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9'))   out += c;
        else if (c == '_' || c == '-' || c == ' ' || c == '.')       out += '_';
        // anything else is dropped
    }
    while (!out.empty() && !(out[0] >= 'a' && out[0] <= 'z')) out.erase(out.begin());
    if (out.empty()) out = "n";
    if (out.size() > 32) out.resize(32);
    return out;
}

namespace {

std::string str(const nlohmann::json& j, const char* key, const std::string& def = {}) {
    auto it = j.find(key);
    if (it != j.end() && it->is_string()) return it->get<std::string>();
    return def;
}
float flt(const nlohmann::json& j, const char* key, float def) {
    auto it = j.find(key);
    if (it != j.end() && it->is_number()) return sanitize(it->get<float>(), def);
    return def;
}
int integer(const nlohmann::json& j, const char* key, int def) {
    auto it = j.find(key);
    if (it != j.end() && it->is_number()) return static_cast<int>(std::lround(it->get<double>()));
    return def;
}
bool boolean(const nlohmann::json& j, const char* key, bool def) {
    auto it = j.find(key);
    if (it != j.end() && it->is_boolean()) return it->get<bool>();
    return def;
}
Taper taper(const nlohmann::json& j, const char* key, Taper def) {
    Taper t = def;
    auto it = j.find(key);
    if (it != j.end() && it->is_string()) parseTaper(it->get<std::string>(), t);
    return t;
}

const nlohmann::json* arrayAt(const nlohmann::json& j, const char* key) {
    auto it = j.find(key);
    return (it != j.end() && it->is_array()) ? &(*it) : nullptr;
}

std::string idx(const char* field, size_t i) {
    return std::string(field) + "[" + std::to_string(i) + "]";
}

} // namespace

// --- Instrument lookups ----------------------------------------------------

const NodeSpec* Instrument::findNode(const std::string& id) const {
    for (const auto& n : nodes) if (n.id == id) return &n;
    return nullptr;
}
NodeSpec* Instrument::findNode(const std::string& id) {
    for (auto& n : nodes) if (n.id == id) return &n;
    return nullptr;
}
const ParamSpec* Instrument::findParam(const std::string& id) const {
    for (const auto& p : params) if (p.id == id) return &p;
    return nullptr;
}
int Instrument::indexOfNode(const std::string& id) const {
    for (size_t i = 0; i < nodes.size(); ++i) if (nodes[i].id == id) return static_cast<int>(i);
    return -1;
}

// --- Safe parse ------------------------------------------------------------

bool parseJsonSafely(const std::string& text, nlohmann::json& out, IrReport& report) {
    if (text.empty()) {
        report.error("", "Empty response.");
        return false;
    }
    if (text.size() > kMaxIrBytes) {
        report.error("", "Response is " + std::to_string(text.size() / 1024)
                         + " KB, over the " + std::to_string(kMaxIrBytes / 1024) + " KB limit.");
        return false;
    }

    // Depth pre-scan. nlohmann parses recursively, so a pathological nesting
    // depth would blow the stack before any of our checks could run.
    int depth = 0, maxDepth = 0;
    bool inString = false, escaped = false;
    for (char c : text) {
        if (inString) {
            if (escaped)          escaped = false;
            else if (c == '\\')   escaped = true;
            else if (c == '"')    inString = false;
            continue;
        }
        if (c == '"')                     inString = true;
        else if (c == '{' || c == '[')  { maxDepth = std::max(maxDepth, ++depth); }
        else if (c == '}' || c == ']')  { --depth; }
    }
    if (maxDepth > kMaxJsonDepth) {
        report.error("", "JSON nesting depth " + std::to_string(maxDepth) + " exceeds the limit.");
        return false;
    }

    out = nlohmann::json::parse(text, nullptr, /*allow_exceptions*/ false, /*ignore_comments*/ true);
    if (out.is_discarded()) {
        report.error("", "Response is not valid JSON.");
        return false;
    }
    if (!out.is_object()) {
        report.error("", "Top level of the specification must be a JSON object.");
        return false;
    }
    return true;
}

// --- Extraction ------------------------------------------------------------

bool fromJson(const nlohmann::json& j, Instrument& out, IrReport& report) {
    out = Instrument{};

    out.irVersion   = str(j, "ir_version", kCurrentIrVersion);
    out.name        = str(j, "name", "Untitled");
    out.description = str(j, "description");
    if (auto it = j.find("meta"); it != j.end() && it->is_object()) out.meta = *it;

    out.voicing   = str(j, "voicing", "poly");
    out.glideMs   = clampT(flt(j, "glide_ms", 0.0f), 0.0f, 5000.0f);
    out.polyphony = clampT(integer(j, "polyphony", kDefaultVoices), 1, kMaxVoices);
    if (auto it = j.find("seed"); it != j.end() && it->is_number_unsigned())
        out.seed = it->get<uint64_t>();

    if (out.name.size() > 64) out.name.resize(64);
    if (out.description.size() > 400) out.description.resize(400);

    // --- assets ---
    if (const auto* arr = arrayAt(j, "assets")) {
        for (size_t i = 0; i < arr->size(); ++i) {
            const auto& a = (*arr)[i];
            if (!a.is_object()) { report.error(idx("assets", i), "Asset must be an object."); continue; }
            AssetSpec as;
            as.id   = str(a, "id");
            as.kind = str(a, "kind");
            if (auto it = a.find("spec"); it != a.end() && it->is_object()) as.spec = *it;
            out.assets.push_back(std::move(as));
        }
    }

    // --- nodes ---
    if (const auto* arr = arrayAt(j, "nodes")) {
        for (size_t i = 0; i < arr->size(); ++i) {
            const auto& n = (*arr)[i];
            if (!n.is_object()) { report.error(idx("nodes", i), "Node must be an object."); continue; }
            NodeSpec ns;
            ns.id   = str(n, "id");
            ns.type = str(n, "type");
            const std::string scopeStr = str(n, "scope", "voice");
            if (!parseScope(scopeStr, ns.scope)) {
                ns.scope = Scope::Voice;
                report.warn(idx("nodes", i) + ".scope",
                            "Unknown scope '" + scopeStr + "', assumed 'voice'.");
            }
            if (auto it = n.find("settings"); it != n.end() && it->is_object()) ns.settings = *it;
            out.nodes.push_back(std::move(ns));
        }
    } else {
        report.error("nodes", "Missing required 'nodes' array.");
    }

    // --- audio connections ---
    if (const auto* arr = arrayAt(j, "audio")) {
        for (size_t i = 0; i < arr->size(); ++i) {
            const auto& c = (*arr)[i];
            if (!c.is_object()) { report.error(idx("audio", i), "Connection must be an object."); continue; }
            AudioConn conn;
            conn.from = stripPort(str(c, "from"));
            conn.to   = stripPort(str(c, "to"));
            conn.gain = clampT(flt(c, "gain", 1.0f), -4.0f, 4.0f);
            out.audio.push_back(std::move(conn));
        }
    }

    // --- modulation routes ---
    if (const auto* arr = arrayAt(j, "mod")) {
        for (size_t i = 0; i < arr->size(); ++i) {
            const auto& r = (*arr)[i];
            if (!r.is_object()) { report.error(idx("mod", i), "Mod route must be an object."); continue; }
            ModRoute route;
            route.source  = stripPort(str(r, "source"));
            route.depth   = clampT(flt(r, "depth", 0.0f), -4.0f, 4.0f);
            route.curve   = taper(r, "curve", Taper::Linear);
            route.bipolar = boolean(r, "bipolar", false);

            // The target may arrive as "node.param" or as {"node":..,"param":..}.
            bool gotTarget = false;
            if (auto it = r.find("target"); it != r.end()) {
                if (it->is_string()) {
                    gotTarget = splitTarget(it->get<std::string>(), route.targetNode, route.targetParam);
                } else if (it->is_object()) {
                    route.targetNode  = str(*it, "node");
                    route.targetParam = str(*it, "param");
                    gotTarget = !route.targetNode.empty() && !route.targetParam.empty();
                }
            }
            if (!gotTarget) {
                route.targetNode  = str(r, "target_node");
                route.targetParam = str(r, "target_param");
                gotTarget = !route.targetNode.empty() && !route.targetParam.empty();
            }
            if (!gotTarget) {
                report.error(idx("mod", i) + ".target",
                             "Modulation target must be \"nodeId.paramId\".");
                continue;
            }
            out.mod.push_back(std::move(route));
        }
    }

    // --- exposed parameters ---
    if (const auto* arr = arrayAt(j, "params")) {
        for (size_t i = 0; i < arr->size(); ++i) {
            const auto& p = (*arr)[i];
            if (!p.is_object()) { report.error(idx("params", i), "Param must be an object."); continue; }
            ParamSpec ps;
            ps.id      = str(p, "id");
            ps.label   = str(p, "label", ps.id);
            ps.unit    = str(p, "unit");
            ps.min     = flt(p, "min", 0.0f);
            ps.max     = flt(p, "max", 1.0f);
            ps.def     = flt(p, "default", flt(p, "def", ps.min));
            ps.taper   = taper(p, "taper", Taper::Linear);
            ps.control = str(p, "control", "knob");
            ps.group   = str(p, "group", "General");
            ps.panel   = str(p, "panel", "");

            if (const auto* binds = arrayAt(p, "bind")) {
                for (const auto& b : *binds) {
                    ParamBind pb;
                    if (b.is_string()) {
                        if (!splitTarget(b.get<std::string>(), pb.node, pb.param)) continue;
                    } else if (b.is_object()) {
                        pb.node   = str(b, "node");
                        pb.param  = str(b, "param");
                        pb.scale  = clampT(flt(b, "scale", 1.0f), -8.0f, 8.0f);
                        pb.offset = clampT(flt(b, "offset", 0.0f), -4.0f, 4.0f);
                    } else {
                        continue;
                    }
                    if (!pb.node.empty() && !pb.param.empty()) ps.bind.push_back(std::move(pb));
                }
            }
            out.params.push_back(std::move(ps));
        }
    }

    // --- switches (settings exposed as UI controls) ---
    if (const auto* arr = arrayAt(j, "switches")) {
        for (size_t i = 0; i < arr->size(); ++i) {
            const auto& s = (*arr)[i];
            if (!s.is_object()) { report.error(idx("switches", i), "Switch must be an object."); continue; }
            SwitchSpec sw;
            sw.id      = str(s, "id");
            sw.label   = str(s, "label", sw.id);
            sw.node    = str(s, "node");
            sw.setting = str(s, "setting");
            sw.group   = str(s, "group", "General");
            sw.panel   = str(s, "panel", "");
            if (!sw.node.empty() && !sw.setting.empty()) out.switches.push_back(std::move(sw));
        }
    }

    // --- macros ---
    if (const auto* arr = arrayAt(j, "macros")) {
        for (size_t i = 0; i < arr->size(); ++i) {
            const auto& m = (*arr)[i];
            if (!m.is_object()) { report.error(idx("macros", i), "Macro must be an object."); continue; }
            MacroSpec ms;
            ms.id    = str(m, "id");
            ms.label = str(m, "label", ms.id);
            ms.def   = clamp01(flt(m, "default", 0.0f));
            if (const auto* routes = arrayAt(m, "routes")) {
                for (const auto& r : *routes) {
                    if (!r.is_object()) continue;
                    MacroRoute mr;
                    mr.param = str(r, "param");
                    mr.depth = clampT(flt(r, "depth", 0.0f), -4.0f, 4.0f);
                    mr.curve = taper(r, "curve", Taper::Linear);
                    if (!mr.param.empty()) ms.routes.push_back(std::move(mr));
                }
            }
            out.macros.push_back(std::move(ms));
        }
    }

    // --- ui ---
    if (auto it = j.find("ui"); it != j.end() && it->is_object()) {
        const auto& u = *it;
        if (auto th = u.find("theme"); th != u.end() && th->is_object()) {
            out.ui.accent = str(*th, "accent", out.ui.accent);
            out.ui.mood   = str(*th, "mood", out.ui.mood);
        }
        out.ui.accent = str(u, "accent", out.ui.accent);
        if (const auto* sections = arrayAt(u, "sections")) {
            for (const auto& s : *sections) {
                if (!s.is_object()) continue;
                UiSection sec;
                sec.title = str(s, "title", "Controls");
                if (const auto* ps = arrayAt(s, "params"))
                    for (const auto& v : *ps)
                        if (v.is_string()) sec.params.push_back(v.get<std::string>());
                out.ui.sections.push_back(std::move(sec));
            }
        }
        if (auto pa = u.find("panel_accent"); pa != u.end() && pa->is_object())
            for (auto it2 = pa->begin(); it2 != pa->end(); ++it2)
                if (it2.value().is_string())
                    out.ui.panelAccent[it2.key()] = it2.value().get<std::string>();
        out.ui.style = str(u, "style", out.ui.style);
        if (const auto* feat = arrayAt(u, "featured"))
            for (const auto& v : *feat)
                if (v.is_string()) out.ui.featuredPanels.push_back(v.get<std::string>());
        if (const auto* mr = arrayAt(u, "macro_row"))
            for (const auto& v : *mr)
                if (v.is_string()) out.ui.macroRow.push_back(v.get<std::string>());
    }

    return !report.hasErrors();
}

bool parse(const std::string& text, Instrument& out, IrReport& report) {
    nlohmann::json j;
    if (!parseJsonSafely(text, j, report)) return false;
    return fromJson(j, out, report);
}

// --- Serialisation ---------------------------------------------------------

nlohmann::json toJson(const Instrument& inst) {
    using nlohmann::json;
    json j;
    j["ir_version"]  = inst.irVersion;
    j["name"]        = inst.name;
    j["description"] = inst.description;
    if (!inst.meta.empty()) j["meta"] = inst.meta;
    j["voicing"]   = inst.voicing;
    j["glide_ms"]  = inst.glideMs;
    j["polyphony"] = inst.polyphony;
    j["seed"]      = inst.seed;

    json assets = json::array();
    for (const auto& a : inst.assets)
        assets.push_back({{"id", a.id}, {"kind", a.kind}, {"spec", a.spec}});
    if (!assets.empty()) j["assets"] = assets;

    json nodes = json::array();
    for (const auto& n : inst.nodes) {
        json nj = {{"id", n.id}, {"type", n.type}, {"scope", toString(n.scope)}};
        if (!n.settings.empty()) nj["settings"] = n.settings;
        nodes.push_back(std::move(nj));
    }
    j["nodes"] = nodes;

    json audio = json::array();
    for (const auto& c : inst.audio)
        audio.push_back({{"from", c.from}, {"to", c.to}, {"gain", c.gain}});
    j["audio"] = audio;

    json mod = json::array();
    for (const auto& r : inst.mod) {
        json rj = {{"source", r.source},
                   {"target", r.targetNode + "." + r.targetParam},
                   {"depth", r.depth},
                   {"curve", toString(r.curve)}};
        if (r.bipolar) rj["bipolar"] = true;
        mod.push_back(std::move(rj));
    }
    j["mod"] = mod;

    json params = json::array();
    for (const auto& p : inst.params) {
        json binds = json::array();
        for (const auto& b : p.bind) {
            json bj = {{"node", b.node}, {"param", b.param}};
            if (b.scale  != 1.0f) bj["scale"]  = b.scale;
            if (b.offset != 0.0f) bj["offset"] = b.offset;
            binds.push_back(std::move(bj));
        }
        params.push_back({{"id", p.id}, {"label", p.label}, {"unit", p.unit},
                          {"min", p.min}, {"max", p.max}, {"default", p.def},
                          {"taper", toString(p.taper)}, {"control", p.control},
                          {"group", p.group}, {"panel", p.panel}, {"bind", binds}});
    }
    j["params"] = params;

    json switches = json::array();
    for (const auto& s : inst.switches)
        switches.push_back({{"id", s.id}, {"label", s.label}, {"node", s.node},
                            {"setting", s.setting}, {"group", s.group}, {"panel", s.panel}});
    if (!switches.empty()) j["switches"] = switches;

    json macros = json::array();
    for (const auto& m : inst.macros) {
        json routes = json::array();
        for (const auto& r : m.routes)
            routes.push_back({{"param", r.param}, {"depth", r.depth}, {"curve", toString(r.curve)}});
        macros.push_back({{"id", m.id}, {"label", m.label}, {"default", m.def}, {"routes", routes}});
    }
    if (!macros.empty()) j["macros"] = macros;

    json sections = json::array();
    for (const auto& s : inst.ui.sections)
        sections.push_back({{"title", s.title}, {"params", s.params}});
    j["ui"] = {{"theme", {{"accent", inst.ui.accent}, {"mood", inst.ui.mood}}},
               {"sections", sections},
               {"panel_accent", inst.ui.panelAccent},
               {"featured", inst.ui.featuredPanels},
               {"style", inst.ui.style},
               {"macro_row", inst.ui.macroRow}};

    return j;
}

} // namespace forge::ir
