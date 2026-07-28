// ---------------------------------------------------------------------------
// Verifying that explicit requests were actually honoured.
//
// The brief that motivated this asked for four named macros, glide, and three
// specific mod-wheel mappings. It got none of them, and a description claiming
// all of them. These cases are that brief, taken apart.
// ---------------------------------------------------------------------------
#include "TestUtil.h"

#include "core/arch/Architecture.h"
#include "core/ir/IrRepair.h"
#include "core/llm/Compliance.h"

#include <nlohmann/json.hpp>

using namespace forge;
using json = nlohmann::json;

namespace {

const char* kBassBrief =
    "Create a dark, aggressive bass synth with a deep sub layer, a gritty distorted "
    "midrange, and a sharp metallic attack. Add glide for overlapping notes and map "
    "the modulation wheel to increase distortion, filter brightness, and movement. "
    "Include four macro controls: Weight, Bite, Motion, and Space. "
    "Keep the low end mono, avoid excessive reverb.";

ir::Instrument built(const json& patch) {
    auto inst = arch::buildFullArchitecture();
    ir::IrReport r;
    arch::applyPatch(inst, patch, r);
    ir::repair(inst, r);
    return inst;
}

bool unmet(const llm::ComplianceReport& c, const std::string& id) {
    for (const auto& r : c.requirements)
        if (r.id == id) return !r.met;
    return false;   // not checked at all counts as "not flagged"
}

bool checked(const llm::ComplianceReport& c, const std::string& id) {
    for (const auto& r : c.requirements) if (r.id == id) return true;
    return false;
}

} // namespace

TEST_CASE(macro_names_are_read_out_of_the_brief) {
    const auto names = llm::requestedMacroNames(kBassBrief);
    CHECK(names.size() == size_t(4));
    CHECK(names[0] == "Weight");
    CHECK(names[1] == "Bite");
    CHECK(names[2] == "Motion");
    CHECK(names[3] == "Space");
}

TEST_CASE(macro_names_survive_other_phrasings) {
    for (const char* p : {"give me four macros: Punch, Air, Drift and Glue",
                          "macro controls called Punch, Air, Drift, Glue",
                          // A real em dash, which is three UTF-8 bytes. It used
                          // to be compared against a char and could never
                          // match, so anyone who typed a proper dash got no
                          // macros and no explanation.
                          "four macros \xE2\x80\x94 Punch, Air, Drift and Glue",
                          "four macros \xE2\x80\x93 Punch, Air, Drift and Glue",
                          "add macros named Punch, Air, Drift and Glue please"}) {
        const auto names = llm::requestedMacroNames(p);
        CHECK(names.size() == size_t(4));
        CHECK(names[0] == "Punch");
        CHECK(names[3] == "Glue");
    }
}

TEST_CASE(macro_extraction_does_not_invent_requests) {
    // Would rather miss a request than fabricate one.
    for (const char* p : {"a warm analog bass", "map the mod wheel to a macro",
                          "something with macro control over the filter",
                          "gritty psych-rock guitar lead"}) {
        CHECK(llm::requestedMacroNames(p).empty());
    }
}

TEST_CASE(the_original_failure_is_now_detected) {
    // Exactly what shipped: a sensible-sounding patch that quietly ignores the
    // performance half of the brief.
    const auto inst = built(json::parse(R"({
        "name":"Iron Pulse",
        "description":"A mono sub bass with glide and mod-wheel control over everything.",
        "values":{"osc_sub_level":0.8,"f1_cutoff":700,"dr_drive":0.6,"rv_mix":0.06}
    })"));

    const auto c = llm::check(kBassBrief, inst);
    CHECK(!c.allMet());
    CHECK(unmet(c, "macros.named"));
    CHECK(unmet(c, "macros.count"));
    CHECK(unmet(c, "voicing.glide"));
    CHECK(unmet(c, "mod.wheel.drive"));
    CHECK(unmet(c, "mod.wheel.bright"));
    // Note: the stock matrix already wires the wheel to vibrato, so "the wheel
    // does something" and "the wheel adds movement" pass even here. Both are
    // honest - the wheel really does move the sound. The two that fail are the
    // two the brief named and the patch never wired.

    // The description claiming all of it is precisely why a checker is needed.
    const auto feedback = c.toModelFeedback();
    CHECK(!feedback.empty());
    CHECK(feedback.find("Weight") != std::string::npos);
    CHECK(feedback.find("glide_ms") != std::string::npos);
    CHECK(feedback.find("m_wheel") != std::string::npos);
}

TEST_CASE(a_patch_that_actually_complies_passes) {
    const auto inst = built(json::parse(R"({
        "name":"Iron Pulse",
        "voicing":"mono", "polyphony":1, "glide_ms":60,
        "values":{"osc_sub_level":0.8,"f1_cutoff":700,"dr_drive":0.6,"rv_mix":0.05,
                  "wd_width":0.30,"v_width":0.35},
        "macros":[
          {"label":"Weight","default":0.5,"routes":[{"param":"osc_sub_level","depth":0.6},
                                                    {"param":"f1_cutoff","depth":-0.2}]},
          {"label":"Bite","default":0.3,"routes":[{"param":"dr_drive","depth":0.7},
                                                  {"param":"f1_reso","depth":0.3}]},
          {"label":"Motion","default":0.2,"routes":[{"param":"lfo_1_rate","depth":0.5},
                                                    {"param":"f1_cutoff","depth":0.2}]},
          {"label":"Space","default":0.2,"routes":[{"param":"rv_mix","depth":0.5},
                                                   {"param":"dl_mix","depth":0.4}]}],
        "mod":[{"source":"m_wheel","target":"shp_drive","depth":0.6},
               {"source":"m_wheel","target":"filt_1.cutoff","depth":0.5},
               {"source":"m_wheel","target":"amt_vib.b","depth":0.5},
               {"source":"m_vel","target":"filt_1.cutoff","depth":0.5},
               {"source":"env_1","target":"amp.gain","depth":1.0}]
    })"));

    const auto c = llm::check(kBassBrief, inst);
    CHECK(c.allMet());
    CHECK(c.toModelFeedback().empty());
    CHECK(c.toUserSummary().find("satisfied") != std::string::npos);
}

TEST_CASE(macros_are_actually_built_from_the_patch) {
    const auto inst = built(json::parse(R"({"macros":[
        {"label":"Weight","routes":[{"param":"osc_sub_level","depth":0.6}]},
        {"label":"Bite","routes":[{"param":"dr_drive","depth":0.7}]}]})"));

    CHECK(inst.macros.size() == size_t(2));
    CHECK(inst.macros[0].label == "Weight");
    CHECK(inst.macros[1].label == "Bite");
    CHECK(inst.ui.macroRow.size() == size_t(2));
}

TEST_CASE(macros_wired_to_nothing_are_discarded_not_shown) {
    // A knob that moves nothing is worse than a missing knob: it lies.
    const auto inst = built(json::parse(R"({"macros":[
        {"label":"Real","routes":[{"param":"f1_cutoff","depth":0.5}]},
        {"label":"Empty","routes":[]},
        {"label":"Bogus","routes":[{"param":"not_a_real_control","depth":0.9}]},
        {"label":"Zero","routes":[{"param":"f1_reso","depth":0.0}]}]})"));

    CHECK(inst.macros.size() == size_t(1));
    CHECK(inst.macros[0].label == "Real");
}

TEST_CASE(macro_input_is_bounded) {
    json macros = json::array();
    for (int i = 0; i < 60; ++i)
        macros.push_back({{"label", std::string(200, 'x')},
                          {"routes", json::array({{{"param", "f1_cutoff"}, {"depth", 99.0}}})}});
    const auto inst = built(json{{"macros", macros}});

    CHECK(inst.macros.size() <= size_t(kMaxMacros));
    for (const auto& m : inst.macros) {
        CHECK(m.label.size() <= size_t(14));
        for (const auto& r : m.routes) CHECK(std::abs(r.depth) <= 1.0f);
    }
}

TEST_CASE(nothing_is_checked_when_nothing_specific_was_asked) {
    const auto inst = built(json::object());
    const auto c = llm::check("something dark and heavy", inst);
    CHECK(c.requirements.empty());
    CHECK(c.allMet());
    CHECK(c.toUserSummary().empty());
}

TEST_CASE(each_requirement_only_fires_when_its_words_appear) {
    const auto inst = built(json::object());

    CHECK(!checked(llm::check("a warm pad", inst), "voicing.glide"));
    CHECK(checked(llm::check("a synth bass with glide", inst), "voicing.glide"));

    CHECK(!checked(llm::check("a warm pad", inst), "mod.wheel"));
    CHECK(checked(llm::check("map the mod wheel to the filter", inst), "mod.wheel"));

    CHECK(!checked(llm::check("a warm pad", inst), "fx.dryness"));
    CHECK(checked(llm::check("a pad, avoid excessive reverb", inst), "fx.dryness"));
}

TEST_CASE(the_summary_names_what_is_missing) {
    const auto inst = built(json::object());
    const auto c = llm::check(kBassBrief, inst);
    const auto summary = c.toUserSummary();
    CHECK(summary.find("missing") != std::string::npos);
    CHECK(!c.allMet());
}

TEST_CASE(mod_targets_accept_control_ids_as_well_as_node_params) {
    // Two namespaces named the same things: the model is shown control ids
    // (`f1_cutoff`) but modulation addresses node.param (`filt_1.cutoff`).
    // Writing the id it was shown used to drop the route silently, which is
    // how a mod wheel ends up doing nothing while the description says it does.
    const auto viaControl = built(json::parse(R"({"mod":[
        {"source":"m_wheel","target":"f1_cutoff","depth":0.5}]})"));
    const auto viaNode = built(json::parse(R"({"mod":[
        {"source":"m_wheel","target":"filt_1.cutoff","depth":0.5}]})"));

    auto routed = [](const ir::Instrument& i) {
        for (const auto& r : i.mod)
            if (r.source == "m_wheel" && r.targetNode == "filt_1" && r.targetParam == "cutoff")
                return true;
        return false;
    };
    CHECK(routed(viaControl));
    CHECK(routed(viaNode));
}

TEST_CASE(a_nonexistent_mod_target_is_still_rejected) {
    // Being generous about spelling must not mean accepting anything.
    const auto inst = built(json::parse(R"({"mod":[
        {"source":"m_wheel","target":"not_a_control","depth":0.5},
        {"source":"m_wheel","target":"filt_1.no_such_param","depth":0.5},
        {"source":"m_wheel","target":"f1_cutoff","depth":0.4}]})"));

    int wheelRoutes = 0;
    for (const auto& r : inst.mod) if (r.source == "m_wheel") ++wheelRoutes;
    CHECK(wheelRoutes == 1);
}
