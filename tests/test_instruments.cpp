// ---------------------------------------------------------------------------
// Real instruments, not just synthesisers.
//
// Someone asked for "a bass like Steve Lacy or Thundercat" and got a synth,
// twice. Both are bass GUITAR players, so the answer was the wrong instrument
// however good it sounded. The cause was the same shape as the macros bug: the
// Karplus-Strong string was in the module library, tested and safe, but not in
// the rack - so no prompt could reach it.
// ---------------------------------------------------------------------------
#include "TestUtil.h"

#include "core/arch/Architecture.h"
#include "core/dsp/GraphBuilder.h"
#include "core/dsp/Registry.h"
#include "core/ir/IrRepair.h"
#include "core/ir/IrSafety.h"
#include "core/ir/IrValidator.h"
#include "core/llm/Compliance.h"
#include "core/llm/PromptBuilder.h"

#include <nlohmann/json.hpp>

#include <cmath>

using namespace forge;
using json = nlohmann::json;

namespace {

ir::Instrument make(const json& patch) {
    auto inst = arch::buildFullArchitecture();
    ir::IrReport r;
    arch::applyPatch(inst, patch, r);
    ir::repair(inst, r);
    return inst;
}

bool hasControl(const ir::Instrument& inst, const char* id) {
    for (const auto& p : inst.params) if (p.id == id) return true;
    return false;
}

bool unmet(const llm::ComplianceReport& c, const char* id) {
    for (const auto& r : c.requirements) if (r.id == id) return !r.met;
    return false;
}

bool checked(const llm::ComplianceReport& c, const char* id) {
    for (const auto& r : c.requirements) if (r.id == id) return true;
    return false;
}

} // namespace

TEST_CASE(the_rack_can_actually_reach_a_physical_model) {
    const auto rack = arch::buildFullArchitecture();

    // The string and the resonant body are the two things that separate an
    // instrument from a tone generator, and both must be addressable.
    CHECK(hasControl(rack, "str_level"));
    CHECK(hasControl(rack, "str_damp"));
    CHECK(hasControl(rack, "str_bright"));
    CHECK(hasControl(rack, "str_decay"));
    CHECK(hasControl(rack, "body_mix"));
    CHECK(hasControl(rack, "vow_mix"));

    bool karplus = false, comb = false, formant = false;
    for (const auto& n : rack.nodes) {
        if (n.type == "osc.karplus")    karplus = true;
        if (n.type == "filter.comb")    comb    = true;
        if (n.type == "filter.formant") formant = true;
    }
    CHECK(karplus);
    CHECK(comb);
    CHECK(formant);
}

TEST_CASE(the_new_modules_are_silent_until_asked_for) {
    // Adding them must not change any existing sound.
    const auto rack = arch::buildFullArchitecture();
    for (const char* id : {"str_level", "body_mix", "vow_mix"})
        for (const auto& p : rack.params)
            if (p.id == id) CHECK(p.def == 0.0f);
}

TEST_CASE(a_physically_modelled_instrument_builds_and_is_safe) {
    auto inst = make(json::parse(R"({
        "name":"Finger Bass",
        "values":{"str_level":0.85,"str_damp":0.40,"str_bright":2500,"str_decay":0.985,
                  "osc_a_level":0.0,"osc_b_level":0.0,"osc_sub_level":0.25,
                  "body_mix":0.30,"body_freq":90,"body_res":0.55,
                  "f1_cutoff":2200,"dr_drive":1.8,
                  "env_1_a":1,"env_1_d":600,"env_1_s":0.15,"env_1_r":400}
    })"));

    ir::IrReport safety;
    CHECK(ir::applySafety(inst, safety, 0.35f));
    ir::IrReport check;
    CHECK(ir::validate(inst, check));
    CHECK(!check.hasErrors());

    // And it really renders - a comb with feedback in the voice chain is
    // exactly the sort of thing that blows up if wired wrong.
    ir::IrReport buildReport;
    auto graph = GraphBuilder::build(inst, 48000.0, buildReport);
    CHECK(graph != nullptr);
    if (graph == nullptr) return;

    float left[512] = {}, right[512] = {};
    float* out[2] = {left, right};
    graph->noteOn(40, 0.8f);
    for (int block = 0; block < 24; ++block) graph->process(out, 512);

    float peak = 0.0f;
    bool  finite = true;
    for (int c = 0; c < 2; ++c)
        for (int i = 0; i < 512; ++i) {
            const float v = out[c][i];
            if (!std::isfinite(v)) finite = false;
            peak = std::max(peak, std::abs(v));
        }
    CHECK(finite);
    CHECK(peak > 0.0001f);     // it makes a sound
    CHECK(peak <= 1.0f);       // and does not escape the limiter
}

TEST_CASE(a_named_acoustic_instrument_answered_with_a_synth_is_caught) {
    // Exactly what shipped: saws, a lowpass, no physical model anywhere.
    const auto synth = make(json::parse(R"({
        "values":{"osc_a_level":0.8,"osc_b_level":0.6,"f1_cutoff":1200,"osc_sub_level":0.4}})"));

    for (const char* prompt : {"a bass like Steve Lacy or Thundercat, lofi",
                               "warm nylon guitar", "a cello line", "bright glockenspiel"}) {
        const auto c = llm::check(prompt, synth);
        CHECK(checked(c, "instrument.physical"));
        CHECK(unmet(c, "instrument.physical"));
        CHECK(c.toModelFeedback().find("not a synth") != std::string::npos);
    }
}

TEST_CASE(a_properly_modelled_instrument_passes_the_check) {
    const auto plucked = make(json::parse(R"({
        "values":{"str_level":0.85,"str_damp":0.4,"body_mix":0.3,"osc_a_level":0.0}})"));
    CHECK(!unmet(llm::check("a bass like Thundercat", plucked), "instrument.physical"));
    CHECK(!unmet(llm::check("warm nylon guitar", plucked), "instrument.physical"));

    const auto struck = make(json::parse(R"({
        "values":{"osc_fm_level":0.8,"osc_a_level":0.0}})"));
    CHECK(!unmet(llm::check("a bright glockenspiel", struck), "instrument.physical"));
    CHECK(!unmet(llm::check("rhodes piano", struck), "instrument.physical"));
}

TEST_CASE(asking_for_a_synth_is_still_allowed) {
    // "synth bass" is a request for a synth. Do not force a physical model on
    // someone who asked for the opposite.
    const auto synth = make(json::parse(R"({"values":{"osc_a_level":0.8}})"));
    for (const char* prompt : {"a synth bass", "moog bass guitar tone but synthetic",
                               "303 acid bass", "808 sub"}) {
        CHECK(!checked(llm::check(prompt, synth), "instrument.physical"));
    }
}

TEST_CASE(the_check_stays_quiet_when_no_instrument_is_named) {
    const auto synth = make(json::parse(R"({"values":{"osc_a_level":0.8}})"));
    for (const char* prompt : {"something dark and heavy", "a wide evolving pad",
                               "gritty and futuristic"}) {
        CHECK(!checked(llm::check(prompt, synth), "instrument.physical"));
    }
}

TEST_CASE(the_prompt_teaches_real_instrument_construction) {
    const auto system = llm::buildPatchSystemPrompt();

    // The modules must be listed, or none of the advice is actionable.
    CHECK(system.find("str_level") != std::string::npos);
    CHECK(system.find("body_mix") != std::string::npos);
    CHECK(system.find("[STRING]") != std::string::npos);

    // And the guidance itself must be there.
    CHECK(system.find("not a synth-only instrument") != std::string::npos);
    CHECK(system.find("Karplus") != std::string::npos);
    CHECK(system.find("Electric bass") != std::string::npos);
    CHECK(system.find("Bowed strings") != std::string::npos);
    CHECK(system.find("Mallets") != std::string::npos);
}

TEST_CASE(even_the_offline_floor_builds_a_real_instrument) {
    // With no model at all, a named guitar must still not come back a synth.
    for (const char* prompt : {"nylon guitar", "a bass like Thundercat",
                               "acoustic guitar", "cello"}) {
        auto inst = arch::buildFullArchitecture();
        ir::IrReport r;
        arch::applyPatch(inst, arch::heuristicPatch(prompt), r);
        ir::repair(inst, r);

        CHECK(!unmet(llm::check(prompt, inst), "instrument.physical"));

        ir::IrReport check;
        CHECK(ir::validate(inst, check));
    }

    // And a synth request still gets a synth offline.
    auto synth = arch::buildFullArchitecture();
    ir::IrReport r;
    arch::applyPatch(synth, arch::heuristicPatch("warm analog synth bass"), r);
    ir::repair(synth, r);
    for (const auto& p : synth.params)
        if (p.id == "str_level") CHECK(p.def == 0.0f);
}

// --- the guard that should have existed from the start ----------------------

TEST_CASE(the_bare_architecture_validates) {
    // This is the check whose absence hid a real bug for weeks: `gt_hld` had a
    // logarithmic taper with a zero minimum, so the full rack did not validate.
    // Every generation that kept the GATE panel failed and fell back silently,
    // and it went unnoticed because the layout pass usually pruned that panel.
    auto rack = arch::buildFullArchitecture();
    ir::IrReport repair;
    ir::repair(rack, repair);

    ir::IrReport v;
    CHECK(ir::validate(rack, v));
    CHECK(!v.hasErrors());

    ir::IrReport build;
    CHECK(GraphBuilder::build(rack, 48000.0, build) != nullptr);
}

TEST_CASE(every_control_in_the_rack_resolves_to_something_real) {
    // A knob bound to a parameter no module has is a knob that does nothing.
    const auto rack = arch::buildFullArchitecture();
    const auto& registry = Registry::instance();

    for (const auto& p : rack.params) {
        CHECK(!p.bind.empty());
        for (const auto& b : p.bind) {
            const auto* node = rack.findNode(b.node);
            CHECK(node != nullptr);
            if (node == nullptr) continue;
            const auto* man = registry.find(node->type);
            CHECK(man != nullptr);
            if (man == nullptr) continue;
            bool found = false;
            for (const auto& pd : man->params) if (pd.id == b.param) found = true;
            CHECK(found);
        }
    }

    for (const auto& sw : rack.switches) {
        const auto* node = rack.findNode(sw.node);
        CHECK(node != nullptr);
        if (node == nullptr) continue;
        const auto* man = registry.find(node->type);
        CHECK(man != nullptr);
        if (man == nullptr) continue;
        bool found = false;
        for (const auto& sd : man->settings) if (sd.id == sw.setting) found = true;
        CHECK(found);
    }
}

TEST_CASE(the_controls_an_instrument_needs_are_all_reachable) {
    // Every one of these was, at some point, described in the prompt as the way
    // to build something - while being unreachable from the rack. That is the
    // bug shape this project keeps producing, so it gets a list.
    const auto rack = arch::buildFullArchitecture();
    for (const char* id : {
            "str_level", "str_damp", "str_bright", "str_decay",   // plucked strings
            "body_mix", "body_freq", "body_res",                  // resonant body
            "vow_mix", "vow_morph",                               // cavity/vowel
            "osc_fm_level", "osc_fm_index", "osc_fm_fine",        // struck and metallic
            "osc_a_pmod", "osc_b_pmod", "osc_fm_pmod",            // pitch envelopes
            "ch_mix", "ch_depth",                                 // ensemble/chorus
        }) {
        bool found = false;
        for (const auto& p : rack.params) if (p.id == id) found = true;
        CHECK(found);
    }
    // FM ratio is a selector, not a knob - and non-integer values must exist.
    bool ratio = false;
    for (const auto& sw : rack.switches) if (sw.id == "osc_fm_ratio") ratio = true;
    CHECK(ratio);
}
