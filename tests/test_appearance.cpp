// ---------------------------------------------------------------------------
// Every instrument should look like its own product.
//
// "I want it to be VERY VERY abstract; if I want a dark, cooler VST use colours
// like teal and knob designs that are very sharp and utilitarian; while if I
// want a jamaica warm groove, use warmer colours like orange, green, and
// designs that are more abstract/large."
//
// Colour alone was never going to do it - two instruments with identical
// geometry in different palettes still read as the same product. So the model
// chooses the CONTROL SHAPES and the VISUAL LANGUAGE as well, and both have to
// survive the trip from the patch to the instrument.
// ---------------------------------------------------------------------------
#include "TestUtil.h"

#include "core/arch/Architecture.h"
#include "core/ir/Ir.h"
#include "core/ir/IrRepair.h"
#include "core/ir/IrValidator.h"
#include "core/llm/PromptBuilder.h"

#include <nlohmann/json.hpp>

#include <set>

using namespace forge;
using json = nlohmann::json;

namespace {

ir::Instrument built(const char* patch) {
    auto inst = arch::buildFullArchitecture();
    ir::IrReport r;
    arch::applyPatch(inst, json::parse(patch), r);
    ir::repair(inst, r);
    return inst;
}

std::string controlOf(const ir::Instrument& inst, const char* id) {
    for (const auto& p : inst.params) if (p.id == id) return p.control;
    return "?";
}

} // namespace

TEST_CASE(the_model_can_choose_a_control_shape) {
    const auto inst = built(R"({"controls":{
        "osc_a_pmod":"vslider","wd_width":"hslider","lfo_1_sync":"toggle",
        "f1_cutoff":"knob"}})");

    CHECK(controlOf(inst, "osc_a_pmod") == "vslider");
    CHECK(controlOf(inst, "wd_width")   == "hslider");
    CHECK(controlOf(inst, "f1_cutoff")  == "knob");
}

TEST_CASE(a_pitch_fader_is_reachable_at_all) {
    // The specific request: "a pitch modifier knob that scales vertically -
    // this is very important for plugins and should be in nearly every VST."
    // The control has to exist before it can be shaped.
    const auto rack = arch::buildFullArchitecture();
    std::set<std::string> ids;
    for (const auto& p : rack.params) ids.insert(p.id);

    CHECK(ids.count("osc_a_pmod") == 1);
    CHECK(ids.count("osc_b_pmod") == 1);
    CHECK(ids.count("osc_fm_pmod") == 1);
    CHECK(ids.count("osc_sub_pmod") == 1);
    CHECK(ids.count("osc_wt_pmod") == 1);

    // And it must be a real modulation target, not just a knob that sits there.
    const auto inst = built(R"({"mod":[
        {"source":"env_3","target":"osc_a_pmod","depth":0.4}]})");
    bool routed = false;
    for (const auto& r : inst.mod)
        if (r.targetNode == "osc_a" && r.targetParam == "pitch_mod_semis") routed = true;
    CHECK(routed);
}

TEST_CASE(nonsense_control_shapes_are_ignored) {
    // Untrusted input. An unknown shape must leave the control alone rather
    // than producing something the UI cannot render.
    const auto inst = built(R"({"controls":{
        "f1_cutoff":"hologram","f1_reso":"","f2_cutoff":"KNOB","rv_mix":42}})");

    for (const char* id : {"f1_cutoff", "f1_reso", "f2_cutoff", "rv_mix"})
        CHECK(controlOf(inst, id) == "knob");
}

TEST_CASE(the_model_chooses_the_visual_language) {
    for (auto pair : {std::make_pair(R"({"layout":{"style":"soft"}})",    "soft"),
                      std::make_pair(R"({"layout":{"style":"vintage"}})", "vintage"),
                      std::make_pair(R"({"layout":{"style":"minimal"}})", "minimal"),
                      std::make_pair(R"({"layout":{"style":"sharp"}})",   "sharp")}) {
        CHECK(built(pair.first).ui.style == pair.second);
    }
    // An unknown style falls back rather than reaching the UI as garbage.
    CHECK(built(R"({"layout":{"style":"brutalist"}})").ui.style == "sharp");
    CHECK(built(R"({"layout":{"style":123}})").ui.style == "sharp");
}

TEST_CASE(style_and_controls_survive_serialisation) {
    // They have to persist, or a saved instrument reopens looking generic.
    auto inst = built(R"({"controls":{"osc_a_pmod":"vslider"},
                          "layout":{"style":"vintage"}})");
    ir::Instrument round;
    ir::IrReport   r;
    CHECK(ir::fromJson(ir::toJson(inst), round, r));
    CHECK(round.ui.style == "vintage");
    CHECK(controlOf(round, "osc_a_pmod") == "vslider");
}

TEST_CASE(a_warm_request_and_a_cold_one_do_not_look_alike) {
    // The heart of it. Offline, with no model at all, these two must differ in
    // more than colour.
    auto build = [](const char* prompt) {
        auto inst = arch::buildFullArchitecture();
        ir::IrReport r;
        arch::applyPatch(inst, arch::heuristicPatch(prompt), r);
        return inst;
    };

    const auto warm = build("a warm dusty reggae groove bass");
    const auto cold = build("a bright clinical techno stab");

    CHECK(warm.ui.style != cold.ui.style);
    CHECK(warm.ui.accent != cold.ui.accent);
}

TEST_CASE(the_offline_floor_still_varies_its_controls) {
    auto inst = arch::buildFullArchitecture();
    ir::IrReport r;
    arch::applyPatch(inst, arch::heuristicPatch("a warm bass"), r);

    // Not everything is a knob, even with no model involved.
    int nonKnob = 0;
    for (const auto& p : inst.params) if (p.control != "knob") ++nonKnob;
    CHECK(nonKnob >= 2);
}

TEST_CASE(an_instrument_with_mixed_controls_still_validates) {
    auto inst = built(R"({"controls":{
        "osc_a_pmod":"vslider","osc_b_pmod":"vslider","wd_width":"hslider",
        "rv_mix":"hslider","lfo_1_sync":"toggle","f1_cutoff":"knob"},
        "layout":{"style":"soft"}})");
    ir::IrReport check;
    CHECK(ir::validate(inst, check));
    CHECK(!check.hasErrors());
}

TEST_CASE(the_prompt_teaches_control_shapes_and_style) {
    const auto system = llm::buildPatchSystemPrompt();

    CHECK(system.find("vslider") != std::string::npos);
    CHECK(system.find("hslider") != std::string::npos);
    CHECK(system.find("toggle")  != std::string::npos);
    CHECK(system.find("not everything is a knob") != std::string::npos);

    CHECK(system.find("style - the visual language") != std::string::npos);
    CHECK(system.find("vintage") != std::string::npos);
    CHECK(system.find("minimal") != std::string::npos);
    // And the specific contrast the request asked for.
    CHECK(system.find("Jamaican") != std::string::npos);
}
